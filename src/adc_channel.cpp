/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/adc_channel.hpp>

#include <zephyr/drivers/adc.h>

#include <array>
#include <cstdint>

namespace zest
{
namespace
{

/*
 * Largest multi-sample burst taken in one sequence. Bounded so the sample buffer
 * stays a predictable size on the caller's stack.
 */
constexpr std::size_t kMaxBurst = 32U;

} /* namespace */

Result<> AdcChannel::init() const noexcept
{
	if (!adc_is_ready_dt(&spec_)) {
		return fail(errors::no_device);
	}
	return check(adc_channel_setup_dt(&spec_));
}

Result<Microvolts> AdcChannel::to_microvolts(std::int32_t raw) const noexcept
{
	std::int32_t value = raw;
	ZEST_TRY(check(adc_raw_to_microvolts_dt(&spec_, &value)));
	return Microvolts{value};
}

Result<std::int32_t> AdcChannel::read_raw() const noexcept
{
	adc_sequence sequence{};

	/*
	 * The buffer must suit the configured resolution: a 16-bit buffer cannot
	 * hold an 18- or 24-bit sample, and cannot represent an unsigned 16-bit
	 * reading above 0x7FFF.
	 */
	if (wide_samples()) {
		std::int32_t raw = 0;
		sequence.buffer = &raw;
		sequence.buffer_size = sizeof(raw);
		ZEST_TRY(check(adc_sequence_init_dt(&spec_, &sequence)));
		ZEST_TRY(check(adc_read_dt(&spec_, &sequence)));
		return raw;
	}

	std::uint16_t raw = 0;
	sequence.buffer = &raw;
	sequence.buffer_size = sizeof(raw);
	ZEST_TRY(check(adc_sequence_init_dt(&spec_, &sequence)));
	ZEST_TRY(check(adc_read_dt(&spec_, &sequence)));

	/* A differential channel reports a signed value in the same width. */
	if (spec_.channel_cfg.differential) {
		return static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
	}
	return static_cast<std::int32_t>(raw);
}

Result<std::int32_t> AdcChannel::read_average_raw(std::size_t samples) const noexcept
{
	if (samples == 0U) {
		return fail(errors::invalid_argument);
	}
	if (samples == 1U) {
		return read_raw();
	}

	/*
	 * Take the burst in one sequence where possible: entering the driver once
	 * for N samples rather than N times avoids N sequence setups, and on a
	 * bus-attached converter it is the difference between one transaction and
	 * N round trips.
	 */
	if (samples <= kMaxBurst && !wide_samples()) {
		std::array<std::uint16_t, kMaxBurst> buffer{};
		const adc_sequence_options options{
			.interval_us = 0U,
			.callback = nullptr,
			.user_data = nullptr,
			.extra_samplings = static_cast<std::uint16_t>(samples - 1U),
		};
		adc_sequence sequence{};
		if (adc_sequence_init_dt(&spec_, &sequence) == 0) {
			sequence.options = &options;
			sequence.buffer = buffer.data();
			sequence.buffer_size = samples * sizeof(std::uint16_t);

			if (adc_read_dt(&spec_, &sequence) == 0) {
				const bool differential = spec_.channel_cfg.differential;
				std::int64_t total = 0;
				for (std::size_t i = 0; i < samples; ++i) {
					/* Sign-extend per element, exactly as the
					 * single-shot path does. Summing a
					 * differential burst as unsigned would turn
					 * every negative sample into a large positive
					 * one and average to nonsense.
					 */
					total += differential
							 ? static_cast<std::int32_t>(
								   static_cast<std::int16_t>(
									   buffer[i]))
							 : static_cast<std::int32_t>(buffer[i]);
				}
				return static_cast<std::int32_t>(
					total / static_cast<std::int64_t>(samples));
			}
			/* Driver declined multi-sampling; fall through. */
		}
	}

	std::int64_t total = 0;
	for (std::size_t i = 0; i < samples; ++i) {
		ZEST_TRY_ASSIGN(raw, read_raw());
		total += raw;
	}
	return static_cast<std::int32_t>(total / static_cast<std::int64_t>(samples));
}

Result<Microvolts> AdcChannel::read_microvolts() const noexcept
{
	ZEST_TRY_ASSIGN(raw, read_raw());
	return to_microvolts(raw);
}

Result<Microvolts> AdcChannel::read_average_microvolts(std::size_t samples) const noexcept
{
	ZEST_TRY_ASSIGN(raw, read_average_raw(samples));
	return to_microvolts(raw);
}

} /* namespace zest */
