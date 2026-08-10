/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace zest
{

/** Result of an SNTP query. */
struct NetworkTime {
	std::chrono::system_clock::time_point time;
	std::chrono::microseconds response_delay;
};

/** One-shot SNTP client that does not touch the system clock. */
class SntpClient
{
      public:
	[[nodiscard]] Result<NetworkTime>
	query(std::string_view server,
	      std::chrono::milliseconds timeout = std::chrono::seconds{10}) const noexcept;
};

/** Query SNTP and update CLOCK_REALTIME. */
class TimeSynchronizer
{
      public:
	[[nodiscard]] Result<NetworkTime>
	synchronize(std::string_view server,
		    std::chrono::milliseconds timeout = std::chrono::seconds{10}) const noexcept;

      private:
	SntpClient client_;
};

} /* namespace zest */
