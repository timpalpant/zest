#pragma once

#include <chrono>
#include <utility>

namespace zest
{

/** Allow one operation per configured interval without sleeping. */
template <typename Clock = std::chrono::steady_clock> class RateLimiter
{
      public:
	using clock = Clock;
	using duration = typename clock::duration;
	using time_point = typename clock::time_point;

	constexpr explicit RateLimiter(duration interval) noexcept
		: interval_{interval < duration::zero() ? duration::zero() : interval}
	{
	}

	/** Return true when an operation is allowed at @p now. The first call succeeds. */
	[[nodiscard]] constexpr bool allow(time_point now) noexcept
	{
		if (!initialized_ || now - last_ >= interval_) {
			last_ = now;
			initialized_ = true;
			return true;
		}
		return false;
	}

	[[nodiscard]] bool allow() noexcept
	{
		return allow(clock::now());
	}

	constexpr void reset() noexcept
	{
		last_ = {};
		initialized_ = false;
	}

      private:
	duration interval_;
	time_point last_{};
	bool initialized_{false};
};

/** Result of observing an input through a Debouncer. */
template <typename T> struct DebounceResult {
	T value;
	bool changed;
};

/** Require an input value to remain unchanged for a duration before accepting it. */
template <typename T, typename Clock = std::chrono::steady_clock> class Debouncer
{
      public:
	using clock = Clock;
	using duration = typename clock::duration;
	using time_point = typename clock::time_point;

	constexpr explicit Debouncer(duration settle_time, T initial = T{}) noexcept
		: settle_time_{settle_time < duration::zero() ? duration::zero() : settle_time},
		  stable_{std::move(initial)}, candidate_{stable_}
	{
	}

	/** Observe a value at a caller-supplied time. */
	[[nodiscard]] constexpr DebounceResult<T> update(const T &value, time_point now) noexcept
	{
		if (value == stable_) {
			candidate_ = stable_;
			candidate_since_ = now;
			candidate_pending_ = false;
			return {stable_, false};
		}

		if (!candidate_pending_ || value != candidate_) {
			candidate_ = value;
			candidate_since_ = now;
			candidate_pending_ = true;
			return {stable_, false};
		}

		if (now - candidate_since_ >= settle_time_) {
			stable_ = candidate_;
			candidate_pending_ = false;
			return {stable_, true};
		}

		return {stable_, false};
	}

	[[nodiscard]] DebounceResult<T> update(const T &value) noexcept
	{
		return update(value, clock::now());
	}

	[[nodiscard]] constexpr const T &value() const noexcept
	{
		return stable_;
	}

	constexpr void reset(T value = T{}) noexcept
	{
		stable_ = std::move(value);
		candidate_ = stable_;
		candidate_since_ = {};
		candidate_pending_ = false;
	}

      private:
	duration settle_time_;
	T stable_;
	T candidate_;
	time_point candidate_since_{};
	bool candidate_pending_{false};
};

} /* namespace zest */
