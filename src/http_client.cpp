/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/http_client.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>

#include <zephyr/net/http/client.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

namespace zest
{
namespace
{

#if defined(CONFIG_ZEST_HTTP_RECV_BUF_SIZE)
constexpr std::size_t http_receive_chunk = CONFIG_ZEST_HTTP_RECV_BUF_SIZE;
#else
constexpr std::size_t http_receive_chunk = 1024;
#endif

#if defined(CONFIG_ZEST_HTTP_MAX_URL_LEN)
constexpr std::size_t max_url_length = CONFIG_ZEST_HTTP_MAX_URL_LEN;
#else
constexpr std::size_t max_url_length = 512;
#endif

constexpr std::size_t max_host_length = 253;
constexpr std::size_t max_path_length = max_url_length - 128;

[[nodiscard]] HttpError error_at(HttpErrorStage stage, int code) noexcept
{
	return HttpError{stage, Error{code}};
}

[[nodiscard]] HttpError error_at(HttpErrorStage stage, Error cause) noexcept
{
	return HttpError{stage, cause};
}

struct ParsedUrl {
	bool tls{};
	bool explicit_port{};
	std::uint16_t port_value{};
	std::array<char, max_host_length + 1> host{};
	std::array<char, 6> port{};
	std::array<char, max_path_length + 1> path{};
};

struct RequestContext {
	std::span<const HttpHeader> default_headers;
	std::span<const HttpHeader> headers;
	std::string_view user_agent;
	std::span<std::byte> output;
	bool keep_alive{};
	std::size_t output_size{};
	std::size_t content_length{};
	std::uint16_t status_code{};
	bool truncated{};
};

class Socket final
{
      public:
	explicit Socket(int descriptor = -1) noexcept : descriptor_{descriptor}
	{
	}
	~Socket() noexcept
	{
		if (descriptor_ >= 0) {
			zsock_close(descriptor_);
		}
	}

	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;

	Socket(Socket &&other) noexcept : descriptor_{other.release()}
	{
	}
	Socket &operator=(Socket &&other) noexcept
	{
		if (this != &other) {
			if (descriptor_ >= 0) {
				zsock_close(descriptor_);
			}
			descriptor_ = other.release();
		}
		return *this;
	}

	[[nodiscard]] int get() const noexcept
	{
		return descriptor_;
	}
	[[nodiscard]] int release() noexcept
	{
		const int result = descriptor_;
		descriptor_ = -1;
		return result;
	}

      private:
	int descriptor_;
};

[[nodiscard]] std::expected<ParsedUrl, HttpError> parse_url(std::string_view url) noexcept
{
	if (url.empty() || url.size() > max_url_length) {
		return std::unexpected(
			error_at(HttpErrorStage::invalid_url, errors::invalid_argument));
	}

	ParsedUrl result;
	std::string_view remainder;
	if (url.starts_with("https://")) {
		result.tls = true;
		remainder = url.substr(8);
		std::memcpy(result.port.data(), "443", 4);
		result.port_value = 443U;
	} else if (url.starts_with("http://")) {
		result.tls = false;
		remainder = url.substr(7);
		std::memcpy(result.port.data(), "80", 3);
		result.port_value = 80U;
	} else {
		return std::unexpected(
			error_at(HttpErrorStage::invalid_url, errors::protocol_not_supported));
	}

	const auto path_position = remainder.find_first_of("/?#");
	const auto authority = remainder.substr(0, path_position);
	std::string_view path = path_position == std::string_view::npos
					? std::string_view{"/"}
					: remainder.substr(path_position);

	/* Fragments are client-side URL syntax and must not be sent to the server. */
	if (const auto fragment = path.find('#'); fragment != std::string_view::npos) {
		path = path.substr(0, fragment);
	}
	if (path.empty() || path.front() != '/') {
		return std::unexpected(
			error_at(HttpErrorStage::invalid_url, errors::invalid_argument));
	}

	std::string_view host = authority;
	std::string_view port_text;
	if (!authority.empty() && authority.front() == '[') {
		const auto bracket = authority.find(']');
		if (bracket == std::string_view::npos) {
			return std::unexpected(
				error_at(HttpErrorStage::invalid_url, errors::invalid_argument));
		}
		host = authority.substr(1, bracket - 1);
		if (bracket + 1 < authority.size()) {
			if (authority[bracket + 1] != ':') {
				return std::unexpected(error_at(HttpErrorStage::invalid_url,
								errors::invalid_argument));
			}
			port_text = authority.substr(bracket + 2);
		}
	} else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
		host = authority.substr(0, colon);
		port_text = authority.substr(colon + 1);
	}

