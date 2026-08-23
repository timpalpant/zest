/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * Zephyr-side smoke tests.
 *
 * Every component's public surface is instantiated so its header and its
 * translation unit both compile, and the parts that can run without hardware are
 * exercised as real assertions. The numeric layers are covered far more
 * thoroughly by tests/host, which needs no Zephyr build.
 */

#include <zephyr/ztest.h>

#include <zest/version.hpp>

/* Always available. */
#include <zest/control.hpp>
#include <zest/error.hpp>
#include <zest/filters.hpp>
#include <zest/function.hpp>
#include <zest/kernel.hpp>
#include <zest/retained_value.hpp>
#include <zest/retry.hpp>
#include <zest/ring_buffer.hpp>
#include <zest/timing.hpp>
#include <zest/transforms.hpp>
#include <zest/units.hpp>

#include <zest/battery.hpp>

#if defined(CONFIG_ZEST_ADC_CHANNEL)
#include <zest/adc_channel.hpp>
#include <zest/voltage_divider.hpp>
#endif
#if defined(CONFIG_ZEST_GPIO)
#include <zest/gpio.hpp>
#endif
#if defined(CONFIG_ZEST_BUTTON)
#include <zest/button.hpp>
#endif
#if defined(CONFIG_ZEST_LED_PATTERN)
#include <zest/led_pattern.hpp>
#endif
#if defined(CONFIG_ZEST_PWM_OUTPUT)
#include <zest/pwm.hpp>
#endif
#if defined(CONFIG_ZEST_SENSOR)
#include <zest/sensor.hpp>
#endif
#if defined(CONFIG_ZEST_I2C)
#include <zest/i2c.hpp>
#endif
#if defined(CONFIG_ZEST_SPI)
#include <zest/spi.hpp>
#endif
#if defined(CONFIG_ZEST_UART)
#include <zest/uart.hpp>
#endif
#if defined(CONFIG_ZEST_SETTINGS)
#include <zest/settings.hpp>
#endif
#if defined(CONFIG_ZEST_PROVISIONING_MANAGER)
#include <zest/provisioning_manager.hpp>
#endif
#if defined(CONFIG_ZEST_CERTIFICATE_STORE)
#include <zest/certificate_store.hpp>
#endif
#if defined(CONFIG_ZEST_NETWORK)
#include <zest/network.hpp>
#include <zest/poller.hpp>
#endif
#if defined(CONFIG_ZEST_NETWORK_MONITOR)
#include <zest/network_monitor.hpp>
#endif
#if defined(CONFIG_ZEST_SNTP_CLIENT)
#include <zest/sntp_client.hpp>
#endif
#if defined(CONFIG_ZEST_MQTT_CLIENT)
#include <zest/mqtt_client.hpp>
#endif
#if defined(CONFIG_ZEST_HTTP_CLIENT)
#include <zest/http_client.hpp>
#endif
#if defined(CONFIG_ZEST_WIFI_MANAGER)
#include <zest/wifi_manager.hpp>
#endif
#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
#include <zest/bluetooth_manager.hpp>
#endif
#if defined(CONFIG_ZEST_SYSTEM)
#include <zest/system.hpp>
#endif

#include <array>
#include <chrono>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace std::chrono_literals;

ZTEST_SUITE(zest_smoke, nullptr, nullptr, nullptr, nullptr, nullptr);

/* ------------------------------------------------------------------ error --- */

ZTEST(zest_smoke, test_error_is_zero_cost)
{
	static_assert(sizeof(zest::Error) == sizeof(int));
	static_assert(!std::is_convertible_v<int, zest::Error>);
	zassert_equal(zest::Error{-EINVAL}.value(), -EINVAL);
	zassert_true(zest::check(0).has_value());
	zassert_false(zest::check(-EIO).has_value());
	zassert_equal(zest::check(-EIO).error(), zest::errors::io_error);
	zassert_false(zest::Error{-EINVAL}.message().empty());
}

ZTEST(zest_smoke, test_version_macros)
{
	zassert_true(ZEST_VERSION_AT_LEAST(0, 2, 0));
	zassert_false(ZEST_VERSION_AT_LEAST(99, 0, 0));
	zassert_true(std::string_view{ZEST_VERSION_STRING}.size() > 0U);
}

/* ----------------------------------------------------------------- kernel --- */

