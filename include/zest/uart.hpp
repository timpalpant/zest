/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/kernel.hpp>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zest
{

/**
 * Polled access to a UART.
 *
 * Many sensors and radio modules speak ASCII or a simple framed protocol over a
 * serial line and have no Zephyr driver at all. This is deliberately the polling
 * API: it needs no interrupt configuration, no ring buffer sizing and no Kconfig
 * beyond `CONFIG_SERIAL`, which makes it usable from a plain thread.
 *
 * `read()` polls with a bounded wait, so it cannot hang a thread the way a raw
 * `uart_poll_in()` loop does.
 */
class Uart
{
      public:
	constexpr explicit Uart(const struct device *device) noexcept : device_{device}
	{
	}

	/** Verify the device is present and ready. */
	Result<> init() const noexcept
	{
		if (device_ == nullptr || !device_is_ready(device_)) {
			return fail(errors::no_device);
		}
		return {};
	}

	/** Apply a line configuration at run time, where the driver supports it. */
	Result<>
	configure(std::uint32_t baud_rate, std::uint8_t data_bits = UART_CFG_DATA_BITS_8,
		  std::uint8_t parity = UART_CFG_PARITY_NONE,
		  std::uint8_t stop_bits = UART_CFG_STOP_BITS_1,
		  std::uint8_t flow_control = UART_CFG_FLOW_CTRL_NONE) const noexcept
	{
		ZEST_TRY(init());
		const uart_config config{
			.baudrate = baud_rate,
			.parity = parity,
			.stop_bits = stop_bits,
			.data_bits = data_bits,
			.flow_ctrl = flow_control,
		};
		return check(uart_configure(device_, &config));
	}

	/** Send every byte, blocking until the driver accepts it. */
	Result<> write(std::span<const std::byte> data) const noexcept
	{
		ZEST_TRY(init());
		for (const std::byte value : data) {
			uart_poll_out(device_, static_cast<unsigned char>(value));
		}
		return {};
	}

	/** Send text. */
	Result<> write(std::string_view text) const noexcept
	{
		return write(std::as_bytes(std::span{text.data(), text.size()}));
	}

	/** Read one byte, waiting up to @p timeout. */
	Result<std::byte> read_byte(std::chrono::milliseconds timeout) const noexcept
	{
		ZEST_TRY(init());
		const auto deadline = uptime() + timeout;
		for (;;) {
			unsigned char value = 0U;
			const int rc = uart_poll_in(device_, &value);
			if (rc == 0) {
				return std::byte{value};
			}
			if (rc != -1 && rc != -EAGAIN) {
				return fail(rc);
			}
			if (uptime() >= deadline) {
				return fail(errors::timed_out);
			}
			sleep_for(std::chrono::milliseconds{1});
		}
	}

	/**
	 * Read up to @p destination.size() bytes, returning what arrived.
	 *
	 * Stops early on timeout rather than failing, so a short frame is still
	 * delivered. An empty result means nothing arrived at all.
	 */
	Result<std::span<std::byte>>
	read(std::span<std::byte> destination, std::chrono::milliseconds timeout) const noexcept
	{
		ZEST_TRY(init());
		const auto deadline = uptime() + timeout;
		std::size_t received = 0U;

		while (received < destination.size()) {
			unsigned char value = 0U;
			const int rc = uart_poll_in(device_, &value);
			if (rc == 0) {
				destination[received++] = std::byte{value};
				continue;
			}
			if (rc != -1 && rc != -EAGAIN) {
				return fail(rc);
			}
			if (uptime() >= deadline) {
				break;
			}
			sleep_for(std::chrono::milliseconds{1});
		}
		return destination.first(received);
	}

	/**
	 * Read until @p terminator, returning the line without it.
	 *
	 * The common shape for a module that answers in ASCII lines.
	 */
	Result<std::string_view> read_line(std::span<char> destination,
							 std::chrono::milliseconds timeout,
							 char terminator = '\n') const noexcept
	{
		ZEST_TRY(init());
		const auto deadline = uptime() + timeout;
		std::size_t received = 0U;

		while (received < destination.size()) {
			unsigned char value = 0U;
			const int rc = uart_poll_in(device_, &value);
			if (rc == 0) {
				if (static_cast<char>(value) == terminator) {
					return std::string_view{destination.data(), received};
				}
				destination[received++] = static_cast<char>(value);
				continue;
			}
			if (rc != -1 && rc != -EAGAIN) {
				return fail(rc);
			}
			if (uptime() >= deadline) {
				return fail(errors::timed_out);
			}
			sleep_for(std::chrono::milliseconds{1});
		}
		return fail(errors::no_buffer_space);
	}

	/** Discard anything already buffered. */
	void flush_input() const noexcept
	{
		if (device_ == nullptr) {
			return;
		}
		unsigned char value = 0U;
		while (uart_poll_in(device_, &value) == 0) {
		}
	}

	[[nodiscard]] const struct device *native_handle() const noexcept
	{
		return device_;
	}

      private:
	const struct device *device_{};
};

} /* namespace zest */
