#include <zest/gpio.hpp>

#include <zephyr/drivers/gpio.h>

#include <cerrno>
#include <expected>

namespace zest
{
namespace
{

[[nodiscard]] constexpr int as_value(GpioState state) noexcept
{
	return state == GpioState::active ? 1 : 0;
}

[[nodiscard]] constexpr GpioState as_state(int value) noexcept
{
	return value == 0 ? GpioState::inactive : GpioState::active;
}

[[nodiscard]] std::expected<void, int> require_ready(const gpio_dt_spec &spec) noexcept
{
	if (!gpio_is_ready_dt(&spec)) {
		return std::unexpected(-ENODEV);
	}
	return {};
}

[[nodiscard]] std::expected<GpioState, int> read(const gpio_dt_spec &spec) noexcept
{
	const int value = gpio_pin_get_dt(&spec);
	if (value < 0) {
		return std::unexpected(value);
	}
	return as_state(value);
}

} /* namespace */

std::expected<void, int> GpioInput::init() const noexcept
{
	if (const auto ready = require_ready(spec_); !ready) {
		return ready;
	}
	if (const int rc = gpio_pin_configure_dt(&spec_, GPIO_INPUT); rc < 0) {
		return std::unexpected(rc);
	}
	return {};
}

std::expected<GpioState, int> GpioInput::get() const noexcept
{
	return read(spec_);
}

std::expected<void, int> GpioOutput::init(GpioState initial) const noexcept
{
	if (const auto ready = require_ready(spec_); !ready) {
		return ready;
	}

	const gpio_flags_t flags =
		initial == GpioState::active ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE;
	if (const int rc = gpio_pin_configure_dt(&spec_, flags); rc < 0) {
		return std::unexpected(rc);
	}
	return {};
}

std::expected<void, int> GpioOutput::set(GpioState state) const noexcept
{
	if (const int rc = gpio_pin_set_dt(&spec_, as_value(state)); rc < 0) {
		return std::unexpected(rc);
	}
	return {};
}

std::expected<void, int> GpioOutput::toggle() const noexcept
{
	if (const int rc = gpio_pin_toggle_dt(&spec_); rc < 0) {
		return std::unexpected(rc);
	}
	return {};
}

std::expected<GpioState, int> GpioOutput::get() const noexcept
{
	return read(spec_);
}

} /* namespace zest */
