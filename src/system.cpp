/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/system.hpp>

#include <zephyr/kernel.h>

#include <algorithm>
#include <cerrno>
#include <limits>

namespace zest
{

#if defined(CONFIG_ZEST_DEVICE_IDENTITY)
std::expected<RebootReason, int> RebootReason::read() noexcept
{
	RebootReason reason{};
	int rc = hwinfo_get_reset_cause(&reason.causes);
	if (rc < 0) {
		return std::unexpected(rc);
	}
	rc = hwinfo_get_supported_reset_cause(&reason.supported);
	if (rc < 0) {
		return std::unexpected(rc);
	}
	return reason;
}

std::expected<void, int> RebootReason::clear() noexcept
{
	const int rc = hwinfo_clear_reset_cause();
	return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
}

std::expected<std::array<std::byte, 8>, int> DeviceIdentity::eui64() noexcept
{
	std::array<std::byte, 8> result{};
	const int rc = hwinfo_get_device_eui64(reinterpret_cast<std::uint8_t *>(result.data()));
	return rc == 0 ? std::expected<std::array<std::byte, 8>, int>{result} : std::unexpected(rc);
}
#endif

#if defined(CONFIG_ZEST_WATCHDOG)
std::expected<void, int> Watchdog::init(std::chrono::milliseconds timeout, std::uint8_t reset_flags,
					std::uint8_t options) noexcept
{
	if (device_ == nullptr || !device_is_ready(device_)) {
		return std::unexpected(-ENODEV);
	}
	if (timeout <= std::chrono::milliseconds::zero() ||
	    timeout.count() > std::numeric_limits<std::uint32_t>::max() || active_) {
		return std::unexpected(active_ ? -EALREADY : -EINVAL);
	}
	const wdt_timeout_cfg config{
		.window = {.min = 0U, .max = static_cast<std::uint32_t>(timeout.count())},
		.callback = nullptr,
		.flags = reset_flags,
	};
	channel_ = wdt_install_timeout(device_, &config);
	if (channel_ < 0) {
		return std::unexpected(channel_);
	}
	const int rc = wdt_setup(device_, options);
	if (rc < 0) {
		channel_ = -1;
		return std::unexpected(rc);
	}
	active_ = true;
	return {};
}

std::expected<void, int> Watchdog::feed() const noexcept
{
	if (!active_) {
		return std::unexpected(-EACCES);
	}
	const int rc = wdt_feed(device_, channel_);
	return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
}

std::expected<void, int> Watchdog::disable() noexcept
{
	if (!active_) {
		return {};
	}
	const int rc = wdt_disable(device_);
	if (rc == 0) {
		active_ = false;
		channel_ = -1;
		return {};
	}
	return std::unexpected(rc);
}
#endif

#if defined(CONFIG_ZEST_RTC)
std::expected<void, int> Rtc::init() const noexcept
{
	return device_ != nullptr && device_is_ready(device_) ? std::expected<void, int>{}
							      : std::unexpected(-ENODEV);
}

std::expected<rtc_time, int> Rtc::get() const noexcept
{
	if (auto ready = init(); !ready) {
		return std::unexpected(ready.error());
	}
	rtc_time value{};
	const int rc = rtc_get_time(device_, &value);
	return rc == 0 ? std::expected<rtc_time, int>{value} : std::unexpected(rc);
}

std::expected<void, int> Rtc::set(const rtc_time &time) const noexcept
{
	if (auto ready = init(); !ready) {
		return ready;
	}
	const int rc = rtc_set_time(device_, &time);
	return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
}
#endif

#if defined(CONFIG_ZEST_POWER_MANAGER)
std::expected<void, int> PowerManager::force_next(std::uint8_t cpu, pm_state state,
						  std::uint8_t substate_id,
						  std::uint32_t minimum_residency_us,
						  std::uint32_t exit_latency_us) noexcept
{
	const pm_state_info info{
		.state = state,
		.substate_id = substate_id,
		.min_residency_us = minimum_residency_us,
		.exit_latency_us = exit_latency_us,
	};
	return pm_state_force(cpu, &info) ? std::expected<void, int>{} : std::unexpected(-ENOTSUP);
}
#endif

} /* namespace zest */
