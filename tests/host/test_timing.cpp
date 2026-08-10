/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"


#include <zest/timing.hpp>

#include <chrono>

using namespace zest;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr bool rate_limiter_works()
{
	RateLimiter<Clock> limiter{10ms};
	const Clock::time_point start{};
	if (!limiter.allow(start)) {
		return false; /* the first call always succeeds */
	}
	if (limiter.allow(start + 5ms)) {
		return false;
	}
	if (!limiter.allow(start + 10ms)) {
		return false;
	}
	limiter.reset();
	return limiter.allow(start + 11ms);
}
static_assert(rate_limiter_works());

/* A zero interval must allow every call rather than none. */
constexpr bool rate_limiter_zero_interval()
{
	RateLimiter<Clock> limiter{0ms};
	const Clock::time_point start{};
	return limiter.allow(start) && limiter.allow(start) && limiter.allow(start);
}
static_assert(rate_limiter_zero_interval());

constexpr bool debouncer_settles()
{
	Debouncer<bool, Clock> debouncer{10ms};
	const Clock::time_point start{};
	if (debouncer.update(true, start).changed) {
		return false;
	}
	if (debouncer.update(true, start + 5ms).changed) {
		return false;
	}
	const auto settled = debouncer.update(true, start + 10ms);
	return settled.changed && settled.value && debouncer.value();
}
static_assert(debouncer_settles());

/* An input that flaps before settling must restart the timer, not accumulate. */
constexpr bool debouncer_rejects_flapping()
{
	Debouncer<bool, Clock> debouncer{10ms};
	const Clock::time_point start{};
	if (debouncer.update(true, start).changed) {
		return false;
	}
	/* Back to the stable value: the candidate is abandoned. */
	if (debouncer.update(false, start + 5ms).changed) {
		return false;
	}
	/* A fresh candidate must wait the full settle time from here. */
	if (debouncer.update(true, start + 8ms).changed) {
		return false;
	}
	if (debouncer.update(true, start + 15ms).changed) {
		return false; /* only 7ms of stability so far */
	}
	return debouncer.update(true, start + 18ms).changed;
}
static_assert(debouncer_rejects_flapping());

constexpr bool debouncer_resets()
{
	Debouncer<bool, Clock> debouncer{10ms};
	debouncer.reset(true);
	return debouncer.value();
}
static_assert(debouncer_resets());

int main()
{
	CHECK(rate_limiter_works());
	CHECK(rate_limiter_zero_interval());
	CHECK(debouncer_settles());
	CHECK(debouncer_rejects_flapping());
	CHECK(debouncer_resets());
	return zest::test::summary("timing");
}
