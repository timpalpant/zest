/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/gpio.hpp>

#include <zephyr/drivers/gpio.h>

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

Result<> require_ready(const gpio_dt_spec &spec) noexcept
{
	if (!gpio_is_ready_dt(&spec)) {
		return fail(errors::no_device);
	}
	return {};
}

Result<GpioState> read(const gpio_dt_spec &spec) noexcept
{
	const int value = gpio_pin_get_dt(&spec);
	if (value < 0) {
		return fail(value);
	}
	return as_state(value);
}

} /* namespace */

Result<> GpioInput::init() const noexcept
{
	ZEST_TRY(require_ready(spec_));
	return check(gpio_pin_configure_dt(&spec_, GPIO_INPUT));
}

Result<GpioState> GpioInput::get() const noexcept
{
	return read(spec_);
}

Result<> GpioOutput::init(GpioState initial, bool enable_readback) noexcept
{
	ZEST_TRY(require_ready(spec_));

	gpio_flags_t flags =
		initial == GpioState::active ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE;
	if (enable_readback) {
		flags |= GPIO_INPUT;
	}
	ZEST_TRY(check(gpio_pin_configure_dt(&spec_, flags)));

	state_ = initial;
	readback_ = enable_readback || (spec_.dt_flags & GPIO_INPUT) != 0;
	return {};
}

Result<> GpioOutput::set(GpioState state) noexcept
{
	ZEST_TRY(check(gpio_pin_set_dt(&spec_, as_value(state))));
	state_ = state;
	return {};
}

Result<> GpioOutput::toggle() noexcept
{
	ZEST_TRY(check(gpio_pin_toggle_dt(&spec_)));
	state_ = invert(state_);
	return {};
}

Result<GpioState> GpioOutput::read_pin() const noexcept
{
	if (!readback_) {
		/*
		 * A pin configured output-only reports a driver constant rather than
		 * its level, so refuse instead of returning something meaningless.
		 */
		return fail(errors::not_supported);
	}
	return read(spec_);
}

} /* namespace zest */
