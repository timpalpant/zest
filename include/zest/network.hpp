/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/net/socket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>

namespace zest
{

/**
 * A name-resolution failure.
 *
 * DNS keeps its own type rather than folding into `Error`, because Zephyr's
 * `enum dns_resolve_status` values collide numerically with errno: `DNS_EAI_NONAME`
 * is `-2`, which as an errno reads as `-ENOENT`, and `DNS_EAI_MEMORY` is `-12`,
 * which reads as `-ENOMEM`. Returning them as a bare `int` made the two
 * indistinguishable.
 */
enum class DnsError {
	bad_flags,
	no_name,
	again,
	fail,
	no_data,
	family,
	socket_type,
	service,
	address_family,
	memory,
	system,
	overflow,
	canceled,
	unknown,
};

/** A short, static description of a resolution failure. */
[[nodiscard]] constexpr const char *to_string(DnsError error) noexcept
{
	switch (error) {
	case DnsError::bad_flags:
		return "invalid resolver flags";
	case DnsError::no_name:
		return "name not found";
	case DnsError::again:
		return "temporary resolver failure";
	case DnsError::fail:
		return "permanent resolver failure";
	case DnsError::no_data:
		return "name has no address";
	case DnsError::family:
		return "address family not supported";
	case DnsError::socket_type:
		return "socket type not supported";
	case DnsError::service:
		return "service not supported";
	case DnsError::address_family:
		return "no address in the requested family";
	case DnsError::memory:
		return "resolver out of memory";
	case DnsError::system:
		return "resolver system error";
	case DnsError::overflow:
		return "resolver buffer overflow";
	case DnsError::canceled:
		return "resolution canceled or timed out";
	case DnsError::unknown:
		break;
	}
	return "unknown resolver error";
}

/** Translate a Zephyr `enum dns_resolve_status` into a `DnsError`. */
[[nodiscard]] DnsError dns_error_from(int status) noexcept;

/** The result of a resolution attempt. */
template <typename T> using DnsResult = std::expected<T, DnsError>;

/** Socket transport requested from DnsResolver. */
enum class SocketType {
	tcp,
	udp,
};

/** One resolved socket address. */
struct ResolvedAddress {
	sockaddr_storage storage{};
	socklen_t length{};

	[[nodiscard]] constexpr const sockaddr *address() const noexcept
	{
		return reinterpret_cast<const sockaddr *>(&storage);
	}
	[[nodiscard]] constexpr sa_family_t family() const noexcept
	{
		return storage.ss_family;
	}
};

/** Fixed-capacity collection of DNS results. */
template <std::size_t Capacity = 4U> struct ResolvedAddresses {
	std::array<ResolvedAddress, Capacity> entries{};
	std::size_t count{};

	[[nodiscard]] constexpr std::span<const ResolvedAddress> values() const noexcept
	{
		return {entries.data(), count};
	}
	[[nodiscard]] constexpr bool empty() const noexcept
	{
		return count == 0U;
	}
	/** The first result, which is the one a client normally connects to. */
	[[nodiscard]] constexpr const ResolvedAddress &front() const noexcept
	{
		return entries[0];
	}
};

/** Synchronous DNS resolver with fixed result storage. */
class DnsResolver
{
      public:
	template <std::size_t Capacity = 4U>
	[[nodiscard]] DnsResult<ResolvedAddresses<Capacity>>
	resolve(std::string_view host, std::uint16_t port, SocketType type = SocketType::tcp,
		int family = AF_UNSPEC) const noexcept
	{
		if (host.empty() || host.size() > 253U) {
			return std::unexpected(DnsError::no_name);
		}

		std::array<char, 254> name{};
		std::copy(host.begin(), host.end(), name.begin());

		zsock_addrinfo hints{};
		hints.ai_family = family;
		hints.ai_socktype = type == SocketType::tcp ? SOCK_STREAM : SOCK_DGRAM;
		hints.ai_protocol = type == SocketType::tcp ? IPPROTO_TCP : IPPROTO_UDP;

		zsock_addrinfo *results = nullptr;
		const int rc = zsock_getaddrinfo(name.data(), nullptr, &hints, &results);
		if (rc != 0) {
			return std::unexpected(dns_error_from(rc));
		}

		ResolvedAddresses<Capacity> output{};
		for (auto *entry = results; entry != nullptr && output.count < Capacity;
		     entry = entry->ai_next) {
			if (entry->ai_addr == nullptr ||
			    entry->ai_addrlen > sizeof(sockaddr_storage)) {
				continue;
			}

			auto &destination = output.entries[output.count];
			std::memcpy(&destination.storage, entry->ai_addr, entry->ai_addrlen);
			destination.length = entry->ai_addrlen;
			if (destination.family() == AF_INET) {
				reinterpret_cast<sockaddr_in *>(&destination.storage)->sin_port =
					htons(port);
			} else if (destination.family() == AF_INET6) {
				reinterpret_cast<sockaddr_in6 *>(&destination.storage)->sin6_port =
					htons(port);
			} else {
				continue;
			}
			++output.count;
		}
		zsock_freeaddrinfo(results);

		if (output.count == 0U) {
			return std::unexpected(DnsError::no_data);
		}
		return output;
	}
};

/** Move-only UDP socket. */
class UdpSocket
{
      public:
	UdpSocket() noexcept = default;
	~UdpSocket() noexcept;
	UdpSocket(const UdpSocket &) = delete;
	UdpSocket &operator=(const UdpSocket &) = delete;
	UdpSocket(UdpSocket &&other) noexcept;
	UdpSocket &operator=(UdpSocket &&other) noexcept;

