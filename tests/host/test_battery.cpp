/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/battery.hpp>

#include <array>

using namespace zest;
using namespace zest::literals;

constexpr std::array cells{
	CurvePoint{4200_mV, 100},
	CurvePoint{3700_mV, 10},
	CurvePoint{3300_mV, 0},
};

/* A literal curve is validated at compile time and costs nothing at run time. */
constexpr auto curve = battery_curve(cells);

static_assert(curve.percent_at(4300_mV) == 100); /* clamped above */
static_assert(curve.percent_at(4200_mV) == 100);
static_assert(curve.percent_at(3950_mV) == 55); /* midway on the upper segment */
static_assert(curve.percent_at(3700_mV) == 10);
static_assert(curve.percent_at(3300_mV) == 0);
static_assert(curve.percent_at(3000_mV) == 0); /* clamped below */
static_assert(curve.full() == 4200_mV);
static_assert(curve.empty() == 3300_mV);

/* validate_curve rejects each malformed shape with the right reason. */
static_assert(validate_curve(cells).has_value());

constexpr std::array too_short{CurvePoint{4200_mV, 100}};
static_assert(validate_curve(too_short).error() == CurveError::insufficient_points);

constexpr std::array ascending{CurvePoint{3700_mV, 100}, CurvePoint{4200_mV, 0}};
static_assert(validate_curve(ascending).error() == CurveError::invalid_voltage_order);

constexpr std::array rising_percent{CurvePoint{4200_mV, 10}, CurvePoint{3700_mV, 90}};
static_assert(validate_curve(rising_percent).error() == CurveError::invalid_percentage);

constexpr std::array over_100{CurvePoint{4200_mV, 120}, CurvePoint{3700_mV, 10}};
static_assert(validate_curve(over_100).error() == CurveError::invalid_percentage);

constexpr std::array duplicate_voltage{CurvePoint{4200_mV, 100}, CurvePoint{4200_mV, 0}};
static_assert(validate_curve(duplicate_voltage).error() == CurveError::invalid_voltage_order);

/* make_battery_curve reports rather than aborting, for runtime curves. */
static_assert(make_battery_curve(cells).has_value());
static_assert(!make_battery_curve(ascending).has_value());

/* The validating convenience overload keeps working. */
static_assert(estimate_charge_percent(3950_mV, cells).value() == 55);
static_assert(estimate_charge_percent(3900_mV, ascending).error() ==
	      CurveError::invalid_voltage_order);

int main()
{
	CHECK_EQ(curve.percent_at(3950_mV), 55);
	CHECK_EQ(curve.percent_at(5000_mV), 100);
	CHECK_EQ(curve.percent_at(0_mV), 0);

	/* Interpolation is monotonically non-increasing as voltage falls. */
	int previous = 101;
	for (std::int32_t mv = 4300; mv >= 3200; mv -= 10) {
		const int percent = curve.percent_at(Millivolts{mv});
		CHECK(percent <= previous);
		previous = percent;
	}

	const auto built = make_battery_curve(cells);
	CHECK(built.has_value());
	CHECK_EQ(built->percent_at(3700_mV), 10);

	const auto rejected = make_battery_curve(ascending);
	CHECK(!rejected.has_value());
	CHECK(std::string_view{to_string(CurveError::invalid_voltage_order)}.size() > 0U);

	return zest::test::summary("battery");
}
