/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/system.hpp>

#include <zephyr/kernel.h>
#include <zephyr/sys/timeutil.h>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace zest
{

#if defined(CONFIG_ZEST_DEVICE_IDENTITY)

Result<RebootReason> RebootReason::read() noexcept
{
	RebootReason reason{};
	ZEST_TRY(check(hwinfo_get_reset_cause(&reason.causes)));
	ZEST_TRY(check(hwinfo_get_supported_reset_cause(&reason.supported)));
	return reason;
}

Result<> RebootReason::clear() noexcept
{
	return check(hwinfo_clear_reset_cause());
}

Result<std::span<const std::byte>> DeviceIdentity::read(std::span<std::byte> destination) noexcept
{
	if (destination.empty()) {
		return fail(errors::invalid_argument);
	}
	const ssize_t size = hwinfo_get_device_id(
		reinterpret_cast<std::uint8_t *>(destination.data()), destination.size());
	if (size < 0) {
		return fail(static_cast<int>(size));
	}
	if (size == 0) {
		return fail(errors::no_data);
	}
	return std::span<const std::byte>{destination.data(), static_cast<std::size_t>(size)};
}

Result<std::string_view> DeviceIdentity::read_hex(std::span<char> destination) noexcept
{
	std::array<std::byte, 16> raw{};
	auto id_result = read(raw);
	if (!id_result) {
		return fail(id_result.error());
	}
	auto id = *id_result;

	if (destination.size() < id.size() * 2U + 1U) {
		return fail(errors::no_buffer_space);
	}
	constexpr char digits[] = "0123456789abcdef";
	std::size_t offset = 0U;
	for (const std::byte value : id) {
		const auto byte = static_cast<unsigned>(value);
		destination[offset++] = digits[(byte >> 4U) & 0x0FU];
		destination[offset++] = digits[byte & 0x0FU];
	}
	destination[offset] = '\0';
	return std::string_view{destination.data(), offset};
}

Result<std::array<std::byte, 8>> DeviceIdentity::eui64() noexcept
{
	std::array<std::byte, 8> result{};
	ZEST_TRY(check(hwinfo_get_device_eui64(reinterpret_cast<std::uint8_t *>(result.data()))));
	return result;
}

#endif /* CONFIG_ZEST_DEVICE_IDENTITY */

#if defined(CONFIG_ZEST_WATCHDOG)

Result<WatchdogChannel> WatchdogDevice::install(std::chrono::milliseconds timeout,
						std::uint8_t reset_flags) noexcept
{
	if (device_ == nullptr || !device_is_ready(device_)) {
		return fail(errors::no_device);
	}
	if (running_) {
		/* wdt_setup() has already run; the hardware will reject a new channel. */
		return fail(errors::busy);
	}
	if (timeout <= std::chrono::milliseconds::zero() ||
	    timeout.count() > std::numeric_limits<std::uint32_t>::max()) {
		return fail(errors::invalid_argument);
	}

	const wdt_timeout_cfg config{
		.window = {.min = 0U, .max = static_cast<std::uint32_t>(timeout.count())},
		.callback = nullptr,
		.flags = reset_flags,
	};
	auto channel_result = check_value(wdt_install_timeout(device_, &config));
	if (!channel_result) {
		return fail(channel_result.error());
	}
	auto channel = *channel_result;
	return WatchdogChannel{this, channel};
}

Result<> WatchdogDevice::start(std::uint8_t options) noexcept
{
	if (device_ == nullptr || !device_is_ready(device_)) {
		return fail(errors::no_device);
	}
	if (running_) {
		return fail(errors::already);
	}
	ZEST_TRY(check(wdt_setup(device_, options)));
	running_ = true;
	return {};
}

Result<> WatchdogDevice::stop() noexcept
{
	if (!running_) {
		return {};
	}
	ZEST_TRY(check(wdt_disable(device_)));
	running_ = false;
	return {};
}

Result<> WatchdogChannel::feed() const noexcept
{
	if (owner_ == nullptr || channel_ < 0) {
		return fail(errors::invalid_argument);
	}
	if (!owner_->running()) {
		return fail(errors::permission_denied);
	}
	return check(wdt_feed(owner_->native_handle(), channel_));
}

#endif /* CONFIG_ZEST_WATCHDOG */

#if defined(CONFIG_ZEST_RTC)

Result<> Rtc::init() const noexcept
{
	if (device_ == nullptr || !device_is_ready(device_)) {
		return fail(errors::no_device);
	}
	return {};
}

Result<rtc_time> Rtc::get() const noexcept
{
	ZEST_TRY(init());
	rtc_time value{};
	ZEST_TRY(check(rtc_get_time(device_, &value)));
	return value;
}

Result<> Rtc::set(const rtc_time &time) const noexcept
{
	ZEST_TRY(init());
	return check(rtc_set_time(device_, &time));
}

Result<std::chrono::system_clock::time_point> Rtc::now() const noexcept
{
	auto value_result = get();
	if (!value_result) {
		return fail(value_result.error());
	}
	auto value = *value_result;
	tm broken{};
	broken.tm_sec = value.tm_sec;
	broken.tm_min = value.tm_min;
	broken.tm_hour = value.tm_hour;
	broken.tm_mday = value.tm_mday;
	broken.tm_mon = value.tm_mon;
	broken.tm_year = value.tm_year;
	broken.tm_wday = value.tm_wday;
	broken.tm_yday = value.tm_yday;
	broken.tm_isdst = value.tm_isdst;

	const time_t seconds = timeutil_timegm(&broken);
	return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

Result<> Rtc::set(std::chrono::system_clock::time_point when) const noexcept
{
	const auto seconds =
		std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
	tm broken{};
	const time_t stamp = static_cast<time_t>(seconds);
	if (gmtime_r(&stamp, &broken) == nullptr) {
		return fail(errors::invalid_argument);
	}

	rtc_time value{};
	value.tm_sec = broken.tm_sec;
	value.tm_min = broken.tm_min;
	value.tm_hour = broken.tm_hour;
	value.tm_mday = broken.tm_mday;
	value.tm_mon = broken.tm_mon;
	value.tm_year = broken.tm_year;
	value.tm_wday = broken.tm_wday;
	value.tm_yday = broken.tm_yday;
	value.tm_isdst = broken.tm_isdst;
	value.tm_nsec = 0;
	return set(value);
}

#endif /* CONFIG_ZEST_RTC */

#if defined(CONFIG_ZEST_POWER_MANAGER)

Result<> PowerManager::force_next(std::uint8_t cpu, pm_state state, std::uint8_t substate_id,
				  std::uint32_t minimum_residency_us,
				  std::uint32_t exit_latency_us) noexcept
{
	const pm_state_info info{
		.state = state,
		.substate_id = substate_id,
		.min_residency_us = minimum_residency_us,
		.exit_latency_us = exit_latency_us,
	};
	if (!pm_state_force(cpu, &info)) {
		return fail(errors::not_supported);
	}
	return {};
}

#endif /* CONFIG_ZEST_POWER_MANAGER */

} /* namespace zest */
