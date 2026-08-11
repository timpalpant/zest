<!--
Copyright (c) 2026 Timothy Palpant

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Zest

Zest is an allocation-conscious C++23 service library for
[Zephyr RTOS](https://zephyrproject.org/). It wraps common embedded operations in
small, synchronous APIs built around `std::expected`, `std::span`, and
`std::chrono`.

Every fallible operation returns `zest::Result<T>`, every failure has a
`message()`, and nothing on a per-sample path uses `double` — no common Cortex-M
part has a double-precision FPU.

[API reference](https://timpalpant.github.io/zest/api/) ·
[project website](https://timpalpant.github.io/zest/) ·
[design notes](DESIGN.md) ·
[source](https://github.com/timpalpant/zest)

## What is in it

| Area | Facilities |
| --- | --- |
| Errors | `Error`, `Result<T>`, `check()`, `ZEST_TRY`, `errors::*` |
| Units | `Quantity`, `Millivolts`, `Milliamps`, `Ohms`, `MilliCelsius`, `Hertz`, literals |
| Analog and digital I/O | `AdcChannel`, `VoltageDivider`, `GpioInput`, `GpioOutput`, `Button` |
| Signal processing | `MovingAverage`, `MedianFilter`, `ExponentialMovingAverage`, `ShiftMovingAverage`, `Hysteresis`, `ThresholdDetector` |
| Transforms | `Calibration`, `IntegerCalibration`, `LinearMap`, `integer_map` |
| Control | `PidController`, `SlewRateLimiter`, `slew_toward`, `StateMachine` |
| Timing and policy | `RateLimiter`, `Debouncer`, `RetryPolicy`, `ExponentialBackoff` |
| Battery | `BatteryMonitor`, `BatteryCurve`, `battery_curve()` (all in `zest/battery.hpp`) |
| Buses | `I2cDevice`, `SpiDevice`, `Uart` |
| Sensors | `SensorReader`, `SensorBatch`, `AsyncSensorReader`, `SensorChannel`, `PeriodicSampler` |
| Actuators | `PwmOutput`, `DimmableLed`, `RgbLed`, `Servo`, `Buzzer`, `LedPatternPlayer` |
| Buffers | `SpscRingBuffer`, `MessageQueue` |
| Serialization | `Schema`, `Format`, `serialize`, `deserialize`, `json::`, `cbor::` |
| Callables | `FunctionRef`, `InplaceFunction` |
| Kernel | `Mutex`, `ScopedLock`, `Semaphore`, `WorkItem`, `DelayableWorkItem`, `WorkQueue`, `PeriodicTimer`, `StaticThread`, `uptime()`, `sleep_for()`, `UptimeClock` |
| Persistence | `Settings`, `ProvisioningManager`, `RetainedValue` |
| Networking | `DnsResolver`, `UdpSocket`, `TcpSocket`, `Poller`, `SntpClient`, `TimeSynchronizer`, `MqttClient`, `NetworkMonitor`, `WifiManager`, `HttpClient` |
| Security | `StaticCredential`, `OwnedCredential` |
| System | `WatchdogDevice`, `WatchdogChannel`, `Rtc`, `RebootReason`, `DeviceIdentity`, `PowerManager` |

## Requirements

Zest targets Zephyr 4.4 or newer and requires the full C++ standard library:

```conf
CONFIG_CPP=y
CONFIG_STD_CPP23=y
CONFIG_REQUIRES_FULL_LIBCPP=y
CONFIG_GLIBCXX_LIBCPP=y
```

Exceptions and RTTI are not required. Each component is independently selected
and only builds when its Zephyr prerequisites are present.

## Add it to a west workspace

Add Zest to the workspace manifest:

```yaml
manifest:
  projects:
    - name: zest
      url: https://github.com/timpalpant/zest.git
      revision: master
      path: modules/lib/zest
```

After `west update`, Zephyr discovers `zephyr/module.yml` automatically. For a
standalone checkout, point an application at it before `find_package(Zephyr)`:

```cmake
list(APPEND EXTRA_ZEPHYR_MODULES /absolute/path/to/zest)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

Enable the library and only the services the image uses:

```conf
CONFIG_ZEST=y
CONFIG_ZEST_ADC_CHANNEL=y
CONFIG_ZEST_GPIO=y
CONFIG_ZEST_BATTERY_MONITOR=y
CONFIG_ZEST_WIFI_MANAGER=y
CONFIG_ZEST_HTTP_CLIENT=y
```

Header-only facilities — errors, units, filters, transforms, control, timing,
retry, ring buffer, callables and the kernel wrappers — need only
`CONFIG_ZEST=y` plus whatever Zephyr facility they touch.

## Errors

Every fallible operation returns `zest::Result<T>`, which is
`std::expected<T, zest::Error>`. `Error` wraps a negative errno, costs exactly
what an `int` costs, and always has a description:

```cpp
if (auto millivolts = battery.read_millivolts()) {
    LOG_INF("battery %d mV", millivolts->count());
} else {
    LOG_ERR("battery read failed: %.*s",
            static_cast<int>(millivolts.error().message().size()),
            millivolts.error().message().data());
}
```

Construction is explicit, so an `int` never silently becomes a failure, and
comparisons read plainly:

```cpp
if (result.error() == zest::errors::timed_out) { /* ... */ }
```

`ZEST_TRY` and `ZEST_TRY_ASSIGN` propagate failures without nesting:

```cpp
zest::Result<> start() noexcept
{
    ZEST_TRY(channel.init());
    ZEST_TRY(pump.init());
    ZEST_TRY_ASSIGN(millivolts, channel.read_millivolts());
    return controller.begin(millivolts);
}
```

Failures that are not errno values keep their own types, so they cannot be
confused with one: `DnsError`, `CurveError`, `TransformError`, and `HttpError`,
which carries both a stage and an `Error` cause. This is not cosmetic — Zephyr's
`DNS_EAI_NONAME` is `-2`, which as an errno would read as `-ENOENT`.

`CONFIG_ZEST_ERROR_STRINGS=n` drops the description table (about 700 bytes of
rodata) on a flash-tight image; the numeric code stays available via `value()`.

## Units

Millivolt-versus-volt mixups are the classic sensing bug, so quantities carry
their unit and scale in the type. Exact conversions are implicit; lossy ones need
`quantity_cast`, exactly as with `std::chrono`:

```cpp
using namespace zest::literals;

zest::Millivolts reading = 3742_mV;
zest::Microvolts fine = reading;                          // implicit, exact
auto coarse = zest::quantity_cast<zest::Volts>(reading);  // explicit, truncates

// Passing volts where ohms are wanted does not compile.
auto input = zest::divider_input(reading, 100_kohm, 200_kohm);
```

## Examples

### ADC and GPIO

```cpp
#include <zest/adc_channel.hpp>
#include <zest/gpio.hpp>

zest::AdcChannel analog{ADC_DT_SPEC_GET(DT_ALIAS(sensor))};
zest::GpioOutput led{GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios)};

zest::Result<> sample() noexcept
{
    ZEST_TRY(analog.init());
    ZEST_TRY(led.init());
    ZEST_TRY_ASSIGN(millivolts, analog.read_average_millivolts(16));
    return led.set(millivolts > 1000_mV ? zest::GpioState::active
                                        : zest::GpioState::inactive);
}
```

`read_average_millivolts(n)` takes the burst in one hardware sequence rather than
entering the driver `n` times. On a bus-attached converter that is the difference
between one transaction and `n` round trips.

There is a microvolt read for every millivolt one --- `read_microvolts()` and
`read_average_microvolts(n)`. Millivolts are the wrong unit for a
high-resolution part: a 16-bit converter across a 2 V span resolves about 31 uV,
so rounding to millivolts discards five bits of exactly the resolution such a
part was chosen for. Reach for microvolts whenever the signal is smaller than a
few hundred millivolts --- a bridge, a thermocouple, or an instrumentation
amplifier's output.

Averaging happens in the raw domain, so a burst costs one raw-to-voltage
conversion rather than `n`, and nothing is rounded to the output unit before the
samples are summed. Differential channels burst too: each sample is sign-extended
individually, because summing a differential burst as unsigned would turn every
negative reading into a large positive one.

`GpioOutput::state()` reports the last state driven and cannot fail. Reading the
pin back needs `init(state, /* enable_readback = */ true)`, because Zephyr's
`gpio_pin_get_dt()` is for input pins and an output-only pin has no input buffer
on most SoCs.

### Battery

Measurement and charge estimation live in one header but stay separate types,
because they fail differently: a curve is a constant of the design, checked once,
while reading the cell is I/O that fails per call.

```cpp
#include <zest/battery.hpp>

// Validated at compile time: a malformed curve fails the build.
constexpr auto discharge = zest::battery_curve(std::array{
    zest::CurvePoint{4200, 100},
    zest::CurvePoint{3700, 10},
    zest::CurvePoint{3300, 0},
});

constexpr zest::BatteryMonitor battery{
    ADC_DT_SPEC_GET(DT_NODELABEL(vbatt)),
    zest::Ohms{DT_PROP(DT_NODELABEL(vbatt), output_ohms)},
    zest::Ohms{DT_PROP(DT_NODELABEL(vbatt), full_ohms)},
};

zest::Result<std::uint8_t> charge() noexcept
{
    ZEST_TRY(battery.init());
    ZEST_TRY_ASSIGN(millivolts, battery.read_millivolts());
    return discharge.percent_at(millivolts.count());  // cannot fail
}
```

`BatteryMonitor` needs `CONFIG_ZEST_BATTERY_MONITOR=y`; the curve half is plain
arithmetic on no dependency, so it is always available and is host-tested.

### Control

```cpp
#include <zest/control.hpp>

zest::PidController<float> heater{
    {.proportional = 2.0F, .integral = 0.5F, .derivative = 0.1F},
    {.output_min = 0.0F, .output_max = 1000.0F, .integral_limit = 500.0F},
};

const float command = heater.update(setpoint, measurement, 100ms);
```

Anti-windup and derivative-on-measurement are built in, so a saturated actuator
does not build an integral it cannot express and a setpoint step does not produce
a derivative spike.

### Wi-Fi

```cpp
#include <zest/wifi_manager.hpp>

zest::WifiManager wifi;
wifi.on_state_change([](zest::WifiManager::State state) noexcept {
    LOG_INF("link %s", zest::to_string(state));
});

// List networks for a provisioning UI.
std::array<zest::WifiScanResult, 16> found{};
if (auto networks = wifi.scan(found)) {
    for (const auto &network : *networks) {
        LOG_INF("%.*s ch%u %ddBm %s",
                static_cast<int>(network.ssid_view().size()), network.ssid_view().data(),
                network.channel, network.rssi, zest::to_string(network.security));
    }
}

if (auto connection = wifi.connect({
        .ssid = "network",
        .password = "password",
        .security = zest::WifiSecurity::wpa3_sae,
    })) {
    LOG_INF("IPv4 %.*s",
            static_cast<int>(connection->address_view().size()),
            connection->address_view().data());
}
```

`connect()` waits for a usable DHCP address, retrying transient association
failures with jittered exponential backoff. It serializes the object for the
whole call; use `request_disconnect()` to interrupt without blocking.

### MQTT event loop

`MqttClient` gives the loop everything it needs — the pollable descriptor and the
time until the next keepalive — instead of leaving them to be dug out of the
transport union:

```cpp
#include <zest/mqtt_client.hpp>
#include <zest/poller.hpp>

zest::MqttClient<> client;
ZEST_TRY(client.configure(broker, {.client_id = "sensor-1"},
                          [](const mqtt_evt &event) noexcept { handle(event); }));
ZEST_TRY(client.connect());

zest::Poller<1> poller;
ZEST_TRY(poller.add(client.poll_fd(), zest::PollEvent::readable));

for (;;) {
    ZEST_TRY_ASSIGN(ready, poller.wait(client.keepalive_time_left()));
    if (ready > 0 && zest::has_event(poller.events(0), zest::PollEvent::readable)) {
        ZEST_TRY(client.input());
    }
    ZEST_TRY(client.keep_alive());
}
```

`publish()` draws its message id from a monotonic counter, so QoS 1 and 2
acknowledgements can be correlated.

### HTTP

```cpp
#include <zest/http_client.hpp>

using namespace std::chrono_literals;

std::array<std::byte, 2048> body{};
constexpr sec_tag_t ca_tags[] = {42};

zest::HttpClient client{zest::HttpClient::Options{
    .timeout = 10s,
    .user_agent = "my-device/1.0",
    .keep_alive = true,            // reuse the TLS session across posts
    .truncation_is_error = true,   // a body that did not fit is a failure
    .peer_verification = zest::HttpClient::PeerVerification::required,
    .security_tags = ca_tags,
}};

if (auto response = client.get("https://example.com/", body)) {
    LOG_INF("%u %.*s", response->status_code,
            static_cast<int>(response->text().size()), response->text().data());
}
```

`request()` needs roughly `CONFIG_ZEST_HTTP_RECV_BUF_SIZE` plus
`CONFIG_ZEST_HTTP_MAX_URL_LEN` bytes of the calling thread's stack — about 1.6 KB
at the defaults, so a 2048-byte thread will overflow. Lower the Kconfig values or
size the thread accordingly.

`StaticCredential` registers a CA that already lives in `.rodata` without copying
it into RAM; `OwnedCredential<N>` copies, for a credential that arrives at run
time. Certificate validation also needs a valid system clock, which
`TimeSynchronizer` can set.

### Serialization

A schema names fields once; the wire representation of each is deduced from its
C++ type, and the *format* is a separate choice. The same schema drives JSON and
CBOR:

```cpp
#include <zest/serde.hpp>

struct Reading {
    std::int32_t millivolts;
    std::int32_t centi_celsius;
    bool charging;
    char label[16];          // copied, not borrowed
};

ZEST_SCHEMA(Reading,
            ZEST_FIELD(Reading, millivolts, "mv"),
            ZEST_FIELD(Reading, centi_celsius, "cc"),
            ZEST_MEMBER(Reading, charging),   // wire name defaults to the member
            ZEST_MEMBER(Reading, label));
```

Pick a format at the call site, or pin one for the build:

```cpp
constexpr auto kFormat = zest::Format::cbor;

std::array<std::byte, 128> buffer{};
ZEST_TRY_ASSIGN(body, zest::serialize<kFormat>(reading, buffer));
ZEST_TRY(client.publish("sensor/1", body));

ZEST_TRY_ASSIGN(parsed, zest::deserialize<kFormat, Reading>(payload));
if (parsed.has("cc")) {                 // absent and zero are different things
    use(parsed->centi_celsius);
}
```

Both codecs delegate to the library Zephyr already ships — Zephyr's JSON library
and zcbor — so no serialization logic is duplicated. Nested objects and arrays
compose from their members' own schemas, and an incoming key with no matching
field is skipped by both, so a sender adding fields will not break a receiver.

A field left out of the schema is simply not part of the mapping: never written,
never populated. That is a compile-time choice covering all values — neither
format supports per-value omission the way Go's `omitempty` does.

**The two formats are not equivalent**, and where they differ CBOR is better
behaved:

| | JSON | CBOR |
| --- | --- | --- |
| Payload size | roughly 2× | baseline |
| Decoding modifies the input buffer | yes, writes NULs | no |
| `char *` members decode | yes, borrowing the buffer | no — use `char[N]` |
| Arrays of objects | yes | no, rejected at compile time |
| Unknown incoming keys | skipped | skipped |

Prefer CBOR where both ends are yours; JSON is for interoperating with something
that expects it. `zest::content_type(format)` gives the matching HTTP media type.

Requires `CONFIG_ZEST_JSON=y` with `CONFIG_JSON_LIBRARY=y`, and/or
`CONFIG_ZEST_CBOR=y` with `CONFIG_ZCBOR=y`. `float` and `double` members need
`CONFIG_JSON_LIBRARY_FP_SUPPORT=y` for the JSON codec.

### Callbacks

Work items, threads and clients take any callable, including a lambda that
captures, with nothing allocated:

```cpp
#include <zest/kernel.hpp>

zest::WorkItem drain{[this]() noexcept { flush_samples(); }};
ZEST_TRY(drain.submit());

zest::DelayableWorkItem retry{[this]() noexcept { reconnect(); }};
ZEST_TRY(retry.schedule(5s));

static zest::WorkQueue<2048> slow;
ZEST_TRY(slow.start(/* priority = */ 5, "telemetry"));
```

### Sampling into a buffer

`SpscRingBuffer` is lock-free for one producer and one consumer, so a sampling
callback can push and a worker can drain without a kernel object between them:

```cpp
#include <zest/ring_buffer.hpp>

zest::SpscRingBuffer<Reading, 64> samples;

void on_sample(Reading reading) noexcept  // ISR or timer context
{
    (void)samples.push_overwrite(reading);  // keep the newest
}

void publish() noexcept                    // worker context
{
    std::array<Reading, 32> batch{};
    const std::size_t count = samples.drain(batch);
    /* ... */
}
```

## Design constraints

The APIs avoid owning dynamic buffers: strings and byte storage are supplied by
the caller and must remain alive for the documented operation or client lifetime.
Managers serialize lifecycle operations and expose Zephyr failures as `Error`
through `Result<T>`.

Platform configuration remains outside the library. Network pools, TLS algorithms
and credentials, DHCP policy, Wi-Fi drivers, Bluetooth controllers, and
devicetree wiring belong to the application and board.

The helpers that take a `Clock` template parameter -- `RateLimiter`, `Debouncer`,
`Button`, `LedPatternPlayer`, `PeriodicSampler` -- all accept a caller-supplied
time point, and that overload is the one to prefer when the calling code already
knows what time it is. For the argument-less overloads, pass `zest::UptimeClock`
rather than taking the `std::chrono::steady_clock` default:

```cpp
zest::RateLimiter<zest::UptimeClock> warnings{5s};
```

libstdc++ keeps `steady_clock::now()` and `system_clock::now()` in one object
file, so calling either one links both, and `system_clock::now()` needs a
`gettimeofday` that most Zephyr configurations do not provide. The failure
arrives at link time, names a function nobody called, and points at an object
nobody asked for. `UptimeClock` reads `k_uptime_get()` and links everywhere. The
default is left alone so the headers stay host-testable.

See [DESIGN.md](DESIGN.md) for the architectural layers, the relationship with
zpp, and the thread-safety contracts.

## Tests

The Zephyr-independent layers build and run with a plain host compiler, in
seconds and without a west workspace:

```sh
cmake -S tests/host -B build/host && cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The Zephyr-side suite is a `ztest` application. Every `*.conf` in `tests/smoke`
is a separate configuration, and CI builds and runs all of them on
`native_sim`:

```sh
west build -b native_sim/native/64 tests/smoke -- \
  -DEXTRA_ZEPHYR_MODULES=/path/to/zest \
  -DEXTRA_CONF_FILE=tests/smoke/mqtt.conf
./build/zephyr/zephyr.exe
```

## Documentation

Install Doxygen and run:

```sh
mkdir -p build/docs
doxygen Doxyfile
```

Open `build/docs/html/index.html`. GitHub Pages publishes the landing page and
generated API reference automatically from `master`.

## License

LGPL-3.0-or-later. See [COPYING.LESSER](COPYING.LESSER) and
[COPYING](COPYING).

Linking a Zest-using firmware image against the unmodified library does not
require releasing the application's own source, provided the LGPL's relinking
terms are met. Modifications to Zest itself remain under the LGPL.
