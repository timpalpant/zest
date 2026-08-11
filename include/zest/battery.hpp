/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

/**
 * @file
 * @brief Battery voltage measurement and state-of-charge estimation.
 *
 * ```text
 * AdcChannel -> VoltageDivider -> BatteryMonitor -> BatteryCurve::percent_at()
 * ```
 *
 * Measurement and charge estimation are separate types: reading the cell is I/O
 * that fails per call, while a curve is validated once and then cannot fail.
 *
 * The curve half needs no Kconfig option and no Zephyr facility. `BatteryMonitor`
 * is declared only under `CONFIG_ZEST_BATTERY_MONITOR=y`.
 */

namespace zest
{

/* ------------------------------------------------------- discharge curve --- */

/** One point on a battery discharge curve. */
struct CurvePoint {
	std::int32_t millivolts;
	std::uint8_t percent;
};

/** Errors found while validating a discharge curve. */
enum class CurveError {
	insufficient_points,
	invalid_voltage_order,
	invalid_percentage,
};

/** A short, static description of a curve error. */
[[nodiscard]] constexpr const char *to_string(CurveError error) noexcept
{
	switch (error) {
	case CurveError::insufficient_points:
		return "a discharge curve needs at least two points";
	case CurveError::invalid_voltage_order:
		return "curve voltages must strictly descend";
	case CurveError::invalid_percentage:
		return "curve percentages must be 0..100 and must not rise as voltage falls";
	}
	return "unknown curve error";
}

/**
 * Check that a discharge curve is well formed.
 *
 * Points must be ordered from highest to lowest voltage, with percentages in
 * 0..100 that never increase as voltage falls.
 */
[[nodiscard]] constexpr std::expected<void, CurveError>
validate_curve(std::span<const CurvePoint> curve) noexcept
{
	if (curve.size() < 2U) {
		return std::unexpected(CurveError::insufficient_points);
	}
	if (curve.front().percent > 100U) {
		return std::unexpected(CurveError::invalid_percentage);
	}

	for (std::size_t i = 1; i < curve.size(); ++i) {
		const CurvePoint &previous = curve[i - 1];
		const CurvePoint &current = curve[i];

		if (previous.millivolts <= current.millivolts) {
			return std::unexpected(CurveError::invalid_voltage_order);
		}
		if (previous.percent < current.percent || current.percent > 100U) {
			return std::unexpected(CurveError::invalid_percentage);
		}
	}
	return {};
}

/**
 * Interpolate a validated curve. The caller guarantees `validate_curve()` passed.
 *
 * Values outside the curve clamp to its endpoint percentages.
 */
[[nodiscard]] constexpr std::uint8_t
interpolate_validated_curve(std::int32_t millivolts, std::span<const CurvePoint> curve) noexcept
{
	if (millivolts >= curve.front().millivolts) {
		return curve.front().percent;
	}

	for (std::size_t i = 1; i < curve.size(); ++i) {
		const CurvePoint &high = curve[i - 1];
		const CurvePoint &low = curve[i];

		if (millivolts >= low.millivolts) {
			const std::int64_t percentage_span = high.percent - low.percent;
			const std::int64_t voltage_span = high.millivolts - low.millivolts;
			const std::int64_t offset = millivolts - low.millivolts;

			return static_cast<std::uint8_t>(low.percent +
							 offset * percentage_span / voltage_span);
		}
	}

	return curve.back().percent;
}

/**
 * A discharge curve whose shape has already been checked.
 *
 * Validation happens once, when the curve is built, so `percent_at()` only
 * interpolates and cannot fail.
 *
 * Build one with `make_battery_curve()` for a runtime curve, or `battery_curve()`
 * for a literal that should fail the build if it is malformed.
 */
template <std::size_t Points> class BatteryCurve
{
      public:
	static_assert(Points >= 2U, "a discharge curve needs at least two points");

	/** Estimate charge, clamped to the curve's endpoints. Cannot fail. */
	[[nodiscard]] constexpr std::uint8_t percent_at(std::int32_t millivolts) const noexcept
	{
		return interpolate_validated_curve(millivolts, points_);
	}

	[[nodiscard]] constexpr std::span<const CurvePoint> points() const noexcept
	{
		return points_;
	}

	/** The voltage at which the curve reports a full battery. */
	[[nodiscard]] constexpr std::int32_t full_millivolts() const noexcept
	{
		return points_.front().millivolts;
	}

	/** The voltage at which the curve reports an empty battery. */
	[[nodiscard]] constexpr std::int32_t empty_millivolts() const noexcept
	{
		return points_.back().millivolts;
	}

      private:
	template <std::size_t N>
	friend constexpr std::expected<BatteryCurve<N>, CurveError>
	make_battery_curve(const std::array<CurvePoint, N> &) noexcept;

	constexpr explicit BatteryCurve(const std::array<CurvePoint, Points> &points) noexcept
		: points_{points}
	{
	}

	std::array<CurvePoint, Points> points_;
};

/** Validate @p points once and wrap them, or report why they are malformed. */
template <std::size_t N>
[[nodiscard]] constexpr std::expected<BatteryCurve<N>, CurveError>
make_battery_curve(const std::array<CurvePoint, N> &points) noexcept
{
	if (const auto valid = validate_curve(points); !valid) {
		return std::unexpected(valid.error());
	}
	return BatteryCurve<N>{points};
}

namespace detail
{
/* Declared, never defined: calling it in a constant expression fails the build. */
const char *zest_discharge_curve_is_malformed();
} /* namespace detail */

/**
 * Build a curve from a literal, failing the build if it is malformed.
 *
 * ```cpp
 * constexpr auto curve = zest::battery_curve(std::array{
 *         zest::CurvePoint{4200, 100},
 *         zest::CurvePoint{3700, 10},
 *         zest::CurvePoint{3300, 0},
 * });
 * ```
 *
 * A curve that is out of order or out of range stops compilation rather than
 * becoming a runtime error nobody handles.
 */
template <std::size_t N>
[[nodiscard]] consteval BatteryCurve<N> battery_curve(const std::array<CurvePoint, N> &points)
{
	auto built = make_battery_curve(points);
	if (!built) {
		/* Reaching a non-constexpr function here is the diagnostic. */
		detail::zest_discharge_curve_is_malformed();
	}
	return *built;
}

/**
 * Estimate charge percentage by linearly interpolating a discharge curve.
 *
 * This convenience overload validates @p curve on every call. For a curve that
 * is a constant of the design --- which is the normal case --- prefer
 * `battery_curve()` or `make_battery_curve()` and call `percent_at()`, which
 * validates once and cannot fail.
 */
[[nodiscard]] constexpr std::expected<std::uint8_t, CurveError>
estimate_charge_percent(std::int32_t millivolts, std::span<const CurvePoint> curve) noexcept
{
	if (const auto valid = validate_curve(curve); !valid) {
		return std::unexpected(valid.error());
	}
	return interpolate_validated_curve(millivolts, curve);
}

} /* namespace zest */

