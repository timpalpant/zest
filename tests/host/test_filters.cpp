/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/filters.hpp>

#include <cstdint>

using namespace zest;

/* MovingAverage tracks the mean of the samples seen so far, then of the window. */
constexpr bool moving_average_works()
{
	MovingAverage<int, 3> average;
	if (average.update(3) != 3 || average.update(6) != 4 || average.update(9) != 6) {
		return false;
	}
	if (!average.full() || average.size() != 3U) {
		return false;
	}
	/* 12 evicts 3, leaving (6+9+12)/3. */
	if (average.update(12) != 9) {
		return false;
	}
	average.reset();
	return average.size() == 0U && average.value() == 0;
}
static_assert(moving_average_works());

/* push() must behave exactly as update() without the read-back. */
constexpr bool moving_average_push_matches_update()
{
	MovingAverage<int, 4> pushed;
	MovingAverage<int, 4> updated;
	for (int sample : {5, 10, 15, 20, 25}) {
		pushed.push(sample);
		(void)updated.update(sample);
		if (pushed.value() != updated.value()) {
			return false;
		}
	}
	return true;
}
static_assert(moving_average_push_matches_update());

/* An odd window returns the true median. */
constexpr bool median_odd_window()
{
	MedianFilter<int, 3> median;
	if (median.update(9) != 9 || median.update(1) != 1 || median.update(5) != 5) {
		return false;
	}
	/* Evicting the oldest (9) leaves {1,5,2}; the median is 2. */
	return median.update(2) == 2;
}
static_assert(median_odd_window());

/* An even window returns the lower median, as documented. */
constexpr bool median_even_window()
{
	MedianFilter<int, 4> median;
	(void)median.update(1);
	(void)median.update(2);
	(void)median.update(3);
	return median.update(4) == 2 && median.full();
}
static_assert(median_even_window());

/* Duplicate samples must not corrupt the incremental sorted view. */
constexpr bool median_handles_duplicates()
{
	MedianFilter<int, 5> median;
	for (int sample : {7, 7, 7, 7, 7}) {
		if (median.update(sample) != 7) {
			return false;
		}
	}
	/* Rolling a distinct value through a run of duplicates. */
	(void)median.update(1);
	(void)median.update(1);
	(void)median.update(1);
	return median.value() == 1;
}
static_assert(median_handles_duplicates());

/* A long run must stay correct after many evictions. */
constexpr bool median_survives_wraparound()
{
	MedianFilter<int, 3> median;
	int last = 0;
	for (int i = 0; i < 40; ++i) {
		last = median.update(i % 7);
	}
	return last >= 0 && last <= 6 && median.size() == 3U;
}
static_assert(median_survives_wraparound());

/* The integer EMA needs no floating point and is exactly reproducible. */
constexpr bool shift_average_works()
{
	ShiftMovingAverage<std::int32_t, 2> filter; /* alpha = 1/4 */
	if (filter.update(100) != 100) {
		return false;
	}
	if (filter.update(0) != 75) {
		return false;
	}
	/* 225 >> 2 truncates to 56. */
	if (filter.update(0) != 56) {
		return false;
	}
	filter.reset();
	return !filter.initialized();
}
static_assert(shift_average_works());

/* Hysteresis in the 'below' direction latches low and releases high. */
constexpr bool hysteresis_below()
{
	Hysteresis<int> low_battery{3400, 3500};
	if (low_battery.update(3600)) {
		return false;
	}
	if (!low_battery.update(3400)) {
		return false;
	}
	/* Inside the band the state is held. */
	if (!low_battery.update(3450)) {
		return false;
	}
	return !low_battery.update(3500);
}
static_assert(hysteresis_below());

/* The 'above' direction was previously untested. */
constexpr bool hysteresis_above()
{
	Hysteresis<int> overheat{60, 80, ThresholdDirection::above};
	if (overheat.update(70)) {
		return false;
	}
	if (!overheat.update(80)) {
		return false;
	}
	if (!overheat.update(70)) {
		return false;
	}
	return !overheat.update(60);
}
static_assert(hysteresis_above());

/* Boundaries passed the wrong way round are ordered, not rejected. */
constexpr bool hysteresis_orders_bounds()
{
	Hysteresis<int> swapped{3500, 3400};
	return swapped.low() == 3400 && swapped.high() == 3500;
}
static_assert(hysteresis_orders_bounds());

constexpr bool threshold_detector_works()
{
	ThresholdDetector<int> hot{50};
	ThresholdDetector<int> cold{10, ThresholdDirection::below};
	return hot.active(50) && hot.active(51) && !hot.active(49) && cold.active(10) &&
	       cold.active(9) && !cold.active(11);
}
static_assert(threshold_detector_works());

int main()
{
	CHECK(moving_average_works());
	CHECK(median_odd_window());
	CHECK(median_even_window());
	CHECK(median_handles_duplicates());
	CHECK(median_survives_wraparound());
	CHECK(shift_average_works());
	CHECK(hysteresis_below());
	CHECK(hysteresis_above());
	CHECK(threshold_detector_works());

	/* The float EMA converges toward the input. */
	ExponentialMovingAverage<int> smooth{0.5F};
	CHECK_NEAR(smooth.update(10), 10.0F, 0.001F);
	CHECK_NEAR(smooth.update(20), 15.0F, 0.001F);
	CHECK_NEAR(smooth.update(20), 17.5F, 0.001F);

	/* Alpha is clamped rather than rejected. */
	ExponentialMovingAverage<int> clamped{5.0F};
	CHECK_NEAR(clamped.alpha(), 1.0F, 0.001F);

	return zest::test::summary("filters");
}
