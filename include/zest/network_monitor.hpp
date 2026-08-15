/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/kernel.hpp>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

#include <chrono>
#include <cstdint>

namespace zest
{

struct NetworkState {
	bool interface_up{};
	bool ipv4_ready{};
	bool ipv6_ready{};

	/** Whether the interface can carry traffic on any family. */
	[[nodiscard]] constexpr bool ready() const noexcept
	{
		return interface_up && (ipv4_ready || ipv6_ready);
	}
};

/** Observes interface and address readiness without owning the interface. */
class NetworkMonitor
{
      public:
	explicit NetworkMonitor(net_if *interface = net_if_get_default()) noexcept;
	~NetworkMonitor() noexcept;
	NetworkMonitor(const NetworkMonitor &) = delete;
	NetworkMonitor &operator=(const NetworkMonitor &) = delete;

	[[nodiscard]] Result<> start() noexcept;
	void stop() noexcept;
	[[nodiscard]] NetworkState state() const noexcept;

	/**
	 * Wait for an IPv4 address.
	 *
	 * A `milliseconds::max()` timeout waits indefinitely.
	 */
	[[nodiscard]] Result<NetworkState>
	wait_for_ipv4(std::chrono::milliseconds timeout) noexcept;

	/** Wait for the interface to be usable on any address family. */
	[[nodiscard]] Result<NetworkState>
	wait_for_ready(std::chrono::milliseconds timeout) noexcept;

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

	template <typename Predicate>
	[[nodiscard]] Result<NetworkState> wait_until(std::chrono::milliseconds timeout,
						      Predicate ready) noexcept;

	net_if *interface_{};
	Callback interface_callback_{};
	Callback ipv4_callback_{};
	Callback ipv6_callback_{};
	Semaphore changed_{0U, 1U};
	atomic_t flags_{ATOMIC_INIT(0)};
	bool started_{};
};

} /* namespace zest */
