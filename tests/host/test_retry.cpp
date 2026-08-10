/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/retry.hpp>

#include <chrono>

using namespace zest;
using namespace std::chrono_literals;

/* The documented sequence, with the growth factor as an integer percentage. */
constexpr bool retry_exhausts()
{
	RetryPolicy retry{{
		.maximum_attempts = 3,
		.initial_delay = 10ms,
		.maximum_delay = 20ms,
		.multiplier_percent = 200,
	}};
	const auto first = retry.failure();
	const auto second = retry.failure();
	const auto exhausted = retry.failure();
	return first == 10ms && second == 20ms && !exhausted && retry.attempts() == 3U &&
	       retry.exhausted();
}
static_assert(retry_exhausts());

/* A maximum of zero means unlimited, so failure() never reports exhaustion. */
constexpr bool retry_unlimited()
{
	RetryPolicy retry{{.maximum_attempts = 0, .initial_delay = 1ms, .maximum_delay = 4ms}};
	for (int i = 0; i < 50; ++i) {
		if (!retry.failure().has_value()) {
			return false;
		}
	}
	return !retry.exhausted();
}
static_assert(retry_unlimited());

/* Backoff must saturate at the maximum and stay there. */
constexpr bool backoff_saturates()
{
	ExponentialBackoff backoff{{
		.initial_delay = 100ms,
		.maximum_delay = 400ms,
		.multiplier_percent = 200,
	}};
	return backoff.next_delay() == 100ms && backoff.next_delay() == 200ms &&
	       backoff.next_delay() == 400ms && backoff.next_delay() == 400ms &&
	       backoff.next_delay() == 400ms;
}
static_assert(backoff_saturates());

/* A multiplier below 100 would shrink the delay; it is raised to 100. */
constexpr bool backoff_never_shrinks()
{
	ExponentialBackoff backoff{{
		.initial_delay = 50ms,
		.maximum_delay = 100ms,
		.multiplier_percent = 10,
	}};
	return backoff.next_delay() == 50ms && backoff.next_delay() == 50ms;
}
static_assert(backoff_never_shrinks());

/* An inverted configuration is normalized rather than rejected. */
constexpr bool backoff_normalizes()
{
	ExponentialBackoff backoff{{.initial_delay = 500ms, .maximum_delay = 100ms}};
	return backoff.next_delay() == 500ms;
}
static_assert(backoff_normalizes());

constexpr bool backoff_resets()
{
	ExponentialBackoff backoff{{.initial_delay = 10ms, .maximum_delay = 1000ms}};
	(void)backoff.next_delay();
	(void)backoff.next_delay();
	backoff.reset();
	return backoff.next_delay() == 10ms;
}
static_assert(backoff_resets());

/* Jitter stays within its window and is reproducible for a given seed. */
constexpr bool backoff_jitter_bounded()
{
	RetryConfig config{
		.initial_delay = 1000ms,
		.maximum_delay = 1000ms,
		.multiplier_percent = 100,
		.jitter_percent = 50,
		.jitter_seed = 12345,
	};
	ExponentialBackoff first{config};
	ExponentialBackoff second{config};
	for (int i = 0; i < 20; ++i) {
		const auto a = first.next_delay();
		const auto b = second.next_delay();
		if (a != b) {
			return false; /* same seed must replay identically */
		}
		if (a > 1000ms || a < 500ms) {
			return false; /* within [delay - 50%, delay] */
		}
	}
	return true;
}
static_assert(backoff_jitter_bounded());

/* Zero jitter must leave the sequence exactly deterministic. */
constexpr bool backoff_without_jitter_is_exact()
{
	ExponentialBackoff backoff{{
		.initial_delay = 250ms,
		.maximum_delay = 10'000ms,
		.multiplier_percent = 200,
		.jitter_percent = 0,
	}};
	return backoff.next_delay() == 250ms && backoff.next_delay() == 500ms &&
	       backoff.next_delay() == 1000ms;
}
static_assert(backoff_without_jitter_is_exact());

int main()
{
	CHECK(retry_exhausts());
	CHECK(retry_unlimited());
	CHECK(backoff_saturates());
	CHECK(backoff_never_shrinks());
	CHECK(backoff_normalizes());
	CHECK(backoff_resets());
	CHECK(backoff_jitter_bounded());
	CHECK(backoff_without_jitter_is_exact());
	return zest::test::summary("retry");
}
