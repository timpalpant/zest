/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/retry.hpp>
#include <zest/wifi_manager.hpp>

#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>

#include <algorithm>
#include <cstring>

namespace zest
{
namespace
{

[[nodiscard]] wifi_security_type to_zephyr_security(WifiSecurity security) noexcept
{
	switch (security) {
	case WifiSecurity::open:
		return WIFI_SECURITY_TYPE_NONE;
	case WifiSecurity::psk:
		return WIFI_SECURITY_TYPE_PSK;
	case WifiSecurity::psk_sha256:
		return WIFI_SECURITY_TYPE_PSK_SHA256;
	case WifiSecurity::wpa3_sae:
		return WIFI_SECURITY_TYPE_SAE;
	case WifiSecurity::wpa2_enterprise:
		return WIFI_SECURITY_TYPE_EAP;
	}
	return WIFI_SECURITY_TYPE_NONE;
}

[[nodiscard]] WifiSecurity from_zephyr_security(int security) noexcept
{
	switch (security) {
	case WIFI_SECURITY_TYPE_NONE:
		return WifiSecurity::open;
	case WIFI_SECURITY_TYPE_PSK:
		return WifiSecurity::psk;
	case WIFI_SECURITY_TYPE_PSK_SHA256:
		return WifiSecurity::psk_sha256;
	case WIFI_SECURITY_TYPE_SAE:
		return WifiSecurity::wpa3_sae;
	case WIFI_SECURITY_TYPE_EAP:
		return WifiSecurity::wpa2_enterprise;
	default:
		break;
	}
	return WifiSecurity::open;
}

[[nodiscard]] WifiBand from_zephyr_band(int band) noexcept
{
	switch (band) {
	case WIFI_FREQ_BAND_2_4_GHZ:
		return WifiBand::band_2_4_ghz;
	case WIFI_FREQ_BAND_5_GHZ:
		return WifiBand::band_5_ghz;
	case WIFI_FREQ_BAND_6_GHZ:
		return WifiBand::band_6_ghz;
	default:
		break;
	}
	return WifiBand::any;
}

[[nodiscard]] std::uint8_t to_zephyr_band(WifiBand band) noexcept
{
	switch (band) {
	case WifiBand::band_2_4_ghz:
		return WIFI_FREQ_BAND_2_4_GHZ;
	case WifiBand::band_5_ghz:
		return WIFI_FREQ_BAND_5_GHZ;
	case WifiBand::band_6_ghz:
		return WIFI_FREQ_BAND_6_GHZ;
	case WifiBand::any:
		break;
	}
	return WIFI_FREQ_BAND_UNKNOWN;
}

} /* namespace */

WifiManager *WifiManager::instance_ = nullptr;

WifiManager::WifiManager() noexcept : iface_{net_if_get_default()}
{
	/*
	 * One manager owns the interface's event callbacks. A second instance stays
	 * inert and reports errors::no_device rather than fighting over them.
	 */
	if (instance_ != nullptr) {
		return;
	}
	instance_ = this;

	net_mgmt_init_event_callback(&wifi_callback_, event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_COMPLETE |
					     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
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

bool WifiManager::owns_interface() const noexcept
{
	return instance_ == this && iface_ != nullptr;
}

void WifiManager::set_state(State state) noexcept
{
	atomic_set(&state_, static_cast<atomic_val_t>(state));
	if (state_handler_) {
		state_handler_(state);
	}
	state_changed_.give();
}

void WifiManager::event_handler(struct net_mgmt_event_callback *callback, std::uint64_t event,
				struct net_if *iface) noexcept
{
	if (instance_ != nullptr) {
		instance_->handle_event(event, iface, callback->info);
	}
}

void WifiManager::record_scan_result(const void *info) noexcept
{
	if (!scanning_ || info == nullptr || scan_count_ >= scan_results_.size()) {
		return;
	}
	const auto *entry = static_cast<const struct wifi_scan_result *>(info);

	auto &slot = scan_results_[scan_count_];
	slot = WifiScanResult{};
	const auto length = std::min<std::size_t>(entry->ssid_length, slot.ssid.size() - 1U);
	std::memcpy(slot.ssid.data(), entry->ssid, length);
	slot.ssid_length = static_cast<std::uint8_t>(length);
	std::memcpy(slot.bssid.data(), entry->mac,
		    std::min<std::size_t>(entry->mac_length, slot.bssid.size()));
	slot.security = from_zephyr_security(entry->security);
	slot.band = from_zephyr_band(entry->band);
	slot.channel = entry->channel;
	slot.rssi = static_cast<std::int8_t>(entry->rssi);
	++scan_count_;
}

void WifiManager::handle_event(std::uint64_t event, struct net_if *iface, const void *info) noexcept
{
	if (iface != iface_) {
		return;
	}

	if (event == NET_EVENT_WIFI_SCAN_RESULT) {
		record_scan_result(info);
		return;
	}
	if (event == NET_EVENT_WIFI_SCAN_DONE) {
		scan_done_.give();
		return;
	}

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const auto *result = static_cast<const struct wifi_status *>(info);
		if (result != nullptr && result->status != 0) {
			set_state(State::disconnected);
		}
	} else if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		set_state(State::connected);
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT ||
		   event == NET_EVENT_WIFI_DISCONNECT_COMPLETE ||
		   event == NET_EVENT_IPV4_DHCP_STOP) {
		set_state(State::disconnected);
	}
}

Result<WifiManager::ConnectionInfo> WifiManager::connect(const Credentials &credentials,
							 std::chrono::milliseconds timeout) noexcept
{
	if (!owns_interface()) {
		return fail(errors::no_device);
	}
	if (credentials.ssid.empty() || credentials.ssid.size() > 32U ||
	    credentials.password.size() > 64U) {
		return fail(errors::invalid_argument);
	}
	/* Refuse a contradictory pairing rather than reinterpreting it. */
	if (credentials.security == WifiSecurity::open && !credentials.password.empty()) {
		return fail(errors::invalid_argument);
	}
	if (credentials.security != WifiSecurity::open && credentials.password.empty()) {
		return fail(errors::invalid_argument);
	}
	if (!credentials.bssid.empty() && credentials.bssid.size() != 6U) {
		return fail(errors::invalid_argument);
	}
	if (timeout <= std::chrono::milliseconds::zero()) {
		return fail(errors::timed_out);
	}

	ScopedLock lock{mutex_};
	if (connected()) {
		return status();
	}

	std::ranges::fill(ssid_, 0U);
	std::ranges::fill(password_, 0U);
	std::ranges::copy(credentials.ssid, ssid_.begin());
	std::ranges::copy(credentials.password, password_.begin());

	struct wifi_connect_req_params params{};
	params.ssid = ssid_.data();
	params.ssid_length = static_cast<std::uint8_t>(credentials.ssid.size());
	params.channel = credentials.channel == 0U ? WIFI_CHANNEL_ANY : credentials.channel;
	params.security = to_zephyr_security(credentials.security);
	params.band = to_zephyr_band(credentials.band);
	if (credentials.security != WifiSecurity::open) {
		params.psk = password_.data();
		params.psk_length = static_cast<std::uint8_t>(credentials.password.size());
	}
	if (!credentials.bssid.empty()) {
		std::ranges::copy(credentials.bssid, bssid_.begin());
		std::memcpy(params.bssid, bssid_.data(), bssid_.size());
	}

	const auto deadline = uptime() + timeout;
	RetryPolicy retry{{
		.maximum_attempts = 0U,
		.initial_delay = std::chrono::seconds{1},
		.maximum_delay = std::chrono::seconds{5},
		.multiplier_percent = 200U,
		.jitter_percent = 20U,
	}};

	for (;;) {
		const auto remaining = deadline - uptime();
		if (remaining <= std::chrono::milliseconds::zero()) {
			set_state(State::disconnected);
			return fail(errors::timed_out);
		}

		/*
		 * A failed association can be followed by another disconnect
		 * notification. Drain it before the next attempt; a stale event that
		 * arrives afterwards merely causes an early retry rather than
		 * escaping as a false terminal failure.
		 */
		state_changed_.reset();
		set_state(State::connecting);

		const int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface_, &params, sizeof(params));
		if (rc != 0 && rc != -EALREADY) {
			set_state(State::disconnected);
			return fail(rc);
		}

		const bool signalled = state_changed_.take(remaining).has_value();
		if (connected()) {
			return status();
		}
		if (!signalled) {
			set_state(State::disconnected);
			return fail(errors::timed_out);
		}

		const auto before_retry = deadline - uptime();
		if (before_retry <= std::chrono::milliseconds::zero()) {
			set_state(State::disconnected);
			return fail(errors::timed_out);
		}
		/* Unlimited attempts, so failure() always yields a delay; be explicit. */
		const auto delay = retry.failure().value_or(std::chrono::seconds{5});
		sleep_for(std::min(delay, before_retry));
	}
}

