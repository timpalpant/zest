/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/ble.hpp>

#include <zest/kernel.hpp>

#include <cstring>

namespace zest
{

namespace
{

constexpr const char *kNoAddress = "(none)";

/** The role Zephyr reports for @p connection, if it can be read at all. */
[[nodiscard]] Result<BleRole> role_of(const bt_conn *connection) noexcept
{
	if (connection == nullptr) {
		return fail(errors::not_connected);
	}
	bt_conn_info info{};
	ZEST_TRY(check(bt_conn_get_info(connection, &info)));
	return info.role == BT_CONN_ROLE_CENTRAL ? BleRole::central : BleRole::peripheral;
}

} /* namespace */

const char *to_string(BleRole role) noexcept
{
	switch (role) {
	case BleRole::central:
		return "central";
	case BleRole::peripheral:
		return "peripheral";
	}
	return "unknown";
}

/* ----------------------------------------------------------------- address --- */

BleAddress::BleAddress() noexcept
{
	std::memcpy(text_.data(), kNoAddress, std::strlen(kNoAddress) + 1U);
}

BleAddress::BleAddress(const bt_addr_le_t *address) noexcept
{
	if (address == nullptr) {
		std::memcpy(text_.data(), kNoAddress, std::strlen(kNoAddress) + 1U);
		return;
	}
	/* bt_addr_le_to_str() always terminates within BT_ADDR_LE_STR_LEN. */
	(void)bt_addr_le_to_str(address, text_.data(), text_.size());
}

BleAddress::BleAddress(const bt_conn *connection) noexcept
	: BleAddress{connection != nullptr ? bt_conn_get_dst(connection) : nullptr}
{
}

std::string_view BleAddress::view() const noexcept
{
	std::size_t length = 0U;
	while (length < text_.size() && text_[length] != '\0') {
		++length;
	}
	return {text_.data(), length};
}

/* -------------------------------------------------------------- connection --- */

BleConnection BleConnection::retain(bt_conn *connection) noexcept
{
	if (connection == nullptr) {
		return BleConnection{};
	}
	return BleConnection{bt_conn_ref(connection)};
}

BleConnection BleConnection::adopt(bt_conn *connection) noexcept
{
	return BleConnection{connection};
}

void BleConnection::reset() noexcept
{
	if (connection_ != nullptr) {
		bt_conn_unref(connection_);
		connection_ = nullptr;
	}
}

Result<bt_conn_info> BleConnection::info() const noexcept
{
	if (connection_ == nullptr) {
		return fail(errors::not_connected);
	}
	bt_conn_info info{};
	ZEST_TRY(check(bt_conn_get_info(connection_, &info)));
	return info;
}

Result<BleRole> BleConnection::role() const noexcept
{
	return role_of(connection_);
}

Result<> BleConnection::set_security(bt_security_t level) noexcept
{
	if (connection_ == nullptr) {
		return fail(errors::not_connected);
	}
	return check(bt_conn_set_security(connection_, level));
}

Result<> BleConnection::disconnect(std::uint8_t reason) noexcept
{
	if (connection_ == nullptr) {
		return fail(errors::not_connected);
	}
	return check(bt_conn_disconnect(connection_, reason));
}

Result<BleConnection> ble_connect(const bt_addr_le_t &peer,
				  const bt_conn_le_create_param &create_params,
				  const bt_le_conn_param &connection_params) noexcept
{
	bt_conn *connection = nullptr;
	const int rc = bt_conn_le_create(&peer, &create_params, &connection_params, &connection);
	if (rc != 0) {
		/* A failing bt_conn_le_create() is not documented to hand back a
		 * reference, but releasing anything it did leave behind is cheaper
		 * than trusting that and leaking a slot out of the conn pool. */
		if (connection != nullptr) {
			bt_conn_unref(connection);
		}
		return fail(rc);
	}
	if (connection == nullptr) {
		return fail(errors::io_error);
	}
	return BleConnection::adopt(connection);
}

/* ---------------------------------------------------------------- observer --- */

/**
 * The library's single `bt_conn_cb` and the observers it fans out to.
 *
 * Registration and dispatch are serialized so an observer cannot be unlinked
 * while the Bluetooth RX thread is walking the list. Zephyr's `k_mutex` is
 * recursive for its owner, so a handler that starts or stops an observer from
 * inside a callback re-enters rather than deadlocking.
 */
struct BleObserverDispatch {
	static Mutex &lock() noexcept
	{
		static Mutex mutex;
		return mutex;
	}

