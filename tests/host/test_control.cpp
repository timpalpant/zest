/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/control.hpp>

#include <array>
#include <chrono>
#include <cstdint>

using namespace zest;
using namespace std::chrono_literals;

/* Proportional-only control is a straight gain on the error. */
constexpr bool proportional_only()
{
	PidController<float> pid{{.proportional = 2.0F},
				 {.output_min = -100.0F, .output_max = 100.0F}};
	const float command = pid.update(10.0F, 4.0F, 1s);
	return command > 11.9F && command < 12.1F;
}
static_assert(proportional_only());

/* The command is clamped, and saturation is reported. */
constexpr bool clamps_output()
{
	PidController<float> pid{{.proportional = 10.0F},
				 {.output_min = -5.0F, .output_max = 5.0F}};
	const float command = pid.update(10.0F, 0.0F, 1s);
	return command == 5.0F && pid.saturated();
}
static_assert(clamps_output());

/* A non-positive interval must not divide by zero, and must not integrate. */
constexpr bool tolerates_zero_interval()
{
	PidController<float> pid{{.proportional = 1.0F, .integral = 100.0F}};
	(void)pid.update(1.0F, 0.0F, 0s);
	return pid.integral() == 0.0F;
}
static_assert(tolerates_zero_interval());

int main()
{
	CHECK(proportional_only());
	CHECK(clamps_output());
	CHECK(tolerates_zero_interval());

	/* Anti-windup: while saturated, an error pushing further in must not integrate. */
	{
		PidController<float> pid{
			{.proportional = 1.0F, .integral = 50.0F},
			{.output_min = -10.0F, .output_max = 10.0F, .integral_limit = 1000.0F}};
		for (int i = 0; i < 20; ++i) {
			(void)pid.update(100.0F, 0.0F, 10ms);
		}
		CHECK(pid.saturated());
		/* The integral never wound up, so recovery is immediate. */
		CHECK_NEAR(pid.integral(), 0.0F, 0.001F);
		const float recovery = pid.update(0.0F, 0.0F, 10ms);
		CHECK_NEAR(recovery, 0.0F, 0.001F);
	}

	/* An integral term drives the error to zero when the plant is a pure gain. */
	{
		PidController<float> pid{{.proportional = 0.5F, .integral = 4.0F},
					 {.output_min = -50.0F, .output_max = 50.0F}};
		float measurement = 0.0F;
		for (int i = 0; i < 400; ++i) {
			const float command = pid.update(10.0F, measurement, 10ms);
			measurement += (command - measurement) * 0.2F;
		}
		CHECK_NEAR(measurement, 10.0F, 0.2F);
	}

	/* Derivative acts on the measurement, so a setpoint step causes no kick. */
	{
		PidController<float> steady{{.proportional = 1.0F, .derivative = 100.0F},
					    {.output_min = -1000.0F, .output_max = 1000.0F}};
		(void)steady.update(0.0F, 0.0F, 10ms);
		const float after_step = steady.update(5.0F, 0.0F, 10ms);
		/* Only the proportional term responds: no 1/dt spike. */
		CHECK_NEAR(after_step, 5.0F, 0.001F);
	}

	/* reset() clears accumulated state. */
	{
		PidController<float> pid{{.proportional = 1.0F, .integral = 10.0F},
					 {.output_min = -100.0F, .output_max = 100.0F}};
		(void)pid.update(5.0F, 0.0F, 100ms);
		CHECK(pid.integral() != 0.0F);
		pid.reset();
		CHECK_NEAR(pid.integral(), 0.0F, 0.001F);
		CHECK_NEAR(pid.value(), 0.0F, 0.001F);
	}

	/* Slew limiting ramps rather than steps. */
	{
		SlewRateLimiter<float> ramp{10.0F};
		CHECK_NEAR(ramp.update(100.0F, 1s), 10.0F, 0.001F);
		CHECK_NEAR(ramp.update(100.0F, 500ms), 15.0F, 0.001F);
		/* A target within reach is met exactly. */
		CHECK_NEAR(ramp.update(15.5F, 1s), 15.5F, 0.001F);
		CHECK(ramp.settled(15.5F, 0.01F));
		/* Ramping down is limited too. */
		CHECK_NEAR(ramp.update(-100.0F, 1s), 5.5F, 0.001F);
		/* A non-positive interval holds the value. */
		CHECK_NEAR(ramp.update(-100.0F, 0s), 5.5F, 0.001F);
	}

	/* The integer slew helper needs no FPU. */
	CHECK_EQ(slew_toward<std::int32_t>(0, 100, 10), 10);
	CHECK_EQ(slew_toward<std::int32_t>(0, 5, 10), 5);
	CHECK_EQ(slew_toward<std::int32_t>(0, -100, 10), -10);
	CHECK_EQ(slew_toward<std::int32_t>(0, -5, 10), -5);
	CHECK_EQ(slew_toward<std::int32_t>(50, 50, 10), 50);

	/* A table-driven state machine ignores events it does not accept. */
	{
		enum class Link {
			down,
			joining,
			up
		};
		enum class Signal {
			start,
			joined,
			lost
		};

		constexpr std::array table{
			Transition<Link, Signal>{Link::down, Signal::start, Link::joining},
			Transition<Link, Signal>{Link::joining, Signal::joined, Link::up},
			Transition<Link, Signal>{Link::up, Signal::lost, Link::down},
		};
		StateMachine machine{Link::down, table};

		CHECK(machine.state() == Link::down);
		CHECK(machine.accepts(Signal::start));
		CHECK(!machine.accepts(Signal::joined));

		CHECK(machine.dispatch(Signal::start).value() == Link::joining);
		/* An event with no transition leaves the state alone. */
		CHECK(!machine.dispatch(Signal::start).has_value());
		CHECK(machine.state() == Link::joining);

		CHECK(machine.dispatch(Signal::joined).value() == Link::up);
		CHECK(machine.dispatch(Signal::lost).value() == Link::down);

		machine.force(Link::up);
		CHECK(machine.state() == Link::up);
	}

	return zest::test::summary("control");
}
