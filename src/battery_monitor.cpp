/*
 * Battery state-of-charge for the Adafruit HUZZAH32 / Feather ESP32.
 *
 * The board permanently ties the LiPo connector to A13 through a 100k/100k
 * divider, so the ADC always sees exactly half the pack voltage.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zest/battery_monitor.hpp>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>

#include <cstdint>
#include <expected>

namespace zest
{

std::expected<void, Error> BatteryMonitor::init() const
{
	if (output_ohms_ <= 0 || full_ohms_ < output_ohms_) {
		return std::unexpected(-EINVAL);
	}
	if (!adc_is_ready_dt(&channel_)) {
		return std::unexpected(-ENODEV);
	}

	if (const int rc = adc_channel_setup_dt(&channel_); rc < 0) {
		return std::unexpected(rc);
	}

	return {};
}

std::expected<std::int32_t, Error> BatteryMonitor::sample_mv() const
{
	std::uint16_t raw;

	adc_sequence seq{};
	seq.buffer = &raw;
	seq.buffer_size = sizeof(raw);

	if (const int rc = adc_sequence_init_dt(&channel_, &seq); rc < 0) {
		return std::unexpected(rc);
	}

	if (const int rc = adc_read_dt(&channel_, &seq); rc < 0) {
		return std::unexpected(rc);
	}

	std::int32_t val_mv = raw;
	if (const int rc = adc_raw_to_millivolts_dt(&channel_, &val_mv); rc < 0) {
		return std::unexpected(rc);
	}

	/* Undo the divider to recover the pack voltage. */
	return static_cast<std::int32_t>((static_cast<std::int64_t>(val_mv) * full_ohms_) /
					 output_ohms_);
}

std::expected<Reading, Error> BatteryMonitor::read() const
{
	std::int32_t total = 0;

	for (int i = 0; i < kOversample; ++i) {
		const auto mv = sample_mv();

		if (!mv) {
			return std::unexpected(mv.error());
		}

		total += *mv;
	}

	const std::int32_t millivolts = total / kOversample;

	return Reading{millivolts, percent_from_mv(millivolts)};
}

} /* namespace zest */
