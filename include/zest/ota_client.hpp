/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * Fetching a firmware image from a static update directory.
 *
 * The other half of @ref zest::FirmwareUpdate. That says nothing about where an image
 * comes from; this says nothing about where it goes. Given a directory URL that
 * holds a `manifest.json` and the images it names --- plain files behind any web
 * server, which must honour HTTP Range --- it reads the manifest and streams a
 * named image, one range request at a time, into a sink the caller supplies. The
 * image never has to be held in RAM.
 *
 * Everything that is policy stays with the application: when to check, whether
 * "different" or "newer" means install, where the bytes are written (the spare
 * slot, or the primary slot in place), and what happens afterwards. An update
 * client that owns those decisions fits exactly one product.
 *
 * Trust is not this class's job either. The transport is plain HTTP by design,
 * so the manifest and the bytes are untrusted: an image must be authenticated by
 * a signature the bootloader checks (MCUboot does), and the worst a hostile
 * network can then do is make an update fail. The one thing checked here is
 * that a manifest cannot aim the download elsewhere --- the image name is a bare
 * file name, resolved against the directory the client was given.
 *
 * The manifest, one JSON object; unknown keys are ignored, so it can grow:
 *
 *     { "version_major": 1, "version_minor": 2, "version_revision": 3,
 *       "version_build": 45, "size": 687240,
 *       "image": "app-1.2.3-45.bin", "version": "1.2.3+45" }
 */

