#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>

namespace zest
{

/** Result of an SNTP query. */
struct NetworkTime {
	std::chrono::system_clock::time_point time;
	std::chrono::microseconds response_delay;
};

/** One-shot SNTP client. */
class SntpClient
{
      public:
	[[nodiscard]] std::expected<NetworkTime, int>
	query(std::string_view server,
	      std::chrono::milliseconds timeout = std::chrono::seconds{10}) const noexcept;
};

/** Query SNTP and update CLOCK_REALTIME. */
class TimeSynchronizer
{
      public:
	[[nodiscard]] std::expected<NetworkTime, int>
	synchronize(std::string_view server,
		    std::chrono::milliseconds timeout = std::chrono::seconds{10}) const noexcept;

      private:
	SntpClient client_;
};

} /* namespace zest */
