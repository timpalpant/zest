/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/json.hpp>
#include <zest/ota_client.hpp>
#include <zest/schema.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>

/*
 * Member order is field order. A publisher may write more keys than these (a
 * channel, a checksum, a timestamp); Zephyr's parser skips a key with no
 * matching field, so a manifest can grow without breaking a device already in
 * the field.
 */
ZEST_SCHEMA(zest::OtaManifest, ZEST_MEMBER(zest::OtaManifest, version_major),
	    ZEST_MEMBER(zest::OtaManifest, version_minor),
	    ZEST_MEMBER(zest::OtaManifest, version_revision),
	    ZEST_MEMBER(zest::OtaManifest, version_build), ZEST_MEMBER(zest::OtaManifest, size),
	    ZEST_MEMBER(zest::OtaManifest, image), ZEST_MEMBER(zest::OtaManifest, version));

namespace zest
{
namespace
{

constexpr std::size_t kChunkSize = CONFIG_ZEST_OTA_CHUNK_SIZE;
constexpr std::size_t kManifestBuffer = CONFIG_ZEST_OTA_MANIFEST_BUFFER_SIZE;

/* A range response is parsed into a buffer of this size, so it must hold a chunk. */
static_assert(CONFIG_ZEST_OTA_CHUNK_SIZE <= CONFIG_ZEST_HTTP_RECV_BUF_SIZE,
	      "ZEST_OTA_CHUNK_SIZE must not exceed ZEST_HTTP_RECV_BUF_SIZE");

HttpClient::Options client_options(const OtaClient::Options &options)
{
	return HttpClient::Options{
		.timeout = options.timeout,
		.user_agent =
			options.user_agent.empty() ? std::string{"zest-ota"} : options.user_agent,
		/* Hundreds of ranges back to back ride one pooled socket. */
		.keep_alive = true,
		/* A range that comes back short is a hard failure, not a flag to
		 * inspect. */
		.truncation_is_error = true,
	};
}

/* base_url + name into a NUL-terminated URL buffer. */
Result<std::string_view> join_url(std::span<char> destination, std::string_view base,
				  std::string_view name) noexcept
{
	const int written = std::snprintf(destination.data(), destination.size(), "%.*s%.*s",
					  static_cast<int>(base.size()), base.data(),
					  static_cast<int>(name.size()), name.data());
	if (written < 0 || static_cast<std::size_t>(written) >= destination.size()) {
		return fail(errors::name_too_long);
	}
	return std::string_view{destination.data(), static_cast<std::size_t>(written)};
}

} /* namespace */

OtaClient::OtaClient(Options options) noexcept
	: base_url_{std::move(options.base_url)}, client_{client_options(options)}
{
}

void OtaClient::close() noexcept
{
	client_.close();
}

Result<OtaManifest, OtaError> OtaClient::fetch_manifest() noexcept
{
	char url_buffer[CONFIG_ZEST_HTTP_MAX_URL_LEN];
	const auto url = join_url(url_buffer, base_url_, "manifest.json");
	if (!url) {
		return std::unexpected(OtaError{OtaStage::manifest_request, url.error()});
	}

	const std::unique_ptr<char[]> text{new (std::nothrow) char[kManifestBuffer]};
	if (!text) {
		return std::unexpected(OtaError{OtaStage::no_memory, errors::no_memory});
	}

	const auto response =
		client_.get(*url, std::as_writable_bytes(std::span{text.get(), kManifestBuffer}));
	if (!response) {
		return std::unexpected(
			OtaError{OtaStage::manifest_request, response.error().cause});
	}
	if (!response->is_success()) {
		return std::unexpected(OtaError{OtaStage::manifest_status, errors::bad_message,
						response->status_code});
	}

	const auto parsed =
		json::parse<OtaManifest>(std::span<char>{text.get(), response->body.size()});
	if (!parsed) {
		return std::unexpected(OtaError{OtaStage::manifest_parse, parsed.error()});
	}
	const OtaManifest &manifest = parsed->value;
	if (manifest.size <= 0 || !is_plain_file_name(manifest.image_name())) {
		return std::unexpected(OtaError{OtaStage::manifest_invalid, errors::bad_message});
	}
	return manifest;
}

Result<OtaDownload, OtaError> OtaClient::download(const OtaManifest &manifest, Sink sink,
						  Progress progress) noexcept
{
	if (manifest.size <= 0 || !is_plain_file_name(manifest.image_name())) {
		return std::unexpected(OtaError{OtaStage::manifest_invalid, errors::bad_message});
	}
	char url_buffer[CONFIG_ZEST_HTTP_MAX_URL_LEN];
	const auto url = join_url(url_buffer, base_url_, manifest.image_name());
	if (!url) {
		return std::unexpected(OtaError{OtaStage::image_request, url.error()});
	}

	const std::unique_ptr<std::byte[]> chunk{new (std::nothrow) std::byte[kChunkSize]};
	if (!chunk) {
		return std::unexpected(OtaError{OtaStage::no_memory, errors::no_memory});
	}

	const auto total = static_cast<std::size_t>(manifest.size);
	std::size_t offset = 0;
	while (offset < total) {
		if (!progress(offset, total)) {
			return OtaDownload::cancelled;
		}

		const std::size_t want = std::min(kChunkSize, total - offset);
		char range_buffer[40];
		const auto range = format_range_header(range_buffer, offset, want);
		if (!range) {
			return std::unexpected(
				OtaError{OtaStage::image_request, range.error(), 0, offset});
		}
		const HttpHeader headers[] = {{"Range", *range}};

		const auto response =
			client_.get(*url, std::span<std::byte>{chunk.get(), kChunkSize}, headers);
		if (!response) {
			return std::unexpected(OtaError{OtaStage::image_request,
							response.error().cause, 0, offset});
		}
		/* 206 is what a range answers with. A server that ignores Range sends
		 * 200 and the whole file, which does not fit `want` and fails above. */
		if (response->status_code != 206U || response->body.size() != want) {
			return std::unexpected(OtaError{OtaStage::image_range, errors::bad_message,
							response->status_code, offset});
		}

		const bool last = (offset + want) >= total;
		if (const auto accepted = sink(response->body, last); !accepted) {
			return std::unexpected(
				OtaError{OtaStage::sink, accepted.error(), 0, offset});
		}
		offset += want;
	}
	(void)progress(total, total);
	return OtaDownload::complete;
}

} /* namespace zest */
