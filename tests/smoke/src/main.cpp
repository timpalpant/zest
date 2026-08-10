/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/adc_channel.hpp>
#include <zest/battery_curve.hpp>
#include <zest/battery_monitor.hpp>
#include <zest/bluetooth_manager.hpp>
#include <zest/button.hpp>
#include <zest/certificate_store.hpp>
#include <zest/filters.hpp>
#include <zest/gpio.hpp>
#include <zest/http_client.hpp>
#include <zest/led_pattern.hpp>
#include <zest/kernel.hpp>
#if defined(CONFIG_ZEST_MQTT_CLIENT)
#include <zest/mqtt_client.hpp>
#endif
#if defined(CONFIG_ZEST_NETWORK)
#include <zest/network.hpp>
#endif
#if defined(CONFIG_ZEST_NETWORK_MONITOR)
#include <zest/network_monitor.hpp>
#endif
#include <zest/pwm.hpp>
#include <zest/retry.hpp>
#include <zest/retained_value.hpp>
#include <zest/sensor.hpp>
#include <zest/settings.hpp>
#include <zest/sntp_client.hpp>
#include <zest/system.hpp>
#include <zest/provisioning_manager.hpp>
#include <zest/timing.hpp>
#include <zest/transforms.hpp>
#include <zest/voltage_divider.hpp>
#include <zest/wifi_manager.hpp>

#include <array>
#include <chrono>
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

constexpr bool behavior_utilities_work()
{
	zest::MovingAverage<int, 3> average;
	if (average.update(3) != 3 || average.update(6) != 4 || average.update(9) != 6 ||
	    average.update(12) != 9 || !average.full()) {
		return false;
	}

	zest::MedianFilter<int, 3> median;
	if (median.update(9) != 9 || median.update(1) != 1 || median.update(5) != 5) {
		return false;
	}

	zest::ExponentialMovingAverage<int> exponential{0.5};
	if (exponential.update(10) != 10.0 || exponential.update(20) != 15.0) {
		return false;
	}

	zest::Hysteresis<int> alarm{10, 20};
	if (alarm.update(15) || !alarm.update(10) || !alarm.update(15) || alarm.update(20)) {
		return false;
	}

	constexpr zest::Calibration<int> calibration{2.0, -1.0};
	if (calibration.apply(4) != 7.0) {
		return false;
	}
	constexpr zest::LinearMap<int> mapping{0, 10, 0.0, 100.0};
	if (mapping.map(5).value() != 50.0) {
		return false;
	}

	using namespace std::chrono_literals;
	using Clock = std::chrono::steady_clock;
	zest::RateLimiter<Clock> limiter{10ms};
	const Clock::time_point start{};
	if (!limiter.allow(start) || limiter.allow(start + 5ms) || !limiter.allow(start + 10ms)) {
		return false;
	}

	zest::Debouncer<bool, Clock> debouncer{10ms};
	if (debouncer.update(true, start).changed || debouncer.update(true, start + 5ms).changed ||
	    !debouncer.update(true, start + 10ms).changed || !debouncer.value()) {
		return false;
	}

	return true;
}

static_assert(behavior_utilities_work());

constexpr bool retry_works()
{
	zest::RetryPolicy retry{{
		.maximum_attempts = 3,
		.initial_delay = std::chrono::milliseconds{10},
		.maximum_delay = std::chrono::milliseconds{20},
		.multiplier = 2.0,
	}};
	const auto first = retry.failure();
	const auto second = retry.failure();
	const auto exhausted = retry.failure();
	return first == std::chrono::milliseconds{10} && second == std::chrono::milliseconds{20} &&
	       !exhausted && retry.attempts() == 3U;
}

static_assert(retry_works());
static_assert(std::is_nothrow_constructible_v<zest::Settings<>, std::string_view>);
#if defined(CONFIG_ZEST_NETWORK)
static_assert(std::is_nothrow_default_constructible_v<zest::DnsResolver>);
static_assert(std::is_nothrow_default_constructible_v<zest::UdpSocket>);
static_assert(std::is_nothrow_move_constructible_v<zest::TcpSocket>);
static_assert(noexcept(std::declval<zest::TcpSocket &>().close()));
#endif
static_assert(noexcept(std::declval<const zest::SntpClient &>().query(
	std::declval<std::string_view>(), std::declval<std::chrono::milliseconds>())));
static_assert(std::is_nothrow_constructible_v<zest::BatteryMonitor, adc_dt_spec, std::int32_t,
					      std::int32_t>);