#include <zest/error.hpp>
#include <zest/function.hpp>
#include <zest/http_client.hpp>
#include <zest/image_version.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace zest
{

/** The manifest fields a device acts on. */
struct OtaManifest {
	std::int32_t version_major;
	std::int32_t version_minor;
	std::int32_t version_revision;
	std::int32_t version_build;
	/** Size of the image in bytes. */
	std::int32_t size;
	/** Image file name, relative to the manifest's directory. */
	char image[80];
	/** "major.minor.revision+build", for log lines only. */
	char version[40];

	/** The version as MCUboot orders it, or an error if a field is out of range. */
	[[nodiscard]] Result<ImageVersion> image_version() const noexcept
	{
		return ImageVersion::from_parts(version_major, version_minor, version_revision,
						version_build);
	}

	/** A view of @ref image up to its NUL. */
	[[nodiscard]] std::string_view image_name() const noexcept
	{
		return {image, std::strlen(image)};
	}
};

/**
 * Whether @p name is a bare file name safe to append to a directory URL: not
 * empty, no path separator, no query, no ".." anywhere. A manifest is untrusted,
 * so this is what stops one from naming another host or path.
 */
[[nodiscard]] constexpr bool is_plain_file_name(std::string_view name) noexcept
{
	return !name.empty() && name.find('/') == std::string_view::npos &&
	       name.find('?') == std::string_view::npos &&
	       name.find('#') == std::string_view::npos &&
	       name.find("..") == std::string_view::npos;
}

/**
 * Write the value of an HTTP `Range` header for @p length bytes at @p offset
 * ("bytes=0-2047") into @p destination, NUL-terminated. Returns the text, or an
 * error if it does not fit or @p length is zero.
 */
[[nodiscard]] inline Result<std::string_view>
format_range_header(std::span<char> destination, std::size_t offset, std::size_t length) noexcept
{
	if (length == 0U || destination.empty()) {
		return fail(errors::invalid_argument);
	}
	const int written = std::snprintf(destination.data(), destination.size(), "bytes=%zu-%zu",
					  offset, offset + length - 1U);
	if (written < 0 || static_cast<std::size_t>(written) >= destination.size()) {
		return fail(errors::no_buffer_space);
	}
	return std::string_view{destination.data(), static_cast<std::size_t>(written)};
}

/** Which step of an update fetch failed. */
enum class OtaStage : std::uint8_t {
	/** The request for the manifest failed (DNS, connect, timeout, ...). */
	manifest_request,
	/** The server answered the manifest request with a non-success status. */
	manifest_status,
	/** The manifest is not valid JSON. */
	manifest_parse,
	/** The manifest parsed but names nothing usable (no size, unsafe image name). */
	manifest_invalid,
	/** A range request failed. */
	image_request,
	/** A range came back with the wrong status or the wrong number of bytes. */
	image_range,
	/** The caller's sink refused a chunk (a flash write failed). */
	sink,
	/** The client could not allocate its working buffer. */
	no_memory,
};

[[nodiscard]] constexpr const char *to_string(OtaStage stage) noexcept
{
	switch (stage) {
	case OtaStage::manifest_request:
		return "manifest request";
	case OtaStage::manifest_status:
		return "manifest status";
	case OtaStage::manifest_parse:
		return "manifest parse";
	case OtaStage::manifest_invalid:
		return "manifest invalid";
	case OtaStage::image_request:
		return "image request";
	case OtaStage::image_range:
		return "image range";
	case OtaStage::sink:
		return "sink";
	case OtaStage::no_memory:
		return "no memory";
	}
	return "unknown";
}

/** A failure: which step, the underlying cause, and where in the image. */
struct OtaError {
	OtaStage stage;
	Error cause;
	/** The HTTP status, when the server answered with one; else 0. */
	std::uint16_t http_status{};
	/** For image_request, image_range and sink: the byte offset of the chunk. */
	std::size_t offset{};

	[[nodiscard]] const char *stage_name() const noexcept
	{
		return to_string(stage);
	}
	[[nodiscard]] std::string_view message() const noexcept
	{
		return cause.message();
	}
};

/** How an image download ended, when it did not fail. */
enum class OtaDownload : std::uint8_t {
	/** Every byte was handed to the sink; the last write was marked. */
	complete,
	/** The progress callback asked to stop. The sink has seen a partial image. */
	cancelled,
};

/**
 * A client for one update directory.
 *
 * Holds a keep-alive connection across the manifest fetch and the download, so
 * a few hundred range requests do not each open a socket --- which exhausts
 * `CONFIG_NET_MAX_CONN` long before the end of an image. Call @ref close when a
 * check is finished so no connection is held between checks: hours later the
 * server has long since dropped it, and a half-dead descriptor is what blocks
 * the next request past its timeout.
 *
 * Working buffers (the manifest, one chunk) are heap-allocated for the length
 * of each call and freed after, so an idle client costs nothing and a caller
 * pays no stack or static RAM for them. **Stack cost.** The HTTP request itself
 * still puts about `CONFIG_ZEST_HTTP_RECV_BUF_SIZE` plus `CONFIG_ZEST_HTTP_MAX_URL_LEN`
 * under the calling thread, so size its stack for that.
 */
class OtaClient
{
      public:
	struct Options {
		/** The update directory, with a trailing slash: "http://host/app/stable/". */
		std::string base_url;
		/** Bounds one manifest fetch or one range request. */
		std::chrono::milliseconds timeout{10'000};
		/** Empty selects "zest-ota". */
		std::string user_agent{};
	};

	/**
	 * Receives each chunk of the image, in order. @p last is true on the final
	 * one. Returning an error abandons the download. The chunk is only valid
	 * for the duration of the call.
	 */
	using Sink = FunctionRef<Result<>(std::span<const std::byte> chunk, bool last) noexcept>;

	/**
	 * Called before each chunk with bytes written so far and the image size
	 * (first with 0), and once more with both equal at the end. Returning false
	 * stops the download --- the place to yield to something more important, or
	 * to feed a watchdog.
	 */
	using Progress = FunctionRef<bool(std::size_t done, std::size_t total) noexcept>;

	explicit OtaClient(Options options) noexcept;

	OtaClient(const OtaClient &) = delete;
	OtaClient &operator=(const OtaClient &) = delete;

	/** Fetch and validate `manifest.json`. */
	[[nodiscard]] Result<OtaManifest, OtaError> fetch_manifest() noexcept;

	/**
	 * Stream the image @p manifest names into @p sink.
	 *
	 * Each range must answer 206 with exactly the bytes asked for: a server that
	 * ignores Range and sends 200 with the whole file is a failure, not a
	 * download, and so is a short read --- a truncated chunk written to flash
	 * corrupts the image.
	 */
	[[nodiscard]] Result<OtaDownload, OtaError> download(const OtaManifest &manifest, Sink sink,
							     Progress progress) noexcept;

	/** Drop the kept-alive connection. Safe to call at any time. */
	void close() noexcept;

      private:
	std::string base_url_;
	HttpClient client_;
};

} /* namespace zest */
