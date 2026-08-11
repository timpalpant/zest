/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

namespace zest
{

/**
 * A lock-free typed ring buffer for one producer and one consumer.
 *
 * This is the buffer an interrupt or sampling callback pushes into and a worker
 * thread drains. `MessageQueue` is a kernel synchronization primitive --- it can
 * block, wakes a waiter on every message, and cannot be inspected without
 * removing --- which makes it the wrong tool for buffering a sample stream.
 * This has no kernel involvement at all: pushing from an ISR costs a couple of
 * loads, a store and a release fence, and never blocks.
 *
 * **Concurrency contract.** Exactly one context may call `try_push()` and
 * exactly one may call `try_pop()`/`drain()`, and they may run concurrently.
 * `push_overwrite()` and `clear()` touch both ends, so they need the two sides to
 * be the same context, or external synchronization.
 *
 * One slot beyond `Capacity` is reserved to distinguish full from empty, so the
 * object holds `Capacity + 1` elements.
 */
template <typename T, std::size_t Capacity>
	requires(std::is_trivially_copyable_v<T> && Capacity > 0U)
class SpscRingBuffer
{
      public:
	using value_type = T;

	SpscRingBuffer() noexcept = default;
	SpscRingBuffer(const SpscRingBuffer &) = delete;
	SpscRingBuffer &operator=(const SpscRingBuffer &) = delete;

	/** Largest number of elements the buffer can hold. */
	[[nodiscard]] static constexpr std::size_t capacity() noexcept
	{
		return Capacity;
	}

	/**
	 * Append @p value. Producer side only.
	 *
	 * Returns false and keeps the buffer unchanged when it is full, so the
	 * caller decides whether to drop the newest sample or the oldest.
	 */
	[[nodiscard]] bool try_push(const T &value) noexcept
	{
		const std::size_t tail = tail_.load(std::memory_order_relaxed);
		const std::size_t next = advance(tail);
		if (next == head_.load(std::memory_order_acquire)) {
			return false;
		}
		storage_[tail] = value;
		tail_.store(next, std::memory_order_release);
		return true;
	}

	/**
	 * Append @p value, discarding the oldest element when full.
	 *
	 * The right policy for telemetry where recent samples matter more than a
	 * complete history. Requires that the producer and consumer be the same
	 * context, or be synchronized externally, because it moves both ends.
	 */
	bool push_overwrite(const T &value) noexcept
	{
		if (try_push(value)) {
			return true;
		}
		(void)try_pop();
		return try_push(value);
	}

	/** Remove and return the oldest element. Consumer side only. */
	[[nodiscard]] std::optional<T> try_pop() noexcept
	{
		const std::size_t head = head_.load(std::memory_order_relaxed);
		if (head == tail_.load(std::memory_order_acquire)) {
			return std::nullopt;
		}
		T value = storage_[head];
		head_.store(advance(head), std::memory_order_release);
		return value;
	}

	/** Read the oldest element without removing it. Consumer side only. */
	[[nodiscard]] std::optional<T> peek() const noexcept
	{
		const std::size_t head = head_.load(std::memory_order_relaxed);
		if (head == tail_.load(std::memory_order_acquire)) {
			return std::nullopt;
		}
		return storage_[head];
	}

	/**
	 * Move as many elements as will fit into @p destination.
	 *
	 * Consumer side only. Returns the number copied, oldest first. Draining in
	 * bulk is how a worker turns a burst of samples into one transmission.
	 */
	[[nodiscard]] std::size_t drain(std::span<T> destination) noexcept
	{
		std::size_t head = head_.load(std::memory_order_relaxed);
		const std::size_t tail = tail_.load(std::memory_order_acquire);
		std::size_t copied = 0U;

		while (copied < destination.size() && head != tail) {
			destination[copied] = storage_[head];
			head = advance(head);
			++copied;
		}
		if (copied != 0U) {
			head_.store(head, std::memory_order_release);
		}
		return copied;
	}

	[[nodiscard]] std::size_t size() const noexcept
	{
		const std::size_t head = head_.load(std::memory_order_acquire);
		const std::size_t tail = tail_.load(std::memory_order_acquire);
		return tail >= head ? tail - head : kSlots - head + tail;
	}

	[[nodiscard]] bool empty() const noexcept
	{
		return head_.load(std::memory_order_acquire) ==
		       tail_.load(std::memory_order_acquire);
	}

	[[nodiscard]] bool full() const noexcept
	{
		return advance(tail_.load(std::memory_order_acquire)) ==
		       head_.load(std::memory_order_acquire);
	}

	/** Number of further elements that will fit. */
	[[nodiscard]] std::size_t available() const noexcept
	{
		return Capacity - size();
	}

	/**
	 * Discard everything. Requires both sides to be quiescent or synchronized.
	 */
	void clear() noexcept
	{
		head_.store(0U, std::memory_order_relaxed);
		tail_.store(0U, std::memory_order_release);
	}

      private:
	static constexpr std::size_t kSlots = Capacity + 1U;

	[[nodiscard]] static constexpr std::size_t advance(std::size_t index) noexcept
	{
		return index + 1U == kSlots ? 0U : index + 1U;
	}

	std::array<T, kSlots> storage_{};
	std::atomic<std::size_t> head_{0U};
	std::atomic<std::size_t> tail_{0U};
};

} /* namespace zest */