	static BleConnectionObserver *&head() noexcept
	{
		static BleConnectionObserver *observers = nullptr;
		return observers;
	}

	/** Deliver to every started observer whose role matches @p connection. */
	template <typename Visit>
	static void dispatch(const bt_conn *connection, Visit &&visit) noexcept
	{
		/* A connection whose role cannot be read is not attributable to any
		 * observer, so drop it rather than deliver it to all of them. */
		const auto role = role_of(connection);
		if (!role) {
			return;
		}
		ScopedLock guard{lock()};
		for (auto *observer = head(); observer != nullptr; observer = observer->next_) {
			if (observer->started_ && observer->role_ == *role) {
				visit(*observer);
			}
		}
	}

	static void connected(bt_conn *connection, std::uint8_t error) noexcept
	{
		dispatch(connection, [&](BleConnectionObserver &observer) noexcept {
			if (observer.connected_) {
				observer.connected_(connection, error);
			}
		});
	}

	static void disconnected(bt_conn *connection, std::uint8_t reason) noexcept
	{
		dispatch(connection, [&](BleConnectionObserver &observer) noexcept {
			if (observer.disconnected_) {
				observer.disconnected_(connection, reason);
			}
		});
	}

#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
	static void security_changed(bt_conn *connection, bt_security_t level,
				     bt_security_err error) noexcept
	{
		dispatch(connection, [&](BleConnectionObserver &observer) noexcept {
			if (observer.security_changed_) {
				observer.security_changed_(connection, level, error);
			}
		});
	}
#endif /* CONFIG_BT_SMP || CONFIG_BT_CLASSIC */

	/** Register the shared callback set once, on the first observer to start. */
	[[nodiscard]] static Result<> ensure_registered() noexcept
	{
		static bt_conn_cb callbacks = {
			.connected = connected,
			.disconnected = disconnected,
/* Zephyr only declares this member when a security manager is compiled in;
 * without one, on_security_changed() is accepted but never fires. */
#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
			.security_changed = security_changed,
#endif
		};
		static bool registered = false;

		if (registered) {
			return {};
		}
		ZEST_TRY(check(bt_conn_cb_register(&callbacks)));
		registered = true;
		return {};
	}
};

BleConnectionObserver::BleConnectionObserver(BleRole role) noexcept : role_{role}
{
}

BleConnectionObserver::~BleConnectionObserver() noexcept
{
	(void)stop();
}

Result<> BleConnectionObserver::start() noexcept
{
	ScopedLock guard{BleObserverDispatch::lock()};
	if (started_) {
		return fail(errors::already);
	}
	ZEST_TRY(BleObserverDispatch::ensure_registered());

	next_ = BleObserverDispatch::head();
	BleObserverDispatch::head() = this;
	started_ = true;
	return {};
}

Result<> BleConnectionObserver::stop() noexcept
{
	ScopedLock guard{BleObserverDispatch::lock()};
	if (!started_) {
		return {};
	}
	/* The shared bt_conn_cb stays registered: unregistering it would silence
	 * every other observer, and an unlinked one costs nothing to skip. */
	for (auto **link = &BleObserverDispatch::head(); *link != nullptr; link = &(*link)->next_) {
		if (*link == this) {
			*link = next_;
			break;
		}
	}
	next_ = nullptr;
	started_ = false;
	return {};
}

} /* namespace zest */
