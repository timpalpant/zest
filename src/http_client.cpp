/* SPDX-License-Identifier: Apache-2.0 */

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

constexpr std::size_t max_url_length = 512;
constexpr std::size_t max_host_length = 253;
constexpr std::size_t max_path_length = 384;
constexpr std::size_t http_receive_chunk = 1024;

struct ParsedUrl {
	bool tls{};
	bool explicit_port{};
	std::array<char, max_host_length + 1> host{};
	std::array<char, 6> port{};
	std::array<char, max_path_length + 1> path{};
};

struct RequestContext {
	std::span<const HttpHeader> default_headers;
	std::span<const HttpHeader> headers;
	std::string_view user_agent;
	std::span<std::byte> output;
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
	~Socket()
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

[[nodiscard]] std::expected<ParsedUrl, HttpError> parse_url(std::string_view url)
{
	if (url.empty() || url.size() > max_url_length) {
		return std::unexpected(HttpError{HttpErrorStage::invalid_url, -EINVAL});
	}

	ParsedUrl result;
	std::string_view remainder;
	if (url.starts_with("https://")) {
		result.tls = true;
		remainder = url.substr(8);
		std::memcpy(result.port.data(), "443", 4);
	} else if (url.starts_with("http://")) {
		result.tls = false;
		remainder = url.substr(7);
		std::memcpy(result.port.data(), "80", 3);
	} else {
		return std::unexpected(HttpError{HttpErrorStage::invalid_url, -EPROTONOSUPPORT});
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
		return std::unexpected(HttpError{HttpErrorStage::invalid_url, -EINVAL});
	}

	std::string_view host = authority;
	if (!authority.empty() && authority.front() == '[') {
		const auto bracket = authority.find(']');
		if (bracket == std::string_view::npos) {
			return std::unexpected(HttpError{HttpErrorStage::invalid_url, -EINVAL});
		}
		host = authority.substr(1, bracket - 1);
		if (bracket + 1 < authority.size()) {
			if (authority[bracket + 1] != ':') {
				return std::unexpected(
					HttpError{HttpErrorStage::invalid_url, -EINVAL});
			}
			const auto port = authority.substr(bracket + 2);
			if (port.empty() || port.size() >= result.port.size()) {
				return std::unexpected(
					HttpError{HttpErrorStage::invalid_url, -EINVAL});
			}
			std::ranges::copy(port, result.port.begin());
			result.port[port.size()] = '\0';
			result.explicit_port = true;
		}
	} else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
		host = authority.substr(0, colon);
		const auto port = authority.substr(colon + 1);
		if (port.empty() || port.size() >= result.port.size()) {
			return std::unexpected(HttpError{HttpErrorStage::invalid_url, -EINVAL});
		}
		std::ranges::copy(port, result.port.begin());
		result.port[port.size()] = '\0';
		result.explicit_port = true;
	}

	if (host.empty() || host.size() > max_host_length || path.size() > max_path_length) {
		return std::unexpected(HttpError{HttpErrorStage::invalid_url, -ENAMETOOLONG});
	}
	if (authority.find('@') != std::string_view::npos) {
		/* Credentials in URLs are deliberately rejected to avoid accidental
		 * disclosure through logs and redirects.
		 */
		return std::unexpected(HttpError{HttpErrorStage::invalid_url, -EINVAL});
	}

	std::ranges::copy(host, result.host.begin());
	result.host[host.size()] = '\0';
	std::ranges::copy(path, result.path.begin());
	result.path[path.size()] = '\0';
	return result;
}

[[nodiscard]] enum http_method to_zephyr_method(HttpMethod method)
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

int send_all(int socket, std::string_view data)
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

int optional_headers_callback(int socket, struct http_request *, void *user_data)
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

	if (!send_part("Connection: close\r\n")) {
		return bytes_sent;
	}
	return bytes_sent;
}

int response_callback(struct http_response *response, enum http_final_call, void *user_data)
{
	auto &context = *static_cast<RequestContext *>(user_data);
	context.status_code = response->http_status_code;
	context.content_length = response->content_length;

	if (response->body_frag_start != nullptr && response->body_frag_len != 0U) {
		const std::size_t available = context.output.size() - context.output_size;
		const std::size_t copied = std::min(available, response->body_frag_len);
		std::memcpy(context.output.data() + context.output_size, response->body_frag_start,
			    copied);
		context.output_size += copied;
		context.truncated = context.truncated || copied != response->body_frag_len;
	}
	return 0;
}

