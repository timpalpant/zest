/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/cbor.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

using zest::CborWriter;

namespace
{

/* Compare an encoder's output against an expected byte sequence. */
template <std::size_t Capacity>
bool encodes_to(const CborWriter<Capacity> &writer, std::initializer_list<unsigned> expected)
{
	const auto produced = writer.bytes();
	if (produced.size() != expected.size()) {
		return false;
	}
	std::size_t index = 0U;
	for (const unsigned value : expected) {
		if (produced[index] != static_cast<std::byte>(value)) {
			return false;
		}
		++index;
	}
	return true;
}

} /* namespace */

int main()
{
	/*
	 * Test vectors from RFC 8949 Appendix A, so the encoder is checked against
	 * the specification rather than against itself.
	 */

	/* Unsigned integers, at each width boundary. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_uint(0));
		CHECK(encodes_to(writer, {0x00}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_uint(23));
		CHECK(encodes_to(writer, {0x17}));
	}
	{
		/* 24 no longer fits the immediate form. */
		CborWriter<16> writer;
		CHECK_OK(writer.add_uint(24));
		CHECK(encodes_to(writer, {0x18, 0x18}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_uint(255));
		CHECK(encodes_to(writer, {0x18, 0xFF}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_uint(1000));
		CHECK(encodes_to(writer, {0x19, 0x03, 0xE8}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_uint(1'000'000));
		CHECK(encodes_to(writer, {0x1A, 0x00, 0x0F, 0x42, 0x40}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_uint(1'000'000'000'000ULL));
		CHECK(encodes_to(writer, {0x1B, 0x00, 0x00, 0x00, 0xE8, 0xD4, 0xA5, 0x10, 0x00}));
	}

	/* Negative integers store -1 - n. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_int(-1));
		CHECK(encodes_to(writer, {0x20}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_int(-10));
		CHECK(encodes_to(writer, {0x29}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_int(-100));
		CHECK(encodes_to(writer, {0x38, 0x63}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_int(-1000));
		CHECK(encodes_to(writer, {0x39, 0x03, 0xE7}));
	}
	{
		/* A non-negative signed value must use major type 0. */
		CborWriter<16> writer;
		CHECK_OK(writer.add_int(10));
		CHECK(encodes_to(writer, {0x0A}));
	}

	/* Text and byte strings. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_text(""));
		CHECK(encodes_to(writer, {0x60}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_text("a"));
		CHECK(encodes_to(writer, {0x61, 0x61}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_text("IETF"));
		CHECK(encodes_to(writer, {0x64, 0x49, 0x45, 0x54, 0x46}));
	}
	{
		CborWriter<16> writer;
		constexpr std::array payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
					     std::byte{0x04}};
		CHECK_OK(writer.add_bytes(payload));
		CHECK(encodes_to(writer, {0x44, 0x01, 0x02, 0x03, 0x04}));
	}

	/* Simple values. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_bool(false));
		CHECK_OK(writer.add_bool(true));
		CHECK_OK(writer.add_null());
		CHECK_OK(writer.add_undefined());
		CHECK(encodes_to(writer, {0xF4, 0xF5, 0xF6, 0xF7}));
	}

	/* Single-precision floats. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_float(1.0F));
		CHECK(encodes_to(writer, {0xFA, 0x3F, 0x80, 0x00, 0x00}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_float(100000.0F));
		CHECK(encodes_to(writer, {0xFA, 0x47, 0xC3, 0x50, 0x00}));
	}

	/* Definite-length containers. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.begin_array(0));
		CHECK(encodes_to(writer, {0x80}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.begin_array(3));
		CHECK_OK(writer.add_uint(1));
		CHECK_OK(writer.add_uint(2));
		CHECK_OK(writer.add_uint(3));
		CHECK(encodes_to(writer, {0x83, 0x01, 0x02, 0x03}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.begin_map(0));
		CHECK(encodes_to(writer, {0xA0}));
	}
	{
		/* {"a": 1, "b": [2, 3]} */
		CborWriter<32> writer;
		CHECK_OK(writer.begin_map(2));
		CHECK_OK(writer.add_text("a"));
		CHECK_OK(writer.add_uint(1));
		CHECK_OK(writer.add_text("b"));
		CHECK_OK(writer.begin_array(2));
		CHECK_OK(writer.add_uint(2));
		CHECK_OK(writer.add_uint(3));
		CHECK(encodes_to(writer, {0xA2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03}));
	}

	/* Indefinite-length containers, for counts unknown until the loop ends. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.begin_indefinite_array());
		CHECK_OK(writer.add_uint(1));
		CHECK_OK(writer.add_uint(2));
		CHECK_OK(writer.end_indefinite());
		CHECK(encodes_to(writer, {0x9F, 0x01, 0x02, 0xFF}));
	}
	{
		CborWriter<16> writer;
		CHECK_OK(writer.begin_indefinite_map());
		CHECK_OK(writer.add_text("a"));
		CHECK_OK(writer.add_uint(1));
		CHECK_OK(writer.end_indefinite());
		CHECK(encodes_to(writer, {0xBF, 0x61, 0x61, 0x01, 0xFF}));
	}

	/* Tags. */
	{
		CborWriter<16> writer;
		CHECK_OK(writer.add_tag(1));
		CHECK_OK(writer.add_uint(1'363'896'240));
		CHECK(encodes_to(writer, {0xC1, 0x1A, 0x51, 0x4B, 0x67, 0xB0}));
	}

	/* Overflow is reported, sticky, and hides the partial document. */
	{
		CborWriter<3> writer;
		CHECK_OK(writer.add_uint(1));
		CHECK_EQ(writer.size(), 1U);
		CHECK(!writer.overflowed());

		/* A four-byte text will not fit in the two remaining bytes. */
		CHECK_ERR(writer.add_text("IETF"), zest::errors::no_buffer_space);
		CHECK(writer.overflowed());
		/* Once overflowed, output is withheld rather than truncated. */
		CHECK(writer.bytes().empty());
		/* And the state does not clear itself on a later small write. */
		CHECK_ERR(writer.add_uint(1), zest::errors::no_buffer_space);
		CHECK(writer.overflowed());

		writer.clear();
		CHECK(!writer.overflowed());
		CHECK_EQ(writer.size(), 0U);
		CHECK_OK(writer.add_uint(1));
	}

	/* An exactly-full buffer is not an overflow. */
	{
		CborWriter<2> writer;
		CHECK_OK(writer.add_uint(24)); /* two bytes */
		CHECK(!writer.overflowed());
		CHECK_EQ(writer.size(), 2U);
		CHECK_ERR(writer.add_uint(0), zest::errors::no_buffer_space);
	}

	/* A realistic telemetry document. */
	{
		CborWriter<64> writer;
		CHECK_OK(writer.begin_map(3));
		CHECK_OK(writer.add_text("mv"));
		CHECK_OK(writer.add_int(3742));
		CHECK_OK(writer.add_text("degc"));
		CHECK_OK(writer.add_int(-5));
		CHECK_OK(writer.add_text("ok"));
		CHECK_OK(writer.add_bool(true));
		CHECK(!writer.overflowed());
		/* Comfortably smaller than the equivalent JSON. */
		CHECK(writer.size() < 32U);
	}

	return zest::test::summary("cbor");
}