/* ----------------------------------------------------------- measurement --- */

/*
 * ZEST_BATTERY_MONITOR depends on ADC and selects ZEST_ADC_CHANNEL, so this guard
 * is also what makes the includes below safe. Everything above is Zephyr-free and
 * stays available without it.
 */
#if defined(CONFIG_ZEST_BATTERY_MONITOR)

#include <zest/error.hpp>
#include <zest/units.hpp>
#include <zest/voltage_divider.hpp>

namespace zest
{

/**
 * Reads battery voltage through a resistive divider.
 *
 * The divider ratio is given as the two resistances from the devicetree
 * `voltage-divider` node. Pair a reading with a `BatteryCurve` above to turn it
 * into a charge estimate.
 *
 * Requires `CONFIG_ZEST_BATTERY_MONITOR=y`.
 */
class BatteryMonitor
{
      public:
	/** Number of ADC conversions averaged into one reading by default. */
	static constexpr std::size_t kDefaultOversample = 16U;

	constexpr BatteryMonitor(adc_dt_spec channel, Ohms measured, Ohms full,
				 std::size_t oversample = kDefaultOversample) noexcept
		: divider_{channel, measured, full}, oversample_{oversample == 0U ? 1U : oversample}
	{
	}

	constexpr BatteryMonitor(adc_dt_spec channel, std::int32_t output_ohms,
				 std::int32_t full_ohms,
				 std::size_t oversample = kDefaultOversample) noexcept
		: divider_{channel, output_ohms, full_ohms},
		  oversample_{oversample == 0U ? 1U : oversample}
	{
	}

	/** Configure the ADC channel. Call once before read_millivolts(). */
	Result<> init() const noexcept
	{
		return divider_.init();
	}

	/** Sample the battery, averaging the configured number of conversions. */
	Result<Millivolts> read_millivolts() const noexcept
	{
		return divider_.read_average_millivolts(oversample_);
	}

	[[nodiscard]] constexpr const VoltageDivider &divider() const noexcept
	{
		return divider_;
	}
	[[nodiscard]] constexpr std::size_t oversample() const noexcept
	{
		return oversample_;
	}

      private:
	VoltageDivider divider_;
	std::size_t oversample_;
};

} /* namespace zest */

#endif /* CONFIG_ZEST_BATTERY_MONITOR */
