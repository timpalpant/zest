/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/units.hpp>

#include <cstdint>
#include <type_traits>

using namespace zest;
using namespace zest::literals;

/* A quantity costs exactly what its representation costs. */
static_assert(sizeof(Millivolts) == sizeof(std::int32_t));
static_assert(std::is_trivially_copyable_v<Millivolts>);

/* A bare number never becomes a quantity implicitly. */
static_assert(!std::is_convertible_v<std::int32_t, Millivolts>);
static_assert(std::is_constructible_v<Millivolts, std::int32_t>);

/* Different tags never interconvert, so volts cannot be passed as ohms. */
static_assert(!std::is_convertible_v<Millivolts, Ohms>);
static_assert(!std::is_constructible_v<Ohms, Millivolts>);

/* Widening to a finer scale is exact, so it is implicit. */
static_assert(std::is_convertible_v<Millivolts, Microvolts>);
static_assert(Microvolts{Millivolts{3742}}.count() == 3'742'000);

/* Narrowing loses information, so it needs an explicit cast. */
static_assert(!std::is_convertible_v<Millivolts, Volts>);
static_assert(quantity_cast<Volts>(Millivolts{3742}).count() == 3);
static_assert(quantity_cast<Millivolts>(Volts{4}).count() == 4000);

/* Arithmetic stays inside the unit. */
static_assert((Millivolts{100} + Millivolts{50}).count() == 150);
static_assert((Millivolts{100} - Millivolts{50}).count() == 50);
static_assert((Millivolts{100} * 3).count() == 300);
static_assert((3 * Millivolts{100}).count() == 300);
static_assert((Millivolts{100} / 4).count() == 25);
static_assert(Millivolts{100} / Millivolts{25} == 4);
static_assert((-Millivolts{100}).count() == -100);

/* Comparison is ordered. */
static_assert(Millivolts{100} > Millivolts{50});
static_assert(Millivolts{100} == Millivolts{100});
static_assert(Millivolts{50} < Millivolts{100});

/* Literals read the way a schematic does. */
static_assert(3300_mV == Millivolts{3300});
static_assert(100_kohm == Kilohms{100});
static_assert(25_degC == Celsius{25});
static_assert(quantity_cast<Ohms>(100_kohm).count() == 100'000);

/* A float representation converts freely, since nothing is lost. */
static_assert(std::is_convertible_v<Volts, VoltsF>);

/* The divider helper reconstructs the input voltage in integer arithmetic. */
static_assert(divider_input(Millivolts{1000}, Ohms{100}, Ohms{300}).count() == 3000);
static_assert(divider_input(Millivolts{1650}, Ohms{100}, Ohms{200}).count() == 3300);
/* A degenerate divider yields zero rather than dividing by zero. */
static_assert(divider_input(Millivolts{1000}, Ohms{0}, Ohms{300}).count() == 0);

int main()
{
	Millivolts battery{4100};
	battery -= Millivolts{400};
	CHECK_EQ(battery.count(), 3700);
	battery += Millivolts{100};
	CHECK_EQ(battery.count(), 3800);

	CHECK_EQ(quantity_cast<Volts>(battery).count(), 3);
	CHECK_EQ(Microvolts{battery}.count(), 3'800'000);

	MilliCelsius reading{23'500};
	CHECK_EQ(quantity_cast<Celsius>(reading).count(), 23);
	CHECK_EQ(quantity_cast<DeciCelsius>(reading).count(), 235);

	CHECK_EQ(divider_input(1650_mV, Ohms{100}, Ohms{200}).count(), 3300);
	CHECK_EQ(quantity_cast<Hertz>(2_kHz).count(), 2000U);

	return zest::test::summary("units");
}