	if (!port_text.empty()) {
		if (port_text.size() >= result.port.size()) {
			return std::unexpected(
				error_at(HttpErrorStage::invalid_url, errors::invalid_argument));
		}
		std::uint32_t value = 0U;
		for (const char digit : port_text) {
			if (digit < '0' || digit > '9') {
				return std::unexpected(error_at(HttpErrorStage::invalid_url,
								errors::invalid_argument));
			}
			value = value * 10U + static_cast<std::uint32_t>(digit - '0');
			if (value > 65535U) {
				return std::unexpected(error_at(HttpErrorStage::invalid_url,
								errors::invalid_argument));
			}
		}
		std::ranges::copy(port_text, result.port.begin());
		result.port[port_text.size()] = '\0';
		result.port_value = static_cast<std::uint16_t>(value);
		result.explicit_port = true;
	}

	if (host.empty() || host.size() > max_host_length || path.size() > max_path_length) {
		return std::unexpected(
			error_at(HttpErrorStage::invalid_url, errors::name_too_long));
	}
	if (authority.find('@') != std::string_view::npos) {
		/*
		 * Credentials in URLs are rejected to avoid accidental disclosure
		 * through logs and redirects.
		 */
		return std::unexpected(
			error_at(HttpErrorStage::invalid_url, errors::invalid_argument));
	}

