/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zest
{

/**
 * A fixed-buffer CBOR encoder for telemetry payloads.
 *
 * `MqttClient::publish()` and `HttpClient::post()` both want a span of bytes, and
 * a device reporting readings needs some way to produce one. JSON costs roughly
 * twice the bytes and a text formatter; CBOR (RFC 8949) is a byte-for-byte
 * self-describing binary encoding that every ingestion stack already speaks, and
 * encoding it needs nothing but shifts and copies.
 *
 * Everything is written into inline storage, so nothing allocates. A write that
 * would exceed the buffer leaves the encoder in an overflowed state and reports
 * `errors::no_buffer_space`; the state is sticky, so a caller may write a whole
 * document and check once at the end rather than after every field.
 *
 * ```cpp
 * zest::CborWriter<64> writer;
 * (void)writer.begin_map(3);
 * (void)writer.add_text("mv");   (void)writer.add_int(millivolts.count());
 * (void)writer.add_text("degc"); (void)writer.add_int(temperature.count());
 * (void)writer.add_text("ok");   (void)writer.add_bool(true);
 *
 * if (!writer.overflowed()) {
 *         ZEST_TRY(client.publish("sensor/1", writer.bytes()));
 * }
 * ```
 */
template <std::size_t Capacity> class CborWriter
{
      public:
	static_assert(Capacity > 0U, "a CBOR writer needs storage");

	/** Encoded bytes written so far. Empty when the encoder has overflowed. */
	[[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept
	{
		return overflowed_ ? std::span<const std::byte>{}
				   : std::span<const std::byte>{buffer_.data(), size_};
	}

	/** Whether any write did not fit. Sticky once set. */
	[[nodiscard]] constexpr bool overflowed() const noexcept
	{
		return overflowed_;
	}

	[[nodiscard]] constexpr std::size_t size() const noexcept
	{
		return size_;
	}
	[[nodiscard]] static constexpr std::size_t capacity() noexcept
	{
		return Capacity;
	}

	constexpr void clear() noexcept
	{
		size_ = 0U;
		overflowed_ = false;
	}

	/** Unsigned integer (major type 0). */
	constexpr Result<> add_uint(std::uint64_t value) noexcept
	{
		return write_head(0U, value);
	}

	/** Signed integer, encoded as major type 0 or 1 as appropriate. */
	constexpr Result<> add_int(std::int64_t value) noexcept
	{
		if (value < 0) {
			/* CBOR stores -1 - n, so -1 becomes 0. */
			return write_head(1U, static_cast<std::uint64_t>(-(value + 1)));
		}
		return write_head(0U, static_cast<std::uint64_t>(value));
	}

	/** Byte string (major type 2). */
	constexpr Result<> add_bytes(std::span<const std::byte> value) noexcept
	{
		ZEST_TRY(write_head(2U, value.size()));
		return write_raw(value);
	}

	/** Text string (major type 3). Assumed to be valid UTF-8. */
	constexpr Result<> add_text(std::string_view value) noexcept
	{
		ZEST_TRY(write_head(3U, value.size()));
		for (const char character : value) {
			ZEST_TRY(write_byte(static_cast<std::byte>(character)));
		}
		return {};
	}

	/** Definite-length array header; @p count items must follow. */
	constexpr Result<> begin_array(std::size_t count) noexcept
	{
		return write_head(4U, count);
	}

	/** Definite-length map header; @p count key/value pairs must follow. */
	constexpr Result<> begin_map(std::size_t count) noexcept
	{
		return write_head(5U, count);
	}

	/**
	 * Indefinite-length array header, closed by `end_indefinite()`.
	 *
	 * Useful when the item count is not known until the loop has run.
	 */
	constexpr Result<> begin_indefinite_array() noexcept
	{
		return write_byte(std::byte{0x9F});
	}

	/** Indefinite-length map header, closed by `end_indefinite()`. */
	constexpr Result<> begin_indefinite_map() noexcept
	{
		return write_byte(std::byte{0xBF});
	}

	/** Close the most recent indefinite-length container. */
	constexpr Result<> end_indefinite() noexcept
	{
		return write_byte(std::byte{0xFF});
	}

	constexpr Result<> add_bool(bool value) noexcept
	{
		return write_byte(static_cast<std::byte>(value ? 0xF5U : 0xF4U));
	}

	constexpr Result<> add_null() noexcept
	{
		return write_byte(std::byte{0xF6});
	}

	constexpr Result<> add_undefined() noexcept
	{
		return write_byte(std::byte{0xF7});
	}

	/**
	 * Single-precision float (major type 7, additional information 26).
	 *
	 * Single precision on purpose: no common Cortex-M part has a
	 * double-precision FPU, and a sensor reading never needs the extra bits.
	 */
	constexpr Result<> add_float(float value) noexcept
	{
		ZEST_TRY(write_byte(std::byte{0xFA}));
		const auto bits = std::bit_cast<std::uint32_t>(value);
		return write_big_endian(bits, 4U);
	}

	/** A CBOR tag (major type 6), which the tagged item must follow. */
	constexpr Result<> add_tag(std::uint64_t tag) noexcept
	{
		return write_head(6U, tag);
	}

      private:
	constexpr Result<> write_byte(std::byte value) noexcept
	{
		if (overflowed_ || size_ >= Capacity) {
			overflowed_ = true;
			return fail(errors::no_buffer_space);
		}
		buffer_[size_++] = value;
		return {};
	}

	constexpr Result<> write_raw(std::span<const std::byte> value) noexcept
	{
		for (const std::byte item : value) {
			ZEST_TRY(write_byte(item));
		}
		return {};
	}

	constexpr Result<> write_big_endian(std::uint64_t value, unsigned width) noexcept
	{
		for (unsigned index = width; index > 0U; --index) {
			const auto shift = (index - 1U) * 8U;
			ZEST_TRY(write_byte(static_cast<std::byte>((value >> shift) & 0xFFU)));
		}
		return {};
	}

	/** Write a major type and its argument in the shortest CBOR form. */
	constexpr Result<> write_head(std::uint8_t major, std::uint64_t value) noexcept
	{
		const auto prefix = static_cast<std::uint8_t>(major << 5U);
		if (value < 24U) {
			return write_byte(
				static_cast<std::byte>(prefix | static_cast<std::uint8_t>(value)));
		}
		if (value <= 0xFFU) {
			ZEST_TRY(write_byte(static_cast<std::byte>(prefix | 24U)));
			return write_big_endian(value, 1U);
		}
		if (value <= 0xFFFFU) {
			ZEST_TRY(write_byte(static_cast<std::byte>(prefix | 25U)));
			return write_big_endian(value, 2U);
		}
		if (value <= 0xFFFFFFFFU) {
			ZEST_TRY(write_byte(static_cast<std::byte>(prefix | 26U)));
			return write_big_endian(value, 4U);
		}
		ZEST_TRY(write_byte(static_cast<std::byte>(prefix | 27U)));
		return write_big_endian(value, 8U);
	}

	std::array<std::byte, Capacity> buffer_{};
	std::size_t size_{0U};
	bool overflowed_{false};
};

} /* namespace zest */
