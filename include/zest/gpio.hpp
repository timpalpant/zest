/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/drivers/gpio.h>

namespace zest
{

/** A logical GPIO state that respects active-low devicetree flags. */
enum class GpioState {
	inactive,
	active,
};

/** The opposite logical state. */
[[nodiscard]] constexpr GpioState invert(GpioState state) noexcept
{
	return state == GpioState::active ? GpioState::inactive : GpioState::active;
}

[[nodiscard]] constexpr const char *to_string(GpioState state) noexcept
{
	return state == GpioState::active ? "active" : "inactive";
}

/** A devicetree-configured digital input. */
class GpioInput
{
      public:
	constexpr explicit GpioInput(gpio_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Configure the pin as an input. */
	[[nodiscard]] Result<> init() const noexcept;

	/** Read the pin's logical active/inactive state. */
	[[nodiscard]] Result<GpioState> get() const noexcept;

	[[nodiscard]] constexpr const gpio_dt_spec &native_spec() const noexcept
	{
		return spec_;
	}

      private:
	gpio_dt_spec spec_;
};

/**
 * A devicetree-configured digital output.
 *
 * The last driven state is tracked in the object, so `state()` is exact and
 * cannot fail. Reading the pin back instead --- which an earlier version did via
 * `gpio_pin_get_dt()` --- does not work for a plain output: Zephyr documents that
 * call for *input* pins, and on most SoCs a pin configured `GPIO_OUTPUT_*` has its
 * input buffer disabled, so the read returns a constant rather than an error.
 *
 * `read_pin()` remains available when the electrical level genuinely matters, but
 * it requires the pin to have been configured for readback --- see `init()`.
 *
 * Because the object now carries state, `set()` and `toggle()` are non-const.
 */
class GpioOutput
{
      public:
	constexpr explicit GpioOutput(gpio_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/**
	 * Configure the pin as an output with a known initial logical state.
	 *
	 * Pass @p enable_readback to add `GPIO_INPUT` to the configuration so
	 * `read_pin()` works. Not every pin or SoC can drive and sense at once, so
	 * this is opt-in rather than the default.
	 */
	[[nodiscard]] Result<> init(GpioState initial = GpioState::inactive,
				    bool enable_readback = false) noexcept;

	/** Drive the pin to a logical state. */
	[[nodiscard]] Result<> set(GpioState state) noexcept;

	/** Drive the pin to the opposite of its current state. */
	[[nodiscard]] Result<> toggle() noexcept;

	/** The last state successfully driven. Exact, and cannot fail. */
	[[nodiscard]] constexpr GpioState state() const noexcept
	{
		return state_;
	}

	/**
	 * Read the pin's actual logical level.
	 *
	 * Requires `init(..., enable_readback = true)`, or devicetree flags that
	 * already include `GPIO_INPUT`. Without one of those the result is
	 * meaningless on most hardware, so prefer `state()`.
	 */
	[[nodiscard]] Result<GpioState> read_pin() const noexcept;

	[[nodiscard]] constexpr bool readback_enabled() const noexcept
	{
		return readback_;
	}

	[[nodiscard]] constexpr const gpio_dt_spec &native_spec() const noexcept
	{
		return spec_;
	}

      private:
	gpio_dt_spec spec_;
	GpioState state_{GpioState::inactive};
	bool readback_{false};
};

} /* namespace zest */
