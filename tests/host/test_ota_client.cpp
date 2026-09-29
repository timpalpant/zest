/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/ota_client.hpp>

#include <cstring>

using namespace zest;

/* A manifest is untrusted, so the image name must not be able to name another
 * host, another path, or a query. */
constexpr bool plain_names_are_accepted()
{
	return is_plain_file_name("app-1.2.3-45.bin") && is_plain_file_name("a") &&
	       is_plain_file_name("v1.0.bin");
}
static_assert(plain_names_are_accepted());

constexpr bool unsafe_names_are_refused()
{
	return !is_plain_file_name("") && !is_plain_file_name("/etc/passwd") &&
	       !is_plain_file_name("dir/app.bin") && !is_plain_file_name("../app.bin") &&
	       !is_plain_file_name("app..bin") && !is_plain_file_name("app.bin?x=1") &&
	       !is_plain_file_name("app.bin#frag") && !is_plain_file_name("http://evil/app.bin") &&
	       !is_plain_file_name("..");
}
static_assert(unsafe_names_are_refused());

bool range_header_is_inclusive()
{
	char text[40];
	const auto first = format_range_header(text, 0, 2048);
	if (!first || *first != "bytes=0-2047") {
		return false;
	}
	const auto next = format_range_header(text, 2048, 2048);
	if (!next || *next != "bytes=2048-4095") {
		return false;
	}
	const auto one = format_range_header(text, 687239, 1);
	return one && *one == "bytes=687239-687239";
}

bool range_header_rejects_bad_input()
{
	char text[40];
	char tiny[8];
	return !format_range_header(text, 0, 0) && !format_range_header(tiny, 0, 2048) &&
	       !format_range_header(std::span<char>{}, 0, 1);
}

bool manifest_version_is_range_checked()
{
	OtaManifest good{};
	good.version_major = 1;
	good.version_minor = 2;
	good.version_revision = 3;
	good.version_build = 45;
	const auto version = good.image_version();
	if (!version || *version != ImageVersion{1, 2, 3, 45}) {
		return false;
	}
	OtaManifest bad = good;
	bad.version_major = 300;
	OtaManifest negative = good;
	negative.version_build = -1;
	return !bad.image_version() && !negative.image_version();
}

bool manifest_image_name_stops_at_nul()
{
	OtaManifest manifest{};
	std::strcpy(manifest.image, "app-1.2.3-45.bin");
	return manifest.image_name() == "app-1.2.3-45.bin";
}

bool errors_name_their_stage()
{
	const OtaError error{OtaStage::image_range, errors::bad_message, 200, 4096};
	return std::strcmp(error.stage_name(), "image range") == 0 && error.http_status == 200 &&
	       error.offset == 4096U && !error.message().empty();
}

int main()
{
	CHECK(range_header_is_inclusive());
	CHECK(range_header_rejects_bad_input());
	CHECK(manifest_version_is_range_checked());
	CHECK(manifest_image_name_stops_at_nul());
	CHECK(errors_name_their_stage());
	return zest::test::summary("ota_client");
}
