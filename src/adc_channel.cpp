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

Result<Millivolts> AdcChannel::to_millivolts(std::int32_t raw) const noexcept
{
	std::int32_t value = raw;
	ZEST_TRY(check(adc_raw_to_millivolts_dt(&spec_, &value)));
	return Millivolts{value};
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

Result<Millivolts> AdcChannel::read_millivolts() const noexcept
{
	ZEST_TRY_ASSIGN(raw, read_raw());
	return to_millivolts(raw);
}

Result<Millivolts> AdcChannel::read_average_millivolts(std::size_t samples) const noexcept
{
	if (samples == 0U) {
		return fail(errors::invalid_argument);
	}

	/*
	 * Take the burst in one sequence where possible: entering the driver once
	 * for N samples rather than N times avoids N sequence setups and N
	 * raw-to-millivolt conversions.
	 */
	if (samples > 1U && samples <= kMaxBurst && !wide_samples() &&
	    !spec_.channel_cfg.differential) {
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
				std::int64_t total = 0;
				for (std::size_t i = 0; i < samples; ++i) {
					total += buffer[i];
				}
				return to_millivolts(static_cast<std::int32_t>(
					total / static_cast<std::int64_t>(samples)));
			}
			/* Driver declined multi-sampling; fall through. */
		}
	}

	std::int64_t total = 0;
	for (std::size_t i = 0; i < samples; ++i) {
		ZEST_TRY_ASSIGN(raw, read_raw());
		total += raw;
	}
	return to_millivolts(static_cast<std::int32_t>(total / static_cast<std::int64_t>(samples)));
}

} /* namespace zest */
