/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/pm/pm.h>

#include <cstdint>

namespace zest
{

/** Explicit Zephyr power-state policy requests. */
class PowerManager
{
      public:
	/** Force the next idle transition to a specific state. */
	static Result<> force_next(std::uint8_t cpu, pm_state state,
						 std::uint8_t substate_id = 0U,
						 std::uint32_t minimum_residency_us = 0U,
						 std::uint32_t exit_latency_us = 0U) noexcept;
};

} /* namespace zest */
