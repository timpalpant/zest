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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zest
{

/**
 * What the interrupt has been doing, for a link that has gone quiet.
 *
 * A serial link that stops working is nearly always one of four things, and the
 * four are indistinguishable from the application's side: the interrupt is not
 * firing at all, bytes are arriving and being dropped, bytes are queued and the
 * hardware is not taking them, or nothing is arriving. These separate them
 * without a logic analyser.
 *
 * Written only from the interrupt and read from a thread. Each field is a
 * naturally aligned 32-bit word, so a reader sees a whole value even when it
 * races the update; a *set* of fields read together may straddle one interrupt,
 * which is fine for diagnostics and wrong for arithmetic between them.
 */
struct BufferedUartStats {
	/** Interrupt entries. Zero on a link that never comes up at all. */
	std::uint32_t interrupts{0U};
	std::uint32_t bytes_received{0U};
	std::uint32_t bytes_sent{0U};
	/**
	 * Received bytes dropped because the receive ring was full.
	 *
	 * An overrun is silent at the wire, so a link quietly losing bytes is
	 * invisible without this.
	 */
	std::uint32_t receive_overruns{0U};
	/**
	 * Interrupt passes that found the transmitter ready for more bytes.
	 *
	 * Read against @ref transmit_stalls, this separates the two ways a
	 * transmit stops: zero here with bytes queued means the transmit
	 * interrupt is not being raised at all, while a count that climbs
	 * alongside the stalls means it is being raised and the FIFO is refusing
	 * the bytes.
	 */
	std::uint32_t transmit_ready{0U};
	/**
	 * Transmit interrupts where the FIFO accepted nothing.
	 *
	 * A few are normal. A count that climbs while bytes stay queued means
	 * flow control is asserted or the peer has stopped reading.
	 */
	std::uint32_t transmit_stalls{0U};
	/** Most bytes ever queued at once, per direction. */
	std::uint32_t receive_high_water{0U};
	std::uint32_t transmit_high_water{0U};
};

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
		transmitting_ = false;
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
	 * Wait up to @p wait for at least one byte, then take what is there.
	 *
	 * Sleeps on the interrupt rather than polling, so a byte that arrives one
	 * microsecond after the call is delivered immediately.
	 *
	 * The ring is drained before each wait, which is what keeps a partial read
	 * from blocking: the interrupt signals once for a burst, so a reader that
	 * took half of it would otherwise wait for a signal that has already been
	 * consumed.
	 */
	template <typename Rep, typename Period>
	[[nodiscard]] std::size_t read(std::span<std::byte> destination,
				       std::chrono::duration<Rep, Period> wait) noexcept
	{
		const auto deadline =
			uptime() +
			std::chrono::ceil<std::chrono::milliseconds>(
				wait > decltype(wait)::zero() ? wait : decltype(wait)::zero());
		while (true) {
			if (const std::size_t taken = receive_.get(destination); taken > 0U) {
				return taken;
			}
			const auto remaining = deadline - uptime();
			if (remaining <= std::chrono::milliseconds::zero()) {
				return 0U;
			}
			(void)receive_ready_.take(remaining);
		}
	}

	/**
	 * Wait until at least @p bytes of transmit room are free.
	 *
	 * For a producer that must place a whole message or none of it: a message
	 * split by a full ring puts a truncated header on the wire, and the peer
	 * has to resynchronize past it — a worse failure than a cleanly dropped
	 * message.
	 *
	 * The signal is cleared before each re-test, so a wakeup arriving between
	 * the test and the wait is not lost — which would otherwise park the
	 * producer for the whole timeout while room was in fact available.
	 */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> wait_for_space(std::size_t bytes,
					      std::chrono::duration<Rep, Period> wait) noexcept
	{
		if (bytes > TransmitCapacity) {
			return fail(errors::message_size);
		}
		const auto deadline =
			uptime() +
			std::chrono::ceil<std::chrono::milliseconds>(
				wait > decltype(wait)::zero() ? wait : decltype(wait)::zero());
		while (transmit_.space() < bytes) {
			transmit_room_.reset();
			if (transmit_.space() >= bytes) {
				return {};
			}
			const auto remaining = deadline - uptime();
			if (remaining <= std::chrono::milliseconds::zero()) {
				return fail(errors::timed_out);
			}
			ZEST_TRY(transmit_room_.take(remaining));
		}
		return {};
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
		if (queued > 0U) {
			stats_.transmit_high_water =
				std::max(stats_.transmit_high_water,
					 static_cast<std::uint32_t>(transmit_.size()));
			if (started_) {
				transmitting_ = true;
				uart_irq_tx_enable(device_);
			}
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
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> write_all(std::span<const std::byte> data,
					 std::chrono::duration<Rep, Period> wait) noexcept
	{
		const auto deadline =
			uptime() +
			std::chrono::ceil<std::chrono::milliseconds>(
				wait > decltype(wait)::zero() ? wait : decltype(wait)::zero());
		while (!data.empty()) {
			const std::size_t queued = write(data);
			data = data.subspan(queued);
			if (data.empty()) {
				return {};
			}
			const auto remaining = deadline - uptime();
			if (remaining <= std::chrono::milliseconds::zero()) {
				return fail(errors::timed_out);
			}
			ZEST_TRY(wait_for_space(1U, remaining));
		}
		return {};
	}

	/**
	 * Queue @p data whole, or queue none of it.
	 *
	 * Reserves the room first, so a message never goes out truncated. The
	 * caller still serializes its own writes: two threads reserving
	 * concurrently can each see room that the other then takes.
	 */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> write_atomic(std::span<const std::byte> data,
					    std::chrono::duration<Rep, Period> wait) noexcept
	{
		if (data.empty()) {
			return {};
		}
		ZEST_TRY(wait_for_space(data.size(), wait));
		if (write(data) != data.size()) {
			return fail(errors::no_buffer_space);
		}
		return {};
	}

	/** Wait for the transmit ring to empty. The FIFO may still hold bytes. */
	template <typename Rep, typename Period>
	[[nodiscard]] Result<> drain(std::chrono::duration<Rep, Period> wait) noexcept
	{
		return wait_for_space(TransmitCapacity, wait);
	}

	/** Bytes currently queued for transmission. */
	[[nodiscard]] std::size_t pending_transmit_bytes() noexcept
	{
		return transmit_.size();
	}

	/**
	 * Whether the transmit interrupt is armed to drain the ring.
	 *
	 * Worth reporting because the UART API offers no way to ask, and the
	 * answer is the difference between a link that is busy and one that has
	 * stopped for good.
	 */
	[[nodiscard]] bool transmitting() const noexcept
	{
		return transmitting_;
	}

	/** Counters the interrupt keeps. Never reset themselves. */
	[[nodiscard]] const BufferedUartStats &stats() const noexcept
	{
		return stats_;
	}

	void reset_stats() noexcept
	{
		stats_ = BufferedUartStats{};
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
		++self->stats_.interrupts;
		/* uart_irq_update() latches the pending state for this pass; both
		 * ready checks below are meaningless without it. */
		while (uart_irq_update(device) != 0 && uart_irq_is_pending(device) != 0) {
			if (uart_irq_rx_ready(device) != 0) {
				self->service_receive(device);
			}
			if (uart_irq_tx_ready(device) != 0) {
				++self->stats_.transmit_ready;
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
				stats_.receive_overruns += static_cast<std::uint32_t>(dropped);
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
	/** Signalled by the interrupt when bytes have arrived. */
	Semaphore receive_ready_{0U, 1U};
	/** Signalled by the interrupt when transmit room has been freed. */
	Semaphore transmit_room_{0U, 1U};
	BufferedUartStats stats_{};
	/*
	 * Whether the transmit interrupt is enabled, as this class believes.
	 * Written from both the interrupt and the producer, and only ever read as
	 * a hint, so it is volatile rather than atomic.
	 */
	volatile bool transmitting_{false};
	bool started_{false};
};

} /* namespace zest */
