/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/sntp_client.hpp>

#include <cerrno>
#include <chrono>
#include <string_view>
#include <time.h>

namespace zest
{

Result<NetworkTime> TimeSynchronizer::synchronize(std::string_view server,
						  std::chrono::milliseconds timeout) const noexcept
{
	auto network_time_result = client_.query(server, timeout);
	if (!network_time_result) {
		return fail(network_time_result.error());
	}
	auto network_time = *network_time_result;

	const auto since_epoch = network_time.time.time_since_epoch();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
	const auto nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - seconds);
	const timespec value{
		.tv_sec = static_cast<time_t>(seconds.count()),
		.tv_nsec = static_cast<long>(nanoseconds.count()),
	};
	if (clock_settime(CLOCK_REALTIME, &value) < 0) {
		return fail(errno == 0 ? errors::io_error.value() : -errno);
	}
	return network_time;
}

} /* namespace zest */
