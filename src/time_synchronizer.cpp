/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/sntp_client.hpp>

#include <cerrno>
#include <chrono>
#include <expected>
#include <string_view>
#include <time.h>

namespace zest
{

std::expected<NetworkTime, int>
TimeSynchronizer::synchronize(std::string_view server,
			      std::chrono::milliseconds timeout) const noexcept
{
	auto result = client_.query(server, timeout);
	if (!result) {
		return std::unexpected(result.error());
	}

	const auto since_epoch = result->time.time_since_epoch();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
	const auto nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - seconds);
	const timespec value{
		.tv_sec = static_cast<time_t>(seconds.count()),
		.tv_nsec = static_cast<long>(nanoseconds.count()),
	};
	if (clock_settime(CLOCK_REALTIME, &value) < 0) {
		return std::unexpected(errno == 0 ? -EIO : -errno);
	}
	return result;
}

} /* namespace zest */