[[nodiscard]] std::expected<Socket, HttpError> connect_socket(const ParsedUrl &url,
							      const HttpClient::Options &options)
{
	struct zsock_addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct zsock_addrinfo *addresses = nullptr;
	const int dns_result =
		zsock_getaddrinfo(url.host.data(), url.port.data(), &hints, &addresses);
	if (dns_result != 0 || addresses == nullptr) {
		return std::unexpected(HttpError{HttpErrorStage::dns,
						 dns_result != 0 ? dns_result : -EHOSTUNREACH});
	}

	int last_error = -EHOSTUNREACH;
	HttpErrorStage last_stage = HttpErrorStage::connect;
	for (auto *address = addresses; address != nullptr; address = address->ai_next) {
		const int protocol =
			url.tls ? static_cast<int>(IPPROTO_TLS_1_2) : static_cast<int>(IPPROTO_TCP);
		Socket socket{zsock_socket(address->ai_family, SOCK_STREAM, protocol)};
		if (socket.get() < 0) {
			last_error = -errno;
			last_stage = HttpErrorStage::socket;
			continue;
		}

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
					return std::unexpected(HttpError{
						HttpErrorStage::tls_configuration, -ENOENT});
				}
				break;
			}

			if (zsock_setsockopt(socket.get(), SOL_TLS, TLS_PEER_VERIFY, &verification,
					     sizeof(verification)) < 0) {
				last_error = -errno;
				last_stage = HttpErrorStage::tls_configuration;
				continue;
			}
			if (!options.security_tags.empty() &&
			    zsock_setsockopt(socket.get(), SOL_TLS, TLS_SEC_TAG_LIST,
					     options.security_tags.data(),
					     options.security_tags.size_bytes()) < 0) {
				last_error = -errno;
				last_stage = HttpErrorStage::tls_configuration;
				continue;
			}
			if (zsock_setsockopt(socket.get(), SOL_TLS, TLS_HOSTNAME, url.host.data(),
					     std::strlen(url.host.data()) + 1U) < 0) {
				last_error = -errno;
				last_stage = HttpErrorStage::tls_configuration;
				continue;
			}
		}

		if (zsock_connect(socket.get(), address->ai_addr, address->ai_addrlen) == 0) {
			zsock_freeaddrinfo(addresses);
			return socket;
		}
		last_error = -errno;
		last_stage = HttpErrorStage::connect;
	}

	zsock_freeaddrinfo(addresses);
	return std::unexpected(HttpError{last_stage, last_error});
}

} // namespace

HttpClient::HttpClient() noexcept = default;

HttpClient::HttpClient(Options options) noexcept : options_{options}
{
}

std::expected<HttpResponse, HttpError> HttpClient::request(const HttpRequest &request,
							   std::span<std::byte> response_buffer)
{
	auto parsed = parse_url(request.url);
	if (!parsed) {
		return std::unexpected(parsed.error());
	}

	auto socket = connect_socket(*parsed, options_);
	if (!socket) {
		return std::unexpected(socket.error());
	}

	RequestContext context{
		.default_headers = options_.default_headers,
		.headers = request.headers,
		.user_agent = options_.user_agent,
		.output = response_buffer,
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
	zephyr_request.payload = reinterpret_cast<const char *>(request.body.data());
	zephyr_request.payload_len = request.body.size();

	std::array<char, 96> content_type{};
	if (!request.content_type.empty()) {
		if (request.content_type.size() >= content_type.size()) {
			return std::unexpected(HttpError{HttpErrorStage::request, -ENAMETOOLONG});
		}
		std::ranges::copy(request.content_type, content_type.begin());
		zephyr_request.content_type_value = content_type.data();
	}

	const auto timeout_count = options_.timeout.count();
	const auto timeout =
		static_cast<std::int32_t>(std::clamp<std::int64_t>(timeout_count, 1, INT32_MAX));
	const int result = http_client_req(socket->get(), &zephyr_request, timeout, &context);
	if (result < 0) {
		return std::unexpected(HttpError{HttpErrorStage::request, result});
	}

	HttpResponse response{
		.status_code = context.status_code,
		.body = response_buffer.first(context.output_size),
		.content_length = context.content_length,
		.body_truncated = context.truncated,
	};
	return response;
}

std::expected<HttpResponse, HttpError> HttpClient::get(std::string_view url,
						       std::span<std::byte> response_buffer,
						       std::span<const HttpHeader> headers)
{
	return request(HttpRequest{.method = HttpMethod::get, .url = url, .headers = headers},
		       response_buffer);
}

std::expected<HttpResponse, HttpError> HttpClient::post(std::string_view url,
							std::span<const std::byte> body,
							std::span<std::byte> response_buffer,
							std::string_view content_type,
							std::span<const HttpHeader> headers)
{
	return request(HttpRequest{.method = HttpMethod::post,
				   .url = url,
				   .headers = headers,
				   .body = body,
				   .content_type = content_type},
		       response_buffer);
}

std::expected<HttpResponse, HttpError> HttpClient::put(std::string_view url,
						       std::span<const std::byte> body,
						       std::span<std::byte> response_buffer,
						       std::string_view content_type,
						       std::span<const HttpHeader> headers)
{
	return request(HttpRequest{.method = HttpMethod::put,
				   .url = url,
				   .headers = headers,
				   .body = body,
				   .content_type = content_type},
		       response_buffer);
}

std::expected<HttpResponse, HttpError> HttpClient::patch(std::string_view url,
							 std::span<const std::byte> body,
							 std::span<std::byte> response_buffer,
							 std::string_view content_type,
							 std::span<const HttpHeader> headers)
{
	return request(HttpRequest{.method = HttpMethod::patch,
				   .url = url,
				   .headers = headers,
				   .body = body,
				   .content_type = content_type},
		       response_buffer);
}

std::expected<HttpResponse, HttpError>
HttpClient::delete_request(std::string_view url, std::span<std::byte> response_buffer,
			   std::span<const HttpHeader> headers)
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

void HttpClient::set_peer_verification(PeerVerification verification,
				       std::span<const sec_tag_t> security_tags) noexcept
{
	options_.peer_verification = verification;
	options_.security_tags = security_tags;
}

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

} // namespace zest
