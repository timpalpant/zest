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

	/**
	 * Perform one conversion and return the input voltage in microvolts.
	 *
	 * Microvolts are the finest scale the converter resolves and the one this
	 * class reports. A view of the reading in millivolts or volts is a
	 * `quantity_cast` to a coarser scale (it may truncate) --- which the units
	 * type already provides --- so there is no separate millivolt method.
	 *
	 * ```cpp
	 * if (auto micro = channel.read_microvolts()) {
	 *     const Millivolts coarse = quantity_cast<Millivolts>(*micro);
	 * }
	 * ```
	 */
	[[nodiscard]] Result<Microvolts> read_microvolts() const noexcept;

	/**
	 * Average @p samples conversions taken in a single hardware sequence, in
	 * microvolts.
	 *
	 * Uses `adc_sequence_options::extra_samplings`, so the driver is entered
	 * once rather than once per sample. Drivers that reject multi-sampling fall
	 * back to repeated single conversions automatically, and are asked only
	 * once: several of them log an error of their own when they decline, so a
	 * caller averaging on a timer would otherwise produce one driver error per
	 * reading for an operation that is working.
	 */
	[[nodiscard]] Result<Microvolts>
	read_average_microvolts(std::size_t samples) const noexcept;

	/** Compile-time sample count, for call sites that had one. */
	template <std::size_t Samples>
	[[nodiscard]] Result<Microvolts> read_average_microvolts() const noexcept
	{
		static_assert(Samples > 0U, "at least one ADC sample is required");
		return read_average_microvolts(Samples);
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
	/** Convert a raw reading to microvolts using the channel's reference. */
	[[nodiscard]] Result<Microvolts> to_microvolts(std::int32_t raw) const noexcept;

	/**
	 * Mean of @p samples raw conversions, burst where the driver allows it.
	 *
	 * Averaging in the raw domain rather than the converted one is deliberate:
	 * one raw-to-voltage conversion per burst instead of per sample, and no
	 * rounding to the output unit before the samples are summed.
	 */
	[[nodiscard]] Result<std::int32_t> read_average_raw(std::size_t samples) const noexcept;

	/**
	 * Whether the driver behind this channel accepts a multi-sample sequence.
	 *
	 * Discovered by asking, because the ADC API has no capability query, and
	 * then remembered, because the answer is a property of the driver rather
	 * than of any one reading.
	 */
	enum class BurstSupport : std::uint8_t {
		unknown,
		supported,
		unsupported,
	};

	adc_dt_spec spec_;

	/*
	 * Mutable because reading is const and this is a cache of what the driver
	 * can do, not state a caller can observe.
	 *
	 * Deliberately a plain member and not a zest::Atomic. Zephyr's ADC drivers
	 * serialize concurrent reads on a device, so two threads may share one
	 * channel and race here --- but they race to write the same value, because
	 * a driver either has a multi-sample path or it does not, and that does not
	 * vary per call. Losing the race costs one redundant probe, and an Atomic
	 * would cost the type its implicit copy and move operations for a hint that
	 * converges either way.
	 */
	mutable BurstSupport burst_{BurstSupport::unknown};
};

} /* namespace zest */
