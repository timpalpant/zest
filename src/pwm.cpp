/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/pwm.hpp>

#include <zephyr/drivers/pwm.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace zest
{

Result<> PwmOutput::init() const noexcept
{
	if (!pwm_is_ready_dt(&spec_)) {
		return fail(errors::no_device);
	}
	return disable();
}

Result<> PwmOutput::set_pulse(std::chrono::nanoseconds pulse) const noexcept
{
	return set(period(), pulse);
}

Result<> PwmOutput::set_duty(PerMille duty) const noexcept
{
	if (duty > kFullScale) {
		return fail(errors::invalid_argument);
	}
	/* Integer scaling: no FPU involved. */
	const auto pulse = static_cast<std::int64_t>(spec_.period) * duty / kFullScale;
	return set_pulse(std::chrono::nanoseconds{pulse});
}

Result<> PwmOutput::set(std::chrono::nanoseconds period,
			std::chrono::nanoseconds pulse) const noexcept
{
	if (period.count() <= 0 || pulse.count() < 0 || pulse > period ||
	    period.count() > std::numeric_limits<std::uint32_t>::max() ||
	    pulse.count() > std::numeric_limits<std::uint32_t>::max()) {
		return fail(errors::invalid_argument);
	}
	return check(pwm_set(spec_.dev, spec_.channel, static_cast<std::uint32_t>(period.count()),
			     static_cast<std::uint32_t>(pulse.count()), spec_.flags));
}

Result<> PwmOutput::disable() const noexcept
{
	return check(pwm_set_pulse_dt(&spec_, 0U));
}

Result<> RgbLed::init() const noexcept
{
	ZEST_TRY(red_.init());
	ZEST_TRY(green_.init());
	return blue_.init();
}

Result<> RgbLed::set(RgbColor color) const noexcept
{
	/* 8-bit intensity scaled to per-mille without floating point. */
	constexpr auto to_per_mille = [](std::uint8_t level) constexpr -> PerMille {
		return static_cast<PerMille>((static_cast<std::uint32_t>(level) * kFullScale + 127U) /
					     255U);
	};
	ZEST_TRY(red_.set_duty(to_per_mille(color.red)));
	ZEST_TRY(green_.set_duty(to_per_mille(color.green)));
	return blue_.set_duty(to_per_mille(color.blue));
}

Result<> RgbLed::off() const noexcept
{
	return set(colors::off);
}

Result<> Servo::set_position(PerMille position) const noexcept
{
	if (position > kFullScale || minimum_pulse_.count() < 0 || maximum_pulse_ < minimum_pulse_) {
		return fail(errors::invalid_argument);
	}
	const auto span = (maximum_pulse_ - minimum_pulse_).count();
	const auto offset = static_cast<std::int64_t>(span) * position / kFullScale;
	return output_.set_pulse(minimum_pulse_ + std::chrono::nanoseconds{offset});
}

Result<> Buzzer::tone(Hertz frequency, PerMille volume) const noexcept
{
	if (frequency.count() == 0U || volume > kFullScale) {
		return fail(errors::invalid_argument);
	}

	constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
	const auto period =
		std::chrono::nanoseconds{kNanosecondsPerSecond / static_cast<std::int64_t>(
							 frequency.count())};
	/* A square wave at half duty, scaled by volume. */
	const auto pulse = std::chrono::nanoseconds{period.count() * volume / (2 * kFullScale)};
	return output_.set(period, pulse);
}

} /* namespace zest */
