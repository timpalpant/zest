/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * Interrupt-driven, ring-buffered access to a UART.
 */

#include <zest/byte_ring.hpp>
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
 * A UART with interrupt-driven receive and transmit.
 *
 * @ref Uart is the polling API, and it is the right one for a sensor that
 * answers a command every few hundred milliseconds. It is the wrong one for a
 * link that carries a continuous stream: polling either burns a thread or misses
 * bytes, and neither is recoverable at a rate where the FIFO fills between
 * scheduling quanta.
 *
 * This is the interrupt shape. Received bytes land in a ring from the ISR and a
 * thread drains them at its own pace; transmitted bytes go into a second ring
 * and the ISR feeds the FIFO from it. Both use @ref ByteRing's claim/finish
 * access, so the ISR moves bytes straight between the FIFO and the ring without
 * a bounce buffer.
 *
 * Sizing is the caller's: the receive ring must cover the longest gap between
 * drains at the line rate, and the transmit ring the largest burst the producer
 * writes at once.
 *
 * Needs `CONFIG_UART_INTERRUPT_DRIVEN`.
 */
template <std::size_t ReceiveCapacity, std::size_t TransmitCapacity>
	requires(ReceiveCapacity > 0U && TransmitCapacity > 0U)
class BufferedUart
{
      public:
	constexpr explicit BufferedUart(const struct device *device) noexcept : device_{device}
	{
	}

	BufferedUart(const BufferedUart &) = delete;
	BufferedUart &operator=(const BufferedUart &) = delete;

	~BufferedUart() noexcept
	{
		stop();
	}

	/**
	 * Install the interrupt handler and begin receiving.
	 *
	 * Transmission stays disabled until there is something to send: leaving the
	 * TX interrupt enabled on an empty ring produces a continuous interrupt
	 * storm, because "the FIFO has room" is always true.
	 */
	[[nodiscard]] Result<> start() noexcept
	{
		if (device_ == nullptr || !device_is_ready(device_)) {
			return fail(errors::no_device);
		}
		if (started_) {
			return fail(errors::already);
		}
		ZEST_TRY(check(uart_irq_callback_user_data_set(device_, &BufferedUart::isr, this)));
		started_ = true;
		uart_irq_rx_enable(device_);
		return {};
	}

	/** Disable both interrupts. Idempotent, and called by the destructor. */
	void stop() noexcept
	{
		if (!started_) {
			return;
		}
		uart_irq_rx_disable(device_);
		uart_irq_tx_disable(device_);
		started_ = false;
	}

	[[nodiscard]] bool started() const noexcept
	{
		return started_;
	}

	/** Bytes waiting to be read. */
	[[nodiscard]] std::size_t available() noexcept
	{
		return receive_.size();
	}

	/**
	 * Take up to `destination.size()` received bytes.
	 *
	 * Never blocks and never fails: an empty return means nothing had arrived.
	 */
	[[nodiscard]] std::size_t read(std::span<std::byte> destination) noexcept
	{
		return receive_.get(destination);
	}

	/**
	 * Wait up to @p timeout for at least one byte, then take what is there.
	 *
	 * Polls in @p poll_interval slices, because Zephyr's interrupt-driven UART
	 * API has no completion object to wait on.
	 */
	[[nodiscard]] std::size_t
	read(std::span<std::byte> destination, std::chrono::milliseconds timeout,
	     std::chrono::milliseconds poll_interval = std::chrono::milliseconds{1}) noexcept
	{
		if (poll_interval <= std::chrono::milliseconds::zero()) {
			poll_interval = std::chrono::milliseconds{1};
		}
		auto remaining = timeout;
		while (true) {
			if (const std::size_t taken = receive_.get(destination); taken > 0U) {
				return taken;
			}
			if (remaining <= std::chrono::milliseconds::zero()) {
				return 0U;
			}
			const auto slice = remaining < poll_interval ? remaining : poll_interval;
			sleep_for(slice);
			remaining -= slice;
		}
	}

	/**
	 * Queue @p data, returning how many bytes the ring accepted.
	 *
	 * A short return is back-pressure, not an error: the caller decides whether
	 * to drop the rest, retry, or block. Transmission is armed only when
	 * something was actually queued.
	 */
	[[nodiscard]] std::size_t write(std::span<const std::byte> data) noexcept
	{
		const std::size_t queued = transmit_.put(data);
		if (queued > 0U && started_) {
			uart_irq_tx_enable(device_);
		}
		return queued;
	}

	[[nodiscard]] std::size_t write(std::string_view text) noexcept
	{
		return write(std::as_bytes(std::span{text.data(), text.size()}));
	}

