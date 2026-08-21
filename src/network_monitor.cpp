/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/network_monitor.hpp>

namespace zest
{
namespace
{
constexpr atomic_val_t interface_up = 1 << 0;
constexpr atomic_val_t ipv4_ready = 1 << 1;
constexpr atomic_val_t ipv6_ready = 1 << 2;
} /* namespace */

NetworkMonitor::NetworkMonitor(net_if *interface) noexcept : interface_{interface}
{
	interface_callback_.owner = this;
	ipv4_callback_.owner = this;
	ipv6_callback_.owner = this;
}

NetworkMonitor::~NetworkMonitor() noexcept
{
	stop();
}

Result<> NetworkMonitor::start() noexcept
{
	if (interface_ == nullptr) {
		return fail(errors::no_device);
	}
	if (started_) {
		return {};
	}
	net_mgmt_init_event_callback(&interface_callback_.callback, event_handler,
				     NET_EVENT_IF_UP | NET_EVENT_IF_DOWN);
	net_mgmt_init_event_callback(&ipv4_callback_.callback, event_handler,
				     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL |
					     NET_EVENT_IPV4_DHCP_BOUND | NET_EVENT_IPV4_DHCP_STOP);

#if defined(CONFIG_NET_IPV6)
	net_mgmt_init_event_callback(&ipv6_callback_.callback, event_handler,
				     NET_EVENT_IPV6_ADDR_ADD | NET_EVENT_IPV6_ADDR_DEL);
#endif
	/* Register before sampling, so no transition can slip between the two. */
	net_mgmt_add_event_callback(&interface_callback_.callback);
	net_mgmt_add_event_callback(&ipv4_callback_.callback);
#if defined(CONFIG_NET_IPV6)
	net_mgmt_add_event_callback(&ipv6_callback_.callback);
#endif
	started_ = true;

	if (net_if_is_up(interface_)) {
		flags_.fetch_or(interface_up);
	}
	if (net_if_ipv4_get_global_addr(interface_, NET_ADDR_PREFERRED) != nullptr) {
		flags_.fetch_or(ipv4_ready);
	}
#if defined(CONFIG_NET_IPV6)
	auto *ipv6_interface = interface_;
	if (net_if_ipv6_get_global_addr(NET_ADDR_PREFERRED, &ipv6_interface) != nullptr &&
	    ipv6_interface == interface_) {
		flags_.fetch_or(ipv6_ready);
	}
#endif
	return {};
}

void NetworkMonitor::stop() noexcept
{
	if (!started_) {
		return;
	}
	net_mgmt_del_event_callback(&interface_callback_.callback);
	net_mgmt_del_event_callback(&ipv4_callback_.callback);
#if defined(CONFIG_NET_IPV6)
	net_mgmt_del_event_callback(&ipv6_callback_.callback);
#endif
	started_ = false;
}

NetworkState NetworkMonitor::state() const noexcept
{
	const auto flags = flags_.load();
	return {
		.interface_up = (flags & interface_up) != 0,
		.ipv4_ready = (flags & ipv4_ready) != 0,
		.ipv6_ready = (flags & ipv6_ready) != 0,
	};
}

template <typename Predicate>
Result<NetworkState> NetworkMonitor::wait_until(std::chrono::milliseconds timeout,
						Predicate ready) noexcept
{
	if (!started_) {
		return fail(errors::permission_denied);
	}
	if (ready(state())) {
		return state();
	}

	/*
	 * A max() timeout means "wait forever". Computing an absolute deadline from
	 * it overflows, which is why this waits on relative slices instead.
	 */
	const bool forever = timeout == std::chrono::milliseconds::max();
	auto remaining = timeout;

	while (!ready(state())) {
		if (forever) {
			ZEST_TRY(changed_.take(std::chrono::milliseconds::max()));
			continue;
		}
		if (remaining <= std::chrono::milliseconds::zero()) {
			return fail(errors::timed_out);
		}
		const auto slice_start = uptime();
		if (!changed_.take(remaining)) {
			return fail(errors::timed_out);
		}
		const auto elapsed = uptime() - slice_start;
		remaining -= elapsed > std::chrono::milliseconds::zero()
				     ? elapsed
				     : std::chrono::milliseconds{1};
	}
	return state();
}

Result<NetworkState> NetworkMonitor::wait_for_ipv4(std::chrono::milliseconds timeout) noexcept
{
	return wait_until(timeout, [](const NetworkState &state) { return state.ipv4_ready; });
}

Result<NetworkState> NetworkMonitor::wait_for_ready(std::chrono::milliseconds timeout) noexcept
{
	return wait_until(timeout, [](const NetworkState &state) { return state.ready(); });
}

void NetworkMonitor::event_handler(net_mgmt_event_callback *callback, std::uint64_t event,
				   net_if *interface) noexcept
{
	auto *wrapper = CONTAINER_OF(callback, Callback, callback);
	if (wrapper->owner != nullptr) {
		wrapper->owner->handle(event, interface);
	}
}

void NetworkMonitor::handle(std::uint64_t event, net_if *interface) noexcept
{
	if (interface != interface_) {
		return;
	}
	if (event == NET_EVENT_IF_UP) {
		flags_.fetch_or(interface_up);
	} else if (event == NET_EVENT_IF_DOWN) {
		flags_.fetch_and(~(interface_up | ipv4_ready | ipv6_ready));
	} else if (event == NET_EVENT_IPV4_ADDR_ADD || event == NET_EVENT_IPV4_DHCP_BOUND) {
		flags_.fetch_or(ipv4_ready);
	} else if (event == NET_EVENT_IPV4_ADDR_DEL || event == NET_EVENT_IPV4_DHCP_STOP) {
		flags_.fetch_and(~ipv4_ready);
#if defined(CONFIG_NET_IPV6)
	} else if (event == NET_EVENT_IPV6_ADDR_ADD) {
		flags_.fetch_or(ipv6_ready);
	} else if (event == NET_EVENT_IPV6_ADDR_DEL) {
		flags_.fetch_and(~ipv6_ready);
#endif
	}
	changed_.give();
}

} /* namespace zest */
