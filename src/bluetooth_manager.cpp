/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/bluetooth_manager.hpp>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci_types.h>

#include <array>
#include <cstring>

namespace zest
{

BluetoothManager *BluetoothManager::instance_ = nullptr;

BluetoothManager::BluetoothManager() noexcept
{
	if (instance_ != nullptr) {
		return;
	}
	instance_ = this;

	callbacks_.connected = connected_callback;
	callbacks_.disconnected = disconnected_callback;
	(void)bt_conn_cb_register(&callbacks_);
}

BluetoothManager::~BluetoothManager() noexcept
{
	if (instance_ == this) {
		(void)bt_conn_cb_unregister(&callbacks_);
		instance_ = nullptr;
	}
}

bool BluetoothManager::owns_stack() const noexcept
{
	return instance_ == this;
}

void BluetoothManager::set_state(State state) noexcept
{
	state_.store(state);
	if (state_handler_) {
		state_handler_(state);
	}
	state_changed_.give();
}

Result<> BluetoothManager::enable(std::string_view device_name) noexcept
{
	if (!owns_stack()) {
		return fail(errors::busy);
	}

	ScopedLock lock{mutex_};
	if (!bt_is_ready()) {
		ZEST_TRY(check(bt_enable(nullptr)));
	}

	if (!device_name.empty()) {
#if defined(CONFIG_BT_DEVICE_NAME_DYNAMIC)
		std::array<char, CONFIG_BT_DEVICE_NAME_MAX + 1> name{};
		if (device_name.size() >= name.size()) {
			return fail(errors::name_too_long);
		}
		std::memcpy(name.data(), device_name.data(), device_name.size());
		ZEST_TRY(check(bt_set_name(name.data())));
#else
		/*
		 * Silently ignoring the name would leave the device advertising
		 * something else entirely, so say so instead.
		 */
		return fail(errors::not_supported);
#endif
	}

	set_state(State::enabled);
	return {};
}

Result<> BluetoothManager::disable() noexcept
{
	if (!owns_stack()) {
		return fail(errors::busy);
	}
	if (advertising_) {
		ZEST_TRY(stop_advertising());
	}
	if (connected()) {
		ZEST_TRY(disconnect());
	}

	ScopedLock lock{mutex_};
	if (bt_is_ready()) {
		ZEST_TRY(check(bt_disable()));
	}
	set_state(State::disabled);
	return {};
}

Result<> BluetoothManager::start_advertising(const AdvertisingOptions &options) noexcept
{
#if !defined(CONFIG_BT_PERIPHERAL)
	(void)options;
	return fail(errors::not_supported);
#else
	if (!owns_stack() || !bt_is_ready()) {
		return fail(errors::no_device);
	}
	if (options.interval_min <= std::chrono::milliseconds::zero() ||
	    options.interval_max < options.interval_min) {
		return fail(errors::invalid_argument);
	}

	ScopedLock lock{mutex_};
	if (advertising_) {
		return {};
	}

	/* Advertising intervals are in units of 0.625ms, so milliseconds scale by
	 * 1/0.625 — exactly 8/5, done in integers to keep the rounding visible. */
	bt_le_adv_param params{};
	params.id = BT_ID_DEFAULT;
	params.sid = 0U;
	params.interval_min = static_cast<std::uint32_t>(options.interval_min.count()) * 8U / 5U;
	params.interval_max = static_cast<std::uint32_t>(options.interval_max.count()) * 8U / 5U;
	params.options = BT_LE_ADV_OPT_NONE;
	if (options.connectable) {
		params.options |= BT_LE_ADV_OPT_CONN;
	}

	/* Zephyr removed BT_LE_ADV_OPT_USE_NAME, which used to append the device
	 * name for the caller; the name is now just another AD structure. It has
	 * to outlive bt_le_adv_start(), which retains the caller's buffer rather
	 * than copying it — bt_get_name() returns storage the stack owns, so the
	 * bt_data may point at it, but nothing on this stack frame could. */
	const char *name = bt_get_name();
	const std::size_t name_length = name != nullptr ? std::strlen(name) : 0U;
	const bt_data advertisement[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, name, static_cast<std::uint8_t>(name_length)),
	};
	const bool include_name = options.include_name && name_length > 0U;

	ZEST_TRY(
		check(bt_le_adv_start(&params, include_name ? advertisement : nullptr,
				      include_name ? ARRAY_SIZE(advertisement) : 0U, nullptr, 0U)));
	advertising_ = true;
	set_state(State::advertising);
	return {};
#endif
}

Result<> BluetoothManager::stop_advertising() noexcept
{
#if !defined(CONFIG_BT_PERIPHERAL)
	return fail(errors::not_supported);
#else
	if (!advertising_) {
		return {};
	}
	ScopedLock lock{mutex_};
	ZEST_TRY(check(bt_le_adv_stop()));
	advertising_ = false;
	if (!connected()) {
		set_state(State::enabled);
	}
	return {};
#endif
}

