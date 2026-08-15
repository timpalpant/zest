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

	Result<> init() const noexcept
	{
		if (measured_.count() <= 0 || full_.count() < measured_.count()) {
			return fail(errors::invalid_argument);
		}
		return channel_.init();
	}

	/** Sample once and reconstruct the divider's input voltage, in microvolts. */
	Result<Microvolts> read_microvolts() const noexcept
	{
		ZEST_TRY_ASSIGN(output, channel_.read_microvolts());
		return divider_input(output, measured_, full_);
	}

	/**
	 * Average @p samples conversions, then reconstruct the input voltage, in
	 * microvolts.
	 */
	Result<Microvolts> read_average_microvolts(std::size_t samples) const noexcept
	{
		ZEST_TRY_ASSIGN(output, channel_.read_average_microvolts(samples));
		return divider_input(output, measured_, full_);
	}

	/** Compile-time sample count, for call sites that had one. */
	template <std::size_t Samples>
	Result<Microvolts> read_average_microvolts() const noexcept
	{
		static_assert(Samples > 0U, "at least one ADC sample is required");
		return read_average_microvolts(Samples);
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
