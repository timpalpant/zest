/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/gpio.hpp>
#include <zest/kernel.hpp>
#include <zest/timing.hpp>

#include <zephyr/drivers/gpio.h>

#include <chrono>
#include <cstdint>

namespace zest
{

/** Timing policy for a debounced button. */
struct ButtonConfig {
	std::chrono::milliseconds debounce{30};
	std::chrono::milliseconds long_press{1000};
};

/** Events observed during one button poll. Multiple bits may be set. */
enum class ButtonEvent : std::uint8_t {
	none = 0U,
	pressed = 1U << 0U,
	released = 1U << 1U,
	clicked = 1U << 2U,
	long_pressed = 1U << 3U,
};

[[nodiscard]] constexpr ButtonEvent operator|(ButtonEvent lhs, ButtonEvent rhs) noexcept
{
	return static_cast<ButtonEvent>(static_cast<std::uint8_t>(lhs) |
					static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool has_event(ButtonEvent events, ButtonEvent event) noexcept
{
	return (static_cast<std::uint8_t>(events) & static_cast<std::uint8_t>(event)) != 0U;
}

/** Current stable state and any transitions produced by a button poll. */
struct ButtonUpdate {
	GpioState state;
	ButtonEvent events;
};

/**
 * Poll-driven debounced button with click and long-press recognition.
 *
 * Debouncing and event decoding happen in thread context; the interrupt only
 * signals a semaphore, so no application code runs in ISR context.
 */
template <typename Clock = std::chrono::steady_clock> class Button
{
      public:
	using clock = Clock;
	using time_point = typename clock::time_point;

	Button(gpio_dt_spec input, ButtonConfig config = {}) noexcept
		: input_{input}, config_{config}, debouncer_{config.debounce}
	{
	}
	~Button() noexcept
	{
		disable_interrupts();
	}
	Button(const Button &) = delete;
	Button &operator=(const Button &) = delete;

	/** Configure and sample the input to establish its initial stable state. */
	Result<> init(time_point now = clock::now()) noexcept
	{
		ZEST_TRY(input_.init());
		ZEST_TRY_ASSIGN(state, input_.get());

		const bool active = state == GpioState::active;
		debouncer_.reset(active);
		pressed_at_ = now;
		long_press_sent_ = active && config_.long_press.count() == 0;
		initialized_ = true;
		return {};
	}

	/** Enable both-edge wakeups. Events are still decoded by poll(). */
	Result<> enable_interrupts() noexcept
	{
		if (!initialized_) {
			return fail(errors::not_connected);
		}
		if (interrupts_enabled_) {
			return {};
		}
		const auto &spec = input_.native_spec();
		gpio_init_callback(&callback_, interrupt_callback, BIT(spec.pin));
		ZEST_TRY(check(gpio_add_callback(spec.port, &callback_)));

		if (const int rc = gpio_pin_interrupt_configure_dt(&spec, GPIO_INT_EDGE_BOTH);
		    rc < 0) {
			(void)gpio_remove_callback(spec.port, &callback_);
			return fail(rc);
		}
		interrupts_enabled_ = true;
		return {};
	}

	void disable_interrupts() noexcept
	{
		if (!interrupts_enabled_) {
			return;
		}
		const auto &spec = input_.native_spec();
		(void)gpio_pin_interrupt_configure_dt(&spec, GPIO_INT_DISABLE);
		(void)gpio_remove_callback(spec.port, &callback_);
		interrupts_enabled_ = false;
	}

	/** Poll the GPIO and report any debounced events. */
	Result<ButtonUpdate> poll(time_point now = clock::now()) noexcept
	{
		if (!initialized_) {
			return fail(errors::not_connected);
		}
		ZEST_TRY_ASSIGN(state, input_.get());

		const auto debounced = debouncer_.update(state == GpioState::active, now);
		ButtonEvent events = ButtonEvent::none;

		if (debounced.changed) {
			if (debounced.value) {
				pressed_at_ = now;
				long_press_sent_ = false;
				events = ButtonEvent::pressed;
			} else {
				events = ButtonEvent::released;
				if (!long_press_sent_) {
					events = events | ButtonEvent::clicked;
				}
			}
		}

		if (debounced.value && !long_press_sent_ &&
		    now - pressed_at_ >= config_.long_press) {
			long_press_sent_ = true;
			events = events | ButtonEvent::long_pressed;
		}

		return ButtonUpdate{debounced.value ? GpioState::active : GpioState::inactive,
				    events};
	}

	/**
	 * Wait in thread context for any button event.
	 *
	 * A `milliseconds::max()` timeout waits indefinitely. When interrupts are
	 * enabled the wait sleeps on the edge semaphore and only polls to re-check
	 * timing; otherwise it polls at @p poll_interval.
	 */
	Result<ButtonUpdate>
	wait(std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
	     std::chrono::milliseconds poll_interval = std::chrono::milliseconds{5}) noexcept
	{
		if (poll_interval <= std::chrono::milliseconds::zero()) {
			return fail(errors::invalid_argument);
		}
		const auto started = clock::now();
		const bool forever = timeout == std::chrono::milliseconds::max();

		for (;;) {
			ZEST_TRY_ASSIGN(update, poll(clock::now()));
			if (update.events != ButtonEvent::none) {
				return update;
			}
			if (!forever && clock::now() - started >= timeout) {
				return fail(errors::timed_out);
			}
			if (interrupts_enabled_) {
				(void)activity_.take(poll_interval);
			} else {
				sleep_for(poll_interval);
			}
		}
	}

	[[nodiscard]] constexpr bool interrupts_enabled() const noexcept
	{
		return interrupts_enabled_;
	}

      private:
	static void interrupt_callback(const struct device *, gpio_callback *callback,
				       gpio_port_pins_t) noexcept
	{
		auto *self = CONTAINER_OF(callback, Button, callback_);
		self->activity_.give();
	}

	GpioInput input_;
	ButtonConfig config_;
	Debouncer<bool, Clock> debouncer_;
	time_point pressed_at_{};
	bool initialized_{false};
	bool long_press_sent_{false};
	gpio_callback callback_{};
	Semaphore activity_{0U, 1U};
	bool interrupts_enabled_{false};
};

} /* namespace zest */
