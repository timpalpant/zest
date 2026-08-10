/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

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

/**
 * Fixed-storage arithmetic moving average.
 *
 * The accumulator defaults to `std::int64_t` for integral samples, which cannot
 * overflow for any window a small device can hold. Narrow it to `std::int32_t`
 * when the sample range makes that safe and 64-bit arithmetic is too expensive.
 *
 * For floating-point samples the running sum accumulates rounding error over
 * long runs; call `reset()` periodically, or prefer `MedianFilter` when exactness
 * matters more than smoothness.
 */
template <typename T, std::size_t Window, typename Accumulator = detail::default_accumulator_t<T>>
	requires std::is_arithmetic_v<T> && std::is_arithmetic_v<Accumulator>
class MovingAverage
{
      public:
	static_assert(Window > 0U, "moving-average window must not be empty");

	/** Add a sample without reading the average back. */
	constexpr void push(T sample) noexcept
	{
		if (count_ == Window) {
			total_ -= static_cast<Accumulator>(samples_[next_]);
		} else {
			++count_;
		}

		samples_[next_] = sample;
		next_ = (next_ + 1U) % Window;
		total_ += static_cast<Accumulator>(sample);
	}

	/** Add a sample and return the current average. */
	[[nodiscard]] constexpr T update(T sample) noexcept
	{
		push(sample);
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

/**
 * Fixed-storage median filter with a constant-time read.
 *
 * A sorted view of the window is maintained incrementally, so `update()` is
 * linear in the window size and copies nothing, and `value()` is constant time.
 * For an even sample count the lower median is returned.
 */
template <std::totally_ordered T, std::size_t Window> class MedianFilter
{
      public:
	static_assert(Window > 0U, "median-filter window must not be empty");

	/** Add a sample without reading the median back. */
	constexpr void push(T sample) noexcept
	{
		if (count_ == Window) {
			/* Drop the oldest sample from the sorted view. */
			const auto ordered_end =
				sorted_.begin() + static_cast<std::ptrdiff_t>(count_);
			const auto stale =
				std::lower_bound(sorted_.begin(), ordered_end, samples_[next_]);
			std::move(stale + 1, ordered_end, stale);
			--count_;
		}

		samples_[next_] = sample;
		next_ = (next_ + 1U) % Window;

		/* Insert the new sample, keeping the view ascending. */
		const auto tail = sorted_.begin() + static_cast<std::ptrdiff_t>(count_);
		const auto slot = std::upper_bound(sorted_.begin(), tail, sample);
		std::move_backward(slot, tail, tail + 1);
		*slot = sample;
		++count_;
	}

	/** Add a sample and return the current median. */
	[[nodiscard]] constexpr T update(T sample) noexcept
	{
		push(sample);
		return value();
	}

	/** Return the current median, or a value-initialized T before the first sample. */
	[[nodiscard]] constexpr T value() const noexcept
	{
		return count_ == 0U ? T{} : sorted_[(count_ - 1U) / 2U];
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
		sorted_ = {};
		next_ = 0U;
		count_ = 0U;
	}

      private:
	std::array<T, Window> samples_{};
	std::array<T, Window> sorted_{};
	std::size_t next_{0U};
	std::size_t count_{0U};
};

/**
 * Exponential moving average with a runtime smoothing factor.
 *
 * The arithmetic type defaults to `float`. No common Cortex-M part has a
 * double-precision FPU, so a `double` filter compiles to soft-float calls in the
 * per-sample path; pass `double` explicitly only where that cost is acceptable.
 * When the smoothing factor can be a power of two, `ShiftMovingAverage` avoids
 * floating point entirely.
 */
template <typename T, typename Real = float>
	requires std::is_arithmetic_v<T> && std::floating_point<Real>
class ExponentialMovingAverage
{
      public:
	using value_type = Real;

	/** Construct with alpha clamped to the inclusive range 0..1. */
	constexpr explicit ExponentialMovingAverage(Real alpha) noexcept
		: alpha_{std::clamp(alpha, Real{0}, Real{1})}
	{
	}

	/** Add a sample and return the filtered value. */
	[[nodiscard]] constexpr Real update(T sample) noexcept
	{
		const Real input = static_cast<Real>(sample);
		if (!initialized_) {
			value_ = input;
			initialized_ = true;
		} else {
			value_ += alpha_ * (input - value_);
		}
		return value_;
	}

	[[nodiscard]] constexpr Real value() const noexcept
	{
		return value_;
	}
	[[nodiscard]] constexpr Real alpha() const noexcept
	{
		return alpha_;
	}
	[[nodiscard]] constexpr bool initialized() const noexcept
	{
		return initialized_;
	}

	constexpr void reset() noexcept
	{
		value_ = Real{0};
		initialized_ = false;
	}

      private:
	Real alpha_;
	Real value_{0};
	bool initialized_{false};
};

/**
 * Integer exponential moving average with a power-of-two smoothing factor.
 *
 * Equivalent to an exponential moving average with `alpha = 1 / 2^Shift`, using
 * only shifts and adds. This is the form to reach for on a part without an FPU:
 * it needs no floating point, is exactly reproducible, and carries `Shift` extra
 * bits of fractional state internally.
 */
template <std::integral T, unsigned Shift, typename Accumulator = std::int64_t>
	requires std::is_signed_v<Accumulator>
class ShiftMovingAverage
{
      public:
	static_assert(Shift > 0U, "a shift of zero would pass samples through unfiltered");
	static_assert(Shift < 24U, "shift is too large for the accumulator's fractional headroom");

	/** Add a sample and return the filtered value. */
	[[nodiscard]] constexpr T update(T sample) noexcept
	{
		if (!initialized_) {
			accumulator_ = static_cast<Accumulator>(sample) << Shift;
			initialized_ = true;
		} else {
			accumulator_ += static_cast<Accumulator>(sample) - (accumulator_ >> Shift);
		}
		return value();
	}

	[[nodiscard]] constexpr T value() const noexcept
	{
		return static_cast<T>(accumulator_ >> Shift);
	}
	[[nodiscard]] constexpr bool initialized() const noexcept
	{
		return initialized_;
	}

	constexpr void reset() noexcept
	{
		accumulator_ = 0;
		initialized_ = false;
	}

      private:
	Accumulator accumulator_{0};
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
	/**
	 * Construct with the two boundaries. They are ordered internally, so
	 * passing them the wrong way round is not an error.
	 */
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
	[[nodiscard]] constexpr T low() const noexcept
	{
		return low_;
	}
	[[nodiscard]] constexpr T high() const noexcept
	{
		return high_;
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

	[[nodiscard]] constexpr T threshold() const noexcept
	{
		return threshold_;
	}

      private:
	T threshold_;
	ThresholdDirection direction_;
};

} /* namespace zest */
