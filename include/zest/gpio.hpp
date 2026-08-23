/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/kernel.hpp>

#include <zephyr/drivers/gpio.h>

#include <chrono>

namespace zest
{

/** A logical GPIO state that respects active-low devicetree flags. */
enum class GpioState {
	inactive,
	active,
};

/** The logical transition that wakes an interrupt-driven input. */
enum class GpioEdge {
	to_active,
	to_inactive,
	both,
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

/** An interrupt-driven digital input with thread-context edge waits. */
class GpioInterruptInput
{
      public:
	explicit GpioInterruptInput(gpio_dt_spec spec) noexcept : input_{spec}
	{
	}
	~GpioInterruptInput() noexcept;
	GpioInterruptInput(const GpioInterruptInput &) = delete;
	GpioInterruptInput &operator=(const GpioInterruptInput &) = delete;

	/** Configure the pin as an input. */
	[[nodiscard]] Result<> init() const noexcept
	{
		return input_.init();
	}

	/** Read the pin's logical active/inactive state. */
	[[nodiscard]] Result<GpioState> get() const noexcept
	{
		return input_.get();
	}

	/** Enable interrupt wakeups for a logical edge (active-low is respected). */
	[[nodiscard]] Result<> enable_interrupts(GpioEdge edge) noexcept;

	/** Disable interrupt wakeups and discard any pending wakeup. */
	void disable_interrupts() noexcept;

	/** Wait in thread context for an enabled edge interrupt. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> wait(std::chrono::duration<Rep, Period> timeout) noexcept
	{
		if (!interrupts_enabled_) {
			return fail(errors::not_connected);
		}
		return activity_.take(timeout);
	}

	[[nodiscard]] constexpr bool interrupts_enabled() const noexcept
	{
		return interrupts_enabled_;
	}

	[[nodiscard]] constexpr const gpio_dt_spec &native_spec() const noexcept
	{
		return input_.native_spec();
	}

      private:
	static void interrupt_callback(const struct device *, gpio_callback *callback,
				       gpio_port_pins_t) noexcept;

	GpioInput input_;
	gpio_callback callback_{};
	Semaphore activity_{0U, 1U};
	bool interrupts_enabled_{false};
};

/**
 * A devicetree-configured digital output.
 *
 * The object tracks the last state it drove, so `state()` is exact and cannot
 * fail. It does not read the pin: `gpio_pin_get_dt()` is for *input* pins, and on
 * most SoCs an output has its input buffer disabled, so the read returns a
 * constant rather than an error.
 *
 * Use `read_pin()` when the electrical level genuinely matters; it requires the
 * pin to have been configured for readback --- see `init()`.
 *
 * `set()` and `toggle()` are non-const, since they update the tracked state.
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
