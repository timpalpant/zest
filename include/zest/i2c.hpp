/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/drivers/i2c.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zest
{

/**
 * A devicetree-configured I2C peripheral.
 *
 * Raw register access for a part with no Zephyr driver, following the library's
 * conventions: spans instead of pointer-and-length, `Result` instead of errno,
 * and no allocation.
 *
 * ```cpp
 * constexpr zest::I2cDevice sensor{I2C_DT_SPEC_GET(DT_NODELABEL(my_sensor))};
 *
 * ZEST_TRY(sensor.init());
 * auto who_am_i = sensor.read_register(0x0F);
 * if (!who_am_i) return zest::fail(who_am_i.error());
 * ZEST_TRY(sensor.write_register(0x20, 0x57));
 *
 * std::array<std::byte, 6> measurement{};
 * ZEST_TRY(sensor.read_registers(0x28, measurement));
 * ```
 */
class I2cDevice
{
      public:
	constexpr explicit I2cDevice(i2c_dt_spec spec) noexcept : spec_{spec}
	{
	}

	/** Verify the bus controller is present and ready. */
	[[nodiscard]] Result<> init() const noexcept
	{
		if (spec_.bus == nullptr || !device_is_ready(spec_.bus)) {
			return fail(errors::no_device);
		}
		return {};
	}

	[[nodiscard]] Result<> write(std::span<const std::byte> data) const noexcept
	{
		if (data.empty()) {
			return fail(errors::invalid_argument);
		}
		return check(i2c_write_dt(
			&spec_, reinterpret_cast<const std::uint8_t *>(data.data()), data.size()));
	}

	[[nodiscard]] Result<> read(std::span<std::byte> destination) const noexcept
	{
		if (destination.empty()) {
			return fail(errors::invalid_argument);
		}
		return check(i2c_read_dt(&spec_,
					 reinterpret_cast<std::uint8_t *>(destination.data()),
					 destination.size()));
	}

	/** Write then read in one transaction, without releasing the bus. */
	[[nodiscard]] Result<> write_read(std::span<const std::byte> command,
					  std::span<std::byte> destination) const noexcept
	{
		if (command.empty() || destination.empty()) {
			return fail(errors::invalid_argument);
		}
		return check(i2c_write_read_dt(
			&spec_, reinterpret_cast<const std::uint8_t *>(command.data()),
			command.size(), reinterpret_cast<std::uint8_t *>(destination.data()),
			destination.size()));
	}

	/** Read one eight-bit register. */
	[[nodiscard]] Result<std::uint8_t> read_register(std::uint8_t address) const noexcept
	{
		std::uint8_t value = 0U;
		ZEST_TRY(check(i2c_reg_read_byte_dt(&spec_, address, &value)));
		return value;
	}

	/** Write one eight-bit register. */
	[[nodiscard]] Result<> write_register(std::uint8_t address,
					      std::uint8_t value) const noexcept
	{
		return check(i2c_reg_write_byte_dt(&spec_, address, value));
	}

	/** Read-modify-write one eight-bit register. */
	[[nodiscard]] Result<> update_register(std::uint8_t address, std::uint8_t mask,
					       std::uint8_t value) const noexcept
	{
		return check(i2c_reg_update_byte_dt(&spec_, address, mask, value));
	}

	/** Read a run of registers starting at @p address. */
	[[nodiscard]] Result<> read_registers(std::uint8_t address,
					      std::span<std::byte> destination) const noexcept
	{
		if (destination.empty()) {
			return fail(errors::invalid_argument);
		}
		return check(i2c_burst_read_dt(&spec_, address,
					       reinterpret_cast<std::uint8_t *>(destination.data()),
					       destination.size()));
	}

	/** Write a run of registers starting at @p address. */
	[[nodiscard]] Result<> write_registers(std::uint8_t address,
					       std::span<const std::byte> data) const noexcept
	{
		if (data.empty()) {
			return fail(errors::invalid_argument);
		}
		return check(i2c_burst_write_dt(&spec_, address,
						reinterpret_cast<const std::uint8_t *>(data.data()),
						data.size()));
	}

	/** Read a big-endian sixteen-bit register pair. */
	[[nodiscard]] Result<std::uint16_t> read_register16_be(std::uint8_t address) const noexcept
	{
		std::array<std::byte, 2> raw{};
		ZEST_TRY(read_registers(address, raw));
		return static_cast<std::uint16_t>((static_cast<unsigned>(raw[0]) << 8U) |
						  static_cast<unsigned>(raw[1]));
	}

	/** Read a little-endian sixteen-bit register pair. */
	[[nodiscard]] Result<std::uint16_t> read_register16_le(std::uint8_t address) const noexcept
	{
		std::array<std::byte, 2> raw{};
		ZEST_TRY(read_registers(address, raw));
		return static_cast<std::uint16_t>((static_cast<unsigned>(raw[1]) << 8U) |
						  static_cast<unsigned>(raw[0]));
	}

	[[nodiscard]] constexpr std::uint16_t address() const noexcept
	{
		return spec_.addr;
	}
	[[nodiscard]] constexpr const i2c_dt_spec &native_spec() const noexcept
	{
		return spec_;
	}

      private:
	i2c_dt_spec spec_;
};

} /* namespace zest */
