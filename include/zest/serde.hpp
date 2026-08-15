/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/schema.hpp>

#if defined(CONFIG_ZEST_JSON)
#include <zest/json.hpp>
#endif
#if defined(CONFIG_ZEST_CBOR)
#include <zest/cbor.hpp>
#endif

#include <cstddef>
#include <span>

/**
 * @file
 * @brief Choosing a serialization format without changing the schema.
 *
 * One `ZEST_SCHEMA` describes the struct; the format is a template argument, so a
 * device can be built to speak CBOR to its own backend and JSON to a third party
 * without a second schema and without the two drifting apart:
 *
 * ```cpp
 * constexpr auto kFormat = zest::Format::cbor;
 *
 * std::array<std::byte, 128> buffer{};
 * auto body = zest::serialize<kFormat>(reading, buffer);
 * if (!body) return zest::fail(body.error());
 * ZEST_TRY(client.publish("sensor/1", *body));
 *
 * auto parsed = zest::deserialize<kFormat, Reading>(payload);
 * ```
 *
 * The two formats are not equivalent, and where they differ CBOR is the better
 * behaved:
 *
 * | | JSON | CBOR |
 * | --- | --- | --- |
 * | Size | ~2x | baseline |
 * | Decoding modifies the input | yes | no |
 * | `char *` members decode | yes, borrowing | no, use `char[N]` |
 * | Arrays of objects | yes | no, compile-time error |
 * | Unknown incoming keys | skipped | skipped |
 *
 * Prefer CBOR where both ends are yours; JSON is for interoperating with
 * something that expects it.
 */

namespace zest
{

/** A wire format a schema can be serialized to. */
enum class Format : std::uint8_t {
	json,
	cbor,
};

/**
 * Encode @p value into @p buffer in the chosen format.
 *
 * Returns a view of the encoded bytes, ready for `MqttClient::publish()` or
 * `HttpClient::post()`.
 */
template <Format F, Serializable T>
[[nodiscard]] Result<std::span<const std::byte>> serialize(const T &value,
							   std::span<std::byte> buffer) noexcept
{
	if constexpr (F == Format::json) {
#if defined(CONFIG_ZEST_JSON)
		return json::encode(value, buffer);
#else
		static_assert(detail::always_false<T>,
			      "Format::json needs CONFIG_ZEST_JSON=y and CONFIG_JSON_LIBRARY=y");
#endif
	} else {
#if defined(CONFIG_ZEST_CBOR)
		return cbor::encode(value, buffer);
#else
		static_assert(detail::always_false<T>,
			      "Format::cbor needs CONFIG_ZEST_CBOR=y and CONFIG_ZCBOR=y");
#endif
	}
}

/**
 * Decode @p payload in the chosen format.
 *
 * The buffer is mutable because the JSON parser rewrites it in place; the CBOR
 * decoder leaves it untouched.
 */
template <Format F, Serializable T>
[[nodiscard]] Result<Parsed<T>> deserialize(std::span<std::byte> payload) noexcept
{
	if constexpr (F == Format::json) {
#if defined(CONFIG_ZEST_JSON)
		return json::parse<T>(payload);
#else
		static_assert(detail::always_false<T>,
			      "Format::json needs CONFIG_ZEST_JSON=y and CONFIG_JSON_LIBRARY=y");
#endif
	} else {
#if defined(CONFIG_ZEST_CBOR)
		return cbor::parse<T>(payload);
#else
		static_assert(detail::always_false<T>,
			      "Format::cbor needs CONFIG_ZEST_CBOR=y and CONFIG_ZCBOR=y");
#endif
	}
}

/** The media type describing @p F, for an HTTP `Content-Type` header. */
[[nodiscard]] constexpr std::string_view content_type(Format format) noexcept
{
	return format == Format::json ? std::string_view{"application/json"}
				      : std::string_view{"application/cbor"};
}

} /* namespace zest */
