/* SPDX-License-Identifier: Apache-2.0 */

#include <zest/bluetooth_manager.hpp>

#include <array>
#include <cerrno>
#include <cstring>

#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci_types.h>
#endif

namespace zest
{

BluetoothManager *BluetoothManager::instance_ = nullptr;

BluetoothManager::BluetoothManager()
{
	k_sem_init(&state_changed_, 0, 1);
	k_mutex_init(&mutex_);
	if (instance_ != nullptr) {
		return;
	}
	instance_ = this;

#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
	callbacks_.connected = connected_callback;
	callbacks_.disconnected = disconnected_callback;
	(void)bt_conn_cb_register(&callbacks_);
#endif
}

BluetoothManager::~BluetoothManager()
{
#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
	if (instance_ == this) {
		(void)bt_conn_cb_unregister(&callbacks_);
	}
#endif
	if (instance_ == this) {
		instance_ = nullptr;
	}
}

std::expected<void, int> BluetoothManager::enable(std::string_view device_name)
{
#if !defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
	ARG_UNUSED(device_name);
	return std::unexpected(-ENOTSUP);
#else
	if (instance_ != this) {
		return std::unexpected(-EBUSY);
	}
	k_mutex_lock(&mutex_, K_FOREVER);
	if (!bt_is_ready()) {
		const int result = bt_enable(nullptr);
		if (result != 0) {
			k_mutex_unlock(&mutex_);
			return std::unexpected(result);
		}
	}

	if (!device_name.empty()) {
		std::array<char, 32> name{};
		if (device_name.size() >= name.size()) {
			k_mutex_unlock(&mutex_);
			return std::unexpected(-ENAMETOOLONG);
		}
		std::memcpy(name.data(), device_name.data(), device_name.size());
		const int result = bt_set_name(name.data());
		if (result != 0) {
			k_mutex_unlock(&mutex_);
			return std::unexpected(result);
		}
	}

	atomic_set(&state_, static_cast<atomic_val_t>(State::enabled));
	k_mutex_unlock(&mutex_);
	return {};
#endif
}

std::expected<void, int> BluetoothManager::disable()
{
#if !defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
	return std::unexpected(-ENOTSUP);
#else
	if (instance_ != this) {
		return std::unexpected(-EBUSY);
	}
	if (connected()) {
		auto result = disconnect();
		if (!result) {
			return result;
		}
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	if (bt_is_ready()) {
		const int result = bt_disable();
		if (result != 0) {
			k_mutex_unlock(&mutex_);
			return std::unexpected(result);
		}
	}
	atomic_set(&state_, static_cast<atomic_val_t>(State::disabled));
	k_mutex_unlock(&mutex_);
	return {};
#endif
}

std::expected<void, int> BluetoothManager::connect(const Peer &peer,
						   std::chrono::milliseconds timeout)
{
#if !defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
	ARG_UNUSED(peer);
	ARG_UNUSED(timeout);
	return std::unexpected(-ENOTSUP);
#else
	if (instance_ != this || !bt_is_ready()) {
		return std::unexpected(-ENODEV);
	}
	if (peer.address.size() != 17U) {
		return std::unexpected(-EINVAL);
	}

	std::array<char, 18> address_text{};
	std::memcpy(address_text.data(), peer.address.data(), peer.address.size());
	bt_addr_le_t address{};
	const char *type = peer.type == AddressType::public_ ? "public" : "random";
	int result = bt_addr_le_from_str(address_text.data(), type, &address);
	if (result != 0) {
		return std::unexpected(result);
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	if (connected()) {
		k_mutex_unlock(&mutex_);
		return {};
	}

	const struct bt_conn_le_create_param create_params = BT_CONN_LE_CREATE_PARAM_INIT(
		BT_CONN_LE_OPT_NONE, BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_INTERVAL);
	const struct bt_le_conn_param connection_params =
		BT_LE_CONN_PARAM_INIT(BT_GAP_INIT_CONN_INT_MIN, BT_GAP_INIT_CONN_INT_MAX, 0,
				      BT_GAP_MS_TO_CONN_TIMEOUT(4000));

	connection_error_ = 0;
	k_sem_reset(&state_changed_);
	atomic_set(&state_, static_cast<atomic_val_t>(State::connecting));
	result = bt_conn_le_create(&address, &create_params, &connection_params, &connection_);
	if (result != 0) {
		atomic_set(&state_, static_cast<atomic_val_t>(State::enabled));
		k_mutex_unlock(&mutex_);
		return std::unexpected(result);
	}

	result = k_sem_take(&state_changed_, K_MSEC(timeout.count()));
	if (result != 0) {
		(void)bt_conn_disconnect(connection_, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		k_mutex_unlock(&mutex_);
		return std::unexpected(-ETIMEDOUT);
	}
	if (!connected()) {
		const int error = connection_error_ != 0 ? connection_error_ : -ECONNREFUSED;
		k_mutex_unlock(&mutex_);
		return std::unexpected(error);
	}
	k_mutex_unlock(&mutex_);
	return {};
#endif
}

std::expected<void, int> BluetoothManager::disconnect()
{
#if !defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
	return std::unexpected(-ENOTSUP);
#else
	if (instance_ != this || connection_ == nullptr) {
		return std::unexpected(-ENOTCONN);
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	k_sem_reset(&state_changed_);
	atomic_set(&state_, static_cast<atomic_val_t>(State::disconnecting));
	const int result = bt_conn_disconnect(connection_, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (result != 0) {
		k_mutex_unlock(&mutex_);
		return std::unexpected(result);
	}
	const int wait_result = k_sem_take(&state_changed_, K_SECONDS(10));
	k_mutex_unlock(&mutex_);
	if (wait_result != 0) {
		return std::unexpected(-ETIMEDOUT);
	}
	return {};
#endif
}

BluetoothManager::State BluetoothManager::state() const noexcept
{
	return static_cast<State>(atomic_get(&state_));
}

#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
void BluetoothManager::connected_callback(struct bt_conn *connection, std::uint8_t error)
{
	if (instance_ == nullptr || connection != instance_->connection_) {
		return;
	}
	if (error == 0U) {
		atomic_set(&instance_->state_, static_cast<atomic_val_t>(State::connected));
	} else {
		instance_->connection_error_ = -static_cast<int>(error);
		atomic_set(&instance_->state_, static_cast<atomic_val_t>(State::enabled));
		bt_conn_unref(instance_->connection_);
		instance_->connection_ = nullptr;
	}
	k_sem_give(&instance_->state_changed_);
}

void BluetoothManager::disconnected_callback(struct bt_conn *connection, std::uint8_t reason)
{
	ARG_UNUSED(reason);
	if (instance_ == nullptr || connection != instance_->connection_) {
		return;
	}
	bt_conn_unref(instance_->connection_);
	instance_->connection_ = nullptr;
	atomic_set(&instance_->state_, static_cast<atomic_val_t>(State::enabled));
	k_sem_give(&instance_->state_changed_);
}
#endif

const char *to_string(BluetoothManager::State state) noexcept
{
	switch (state) {
	case BluetoothManager::State::disabled:
		return "disabled";
	case BluetoothManager::State::enabled:
		return "enabled";
	case BluetoothManager::State::connecting:
		return "connecting";
	case BluetoothManager::State::connected:
		return "connected";
	case BluetoothManager::State::disconnecting:
		return "disconnecting";
	}
	return "unknown";
}

} // namespace zest