	[[nodiscard]] Result<> open(int family = AF_INET) noexcept;
	[[nodiscard]] Result<> bind(const ResolvedAddress &address) noexcept;
	[[nodiscard]] Result<> connect(const ResolvedAddress &address) noexcept;
	[[nodiscard]] Result<std::size_t> send(std::span<const std::byte> data) noexcept;
	[[nodiscard]] Result<std::size_t> send_to(std::span<const std::byte> data,
						  const ResolvedAddress &address) noexcept;
	[[nodiscard]] Result<std::size_t> receive(std::span<std::byte> buffer) noexcept;

	/** Bound the time a blocking receive may take. */
	[[nodiscard]] Result<> set_receive_timeout(std::chrono::milliseconds timeout) noexcept;

	void close() noexcept;

	[[nodiscard]] constexpr bool is_open() const noexcept
	{
		return descriptor_ >= 0;
	}
	[[nodiscard]] constexpr int native_handle() const noexcept
	{
		return descriptor_;
	}

      private:
	int descriptor_{-1};
};

/** Move-only connected TCP socket. */
class TcpSocket
{
      public:
	TcpSocket() noexcept = default;
	~TcpSocket() noexcept;
	TcpSocket(const TcpSocket &) = delete;
	TcpSocket &operator=(const TcpSocket &) = delete;
	TcpSocket(TcpSocket &&other) noexcept;
	TcpSocket &operator=(TcpSocket &&other) noexcept;

	[[nodiscard]] Result<> open(int family = AF_INET) noexcept;
	[[nodiscard]] Result<> connect(const ResolvedAddress &address) noexcept;
	[[nodiscard]] Result<std::size_t> send_all(std::span<const std::byte> data) noexcept;
	[[nodiscard]] Result<std::size_t> receive(std::span<std::byte> buffer) noexcept;
	[[nodiscard]] Result<> shutdown() noexcept;

	[[nodiscard]] Result<> set_receive_timeout(std::chrono::milliseconds timeout) noexcept;
	/** Disable Nagle, for a request/response protocol that sends small frames. */
	[[nodiscard]] Result<> set_no_delay(bool enabled) noexcept;

	void close() noexcept;

	[[nodiscard]] constexpr bool is_open() const noexcept
	{
		return descriptor_ >= 0;
	}
	[[nodiscard]] constexpr int native_handle() const noexcept
	{
		return descriptor_;
	}

      private:
	int descriptor_{-1};
};

} /* namespace zest */
