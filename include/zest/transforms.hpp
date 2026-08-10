#pragma once

#include <concepts>
#include <expected>
#include <type_traits>

namespace zest
{

/** Errors produced by numeric transforms. */
enum class TransformError {
	empty_input_range,
};

/** Apply a gain and offset calibration to arithmetic samples. */
template <typename T, typename Result = double>
	requires std::is_arithmetic_v<T> && std::is_arithmetic_v<Result>
class Calibration
{
      public:
	constexpr Calibration(Result gain = Result{1}, Result offset = Result{}) noexcept
		: gain_{gain}, offset_{offset}
	{
	}

	[[nodiscard]] constexpr Result apply(T value) const noexcept
	{
		return static_cast<Result>(value) * gain_ + offset_;
	}

      private:
	Result gain_;
	Result offset_;
};

/** Linearly map values between two numeric ranges. */
template <typename T, typename Result = double>
	requires std::is_arithmetic_v<T> && std::is_arithmetic_v<Result>
class LinearMap
{
      public:
	constexpr LinearMap(T input_min, T input_max, Result output_min, Result output_max) noexcept
		: input_min_{input_min}, input_max_{input_max}, output_min_{output_min},
		  output_max_{output_max}
	{
	}

	[[nodiscard]] constexpr std::expected<Result, TransformError> map(T value) const noexcept
	{
		if (input_min_ == input_max_) {
			return std::unexpected(TransformError::empty_input_range);
		}

		const Result position =
			(static_cast<Result>(value) - static_cast<Result>(input_min_)) /
			(static_cast<Result>(input_max_) - static_cast<Result>(input_min_));
		return output_min_ + position * (output_max_ - output_min_);
	}

      private:
	T input_min_;
	T input_max_;
	Result output_min_;
	Result output_max_;
};

} /* namespace zest */
