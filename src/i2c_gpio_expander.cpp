/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/i2c_gpio_expander.hpp>

#include <array>
#include <cstddef>

namespace zest
{

Result<> I2cGpioExpander::init() noexcept
{
	ZEST_TRY(device_.init());
	initialized_ = true;
	return {};
}

Result<> I2cGpioExpander::require_initialized() const noexcept
{
	return initialized_ ? Result<>{} : fail(errors::no_device);
}

Result<std::uint8_t> I2cGpioExpander::probe() const noexcept
{
	ZEST_TRY(require_initialized());
	std::array<std::byte, 1> value{};
	ZEST_TRY(device_.read(value));
	return std::to_integer<std::uint8_t>(value[0]);
}

Result<> I2cGpioExpander::write(std::uint8_t state) noexcept
{
	ZEST_TRY(require_initialized());
	const std::array<std::byte, 1> value{static_cast<std::byte>(state)};
	ZEST_TRY(device_.write(value));
	shadow_ = state;
	return {};
}

Result<> I2cGpioExpander::update(std::uint8_t mask, std::uint8_t value) noexcept
{
	return write(static_cast<std::uint8_t>((shadow_ & ~mask) | (value & mask)));
}

} /* namespace zest */
