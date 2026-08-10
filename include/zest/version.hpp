/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/** Zest version, so an application can feature-test against a minimum. */
#define ZEST_VERSION_MAJOR 0
#define ZEST_VERSION_MINOR 2
#define ZEST_VERSION_PATCH 0

#define ZEST_VERSION_NUMBER                                                                        \
	((ZEST_VERSION_MAJOR * 10000) + (ZEST_VERSION_MINOR * 100) + ZEST_VERSION_PATCH)

/** True when the library is at least the given version. */
#define ZEST_VERSION_AT_LEAST(major, minor, patch)                                                 \
	(ZEST_VERSION_NUMBER >= ((major) * 10000 + (minor) * 100 + (patch)))

#define ZEST_VERSION_STRING "0.2.0"