ZTEST(zest_smoke, test_timeout_preserves_sub_millisecond_waits)
{
	/* A sub-millisecond wait must stay a wait, not collapse to K_NO_WAIT. */
	const auto sub_ms = zest::detail::timeout(500us);
	zassert_true(sub_ms.ticks > 0, "500us must not become K_NO_WAIT");

	const auto forever = zest::detail::timeout(std::chrono::milliseconds::max());
	zassert_equal(forever.ticks, K_TICKS_FOREVER);

	const auto negative = zest::detail::timeout(-5ms);
	zassert_equal(negative.ticks, 0, "a negative wait must not block");

	const auto zero = zest::detail::timeout(0ms);
	zassert_equal(zero.ticks, 0);
}

ZTEST(zest_smoke, test_message_queue_round_trip)
{
	zest::MessageQueue<int, 2> queue;

	zassert_equal(queue.capacity(), 2U);
	zassert_equal(queue.size(), 0U);
	zassert_true(queue.try_put(42).has_value());
	zassert_true(queue.try_put(43).has_value());
	/* Full: a non-blocking put must fail rather than block. */
	zassert_false(queue.try_put(44).has_value());

	zassert_equal(queue.peek().value(), 42);
	zassert_equal(queue.try_get().value(), 42);
	zassert_equal(queue.try_get().value(), 43);
	zassert_false(queue.try_get().has_value());
}

ZTEST(zest_smoke, test_message_queue_blocking_put_times_out)
{
	zest::MessageQueue<int, 1> queue;
	zassert_true(queue.try_put(1).has_value());
	/* A real, short wait must elapse and report a timeout. */
	const auto result = queue.put(2, 5ms);
	zassert_false(result.has_value());
}

ZTEST(zest_smoke, test_mutex_and_scoped_lock)
{
	zest::Mutex mutex;
	{
		zest::ScopedLock lock{mutex};
		zassert_false(mutex.try_lock() == false, "recursive lock by owner is permitted");
		mutex.unlock();
	}
	/* The scope released it, so it can be taken again. */
	zassert_true(mutex.try_lock());
	mutex.unlock();
}

ZTEST(zest_smoke, test_semaphore)
{
	zest::Semaphore semaphore{0U, 2U};
	zassert_false(semaphore.try_take());
	semaphore.give();
	zassert_equal(semaphore.count(), 1U);
	zassert_true(semaphore.take(10ms).has_value());
	zassert_false(semaphore.take(1ms).has_value());
}

ZTEST(zest_smoke, test_work_item_runs_a_capturing_lambda)
{
	/* The point of InplaceFunction: no trampoline, no void* context. */
	static int observed = 0;
	observed = 0;
	int increment = 7;

	zest::WorkItem work{[increment]() noexcept { observed += increment; }};
	zassert_true(work.submit().has_value());

	for (int i = 0; i < 100 && observed == 0; ++i) {
		zest::sleep_for(1ms);
	}
	zassert_equal(observed, 7);
}

ZTEST(zest_smoke, test_delayable_work_item)
{
	static bool fired = false;
	fired = false;

	zest::DelayableWorkItem work{[]() noexcept { fired = true; }};
	zassert_true(work.schedule(5ms).has_value());
	zassert_false(fired, "must not run before its deadline");

	for (int i = 0; i < 200 && !fired; ++i) {
		zest::sleep_for(1ms);
	}
	zassert_true(fired);
}

ZTEST(zest_smoke, test_static_thread_named_start)
{
	static zest::StaticThread<2048> thread;
	static int ran = 0;
	ran = 0;

	zassert_true(thread.start([]() noexcept { ran = 1; }, 5, "zest_smoke").has_value());
	zassert_true(thread.started());
	/* Starting twice must be refused, not silently ignored. */
	zassert_equal(thread.start([]() noexcept {}, 5).error(), zest::errors::already);
	zassert_true(thread.join(1s).has_value());
	zassert_equal(ran, 1);
}

ZTEST(zest_smoke, test_periodic_timer)
{
	zest::PeriodicTimer timer;
	timer.start(5ms, 5ms);
	zassert_true(timer.wait() > 0U);
	timer.stop();
}

