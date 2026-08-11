/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zest
{

/** Caller-placed retained storage, suitable for a no-init or retention section. */
template <typename T>
	requires std::is_trivially_copyable_v<T>
struct RetainedStorage {
	std::uint32_t magic;
	std::uint32_t checksum;
	T value;
};

/**
 * Integrity-checked access to retained, caller-owned storage.
 *
 * `store()` writes the payload, then its checksum, then the magic word, in that
 * order, so a reset partway through leaves the record invalid rather than
 * plausible.
 */
template <typename T>
	requires std::is_trivially_copyable_v<T>
class RetainedValue
{
      public:
	constexpr explicit RetainedValue(RetainedStorage<T> &storage,
					 std::uint32_t magic = 0x5a455354U) noexcept
		: storage_{storage}, magic_{magic}
	{
	}

	[[nodiscard]] bool valid() const noexcept
	{
		return storage_.magic == magic_ && storage_.checksum == checksum(storage_.value);
	}

	[[nodiscard]] T value_or(T fallback) const noexcept
	{
		return valid() ? storage_.value : fallback;
	}

	void store(const T &value) noexcept
	{
		storage_.value = value;
		storage_.checksum = checksum(storage_.value);
		/* The magic word must be the last write to become visible. */
		std::atomic_signal_fence(std::memory_order_release);
		std::atomic_thread_fence(std::memory_order_release);
		storage_.magic = magic_;
	}

	void clear() noexcept
	{
		storage_.magic = 0U;
		std::atomic_signal_fence(std::memory_order_release);
		storage_.checksum = 0U;
	}

      private:
	static std::uint32_t checksum(const T &value) noexcept
	{
		/* FNV-1a over the object representation, padding included. */
		std::uint32_t hash = 2166136261U;
		const auto *bytes = reinterpret_cast<const unsigned char *>(&value);
		for (std::size_t index = 0; index < sizeof(T); ++index) {
			hash = (hash ^ bytes[index]) * 16777619U;
		}
		return hash;
	}

	RetainedStorage<T> &storage_;
	std::uint32_t magic_;
};

} /* namespace zest */
