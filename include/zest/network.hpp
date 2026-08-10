/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zephyr/net/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>

namespace zest
{

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
};

/** Synchronous DNS resolver with fixed result storage. */
class DnsResolver
{
      public:
	template <std::size_t Capacity = 4U>
	[[nodiscard]] std::expected<ResolvedAddresses<Capacity>, int>
	resolve(std::string_view host, std::uint16_t port, SocketType type = SocketType::tcp,
		int family = AF_UNSPEC) const noexcept
	{
		if (host.empty() || host.size() > 253U) {
			return std::unexpected(DNS_EAI_NONAME);
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
			return std::unexpected(rc);
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
			return std::unexpected(DNS_EAI_NONAME);
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

	[[nodiscard]] std::expected<void, int> open(int family = AF_INET) noexcept;
	[[nodiscard]] std::expected<void, int> bind(const ResolvedAddress &address) noexcept;
	[[nodiscard]] std::expected<void, int> connect(const ResolvedAddress &address) noexcept;
	[[nodiscard]] std::expected<std::size_t, int>
	send(std::span<const std::byte> data) noexcept;
	[[nodiscard]] std::expected<std::size_t, int>
	send_to(std::span<const std::byte> data, const ResolvedAddress &address) noexcept;
	[[nodiscard]] std::expected<std::size_t, int> receive(std::span<std::byte> buffer) noexcept;
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

	[[nodiscard]] std::expected<void, int> open(int family = AF_INET) noexcept;
	[[nodiscard]] std::expected<void, int> connect(const ResolvedAddress &address) noexcept;
	[[nodiscard]] std::expected<std::size_t, int>
	send_all(std::span<const std::byte> data) noexcept;
	[[nodiscard]] std::expected<std::size_t, int> receive(std::span<std::byte> buffer) noexcept;
	[[nodiscard]] std::expected<void, int> shutdown() noexcept;
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