ZTEST(zest_smoke, test_sleep_for_sub_millisecond)
{
	const auto before = zest::uptime();
	zest::sleep_for(200us);
	/* Chiefly a check that it neither hangs nor rejects the request. */
	zassert_true(zest::uptime() >= before);
}

ZTEST(zest_smoke, test_uptime_clock_drives_the_timing_helpers)
{
	static_assert(zest::UptimeClock::is_steady);
	static_assert(std::is_same_v<zest::UptimeClock::duration, std::chrono::milliseconds>);

	const auto before = zest::UptimeClock::now();
	zest::sleep_for(5ms);
	zassert_true(zest::UptimeClock::now() > before);

	/*
	 * The point of the clock: the argument-less overloads work without
	 * linking libstdc++'s std::chrono, which needs a gettimeofday most
	 * Zephyr builds do not have.
	 */
	zest::RateLimiter<zest::UptimeClock> limiter{1h};
	zassert_true(limiter.allow());
	zassert_false(limiter.allow());
}

/* ------------------------------------------------------------ ring buffer --- */

ZTEST(zest_smoke, test_ring_buffer)
{
	zest::SpscRingBuffer<int, 3> buffer;
	zassert_true(buffer.empty());
	for (int i = 1; i <= 3; ++i) {
		zassert_true(buffer.try_push(i));
	}
	zassert_true(buffer.full());
	zassert_false(buffer.try_push(4));

	std::array<int, 3> drained{};
	zassert_equal(buffer.drain(drained), 3U);
	zassert_equal(drained[0], 1);
	zassert_equal(drained[2], 3);
	zassert_true(buffer.empty());
}

/* --------------------------------------------------------- retained value --- */

ZTEST(zest_smoke, test_retained_value)
{
	static zest::RetainedStorage<int> storage{};
	zest::RetainedValue<int> retained{storage};

	zassert_false(retained.valid());
	zassert_equal(retained.value_or(7), 7);
	retained.store(42);
	zassert_true(retained.valid());
	zassert_equal(retained.value_or(0), 42);

	/* A corrupted payload must fail its checksum. */
	storage.value = 43;
	zassert_false(retained.valid());

	retained.store(43);
	zassert_true(retained.valid());
	retained.clear();
	zassert_false(retained.valid());
}

/* ------------------------------------------------------------- behavior --- */

ZTEST(zest_smoke, test_filters_and_control_compile_and_run)
{
	zest::MovingAverage<int, 3> average;
	zassert_equal(average.update(3), 3);
	zassert_equal(average.update(6), 4);

	zest::MedianFilter<int, 3> median;
	(void)median.update(9);
	(void)median.update(1);
	zassert_equal(median.update(5), 5);

	zest::ShiftMovingAverage<std::int32_t, 2> integer_ema;
	zassert_equal(integer_ema.update(100), 100);
	zassert_equal(integer_ema.update(0), 75);

	zest::PidController<float> pid{{.proportional = 2.0F},
				       {.output_min = -10.0F, .output_max = 10.0F}};
	/* Exactly at the limit is not clamping, so ask for more than the limit. */
	zassert_within(pid.update(4.0F, 0.0F, 10ms), 8.0F, 0.01F);
	zassert_false(pid.saturated());
	zassert_within(pid.update(50.0F, 0.0F, 10ms), 10.0F, 0.01F);
	zassert_true(pid.saturated());

	zest::StateMachine machine{0, std::array{zest::Transition<int, int>{0, 1, 2}}};
	zassert_equal(machine.dispatch(1).value(), 2);
	zassert_false(machine.dispatch(1).has_value());
}

ZTEST(zest_smoke, test_units_prevent_mixups)
{
	using namespace zest::literals;
	static_assert(!std::is_convertible_v<zest::Millivolts, zest::Ohms>);
	zassert_equal(zest::quantity_cast<zest::Volts>(3742_mV).count(), 3);
	zassert_equal(zest::divider_input(1650_mV, zest::Ohms{100}, zest::Ohms{200}).count(), 3300);
}

ZTEST(zest_smoke, test_retry_policy)
{
	zest::RetryPolicy retry{{
		.maximum_attempts = 3,
		.initial_delay = 10ms,
		.maximum_delay = 20ms,
		.multiplier_percent = 200,
	}};
	zassert_equal(retry.failure().value().count(), 10);
	zassert_equal(retry.failure().value().count(), 20);
	zassert_false(retry.failure().has_value());
	zassert_true(retry.exhausted());
}

