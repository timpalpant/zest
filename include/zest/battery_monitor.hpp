/*
 * Battery state-of-charge for the Adafruit HUZZAH32 / Feather ESP32.
 */

#pragma once

#include <zephyr/drivers/adc.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

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

/**
 * Reads state of charge from a battery sitting behind a resistive divider.
 *
 * The divider ratio is given as the two resistances from the devicetree
 * `voltage-divider` node. The caller supplies a full-to-empty discharge curve
 * appropriate for its battery chemistry and expected load. The curve storage
 * must outlive the monitor.
 */
class BatteryMonitor
{
      public:
	constexpr BatteryMonitor(adc_dt_spec channel, std::int32_t output_ohms,
				 std::int32_t full_ohms,
				 std::span<const CurvePoint> discharge_curve) noexcept
		: channel_{channel}, output_ohms_{output_ohms}, full_ohms_{full_ohms},
		  discharge_curve_{discharge_curve}
	{
	}

	/** Configure the ADC channel.  Call once before read(). */
	[[nodiscard]] std::expected<void, Error> init() const noexcept;

	/**
	 * Sample the battery.
	 *
	 * Averages kOversample conversions to settle the ESP32's fairly noisy
	 * SAR ADC.
	 */
	[[nodiscard]] std::expected<Reading, Error> read() const noexcept;

      private:
	/* Number of ADC conversions averaged into one reading.  A single
	 * conversion wanders by tens of millivolts, which is several percent of
	 * charge on the flat part of the discharge curve.
	 */
	static constexpr int kOversample = 16;

	[[nodiscard]] std::expected<std::int32_t, Error> sample_mv() const noexcept;

	adc_dt_spec channel_;
	std::int32_t output_ohms_;
	std::int32_t full_ohms_;
	std::span<const CurvePoint> discharge_curve_;
};

} /* namespace zest */
