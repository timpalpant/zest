/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/units.hpp>
#include <zest/voltage_divider.hpp>

#include <cstddef>
#include <cstdint>

namespace zest
{

/**
 * Reads battery voltage through a resistive divider.
 *
 * The divider ratio is given as the two resistances from the devicetree
 * `voltage-divider` node. Charge estimation is deliberately separate: pair this
 * with a `BatteryCurve` from `zest/battery_curve.hpp`.
 */
class BatteryMonitor
{
      public:
	/** Number of ADC conversions averaged into one reading by default. */
	static constexpr std::size_t kDefaultOversample = 16U;

	constexpr BatteryMonitor(adc_dt_spec channel, Ohms measured, Ohms full,
				 std::size_t oversample = kDefaultOversample) noexcept
		: divider_{channel, measured, full}, oversample_{oversample == 0U ? 1U : oversample}
	{
	}

	constexpr BatteryMonitor(adc_dt_spec channel, std::int32_t output_ohms,
				 std::int32_t full_ohms,
				 std::size_t oversample = kDefaultOversample) noexcept
		: divider_{channel, output_ohms, full_ohms},
		  oversample_{oversample == 0U ? 1U : oversample}
	{
	}

	/** Configure the ADC channel. Call once before read_millivolts(). */
	[[nodiscard]] Result<> init() const noexcept
	{
		return divider_.init();
	}

	/** Sample the battery, averaging the configured number of conversions. */
	[[nodiscard]] Result<Millivolts> read_millivolts() const noexcept
	{
		return divider_.read_average_millivolts(oversample_);
	}

	[[nodiscard]] constexpr const VoltageDivider &divider() const noexcept
	{
		return divider_;
	}
	[[nodiscard]] constexpr std::size_t oversample() const noexcept
	{
		return oversample_;
	}

      private:
	VoltageDivider divider_;
	std::size_t oversample_;
};

} /* namespace zest */