ZTEST(zest_smoke, test_function_ref_accepts_captures)
{
	int offset = 5;
	auto add = [&](int value) noexcept { return value + offset; };
	zest::FunctionRef<int(int) noexcept> reference{add};
	zassert_equal(reference(1), 6);

	zest::InplaceFunction<int(int) noexcept> stored = [offset](int value) noexcept {
		return value * offset;
	};
	zassert_equal(stored(3), 15);
}

/* ------------------------------------------------------ hardware surfaces --- */

#if defined(CONFIG_ZEST_ADC_CHANNEL)
ZTEST(zest_smoke, test_adc_surface_compiles)
{
	static_assert(std::is_nothrow_constructible_v<zest::AdcChannel, adc_dt_spec>);
	static_assert(noexcept(std::declval<const zest::AdcChannel &>().read_microvolts()));
	static_assert(
		noexcept(std::declval<const zest::AdcChannel &>().read_average_microvolts(16)));

	/*
	 * A channel stays a trivially copyable value even though it caches what its
	 * driver can do: holders like VoltageDivider keep one by value, and the
	 * cache is a plain member rather than an Atomic precisely so that the
	 * implicit copy and move operations survive.
	 */
	static_assert(std::is_trivially_copyable_v<zest::AdcChannel>);
	static_assert(std::is_nothrow_copy_constructible_v<zest::AdcChannel>);
	static_assert(std::is_nothrow_copy_assignable_v<zest::AdcChannel>);

	/*
	 * The scale is carried in the type, so a microvolt reading cannot silently
	 * be used where a millivolt one was meant.
	 */
	static_assert(
		std::is_same_v<decltype(std::declval<const zest::AdcChannel &>().read_microvolts()),
			       zest::Result<zest::Microvolts>>);
	static_assert(std::is_same_v<decltype(std::declval<const zest::AdcChannel &>()
						      .read_average_microvolts<8>()),
				     zest::Result<zest::Microvolts>>);

	constexpr auto curve = zest::battery_curve(std::array{
		zest::CurvePoint{zest::Millivolts{4200}, 100},
		zest::CurvePoint{zest::Millivolts{3700}, 10},
		zest::CurvePoint{zest::Millivolts{3300}, 0},
	});
	zassert_equal(curve.percent_at(zest::Millivolts{3950}), 55);
	zassert_equal(curve.percent_at(zest::Millivolts{5000}), 100);
}
#endif

#if defined(CONFIG_ZEST_GPIO)
ZTEST(zest_smoke, test_gpio_output_tracks_state_without_reading_the_pin)
{
	static_assert(std::is_nothrow_constructible_v<zest::GpioOutput, gpio_dt_spec>);

	/*
	 * state() is exact and infallible because the object remembers what it
	 * drove. gpio_pin_get_dt() is documented for input pins, and an output-only
	 * pin has no input buffer on most SoCs, so reading it back returned a
	 * driver constant dressed up as a GpioState.
	 */
	zest::GpioOutput output{gpio_dt_spec{}};
	zassert_equal(output.state(), zest::GpioState::inactive);
	zassert_false(output.readback_enabled());
	/* Refuses rather than lying when readback was never configured. */
	zassert_equal(output.read_pin().error(), zest::errors::not_supported);

	zassert_equal(zest::invert(zest::GpioState::active), zest::GpioState::inactive);
}
#endif

#if defined(CONFIG_ZEST_PWM_OUTPUT)
ZTEST(zest_smoke, test_pwm_duty_is_integer)
{
	static_assert(std::is_nothrow_constructible_v<zest::PwmOutput, pwm_dt_spec>);
	zassert_equal(zest::per_mille_from(0.5F).count(), 500);
	zassert_equal(zest::per_mille_from(0.0F).count(), 0);
	zassert_equal(zest::per_mille_from(1.0F).count(), 1000);
	zassert_equal(zest::per_mille_from(2.0F).count(), 1000);
	zassert_equal(zest::per_mille_from(-1.0F).count(), 0);
}
#endif

