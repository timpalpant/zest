/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(CONFIG_ZEST_HTTP_CLIENT_TLS)
#include <zephyr/net/tls_credentials.h>
#endif

namespace zest
{

enum class HttpMethod {
	get,
	head,
	post,
	put,
	patch,
	delete_,
};

struct HttpHeader {
	std::string_view name;
	std::string_view value;
};

/** An owning header used for configuration retained by an HttpClient. */
struct OwnedHttpHeader {
	std::string name;
	std::string value;
};

/** Which stage of the request failed. */
enum class HttpErrorStage {
	invalid_url,
	dns,
	socket,
	tls_configuration,
	connect,
	request,
	response_too_large,
};

[[nodiscard]] const char *to_string(HttpErrorStage stage) noexcept;

/**
 * A request failure: which stage, and the underlying cause.
 *
 * The stage stays a separate type rather than collapsing into `Error`, because
 * "DNS failed" and "the socket failed" carry information a bare errno does not.
 */
struct HttpError {
	HttpErrorStage stage;
	Error cause;

	/** A short description of the stage. */
	[[nodiscard]] const char *stage_name() const noexcept
	{
		return to_string(stage);
	}
	/** A short description of the underlying cause. */
	[[nodiscard]] std::string_view message() const noexcept
	{
		return cause.message();
	}
};

/** The result of an HTTP request. */
struct HttpResponse {
	std::uint16_t status_code{};
	std::span<const std::byte> body{};
	std::size_t content_length{};
	/**
	 * Set when the body did not fit the caller's buffer.
	 *
	 * Check it: a truncated response still reports a 200, so treating
	 * `ok() == true` as "I have the whole body" is wrong. Set
	 * `Options::truncation_is_error` to have this reported as a failure instead.
	 */
	bool body_truncated{};

	[[nodiscard]] bool is_success() const noexcept
	{
		return status_code >= 200U && status_code < 300U;
	}

	[[nodiscard]] bool is_redirect() const noexcept
	{
		return status_code >= 300U && status_code < 400U;
	}

	[[nodiscard]] std::string_view text() const noexcept
	{
		return {reinterpret_cast<const char *>(body.data()), body.size()};
	}
};

struct HttpRequest {
	/** All views are borrowed only for the duration of the synchronous request call. */
	HttpMethod method{HttpMethod::get};
	std::string_view url;
	std::span<const HttpHeader> headers{};
	std::span<const std::byte> body{};
	std::string_view content_type{};
};

/**
 * Synchronous HTTP/1.1 and HTTPS client with caller-owned response storage.
 *
 * **Stack cost.** `request()` needs roughly `CONFIG_ZEST_HTTP_RECV_BUF_SIZE` plus
 * `CONFIG_ZEST_HTTP_MAX_URL_LEN` bytes of the calling thread's stack, about 1.6 KB
 * at the defaults. A 2048-byte thread will overflow; size the thread accordingly
 * or lower the Kconfig values.
 */
class HttpClient
{
      public:
	enum class PeerVerification {
		none,
		optional,
		required,
	};

	struct Options {
		/** Retained configuration owns its variable-length strings and collections. */
		std::chrono::milliseconds timeout{15'000};
		/** Empty selects Zest's default User-Agent. */
		std::string user_agent{};
		std::vector<OwnedHttpHeader> default_headers{};
		/**
		 * Keep the connection open for a following request to the same host.
		 *
		 * A device posting telemetry on a timer otherwise pays a fresh TCP
		 * and TLS handshake every time, which on a battery is the dominant
		 * energy cost of reporting.
		 */
		bool keep_alive{false};
		/** Report a body that did not fit as a failure rather than a flag. */
		bool truncation_is_error{false};
#if defined(CONFIG_ZEST_HTTP_CLIENT_TLS)
		PeerVerification peer_verification{PeerVerification::required};
		std::vector<sec_tag_t> security_tags{};
#endif
	};
	/*
	 * Constructing or changing retained string/vector configuration may allocate.
	 * request() itself uses caller storage and fixed internal buffers.
	 */

	/** Construct with Zest's default User-Agent and no allocated storage. */
	HttpClient() noexcept;
	/** Move preconfigured options into the client without further allocation. */
	explicit HttpClient(Options options) noexcept;
	~HttpClient() noexcept;

	HttpClient(const HttpClient &) = delete;
	HttpClient &operator=(const HttpClient &) = delete;

	[[nodiscard]] Result<HttpResponse, HttpError>
	request(const HttpRequest &request, std::span<std::byte> response_buffer) noexcept;

	[[nodiscard]] Result<HttpResponse, HttpError>
	get(std::string_view url, std::span<std::byte> response_buffer,
	    std::span<const HttpHeader> headers = {}) noexcept;

	[[nodiscard]] Result<HttpResponse, HttpError>
	post(std::string_view url, std::span<const std::byte> body,
	     std::span<std::byte> response_buffer,
	     std::string_view content_type = "application/octet-stream",
	     std::span<const HttpHeader> headers = {}) noexcept;

	[[nodiscard]] Result<HttpResponse, HttpError>
	put(std::string_view url, std::span<const std::byte> body,
	    std::span<std::byte> response_buffer,
	    std::string_view content_type = "application/octet-stream",
	    std::span<const HttpHeader> headers = {}) noexcept;

	[[nodiscard]] Result<HttpResponse, HttpError>
	patch(std::string_view url, std::span<const std::byte> body,
	      std::span<std::byte> response_buffer,
	      std::string_view content_type = "application/octet-stream",
	      std::span<const HttpHeader> headers = {}) noexcept;

	[[nodiscard]] Result<HttpResponse, HttpError>
	delete_request(std::string_view url, std::span<std::byte> response_buffer,
		       std::span<const HttpHeader> headers = {}) noexcept;

	/** Close any connection kept open by `keep_alive`. */
	void close() noexcept;

	[[nodiscard]] const Options &options() const noexcept
	{
		return options_;
	}

      private:
	struct Connection;

	Options options_;
	/* Host and port of the pooled connection, when one is held. */
	int pooled_descriptor_{-1};
	std::array<char, 254> pooled_host_{};
	std::uint16_t pooled_port_{0U};
	bool pooled_tls_{false};
};

} /* namespace zest */
