/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/drivers/spi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zest
{

/**
 * A devicetree-configured SPI peripheral.
 *
 * SPI register conventions vary by part --- the read/write bit may be the top bit,
 * the bottom bit, or a separate command byte --- so this exposes transfers rather
 * than pretending there is one register protocol.
 *
 * ```cpp
 * constexpr zest::SpiDevice imu{SPI_DT_SPEC_GET(DT_NODELABEL(my_imu),
 *                                               SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0)};
 *
 * ZEST_TRY(imu.init());
 *
 * const std::array command{std::byte{0x80 | 0x0F}};   // read bit set
 * std::array<std::byte, 1> id{};
 * ZEST_TRY(imu.write_then_read(command, id));
 * ```
 */
class SpiDevice
{
      public:
	constexpr explicit SpiDevice(spi_dt_spec spec) noexcept : spec_{spec}
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

	/** Clock @p data out, discarding what arrives. */
	[[nodiscard]] Result<> write(std::span<const std::byte> data) const noexcept
	{
		if (data.empty()) {
			return fail(errors::invalid_argument);
		}
		const spi_buf transmit{.buf = const_cast<std::byte *>(data.data()),
				       .len = data.size()};
		const spi_buf_set transmit_set{.buffers = &transmit, .count = 1U};
		return check(spi_write_dt(&spec_, &transmit_set));
	}

	/** Clock zeros out and capture what arrives. */
	[[nodiscard]] Result<> read(std::span<std::byte> destination) const noexcept
	{
		if (destination.empty()) {
			return fail(errors::invalid_argument);
		}
		const spi_buf receive{.buf = destination.data(), .len = destination.size()};
		const spi_buf_set receive_set{.buffers = &receive, .count = 1U};
		return check(spi_read_dt(&spec_, &receive_set));
	}

	/** Full-duplex transfer. The two spans may differ in length. */
	[[nodiscard]] Result<> transceive(std::span<const std::byte> transmit,
					  std::span<std::byte> receive) const noexcept
	{
		if (transmit.empty() && receive.empty()) {
			return fail(errors::invalid_argument);
		}
		const spi_buf transmit_buffer{.buf = const_cast<std::byte *>(transmit.data()),
					      .len = transmit.size()};
		const spi_buf receive_buffer{.buf = receive.data(), .len = receive.size()};
		const spi_buf_set transmit_set{.buffers = &transmit_buffer, .count = 1U};
		const spi_buf_set receive_set{.buffers = &receive_buffer, .count = 1U};

		return check(spi_transceive_dt(&spec_, transmit.empty() ? nullptr : &transmit_set,
					       receive.empty() ? nullptr : &receive_set));
	}

	/**
	 * Send @p command, then capture @p destination, in one chip-select assertion.
	 *
	 * This is the shape most register reads take: the response arrives after the
	 * command bytes rather than alongside them.
	 */
	[[nodiscard]] Result<> write_then_read(std::span<const std::byte> command,
					       std::span<std::byte> destination) const noexcept
	{
		if (command.empty() || destination.empty()) {
			return fail(errors::invalid_argument);
		}
		const std::array<spi_buf, 2> transmit{
			spi_buf{.buf = const_cast<std::byte *>(command.data()),
				.len = command.size()},
			spi_buf{.buf = nullptr, .len = destination.size()},
		};
		const std::array<spi_buf, 2> receive{
			spi_buf{.buf = nullptr, .len = command.size()},
			spi_buf{.buf = destination.data(), .len = destination.size()},
		};
		const spi_buf_set transmit_set{.buffers = transmit.data(),
					       .count = transmit.size()};
		const spi_buf_set receive_set{.buffers = receive.data(), .count = receive.size()};
		return check(spi_transceive_dt(&spec_, &transmit_set, &receive_set));
	}

	/** Release the bus when the configuration holds chip select between transfers. */
	[[nodiscard]] Result<> release() const noexcept
	{
		return check(spi_release_dt(&spec_));
	}

	[[nodiscard]] constexpr const spi_dt_spec &native_spec() const noexcept
	{
		return spec_;
	}

      private:
	spi_dt_spec spec_;
};

} /* namespace zest */
