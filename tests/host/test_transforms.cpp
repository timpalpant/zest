/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"


#include <zest/transforms.hpp>

#include <cstdint>

using namespace zest;

constexpr bool calibration_works()
{
	constexpr Calibration<int> calibration{2.0F, -1.0F};
	return calibration.apply(4) > 6.9F && calibration.apply(4) < 7.1F;
}
static_assert(calibration_works());

/* The integer calibration must round half away from zero, both signs. */
constexpr bool integer_calibration_rounds()
{
	constexpr IntegerCalibration<std::int32_t> scale{3, 2, 0}; /* x1.5 */
	return scale.apply(10) == 15 && scale.apply(1) == 2 && scale.apply(-1) == -2 &&
	       scale.apply(-10) == -15;
}
static_assert(integer_calibration_rounds());

constexpr bool integer_calibration_offsets()
{
	constexpr IntegerCalibration<std::int32_t> sensor{1, 1, -273};
	return sensor.apply(300) == 27;
}
static_assert(integer_calibration_offsets());

/* A zero denominator must not divide by zero. */
constexpr bool integer_calibration_guards_denominator()
{
	constexpr IntegerCalibration<std::int32_t> broken{5, 0, 0};
	return broken.apply(3) == 15;
}
static_assert(integer_calibration_guards_denominator());

constexpr bool linear_map_works()
{
	constexpr LinearMap<int> mapping{0, 10, 0.0F, 100.0F};
	const auto midpoint = mapping.map(5);
	return midpoint.has_value() && *midpoint > 49.9F && *midpoint < 50.1F;
}
static_assert(linear_map_works());

constexpr bool linear_map_rejects_empty_range()
{
	constexpr LinearMap<int> degenerate{5, 5, 0.0F, 1.0F};
	const auto mapped = degenerate.map(5);
	return !mapped.has_value() && mapped.error() == TransformError::empty_input_range;
}
static_assert(linear_map_rejects_empty_range());

/* map() extrapolates; map_clamped() does not. */
constexpr bool linear_map_clamps()
{
	constexpr LinearMap<int> mapping{0, 10, 0.0F, 100.0F};
	const auto beyond = mapping.map(20);
	const auto clamped = mapping.map_clamped(20);
	const auto under = mapping.map_clamped(-5);
	return beyond.has_value() && *beyond > 199.0F && clamped.has_value() &&
	       *clamped > 99.9F && *clamped < 100.1F && under.has_value() && *under == 0.0F;
}
static_assert(linear_map_clamps());

constexpr bool integer_map_works()
{
	const auto half = integer_map<std::int32_t>(5, 0, 10, 0, 100);
	const auto low = integer_map<std::int32_t>(0, 0, 4095, -40, 125);
	const auto degenerate = integer_map<std::int32_t>(1, 7, 7, 0, 10);
	return half.has_value() && *half == 50 && low.has_value() && *low == -40 &&
	       !degenerate.has_value();
}
static_assert(integer_map_works());

/* Inverted output ranges must map monotonically downward. */
constexpr bool integer_map_inverted_output()
{
	const auto mapped = integer_map<std::int32_t>(0, 0, 100, 100, 0);
	const auto other = integer_map<std::int32_t>(100, 0, 100, 100, 0);
	return mapped.has_value() && *mapped == 100 && other.has_value() && *other == 0;
}
static_assert(integer_map_inverted_output());

int main()
{
	CHECK(calibration_works());
	CHECK(integer_calibration_rounds());
	CHECK(integer_calibration_offsets());
	CHECK(integer_calibration_guards_denominator());
	CHECK(linear_map_works());
	CHECK(linear_map_rejects_empty_range());
	CHECK(linear_map_clamps());
	CHECK(integer_map_works());
	CHECK(integer_map_inverted_output());
	CHECK(std::string_view{to_string(TransformError::empty_input_range)}.size() > 0U);
	return zest::test::summary("transforms");
}
