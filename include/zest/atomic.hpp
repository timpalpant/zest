/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zephyr/sys/atomic.h>

#include <type_traits>

namespace zest
{

/**
 * A type-safe wrapper around Zephyr's `atomic_t`.
 *
 * `std::atomic<T>` needs the compiler to prove `T` is lock-free, which for
 * anything the target has no native instruction for falls back to libatomic
 * --- not available in Zephyr's minimal C library. Zephyr ships its own
 * atomic implementation for every architecture it supports instead: a
 * handful of instructions where the core has them, an interrupt-locked
 * fallback where it does not, and always safe to call from an ISR. This wraps
 * that API in the shape an `std::atomic` user expects, for any enum or
 * integral type narrow enough to fit in one.
 */
template <typename T>
	requires((std::is_enum_v<T> || std::is_integral_v<T>) && sizeof(T) <= sizeof(atomic_val_t))
class Atomic
{
      public:
	using value_type = T;

	constexpr Atomic() noexcept = default;
	constexpr explicit Atomic(T initial) noexcept : raw_{to_raw(initial)}
	{
	}
	Atomic(const Atomic &) = delete;
	Atomic &operator=(const Atomic &) = delete;

	/** Safe to call from an ISR. */
	[[nodiscard]] T load() const noexcept
	{
		return from_raw(atomic_get(&raw_));
	}

	/** Safe to call from an ISR. */
	void store(T value) noexcept
	{
		(void)atomic_set(&raw_, to_raw(value));
	}

	/** Store @p value, returning the value it replaced. Safe to call from an ISR. */
	[[nodiscard]] T exchange(T value) noexcept
	{
		return from_raw(atomic_set(&raw_, to_raw(value)));
	}

	/** Replace with @p value only if the current value is @p expected. */
	[[nodiscard]] bool compare_exchange_strong(T expected, T value) noexcept
	{
		return atomic_cas(&raw_, to_raw(expected), to_raw(value));
	}

	/** Bitwise-OR @p bits into the value, returning the value it replaced. */
	T fetch_or(T bits) noexcept
	{
		return from_raw(atomic_or(&raw_, to_raw(bits)));
	}

	/** Bitwise-AND @p bits into the value, returning the value it replaced. */
	T fetch_and(T bits) noexcept
	{
		return from_raw(atomic_and(&raw_, to_raw(bits)));
	}

      private:
	[[nodiscard]] static constexpr atomic_val_t to_raw(T value) noexcept
	{
		return static_cast<atomic_val_t>(value);
	}
	[[nodiscard]] static constexpr T from_raw(atomic_val_t value) noexcept
	{
		return static_cast<T>(value);
	}

	atomic_t raw_{};
};

} /* namespace zest */
