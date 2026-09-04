/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * Access to a fixed flash partition by its devicetree id.
 */

#include <zest/error.hpp>

#include <zephyr/storage/flash_map.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace zest
{

/**
 * An open handle to one fixed partition.
 *
 * Read-mostly assets — a sound bank, a lookup table, a font, a firmware slot's
 * header — live in their own partition rather than in the image, and Zephyr
 * reaches them through `flash_area_open()`/`flash_area_close()`. That pairing is
 * the usual leak, and the usual reason `struct flash_area` ends up
 * forward-declared in an application header just to hold the pointer between the
 * two calls.
 *
 * The handle closes itself, bounds every access against the partition size
 * rather than trusting the caller's offset, and reports a short read as an error
 * instead of leaving the tail of the destination undefined.
 *
 * Move-only, and not synchronized: Zephyr's flash drivers serialize at the
 * device, but an offset argument is the caller's, so two threads sharing one
 * handle is fine while two threads sharing a *cursor* built on it is not.
 */
class FlashPartition
{
      public:
	FlashPartition() noexcept = default;

	/**
	 * Name the partition without opening it.
	 *
	 * Take @p id from `FIXED_PARTITION_ID(label)`, so a partition that is not
	 * in the devicetree is a build error rather than a run-time surprise.
	 */
	constexpr explicit FlashPartition(std::uint8_t id) noexcept : id_{id}
	{
	}

	FlashPartition(FlashPartition &&other) noexcept
		: area_{std::exchange(other.area_, nullptr)}, id_{other.id_}
	{
	}

	FlashPartition &operator=(FlashPartition &&other) noexcept
	{
		if (this != &other) {
			close();
			area_ = std::exchange(other.area_, nullptr);
			id_ = other.id_;
		}
		return *this;
	}

	FlashPartition(const FlashPartition &) = delete;
	FlashPartition &operator=(const FlashPartition &) = delete;

	~FlashPartition() noexcept
	{
		close();
	}

	/** Open the partition. Opening an already-open handle is a no-op. */
	[[nodiscard]] Result<> open() noexcept;

	/** Close it. Idempotent, and called by the destructor. */
	void close() noexcept;

	[[nodiscard]] bool is_open() const noexcept
	{
		return area_ != nullptr;
	}

	/** Total size of the partition in bytes. */
	[[nodiscard]] Result<std::size_t> size() const noexcept;

	/** Where the partition starts within its flash device. */
	[[nodiscard]] Result<std::size_t> offset() const noexcept;

	/**
	 * Fill @p destination from @p offset within the partition.
	 *
	 * A read that would run past the end of the partition is rejected rather
	 * than truncated, because a caller that got a short read and no error goes
	 * on to parse whatever was already in the buffer.
	 */
	[[nodiscard]] Result<> read(std::size_t offset,
				    std::span<std::byte> destination) const noexcept;

	/**
	 * Read a trivially copyable value from @p offset.
	 *
	 * The bytes are taken as they are stored; nothing is byte-swapped, so a
	 * value written by a different-endian host needs converting by the caller.
	 */
	template <typename T>
		requires std::is_trivially_copyable_v<T>
	[[nodiscard]] Result<T> read_value(std::size_t offset) const noexcept
	{
		T value{};
		ZEST_TRY(read(offset, std::as_writable_bytes(std::span{&value, 1})));
		return value;
	}

	/**
	 * Erase @p length bytes at @p offset.
	 *
	 * Both must land on the device's erase-block boundaries; the driver
	 * reports it when they do not.
	 */
	[[nodiscard]] Result<> erase(std::size_t offset, std::size_t length) noexcept;

	/**
	 * Write @p data at @p offset, which must already be erased.
	 *
	 * Flash writes clear bits and cannot set them, so writing over live data
	 * silently produces the AND of the two rather than failing.
	 */
	[[nodiscard]] Result<> write(std::size_t offset, std::span<const std::byte> data) noexcept;

	[[nodiscard]] constexpr std::uint8_t id() const noexcept
	{
		return id_;
	}

	/** The open area, or null. For the Zephyr APIs that take one directly. */
	[[nodiscard]] const struct flash_area *native_area() const noexcept
	{
		return area_;
	}

      private:
	/** Reject an access that would run off the end before issuing it. */
	[[nodiscard]] Result<> check_range(std::size_t offset, std::size_t length) const noexcept;

	const struct flash_area *area_{};
	std::uint8_t id_{};
};

} /* namespace zest */
