# Zest design and component guide

Zest is an allocation-conscious C++23 library that complements Zephyr's C
APIs with typed hardware handles, reusable embedded behavior, and
application-level services. It favors explicit initialization, explicit
errors, caller-owned storage, static composition, and `noexcept` APIs.

## Relationship with zpp

[lowlander/zpp](https://github.com/lowlander/zpp) is a similar but distinct
project. It describes itself as an experimental C++20 wrapper around Zephyr's
C API and currently covers primarily kernel and standard-library-adjacent
facilities: threads, mutexes, condition variables, semaphores, timers, polling,
futexes, FIFOs, heaps, memory slabs, clocks, formatting, and result/error
types. It is header-only, declares its API unstable, and its latest commit was
made in January 2023 for the Zephyr 3.1 era.

Zest will **complement zpp without depending on it**:

- Zest owns typed peripheral wrappers, signal-processing helpers, and
  application services.
- Zest does not attempt to replace the standard library or zpp with a broad
  kernel-primitives layer. Its few kernel helpers are fixed-storage building
  blocks (`MessageQueue`, `WorkItem`, `PeriodicTimer`, and `StaticThread`) whose
  ownership or callback semantics are useful to the higher application layers.
- Zest uses the C++23 standard library directly where the configured Zephyr
  toolchain provides it, including `std::expected`, `std::span`, and
  `std::chrono`.
- When the standard library does not expose required Zephyr semantics, Zest
  uses the Zephyr API directly and keeps that choice local to the relevant
  abstraction.
- Public wrappers may expose native Zephyr handles when this materially
  improves interoperability, but zpp is not a build or API dependency.
- An optional adapter package could be considered later if active zpp users
  request it. It does not belong in the core library.

This boundary avoids coupling Zest to an inactive, unstable dependency while
leaving both libraries usable in the same west workspace.

## Architectural layers

Zest is organized conceptually into three layers:

| Layer | Examples | Purpose |
| --- | --- | --- |
| Hardware handles | `AdcChannel`, `GpioOutput`, `PwmOutput` | Thin, typed Zephyr wrappers |
| Reusable behavior | `MovingAverage`, `Debouncer`, `RetryPolicy` | Hardware-independent composition |
| Services | `BatteryMonitor`, `WifiManager`, `HttpClient` | Stateful application operations |

Zephyr already supplies hardware abstraction through `device`,
`adc_dt_spec`, `gpio_dt_spec`, and related devicetree-aware structures. Zest
does not add a conventional inheritance hierarchy such as
`Peripheral -> AnalogPeripheral -> AdcChannel`. Thin value-semantic wrappers
instead add safe initialization, typed results, consistent errors, and
composable behavior.

## Common API conventions

- Constructors capture configuration and do not perform fallible work.
- Fallible setup is performed by an explicit `init()` or `connect()` call.
- Expected operational failures use `std::expected`.
- Public operations that cannot throw are marked `noexcept`.
- Logical states such as GPIO active/inactive are preferred over raw
  electrical levels.
- Buffers are fixed-size or caller-owned unless dynamic allocation is an
  explicit feature of the abstraction.
- Hardware-independent algorithms are header-only and host-testable where
  practical.
- Runtime polymorphism and virtual base classes are avoided unless a concrete
  use case requires them.
- ISR-facing APIs do not invoke arbitrary owning callbacks or allocate memory.

## 1. ADC channels

`AdcChannel` removes Zephyr ADC sequence and conversion boilerplate:

```cpp
zest::AdcChannel channel{ADC_DT_SPEC_GET(DT_ALIAS(sensor))};

auto ready = channel.init();
auto raw = channel.read_raw();
auto millivolts = channel.read_millivolts();
auto average = channel.read_average_millivolts<16>();
```

Public interface:

```cpp
class AdcChannel {
public:
    constexpr explicit AdcChannel(adc_dt_spec spec) noexcept;

    [[nodiscard]] std::expected<void, int> init() const noexcept;
    [[nodiscard]] std::expected<std::int32_t, int> read_raw() const noexcept;
    [[nodiscard]] std::expected<std::int32_t, int>
    read_millivolts() const noexcept;

    template<std::size_t Samples>
    [[nodiscard]] std::expected<std::int32_t, int>
    read_average_millivolts() const noexcept;
};
```

`BatteryMonitor` composes an `AdcChannel` and adds only divider scaling:

```text
AdcChannel
    ↓
VoltageDivider / BatteryMonitor
    ↓
estimate_charge_percent()
```

`VoltageDivider` is the reusable adaptor between `AdcChannel` and
`BatteryMonitor`; it accepts the measured-output and full-divider resistance
and supports a compile-time sample count.

## 2. GPIO outputs

`GpioOutput` is a small value-semantic wrapper around `gpio_dt_spec`:

```cpp
zest::GpioOutput led{GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios)};

led.init(zest::GpioState::inactive);
led.set(zest::GpioState::active);
led.toggle();
```

`active` and `inactive` respect devicetree flags such as `GPIO_ACTIVE_LOW`;
applications should rarely deal in raw electrical high and low values.

```cpp
enum class GpioState {
    inactive,
    active,
};

class GpioOutput {
public:
    constexpr explicit GpioOutput(gpio_dt_spec spec) noexcept;

    [[nodiscard]] std::expected<void, int>
    init(GpioState initial = GpioState::inactive) const noexcept;
    [[nodiscard]] std::expected<void, int> set(GpioState) const noexcept;
    [[nodiscard]] std::expected<void, int> toggle() const noexcept;
    [[nodiscard]] std::expected<GpioState, int> get() const noexcept;
};
```

## 3. GPIO inputs and buttons

Raw digital input remains separate from button semantics:

```cpp
zest::GpioInput input{button_spec};
input.init();
auto state = input.get();
```

`Button` adds application behavior without invoking application callbacks from
an ISR:

```cpp
zest::Button button{button_spec, {
    .debounce = 30ms,
    .long_press = 1s,
}};

auto event = button.wait();
```

Events include `pressed`, `released`, `clicked`, and `long_pressed`.
`enable_interrupts()` translates both-edge ISR activity into a semaphore;
`poll()` and `wait()` perform debouncing and event decoding in thread context.
The class does not own a `std::function` or invoke arbitrary application code
from an ISR.

## 4. Sampling and signal-processing helpers

These helpers work with ADCs, sensors, and derived measurements:

```cpp
zest::MovingAverage<std::int32_t, 16> average;
zest::ExponentialMovingAverage<std::int32_t> low_pass{0.1};
zest::Hysteresis<std::int32_t> low_battery{3400, 3500};
zest::RateLimiter limiter{1s};
zest::Debouncer<bool> debouncer{30ms};
```

The implemented set includes:

- `MovingAverage<T, N>`
- `MedianFilter<T, N>`
- `ExponentialMovingAverage<T>`
- `Hysteresis<T>`
- `Calibration<T>` for gain and offset
- `LinearMap` for converting numeric ranges
- `ThresholdDetector`
- `RateLimiter`
- `Debouncer<T>`

The numerical helpers have no Zephyr dependency. Timing helpers use standard
chrono clocks and remain independent of Zephyr scheduling APIs.

## 5. Typed sensor access

Zest does not introduce an abstract virtual `Sensor` base class. Concrete
readers and compile-time composition are preferred:

```cpp
struct Temperature {
    std::int32_t milli_celsius;
};

zest::SensorChannel<Temperature> temperature{
    device,
    SENSOR_CHAN_AMBIENT_TEMP,
};

auto sample = temperature.read();
```

Timestamped values can use:

```cpp
template<typename T>
struct Sample {
    T value;
    std::chrono::milliseconds timestamp;
};
```

Sensor access targets Zephyr's Read-and-Decode API instead of building a large
abstraction around the older `sensor_sample_fetch()` and
`sensor_channel_get()` pair. The implemented sensor surface is:

- `SensorReader`: synchronous access to one sensor or channel.
- `SensorBatch`: several channels in one operation.
- `AsyncSensorReader`: RTIO-backed acquisition.
- `PeriodicSampler<T>`: owns scheduling policy rather than the sensor.

The API makes RTIO explicit: concurrent, batched, or streaming I/O uses an
application-provided context and mempool, while `SensorReader::read()` keeps a
single operation blocking and straightforward.

## 6. PWM and higher-level actuators

```cpp
zest::PwmOutput pwm{PWM_DT_SPEC_GET(DT_ALIAS(motor))};

pwm.init();
pwm.set_duty_cycle(0.5);
pwm.disable();
```

The composed abstractions are:

- `DimmableLed`
- `RgbLed`
- `Servo`
- `Buzzer`
- `LedPatternPlayer`

`LedPatternPlayer` provides reusable connecting, connected, warning, and
failure patterns for Wi-Fi and Bluetooth applications.

## 7. Persistent settings

The typed `Settings` wrapper removes callback and serialization boilerplate:

```cpp
zest::Settings settings{"app"};

settings.init();
settings.set("wifi/ssid", std::as_bytes(std::span{ssid}));
auto period = settings.get<std::chrono::seconds>("sample_period");
```

Only trivially copyable values receive an automatic binary codec.
Structured or versioned values require an explicit serializer. The backend,
partition, and persistence policy remain application configuration.

## 8. Networking additions

The networking layer beside `WifiManager` and `HttpClient` includes:

- `DnsResolver`
- `SntpClient` or `TimeSynchronizer`
- `MqttClient`
- `UdpSocket` and `TcpSocket`
- `NetworkMonitor`
- `ProvisioningManager`
- `CertificateStore`
- `RetryPolicy` and `ExponentialBackoff`

`RetryPolicy` and `ExponentialBackoff` are reusable policies. `WifiManager`
uses the common policy internally; other clients expose failures so an
application can apply the same policy at the operation boundary it chooses.

## 9. System-management facilities

The system layer includes:

- `Watchdog`
- `RebootReason`
- `DeviceIdentity`
- `PowerManager` or `SleepController`
- `Rtc`
- `PeriodicTimer`
- `WorkItem`
- `StaticThread`
- `MessageQueue<T, N>`
- `RetainedValue<T>`

The kernel surface intentionally stays small. Zephyr primitives have
static-initialization and ISR semantics that a generic RAII surface can easily
obscure, and comprehensive standard-library-adjacent wrappers remain zpp's
domain.

## Implementation status

The roadmap is implemented. The public headers are grouped as follows:

| Area | Public facilities |
| --- | --- |
| Analog and digital I/O | `AdcChannel`, `VoltageDivider`, `GpioInput`, `GpioOutput`, `Button` |
| Signal processing | `MovingAverage`, `MedianFilter`, `ExponentialMovingAverage`, `Hysteresis`, `ThresholdDetector`, `Calibration`, `LinearMap`, `RateLimiter`, `Debouncer` |
| Sensors | `SensorReader`, `SensorBatch`, `AsyncSensorReader`, `SensorChannel`, `PeriodicSampler`, `Sample` |
| Actuators | `PwmOutput`, `DimmableLed`, `RgbLed`, `Servo`, `Buzzer`, `LedPatternPlayer` |
| Persistence | `Settings`, `ProvisioningManager`, `RetainedValue` |
| Networking | `DnsResolver`, `UdpSocket`, `TcpSocket`, `SntpClient`, `TimeSynchronizer`, `MqttClient`, `NetworkMonitor`, `CertificateStore`, `WifiManager`, `HttpClient` |
| System | `Watchdog`, `RebootReason`, `DeviceIdentity`, `PowerManager`, `SleepController`, `Rtc`, `PeriodicTimer`, `WorkItem`, `StaticThread`, `MessageQueue` |

All hardware-facing components retain native Zephyr interoperability, and all
fixed-capacity templates make their storage cost visible in the type.