	/**
	 * Queue all of @p data, waiting for room as the ISR drains the ring.
	 *
	 * Reports `errors::timed_out` with part of the data queued, which is the
	 * honest outcome — a byte already handed to the ISR cannot be recalled.
	 */
	[[nodiscard]] Result<>
	write_all(std::span<const std::byte> data, std::chrono::milliseconds timeout,
		  std::chrono::milliseconds poll_interval = std::chrono::milliseconds{1}) noexcept
	{
		if (poll_interval <= std::chrono::milliseconds::zero()) {
			poll_interval = std::chrono::milliseconds{1};
		}
		auto remaining = timeout;
		while (!data.empty()) {
			const std::size_t queued = write(data);
			data = data.subspan(queued);
			if (data.empty()) {
				return {};
			}
			if (remaining <= std::chrono::milliseconds::zero()) {
				return fail(errors::timed_out);
			}
			const auto slice = remaining < poll_interval ? remaining : poll_interval;
			sleep_for(slice);
			remaining -= slice;
		}
		return {};
	}

	/** Wait for the transmit ring to empty. The FIFO may still hold bytes. */
	[[nodiscard]] Result<>
	drain(std::chrono::milliseconds timeout,
	      std::chrono::milliseconds poll_interval = std::chrono::milliseconds{1}) noexcept
	{
		if (poll_interval <= std::chrono::milliseconds::zero()) {
			poll_interval = std::chrono::milliseconds{1};
		}
		auto remaining = timeout;
		while (!transmit_.empty()) {
			if (remaining <= std::chrono::milliseconds::zero()) {
				return fail(errors::timed_out);
			}
			const auto slice = remaining < poll_interval ? remaining : poll_interval;
			sleep_for(slice);
			remaining -= slice;
		}
		return {};
	}

	/**
	 * Received bytes dropped because the ring was full.
	 *
	 * An overrun is silent at the wire, so a link that is quietly losing bytes
	 * is invisible without a counter. Never resets itself.
	 */
	[[nodiscard]] std::uint32_t receive_overruns() const noexcept
	{
		return overruns_;
	}

	[[nodiscard]] constexpr const struct device *device() const noexcept
	{
		return device_;
	}

      private:
	static void isr(const struct device *device, void *user_data) noexcept
	{
		auto *self = static_cast<BufferedUart *>(user_data);
		if (self == nullptr) {
			return;
		}
		/* uart_irq_update() latches the pending state for this pass; both
		 * ready checks below are meaningless without it. */
		while (uart_irq_update(device) != 0 && uart_irq_is_pending(device) != 0) {
			if (uart_irq_rx_ready(device) != 0) {
				self->service_receive(device);
			}
			if (uart_irq_tx_ready(device) != 0) {
				self->service_transmit(device);
			}
		}
	}

	void service_receive(const struct device *device) noexcept
	{
		while (true) {
			auto claim = receive_.claim_put(ReceiveCapacity);
			if (claim.empty()) {
				/* No room: drain the FIFO anyway and count what is
				 * lost, because leaving bytes in it re-raises the
				 * interrupt immediately and livelocks the ISR. */
				std::uint8_t discard[16];
				const int dropped =
					uart_fifo_read(device, discard, sizeof(discard));
				if (dropped <= 0) {
					return;
				}
				overruns_ += static_cast<std::uint32_t>(dropped);
				continue;
			}

			const int read = uart_fifo_read(
				device, reinterpret_cast<std::uint8_t *>(claim.data()),
				static_cast<int>(claim.size()));
			const std::size_t written = read > 0 ? static_cast<std::size_t>(read) : 0U;
			(void)receive_.finish_put(written);
			if (read <= 0 || written < claim.size()) {
				/* The FIFO is drained: a short fill means it had less
				 * than the claim, so there is nothing left to wrap to. */
				return;
			}
		}
	}

	void service_transmit(const struct device *device) noexcept
	{
		auto claim = transmit_.claim_get(TransmitCapacity);
		if (claim.empty()) {
			/* Nothing left to send. Disabling here is what keeps "the
			 * FIFO has room" from re-raising this interrupt forever. */
			(void)transmit_.finish_get(0U);
			uart_irq_tx_disable(device);
			return;
		}
		const int written =
			uart_fifo_fill(device, reinterpret_cast<const std::uint8_t *>(claim.data()),
				       static_cast<int>(claim.size()));
		(void)transmit_.finish_get(written > 0 ? static_cast<std::size_t>(written) : 0U);
	}

	const struct device *device_;
	ByteRing<ReceiveCapacity> receive_{};
	ByteRing<TransmitCapacity> transmit_{};
	std::uint32_t overruns_{0U};
	bool started_{false};
};

} /* namespace zest */
