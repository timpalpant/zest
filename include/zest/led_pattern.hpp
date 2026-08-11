/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/gpio.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>

namespace zest
{

/** One logical state and duration in an LED pattern. */
struct LedPatternStep {
	GpioState state;
	std::chrono::milliseconds duration;
};

/**
 * Fixed-storage, poll-driven GPIO LED pattern player.
 *
 * `start()` copies the supplied pattern, so the caller's storage need not
 * outlive the call. `update()` should be called from an existing event loop or
 * periodic work item; the player does not create a thread.
 */
template <std::size_t MaxSteps = 16U, typename Clock = std::chrono::steady_clock>
class LedPatternPlayer
{
      public:
	using clock = Clock;
	using time_point = typename clock::time_point;

	static_assert(MaxSteps > 0U, "an LED pattern player needs storage for a step");

	constexpr explicit LedPatternPlayer(gpio_dt_spec output) noexcept : output_{output}
	{
	}

	Result<> init() noexcept
	{
		return output_.init(GpioState::inactive);
	}

	Result<> start(std::span<const LedPatternStep> pattern, bool repeat = true,
				     time_point now = clock::now()) noexcept
	{
		if (pattern.empty()) {
			return fail(errors::invalid_argument);
		}
		if (pattern.size() > MaxSteps) {
			return fail(errors::too_big);
		}
		for (const auto &step : pattern) {
			if (step.duration <= std::chrono::milliseconds::zero()) {
				return fail(errors::invalid_argument);
			}
		}

		std::copy(pattern.begin(), pattern.end(), steps_.begin());
		count_ = pattern.size();
		index_ = 0U;
		repeat_ = repeat;
		deadline_ = now + steps_[0].duration;

		if (const auto result = output_.set(steps_[0].state); !result) {
			count_ = 0U;
			return result;
		}
		return {};
	}

	/**
	 * Advance the pattern to @p now. Safe to call more often than necessary.
	 *
	 * A long gap since the previous call is skipped over rather than replayed
	 * step by step, so a delayed event loop does not produce a burst of writes.
	 */
	Result<> update(time_point now = clock::now()) noexcept
	{
		if (count_ == 0U || now < deadline_) {
			return {};
		}

		std::size_t guard = 0U;
		const std::size_t limit = count_ + 1U;
		while (count_ != 0U && now >= deadline_) {
			++index_;
			if (index_ == count_) {
				if (!repeat_) {
					count_ = 0U;
					return output_.set(GpioState::inactive);
				}
				index_ = 0U;
			}
			deadline_ += steps_[index_].duration;

			/*
			 * After a full cycle of catch-up, jump the deadline forward
			 * instead of writing the pin once per skipped step.
			 */
			if (++guard > limit) {
				while (now >= deadline_) {
					deadline_ += steps_[index_].duration;
				}
				break;
			}
		}

		if (count_ == 0U) {
			return {};
		}
		if (const auto result = output_.set(steps_[index_].state); !result) {
			count_ = 0U;
			return result;
		}
		return {};
	}

	Result<> stop() noexcept
	{
		count_ = 0U;
		index_ = 0U;
		return output_.set(GpioState::inactive);
	}

	[[nodiscard]] constexpr bool playing() const noexcept
	{
		return count_ != 0U;
	}

      private:
	GpioOutput output_;
	std::array<LedPatternStep, MaxSteps> steps_{};
	std::size_t count_{0U};
	std::size_t index_{0U};
	time_point deadline_{};
	bool repeat_{false};
};

namespace patterns
{
using namespace std::chrono_literals;

inline constexpr std::array connecting{
	LedPatternStep{GpioState::active, 150ms},
	LedPatternStep{GpioState::inactive, 850ms},
};
inline constexpr std::array connected{
	LedPatternStep{GpioState::active, 1000ms},
};
inline constexpr std::array warning{
	LedPatternStep{GpioState::active, 250ms},
	LedPatternStep{GpioState::inactive, 250ms},
};
inline constexpr std::array failure{
	LedPatternStep{GpioState::active, 100ms},
	LedPatternStep{GpioState::inactive, 100ms},
	LedPatternStep{GpioState::active, 100ms},
	LedPatternStep{GpioState::inactive, 700ms},
};

} /* namespace patterns */

} /* namespace zest */
