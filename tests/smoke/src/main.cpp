#include <zest/battery_curve.hpp>
#include <zest/battery_monitor.hpp>
#include <zest/bluetooth_manager.hpp>
#include <zest/http_client.hpp>
#include <zest/wifi_manager.hpp>

#include <array>
#include <span>
#include <type_traits>
#include <utility>

constexpr std::array test_curve{
	zest::CurvePoint{4200, 100},
	zest::CurvePoint{3700, 10},
	zest::CurvePoint{3300, 0},
};
constexpr std::array invalid_curve{
	zest::CurvePoint{3700, 100},
	zest::CurvePoint{4200, 0},
};

static_assert(zest::estimate_charge_percent(4300, test_curve).value() == 100);
static_assert(zest::estimate_charge_percent(3950, test_curve).value() == 55);
static_assert(zest::estimate_charge_percent(3000, test_curve).value() == 0);
static_assert(zest::estimate_charge_percent(3900, invalid_curve).error() ==
	      zest::CurveError::invalid_voltage_order);
static_assert(std::is_nothrow_constructible_v<zest::BatteryMonitor, adc_dt_spec, std::int32_t,
					      std::int32_t>);
static_assert(noexcept(std::declval<const zest::BatteryMonitor &>().init()));
static_assert(noexcept(std::declval<const zest::BatteryMonitor &>().read_millivolts()));
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
