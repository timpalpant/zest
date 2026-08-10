/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/adc_channel.hpp>
#include <zest/error.hpp>
#include <zest/units.hpp>

#include <cstddef>
#include <cstdint>

namespace zest
{

/**
 * Reconstructs an input voltage measured through a resistive divider.
 *
 * The resistances are the `output-ohms` and `full-ohms` properties of a
 * devicetree `voltage-divider` node. Reconstruction is integer-only.
 */
class VoltageDivider
{
      public:
	constexpr VoltageDivider(adc_dt_spec channel, Ohms measured, Ohms full) noexcept
		: channel_{channel}, measured_{measured}, full_{full}
	{
	}

	/** Accepts raw ohms, for call sites reading straight from devicetree. */
	constexpr VoltageDivider(adc_dt_spec channel, std::int32_t output_ohms,
				 std::int32_t full_ohms) noexcept
		: channel_{channel}, measured_{output_ohms}, full_{full_ohms}
	{
	}

	[[nodiscard]] Result<> init() const noexcept
	{
		if (measured_.count() <= 0 || full_.count() < measured_.count()) {
			return fail(errors::invalid_argument);
		}
		return channel_.init();
	}

	/** Sample once and reconstruct the divider's input voltage. */
	[[nodiscard]] Result<Millivolts> read_millivolts() const noexcept
	{
		ZEST_TRY_ASSIGN(output, channel_.read_millivolts());
		return divider_input(output, measured_, full_);
	}

	/** Average @p samples conversions, then reconstruct. */
	[[nodiscard]] Result<Millivolts> read_average_millivolts(std::size_t samples) const noexcept
	{
		ZEST_TRY_ASSIGN(output, channel_.read_average_millivolts(samples));
		return divider_input(output, measured_, full_);
	}

	template <std::size_t Samples = 1U>
	[[nodiscard]] Result<Millivolts> read_millivolts() const noexcept
	{
		return read_average_millivolts(Samples);
	}

	[[nodiscard]] constexpr const AdcChannel &channel() const noexcept
	{
		return channel_;
	}
	[[nodiscard]] constexpr Ohms measured_resistance() const noexcept
	{
		return measured_;
	}
	[[nodiscard]] constexpr Ohms full_resistance() const noexcept
	{
		return full_;
	}

      private:
	AdcChannel channel_;
	Ohms measured_;
	Ohms full_;
};

} /* namespace zest */
