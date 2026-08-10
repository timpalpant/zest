#include <zest/pwm.hpp>

#include <zephyr/drivers/pwm.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>

namespace zest
{
namespace
{

[[nodiscard]] std::expected<void, int> as_expected(int rc) noexcept
{
	if (rc < 0) {
		return std::unexpected(rc);
	}
	return {};
}

} /* namespace */

std::expected<void, int> PwmOutput::init() const noexcept
{
	if (!pwm_is_ready_dt(&spec_)) {
		return std::unexpected(-ENODEV);
	}
	return disable();
}

std::expected<void, int> PwmOutput::set_pulse(std::chrono::nanoseconds pulse) const noexcept
{
	return set(period(), pulse);
}

std::expected<void, int> PwmOutput::set_duty_cycle(double duty_cycle) const noexcept
{
	if (duty_cycle < 0.0 || duty_cycle > 1.0) {
		return std::unexpected(-EINVAL);
	}
	const auto pulse = std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(
		static_cast<double>(spec_.period) * duty_cycle)};
	return set_pulse(pulse);
}

std::expected<void, int> PwmOutput::set(std::chrono::nanoseconds period,
					std::chrono::nanoseconds pulse) const noexcept
{
	if (period.count() <= 0 || pulse.count() < 0 || pulse > period ||
	    period.count() > std::numeric_limits<std::uint32_t>::max() ||
	    pulse.count() > std::numeric_limits<std::uint32_t>::max()) {
		return std::unexpected(-EINVAL);
	}
	return as_expected(pwm_set(spec_.dev, spec_.channel,
				   static_cast<std::uint32_t>(period.count()),
				   static_cast<std::uint32_t>(pulse.count()), spec_.flags));
}

std::expected<void, int> PwmOutput::disable() const noexcept
{
	return as_expected(pwm_set_pulse_dt(&spec_, 0U));
}

std::expected<void, int> RgbLed::init() const noexcept
{
	if (const auto result = red_.init(); !result) {
		return result;
	}
	if (const auto result = green_.init(); !result) {
		return result;
	}
	return blue_.init();
}

std::expected<void, int> RgbLed::set(RgbColor color) const noexcept
{
	constexpr double kMaximum = 255.0;
	if (const auto result = red_.set_duty_cycle(color.red / kMaximum); !result) {
		return result;
	}
	if (const auto result = green_.set_duty_cycle(color.green / kMaximum); !result) {
		return result;
	}
	return blue_.set_duty_cycle(color.blue / kMaximum);
}

std::expected<void, int> RgbLed::off() const noexcept
{
	return set({0U, 0U, 0U});
}

std::expected<void, int> Servo::set_position(double position) const noexcept
{
	if (position < 0.0 || position > 1.0 || minimum_pulse_.count() < 0 ||
	    maximum_pulse_ < minimum_pulse_) {
		return std::unexpected(-EINVAL);
	}
	const auto range = maximum_pulse_ - minimum_pulse_;
	const auto pulse = minimum_pulse_ +
			   std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(
				   static_cast<double>(range.count()) * position)};
	return output_.set_pulse(pulse);
}

std::expected<void, int> Buzzer::tone(std::uint32_t frequency_hz, double volume) const noexcept
{
	if (frequency_hz == 0U || volume < 0.0 || volume > 1.0) {
		return std::unexpected(-EINVAL);
	}

	constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
	const auto period = std::chrono::nanoseconds{
		static_cast<std::chrono::nanoseconds::rep>(kNanosecondsPerSecond / frequency_hz)};
	const auto pulse = std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(
		static_cast<double>(period.count()) * 0.5 * volume)};
	return output_.set(period, pulse);
}

} /* namespace zest */
