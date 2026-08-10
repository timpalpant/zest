#include <zest/battery_monitor.hpp>
#include <zest/bluetooth_manager.hpp>
#include <zest/http_client.hpp>
#include <zest/wifi_manager.hpp>

#include <span>
#include <type_traits>
#include <utility>

static_assert(std::is_nothrow_constructible_v<zest::BatteryMonitor, adc_dt_spec, std::int32_t,
					      std::int32_t, std::span<const zest::CurvePoint>>);
static_assert(noexcept(std::declval<const zest::BatteryMonitor &>().init()));
static_assert(noexcept(std::declval<const zest::BatteryMonitor &>().read()));
static_assert(std::is_nothrow_default_constructible_v<zest::WifiManager>);
static_assert(noexcept(std::declval<zest::WifiManager &>().status()));
static_assert(std::is_nothrow_default_constructible_v<zest::HttpClient>);
static_assert(noexcept(std::declval<zest::HttpClient &>().request(
	std::declval<const zest::HttpRequest &>(), std::declval<std::span<std::byte>>())));
static_assert(std::is_nothrow_default_constructible_v<zest::BluetoothManager>);

int main()
{
	return 0;
}
