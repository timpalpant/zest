/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/units.hpp>

#include <zephyr/drivers/adc.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace zest
{

/**
 * A devicetree-configured ADC channel.
 *
 * Sample width follows the channel's configured resolution, so 18- and 24-bit
 * parts read correctly and an unsigned 16-bit value above `0x7FFF` does not come
 * back negative.
 */
class AdcChannel
{
      public:
	constexpr explicit AdcChannel(adc_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Configure the underlying ADC channel. */
	[[nodiscard]] Result<> init() const noexcept;

	/** Perform one conversion and return its raw sample value. */
	[[nodiscard]] Result<std::int32_t> read_raw() const noexcept;

	/** Perform one conversion and return the input voltage. */
	[[nodiscard]] Result<Millivolts> read_millivolts() const noexcept;

	/**
	 * Average @p samples conversions taken in a single hardware sequence.
	 *
	 * Uses `adc_sequence_options::extra_samplings`, so the driver is entered
	 * once rather than once per sample. Drivers that reject multi-sampling fall
	 * back to repeated single conversions automatically.
	 */
	[[nodiscard]] Result<Millivolts>
	read_average_millivolts(std::size_t samples) const noexcept;

	/** Compile-time sample count, for call sites that had one. */
	template <std::size_t Samples>
	[[nodiscard]] Result<Millivolts> read_average_millivolts() const noexcept
	{
		static_assert(Samples > 0U, "at least one ADC sample is required");
		return read_average_millivolts(Samples);
	}

	/** The channel's configured resolution in bits. */
	[[nodiscard]] constexpr std::uint8_t resolution() const noexcept
	{
		return spec_.resolution;
	}

	/** Whether samples are wider than 16 bits and so need 32-bit storage. */
	[[nodiscard]] constexpr bool wide_samples() const noexcept
	{
		return spec_.resolution > 16U;
	}

	[[nodiscard]] constexpr const adc_dt_spec &native_spec() const noexcept
	{
		return spec_;
	}

      private:
	/** Convert a raw reading to millivolts using the channel's reference. */
	[[nodiscard]] Result<Millivolts> to_millivolts(std::int32_t raw) const noexcept;

	adc_dt_spec spec_;
};

} /* namespace zest */
