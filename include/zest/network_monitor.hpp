/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

#include <chrono>
#include <cstdint>
#include <expected>

namespace zest
{

struct NetworkState {
	bool interface_up{};
	bool ipv4_ready{};
	bool ipv6_ready{};
};

/** Observes interface and address readiness without owning the interface. */
class NetworkMonitor
{
      public:
	explicit NetworkMonitor(net_if *interface = net_if_get_default()) noexcept;
	~NetworkMonitor() noexcept;
	NetworkMonitor(const NetworkMonitor &) = delete;
	NetworkMonitor &operator=(const NetworkMonitor &) = delete;

	[[nodiscard]] std::expected<void, int> start() noexcept;
	void stop() noexcept;
	[[nodiscard]] NetworkState state() const noexcept;
	[[nodiscard]] std::expected<NetworkState, int>
	wait_for_ipv4(std::chrono::milliseconds timeout) noexcept;
	[[nodiscard]] net_if *interface() const noexcept
	{
		return interface_;
	}

      private:
	struct Callback {
		net_mgmt_event_callback callback{};
		NetworkMonitor *owner{};
	};
	static void event_handler(net_mgmt_event_callback *callback, std::uint64_t event,
				  net_if *interface) noexcept;
	void handle(std::uint64_t event, net_if *interface) noexcept;

	net_if *interface_{};
	Callback interface_callback_{};
	Callback ipv4_callback_{};
	Callback ipv6_callback_{};
	k_sem changed_{};
	atomic_t flags_{ATOMIC_INIT(0)};
	bool started_{};
};

} /* namespace zest */
