/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef ZEST_BLUETOOTH_MANAGER_HPP_
#define ZEST_BLUETOOTH_MANAGER_HPP_

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>

#include <zephyr/kernel.h>

#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
#include <zephyr/bluetooth/conn.h>
#endif

namespace zest
{

/* A small BLE connection manager. Zephyr's ESP32 support exposes BLE rather
 * than Arduino's BluetoothSerial API, so connect() accepts a BLE identity
 * address and its public/random address type.
 */
class BluetoothManager
{
      public:
	enum class State : std::uint8_t {
		disabled,
		enabled,
		connecting,
		connected,
		disconnecting,
	};

	enum class AddressType : std::uint8_t {
		public_,
		random
	};

	struct Peer {
		std::string_view address;
		AddressType type{AddressType::public_};
	};

	BluetoothManager() noexcept;
	~BluetoothManager() noexcept;

	BluetoothManager(const BluetoothManager &) = delete;
	BluetoothManager &operator=(const BluetoothManager &) = delete;

	[[nodiscard]] std::expected<void, int> enable(std::string_view device_name = {}) noexcept;
	[[nodiscard]] std::expected<void, int> disable() noexcept;
	[[nodiscard]] std::expected<void, int>
	connect(const Peer &peer,
		std::chrono::milliseconds timeout = std::chrono::seconds{30}) noexcept;
	[[nodiscard]] std::expected<void, int> disconnect() noexcept;

	[[nodiscard]] State state() const noexcept;
	[[nodiscard]] bool connected() const noexcept
	{
		return state() == State::connected;
	}

      private:
#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
	static void connected_callback(struct bt_conn *connection, std::uint8_t error) noexcept;
	static void disconnected_callback(struct bt_conn *connection, std::uint8_t reason) noexcept;
	struct bt_conn *connection_{};
	struct bt_conn_cb callbacks_{};
#endif
	struct k_sem state_changed_{};
	struct k_mutex mutex_{};
	atomic_t state_{ATOMIC_INIT(static_cast<atomic_val_t>(State::disabled))};
	int connection_error_{};
	static BluetoothManager *instance_;
};

[[nodiscard]] const char *to_string(BluetoothManager::State state) noexcept;

} // namespace zest

#endif
