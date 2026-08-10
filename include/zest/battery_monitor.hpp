/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/voltage_divider.hpp>

#include <cstdint>
#include <expected>

namespace zest
{

/**
 * Failures are reported as the negative errno from the underlying Zephyr ADC
 * call, passed through unchanged.
 */
using Error = int;

/**
 * Reads battery voltage through a resistive divider.
 *
 * The divider ratio is given as the two resistances from the devicetree
 * `voltage-divider` node.
 */
class BatteryMonitor
{
      public:
	constexpr BatteryMonitor(adc_dt_spec channel, std::int32_t output_ohms,
				 std::int32_t full_ohms) noexcept
		: divider_{channel, output_ohms, full_ohms}
	{
	}

	/** Configure the ADC channel. Call once before read_millivolts(). */
	[[nodiscard]] std::expected<void, Error> init() const noexcept;

	/**
	 * Sample the battery.
	 *
	 * Averages kOversample conversions to reduce ADC noise.
	 */
	[[nodiscard]] std::expected<std::int32_t, Error> read_millivolts() const noexcept;

      private:
	/* Number of ADC conversions averaged into one reading. */
	static constexpr int kOversample = 16;

	VoltageDivider divider_;
};

} /* namespace zest */
