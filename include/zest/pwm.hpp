/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/units.hpp>

#include <zephyr/drivers/pwm.h>

#include <chrono>
#include <cstdint>

namespace zest
{

/**
 * Duty cycle in per-mille (0..1000).
 *
 * Integer rather than floating point, which would cost a soft-float call in an
 * LED or motor update loop. Per-mille is finer than any PWM peripheral resolves
 * in practice.
 */
using PerMille = std::uint16_t;

inline constexpr PerMille kFullScale = 1000U;

/** Convert a 0..1 fraction to per-mille, clamping out-of-range input. */
[[nodiscard]] constexpr PerMille per_mille_from(float fraction) noexcept
{
	if (fraction <= 0.0F) {
		return 0U;
	}
	if (fraction >= 1.0F) {
		return kFullScale;
	}
	return static_cast<PerMille>(fraction * 1000.0F + 0.5F);
}

/** A devicetree-configured PWM output. */
class PwmOutput
{
      public:
	constexpr explicit PwmOutput(pwm_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Verify the controller and drive a zero pulse. */
	[[nodiscard]] Result<> init() const noexcept;

	/** Set the pulse width using the period from devicetree. */
	[[nodiscard]] Result<> set_pulse(std::chrono::nanoseconds pulse) const noexcept;

	/** Set the duty cycle in per-mille, using integer arithmetic throughout. */
	[[nodiscard]] Result<> set_duty(PerMille duty) const noexcept;

	/** Convenience for a 0..1 fraction; converts to per-mille first. */
	[[nodiscard]] Result<> set_duty_cycle(float duty_cycle) const noexcept
	{
		return set_duty(per_mille_from(duty_cycle));
	}

	/** Set both period and pulse width. */
	[[nodiscard]] Result<> set(std::chrono::nanoseconds period,
				   std::chrono::nanoseconds pulse) const noexcept;

	/** Drive a zero pulse. */
	[[nodiscard]] Result<> disable() const noexcept;

	[[nodiscard]] constexpr std::chrono::nanoseconds period() const noexcept
	{
		return std::chrono::nanoseconds{spec_.period};
	}

	[[nodiscard]] constexpr const pwm_dt_spec &native_spec() const noexcept
	{
		return spec_;
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

	[[nodiscard]] Result<> init() const noexcept
	{
		return output_.init();
	}
	[[nodiscard]] Result<> set_brightness(PerMille brightness) const noexcept
	{
		return output_.set_duty(brightness);
	}
	[[nodiscard]] Result<> set_brightness(float brightness) const noexcept
	{
		return output_.set_duty_cycle(brightness);
	}
	[[nodiscard]] Result<> off() const noexcept
	{
		return output_.disable();
	}

	[[nodiscard]] constexpr const PwmOutput &output() const noexcept
	{
		return output_;
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

namespace colors
{
inline constexpr RgbColor off{0U, 0U, 0U};
inline constexpr RgbColor red{255U, 0U, 0U};
inline constexpr RgbColor green{0U, 255U, 0U};
inline constexpr RgbColor blue{0U, 0U, 255U};
inline constexpr RgbColor amber{255U, 191U, 0U};
inline constexpr RgbColor white{255U, 255U, 255U};
} /* namespace colors */

/** Three-channel PWM RGB LED. */
class RgbLed
{
      public:
	constexpr RgbLed(pwm_dt_spec red, pwm_dt_spec green, pwm_dt_spec blue) noexcept
		: red_{red}, green_{green}, blue_{blue}
	{
	}

	[[nodiscard]] Result<> init() const noexcept;
	[[nodiscard]] Result<> set(RgbColor color) const noexcept;
	[[nodiscard]] Result<> off() const noexcept;

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

	[[nodiscard]] Result<> init() const noexcept
	{
		return output_.init();
	}

	/** Set position in per-mille of travel, from 0 to 1000. */
	[[nodiscard]] Result<> set_position(PerMille position) const noexcept;

	[[nodiscard]] Result<> set_position(float position) const noexcept
	{
		return set_position(per_mille_from(position));
	}

	[[nodiscard]] Result<> disable() const noexcept
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

	[[nodiscard]] Result<> init() const noexcept
	{
		return output_.init();
	}

	/** Start a tone. Volume is a duty amplitude in per-mille. */
	[[nodiscard]] Result<> tone(Hertz frequency, PerMille volume = kFullScale) const noexcept;

	[[nodiscard]] Result<> stop() const noexcept
	{
		return output_.disable();
	}

      private:
	PwmOutput output_;
};

} /* namespace zest */
