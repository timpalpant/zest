/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/i2c.hpp>

#include <cstdint>

namespace zest
{

/**
 * An eight-bit output-only I2C GPIO port for registerless expanders.
 *
 * The software shadow is intentional: devices such as the PCF8574 read the
 * electrical state of their pins, which is not a reliable copy of their output
 * latches. Applications that need masked changes must use update(), never a
 * bus read followed by a write.
 */
class I2cGpioExpander
{
      public:
	constexpr explicit I2cGpioExpander(i2c_dt_spec spec) noexcept : device_{spec}
	{
	}

	/** Verify the bus is ready before use. */
	[[nodiscard]] Result<> init() noexcept;
	/** Probe the device by reading its eight pin levels. */
	[[nodiscard]] Result<std::uint8_t> probe() const noexcept;
	/** Write the complete output port and update the shadow on success. */
	[[nodiscard]] Result<> write(std::uint8_t state) noexcept;
	/** Update only @p mask bits in the output shadow with @p value. */
	[[nodiscard]] Result<> update(std::uint8_t mask, std::uint8_t value) noexcept;

	[[nodiscard]] constexpr std::uint8_t shadow() const noexcept
	{
		return shadow_;
	}
	[[nodiscard]] constexpr std::uint16_t address() const noexcept
	{
		return device_.address();
	}

      private:
	[[nodiscard]] Result<> require_initialized() const noexcept;

	I2cDevice device_;
	std::uint8_t shadow_{0};
	bool initialized_{false};
};

} /* namespace zest */