#if defined(CONFIG_ZEST_BUTTON)
ZTEST(zest_smoke, test_button_surface_compiles)
{
	static_assert(
		std::is_nothrow_constructible_v<zest::Button<>, gpio_dt_spec, zest::ButtonConfig>);
	zassert_true(zest::has_event(zest::ButtonEvent::pressed | zest::ButtonEvent::clicked,
				     zest::ButtonEvent::clicked));
	zassert_false(zest::has_event(zest::ButtonEvent::pressed, zest::ButtonEvent::clicked));
}
#endif

#if defined(CONFIG_ZEST_LED_PATTERN)
ZTEST(zest_smoke, test_led_pattern_surface_compiles)
{
	static_assert(std::is_nothrow_constructible_v<zest::LedPatternPlayer<>, gpio_dt_spec>);
	zassert_equal(zest::patterns::connecting.size(), 2U);
	zassert_equal(zest::patterns::failure.size(), 4U);
}
#endif

#if defined(CONFIG_ZEST_SENSOR)
ZTEST(zest_smoke, test_sensor_surface_compiles)
{
	static_assert(std::is_nothrow_constructible_v<zest::SensorReader, const rtio_iodev &,
						      rtio &, const sensor_decoder_api &>);
	static_assert(std::is_nothrow_move_constructible_v<zest::AsyncSensorFrame>);
	static_assert(std::is_same_v<
		      decltype(std::declval<const zest::AsyncSensorReader &>().try_receive()),
		      zest::Result<zest::AsyncSensorFrame>>);
}
#endif

#if defined(CONFIG_ZEST_SETTINGS)
ZTEST(zest_smoke, test_settings_rejects_persisting_a_view)
{
	/*
	 * View types are trivially copyable, so an unconstrained overload would
	 * write a pointer and a length to flash. TriviallySerializable rejects
	 * them; the string_view overload persists the characters instead.
	 */
	static_assert(!zest::TriviallySerializable<std::string_view>);
	static_assert(!zest::TriviallySerializable<const char *>);
	static_assert(!zest::TriviallySerializable<std::span<const std::byte>>);
	static_assert(zest::TriviallySerializable<int>);
	static_assert(zest::TriviallySerializable<std::chrono::seconds>);
	static_assert(std::is_nothrow_constructible_v<zest::Settings<>, std::string_view>);

	zest::Settings settings{"zest_smoke"};
	zassert_equal(settings.root(), std::string_view{"zest_smoke"});
	/* A key that overflows the name budget is reported, not truncated. */
	std::array<char, 200> long_key{};
	long_key.fill('k');
	zassert_equal(settings.set(std::string_view{long_key.data(), long_key.size()},
				   std::string_view{"x"})
			      .error(),
		      zest::errors::name_too_long);
}
#endif

#if defined(CONFIG_ZEST_CERTIFICATE_STORE)
ZTEST(zest_smoke, test_credential_surfaces_compile)
{
	static_assert(std::is_nothrow_default_constructible_v<zest::StaticCredential>);
	static_assert(std::is_nothrow_default_constructible_v<zest::OwnedCredential<2048>>);
	static_assert(zest::OwnedCredential<2048>::capacity() == 2048U);

	zest::StaticCredential credential;
	zassert_false(credential.registered());
	/* An empty credential is rejected before it reaches the registry. */
	zassert_equal(credential.add(1, TLS_CREDENTIAL_CA_CERTIFICATE, {}).error(),
		      zest::errors::invalid_argument);
}
#endif

#if defined(CONFIG_ZEST_PROVISIONING_MANAGER)
ZTEST(zest_smoke, test_provisioning_validates_input)
{
	static_assert(
		std::is_nothrow_constructible_v<zest::ProvisioningManager<>, std::string_view>);
	zest::ProvisioningManager manager{"zest_smoke"};

	std::array<char, 40> too_long{};
	too_long.fill('s');
	zassert_equal(
		manager.provision(std::string_view{too_long.data(), too_long.size()}, "pw").error(),
		zest::errors::invalid_argument);
	zassert_equal(manager.provision("", "pw").error(), zest::errors::invalid_argument);
}
#endif

