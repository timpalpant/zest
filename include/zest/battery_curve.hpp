#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace zest
{

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

/**
 * Estimate charge percentage by linearly interpolating a discharge curve.
 *
 * Points must be ordered from highest to lowest voltage. Percentages must be
 * in the range 0..100 and must not increase as voltage falls. Values outside
 * the curve are clamped to its endpoint percentages. The curve is borrowed
 * only for the duration of this call.
 */
[[nodiscard]] constexpr std::expected<std::uint8_t, CurveError>
estimate_charge_percent(std::int32_t millivolts, std::span<const CurvePoint> curve) noexcept
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

} /* namespace zest */
