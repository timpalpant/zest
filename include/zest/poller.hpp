/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/net/socket.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace zest
{

/** Events a poller can wait for or report. */
enum class PollEvent : std::uint16_t {
	none = 0U,
	readable = 1U << 0U,
	writable = 1U << 1U,
	error = 1U << 2U,
	hangup = 1U << 3U,
};

[[nodiscard]] constexpr PollEvent operator|(PollEvent lhs, PollEvent rhs) noexcept
{
	return static_cast<PollEvent>(static_cast<std::uint16_t>(lhs) |
				      static_cast<std::uint16_t>(rhs));
}

[[nodiscard]] constexpr bool has_event(PollEvent events, PollEvent event) noexcept
{
	return (static_cast<std::uint16_t>(events) & static_cast<std::uint16_t>(event)) != 0U;
}

/**
 * Wait on several sockets at once, with fixed storage.
 *
 * An event loop that owns an MQTT connection, a UDP listener and a control socket
 * needs one blocking call that covers all of them; without this it either polls
 * each in turn with a short timeout, which wastes power, or reaches for
 * `zsock_poll` directly and hand-manages the descriptor array.
 *
 * ```cpp
 * zest::Poller<3> poller;
 * (void)poller.add(client.poll_fd(), zest::PollEvent::readable);
 * if (auto ready = poller.wait(client.keepalive_time_left()); ready && *ready > 0) {
 *         if (zest::has_event(poller.events(0), zest::PollEvent::readable)) {
 *                 (void)client.input();
 *         }
 * }
 * (void)client.keep_alive();
 * ```
 */
template <std::size_t Capacity> class Poller
{
      public:
	static_assert(Capacity > 0U, "a poller needs room for at least one descriptor");

	/** Register @p descriptor for @p events. */
	[[nodiscard]] Result<> add(int descriptor, PollEvent events) noexcept
	{
		if (descriptor < 0) {
			return fail(errors::bad_descriptor);
		}
		if (count_ == Capacity) {
			return fail(errors::no_buffer_space);
		}
		short requested = 0;
		if (has_event(events, PollEvent::readable)) {
			requested |= ZSOCK_POLLIN;
		}
		if (has_event(events, PollEvent::writable)) {
			requested |= ZSOCK_POLLOUT;
		}
		entries_[count_] = zsock_pollfd{
			.fd = descriptor,
			.events = requested,
			.revents = 0,
		};
		++count_;
		return {};
	}

	/**
	 * Block until a registered descriptor is ready.
	 *
	 * Returns the number of ready descriptors, which is zero on timeout. A
	 * `milliseconds::max()` timeout waits indefinitely.
	 */
	[[nodiscard]] Result<std::size_t> wait(std::chrono::milliseconds timeout) noexcept
	{
		if (count_ == 0U) {
			return fail(errors::invalid_argument);
		}
		const int milliseconds =
			timeout == std::chrono::milliseconds::max()
				? -1
				: static_cast<int>(timeout.count() < 0 ? 0 : timeout.count());

		const int ready = zsock_poll(entries_.data(), static_cast<int>(count_), milliseconds);
		if (ready < 0) {
			return fail(errno == 0 ? errors::io_error.value() : -errno);
		}
		return static_cast<std::size_t>(ready);
	}

	/** Events reported for the descriptor registered at @p index. */
	[[nodiscard]] PollEvent events(std::size_t index) const noexcept
	{
		if (index >= count_) {
			return PollEvent::none;
		}
		const short reported = entries_[index].revents;
		PollEvent result = PollEvent::none;
		if ((reported & ZSOCK_POLLIN) != 0) {
			result = result | PollEvent::readable;
		}
		if ((reported & ZSOCK_POLLOUT) != 0) {
			result = result | PollEvent::writable;
		}
		if ((reported & ZSOCK_POLLERR) != 0) {
			result = result | PollEvent::error;
		}
		if ((reported & ZSOCK_POLLHUP) != 0) {
			result = result | PollEvent::hangup;
		}
		return result;
	}

	/** The descriptor registered at @p index. */
	[[nodiscard]] int descriptor(std::size_t index) const noexcept
	{
		return index < count_ ? entries_[index].fd : -1;
	}

	void clear() noexcept
	{
		entries_ = {};
		count_ = 0U;
	}

	[[nodiscard]] constexpr std::size_t size() const noexcept
	{
		return count_;
	}
	[[nodiscard]] static constexpr std::size_t capacity() noexcept
	{
		return Capacity;
	}

      private:
	std::array<zsock_pollfd, Capacity> entries_{};
	std::size_t count_{0U};
};

} /* namespace zest */
