/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * Primitives for building on Zephyr's Bluetooth LE API directly.
 *
 * These are deliberately below the profile layer. A Bluetooth application's
 * value is in its profile — GATT, BAP, A2DP — and every profile brings its own
 * registration order, its own callback structs and its own negotiated
 * parameters, so there is no single class that covers Bluetooth the way
 * @ref WifiManager covers a station. What generalizes is the plumbing every
 * profile has to get right first: reference counting a `bt_conn` across
 * callbacks, telling one link's events from another's, and reading a peer
 * address without hand-sizing a buffer.
 *
 * Nothing here registers a profile, owns the stack, or assumes there is only
 * one connection, so these compose with hand-written profile code rather than
 * standing in front of it.
 */

#include <zest/error.hpp>
#include <zest/function.hpp>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace zest
{

/** Which side of a link this device is on. */
enum class BleRole : std::uint8_t {
	central,
	peripheral,
};

[[nodiscard]] const char *to_string(BleRole role) noexcept;

/**
 * A peer's identity address in text form.
 *
 * `bt_addr_le_to_str()` needs a caller-provided buffer of exactly the right
 * size, which is why the same `char addr[BT_ADDR_LE_STR_LEN]` declaration
 * appears in every connection callback ever written. This carries its own.
 */
class BleAddress
{
      public:
	/** An address that formats as "(none)". */
	BleAddress() noexcept;

	explicit BleAddress(const bt_addr_le_t *address) noexcept;

	/** The remote address of @p connection, or "(none)" if it has none. */
	explicit BleAddress(const bt_conn *connection) noexcept;

	/** Null-terminated, so it can go straight to a `%s`. */
	[[nodiscard]] const char *c_str() const noexcept
	{
		return text_.data();
	}

	[[nodiscard]] std::string_view view() const noexcept;

      private:
	std::array<char, BT_ADDR_LE_STR_LEN> text_{};
};

/**
 * An owning reference to a `bt_conn`.
 *
 * Zephyr's connection objects are reference counted, and the count is the
 * single easiest thing to get wrong in a Bluetooth application: the reference
 * taken in `connected()` has to be released in `disconnected()`, on the failure
 * path of `connected()` where the callback reports an HCI error, and again
 * wherever a connect attempt is abandoned. A missed `bt_conn_unref()` leaks a
 * slot out of the `CONFIG_BT_MAX_CONN` pool permanently — which surfaces much
 * later as an unexplained `-ENOMEM` from a connect or an advertiser start, not
 * as anything pointing at the leak.
 *
 * The two ways a connection reaches an application need different handling, so
 * they are named rather than left to an overload: @ref retain takes a new
 * reference to a connection the stack owns and passed to a callback, and
 * @ref adopt takes over a reference the caller already holds, as
 * `bt_conn_le_create()` leaves in its out-parameter.
 *
 * Move-only, and not synchronized. One thread — in practice one context —
 * owns the slot, and other contexts observe it. Because callbacks run on the
 * Bluetooth RX thread while application code usually waits on another, an
 * object shared across the two needs the same care as any other mutable state:
 * assign and reset it from a single context, or guard it.
 */
class BleConnection
{
      public:
	BleConnection() noexcept = default;

	/**
	 * Take a new reference to a connection owned by someone else.
	 *
	 * This is the callback case: `connected()` and friends are handed a
	 * borrowed pointer that is valid only for the duration of the call.
	 */
	[[nodiscard]] static BleConnection retain(bt_conn *connection) noexcept;

	/**
	 * Take over a reference the caller already holds.
	 *
	 * This is the `bt_conn_le_create()` case, and @ref connect's. Adopting a
	 * borrowed pointer under-counts and will free the connection early.
	 */
	[[nodiscard]] static BleConnection adopt(bt_conn *connection) noexcept;

	BleConnection(BleConnection &&other) noexcept
		: connection_{std::exchange(other.connection_, nullptr)}
	{
	}

	BleConnection &operator=(BleConnection &&other) noexcept
	{
		if (this != &other) {
			reset();
			connection_ = std::exchange(other.connection_, nullptr);
		}
		return *this;
	}

	BleConnection(const BleConnection &) = delete;
	BleConnection &operator=(const BleConnection &) = delete;

	~BleConnection() noexcept
	{
		reset();
	}

	/** Drop the reference, if any. Idempotent. */
	void reset() noexcept;

	/** Hand the reference to the caller, who becomes responsible for it. */
	[[nodiscard]] bt_conn *release() noexcept
	{
		return std::exchange(connection_, nullptr);
	}

	[[nodiscard]] bt_conn *get() const noexcept
	{
		return connection_;
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return connection_ != nullptr;
	}

	/**
	 * Whether this holds @p connection.
	 *
	 * Connection callbacks are broadcast to every registered `bt_conn_cb`, so
	 * the first line of a handler is always the question this answers. A null
	 * @p connection is never held, so an empty slot does not match a callback
	 * for some other link.
	 */
	[[nodiscard]] bool holds(const bt_conn *connection) const noexcept
	{
		return connection_ != nullptr && connection_ == connection;
	}

	[[nodiscard]] Result<bt_conn_info> info() const noexcept;
	[[nodiscard]] Result<BleRole> role() const noexcept;

	/** The peer's address, or "(none)" when this holds nothing. */
	[[nodiscard]] BleAddress address() const noexcept
	{
		return BleAddress{connection_};
	}

	/**
	 * Begin encryption or pairing.
	 *
	 * Asynchronous: success means the request was accepted, and the outcome
	 * arrives on `bt_conn_cb::security_changed`.
	 */
	[[nodiscard]] Result<> set_security(bt_security_t level) noexcept;

	/**
	 * Ask for the link to be dropped.
	 *
	 * Asynchronous, and the reference is kept: `disconnected()` still runs,
	 * and this object is what releases the reference afterwards.
	 */
	[[nodiscard]] Result<>
	disconnect(std::uint8_t reason = BT_HCI_ERR_REMOTE_USER_TERM_CONN) noexcept;

      private:
	explicit BleConnection(bt_conn *connection) noexcept : connection_{connection}
	{
	}

	bt_conn *connection_{};
};

/**
 * Open a central-role connection, returning the reference it produces.
 *
 * `bt_conn_le_create()` hands back a connection carrying one reference, and that
 * reference stays the caller's however the attempt ends. In particular a *failed*
 * attempt still owns one: the error arrives later on `connected()`, and the
 * reference has to be released afterwards rather than in the callback. Returning
 * an owning @ref BleConnection is what stops that release from being forgotten.
 *
 * Asynchronous: success means the attempt started, not that the link is up. Keep
 * the returned handle until `connected()` or `disconnected()` has reported the
 * outcome.
 */
[[nodiscard]] Result<BleConnection> ble_connect(const bt_addr_le_t &peer,
						const bt_conn_le_create_param &create_params,
						const bt_le_conn_param &connection_params) noexcept;

/**
 * A `bt_conn_cb` registration narrowed to one role.
 *
 * Zephyr broadcasts every connection callback to every registered `bt_conn_cb`,
 * with no way to subscribe to one link. An application that holds two at once —
 * a peripheral link to a phone and a central link to a headset, say — therefore
 * sees both links' events in both sets of handlers, and code written against a
 * single link is silently wrong the moment a second one exists.
 *
 * Role is the filter that works before a link has an identity: an incoming
 * connection is already known to be peripheral-role when `connected()` runs,
 * whereas its address may not be the one that was dialled. Handlers installed
 * here run only for connections in the requested role, so each link's code can
 * be written as though it were the only one.
 *
 * Handlers run on the Bluetooth RX thread and must not block. Delivery is scoped
 * to the object: the destructor stops it, so an observer must outlive the link
 * it watches.
 */
class BleConnectionObserver
{
      public:
	/** Capture budget for a handler, in bytes. Enough for a few pointers. */
	static constexpr std::size_t kHandlerCapacity = 4 * sizeof(void *);

	using ConnectionHandler =
		InplaceFunction<void(bt_conn *, std::uint8_t) noexcept, kHandlerCapacity>;
	using SecurityHandler =
		InplaceFunction<void(bt_conn *, bt_security_t, bt_security_err) noexcept,
				kHandlerCapacity>;

	/** Watch only @p role. */
	explicit BleConnectionObserver(BleRole role) noexcept;

	BleConnectionObserver(const BleConnectionObserver &) = delete;
	BleConnectionObserver &operator=(const BleConnectionObserver &) = delete;

	~BleConnectionObserver() noexcept;

	/**
	 * Install handlers, then begin receiving callbacks.
	 *
	 * Handlers must be installed first: once this returns, callbacks can
	 * arrive on another thread at any time, and replacing a handler after that
	 * races the dispatch that may be reading it.
	 */
	[[nodiscard]] Result<> start() noexcept;

	/** Stop receiving callbacks. Idempotent. */
	[[nodiscard]] Result<> stop() noexcept;

	[[nodiscard]] bool started() const noexcept
	{
		return started_;
	}

	/**
	 * Called for a completed or failed connection attempt in this role.
	 *
	 * The second argument is the HCI error: zero means the link is up. A
	 * non-zero value means it is not, and any reference taken for the attempt
	 * is the application's to release.
	 */
	template <typename F> void on_connected(F &&handler) noexcept
	{
		connected_ = std::forward<F>(handler);
	}

	/** Called when a link in this role drops, with the HCI reason. */
	template <typename F> void on_disconnected(F &&handler) noexcept
	{
		disconnected_ = std::forward<F>(handler);
	}

	/**
	 * Called when encryption or pairing completes or fails.
	 *
	 * Signalled on failure too, so a waiter does not sit out its timeout for
	 * an outcome that has already been decided.
	 *
	 * Zephyr only carries this callback when a security manager is compiled
	 * in, so without `CONFIG_BT_SMP` or `CONFIG_BT_CLASSIC` a handler
	 * installed here is accepted and never called.
	 */
	template <typename F> void on_security_changed(F &&handler) noexcept
	{
		security_changed_ = std::forward<F>(handler);
	}

      private:
	/*
	 * Zephyr's connection callbacks take no context argument — the dispatcher
	 * calls a bare `void (*)(bt_conn *, uint8_t)` and hands it nothing to
	 * recover an owner from, so a per-instance `bt_conn_cb` cannot be routed
	 * back to its instance by `CONTAINER_OF` or otherwise. Zest therefore
	 * registers one `bt_conn_cb` for the whole library and fans out along this
	 * list, filtering each observer by role on the way.
	 */
	friend struct BleObserverDispatch;

	ConnectionHandler connected_{};
	ConnectionHandler disconnected_{};
	SecurityHandler security_changed_{};
	BleConnectionObserver *next_{nullptr};
	BleRole role_;
	bool started_{false};
};

} /* namespace zest */
