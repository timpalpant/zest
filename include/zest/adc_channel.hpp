/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zephyr/drivers/adc.h>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace zest
{

/** A devicetree-configured ADC channel. */
class AdcChannel
{
      public:
	constexpr explicit AdcChannel(adc_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Configure the underlying ADC channel. */
	[[nodiscard]] std::expected<void, int> init() const noexcept;

	/** Perform one conversion and return its raw sample value. */
	[[nodiscard]] std::expected<std::int32_t, int> read_raw() const noexcept;

	/** Perform one conversion and return the input voltage in millivolts. */
	[[nodiscard]] std::expected<std::int32_t, int> read_millivolts() const noexcept;

	/**
	 * Average a compile-time number of voltage conversions.
	 *
	 * The first failed conversion ends the operation and returns its negative
	 * Zephyr errno unchanged.
	 */
	template <std::size_t Samples>
	[[nodiscard]] std::expected<std::int32_t, int> read_average_millivolts() const noexcept
	{
		static_assert(Samples > 0U, "at least one ADC sample is required");

		std::int64_t total = 0;
		for (std::size_t i = 0; i < Samples; ++i) {
			const auto sample = read_millivolts();
			if (!sample) {
				return std::unexpected(sample.error());
			}
			total += *sample;
		}

		return static_cast<std::int32_t>(total / static_cast<std::int64_t>(Samples));
	}

      private:
	adc_dt_spec spec_;
};

} /* namespace zest */
