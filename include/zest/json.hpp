/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/schema.hpp>

#include <zephyr/data/json.h>

#include <cstddef>
#include <span>
#include <string_view>

/**
 * @file
 * @brief The JSON codec for `zest::Schema`.
 *
 * The schema is declared once with `ZEST_SCHEMA` (see `zest/schema.hpp`) and
 * handed to Zephyr's JSON library unchanged, so no serialization logic is
 * duplicated here.
 *
 * ### Constraints inherited from Zephyr's JSON library
 *
 * These are properties of the underlying library, not of this wrapper. CBOR has
 * none of them, which is one reason to prefer it where both ends are yours.
 *
 * - **Parsing mutates the buffer.** `json_obj_parse()` writes NUL terminators in
 *   place, so the buffer must be writable.
 * - **`char *` fields borrow.** A `const char *` member points into the parsed
 *   buffer. Use `char[N]` when the value must outlive it.
 * - **Every schema field is always encoded.** Zephyr's encoder walks the whole
 *   descriptor array, so a field cannot be omitted per value.
 *
 * An incoming key with no matching field is skipped, so a sender adding fields
 * does not break a receiver.
 *
 * Requires `CONFIG_JSON_LIBRARY=y`, and `CONFIG_JSON_LIBRARY_FP_SUPPORT=y` for
 * `float` or `double` members.
 */

namespace zest::json
{

/**
 * Parse a JSON object into @p T, in place.
 *
 * @p json must be writable. Any `char *` member of the result points into it, so
 * the buffer must outlive the value; a `char[N]` member holds a copy.
 */
template <Serializable T> [[nodiscard]] Result<Parsed<T>> parse(std::span<char> json) noexcept
{
	static_assert(Schema<T>::count <= 64U,
		      "field presence is reported in a 64-bit bitmap, so a schema is limited "
		      "to 64 fields; nest a sub-object to go further");

	if (json.empty()) {
		return fail(errors::invalid_argument);
	}

	Parsed<T> parsed{};
	const std::int64_t decoded = json_obj_parse(
		json.data(), json.size(), Schema<T>::descriptors, Schema<T>::count, &parsed.value);
	if (decoded < 0) {
		return fail(static_cast<int>(decoded));
	}
	parsed.present = static_cast<std::uint64_t>(decoded);
	return parsed;
}

/** Parse from a byte buffer, such as an HTTP response body. */
template <Serializable T> [[nodiscard]] Result<Parsed<T>> parse(std::span<std::byte> json) noexcept
{
	return parse<T>(std::span<char>{reinterpret_cast<char *>(json.data()), json.size()});
}

/**
 * Encode @p value as a JSON object into @p buffer.
 *
 * Returns a view of the encoded text, NUL-terminated within @p buffer.
 */
template <Serializable T>
[[nodiscard]] Result<std::string_view> encode(const T &value, std::span<char> buffer) noexcept
{
	if (buffer.empty()) {
		return fail(errors::invalid_argument);
	}

	const int rc = json_obj_encode_buf(Schema<T>::descriptors, Schema<T>::count, &value,
					   buffer.data(), buffer.size());
	if (rc < 0) {
		/* Zephyr reports a short buffer as -ENOMEM; say so in Zest's vocabulary. */
		return fail(rc == -ENOMEM ? errors::no_buffer_space : Error{rc});
	}
	return std::string_view{buffer.data()};
}

/** Encode into a byte buffer, for `HttpClient::post()` and similar. */
template <Serializable T>
[[nodiscard]] Result<std::span<const std::byte>> encode(const T &value,
							std::span<std::byte> buffer) noexcept
{
	auto text_result = encode(
		value, std::span<char>{reinterpret_cast<char *>(buffer.data()), buffer.size()});
	if (!text_result) {
		return fail(text_result.error());
	}
	auto text = *text_result;
	return std::as_bytes(std::span{text.data(), text.size()});
}

/** Bytes an encoding of @p value would occupy, excluding the NUL. */
template <Serializable T> [[nodiscard]] Result<std::size_t> encoded_size(const T &value) noexcept
{
	const ssize_t size =
		json_calc_encoded_len(Schema<T>::descriptors, Schema<T>::count, &value);
	if (size < 0) {
		return fail(static_cast<int>(size));
	}
	return static_cast<std::size_t>(size);
}

/**
 * Parse a top-level JSON array, such as `[{...},{...}]`.
 *
 * @p T must be a wrapper whose schema holds exactly one array field.
 */
template <Serializable T> [[nodiscard]] Result<T> parse_array(std::span<char> json) noexcept
{
	static_assert(Schema<T>::count == 1U,
		      "a top-level array maps to a wrapper struct whose schema has exactly "
		      "one array field");
	if (json.empty()) {
		return fail(errors::invalid_argument);
	}

	T value{};
	ZEST_TRY(check(json_arr_parse(json.data(), json.size(), Schema<T>::descriptors, &value)));
	return value;
}

template <Serializable T> [[nodiscard]] Result<T> parse_array(std::span<std::byte> json) noexcept
{
	return parse_array<T>(std::span<char>{reinterpret_cast<char *>(json.data()), json.size()});
}

/** Encode a wrapper's single array field as a top-level JSON array. */
template <Serializable T>
[[nodiscard]] Result<std::string_view> encode_array(const T &value, std::span<char> buffer) noexcept
{
	static_assert(Schema<T>::count == 1U,
		      "a top-level array maps to a wrapper struct whose schema has exactly "
		      "one array field");
	if (buffer.empty()) {
		return fail(errors::invalid_argument);
	}

	const int rc =
		json_arr_encode_buf(Schema<T>::descriptors, &value, buffer.data(), buffer.size());
	if (rc < 0) {
		return fail(rc == -ENOMEM ? errors::no_buffer_space : Error{rc});
	}
	return std::string_view{buffer.data()};
}

/**
 * Escape @p text in place so it is safe as a JSON string value.
 *
 * Needed only when composing JSON by hand; `encode()` escapes for itself.
 */
[[nodiscard]] inline Result<std::string_view> escape(std::span<char> buffer,
						     std::size_t length) noexcept
{
	if (length > buffer.size()) {
		return fail(errors::invalid_argument);
	}
	std::size_t escaped = length;
	const ssize_t rc = json_escape(buffer.data(), &escaped, buffer.size());
	if (rc < 0) {
		return fail(static_cast<int>(rc));
	}
	return std::string_view{buffer.data(), escaped};
}

} /* namespace zest::json */
