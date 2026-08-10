#pragma once

#include <zest/gpio.hpp>
#include <zest/timing.hpp>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <expected>

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

/** Poll-driven debounced button with click and long-press recognition. */
template <typename Clock = std::chrono::steady_clock> class Button
{
      public:
	using clock = Clock;
	using time_point = typename clock::time_point;

	Button(gpio_dt_spec input, ButtonConfig config = {}) noexcept
		: input_{input}, config_{config}, debouncer_{config.debounce}
	{
		k_sem_init(&activity_, 0, 1);
	}
	~Button() noexcept
	{
		if (interrupts_enabled_) {
			(void)gpio_remove_callback(input_.native_spec().port, &callback_);
		}
	}
	Button(const Button &) = delete;
	Button &operator=(const Button &) = delete;

	/** Configure and sample the input to establish its initial stable state. */
	[[nodiscard]] std::expected<void, int> init(time_point now = clock::now()) noexcept
	{
		if (const auto ready = input_.init(); !ready) {
			return ready;
		}
		const auto state = input_.get();
		if (!state) {
			return std::unexpected(state.error());
		}

		const bool active = *state == GpioState::active;
		debouncer_.reset(active);
		pressed_at_ = now;
		long_press_sent_ = active && config_.long_press.count() == 0;
		initialized_ = true;
		return {};
	}

	/** Enable both-edge wakeups. Events remain decoded by poll() in thread context. */
	[[nodiscard]] std::expected<void, int> enable_interrupts() noexcept
	{
		if (!initialized_) {
			return std::unexpected(-EACCES);
		}
		if (interrupts_enabled_) {
			return {};
		}
		const auto &spec = input_.native_spec();
		gpio_init_callback(&callback_, interrupt_callback, BIT(spec.pin));
		int rc = gpio_add_callback(spec.port, &callback_);
		if (rc < 0) {
			return std::unexpected(rc);
		}
		rc = gpio_pin_interrupt_configure_dt(&spec, GPIO_INT_EDGE_BOTH);
		if (rc < 0) {
			(void)gpio_remove_callback(spec.port, &callback_);
			return std::unexpected(rc);
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
	[[nodiscard]] std::expected<ButtonUpdate, int> poll(time_point now = clock::now()) noexcept
	{
		if (!initialized_) {
			return std::unexpected(-EACCES);
		}

		const auto state = input_.get();
		if (!state) {
			return std::unexpected(state.error());
		}

		const auto debounced = debouncer_.update(*state == GpioState::active, now);
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

	/** Wait in thread context for any button event, using bounded polling. */
	[[nodiscard]] std::expected<ButtonUpdate, int>
	wait(std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
	     std::chrono::milliseconds poll_interval = std::chrono::milliseconds{5}) noexcept
	{
		if (poll_interval <= std::chrono::milliseconds::zero()) {
			return std::unexpected(-EINVAL);
		}
		const auto started = clock::now();
		for (;;) {
			auto update = poll(clock::now());
			if (!update || update->events != ButtonEvent::none) {
				return update;
			}
			if (timeout != std::chrono::milliseconds::max() &&
			    clock::now() - started >= timeout) {
				return std::unexpected(-ETIMEDOUT);
			}
			if (interrupts_enabled_) {
				(void)k_sem_take(&activity_, K_MSEC(poll_interval.count()));
			} else {
				k_sleep(K_MSEC(poll_interval.count()));
			}
		}
	}

      private:
	static void interrupt_callback(const struct device *, gpio_callback *callback,
				       gpio_port_pins_t) noexcept
	{
		auto *self = CONTAINER_OF(callback, Button, callback_);
		k_sem_give(&self->activity_);
	}

	GpioInput input_;
	ButtonConfig config_;
	Debouncer<bool, Clock> debouncer_;
	time_point pressed_at_{};
	bool initialized_{false};
	bool long_press_sent_{false};
	gpio_callback callback_{};
	k_sem activity_{};
	bool interrupts_enabled_{false};
};

} /* namespace zest */
