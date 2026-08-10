/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zephyr/drivers/pwm.h>

#include <chrono>
#include <cstdint>
#include <expected>

namespace zest
{

/** A devicetree-configured PWM output. */
class PwmOutput
{
      public:
	constexpr explicit PwmOutput(pwm_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Verify the controller and drive a zero pulse. */
	[[nodiscard]] std::expected<void, int> init() const noexcept;

	/** Set the pulse width using the period from devicetree. */
	[[nodiscard]] std::expected<void, int>
	set_pulse(std::chrono::nanoseconds pulse) const noexcept;

	/** Set a duty cycle in the inclusive range 0..1. */
	[[nodiscard]] std::expected<void, int> set_duty_cycle(double duty_cycle) const noexcept;

	/** Set both period and pulse width. */
	[[nodiscard]] std::expected<void, int> set(std::chrono::nanoseconds period,
						   std::chrono::nanoseconds pulse) const noexcept;

	/** Drive a zero pulse. */
	[[nodiscard]] std::expected<void, int> disable() const noexcept;

	[[nodiscard]] constexpr std::chrono::nanoseconds period() const noexcept
	{
		return std::chrono::nanoseconds{spec_.period};
	}

      private:
	pwm_dt_spec spec_;
};

/** A single PWM-controlled LED. */
class DimmableLed
{
      public:
	constexpr explicit DimmableLed(pwm_dt_spec spec) noexcept : output_{spec}
	{
	}

	[[nodiscard]] std::expected<void, int> init() const noexcept
	{
		return output_.init();
	}
	[[nodiscard]] std::expected<void, int> set_brightness(double brightness) const noexcept
	{
		return output_.set_duty_cycle(brightness);
	}
	[[nodiscard]] std::expected<void, int> off() const noexcept
	{
		return output_.disable();
	}

      private:
	PwmOutput output_;
};

/** Eight-bit red, green, and blue intensities. */
struct RgbColor {
	std::uint8_t red;
	std::uint8_t green;
	std::uint8_t blue;
};

/** Three-channel PWM RGB LED. */
class RgbLed
{
      public:
	constexpr RgbLed(pwm_dt_spec red, pwm_dt_spec green, pwm_dt_spec blue) noexcept
		: red_{red}, green_{green}, blue_{blue}
	{
	}

	[[nodiscard]] std::expected<void, int> init() const noexcept;
	[[nodiscard]] std::expected<void, int> set(RgbColor color) const noexcept;
	[[nodiscard]] std::expected<void, int> off() const noexcept;

      private:
	PwmOutput red_;
	PwmOutput green_;
	PwmOutput blue_;
};

/** PWM hobby-servo output with configurable endpoint pulse widths. */
class Servo
{
      public:
	constexpr Servo(pwm_dt_spec output, std::chrono::nanoseconds minimum_pulse,
			std::chrono::nanoseconds maximum_pulse) noexcept
		: output_{output}, minimum_pulse_{minimum_pulse}, maximum_pulse_{maximum_pulse}
	{
	}

	[[nodiscard]] std::expected<void, int> init() const noexcept
	{
		return output_.init();
	}

	/** Set normalized position from 0 to 1. */
	[[nodiscard]] std::expected<void, int> set_position(double position) const noexcept;

	[[nodiscard]] std::expected<void, int> disable() const noexcept
	{
		return output_.disable();
	}

      private:
	PwmOutput output_;
	std::chrono::nanoseconds minimum_pulse_;
	std::chrono::nanoseconds maximum_pulse_;
};

/** PWM square-wave buzzer. */
class Buzzer
{
      public:
	constexpr explicit Buzzer(pwm_dt_spec output) noexcept : output_{output}
	{
	}

	[[nodiscard]] std::expected<void, int> init() const noexcept
	{
		return output_.init();
	}

	/** Start a tone. Volume is a duty amplitude in the inclusive range 0..1. */
	[[nodiscard]] std::expected<void, int> tone(std::uint32_t frequency_hz,
						    double volume = 1.0) const noexcept;

	[[nodiscard]] std::expected<void, int> stop() const noexcept
	{
		return output_.disable();
	}

      private:
	PwmOutput output_;
};

} /* namespace zest */
