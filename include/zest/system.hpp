/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * @brief Umbrella header for the system-management facilities.
 *
 * Each facility has its own header matching its own Kconfig option, and this one
 * includes whichever the build has enabled. Prefer the specific header: it makes
 * a missing Kconfig option a compile error naming the type, rather than a link
 * error naming a symbol.
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
