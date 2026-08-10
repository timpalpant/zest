#pragma once

#include <zephyr/drivers/gpio.h>

#include <expected>

namespace zest
{

/** A logical GPIO state that respects active-low devicetree flags. */
enum class GpioState {
	inactive,
	active,
};

/** A devicetree-configured digital input. */
class GpioInput
{
      public:
	constexpr explicit GpioInput(gpio_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Configure the pin as an input. */
	[[nodiscard]] std::expected<void, int> init() const noexcept;

	/** Read the pin's logical active/inactive state. */
	[[nodiscard]] std::expected<GpioState, int> get() const noexcept;
	[[nodiscard]] constexpr const gpio_dt_spec &native_spec() const noexcept
	{
		return spec_;
	}

      private:
	gpio_dt_spec spec_;
};

/** A devicetree-configured digital output. */
class GpioOutput
{
      public:
	constexpr explicit GpioOutput(gpio_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Configure the pin as an output with a known initial logical state. */
	[[nodiscard]] std::expected<void, int>
	init(GpioState initial = GpioState::inactive) const noexcept;

	/** Set the pin's logical active/inactive state. */
	[[nodiscard]] std::expected<void, int> set(GpioState state) const noexcept;

	/** Toggle the pin's logical state. */
	[[nodiscard]] std::expected<void, int> toggle() const noexcept;

	/** Read back the pin's logical active/inactive state. */
	[[nodiscard]] std::expected<GpioState, int> get() const noexcept;
	[[nodiscard]] constexpr const gpio_dt_spec &native_spec() const noexcept
	{
		return spec_;
	}

      private:
	gpio_dt_spec spec_;
};

} /* namespace zest */
