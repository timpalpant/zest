/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <concepts>
#include <cstdint>
#include <zest/error.hpp>
#include <type_traits>

namespace zest
{

/** Errors produced by numeric transforms. */
enum class TransformError {
	empty_input_range,
};

/** A short, static description of a transform error. */
[[nodiscard]] constexpr const char *to_string(TransformError error) noexcept
{
	switch (error) {
	case TransformError::empty_input_range:
		return "input range is empty";
	}
	return "unknown transform error";
}

/**
 * Apply a gain and offset calibration to arithmetic samples.
 *
 * The arithmetic type defaults to `float`, since `double` becomes a soft-float
 * call on a part with no double-precision FPU. Prefer `IntegerCalibration` when
 * the gain can be expressed as a ratio.
 */
template <typename T, typename Real = float>
	requires std::is_arithmetic_v<T> && std::floating_point<Real>
class Calibration
{
      public:
	using value_type = Real;

	constexpr Calibration(Real gain = Real{1}, Real offset = Real{}) noexcept
		: gain_{gain}, offset_{offset}
	{
	}

	[[nodiscard]] constexpr Real apply(T value) const noexcept
	{
		return static_cast<Real>(value) * gain_ + offset_;
	}

	[[nodiscard]] constexpr Real gain() const noexcept
	{
		return gain_;
	}
	[[nodiscard]] constexpr Real offset() const noexcept
	{
		return offset_;
	}

      private:
	Real gain_;
	Real offset_;
};

/**
 * Apply a gain and offset calibration using only integer arithmetic.
 *
 * The gain is the ratio `numerator / denominator`, evaluated in a wider
 * accumulator so intermediate products do not overflow. Rounding is
 * half-away-from-zero. This form needs no FPU at all, which makes it the right
 * default for a sensor path on a part without one.
 */
template <std::integral T, typename Accumulator = std::int64_t>
	requires std::is_signed_v<Accumulator>
class IntegerCalibration
{
      public:
	constexpr IntegerCalibration(Accumulator numerator = 1, Accumulator denominator = 1,
				     Accumulator offset = 0) noexcept
		: numerator_{numerator}, denominator_{denominator == 0 ? 1 : denominator},
		  offset_{offset}
	{
	}

	[[nodiscard]] constexpr T apply(T value) const noexcept
	{
		const Accumulator scaled = static_cast<Accumulator>(value) * numerator_;
		const Accumulator half = denominator_ / 2;
		const Accumulator rounded = scaled >= 0 ? (scaled + half) / denominator_
							: (scaled - half) / denominator_;
		return static_cast<T>(rounded + offset_);
	}

      private:
	Accumulator numerator_;
	Accumulator denominator_;
	Accumulator offset_;
};

/**
 * Linearly map values between two numeric ranges.
 *
 * As with `Calibration`, the arithmetic type defaults to `float`.
 */
template <typename T, typename Real = float>
	requires std::is_arithmetic_v<T> && std::floating_point<Real>
class LinearMap
{
      public:
	using value_type = Real;

	constexpr LinearMap(T input_min, T input_max, Real output_min, Real output_max) noexcept
		: input_min_{input_min}, input_max_{input_max}, output_min_{output_min},
		  output_max_{output_max}
	{
	}

	/** Map @p value, extrapolating outside the input range. */
	[[nodiscard]] constexpr Result<Real, TransformError> map(T value) const noexcept
	{
		if (input_min_ == input_max_) {
			return std::unexpected(TransformError::empty_input_range);
		}

		const Real position =
			(static_cast<Real>(value) - static_cast<Real>(input_min_)) /
			(static_cast<Real>(input_max_) - static_cast<Real>(input_min_));
		return output_min_ + position * (output_max_ - output_min_);
	}

	/** Map @p value, clamping the result to the output range. */
	[[nodiscard]] constexpr Result<Real, TransformError> map_clamped(T value) const noexcept
	{
		const auto mapped = map(value);
		if (!mapped) {
			return mapped;
		}
		const Real low = output_min_ < output_max_ ? output_min_ : output_max_;
		const Real high = output_min_ < output_max_ ? output_max_ : output_min_;
		return *mapped < low ? low : (*mapped > high ? high : *mapped);
	}

      private:
	T input_min_;
	T input_max_;
	Real output_min_;
	Real output_max_;
};

/**
 * Linearly map between integer ranges without floating point.
 *
 * Returns `TransformError::empty_input_range` when the input range is degenerate.
 * Rounding is half-away-from-zero.
 */
template <std::integral T, typename Accumulator = std::int64_t>
	requires std::is_signed_v<Accumulator>
[[nodiscard]] constexpr Result<T, TransformError> integer_map(T value, T input_min, T input_max,
							      T output_min, T output_max) noexcept
{
	if (input_min == input_max) {
		return std::unexpected(TransformError::empty_input_range);
	}

	const Accumulator span = static_cast<Accumulator>(input_max) - input_min;
	const Accumulator reach = static_cast<Accumulator>(output_max) - output_min;
	const Accumulator offset = static_cast<Accumulator>(value) - input_min;
	const Accumulator scaled = offset * reach;
	const Accumulator half = (span >= 0 ? span : -span) / 2;
	const Accumulator rounded = scaled >= 0 ? (scaled + half) / span : (scaled - half) / span;
	return static_cast<T>(output_min + rounded);
}

} /* namespace zest */
