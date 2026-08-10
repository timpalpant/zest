/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/network_monitor.hpp>

#include <cerrno>

namespace zest
{
namespace
{
constexpr atomic_val_t interface_up = 1 << 0;
constexpr atomic_val_t ipv4_ready = 1 << 1;
constexpr atomic_val_t ipv6_ready = 1 << 2;
} // namespace

NetworkMonitor::NetworkMonitor(net_if *interface) noexcept : interface_{interface}
{
	k_sem_init(&changed_, 0, 1);
	interface_callback_.owner = this;
	ipv4_callback_.owner = this;
	ipv6_callback_.owner = this;
}

NetworkMonitor::~NetworkMonitor() noexcept
{
	stop();
}

std::expected<void, int> NetworkMonitor::start() noexcept
{
	if (interface_ == nullptr) {
		return std::unexpected(-ENODEV);
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
	net_mgmt_add_event_callback(&interface_callback_.callback);
	net_mgmt_add_event_callback(&ipv4_callback_.callback);
#if defined(CONFIG_NET_IPV6)
	net_mgmt_add_event_callback(&ipv6_callback_.callback);
#endif
	started_ = true;
	if (net_if_is_up(interface_)) {
		atomic_or(&flags_, interface_up);
	}
	if (net_if_ipv4_get_global_addr(interface_, NET_ADDR_PREFERRED) != nullptr) {
		atomic_or(&flags_, ipv4_ready);
	}
#if defined(CONFIG_NET_IPV6)
	auto *ipv6_interface = interface_;
	if (net_if_ipv6_get_global_addr(NET_ADDR_PREFERRED, &ipv6_interface) != nullptr &&
	    ipv6_interface == interface_) {
		atomic_or(&flags_, ipv6_ready);
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
	const auto flags = atomic_get(&flags_);
	return {
		.interface_up = (flags & interface_up) != 0,
		.ipv4_ready = (flags & ipv4_ready) != 0,
		.ipv6_ready = (flags & ipv6_ready) != 0,
	};
}

std::expected<NetworkState, int>
NetworkMonitor::wait_for_ipv4(std::chrono::milliseconds timeout) noexcept
{
	if (!started_) {
		return std::unexpected(-EACCES);
	}
	const auto deadline = k_uptime_get() + timeout.count();
	while (!state().ipv4_ready) {
		const auto remaining = deadline - k_uptime_get();
		if (remaining <= 0 || k_sem_take(&changed_, K_MSEC(remaining)) != 0) {
			return std::unexpected(-ETIMEDOUT);
		}
	}
	return state();
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
		atomic_or(&flags_, interface_up);
	} else if (event == NET_EVENT_IF_DOWN) {
		atomic_and(&flags_, ~(interface_up | ipv4_ready | ipv6_ready));
	} else if (event == NET_EVENT_IPV4_ADDR_ADD || event == NET_EVENT_IPV4_DHCP_BOUND) {
		atomic_or(&flags_, ipv4_ready);
	} else if (event == NET_EVENT_IPV4_ADDR_DEL || event == NET_EVENT_IPV4_DHCP_STOP) {
		atomic_and(&flags_, ~ipv4_ready);
#if defined(CONFIG_NET_IPV6)
	} else if (event == NET_EVENT_IPV6_ADDR_ADD) {
		atomic_or(&flags_, ipv6_ready);
	} else if (event == NET_EVENT_IPV6_ADDR_DEL) {
		atomic_and(&flags_, ~ipv6_ready);
#endif
	}
	k_sem_give(&changed_);
}

} /* namespace zest */
