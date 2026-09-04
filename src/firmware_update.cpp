/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/firmware_update.hpp>

#include <zephyr/devicetree.h>

#include <cstdio>

namespace zest
{

Result<std::string_view> ImageVersion::format(std::span<char> destination) const noexcept
{
	if (destination.empty()) {
		return fail(errors::no_buffer_space);
	}
	const int written =
		std::snprintf(destination.data(), destination.size(), "%u.%u.%u+%u",
			      static_cast<unsigned>(major), static_cast<unsigned>(minor),
			      static_cast<unsigned>(revision), static_cast<unsigned>(build));
	if (written < 0) {
		return fail(errors::io_error);
	}
	if (static_cast<std::size_t>(written) >= destination.size()) {
		/* snprintf truncated. Report it rather than returning a version
		 * string that is a prefix of the real one and compares equal to
		 * something else. */
		return fail(errors::no_buffer_space);
	}
	return std::string_view{destination.data(), static_cast<std::size_t>(written)};
}

std::uint8_t FirmwareUpdate::partition_id(ImageSlot slot) noexcept
{
	return slot == ImageSlot::running ? FIXED_PARTITION_ID(slot0_partition)
					  : FIXED_PARTITION_ID(slot1_partition);
}

Result<ImageVersion> FirmwareUpdate::version(ImageSlot slot) noexcept
{
	mcuboot_img_header header{};
	ZEST_TRY(check(boot_read_bank_header(partition_id(slot), &header, sizeof(header))));

	/* Only the v1 union member is valid, and only when the header says so;
	 * reading it otherwise reinterprets whatever a future format put there. */
	if (header.mcuboot_version != 1U) {
		return fail(errors::not_supported);
	}
	const mcuboot_img_sem_ver &semantic = header.h.v1.sem_ver;
	return ImageVersion{
		.major = semantic.major,
		.minor = semantic.minor,
		.revision = semantic.revision,
		.build = semantic.build_num,
	};
}

Result<> FirmwareUpdate::request_upgrade(UpgradeMode mode) noexcept
{
	const int permanent =
		mode == UpgradeMode::permanent ? BOOT_UPGRADE_PERMANENT : BOOT_UPGRADE_TEST;
	return check(boot_request_upgrade(permanent));
}

bool FirmwareUpdate::running_image_confirmed() noexcept
{
	return boot_is_img_confirmed();
}

Result<> FirmwareUpdate::confirm_running_image() noexcept
{
	if (boot_is_img_confirmed()) {
		return {};
	}
	return check(boot_write_img_confirmed());
}

/* ------------------------------------------------------------------ writer --- */

Result<> FirmwareUpdate::Writer::begin() noexcept
{
	if (begun_) {
		return fail(errors::already);
	}
	ZEST_TRY(check(flash_img_init(&context_)));
	begun_ = true;
	return {};
}

Result<> FirmwareUpdate::Writer::write(std::span<const std::byte> data, bool last) noexcept
{
	if (!begun_) {
		return fail(errors::bad_descriptor);
	}
	if (data.empty() && !last) {
		return {};
	}
	return check(flash_img_buffered_write(
		&context_, reinterpret_cast<const std::uint8_t *>(data.data()), data.size(), last));
}

std::size_t FirmwareUpdate::Writer::written() const noexcept
{
	if (!begun_) {
		return 0U;
	}
	/* flash_img_bytes_written() takes a non-const context but only reads it. */
	return flash_img_bytes_written(const_cast<flash_img_context *>(&context_));
}

} /* namespace zest */
