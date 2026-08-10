/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>

#include <chrono>

namespace zest
{

/** Typed access to a Zephyr RTC device. */
class Rtc
{
      public:
	constexpr explicit Rtc(const struct device *device) noexcept : device_{device}
	{
	}

	/** Verify the device is present and ready. */
	[[nodiscard]] Result<> init() const noexcept;

	[[nodiscard]] Result<rtc_time> get() const noexcept;
	[[nodiscard]] Result<> set(const rtc_time &time) const noexcept;

	/** Read the RTC as a system-clock time point. */
	[[nodiscard]] Result<std::chrono::system_clock::time_point> now() const noexcept;

	/** Set the RTC from a system-clock time point. */
	[[nodiscard]] Result<> set(std::chrono::system_clock::time_point when) const noexcept;

	[[nodiscard]] const struct device *native_handle() const noexcept
	{
		return device_;
	}

      private:
	const struct device *device_{};
};

} /* namespace zest */