Result<> BluetoothManager::connect(const Peer &peer, std::chrono::milliseconds timeout) noexcept
{
#if !defined(CONFIG_BT_CENTRAL)
	(void)peer;
	(void)timeout;
	return fail(errors::not_supported);
#else
	if (!owns_stack()) {
		return fail(errors::busy);
	}
	/* Validate the address before touching the stack. */
	if (peer.address.size() != 17U) {
		return fail(errors::invalid_argument);
	}

	std::array<char, 18> address_text{};
	std::memcpy(address_text.data(), peer.address.data(), peer.address.size());
	bt_addr_le_t address{};
	const char *type = peer.type == AddressType::public_ ? "public" : "random";
	if (bt_addr_le_from_str(address_text.data(), type, &address) != 0) {
		return fail(errors::invalid_argument);
	}
	if (!bt_is_ready()) {
		return fail(errors::no_device);
	}

	ScopedLock lock{mutex_};
	if (connected()) {
		return {};
	}

	const struct bt_conn_le_create_param create_params = BT_CONN_LE_CREATE_PARAM_INIT(
		BT_CONN_LE_OPT_NONE, BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_INTERVAL);
	const struct bt_le_conn_param connection_params =
		BT_LE_CONN_PARAM_INIT(BT_GAP_INIT_CONN_INT_MIN, BT_GAP_INIT_CONN_INT_MAX, 0,
				      BT_GAP_MS_TO_CONN_TIMEOUT(4000));

	connection_error_ = 0;
	state_changed_.reset();
	set_state(State::connecting);

	if (const int rc =
		    bt_conn_le_create(&address, &create_params, &connection_params, &connection_);
	    rc != 0) {
		set_state(State::enabled);
		return fail(rc);
	}

	if (!state_changed_.take(timeout)) {
		(void)bt_conn_disconnect(connection_, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		/*
		 * Reset the state on the timeout path. Leaving it latched at
		 * `connecting` made state() lie until an asynchronous callback
		 * happened to fire.
		 */
		set_state(State::enabled);
		return fail(errors::timed_out);
	}
	if (!connected()) {
		const int error = connection_error_ != 0 ? connection_error_ : -ECONNREFUSED;
		return fail(error);
	}
	return {};
#endif
}

Result<> BluetoothManager::disconnect() noexcept
{
	if (!owns_stack()) {
		return fail(errors::busy);
	}

	/* Test the connection under the lock: the stack clears it from its own context. */
	ScopedLock lock{mutex_};
	if (connection_ == nullptr) {
		return fail(errors::not_connected);
	}

	state_changed_.reset();
	set_state(State::disconnecting);
	ZEST_TRY(check(bt_conn_disconnect(connection_, BT_HCI_ERR_REMOTE_USER_TERM_CONN)));

	if (!state_changed_.take(std::chrono::seconds{10})) {
		return fail(errors::timed_out);
	}
	return {};
}

BluetoothManager::State BluetoothManager::state() const noexcept
{
	return state_.load();
}

void BluetoothManager::connected_callback(struct bt_conn *connection, std::uint8_t error) noexcept
{
	if (instance_ == nullptr) {
		return;
	}
	/* A peripheral learns of its connection here, without having created it. */
	if (instance_->connection_ == nullptr && error == 0U) {
		instance_->connection_ = bt_conn_ref(connection);
		instance_->advertising_ = false;
		instance_->set_state(State::connected);
		return;
	}
	if (connection != instance_->connection_) {
		return;
	}
	if (error == 0U) {
		instance_->set_state(State::connected);
	} else {
		instance_->connection_error_ = -static_cast<int>(error);
		bt_conn_unref(instance_->connection_);
		instance_->connection_ = nullptr;
		instance_->set_state(State::enabled);
	}
}

void BluetoothManager::disconnected_callback(struct bt_conn *connection,
					     std::uint8_t reason) noexcept
{
	ARG_UNUSED(reason);
	if (instance_ == nullptr || connection != instance_->connection_) {
		return;
	}
	bt_conn_unref(instance_->connection_);
	instance_->connection_ = nullptr;
	instance_->set_state(State::enabled);
}

const char *to_string(BluetoothManager::State state) noexcept
{
	switch (state) {
	case BluetoothManager::State::disabled:
		return "disabled";
	case BluetoothManager::State::enabled:
		return "enabled";
	case BluetoothManager::State::advertising:
		return "advertising";
	case BluetoothManager::State::connecting:
		return "connecting";
	case BluetoothManager::State::connected:
		return "connected";
	case BluetoothManager::State::disconnecting:
		return "disconnecting";
	}
	return "unknown";
}

} /* namespace zest */
