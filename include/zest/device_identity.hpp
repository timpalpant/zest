/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/drivers/hwinfo.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zest
{

/** Hardware reset-cause snapshot. */
struct RebootReason {
	std::uint32_t causes{};
	std::uint32_t supported{};

	[[nodiscard]] constexpr bool has(std::uint32_t cause) const noexcept
	{
		return (causes & cause) != 0U;
	}

	/** Whether the reset cause is reported by this SoC at all. */
	[[nodiscard]] constexpr bool reports(std::uint32_t cause) const noexcept
	{
		return (supported & cause) != 0U;
	}

	[[nodiscard]] static Result<RebootReason> read() noexcept;
	[[nodiscard]] static Result<> clear() noexcept;
};

/** Access to the SoC's stable hardware identifier, without allocating. */
class DeviceIdentity
{
      public:
	/**
	 * Read the device identifier into @p destination.
	 *
	 * Returns a view of exactly the bytes the SoC provided. The previous
	 * fixed-array form zero-padded short identifiers with no way to tell
	 * padding from data, which made two devices with different-length IDs
	 * indistinguishable.
	 */
	[[nodiscard]] static Result<std::span<const std::byte>>
	read(std::span<std::byte> destination) noexcept;

	/** Read into owned storage, reporting how many bytes are meaningful. */
	template <std::size_t Capacity = 16U>
	[[nodiscard]] static Result<std::pair<std::array<std::byte, Capacity>, std::size_t>>
	read_array() noexcept
	{
		std::array<std::byte, Capacity> id{};
		ZEST_TRY_ASSIGN(view, read(id));
		return std::pair{id, view.size()};
	}

	/** Format the identifier as lowercase hexadecimal into @p destination. */
	[[nodiscard]] static Result<std::string_view> read_hex(std::span<char> destination) noexcept;

	[[nodiscard]] static Result<std::array<std::byte, 8>> eui64() noexcept;
};

} /* namespace zest */