#if defined(CONFIG_ZEST_NETWORK)
ZTEST(zest_smoke, test_network_surfaces_compile)
{
	static_assert(std::is_nothrow_default_constructible_v<zest::DnsResolver>);
	static_assert(std::is_nothrow_default_constructible_v<zest::UdpSocket>);
	static_assert(std::is_nothrow_move_constructible_v<zest::TcpSocket>);
	static_assert(noexcept(std::declval<zest::TcpSocket &>().close()));

	zest::UdpSocket socket;
	zassert_false(socket.is_open());
	/* Operating on a closed socket reports a bad descriptor, not a crash. */
	zassert_equal(socket.send({}).error(), zest::errors::bad_descriptor);

	/* DNS keeps its own error type so its codes cannot alias errno. */
	zest::DnsResolver resolver;
	const auto resolved = resolver.resolve("", 80);
	zassert_false(resolved.has_value());
	zassert_equal(resolved.error(), zest::DnsError::no_name);
	zassert_true(std::string_view{zest::to_string(zest::DnsError::no_name)}.size() > 0U);
}

ZTEST(zest_smoke, test_poller_surface)
{
	zest::Poller<4> poller;
	zassert_equal(poller.size(), 0U);
	zassert_equal(poller.capacity(), 4U);
	zassert_true(poller.add(1, zest::PollEvent::readable).has_value());
	zassert_equal(poller.size(), 1U);
	poller.clear();
	zassert_equal(poller.size(), 0U);
}
#endif

#if defined(CONFIG_ZEST_NETWORK_MONITOR)
ZTEST(zest_smoke, test_network_monitor_surface)
{
	static_assert(std::is_nothrow_constructible_v<zest::NetworkMonitor, net_if *>);
	zest::NetworkMonitor monitor;
	/* wait_for_ipv4 before start() must report rather than block. */
	zassert_equal(monitor.wait_for_ipv4(1ms).error(), zest::errors::permission_denied);
}
#endif

#if defined(CONFIG_ZEST_SNTP_CLIENT)
ZTEST(zest_smoke, test_sntp_surface)
{
	static_assert(noexcept(std::declval<const zest::SntpClient &>().query(
		std::declval<std::string_view>(), std::declval<std::chrono::milliseconds>())));
	zest::SntpClient client;
	zassert_equal(client.query("", 1s).error(), zest::errors::invalid_argument);
}
#endif

#if defined(CONFIG_ZEST_MQTT_CLIENT)
ZTEST(zest_smoke, test_mqtt_surface)
{
	static_assert(std::is_nothrow_default_constructible_v<zest::MqttClient<>>);
	zest::MqttClient<> client;

	/* Publishing before configure() is refused. */
	zassert_false(client.publish("topic", std::span<const std::byte>{}).has_value());
	/* An empty client id is invalid. */
	zest::ResolvedAddress broker{};
	zassert_equal(client.configure(broker, {.client_id = ""}).error(),
		      zest::errors::invalid_argument);
	/* Message ids must advance, so QoS 1 and 2 are not all id 1. */
	zassert_not_equal(client.next_message_id(), client.next_message_id());
}
#endif

#if defined(CONFIG_ZEST_HTTP_CLIENT)
ZTEST(zest_smoke, test_http_url_parsing_and_errors)
{
	static_assert(std::is_nothrow_default_constructible_v<zest::HttpClient>);
	static_assert(std::is_nothrow_constructible_v<zest::HttpClient, zest::HttpClient::Options>);
	zest::HttpClient client;
	std::array<std::byte, 64> body{};

	/* A scheme-less URL is rejected at the parse stage. */
	const auto bad_scheme = client.get("example.com/", body);
	zassert_false(bad_scheme.has_value());
	zassert_equal(bad_scheme.error().stage, zest::HttpErrorStage::invalid_url);

	/* Credentials in a URL are refused rather than leaked into logs. */
	const auto credentials = client.get("http://user:pw@example.com/", body);
	zassert_false(credentials.has_value());
	zassert_equal(credentials.error().stage, zest::HttpErrorStage::invalid_url);

	zassert_true(std::string_view{zest::to_string(zest::HttpErrorStage::dns)}.size() > 0U);
}
#endif

#if defined(CONFIG_ZEST_WIFI_MANAGER)
ZTEST(zest_smoke, test_wifi_surface)
{
	static_assert(std::is_nothrow_default_constructible_v<zest::WifiManager>);
	static_assert(noexcept(std::declval<zest::WifiManager &>().status()));

	zest::WifiManager wifi;
	zassert_equal(wifi.state(), zest::WifiManager::State::disconnected);
	zassert_false(wifi.connected());
	/* Credential validation happens before any driver call. */
	zassert_equal(wifi.connect({.ssid = ""}).error(), zest::errors::invalid_argument);
	zassert_true(std::string_view{zest::to_string(wifi.state())}.size() > 0U);
	zassert_true(std::string_view{zest::to_string(zest::WifiSecurity::wpa3_sae)}.size() > 0U);
}
#endif

