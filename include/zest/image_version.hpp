/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * An MCUboot semantic version, with no Zephyr dependency.
 *
 * Split out of firmware_update.hpp so it can be parsed, compared and formatted
 * by code that never touches the bootloader (an update manifest, a host tool)
 * and tested on the host.
 */

#include <zest/error.hpp>

#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace zest
{

/**
 * An image's MCUboot semantic version.
 *
 * Ordered by major, then minor, then revision, then build --- the field order,
 * so the defaulted comparison is the right one and there is no packed integer
 * key to get wrong.
 */
struct ImageVersion {
	std::uint8_t major{};
	std::uint8_t minor{};
	std::uint16_t revision{};
	std::uint32_t build{};

	[[nodiscard]] constexpr auto operator<=>(const ImageVersion &) const noexcept = default;
	[[nodiscard]] constexpr bool operator==(const ImageVersion &) const noexcept = default;

	/**
	 * Build a version from wider integers, refusing any that MCUboot's header
	 * cannot hold (negative, or past the field's width) rather than truncating
	 * them into a different, valid-looking version.
	 */
	[[nodiscard]] static constexpr Result<ImageVersion> from_parts(std::int64_t major,
								       std::int64_t minor,
								       std::int64_t revision,
								       std::int64_t build) noexcept
	{
		const auto fits = [](std::int64_t value, std::int64_t maximum) {
			return value >= 0 && value <= maximum;
		};
		if (!fits(major, std::numeric_limits<std::uint8_t>::max()) ||
		    !fits(minor, std::numeric_limits<std::uint8_t>::max()) ||
		    !fits(revision, std::numeric_limits<std::uint16_t>::max()) ||
		    !fits(build, std::numeric_limits<std::uint32_t>::max())) {
			return fail(errors::invalid_argument);
		}
		return ImageVersion{
			.major = static_cast<std::uint8_t>(major),
			.minor = static_cast<std::uint8_t>(minor),
			.revision = static_cast<std::uint16_t>(revision),
			.build = static_cast<std::uint32_t>(build),
		};
	}

	/**
	 * Parse "major.minor.revision+build" --- what @ref format writes, and what
	 * imgtool is given as the image version. Anything else, including trailing
	 * text, is an error.
	 */
	[[nodiscard]] static Result<ImageVersion> parse(std::string_view text) noexcept
	{
		std::int64_t parts[4]{};
		const char separators[4] = {'.', '.', '+', '\0'};
		const char *cursor = text.data();
		const char *const end = text.data() + text.size();
		for (int i = 0; i < 4; ++i) {
			const auto [next, error] = std::from_chars(cursor, end, parts[i]);
			if (error != std::errc{} || next == cursor) {
				return fail(errors::invalid_argument);
			}
			cursor = next;
			if (separators[i] == '\0') {
				if (cursor != end) {
					return fail(errors::invalid_argument);
				}
			} else if (cursor == end || *cursor != separators[i]) {
				return fail(errors::invalid_argument);
			} else {
				++cursor;
			}
		}
		return from_parts(parts[0], parts[1], parts[2], parts[3]);
	}

	/**
	 * Render as "major.minor.revision+build" into @p destination, NUL-terminated.
	 *
	 * Returns the text written, or an error if it would not fit --- a truncated
	 * version string would compare equal to a different version. 25 bytes (24
	 * characters and the NUL) is always enough.
	 */
	[[nodiscard]] Result<std::string_view> format(std::span<char> destination) const noexcept
	{
		char *cursor = destination.data();
		char *const end = destination.data() + destination.size();
		const auto put_number = [&](std::uint32_t value, char separator) {
			const auto [next, error] = std::to_chars(cursor, end, value);
			if (error != std::errc{} || next >= end) {
				return false;
			}
			cursor = next;
			if (separator != '\0') {
				*cursor++ = separator;
			}
			return true;
		};
		if (destination.empty() || !put_number(major, '.') || !put_number(minor, '.') ||
		    !put_number(revision, '+') || !put_number(build, '\0')) {
			return fail(errors::no_buffer_space);
		}
		*cursor = '\0';
		return std::string_view{destination.data(),
					static_cast<std::size_t>(cursor - destination.data())};
	}
};

} /* namespace zest */