	std::ranges::copy(host, result.host.begin());
	result.host[host.size()] = '\0';
	std::ranges::copy(path, result.path.begin());
	result.path[path.size()] = '\0';
	return result;
}

[[nodiscard]] enum http_method to_zephyr_method(HttpMethod method) noexcept
{
	switch (method) {
	case HttpMethod::get:
		return HTTP_GET;
	case HttpMethod::head:
		return HTTP_HEAD;
	case HttpMethod::post:
		return HTTP_POST;
	case HttpMethod::put:
		return HTTP_PUT;
	case HttpMethod::patch:
		return HTTP_PATCH;
	case HttpMethod::delete_:
		return HTTP_DELETE;
	}
	return HTTP_GET;
}

int send_all(int socket, std::string_view data) noexcept
{
	while (!data.empty()) {
		const auto sent = zsock_send(socket, data.data(), data.size(), 0);
		if (sent < 0) {
			return -errno;
		}
		if (sent == 0) {
			return -ECONNRESET;
		}
		data.remove_prefix(static_cast<std::size_t>(sent));
	}
	return 0;
}

int optional_headers_callback(int socket, struct http_request *, void *user_data) noexcept
{
	auto &context = *static_cast<RequestContext *>(user_data);
	int bytes_sent = 0;

	auto send_part = [&](std::string_view part) -> bool {
		const int result = send_all(socket, part);
		if (result != 0) {
			bytes_sent = result;
			return false;
		}
		bytes_sent += static_cast<int>(part.size());
		return true;
	};

	if (!context.user_agent.empty()) {
		if (!send_part("User-Agent: ") || !send_part(context.user_agent) ||
		    !send_part("\r\n")) {
			return bytes_sent;
		}
	}

	auto send_headers = [&](std::span<const HttpHeader> headers) -> bool {
		for (const auto &header : headers) {
			if (header.name.empty() ||
			    header.name.find_first_of("\r\n:") != std::string_view::npos ||
			    header.value.find_first_of("\r\n") != std::string_view::npos) {
				bytes_sent = -EINVAL;
				return false;
			}
			if (!send_part(header.name) || !send_part(": ") ||
			    !send_part(header.value) || !send_part("\r\n")) {
				return false;
			}
		}
		return true;
	};
	if (!send_headers(context.default_headers) || !send_headers(context.headers)) {
		return bytes_sent;
	}

	if (!send_part(context.keep_alive ? "Connection: keep-alive\r\n"
					  : "Connection: close\r\n")) {
		return bytes_sent;
	}
	return bytes_sent;
}

int response_callback(struct http_response *response, enum http_final_call,
		      void *user_data) noexcept
{
	auto &context = *static_cast<RequestContext *>(user_data);
	context.status_code = response->http_status_code;
	context.content_length = response->content_length;

	if (response->body_frag_start != nullptr && response->body_frag_len != 0U) {
		const std::size_t available = context.output.size() - context.output_size;
		const std::size_t copied = std::min(available, response->body_frag_len);
		if (copied != 0U) {
			std::memcpy(context.output.data() + context.output_size,
				    response->body_frag_start, copied);
		}
		context.output_size += copied;
		context.truncated = context.truncated || copied != response->body_frag_len;
	}
	return 0;
}

[[nodiscard]] std::expected<Socket, HttpError>
connect_socket(const ParsedUrl &url, const HttpClient::Options &options) noexcept
{
#if !defined(CONFIG_ZEST_HTTP_CLIENT_TLS)
	if (url.tls) {
		return std::unexpected(
			error_at(HttpErrorStage::tls_configuration, errors::not_supported));
	}
#endif

	struct zsock_addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct zsock_addrinfo *addresses = nullptr;
	const int dns_result =
		zsock_getaddrinfo(url.host.data(), url.port.data(), &hints, &addresses);
	if (dns_result != 0 || addresses == nullptr) {
		return std::unexpected(
			error_at(HttpErrorStage::dns,
				 dns_result != 0 ? Error{dns_result} : errors::host_unreachable));
	}

	Error last_error = errors::host_unreachable;
	HttpErrorStage last_stage = HttpErrorStage::connect;
	for (auto *address = addresses; address != nullptr; address = address->ai_next) {
#if defined(CONFIG_ZEST_HTTP_CLIENT_TLS)
		const int protocol =
			url.tls ? static_cast<int>(IPPROTO_TLS_1_2) : static_cast<int>(IPPROTO_TCP);
#else
		const int protocol = static_cast<int>(IPPROTO_TCP);
#endif
		Socket socket{zsock_socket(address->ai_family, SOCK_STREAM, protocol)};
		if (socket.get() < 0) {
			last_error = Error{-errno};
			last_stage = HttpErrorStage::socket;
			continue;
		}

#if defined(CONFIG_ZEST_HTTP_CLIENT_TLS)
		if (url.tls) {
			int verification = TLS_PEER_VERIFY_REQUIRED;
			switch (options.peer_verification) {
			case HttpClient::PeerVerification::none:
				verification = TLS_PEER_VERIFY_NONE;
				break;
			case HttpClient::PeerVerification::optional:
				verification = TLS_PEER_VERIFY_OPTIONAL;
				break;
			case HttpClient::PeerVerification::required:
				if (options.security_tags.empty()) {
					zsock_freeaddrinfo(addresses);
					return std::unexpected(
						error_at(HttpErrorStage::tls_configuration,
							 errors::not_found));
				}
				break;
			}

			if (zsock_setsockopt(socket.get(), SOL_TLS, TLS_PEER_VERIFY, &verification,
					     sizeof(verification)) < 0) {
				last_error = Error{-errno};
				last_stage = HttpErrorStage::tls_configuration;
				continue;
			}
			if (!options.security_tags.empty() &&
			    zsock_setsockopt(socket.get(), SOL_TLS, TLS_SEC_TAG_LIST,
					     options.security_tags.data(),
					     options.security_tags.size_bytes()) < 0) {
				last_error = Error{-errno};
				last_stage = HttpErrorStage::tls_configuration;
				continue;
			}
			if (zsock_setsockopt(socket.get(), SOL_TLS, TLS_HOSTNAME, url.host.data(),
					     std::strlen(url.host.data()) + 1U) < 0) {
				last_error = Error{-errno};
				last_stage = HttpErrorStage::tls_configuration;
				continue;
			}
		}
#else
		(void)options;
#endif

		if (zsock_connect(socket.get(), address->ai_addr, address->ai_addrlen) == 0) {
			zsock_freeaddrinfo(addresses);
			return socket;
		}
		last_error = Error{-errno};
		last_stage = HttpErrorStage::connect;
	}

	zsock_freeaddrinfo(addresses);
	return std::unexpected(error_at(last_stage, last_error));
}

} /* namespace */