#if defined(CONFIG_ZEST_BLUETOOTH_MANAGER)
ZTEST(zest_smoke, test_bluetooth_surface)
{
	static_assert(std::is_nothrow_default_constructible_v<zest::BluetoothManager>);
	zest::BluetoothManager bluetooth;
	zassert_equal(bluetooth.state(), zest::BluetoothManager::State::disabled);
	/* A malformed address is rejected before the stack is touched. */
	zassert_equal(bluetooth.connect({.address = "nope"}).error(),
		      zest::errors::invalid_argument);
	zassert_true(std::string_view{zest::to_string(bluetooth.state())}.size() > 0U);
}
#endif

#if defined(CONFIG_ZEST_WATCHDOG)
ZTEST(zest_smoke, test_watchdog_separates_device_from_channel)
{
	/*
	 * wdt_setup() is per-device and is rejected once running, so a
	 * channel-shaped wrapper made the second install fail as if the driver were
	 * broken. Channels now come from the device and retain the native Zephyr
	 * device pointer rather than borrowing the WatchdogDevice wrapper.
	 */
	zest::WatchdogDevice watchdog{nullptr};
	zassert_false(watchdog.running());
	zassert_equal(watchdog.install(1s).error(), zest::errors::no_device);
	zassert_equal(watchdog.start().error(), zest::errors::no_device);
}
#endif

#if defined(CONFIG_ZEST_DEVICE_IDENTITY)
ZTEST(zest_smoke, test_device_identity_reports_real_length)
{
	std::array<std::byte, 16> id{};
	const auto view = zest::DeviceIdentity::read(id);
	if (view.has_value()) {
		/* The length is the SoC's, not the buffer's: no silent zero padding. */
		zassert_true(view->size() > 0U);
		zassert_true(view->size() <= id.size());
	}
	/* An empty destination is rejected. */
	zassert_equal(zest::DeviceIdentity::read({}).error(), zest::errors::invalid_argument);
}
#endif

#if defined(CONFIG_ZEST_I2C)
ZTEST(zest_smoke, test_i2c_surface)
{
	static_assert(std::is_nothrow_constructible_v<zest::I2cDevice, i2c_dt_spec>);

	/* An unbound spec must report no device rather than dereferencing null. */
	constexpr zest::I2cDevice sensor{i2c_dt_spec{}};
	zassert_equal(sensor.init().error(), zest::errors::no_device);
	/* Empty transfers are rejected before reaching the driver. */
	zassert_equal(sensor.write({}).error(), zest::errors::invalid_argument);
	zassert_equal(sensor.read({}).error(), zest::errors::invalid_argument);
	zassert_equal(sensor.read_registers(0x00, {}).error(), zest::errors::invalid_argument);
}
#endif

#if defined(CONFIG_ZEST_SPI)
ZTEST(zest_smoke, test_spi_surface)
{
	static_assert(std::is_nothrow_constructible_v<zest::SpiDevice, spi_dt_spec>);

	constexpr zest::SpiDevice imu{spi_dt_spec{}};
	zassert_equal(imu.init().error(), zest::errors::no_device);
	zassert_equal(imu.write({}).error(), zest::errors::invalid_argument);
	zassert_equal(imu.transceive({}, {}).error(), zest::errors::invalid_argument);
	zassert_equal(imu.write_then_read({}, {}).error(), zest::errors::invalid_argument);
}
#endif

#if defined(CONFIG_ZEST_UART)
ZTEST(zest_smoke, test_uart_surface)
{
	static_assert(std::is_nothrow_constructible_v<zest::Uart, const struct device *>);

	constexpr zest::Uart port{nullptr};
	zassert_equal(port.init().error(), zest::errors::no_device);
	zassert_equal(port.write(std::string_view{"hi"}).error(), zest::errors::no_device);
	/* A bounded read must report rather than spin forever. */
	std::array<std::byte, 4> buffer{};
	zassert_equal(port.read(buffer, 1ms).error(), zest::errors::no_device);
	port.flush_input();
}
#endif
