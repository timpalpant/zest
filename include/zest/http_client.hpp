/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef ZEST_HTTP_CLIENT_HPP_
#define ZEST_HTTP_CLIENT_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <zephyr/net/tls_credentials.h>

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

enum class HttpErrorStage {
	invalid_url,
	dns,
	socket,
	tls_configuration,
	connect,
	request,
	response_too_large,
};

struct HttpError {
	HttpErrorStage stage;
	int code;
};

struct HttpResponse {
	std::uint16_t status_code{};
	std::span<const std::byte> body{};
	std::size_t content_length{};
	bool body_truncated{};

	[[nodiscard]] bool ok() const noexcept
	{
		return status_code >= 200U && status_code < 400U;
	}

	[[nodiscard]] std::string_view text() const noexcept
	{
		return {reinterpret_cast<const char *>(body.data()), body.size()};
	}
};

struct HttpRequest {
	HttpMethod method{HttpMethod::get};
	std::string_view url;
	std::span<const HttpHeader> headers{};
	std::span<const std::byte> body{};
	std::string_view content_type{};
};

class HttpClient
{
      public:
	enum class PeerVerification {
		none,
		optional,
		required,
	};

	struct Options {
		std::chrono::milliseconds timeout{15'000};
		std::string_view user_agent{"zephyr-http-client/1.0"};
		std::span<const HttpHeader> default_headers{};
		PeerVerification peer_verification{PeerVerification::required};
		std::span<const sec_tag_t> security_tags{};
	};

	HttpClient() noexcept;
	explicit HttpClient(Options options) noexcept;

	[[nodiscard]] std::expected<HttpResponse, HttpError>
	request(const HttpRequest &request, std::span<std::byte> response_buffer) noexcept;

	[[nodiscard]] std::expected<HttpResponse, HttpError>
	get(std::string_view url, std::span<std::byte> response_buffer,
	    std::span<const HttpHeader> headers = {}) noexcept;

	[[nodiscard]] std::expected<HttpResponse, HttpError>
	post(std::string_view url, std::span<const std::byte> body,
	     std::span<std::byte> response_buffer,
	     std::string_view content_type = "application/octet-stream",
	     std::span<const HttpHeader> headers = {}) noexcept;
	[[nodiscard]] std::expected<HttpResponse, HttpError>
	put(std::string_view url, std::span<const std::byte> body,
	    std::span<std::byte> response_buffer,
	    std::string_view content_type = "application/octet-stream",
	    std::span<const HttpHeader> headers = {}) noexcept;
	[[nodiscard]] std::expected<HttpResponse, HttpError>
	patch(std::string_view url, std::span<const std::byte> body,
	      std::span<std::byte> response_buffer,
	      std::string_view content_type = "application/octet-stream",
	      std::span<const HttpHeader> headers = {}) noexcept;
	[[nodiscard]] std::expected<HttpResponse, HttpError>
	delete_request(std::string_view url, std::span<std::byte> response_buffer,
		       std::span<const HttpHeader> headers = {}) noexcept;

	void set_timeout(std::chrono::milliseconds timeout) noexcept;
	void set_user_agent(std::string_view user_agent) noexcept;
	void set_peer_verification(PeerVerification verification,
				   std::span<const sec_tag_t> security_tags = {}) noexcept;

	[[nodiscard]] const Options &options() const noexcept
	{
		return options_;
	}

      private:
	Options options_;
};

[[nodiscard]] const char *to_string(HttpErrorStage stage) noexcept;

} // namespace zest

#endif
