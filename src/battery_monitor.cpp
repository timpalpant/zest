/* Battery voltage measurement through a resistive divider. */

#include <zest/battery_monitor.hpp>

#include <cerrno>
#include <cstdint>
#include <expected>

namespace zest
{
std::expected<void, Error> BatteryMonitor::init() const noexcept
{
	return divider_.init();
}

std::expected<std::int32_t, Error> BatteryMonitor::read_millivolts() const noexcept
{
	return divider_.read_millivolts<kOversample>();
}

} /* namespace zest */