HttpClient::HttpClient() noexcept = default;

HttpClient::HttpClient(Options options) noexcept : options_{options}
{
}

HttpClient::~HttpClient() noexcept
{
	close();
}

void HttpClient::close() noexcept
{
	if (pooled_descriptor_ >= 0) {
		zsock_close(pooled_descriptor_);
		pooled_descriptor_ = -1;
	}
	pooled_port_ = 0U;
	pooled_tls_ = false;
	pooled_host_ = {};
}

HttpResult<HttpResponse> HttpClient::request(const HttpRequest &request,
					     std::span<std::byte> response_buffer) noexcept
{
	auto parsed = parse_url(request.url);
	if (!parsed) {
		return std::unexpected(parsed.error());
	}

	/* Reuse a pooled connection when it targets the same origin. */
	Socket socket;
	bool reused = false;
	if (options_.keep_alive && pooled_descriptor_ >= 0 && pooled_port_ == parsed->port_value &&
	    pooled_tls_ == parsed->tls &&
	    std::strncmp(pooled_host_.data(), parsed->host.data(), pooled_host_.size()) == 0) {
		socket = Socket{pooled_descriptor_};
		pooled_descriptor_ = -1;
		reused = true;
	} else {
		close();
		auto fresh = connect_socket(*parsed, options_);
		if (!fresh) {
			return std::unexpected(fresh.error());
		}
		socket = std::move(*fresh);
	}

	RequestContext context{
		.default_headers = options_.default_headers,
		.headers = request.headers,
		.user_agent = options_.user_agent,
		.output = response_buffer,
		.keep_alive = options_.keep_alive,
	};
	std::array<std::uint8_t, http_receive_chunk> receive_buffer{};

	struct http_request zephyr_request{};
	zephyr_request.method = to_zephyr_method(request.method);
	zephyr_request.url = parsed->path.data();
	zephyr_request.protocol = "HTTP/1.1";
	zephyr_request.host = parsed->host.data();
	zephyr_request.port = parsed->explicit_port ? parsed->port.data() : nullptr;
	zephyr_request.response = response_callback;
	zephyr_request.recv_buf = receive_buffer.data();
	zephyr_request.recv_buf_len = receive_buffer.size();
	zephyr_request.optional_headers_cb = optional_headers_callback;
	zephyr_request.payload = request.body.empty()
					 ? nullptr
					 : reinterpret_cast<const char *>(request.body.data());
	zephyr_request.payload_len = request.body.size();

	std::array<char, 96> content_type{};
	if (!request.content_type.empty()) {
		if (request.content_type.size() >= content_type.size()) {
			return std::unexpected(
				error_at(HttpErrorStage::request, errors::name_too_long));
		}
		std::ranges::copy(request.content_type, content_type.begin());
		zephyr_request.content_type_value = content_type.data();
	}

	const auto timeout_count = options_.timeout.count();
	const auto timeout =
		static_cast<std::int32_t>(std::clamp<std::int64_t>(timeout_count, 1, INT32_MAX));
	const int result = http_client_req(socket.get(), &zephyr_request, timeout, &context);
	if (result < 0) {
		/* A reused connection the peer had already closed deserves one retry. */
		if (reused) {
			close();
			return this->request(request, response_buffer);
		}
		return std::unexpected(error_at(HttpErrorStage::request, Error{result}));
	}

	if (context.truncated && options_.truncation_is_error) {
		return std::unexpected(
			error_at(HttpErrorStage::response_too_large, errors::message_size));
	}

	if (options_.keep_alive) {
		pooled_descriptor_ = socket.release();
		pooled_port_ = parsed->port_value;
		pooled_tls_ = parsed->tls;
		pooled_host_ = {};
		std::memcpy(pooled_host_.data(), parsed->host.data(),
			    std::strlen(parsed->host.data()));
	}

	HttpResponse response{
		.status_code = context.status_code,
		.body = response_buffer.first(context.output_size),
		.content_length = context.content_length,
		.body_truncated = context.truncated,
	};
	return response;
}

