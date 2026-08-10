/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/*
 * Umbrella header for the system-management facilities.
 *
 * Each facility now lives in its own header matching its own Kconfig option, so
 * enabling ZEST_RTC alone no longer declares Watchdog::feed() and leaves it to
 * fail at link time with no hint about which option is missing. Include the
 * specific header where you can; this one is for convenience and includes only
 * what the build has enabled.
 */

#include <zest/error.hpp>
#include <zest/kernel.hpp>

#if defined(CONFIG_ZEST_DEVICE_IDENTITY)
#include <zest/device_identity.hpp>
#endif

#if defined(CONFIG_ZEST_WATCHDOG)
#include <zest/watchdog.hpp>
#endif

#if defined(CONFIG_ZEST_RTC)
#include <zest/rtc.hpp>
#endif

#if defined(CONFIG_ZEST_POWER_MANAGER)
#include <zest/power.hpp>
#endif

namespace zest
{

/**
 * Thread-context sleeps with chrono durations.
 *
 * Retained for compatibility; `zest::sleep_for()` in `zest/kernel.hpp` is the
 * same thing without the wrapper class. Both preserve sub-millisecond requests,
 * which the previous millisecond-truncating implementation silently dropped.
 */
class SleepController
{
      public:
	template <typename Rep, typename Period>
	static void sleep_for(std::chrono::duration<Rep, Period> duration) noexcept
	{
		zest::sleep_for(duration);
	}
};

} /* namespace zest */
