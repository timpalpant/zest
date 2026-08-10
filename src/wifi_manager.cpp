#include <zest/wifi_manager.hpp>
#include <zest/retry.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>

namespace zest
{

WifiManager *WifiManager::instance_ = nullptr;

WifiManager::WifiManager() noexcept : iface_{net_if_get_default()}
{
	k_sem_init(&state_changed_, 0, 1);
	k_mutex_init(&mutex_);

	/* The ESP32 station driver exposes one default Wi-Fi interface, therefore
	 * one process-wide manager owns its event callbacks.
	 */
	if (instance_ != nullptr) {
		return;
	}
	instance_ = this;

	net_mgmt_init_event_callback(&wifi_callback_, event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_COMPLETE);
	net_mgmt_add_event_callback(&wifi_callback_);

	/* Event masks can only combine events from the same management layer. */
	net_mgmt_init_event_callback(&ipv4_callback_, event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND | NET_EVENT_IPV4_DHCP_STOP);
	net_mgmt_add_event_callback(&ipv4_callback_);
	callbacks_registered_ = true;
}

WifiManager::~WifiManager() noexcept
{
	if (callbacks_registered_) {
		net_mgmt_del_event_callback(&wifi_callback_);
		net_mgmt_del_event_callback(&ipv4_callback_);
	}
	if (instance_ == this) {
		instance_ = nullptr;
	}
}

void WifiManager::event_handler(struct net_mgmt_event_callback *callback, std::uint64_t event,
				struct net_if *iface) noexcept
{
	if (instance_ != nullptr) {
		instance_->handle_event(event, iface, callback->info);
	}
}

void WifiManager::handle_event(std::uint64_t event, struct net_if *iface, const void *info) noexcept
{
	if (iface != iface_) {
		return;
	}

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const auto *result = static_cast<const struct wifi_status *>(info);
		if (result != nullptr && result->status != 0) {
			atomic_set(&state_, static_cast<atomic_val_t>(State::disconnected));
			k_sem_give(&state_changed_);
		}
	} else if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		atomic_set(&state_, static_cast<atomic_val_t>(State::connected));
		k_sem_give(&state_changed_);
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT ||
		   event == NET_EVENT_WIFI_DISCONNECT_COMPLETE ||
		   event == NET_EVENT_IPV4_DHCP_STOP) {
		atomic_set(&state_, static_cast<atomic_val_t>(State::disconnected));
		k_sem_give(&state_changed_);
	}
}

std::expected<WifiManager::ConnectionInfo, int>
WifiManager::connect(const Credentials &credentials, std::chrono::milliseconds timeout) noexcept
{
	if (iface_ == nullptr || instance_ != this) {
		return std::unexpected(-ENODEV);
	}
	if (credentials.ssid.empty() || credentials.ssid.size() > 32U ||
	    credentials.password.size() > 64U) {
		return std::unexpected(-EINVAL);
	}
	if (timeout <= std::chrono::milliseconds::zero()) {
		return std::unexpected(-ETIMEDOUT);
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	if (connected()) {
		auto info = status();
		k_mutex_unlock(&mutex_);
		return info;
	}

	std::ranges::fill(ssid_, 0U);
	std::ranges::fill(password_, 0U);
	std::ranges::copy(credentials.ssid, ssid_.begin());
	std::ranges::copy(credentials.password, password_.begin());

	struct wifi_connect_req_params params{};
	params.ssid = ssid_.data();
	params.ssid_length = static_cast<std::uint8_t>(credentials.ssid.size());
	params.channel = WIFI_CHANNEL_ANY;
	params.security =
		credentials.password.empty() ? WIFI_SECURITY_TYPE_NONE : WIFI_SECURITY_TYPE_PSK;
	params.psk = password_.data();
	params.psk_length = static_cast<std::uint8_t>(credentials.password.size());

	const std::int64_t deadline = k_uptime_get() + timeout.count();
	RetryPolicy retry{{
		.maximum_attempts = 0U,
		.initial_delay = std::chrono::seconds{1},
		.maximum_delay = std::chrono::seconds{5},
		.multiplier = 2.0,
	}};

	for (;;) {
		const std::int64_t remaining = deadline - k_uptime_get();
		if (remaining <= 0) {
			atomic_set(&state_, static_cast<atomic_val_t>(State::disconnected));
			k_mutex_unlock(&mutex_);
			return std::unexpected(-ETIMEDOUT);
		}

		/* A failed ESP32 association can be followed by another disconnect
		 * notification. Drain it before beginning the next attempt; any stale
		 * event that arrives afterward merely causes an early retry rather than
		 * escaping connect() as a false terminal failure.
		 */
		k_sem_reset(&state_changed_);
		atomic_set(&state_, static_cast<atomic_val_t>(State::connecting));
		int result = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface_, &params, sizeof(params));
		if (result != 0 && result != -EALREADY) {
			atomic_set(&state_, static_cast<atomic_val_t>(State::disconnected));
			k_mutex_unlock(&mutex_);
			return std::unexpected(result);
		}

		result = k_sem_take(&state_changed_, K_MSEC(remaining));
		if (connected()) {
			auto info = status();
			k_mutex_unlock(&mutex_);
			return info;
		}
		if (result != 0) {
			atomic_set(&state_, static_cast<atomic_val_t>(State::disconnected));
			k_mutex_unlock(&mutex_);
			return std::unexpected(-ETIMEDOUT);
		}

		const std::int64_t before_retry = deadline - k_uptime_get();
		if (before_retry <= 0) {
			atomic_set(&state_, static_cast<atomic_val_t>(State::disconnected));
			k_mutex_unlock(&mutex_);
			return std::unexpected(-ETIMEDOUT);
		}

		const auto retry_delay = retry.failure();
		const auto delay = std::min(*retry_delay, std::chrono::milliseconds{before_retry});
		k_sleep(K_MSEC(delay.count()));
	}
}

