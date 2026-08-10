/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/network.hpp>

#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/socket.h>

#include <cerrno>
#include <cstddef>
#include <span>
#include <utility>

namespace zest
{
namespace
{

[[nodiscard]] Error socket_error() noexcept
{
	return errno == 0 ? errors::io_error : Error{-errno};
}

void close_descriptor(int &descriptor) noexcept
{
	if (descriptor >= 0) {
		zsock_close(descriptor);
		descriptor = -1;
	}
}

[[nodiscard]] Result<> open_descriptor(int &descriptor, int family, int type,
				       int protocol) noexcept
{
	if (descriptor >= 0) {
		return fail(errors::already);
	}
	descriptor = zsock_socket(family, type, protocol);
	if (descriptor < 0) {
		return fail(socket_error());
	}
	return {};
}

[[nodiscard]] Result<> set_timeout_option(int descriptor, int option,
					  std::chrono::milliseconds timeout) noexcept
{
	if (descriptor < 0) {
		return fail(errors::bad_descriptor);
	}
	zsock_timeval value{};
	value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
	value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
	if (zsock_setsockopt(descriptor, SOL_SOCKET, option, &value, sizeof(value)) < 0) {
		return fail(socket_error());
	}
	return {};
}

} /* namespace */

DnsError dns_error_from(int status) noexcept
{
	switch (status) {
	case DNS_EAI_BADFLAGS:
		return DnsError::bad_flags;
	case DNS_EAI_NONAME:
		return DnsError::no_name;
	case DNS_EAI_AGAIN:
		return DnsError::again;
	case DNS_EAI_FAIL:
		return DnsError::fail;
	case DNS_EAI_NODATA:
		return DnsError::no_data;
	case DNS_EAI_FAMILY:
		return DnsError::family;
	case DNS_EAI_SOCKTYPE:
		return DnsError::socket_type;
	case DNS_EAI_SERVICE:
		return DnsError::service;
	case DNS_EAI_ADDRFAMILY:
		return DnsError::address_family;
	case DNS_EAI_MEMORY:
		return DnsError::memory;
	case DNS_EAI_SYSTEM:
		return DnsError::system;
	case DNS_EAI_OVERFLOW:
		return DnsError::overflow;
	case DNS_EAI_CANCELED:
		return DnsError::canceled;
	default:
		break;
	}
	return DnsError::unknown;
}

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

Result<> UdpSocket::open(int family) noexcept
{
	return open_descriptor(descriptor_, family, SOCK_DGRAM, IPPROTO_UDP);
}

Result<> UdpSocket::bind(const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	if (zsock_bind(descriptor_, address.address(), address.length) < 0) {
		return fail(socket_error());
	}
	return {};
}

Result<> UdpSocket::connect(const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	if (zsock_connect(descriptor_, address.address(), address.length) < 0) {
		return fail(socket_error());
	}
	return {};
}

Result<std::size_t> UdpSocket::send(std::span<const std::byte> data) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	const ssize_t sent = zsock_send(descriptor_, data.data(), data.size(), 0);
	if (sent < 0) {
		return fail(socket_error());
	}
	return static_cast<std::size_t>(sent);
}

Result<std::size_t> UdpSocket::send_to(std::span<const std::byte> data,
				       const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	const ssize_t sent = zsock_sendto(descriptor_, data.data(), data.size(), 0,
					  address.address(), address.length);
	if (sent < 0) {
		return fail(socket_error());
	}
	return static_cast<std::size_t>(sent);
}

Result<std::size_t> UdpSocket::receive(std::span<std::byte> buffer) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	const ssize_t received = zsock_recv(descriptor_, buffer.data(), buffer.size(), 0);
	if (received < 0) {
		return fail(socket_error());
	}
	return static_cast<std::size_t>(received);
}

Result<> UdpSocket::set_receive_timeout(std::chrono::milliseconds timeout) noexcept
{
	return set_timeout_option(descriptor_, SO_RCVTIMEO, timeout);
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

Result<> TcpSocket::open(int family) noexcept
{
	return open_descriptor(descriptor_, family, SOCK_STREAM, IPPROTO_TCP);
}

Result<> TcpSocket::connect(const ResolvedAddress &address) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	if (zsock_connect(descriptor_, address.address(), address.length) < 0) {
		return fail(socket_error());
	}
	return {};
}

Result<std::size_t> TcpSocket::send_all(std::span<const std::byte> data) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}

	std::size_t total = 0U;
	while (total < data.size()) {
		const ssize_t sent =
			zsock_send(descriptor_, data.data() + total, data.size() - total, 0);
		if (sent < 0) {
			return fail(socket_error());
		}
		if (sent == 0) {
			return fail(errors::connection_reset);
		}
		total += static_cast<std::size_t>(sent);
	}
	return total;
}

Result<std::size_t> TcpSocket::receive(std::span<std::byte> buffer) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	const ssize_t received = zsock_recv(descriptor_, buffer.data(), buffer.size(), 0);
	if (received < 0) {
		return fail(socket_error());
	}
	return static_cast<std::size_t>(received);
}

Result<> TcpSocket::shutdown() noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	if (zsock_shutdown(descriptor_, ZSOCK_SHUT_RDWR) < 0) {
		return fail(socket_error());
	}
	return {};
}

Result<> TcpSocket::set_receive_timeout(std::chrono::milliseconds timeout) noexcept
{
	return set_timeout_option(descriptor_, SO_RCVTIMEO, timeout);
}

Result<> TcpSocket::set_no_delay(bool enabled) noexcept
{
	if (descriptor_ < 0) {
		return fail(errors::bad_descriptor);
	}
	const int value = enabled ? 1 : 0;
	if (zsock_setsockopt(descriptor_, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) < 0) {
		return fail(socket_error());
	}
	return {};
}

void TcpSocket::close() noexcept
{
	close_descriptor(descriptor_);
}

} /* namespace zest */
