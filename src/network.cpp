#include <zest/network.hpp>

#include <zephyr/net/socket.h>

#include <cerrno>
#include <cstddef>
#include <expected>
#include <span>
#include <utility>

namespace zest
{
namespace
{

[[nodiscard]] int socket_error() noexcept
{
	return errno == 0 ? -EIO : -errno;
}

void close_descriptor(int &descriptor) noexcept
{
	if (descriptor >= 0) {
		zsock_close(descriptor);
		descriptor = -1;
	}
}

[[nodiscard]] std::expected<void, int> open_descriptor(int &descriptor, int family, int type,
						       int protocol) noexcept
{
	if (descriptor >= 0) {
		return std::unexpected(-EALREADY);
	}
	descriptor = zsock_socket(family, type, protocol);
	if (descriptor < 0) {
		return std::unexpected(socket_error());
	}
	return {};
}

} /* namespace */

UdpSocket::~UdpSocket() noexcept
{
	close();
}

UdpSocket::UdpSocket(UdpSocket &&other) noexcept : descriptor_{std::exchange(other.descriptor_, -1)}
{
}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept
{
	if (this != &other) {
		close();
		descriptor_ = std::exchange(other.descriptor_, -1);
	}
	return *this;
}

std::expected<void, int> UdpSocket::open(int family) noexcept
{
	return open_descriptor(descriptor_, family, SOCK_DGRAM, IPPROTO_UDP);
}

std::expected<void, int> UdpSocket::bind(const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	if (zsock_bind(descriptor_, address.address(), address.length) < 0) {
		return std::unexpected(socket_error());
	}
	return {};
}

std::expected<void, int> UdpSocket::connect(const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	if (zsock_connect(descriptor_, address.address(), address.length) < 0) {
		return std::unexpected(socket_error());
	}
	return {};
}

std::expected<std::size_t, int> UdpSocket::send(std::span<const std::byte> data) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	const ssize_t sent = zsock_send(descriptor_, data.data(), data.size(), 0);
	if (sent < 0) {
		return std::unexpected(socket_error());
	}
	return static_cast<std::size_t>(sent);
}

std::expected<std::size_t, int> UdpSocket::send_to(std::span<const std::byte> data,
						   const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	const ssize_t sent = zsock_sendto(descriptor_, data.data(), data.size(), 0,
					  address.address(), address.length);
	if (sent < 0) {
		return std::unexpected(socket_error());
	}
	return static_cast<std::size_t>(sent);
}

std::expected<std::size_t, int> UdpSocket::receive(std::span<std::byte> buffer) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	const ssize_t received = zsock_recv(descriptor_, buffer.data(), buffer.size(), 0);
	if (received < 0) {
		return std::unexpected(socket_error());
	}
	return static_cast<std::size_t>(received);
}

void UdpSocket::close() noexcept
{
	close_descriptor(descriptor_);
}

TcpSocket::~TcpSocket() noexcept
{
	close();
}

TcpSocket::TcpSocket(TcpSocket &&other) noexcept : descriptor_{std::exchange(other.descriptor_, -1)}
{
}

TcpSocket &TcpSocket::operator=(TcpSocket &&other) noexcept
{
	if (this != &other) {
		close();
		descriptor_ = std::exchange(other.descriptor_, -1);
	}
	return *this;
}

std::expected<void, int> TcpSocket::open(int family) noexcept
{
	return open_descriptor(descriptor_, family, SOCK_STREAM, IPPROTO_TCP);
}

std::expected<void, int> TcpSocket::connect(const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	if (zsock_connect(descriptor_, address.address(), address.length) < 0) {
		return std::unexpected(socket_error());
	}
	return {};
}

std::expected<std::size_t, int> TcpSocket::send_all(std::span<const std::byte> data) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}

	std::size_t total = 0U;
	while (total < data.size()) {
		const ssize_t sent =
			zsock_send(descriptor_, data.data() + total, data.size() - total, 0);
		if (sent < 0) {
			return std::unexpected(socket_error());
		}
		if (sent == 0) {
			return std::unexpected(-ECONNRESET);
		}
		total += static_cast<std::size_t>(sent);
	}
	return total;
}

std::expected<std::size_t, int> TcpSocket::receive(std::span<std::byte> buffer) noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	const ssize_t received = zsock_recv(descriptor_, buffer.data(), buffer.size(), 0);
	if (received < 0) {
		return std::unexpected(socket_error());
	}
	return static_cast<std::size_t>(received);
}

std::expected<void, int> TcpSocket::shutdown() noexcept
{
	if (descriptor_ < 0) {
		return std::unexpected(-EBADF);
	}
	if (zsock_shutdown(descriptor_, ZSOCK_SHUT_RDWR) < 0) {
		return std::unexpected(socket_error());
	}
	return {};
}

void TcpSocket::close() noexcept
{
	close_descriptor(descriptor_);
}

} /* namespace zest */
