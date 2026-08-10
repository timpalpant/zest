<!--
Copyright (c) 2026 Timothy Palpant

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Zest design and component guide

Zest is an allocation-conscious C++23 library that complements Zephyr's C APIs
with typed hardware handles, reusable embedded behavior, and application-level
services. It favors explicit initialization, explicit errors, caller-owned
storage, static composition, and `noexcept` APIs.

## Relationship with zpp

[lowlander/zpp](https://github.com/lowlander/zpp) is a similar but distinct
project. It describes itself as an experimental C++20 wrapper around Zephyr's C
API and currently covers primarily kernel and standard-library-adjacent
facilities. It is header-only, declares its API unstable, and its latest commit
was made in January 2023 for the Zephyr 3.1 era.

Zest complements zpp **without depending on it**:

- Zest owns typed peripheral wrappers, signal-processing helpers, control
  primitives, and application services.
- Zest does not attempt to replace the standard library with a broad
  kernel-primitives layer. Its kernel helpers are the fixed-storage building
  blocks the higher layers need, plus the few primitives whose absence forced
  application code back to the C API.
- Zest uses the C++23 standard library directly where the configured toolchain
  provides it, including `std::expected`, `std::span`, and `std::chrono`.
- Public wrappers expose native Zephyr handles where that materially improves
  interoperability, but zpp is not a build or API dependency.

## Architectural layers

| Layer | Examples | Purpose |
| --- | --- | --- |
| Vocabulary | `Error`, `Result<T>`, `Quantity`, `FunctionRef` | Types every other layer speaks in |
| Hardware handles | `AdcChannel`, `GpioOutput`, `PwmOutput` | Thin, typed Zephyr wrappers |
| Reusable behavior | `MovingAverage`, `PidController`, `RetryPolicy` | Hardware-independent, host-testable |
| Services | `BatteryMonitor`, `WifiManager`, `HttpClient` | Stateful application operations |

Zephyr already supplies hardware abstraction through `device`, `adc_dt_spec`,
`gpio_dt_spec`, and related devicetree-aware structures. Zest does not add an
inheritance hierarchy over them. Thin value-semantic wrappers instead add safe
initialization, typed results, consistent errors, and composable behavior.

## Common API conventions

- Constructors capture configuration and do not perform fallible work.
- Fallible setup is performed by an explicit `init()`, `start()`, or `connect()`.
- Expected operational failures use `Result<T>`; see the error model below.
- Public operations that cannot throw are marked `noexcept`.
- Logical states such as GPIO active/inactive are preferred over raw electrical
  levels.
- Buffers are fixed-size or caller-owned unless dynamic allocation is an explicit
  feature of the abstraction.
- Hardware-independent algorithms are header-only and host-tested.
- Per-sample and per-update paths use integer or `float` arithmetic, never
  `double`.
- Runtime polymorphism and virtual bases are avoided unless a concrete use case
  requires them.
- ISR-facing APIs neither allocate nor invoke arbitrary owning callbacks.

## 1. The error model

`Result<T>` is `std::expected<T, Error>`, and `Result<>` spells the void case.
`Error` wraps a negative errno in a trivially copyable type the size of an `int`,
so returning one costs nothing extra.

Construction is `explicit`. An `int` in this library is a value; only code that
has decided an `int` *is* a failure may say so. That closes the gap where an
error could be confused with a count, a length, or a descriptor.

Failures that are not errno values keep their own types rather than being folded
in:

| Type | Domain |
| --- | --- |
| `Error` | Negative errno from a Zephyr C API |
| `DnsError` | `enum dns_resolve_status` |
| `CurveError` | Discharge-curve validation |
| `TransformError` | Degenerate numeric ranges |
| `HttpError` | A request stage plus its `Error` cause |

Keeping DNS separate is not cosmetic: `DNS_EAI_NONAME` is `-2`, which as an errno
reads as `-ENOENT`, and `DNS_EAI_MEMORY` is `-12`, which reads as `-ENOMEM`.
Returning them as a bare `int` made the two indistinguishable.

`check()`, `check_value()` and `check_positive()` translate the
`rc < 0 ? error : value` shape that nearly every Zephyr call has, and `ZEST_TRY` /
`ZEST_TRY_ASSIGN` propagate without nesting. `Error::message()` is Kconfig-gated
so a flash-tight image can drop the table.

## 2. Units

`Quantity<Rep, Tag, Ratio>` follows the pattern `std::chrono::duration` proves
works: a representation, a tag saying *what* is measured, and a `std::ratio`
saying at what scale. Conversions that cannot lose information are implicit; the
rest need `quantity_cast`. Different tags never interconvert, so millivolts
cannot be passed where ohms are wanted.

The representation is the caller's choice, so nothing here forces floating point
onto a part without an FPU.

## 3. Callables

`FunctionRef<Sig>` is a two-pointer non-owning reference for callbacks scoped to
a call. `InplaceFunction<Sig, N>` owns a callable in fixed inline storage.

Both exist because `void (*)(void *)` plus a context argument does not accept a
lambda that captures, which forced application code to write static trampolines
and cast context pointers by hand — precisely the boilerplate this library exists
to remove. `WorkItem`, `DelayableWorkItem`, `StaticThread`, `MqttClient`,
`WifiManager` and `BluetoothManager` all take capturing lambdas, and none of them
allocate.

A callable too large for the configured storage is a compile error naming the
size required, never a silent allocation.

## 4. Arithmetic and the FPU

No common Cortex-M part — M0, M0+, M3, M4F or M33 — has a double-precision FPU.
`double` in a per-sample path is therefore a soft-float library call costing
hundreds of cycles and several hundred bytes of `__aeabi_d*` in the image.

Zest defaults to `float` where floating point is natural and provides an integer
alternative wherever one is exact:

| Floating point | Integer alternative |
| --- | --- |
| `ExponentialMovingAverage<T, float>` | `ShiftMovingAverage<T, Shift>` |
| `Calibration<T, float>` | `IntegerCalibration<T>` |
| `LinearMap<T, float>` | `integer_map<T>()` |
| `SlewRateLimiter<float>` | `slew_toward<T>()` |
| `PwmOutput::set_duty_cycle(float)` | `PwmOutput::set_duty(PerMille)` |

`ExponentialBackoff` uses an integer percentage growth factor, and `RgbLed`,
`Servo` and `Buzzer` scale in per-mille.

## 5. ADC channels

```cpp
zest::AdcChannel channel{ADC_DT_SPEC_GET(DT_ALIAS(sensor))};

ZEST_TRY(channel.init());
ZEST_TRY_ASSIGN(raw, channel.read_raw());
ZEST_TRY_ASSIGN(millivolts, channel.read_average_millivolts(16));
```

Sample width follows the channel's configured resolution, so an 18- or 24-bit
part is read correctly and an unsigned 16-bit reading above `0x7FFF` does not come
back negative.

Averaging uses `adc_sequence_options::extra_samplings`, entering the driver once
for N samples instead of N times, with a fallback for drivers that decline
multi-sampling.

`VoltageDivider` is the reusable adaptor between `AdcChannel` and
`BatteryMonitor`; charge estimation is separate, via `BatteryCurve`:

```text
AdcChannel -> VoltageDivider -> BatteryMonitor -> BatteryCurve::percent_at()
```

`battery_curve()` is `consteval`, so a malformed literal curve fails the build
rather than becoming a runtime error nobody handles. `percent_at()` cannot fail.

## 6. GPIO

`GpioOutput` tracks the last state it drove, so `state()` is exact and
infallible. This replaces reading the pin back: Zephyr documents
`gpio_pin_get_dt()` for *input* pins, and a pin configured `GPIO_OUTPUT_*` has its
input buffer disabled on most SoCs, so the read returned a driver constant dressed
up as a `GpioState`.

`read_pin()` remains for cases where the electrical level genuinely matters, and
refuses unless `init(state, /* enable_readback = */ true)` configured for it.

Because the object carries state, `set()` and `toggle()` are non-const.

## 7. Buttons

`Button` adds application behavior without invoking application code from an ISR:
`enable_interrupts()` translates both-edge activity into a semaphore, and `poll()`
and `wait()` perform debouncing and event decoding in thread context. Events are
`pressed`, `released`, `clicked` and `long_pressed`.

## 8. Sensors

Sensor access targets Zephyr's Read-and-Decode API rather than building a large
abstraction over the older `sensor_sample_fetch()` and `sensor_channel_get()`
pair:

- `SensorReader` — one blocking read of one sensor or channel.
- `SensorBatch` — several channels in one operation.
- `AsyncSensorReader` — RTIO mempool-backed acquisition.
- `SensorChannel<Data, EncodedBufferSize>` — fixed encoded storage for one typed
  channel.
- `PeriodicSampler<Source>` — owns the scheduling policy rather than the sensor.
  A read error does not advance the deadline, so a failing sensor is retried on
  the next poll rather than at the sampling interval.

RTIO is explicit: concurrent, batched or streaming I/O uses an
application-provided context and mempool.

## 9. Buffers

`SpscRingBuffer<T, N>` is a lock-free ring for exactly one producer and one
consumer. It is what a sampling callback pushes into and a worker drains:
`MessageQueue` is a kernel synchronisation primitive that can block, wakes a
waiter per message, and cannot be inspected without removing, which makes it the
wrong tool for buffering a sample stream.

`push_overwrite()` and `clear()` touch both ends, so they require the two sides to
be the same context or externally synchronised. `try_push()`, `try_pop()`,
`peek()` and `drain()` respect the single-producer, single-consumer split.

## 10. Kernel facilities

The kernel surface stays small, but three gaps forced application code back to
the C API and are now covered: `DelayableWorkItem` for the retry, timeout and
debounce shape; `WorkQueue<StackSize>` so slow work stays off the system queue;
and `Mutex` with `ScopedLock`, which releases on the early-return paths where a
hand-written unlock gets forgotten.

`detail::timeout()` converts durations through microseconds and rounds up, so a
requested wait is never shorter than asked. Truncating to milliseconds turned
`put(value, 500us)` into a non-blocking call reporting a failure the caller could
not distinguish from a full queue.

`StaticThread` and `WorkQueue` accept a name, which is what makes a fault dump or
the thread analyser readable.

**Storage duration.** `StaticThread` and `WorkQueue` embed Zephyr stack macros
whose architecture-specific alignment only holds for objects with static storage
duration. Declare them at file or class scope, never on another thread's stack.
A `static_assert` cannot detect this, so it is a contract.

## 11. Watchdog

Zephyr's watchdog API is per-device for setup and per-channel for feeding:
`wdt_install_timeout()` adds a channel, but `wdt_setup()` starts the peripheral
and is rejected once it is running. Modelling a channel as if it owned the device
meant a second channel's `init()` failed with an error that read like a driver
fault.

`WatchdogDevice` installs channels and is started once; `WatchdogChannel` only
knows how to `feed()`, and reports `errors::permission_denied` if the device has
not been started.

## 12. Persistence

`Settings` gives typed access to a subtree. Only `TriviallySerializable` values
get an automatic binary codec, and that concept rejects pointers, arrays,
references and view types.

Trivial copyability alone was not enough: `std::string_view` is trivially
copyable, so an unconstrained overload wrote a *pointer and a length* to flash for
`set(key, std::string_view{ssid})` — a call that looks exactly like the intended
one and reads back as a dangling view after reboot. Explicit `std::string_view`
overloads now persist the characters.

`set()` and `get()` use `settings_save_one`/`settings_load_one`, which bypass the
handler mechanism; `load()` calls `settings_load_subtree` so values owned by other
subsystems become visible.

`ProvisionedWifi` carries an explicit version, so a future layout change is
detected rather than misread. The pre-shared key is stored in plaintext, because
Zephyr's settings backends do not encrypt.

`RetainedValue` writes payload, then checksum, then magic word, so a reset partway
through leaves the record invalid rather than plausible. A release fence enforces
that order — without one the compiler may sink the magic store ahead of the
payload it guards, defeating the scheme.

## 13. Networking

`Poller<N>` wraps `zsock_poll`, so one blocking call covers every socket an event
loop owns.

`MqttClient` exposes what a loop actually needs — `poll_fd()` and
`keepalive_time_left()` — rather than leaving them to be dug out of the transport
union. `publish()` draws message ids from a monotonic counter; defaulting every
message to `1` made QoS 1 and 2 acknowledgements uncorrelatable.

`WifiManager` covers `open`, WPA2-PSK, PSK-SHA256, WPA3-SAE and enterprise
security, plus band and BSSID pinning, `scan()` for provisioning UIs, and
state-change callbacks so applications can react rather than poll.
`ConnectionInfo` is sized for IPv6 and exposes `string_view` accessors.

`HttpClient` can keep a connection open for a following request to the same
origin. A device posting telemetry on a timer otherwise pays a fresh TCP and TLS
handshake every time, which on a battery is the dominant energy cost of reporting.
Its stack cost is documented and Kconfig-tunable, and TLS is a separate option so
a plain-HTTP device does not pull in mbedTLS.

`StaticCredential` registers a credential that already lives in read-only memory.
`tls_credential_add()` retains the caller's pointer rather than copying, so
copying into RAM first spends the credential's size for no benefit — on a small
part, a meaningful fraction of the budget. `OwnedCredential<N>` copies, for a
credential that arrives at run time.

## 14. Bluetooth

`BluetoothManager` covers both roles. Central-role `connect()` takes a BLE
identity address; peripheral-role `start_advertising()` was previously absent,
which left the archetypal sensor node — advertise, accept a connection, expose a
characteristic — unable to use the class at all.

## 15. Thread safety

| Category | Contract |
| --- | --- |
| Pure value types (`Error`, `Quantity`, filters, transforms, `PidController`, `StateMachine`) | Not synchronised. One owner, or the caller synchronises. |
| `SpscRingBuffer` | One producer, one consumer, concurrently. `push_overwrite()` and `clear()` need both sides in one context. |
| `MessageQueue`, `Semaphore`, `Mutex` | Fully thread safe. `try_put`, `try_get` and `give` are ISR-safe. |
| `WorkItem`, `DelayableWorkItem` | `submit()`/`schedule()` are ISR-safe. Replacing a handler must not race a pending run. |
| Hardware handles (`AdcChannel`, `GpioOutput`, `PwmOutput`) | One owner per peripheral. |
| `WifiManager`, `BluetoothManager` | Lifecycle operations are serialised internally. One instance per interface or stack; a second is inert and reports `errors::no_device`. |
| `HttpClient`, `MqttClient`, sockets | One owner. Not safe to use from two threads at once. |
| `Button`, `LedPatternPlayer` | Poll from one context. |

## 16. Testing

Two layers, deliberately separated:

- **`tests/host`** builds the Zephyr-independent headers with a plain host
  compiler under `-Werror`. It runs in seconds without a west workspace, which is
  what makes the numeric layers practical to iterate on. Ten suites.
- **`tests/smoke`** is a `ztest` application. Every `*.conf` is a separate
  configuration, and CI discovers them by glob so the matrix cannot drift: ten of
  the fourteen configurations were previously never built at all.

`native_sim` produces a real executable, so CI runs the suites rather than only
building them. Building alone would not have caught a single assertion.

`PowerManager` needs a board advertising `HAS_PM`, which `native_sim` does not, so
`power.conf` is built separately on `qemu_cortex_m3` and CI asserts the option
actually took effect. An option that silently resolves to `n` is a test that
passes while covering nothing.

## Implementation status

Everything described above is implemented. Known gaps, deliberately not yet
covered:

- No typed `I2cDevice`, `SpiDevice` or `Uart` handles. A project always has one
  sensor without a Zephyr driver, and today that means calling
  `i2c_write_read_dt()` directly.
- No flash-backed record store over `stream_flash` or ZMS, and no CBOR writer, so
  offline logging and compact telemetry payloads remain application code.
- No GATT service or characteristic helper beyond advertising.
- `HttpClient` exposes no response headers and does not follow redirects; Zephyr's
  client offers no user-data slot on its header callback in this version, so the
  `HeaderHandler` overload is accepted for API stability and currently ignored.
