/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * Staging and confirming an MCUboot image.
 *
 * Deliberately says nothing about where the bytes come from. Transport is the
 * application's — an HTTP range request, an MQTT topic, a serial link, a file
 * on removable storage — and each one has its own authentication, framing and
 * retry policy. What every one of them then has to do is identical: stream the
 * image into the spare slot, check that what landed is an MCUboot image of the
 * version that was promised, ask the bootloader to install it, and, once the new
 * build has proved itself, confirm it so it is not reverted.
 *
 * That last step is the one worth having a name for. Under a swapping MCUboot
 * mode a freshly installed image runs *on trial*: unless it confirms itself, the
 * bootloader puts the old one back on the next reset. That is the property that
 * makes an unattended device safe to update, and it is lost silently — the
 * update simply appears to work, and then one day a device that rebooted for an
 * unrelated reason comes back on the previous firmware.
 */

#include <zest/error.hpp>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zest
{

/**
 * An image's MCUboot semantic version.
 *
 * Ordered by major, then minor, then revision, then build — the field order, so
 * the defaulted comparison is the right one and there is no packed integer key
 * to get wrong.
 */
struct ImageVersion {
	std::uint8_t major{};
	std::uint8_t minor{};
	std::uint16_t revision{};
	std::uint32_t build{};

	[[nodiscard]] constexpr auto operator<=>(const ImageVersion &) const noexcept = default;
	[[nodiscard]] constexpr bool operator==(const ImageVersion &) const noexcept = default;

	/**
	 * Render as "major.minor.revision+build" into @p destination.
	 *
	 * Returns the text written, or an error if it would not fit. 24 characters
	 * is always enough.
	 */
	[[nodiscard]] Result<std::string_view> format(std::span<char> destination) const noexcept;
};

/** Which MCUboot slot an operation names. */
enum class ImageSlot : std::uint8_t {
	/** The image that is running now. */
	running,
	/** The spare slot an update is written into. */
	upload,
};

/** What the bootloader should do with a staged image. */
enum class UpgradeMode : std::uint8_t {
	/**
	 * Boot it once; revert on the next reset unless it confirms itself.
	 *
	 * Only meaningful under a swapping bootloader mode, which is what keeps a
	 * second copy of the old image to revert *to*.
	 */
	test,
	/** Boot it from now on, with no trial period. */
	permanent,
};

/**
 * Staging an image into the spare slot and arming the bootloader.
 *
 * Everything here is static: there is one bootloader and one pair of slots, and
 * an instance would only invite the impression that two updates could be in
 * flight at once.
 */
class FirmwareUpdate
{
      public:
	/**
	 * A streaming write into the upload slot.
	 *
	 * Takes the image in whatever sized pieces the transport produces and
	 * buffers them into flash-page-sized writes, so a caller can hand over a
	 * 512-byte HTTP chunk without knowing the erase geometry. The slot is
	 * erased as it goes.
	 *
	 * The final piece must be marked, because that is what flushes the partial
	 * page still in the buffer — an image whose last write was not flushed is
	 * short by up to a page and fails the bootloader's hash check.
	 */
	class Writer
	{
	      public:
		Writer() noexcept = default;
		Writer(const Writer &) = delete;
		Writer &operator=(const Writer &) = delete;

		/** Prepare the upload slot. Call once, before the first write. */
		[[nodiscard]] Result<> begin() noexcept;

		/**
		 * Append @p data, flushing if @p last.
		 *
		 * Pass an empty @p data with @p last to flush after the transport
		 * has already delivered everything.
		 */
		[[nodiscard]] Result<> write(std::span<const std::byte> data,
					     bool last = false) noexcept;

		/** Bytes written to the slot so far. */
		[[nodiscard]] std::size_t written() const noexcept;

		[[nodiscard]] bool begun() const noexcept
		{
			return begun_;
		}

	      private:
		flash_img_context context_{};
		bool begun_{false};
	};

	/** Devicetree partition id backing @p slot. */
	[[nodiscard]] static std::uint8_t partition_id(ImageSlot slot) noexcept;

	/**
	 * Read the MCUboot header of @p slot.
	 *
	 * An upload slot that has never been written, or that holds a truncated
	 * download, has no readable header and reports an error — which is the
	 * cheap check worth doing before spending a reboot on it.
	 */
	[[nodiscard]] static Result<ImageVersion> version(ImageSlot slot) noexcept;

	/**
	 * Ask the bootloader to install what is in the upload slot.
	 *
	 * Takes effect on the next reset; this does not reboot.
	 *
	 * @p mode defaults to what the build's bootloader mode can actually
	 * honour: `test` needs a swapping mode, and in overwrite-only builds the
	 * old image is gone the moment the copy completes, so there is nothing to
	 * revert to and `permanent` is the only truthful request.
	 */
	[[nodiscard]] static Result<>
	request_upgrade(UpgradeMode mode = default_upgrade_mode()) noexcept;

	/** The strongest mode this build's bootloader configuration supports. */
	[[nodiscard]] static constexpr UpgradeMode default_upgrade_mode() noexcept
	{
#if defined(CONFIG_MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY)
		return UpgradeMode::permanent;
#else
		return UpgradeMode::test;
#endif
	}

	/**
	 * Whether the running image has been confirmed.
	 *
	 * False means it is on trial and will be reverted on the next reset.
	 */
	[[nodiscard]] static bool running_image_confirmed() noexcept;

	[[nodiscard]] static bool running_image_on_trial() noexcept
	{
		return !running_image_confirmed();
	}

	/**
	 * Mark the running image healthy so it survives the next reset.
	 *
	 * Idempotent and cheap once confirmed. Call it when something the new
	 * build had to get right has actually happened end to end — a telemetry
	 * batch delivered, a server reached, a sensor read — rather than at the
	 * top of `main()`, where it confirms an image that has proved nothing.
	 */
	[[nodiscard]] static Result<> confirm_running_image() noexcept;
};

} /* namespace zest */
