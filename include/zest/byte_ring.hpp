/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * A byte-oriented ring buffer with zero-copy claim/finish access.
 */

#include <zest/error.hpp>

#include <zephyr/sys/ring_buffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zest
{

/**
 * A fixed-capacity ring of bytes.
 *
 * @ref SpscRingBuffer is the right buffer for a stream of *values* — samples,
 * events, messages — and its `try_push(const T &)` shape says so. It is the
 * wrong one for hardware, because a UART FIFO drain or a DMA descriptor wants to
 * write *into the buffer's own storage* and then say how much it managed, rather
 * than to hand over an already-assembled element. Copying into a scratch buffer
 * first, to then copy again into the ring, doubles the work on the exact path
 * that has the least time.
 *
 * So this is the byte-oriented sibling: @ref claim_put hands out the contiguous
 * region at the head and @ref finish_put commits however much was actually
 * written, with the matching pair on the consumer side.
 *
 * **Concurrency contract.** Zephyr's `ring_buf` is safe for exactly one producer
 * and one consumer running concurrently, which is what an ISR-and-thread pair
 * is. A claim and its finish are one operation and must not be interleaved with
 * another on the same side.
 *
 * Claims return the contiguous run only, so a claim near the wrap point comes
 * back short. That is not an error and not a full buffer — call again after
 * finishing to get the rest.
 */
template <std::size_t Capacity>
	requires(Capacity > 0U)
class ByteRing
{
      public:
	ByteRing() noexcept
	{
		ring_buf_init(&ring_, static_cast<std::uint32_t>(Capacity), storage_.data());
	}

	ByteRing(const ByteRing &) = delete;
	ByteRing &operator=(const ByteRing &) = delete;

	[[nodiscard]] static constexpr std::size_t capacity() noexcept
	{
		return Capacity;
	}

	/** Bytes currently buffered. */
	[[nodiscard]] std::size_t size() noexcept
	{
		return ring_buf_size_get(&ring_);
	}

	/** Bytes that would be accepted right now. */
	[[nodiscard]] std::size_t space() noexcept
	{
		return ring_buf_space_get(&ring_);
	}

	[[nodiscard]] bool empty() noexcept
	{
		return ring_buf_is_empty(&ring_);
	}

	/** Discard everything. Needs both sides in one context. */
	void reset() noexcept
	{
		ring_buf_reset(&ring_);
	}

	/**
	 * Copy @p data in, returning how many bytes were accepted.
	 *
	 * A short return means the ring filled; nothing is overwritten.
	 */
	[[nodiscard]] std::size_t put(std::span<const std::byte> data) noexcept
	{
		if (data.empty()) {
			return 0U;
		}
		return ring_buf_put(&ring_, reinterpret_cast<const std::uint8_t *>(data.data()),
				    static_cast<std::uint32_t>(data.size()));
	}

	/** Copy out into @p destination, returning how many bytes were removed. */
	[[nodiscard]] std::size_t get(std::span<std::byte> destination) noexcept
	{
		if (destination.empty()) {
			return 0U;
		}
		return ring_buf_get(&ring_, reinterpret_cast<std::uint8_t *>(destination.data()),
				    static_cast<std::uint32_t>(destination.size()));
	}

	/**
	 * Claim up to @p maximum contiguous writable bytes.
	 *
	 * The span is valid until the matching @ref finish_put. Writing to it does
	 * not publish anything: the bytes become visible to the consumer only when
	 * the claim is finished, which is what lets a partially filled claim be
	 * committed at its true length.
	 */
	[[nodiscard]] std::span<std::byte> claim_put(std::size_t maximum) noexcept
	{
		std::uint8_t *data = nullptr;
		const std::uint32_t claimed =
			ring_buf_put_claim(&ring_, &data, static_cast<std::uint32_t>(maximum));
		if (claimed == 0U || data == nullptr) {
			return {};
		}
		return {reinterpret_cast<std::byte *>(data), claimed};
	}

	/**
	 * Commit @p written bytes of the outstanding put claim.
	 *
	 * Committing more than was claimed is rejected rather than accepted, so a
	 * miscounted FIFO drain fails here instead of publishing whatever the ring
	 * happened to hold beyond it. Pass zero to abandon the claim.
	 */
	[[nodiscard]] Result<> finish_put(std::size_t written) noexcept
	{
		return check(ring_buf_put_finish(&ring_, static_cast<std::uint32_t>(written)));
	}

	/**
	 * Claim up to @p maximum contiguous readable bytes without removing them.
	 *
	 * The span is valid until the matching @ref finish_get.
	 */
	[[nodiscard]] std::span<const std::byte> claim_get(std::size_t maximum) noexcept
	{
		std::uint8_t *data = nullptr;
		const std::uint32_t claimed =
			ring_buf_get_claim(&ring_, &data, static_cast<std::uint32_t>(maximum));
		if (claimed == 0U || data == nullptr) {
			return {};
		}
		return {reinterpret_cast<const std::byte *>(data), claimed};
	}

	/**
	 * Remove @p consumed bytes of the outstanding get claim.
	 *
	 * Pass zero to put the whole claim back — the shape a transmit ISR needs
	 * when the hardware FIFO took nothing.
	 */
	[[nodiscard]] Result<> finish_get(std::size_t consumed) noexcept
	{
		return check(ring_buf_get_finish(&ring_, static_cast<std::uint32_t>(consumed)));
	}

	[[nodiscard]] ring_buf *native_handle() noexcept
	{
		return &ring_;
	}

      private:
	ring_buf ring_{};
	std::array<std::uint8_t, Capacity> storage_{};
};

} /* namespace zest */