std::expected<void, int> WifiManager::disconnect(std::chrono::milliseconds timeout) noexcept
{
	if (iface_ == nullptr || instance_ != this) {
		return std::unexpected(-ENODEV);
	}

	k_mutex_lock(&mutex_, K_FOREVER);
	if (state() == State::disconnected) {
		k_mutex_unlock(&mutex_);
		return {};
	}

	k_sem_reset(&state_changed_);
	atomic_set(&state_, static_cast<atomic_val_t>(State::disconnecting));
	const int request_result = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface_, nullptr, 0);
	if (request_result != 0) {
		k_mutex_unlock(&mutex_);
		return std::unexpected(request_result);
	}

	const int wait_result = k_sem_take(&state_changed_, K_MSEC(timeout.count()));
	k_mutex_unlock(&mutex_);
	if (wait_result != 0) {
		return std::unexpected(-ETIMEDOUT);
	}
	return {};
}

std::expected<void, int> WifiManager::set_power_save(bool enabled) noexcept
{
	if (iface_ == nullptr || instance_ != this) {
		return std::unexpected(-ENODEV);
	}

	struct wifi_ps_params params{};
	params.enabled = enabled ? WIFI_PS_ENABLED : WIFI_PS_DISABLED;
	params.type = WIFI_PS_PARAM_STATE;
	const int result = net_mgmt(NET_REQUEST_WIFI_PS, iface_, &params, sizeof(params));
	if (result != 0) {
		return std::unexpected(result);
	}
	return {};
}

WifiManager::ConnectionInfo WifiManager::status() const noexcept
{
	ConnectionInfo result{};
	result.state = state();
	if (iface_ == nullptr) {
		return result;
	}

	struct wifi_iface_status wifi_status{};
	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface_, &wifi_status, sizeof(wifi_status)) ==
	    0) {
		result.rssi = wifi_status.rssi;
		result.channel = wifi_status.channel;
	}

	const struct net_in_addr *address = net_if_ipv4_get_global_addr(iface_, NET_ADDR_PREFERRED);
	if (address != nullptr) {
		const struct net_in_addr netmask = net_if_ipv4_get_netmask_by_addr(iface_, address);
		net_addr_ntop(AF_INET, address, result.address.data(), result.address.size());
		net_addr_ntop(AF_INET, &netmask, result.netmask.data(), result.netmask.size());
	}
	const struct net_in_addr gateway = net_if_ipv4_get_gw(iface_);
	net_addr_ntop(AF_INET, &gateway, result.gateway.data(), result.gateway.size());
	return result;
}

WifiManager::State WifiManager::state() const noexcept
{
	return static_cast<State>(atomic_get(&state_));
}

const char *to_string(WifiManager::State state) noexcept
{
	switch (state) {
	case WifiManager::State::disconnected:
		return "disconnected";
	case WifiManager::State::connecting:
		return "connecting";
	case WifiManager::State::connected:
		return "connected";
	case WifiManager::State::disconnecting:
		return "disconnecting";
	}
	return "unknown";
}

} // namespace zest
