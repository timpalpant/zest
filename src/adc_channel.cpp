/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/adc_channel.hpp>

#include <zephyr/drivers/adc.h>

#include <array>
#include <cerrno>
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

Result<> AdcChannelSpec::init() const noexcept
{
	if (!adc_is_ready_dt(&spec_)) {
		return fail(errors::no_device);
	}
	return check(adc_channel_setup_dt(&spec_));
}

Result<Microvolts> AdcChannelSpec::to_microvolts(std::int32_t raw) const noexcept
{
	std::int32_t value = raw;
	ZEST_TRY(check(adc_raw_to_microvolts_dt(&spec_, &value)));
	return Microvolts{value};
}

Result<> AdcChannelSpec::init_sequence(adc_sequence &sequence) const noexcept
{
	return check(adc_sequence_init_dt(&spec_, &sequence));
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
		ZEST_TRY(check(adc_sequence_init_dt(&spec_.native_spec(), &sequence)));
		ZEST_TRY(check(adc_read_dt(&spec_.native_spec(), &sequence)));
		return raw;
	}

	std::uint16_t raw = 0;
	sequence.buffer = &raw;
	sequence.buffer_size = sizeof(raw);
	ZEST_TRY(check(adc_sequence_init_dt(&spec_.native_spec(), &sequence)));
	ZEST_TRY(check(adc_read_dt(&spec_.native_spec(), &sequence)));

	/* A differential channel reports a signed value in the same width. */
	return spec_.widen(raw);
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
	if (samples <= kMaxBurst && !wide_samples() && burst_ != BurstSupport::unsupported) {
		std::array<std::uint16_t, kMaxBurst> buffer{};
		const adc_sequence_options options{
			.interval_us = 0U,
			.callback = nullptr,
			.user_data = nullptr,
			.extra_samplings = static_cast<std::uint16_t>(samples - 1U),
		};
		adc_sequence sequence{};
		if (adc_sequence_init_dt(&spec_.native_spec(), &sequence) == 0) {
			sequence.options = &options;
			sequence.buffer = buffer.data();
			sequence.buffer_size = samples * sizeof(std::uint16_t);

			const int status = adc_read_dt(&spec_.native_spec(), &sequence);
			if (status == 0) {
				burst_ = BurstSupport::supported;
				std::int64_t total = 0;
				for (std::size_t i = 0; i < samples; ++i) {
					/* Sign-extend per element, exactly as the
					 * single-shot path does. Summing a
					 * differential burst as unsigned would turn
					 * every negative sample into a large positive
					 * one and average to nonsense.
					 */
					total += spec_.widen(buffer[i]);
				}
				return static_cast<std::int32_t>(
					total / static_cast<std::int64_t>(samples));
			}

			/*
			 * The driver declined; fall through to the per-sample
			 * loop, which every driver supports.
			 *
			 * -ENOTSUP is it saying it has no multi-sample path at
			 * all. That is a property of the driver and will not
			 * change, so latch it and never ask this channel again:
			 * several drivers, adc_esp32.c among them, LOG_ERR when
			 * they decline, and a caller averaging on a timer would
			 * otherwise fill the console with one driver error per
			 * reading. Any other status is transient --- a busy
			 * converter, a bus that glitched --- so it is not
			 * remembered and the burst is tried again next time.
			 */
			if (Error{status} == errors::not_supported) {
				burst_ = BurstSupport::unsupported;
			}
		}
	}

	std::int64_t total = 0;
	for (std::size_t i = 0; i < samples; ++i) {
		auto raw_result = read_raw();
		if (!raw_result) {
			return fail(raw_result.error());
		}
		auto raw = *raw_result;
		total += raw;
	}
	return static_cast<std::int32_t>(total / static_cast<std::int64_t>(samples));
}

Result<Microvolts> AdcChannel::read_microvolts() const noexcept
{
	auto raw_result = read_raw();
	if (!raw_result) {
		return fail(raw_result.error());
	}
	auto raw = *raw_result;
	return to_microvolts(raw);
}

Result<Microvolts> AdcChannel::read_average_microvolts(std::size_t samples) const noexcept
{
	auto raw_result = read_average_raw(samples);
	if (!raw_result) {
		return fail(raw_result.error());
	}
	auto raw = *raw_result;
	return to_microvolts(raw);
}

} /* namespace zest */
