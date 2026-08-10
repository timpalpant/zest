/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace zest
{

/**
 * A PID controller with anti-windup and no setpoint kick.
 *
 * Two details separate a controller that behaves on real hardware from the
 * textbook difference equation, and both are handled here:
 *
 * - **Anti-windup.** When the output is saturated, integrating further only
 *   builds a term the actuator cannot express, so recovery overshoots badly.
 *   The integral is only advanced when doing so does not push deeper into
 *   saturation (conditional integration), and is clamped besides.
 * - **Derivative on measurement.** Differentiating the error makes a step change
 *   in setpoint produce an unbounded derivative spike. This differentiates the
 *   measurement instead, so changing the setpoint is smooth.
 *
 * The arithmetic type defaults to `float`, since no common Cortex-M part has a
 * double-precision FPU.
 */
template <typename Real = float>
	requires std::floating_point<Real>
class PidController
{
      public:
	struct Gains {
		Real proportional{};
		/** Integral gain, per second. */
		Real integral{};
		/** Derivative gain, in seconds. */
		Real derivative{};
	};

	struct Limits {
		Real output_min{-std::numeric_limits<Real>::max()};
		Real output_max{std::numeric_limits<Real>::max()};
		/** Bound on the accumulated integral term, in output units. */
		Real integral_limit{std::numeric_limits<Real>::max()};
	};

	constexpr PidController(Gains gains, Limits limits = {}) noexcept
		: gains_{gains}, limits_{order(limits)}
	{
	}

	/**
	 * Advance the controller and return the actuator command.
	 *
	 * @p elapsed is the time since the previous call. A non-positive interval
	 * skips the integral and derivative terms rather than dividing by zero, so
	 * a duplicated call is harmless.
	 */
	template <typename Rep, typename Period>
	[[nodiscard]] constexpr Real update(Real setpoint, Real measurement,
					    std::chrono::duration<Rep, Period> elapsed) noexcept
	{
		const Real seconds =
			std::chrono::duration_cast<std::chrono::duration<Real>>(elapsed).count();
		const Real error = setpoint - measurement;

		Real derivative = Real{0};
		if (initialized_ && seconds > Real{0}) {
			derivative = (measurement - previous_measurement_) / seconds;
		}

		Real candidate = integral_;
		if (seconds > Real{0}) {
			candidate = std::clamp(integral_ + gains_.integral * error * seconds,
					       -limits_.integral_limit, limits_.integral_limit);
		}

		const Real unclamped =
			gains_.proportional * error + candidate - gains_.derivative * derivative;

		if (unclamped > limits_.output_max) {
			/* Saturated high: only integrate if the error pulls back down. */
			if (error < Real{0}) {
				integral_ = candidate;
			}
			output_ = limits_.output_max;
			saturated_ = true;
		} else if (unclamped < limits_.output_min) {
			if (error > Real{0}) {
				integral_ = candidate;
			}
			output_ = limits_.output_min;
			saturated_ = true;
		} else {
			integral_ = candidate;
			output_ = unclamped;
			saturated_ = false;
		}

		previous_measurement_ = measurement;
		previous_error_ = error;
		initialized_ = true;
		return output_;
	}

	/** The most recent command. */
	[[nodiscard]] constexpr Real value() const noexcept
	{
		return output_;
	}
	/** The accumulated integral term, in output units. */
	[[nodiscard]] constexpr Real integral() const noexcept
	{
		return integral_;
	}
	/** The most recent error. */
	[[nodiscard]] constexpr Real error() const noexcept
	{
		return previous_error_;
	}
	/** Whether the last command hit an output limit. */
	[[nodiscard]] constexpr bool saturated() const noexcept
	{
		return saturated_;
	}

	constexpr void set_gains(Gains gains) noexcept
	{
		gains_ = gains;
	}
	constexpr void set_limits(Limits limits) noexcept
	{
		limits_ = order(limits);
	}

	/**
	 * Clear all accumulated state.
	 *
	 * Call this whenever the loop has been open --- after a fault, a manual
	 * override, or a long pause --- so a stale integral does not slam the
	 * actuator when control resumes.
	 */
	constexpr void reset() noexcept
	{
		integral_ = Real{0};
		previous_measurement_ = Real{0};
		previous_error_ = Real{0};
		output_ = Real{0};
		initialized_ = false;
		saturated_ = false;
	}

      private:
	[[nodiscard]] static constexpr Limits order(Limits limits) noexcept
	{
		if (limits.output_min > limits.output_max) {
			std::swap(limits.output_min, limits.output_max);
		}
		if (limits.integral_limit < Real{0}) {
			limits.integral_limit = -limits.integral_limit;
		}
		return limits;
	}

	Gains gains_;
	Limits limits_;
	Real integral_{0};
	Real previous_measurement_{0};
	Real previous_error_{0};
	Real output_{0};
	bool initialized_{false};
	bool saturated_{false};
};

/**
 * Limit how fast a value may change, so an actuator is never commanded to step.
 *
 * Ramping a setpoint protects mechanisms from inrush and shock, and keeps a
 * supply from browning out when a load switches on.
 */
template <typename Real = float>
	requires std::floating_point<Real>
class SlewRateLimiter
{
      public:
	/** @p max_rate_per_second is the largest permitted change per second. */
	constexpr explicit SlewRateLimiter(Real max_rate_per_second, Real initial = Real{}) noexcept
		: rate_{max_rate_per_second < Real{0} ? -max_rate_per_second : max_rate_per_second},
		  value_{initial}
	{
	}

	/** Move toward @p target by at most the configured rate, and return the result. */
	template <typename Rep, typename Period>
	[[nodiscard]] constexpr Real update(Real target,
					    std::chrono::duration<Rep, Period> elapsed) noexcept
	{
		const Real seconds =
			std::chrono::duration_cast<std::chrono::duration<Real>>(elapsed).count();
		if (seconds <= Real{0}) {
			return value_;
		}
		const Real step = rate_ * seconds;
		value_ = std::clamp(target, value_ - step, value_ + step);
		return value_;
	}

	[[nodiscard]] constexpr Real value() const noexcept
	{
		return value_;
	}
	/** Whether the limiter has caught up with its target. */
	[[nodiscard]] constexpr bool settled(Real target, Real tolerance) const noexcept
	{
		const Real difference = value_ > target ? value_ - target : target - value_;
		return difference <= tolerance;
	}
	constexpr void reset(Real value = Real{}) noexcept
	{
		value_ = value;
	}

      private:
	Real rate_;
	Real value_;
};

/**
 * Step an integer value toward a target by at most @p max_delta.
 *
 * The integer counterpart to `SlewRateLimiter`, for ramping a PWM duty in
 * per-mille or a DAC code without touching floating point.
 */
template <std::integral T>
[[nodiscard]] constexpr T slew_toward(T current, T target, T max_delta) noexcept
{
	const T limit = max_delta < T{0} ? static_cast<T>(-max_delta) : max_delta;
	if (target > current) {
		const T room = static_cast<T>(target - current);
		return room <= limit ? target : static_cast<T>(current + limit);
	}
	const T room = static_cast<T>(current - target);
	return room <= limit ? target : static_cast<T>(current - limit);
}

/**
 * A table-driven finite state machine.
 *
 * The transition table is a plain array, so it can be `constexpr` and live in
 * read-only memory. Dispatching an event that no transition covers leaves the
 * state untouched and reports that nothing happened, which keeps "unexpected
 * event" from silently becoming "wrong state".
 *
 * ```cpp
 * enum class Link { down, joining, up };
 * enum class Signal { start, joined, lost };
 *
 * constexpr std::array table{
 *         zest::Transition{Link::down,    Signal::start,  Link::joining},
 *         zest::Transition{Link::joining, Signal::joined, Link::up},
 *         zest::Transition{Link::up,      Signal::lost,   Link::down},
 * };
 * zest::StateMachine machine{Link::down, table};
 * ```
 */
template <typename State, typename Event> struct Transition {
	State from;
	Event on;
	State to;
};

template <typename State, typename Event, std::size_t Count> class StateMachine
{
      public:
	using transition_type = Transition<State, Event>;

	static_assert(Count > 0U, "a state machine needs at least one transition");

	constexpr StateMachine(State initial,
			       const std::array<transition_type, Count> &table) noexcept
		: table_{table}, state_{initial}
	{
	}

	/**
	 * Apply @p event.
	 *
	 * Returns the new state when a transition matched, or no value when the
	 * event is not valid in the current state.
	 */
	[[nodiscard]] constexpr std::optional<State> dispatch(Event event) noexcept
	{
		for (const auto &entry : table_) {
			if (entry.from == state_ && entry.on == event) {
				state_ = entry.to;
				return state_;
			}
		}
		return std::nullopt;
	}

	/** Whether @p event would cause a transition right now. */
	[[nodiscard]] constexpr bool accepts(Event event) const noexcept
	{
		for (const auto &entry : table_) {
			if (entry.from == state_ && entry.on == event) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] constexpr State state() const noexcept
	{
		return state_;
	}

	/** Override the state, bypassing the table. For recovery paths only. */
	constexpr void force(State state) noexcept
	{
		state_ = state;
	}

      private:
	std::array<transition_type, Count> table_;
	State state_;
};

} /* namespace zest */
