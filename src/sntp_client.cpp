/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/sntp_client.hpp>

#include <zephyr/net/sntp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

namespace zest
{

Result<NetworkTime> SntpClient::query(std::string_view server,
				      std::chrono::milliseconds timeout) const noexcept
{
	if (server.empty() || server.size() > 253U || timeout < std::chrono::milliseconds::zero()) {
		return fail(errors::invalid_argument);
	}

	std::array<char, 254> name{};
	std::copy(server.begin(), server.end(), name.begin());
	const auto bounded_timeout =
		std::min<std::uint64_t>(static_cast<std::uint64_t>(timeout.count()),
					std::numeric_limits<std::uint32_t>::max());

	sntp_time result{};
	ZEST_TRY(check(
		sntp_simple(name.data(), static_cast<std::uint32_t>(bounded_timeout), &result)));

	constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
	const auto fractional_nanoseconds =
		(static_cast<std::uint64_t>(result.fraction) * kNanosecondsPerSecond) >> 32U;
	return NetworkTime{
		std::chrono::system_clock::time_point{
			std::chrono::seconds{result.seconds} +
			std::chrono::nanoseconds{fractional_nanoseconds}},
		std::chrono::microseconds{result.rsp_delay_us},
	};
}

} /* namespace zest */