HttpResult<HttpResponse> HttpClient::request(const HttpRequest &request,
					     std::span<std::byte> response_buffer,
					     HeaderHandler on_header) noexcept
{
	/*
	 * Zephyr's client exposes headers only through its own callback shape, which
	 * has no user-data slot in this version. Until that lands, the handler is
	 * accepted for API stability and the request proceeds without it.
	 */
	(void)on_header;
	return this->request(request, response_buffer);
}

HttpResult<HttpResponse> HttpClient::get(std::string_view url, std::span<std::byte> response_buffer,
					 std::span<const HttpHeader> headers) noexcept
{
	return request(HttpRequest{.method = HttpMethod::get, .url = url, .headers = headers},
		       response_buffer);
}

HttpResult<HttpResponse> HttpClient::post(std::string_view url, std::span<const std::byte> body,
					  std::span<std::byte> response_buffer,
					  std::string_view content_type,
					  std::span<const HttpHeader> headers) noexcept
{
	return request(HttpRequest{.method = HttpMethod::post,
				   .url = url,
				   .headers = headers,
				   .body = body,
				   .content_type = content_type},
		       response_buffer);
}

HttpResult<HttpResponse> HttpClient::put(std::string_view url, std::span<const std::byte> body,
					 std::span<std::byte> response_buffer,
					 std::string_view content_type,
					 std::span<const HttpHeader> headers) noexcept
{
	return request(HttpRequest{.method = HttpMethod::put,
				   .url = url,
				   .headers = headers,
				   .body = body,
				   .content_type = content_type},
		       response_buffer);
}

HttpResult<HttpResponse> HttpClient::patch(std::string_view url, std::span<const std::byte> body,
					   std::span<std::byte> response_buffer,
					   std::string_view content_type,
					   std::span<const HttpHeader> headers) noexcept
{
	return request(HttpRequest{.method = HttpMethod::patch,
				   .url = url,
				   .headers = headers,
				   .body = body,
				   .content_type = content_type},
		       response_buffer);
}

HttpResult<HttpResponse> HttpClient::delete_request(std::string_view url,
						    std::span<std::byte> response_buffer,
						    std::span<const HttpHeader> headers) noexcept
{
	return request(HttpRequest{.method = HttpMethod::delete_, .url = url, .headers = headers},
		       response_buffer);
}

void HttpClient::set_timeout(std::chrono::milliseconds timeout) noexcept
{
	options_.timeout = timeout;
}

void HttpClient::set_user_agent(std::string_view user_agent) noexcept
{
	options_.user_agent = user_agent;
}

void HttpClient::set_keep_alive(bool enabled) noexcept
{
	options_.keep_alive = enabled;
	if (!enabled) {
		close();
	}
}

#if defined(CONFIG_ZEST_HTTP_CLIENT_TLS)
void HttpClient::set_peer_verification(PeerVerification verification,
				       std::span<const sec_tag_t> security_tags) noexcept
{
	options_.peer_verification = verification;
	options_.security_tags = security_tags;
}
#endif

const char *to_string(HttpErrorStage stage) noexcept
{
	switch (stage) {
	case HttpErrorStage::invalid_url:
		return "invalid URL";
	case HttpErrorStage::dns:
		return "DNS lookup";
	case HttpErrorStage::socket:
		return "socket creation";
	case HttpErrorStage::tls_configuration:
		return "TLS configuration";
	case HttpErrorStage::connect:
		return "connection";
	case HttpErrorStage::request:
		return "HTTP request";
	case HttpErrorStage::response_too_large:
		return "response too large";
	}
	return "unknown";
}

} /* namespace zest */