Result<> WifiManager::request_disconnect() noexcept
{
	if (!owns_interface()) {
		return fail(errors::no_device);
	}
	return check(net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface_, nullptr, 0));
}

Result<> WifiManager::disconnect(std::chrono::milliseconds timeout) noexcept
{
	if (!owns_interface()) {
		return fail(errors::no_device);
	}

	ScopedLock lock{mutex_};
	if (state() == State::disconnected) {
		return {};
	}

	state_changed_.reset();
	set_state(State::disconnecting);
	ZEST_TRY(check(net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface_, nullptr, 0)));

	if (!state_changed_.take(timeout)) {
		return fail(errors::timed_out);
	}
	return {};
}

Result<std::span<const WifiScanResult>>
WifiManager::scan(std::span<WifiScanResult> results, std::chrono::milliseconds timeout) noexcept
{
	if (!owns_interface()) {
		return fail(errors::no_device);
	}
	if (results.empty()) {
		return fail(errors::invalid_argument);
	}

	ScopedLock lock{mutex_};
	scan_results_ = results;
	scan_count_ = 0U;
	scan_done_.reset();
	scanning_ = true;

	const int rc = net_mgmt(NET_REQUEST_WIFI_SCAN, iface_, nullptr, 0);
	if (rc != 0) {
		scanning_ = false;
		scan_results_ = {};
		return fail(rc);
	}

	const bool completed = scan_done_.take(timeout).has_value();
	scanning_ = false;
	const auto found = scan_count_;
	scan_results_ = {};

	if (!completed) {
		return fail(errors::timed_out);
	}
	return std::span<const WifiScanResult>{results.data(), found};
}

Result<> WifiManager::set_power_save(bool enabled) noexcept
{
	if (!owns_interface()) {
		return fail(errors::no_device);
	}

	struct wifi_ps_params params{};
	params.enabled = enabled ? WIFI_PS_ENABLED : WIFI_PS_DISABLED;
	params.type = WIFI_PS_PARAM_STATE;
	return check(net_mgmt(NET_REQUEST_WIFI_PS, iface_, &params, sizeof(params)));
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

const char *to_string(WifiSecurity security) noexcept
{
	switch (security) {
	case WifiSecurity::open:
		return "open";
	case WifiSecurity::psk:
		return "WPA2-PSK";
	case WifiSecurity::psk_sha256:
		return "WPA2-PSK-SHA256";
	case WifiSecurity::wpa3_sae:
		return "WPA3-SAE";
	case WifiSecurity::wpa2_enterprise:
		return "WPA2-Enterprise";
	}
	return "unknown";
}

} /* namespace zest */
