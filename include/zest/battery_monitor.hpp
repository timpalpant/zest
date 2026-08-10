/*
 * Battery state-of-charge for the Adafruit HUZZAH32 / Feather ESP32.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/drivers/adc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace zest
{

/** A single battery measurement. */
struct Reading {
	/** Pack voltage, in millivolts. */
	std::int32_t millivolts;
	/** Estimated state of charge, 0..100. */
	std::uint8_t percent;
};

/**
 * Failures are reported as the negative errno from the underlying Zephyr ADC
 * call, passed through unchanged.
 */
using Error = int;

/** One point on the discharge curve. */
struct CurvePoint {
	std::int32_t millivolts;
	std::uint8_t percent;
};

/*
 * Discharge curve for a single-cell LiPo at a light load, ordered from full to
 * empty.  Readings between two points are interpolated linearly.
 *
 * This is a resting-voltage curve: under a heavy load, or while the pack is
 * being charged over USB, the measured voltage will not reflect the true state
 * of charge.
 */
inline constexpr std::array<CurvePoint, 21> kCurve{{
	{4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
	{3910, 65},  {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
	{3770, 30},  {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},  {3270, 0},
}};

/** Map a pack voltage onto the discharge curve above. */
constexpr std::uint8_t percent_from_mv(std::int32_t millivolts)
{
	if (millivolts >= kCurve.front().millivolts) {
		return kCurve.front().percent;
	}

	for (std::size_t i = 1; i < kCurve.size(); ++i) {
		const CurvePoint &hi = kCurve[i - 1];
		const CurvePoint &lo = kCurve[i];

		if (millivolts >= lo.millivolts) {
			const std::int32_t rise = hi.percent - lo.percent;
			const std::int32_t run = hi.millivolts - lo.millivolts;

			return static_cast<std::uint8_t>(lo.percent +
							 (millivolts - lo.millivolts) * rise / run);
		}
	}

	return 0;
}

/* The curve is cheap to evaluate at compile time, so pin down its behaviour
 * here rather than needing hardware to notice a bad edit.
 */
static_assert(percent_from_mv(4300) == 100, "above full clamps to 100");
static_assert(percent_from_mv(4200) == 100, "full");
static_assert(percent_from_mv(4175) == 97, "interpolates between points");
static_assert(percent_from_mv(3845) == 52, "interpolates on the flat region");
static_assert(percent_from_mv(3270) == 0, "empty");
static_assert(percent_from_mv(3000) == 0, "below empty clamps to 0");

/**
 * Reads state of charge from a battery sitting behind a resistive divider.
 *
 * The divider ratio is given as the two resistances from the devicetree
 * `voltage-divider` node, so the class stays independent of this board.
 */
class BatteryMonitor
{
      public:
	constexpr BatteryMonitor(adc_dt_spec channel, std::int32_t output_ohms,
				 std::int32_t full_ohms) noexcept
		: channel_{channel}, output_ohms_{output_ohms}, full_ohms_{full_ohms}
	{
	}

	/** Configure the ADC channel.  Call once before read(). */
	[[nodiscard]] std::expected<void, Error> init() const;

	/**
	 * Sample the battery.
	 *
	 * Averages kOversample conversions to settle the ESP32's fairly noisy
	 * SAR ADC.
	 */
	[[nodiscard]] std::expected<Reading, Error> read() const;

      private:
	/* Number of ADC conversions averaged into one reading.  A single
	 * conversion wanders by tens of millivolts, which is several percent of
	 * charge on the flat part of the discharge curve.
	 */
	static constexpr int kOversample = 16;

	[[nodiscard]] std::expected<std::int32_t, Error> sample_mv() const;

	adc_dt_spec channel_;
	std::int32_t output_ohms_;
	std::int32_t full_ohms_;
};

} /* namespace zest */
