/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/function.hpp>
#include <zest/kernel.hpp>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace zest
{

/**
 * A small BLE connection manager covering both roles.
 *
 * Central-role `connect()` takes a BLE identity address. Peripheral-role
 * `start_advertising()` was previously missing entirely, which left the archetypal
 * sensor node --- advertise, accept a connection, expose a characteristic ---
 * unable to use this class at all.
 */
class BluetoothManager
{
      public:
	enum class State : std::uint8_t {
		disabled,
		enabled,
		advertising,
		connecting,
		connected,
		disconnecting,
	};

	enum class AddressType : std::uint8_t {
		public_,
		random,
	};

	struct Peer {
		std::string_view address;
		AddressType type{AddressType::public_};
	};

	/** How the device should advertise in the peripheral role. */
	struct AdvertisingOptions {
		/** Allow a central to connect, rather than advertising only. */
		bool connectable{true};
		/** Include the device name in the advertisement. */
		bool include_name{true};
		std::chrono::milliseconds interval_min{100};
		std::chrono::milliseconds interval_max{150};
	};

	using StateHandler = InplaceFunction<void(State) noexcept, 3 * sizeof(void *)>;

	BluetoothManager() noexcept;
	~BluetoothManager() noexcept;

	BluetoothManager(const BluetoothManager &) = delete;
	BluetoothManager &operator=(const BluetoothManager &) = delete;

	/** Whether this instance owns the stack's connection callbacks. */
	[[nodiscard]] bool owns_stack() const noexcept;

	[[nodiscard]] Result<> enable(std::string_view device_name = {}) noexcept;
	[[nodiscard]] Result<> disable() noexcept;

	/** Begin advertising in the peripheral role. */
	[[nodiscard]] Result<> start_advertising(const AdvertisingOptions &options = {}) noexcept;
	[[nodiscard]] Result<> stop_advertising() noexcept;

	[[nodiscard]] Result<> connect(const Peer &peer,
				       std::chrono::milliseconds timeout = std::chrono::seconds{
					       30}) noexcept;
	[[nodiscard]] Result<> disconnect() noexcept;

	template <typename F> void on_state_change(F &&handler) noexcept
	{
		state_handler_ = std::forward<F>(handler);
	}

	[[nodiscard]] State state() const noexcept;
	[[nodiscard]] bool connected() const noexcept
	{
		return state() == State::connected;
	}
	[[nodiscard]] bool advertising() const noexcept
	{
		return advertising_;
	}

      private:
	static void connected_callback(struct bt_conn *connection, std::uint8_t error) noexcept;
	static void disconnected_callback(struct bt_conn *connection, std::uint8_t reason) noexcept;
	void set_state(State state) noexcept;

	struct bt_conn *connection_{};
	struct bt_conn_cb callbacks_{};
	Semaphore state_changed_{0U, 1U};
	Mutex mutex_{};
	StateHandler state_handler_{};
	atomic_t state_{ATOMIC_INIT(static_cast<atomic_val_t>(State::disabled))};
	int connection_error_{};
	bool advertising_{false};

	static BluetoothManager *instance_;
};

[[nodiscard]] const char *to_string(BluetoothManager::State state) noexcept;

} /* namespace zest */
