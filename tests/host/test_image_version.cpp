/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/image_version.hpp>

#include <cstring>

using namespace zest;

constexpr bool ordering_follows_field_order()
{
	const ImageVersion a{1, 2, 3, 4};
	return a < ImageVersion{1, 2, 3, 5} && a < ImageVersion{1, 2, 4, 0} &&
	       a < ImageVersion{1, 3, 0, 0} && a < ImageVersion{2, 0, 0, 0} &&
	       a == ImageVersion{1, 2, 3, 4} && !(a < a);
}
static_assert(ordering_follows_field_order());

constexpr bool from_parts_accepts_the_limits()
{
	const auto low = ImageVersion::from_parts(0, 0, 0, 0);
	const auto high = ImageVersion::from_parts(255, 255, 65535, 4294967295LL);
	return low && *low == ImageVersion{} && high &&
	       *high == ImageVersion{255, 255, 65535, 4294967295U};
}
static_assert(from_parts_accepts_the_limits());

/* Anything MCUboot's header cannot hold is refused, not wrapped into a different
 * valid-looking version. */
constexpr bool from_parts_refuses_overflow_and_negatives()
{
	return !ImageVersion::from_parts(256, 0, 0, 0) && !ImageVersion::from_parts(0, 256, 0, 0) &&
	       !ImageVersion::from_parts(0, 0, 65536, 0) &&
	       !ImageVersion::from_parts(0, 0, 0, 4294967296LL) &&
	       !ImageVersion::from_parts(-1, 0, 0, 0) && !ImageVersion::from_parts(0, 0, 0, -1);
}
static_assert(from_parts_refuses_overflow_and_negatives());

bool parse_reads_what_format_writes()
{
	const ImageVersion original{2, 10, 300, 123456};
	char text[32];
	const auto formatted = original.format(text);
	if (!formatted || *formatted != "2.10.300+123456") {
		return false;
	}
	const auto parsed = ImageVersion::parse(*formatted);
	return parsed && *parsed == original;
}

bool parse_accepts_imgtool_versions()
{
	const auto parsed = ImageVersion::parse("0.1.0+27");
	return parsed && *parsed == ImageVersion{0, 1, 0, 27};
}

bool parse_rejects_malformed_text()
{
	for (const char *bad : {"", "1", "1.2", "1.2.3", "1.2.3+", "1.2.3+4x", "1.2.3+4 ",
				" 1.2.3+4", "1.2.3-4", "a.b.c+d", "1..3+4", "1.2.3+4+5", "-1.2.3+4",
				"256.0.0+0", "0.0.65536+0", "0.0.0+4294967296"}) {
		if (ImageVersion::parse(bad)) {
			std::fprintf(stderr, "accepted \"%s\"\n", bad);
			return false;
		}
	}
	return true;
}

/* The longest possible version is 24 characters, 25 bytes with its NUL; a buffer one short
 * is an error, never a truncated string that compares equal to another version. */
bool format_reports_a_short_buffer()
{
	const ImageVersion longest{255, 255, 65535, 4294967295U};
	char exact[25];
	const auto fits = longest.format(exact);
	if (!fits || *fits != "255.255.65535+4294967295" || std::strlen(exact) != 24U) {
		return false;
	}
	char short_buffer[24];
	if (longest.format(short_buffer)) {
		return false;
	}
	char tiny[1];
	return !ImageVersion{}.format(tiny) && !ImageVersion{}.format(std::span<char>{});
}

int main()
{
	CHECK(parse_reads_what_format_writes());
	CHECK(parse_accepts_imgtool_versions());
	CHECK(parse_rejects_malformed_text());
	CHECK(format_reports_a_short_buffer());
	return zest::test::summary("image_version");
}