static_assert(std::is_nothrow_constructible_v<zest::VoltageDivider, adc_dt_spec, std::int32_t,
					      std::int32_t>);
static_assert(std::is_nothrow_constructible_v<zest::AdcChannel, adc_dt_spec>);
static_assert(noexcept(std::declval<const zest::AdcChannel &>().init()));
static_assert(noexcept(std::declval<const zest::AdcChannel &>().read_raw()));
static_assert(noexcept(std::declval<const zest::AdcChannel &>().read_millivolts()));
static_assert(noexcept(std::declval<const zest::AdcChannel &>().read_average_millivolts<16>()));
static_assert(std::is_nothrow_constructible_v<zest::GpioInput, gpio_dt_spec>);
static_assert(std::is_nothrow_constructible_v<zest::GpioOutput, gpio_dt_spec>);
static_assert(noexcept(std::declval<const zest::GpioInput &>().get()));
static_assert(noexcept(std::declval<const zest::GpioOutput &>().set(zest::GpioState::active)));
static_assert(std::is_nothrow_constructible_v<zest::PwmOutput, pwm_dt_spec>);
static_assert(noexcept(std::declval<const zest::PwmOutput &>().set_duty_cycle(0.5)));
static_assert(std::is_nothrow_constructible_v<zest::Button<>, gpio_dt_spec, zest::ButtonConfig>);
static_assert(std::is_nothrow_constructible_v<zest::LedPatternPlayer<>, gpio_dt_spec>);
static_assert(std::is_nothrow_constructible_v<zest::SensorReader, const rtio_iodev &, rtio &,
					      const sensor_decoder_api &>);
static_assert(std::is_nothrow_move_constructible_v<zest::AsyncSensorFrame>);
static_assert(noexcept(std::declval<const zest::BatteryMonitor &>().init()));
static_assert(noexcept(std::declval<const zest::BatteryMonitor &>().read_millivolts()));
static_assert(std::is_nothrow_default_constructible_v<zest::WifiManager>);
static_assert(noexcept(std::declval<zest::WifiManager &>().status()));
static_assert(std::is_nothrow_default_constructible_v<zest::HttpClient>);
static_assert(noexcept(std::declval<zest::HttpClient &>().request(
	std::declval<const zest::HttpRequest &>(), std::declval<std::span<std::byte>>())));
static_assert(std::is_nothrow_default_constructible_v<zest::BluetoothManager>);
static_assert(std::is_nothrow_default_constructible_v<zest::MessageQueue<int, 4>>);
static_assert(std::is_nothrow_constructible_v<zest::WorkItem, zest::WorkItem::Handler, void *>);
static_assert(std::is_nothrow_default_constructible_v<zest::PeriodicTimer>);
static_assert(std::is_nothrow_default_constructible_v<zest::StaticThread<1024>>);
#if defined(CONFIG_ZEST_NETWORK_MONITOR)
static_assert(std::is_nothrow_constructible_v<zest::NetworkMonitor, net_if *>);
#endif
#if defined(CONFIG_ZEST_MQTT_CLIENT)
static_assert(std::is_nothrow_default_constructible_v<zest::MqttClient<>>);
#endif
static_assert(std::is_nothrow_default_constructible_v<zest::CertificateStore<2048>>);
static_assert(std::is_nothrow_constructible_v<zest::ProvisioningManager<>, std::string_view>);

bool retained_value_works()
{
	zest::RetainedStorage<int> storage{};
	zest::RetainedValue<int> retained{storage};
	if (retained.valid() || retained.value_or(7) != 7) {
		return false;
	}
	retained.store(42);
	return retained.valid() && retained.value_or(0) == 42;
}

#if defined(CONFIG_ZEST_MQTT_CLIENT)
[[maybe_unused]] void mqtt_api_compiles(zest::MqttClient<> &client,
					const zest::ResolvedAddress &broker,
					std::span<const std::byte> payload)
{
	(void)client.configure(broker, {.client_id = "zest-smoke"});
	(void)client.connect();
	(void)client.publish("zest/test", payload);
	(void)client.subscribe("zest/test");
	(void)client.input();
	(void)client.live();
	(void)client.disconnect();
#if defined(CONFIG_MQTT_LIB_TLS)
	constexpr std::array<sec_tag_t, 1> tags{1};
	(void)client.use_tls(tags, "broker.example.com");
#endif
}
#endif

int main()
{
	zest::MessageQueue<int, 2> queue;
	if (!queue.try_put(42)) {
		return 1;
	}
	const auto value = queue.try_get();
	if (!value || *value != 42 || !retained_value_works()) {
		return 2;
	}
	return 0;
}
