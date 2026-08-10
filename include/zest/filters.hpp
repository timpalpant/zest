#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zest
{

namespace detail
{

template <typename T>
using default_accumulator_t =
	std::conditional_t<std::floating_point<T>, T, std::common_type_t<T, std::int64_t>>;

} /* namespace detail */

/** Fixed-storage arithmetic moving average. */
template <typename T, std::size_t Window, typename Accumulator = detail::default_accumulator_t<T>>
	requires std::is_arithmetic_v<T> && std::is_arithmetic_v<Accumulator>
class MovingAverage
{
      public:
	static_assert(Window > 0U, "moving-average window must not be empty");

	/** Add a sample and return the current average. */
	[[nodiscard]] constexpr T update(T sample) noexcept
	{
		if (count_ == Window) {
			total_ -= static_cast<Accumulator>(samples_[next_]);
		} else {
			++count_;
		}

		samples_[next_] = sample;
		next_ = (next_ + 1U) % Window;
		total_ += static_cast<Accumulator>(sample);
		return value();
	}

	/** Return the current average, or a value-initialized T before the first sample. */
	[[nodiscard]] constexpr T value() const noexcept
	{
		return count_ == 0U ? T{}
				    : static_cast<T>(total_ / static_cast<Accumulator>(count_));
	}

	/** Number of samples currently represented by the average. */
	[[nodiscard]] constexpr std::size_t size() const noexcept
	{
		return count_;
	}

	/** Whether the configured window is full. */
	[[nodiscard]] constexpr bool full() const noexcept
	{
		return count_ == Window;
	}

	/** Discard all samples. */
	constexpr void reset() noexcept
	{
		samples_ = {};
		total_ = {};
		next_ = 0U;
		count_ = 0U;
	}

      private:
	std::array<T, Window> samples_{};
	Accumulator total_{};
	std::size_t next_{0U};
	std::size_t count_{0U};
};

/** Fixed-storage median filter. For an even sample count, returns the lower median. */
template <std::totally_ordered T, std::size_t Window> class MedianFilter
{
      public:
	static_assert(Window > 0U, "median-filter window must not be empty");

	/** Add a sample and return the current median. */
	[[nodiscard]] constexpr T update(T sample) noexcept
	{
		samples_[next_] = sample;
		next_ = (next_ + 1U) % Window;
		if (count_ < Window) {
			++count_;
		}
		return value();
	}

	/** Return the current median, or a value-initialized T before the first sample. */
	[[nodiscard]] constexpr T value() const noexcept
	{
		if (count_ == 0U) {
			return T{};
		}

		auto ordered = samples_;
		std::sort(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(count_));
		return ordered[(count_ - 1U) / 2U];
	}

	[[nodiscard]] constexpr std::size_t size() const noexcept
	{
		return count_;
	}
	[[nodiscard]] constexpr bool full() const noexcept
	{
		return count_ == Window;
	}

	constexpr void reset() noexcept
	{
		samples_ = {};
		next_ = 0U;
		count_ = 0U;
	}

      private:
	std::array<T, Window> samples_{};
	std::size_t next_{0U};
	std::size_t count_{0U};
};

/** Exponential moving average with a runtime smoothing factor. */
template <typename T>
	requires std::is_arithmetic_v<T>
class ExponentialMovingAverage
{
      public:
	/** Construct with alpha clamped to the inclusive range 0..1. */
	constexpr explicit ExponentialMovingAverage(double alpha) noexcept
		: alpha_{std::clamp(alpha, 0.0, 1.0)}
	{
	}

	/** Add a sample and return the filtered value. */
	[[nodiscard]] constexpr double update(T sample) noexcept
	{
		const double input = static_cast<double>(sample);
		if (!initialized_) {
			value_ = input;
			initialized_ = true;
		} else {
			value_ += alpha_ * (input - value_);
		}
		return value_;
	}

	[[nodiscard]] constexpr double value() const noexcept
	{
		return value_;
	}
	[[nodiscard]] constexpr bool initialized() const noexcept
	{
		return initialized_;
	}

	constexpr void reset() noexcept
	{
		value_ = 0.0;
		initialized_ = false;
	}

      private:
	double alpha_;
	double value_{0.0};
	bool initialized_{false};
};

/** Direction in which a threshold or hysteresis detector becomes active. */
enum class ThresholdDirection {
	below,
	above,
};

/** A two-threshold detector that prevents chatter around a boundary. */
template <std::totally_ordered T> class Hysteresis
{
      public:
	constexpr Hysteresis(T low, T high,
			     ThresholdDirection direction = ThresholdDirection::below) noexcept
		: low_{std::min(low, high)}, high_{std::max(low, high)}, direction_{direction}
	{
	}

	/** Update and return the latched active state. */
	[[nodiscard]] constexpr bool update(const T &value) noexcept
	{
		if (direction_ == ThresholdDirection::below) {
			if (!active_ && value <= low_) {
				active_ = true;
			} else if (active_ && value >= high_) {
				active_ = false;
			}
		} else {
			if (!active_ && value >= high_) {
				active_ = true;
			} else if (active_ && value <= low_) {
				active_ = false;
			}
		}
		return active_;
	}

	[[nodiscard]] constexpr bool active() const noexcept
	{
		return active_;
	}
	constexpr void reset(bool active = false) noexcept
	{
		active_ = active;
	}

      private:
	T low_;
	T high_;
	ThresholdDirection direction_;
	bool active_{false};
};

/** A stateless one-threshold detector. */
template <std::totally_ordered T> class ThresholdDetector
{
      public:
	constexpr ThresholdDetector(
		T threshold, ThresholdDirection direction = ThresholdDirection::above) noexcept
		: threshold_{threshold}, direction_{direction}
	{
	}

	[[nodiscard]] constexpr bool active(const T &value) const noexcept
	{
		return direction_ == ThresholdDirection::above ? value >= threshold_
							       : value <= threshold_;
	}

      private:
	T threshold_;
	ThresholdDirection direction_;
};

} /* namespace zest */
