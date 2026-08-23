/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/atomic.hpp>
#include <zest/error.hpp>
#include <zest/function.hpp>
#include <zest/kernel.hpp>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace zest
{

/** Station security modes. */
enum class WifiSecurity : std::uint8_t {
	open,
	psk,
	psk_sha256,
	wpa3_sae,
	wpa2_enterprise,
};

[[nodiscard]] const char *to_string(WifiSecurity security) noexcept;

/** Radio band to restrict the association to. */
enum class WifiBand : std::uint8_t {
	any,
	band_2_4_ghz,
	band_5_ghz,
	band_6_ghz,
};

/** One access point seen during a scan. */
struct WifiScanResult {
	std::array<char, 33> ssid{};
	std::uint8_t ssid_length{};
	std::array<std::uint8_t, 6> bssid{};
	WifiSecurity security{WifiSecurity::open};
	WifiBand band{WifiBand::any};
	std::uint8_t channel{};
	std::int8_t rssi{};

	[[nodiscard]] std::string_view ssid_view() const noexcept
	{
		return {ssid.data(), ssid_length};
	}
};

class WifiManager
{
      public:
	enum class State : std::uint8_t {
		disconnected,
		connecting,
		connected,
		disconnecting,
	};

	struct Credentials {
		std::string_view ssid;
		std::string_view password{};
		/**
		 * Security mode. `open` with a password, or `psk` without one, is
		 * rejected rather than silently reinterpreted.
		 */
		WifiSecurity security{WifiSecurity::psk};
		WifiBand band{WifiBand::any};
		/** Pin the association to one access point, or leave empty for any. */
		std::span<const std::uint8_t> bssid{};
		std::uint8_t channel{0U};
	};

	/**
	 * Interface addresses and radio status.
	 *
	 * Address fields are sized for IPv6 and exposed as views, so they can be
	 * printed directly.
	 */
	struct ConnectionInfo {
		State state{State::disconnected};
		std::array<char, 46> address{};
		std::array<char, 46> netmask{};
		std::array<char, 46> gateway{};
		std::int8_t rssi{};
		std::uint8_t channel{};

		[[nodiscard]] std::string_view address_view() const noexcept
		{
			return view_of(address);
		}
		[[nodiscard]] std::string_view netmask_view() const noexcept
		{
			return view_of(netmask);
		}
		[[nodiscard]] std::string_view gateway_view() const noexcept
		{
			return view_of(gateway);
		}

	      private:
		template <std::size_t N>
		[[nodiscard]] static std::string_view
		view_of(const std::array<char, N> &text) noexcept
		{
			std::size_t length = 0U;
			while (length < N && text[length] != '\0') {
				++length;
			}
			return {text.data(), length};
		}
	};

	/** Notified whenever the link state changes, from a workqueue context. */
	using StateHandler = InplaceFunction<void(State) noexcept, 3 * sizeof(void *)>;

	WifiManager() noexcept;
	~WifiManager() noexcept;

	WifiManager(const WifiManager &) = delete;
	WifiManager &operator=(const WifiManager &) = delete;

	/**
	 * Whether this instance owns the interface's event callbacks.
	 *
	 * One manager per interface. A second instance is inert rather than
	 * conflicting, and every operation on it reports `errors::no_device`.
	 */
	[[nodiscard]] bool owns_interface() const noexcept;

	/**
	 * Connect and wait for a usable DHCP address.
	 *
	 * The object is serialized for the whole call, which can be the full
	 * timeout. `disconnect()` and `scan()` from another thread will wait; use
	 * `request_disconnect()` to interrupt without blocking.
	 */
	[[nodiscard]] Result<ConnectionInfo>
	connect(const Credentials &credentials,
		std::chrono::milliseconds timeout = std::chrono::seconds{90}) noexcept;

	[[nodiscard]] Result<> disconnect(std::chrono::milliseconds timeout = std::chrono::seconds{
						  10}) noexcept;

	/** Ask the driver to disconnect without waiting for it to finish. */
	[[nodiscard]] Result<> request_disconnect() noexcept;

	/**
	 * Scan for access points, filling @p results and returning the ones found.
	 */
	[[nodiscard]] Result<std::span<const WifiScanResult>>
	scan(std::span<WifiScanResult> results,
	     std::chrono::milliseconds timeout = std::chrono::seconds{15}) noexcept;

	[[nodiscard]] Result<> set_power_save(bool enabled) noexcept;

	/** Install a state-change notifier, replacing any previous one. */
	template <typename F> void on_state_change(F &&handler) noexcept
	{
		ScopedLock lock{mutex_};
		state_handler_ = std::forward<F>(handler);
	}

	[[nodiscard]] ConnectionInfo status() const noexcept;
	[[nodiscard]] State state() const noexcept;
	[[nodiscard]] bool connected() const noexcept
	{
		return state() == State::connected;
	}

      private:
	static void event_handler(struct net_mgmt_event_callback *callback, std::uint64_t event,
				  struct net_if *iface) noexcept;
	void handle_event(std::uint64_t event, struct net_if *iface, const void *info) noexcept;
	void set_state(State state, bool notify_waiters = true) noexcept;
	void record_scan_result(const void *info) noexcept;
	[[nodiscard]] bool ipv4_ready() const noexcept;

	struct net_if *iface_{};
	struct net_mgmt_event_callback wifi_callback_{};
	struct net_mgmt_event_callback ipv4_callback_{};
	Semaphore state_changed_{0U, 1U};
	Semaphore scan_done_{0U, 1U};
	Mutex mutex_{};
	std::array<std::uint8_t, 33> ssid_{};
	std::array<std::uint8_t, 65> password_{};
	std::array<std::uint8_t, 6> bssid_{};
	StateHandler state_handler_{};
	std::span<WifiScanResult> scan_results_{};
	std::size_t scan_count_{0U};
	Atomic<State> state_{State::disconnected};
	bool callbacks_registered_{};
	bool scanning_{};

	static WifiManager *instance_;
};

[[nodiscard]] const char *to_string(WifiManager::State state) noexcept;

} /* namespace zest */
