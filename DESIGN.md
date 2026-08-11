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
  blocks the higher layers need, plus the few primitives an application would
  otherwise reach into the C API for.
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
reads as `-ENOENT`, and `DNS_EAI_MEMORY` is `-12`, which reads as `-ENOMEM`. As a
bare `int` the two domains are indistinguishable.

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
lambda that captures, leaving static trampolines and hand-cast context pointers as
the only option. `WorkItem`, `DelayableWorkItem`, `StaticThread`, `MqttClient`,
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
`BatteryMonitor`. It stays its own header because reconstructing an input voltage
from a divider is not battery-specific.

```text
AdcChannel -> VoltageDivider -> BatteryMonitor -> BatteryCurve::percent_at()
```

`zest/battery.hpp` holds both halves of the battery layer, but they remain
separate types because they fail differently. A discharge curve is a constant of
the design, validated once; reading the cell is I/O that fails per call. Fusing
them would force every charge estimate to handle three `CurveError` cases a valid
literal can never produce.

`battery_curve()` is `consteval`, so a malformed literal curve fails the build
rather than becoming a runtime error nobody handles. `percent_at()` cannot fail.

The curve half depends on nothing — not even Zephyr — so it is exercised by the
host test suite and costs a curve-only image no driver headers. `BatteryMonitor`
is therefore declared under `CONFIG_ZEST_BATTERY_MONITOR`, which is exactly when
the ADC dependencies it pulls in are guaranteed present.

## 6. GPIO

`GpioOutput` tracks the last state it drove, so `state()` is exact and
infallible. It does not read the pin back: Zephyr documents `gpio_pin_get_dt()`
for *input* pins, and a pin configured `GPIO_OUTPUT_*` has its input buffer
disabled on most SoCs, so the read returns a driver constant dressed up as a
`GpioState`.

`read_pin()` covers the cases where the electrical level genuinely matters, and
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
`MessageQueue` is a kernel synchronization primitive that can block, wakes a
waiter per message, and cannot be inspected without removing, which makes it the
wrong tool for buffering a sample stream.

`push_overwrite()` and `clear()` touch both ends, so they require the two sides to
be the same context or externally synchronized. `try_push()`, `try_pop()`,
`peek()` and `drain()` respect the single-producer, single-consumer split.

## 10. Kernel facilities

The kernel surface stays small, covering the three shapes an application would
otherwise reach into the C API for: `DelayableWorkItem` for retry, timeout and
debounce; `WorkQueue<StackSize>` so slow work stays off the system queue; and
`Mutex` with `ScopedLock`, which releases on the early-return paths where a
hand-written unlock gets forgotten.

`detail::timeout()` converts durations through microseconds and rounds up, so a
requested wait is never shorter than asked. Truncating to milliseconds turned
`put(value, 500us)` into a non-blocking call reporting a failure the caller could
not distinguish from a full queue.

`StaticThread` and `WorkQueue` accept a name, which is what makes a fault dump or
the thread analyzer readable.

**Storage duration.** `StaticThread` and `WorkQueue` embed Zephyr stack macros
whose architecture-specific alignment only holds for objects with static storage
duration. Declare them at file or class scope, never on another thread's stack.
A `static_assert` cannot detect this, so it is a contract.

## 11. Watchdog

Zephyr's watchdog API is per-device for setup and per-channel for feeding:
`wdt_install_timeout()` adds a channel, but `wdt_setup()` starts the peripheral
and is rejected once it is running. Modeling a channel as if it owned the device
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

`BluetoothManager` covers both roles: central-role `connect()` takes a BLE
identity address, and peripheral-role `start_advertising()` serves the archetypal
sensor node — advertise, accept a connection, expose a characteristic.

## 15. Thread safety

| Category | Contract |
| --- | --- |
| Pure value types (`Error`, `Quantity`, filters, transforms, `PidController`, `StateMachine`) | Not synchronized. One owner, or the caller synchronizes. |
| `SpscRingBuffer` | One producer, one consumer, concurrently. `push_overwrite()` and `clear()` need both sides in one context. |
| `MessageQueue`, `Semaphore`, `Mutex` | Fully thread safe. `try_put`, `try_get` and `give` are ISR-safe. |
| `WorkItem`, `DelayableWorkItem` | `submit()`/`schedule()` are ISR-safe. Replacing a handler must not race a pending run. |
| Hardware handles (`AdcChannel`, `GpioOutput`, `PwmOutput`) | One owner per peripheral. |
| `WifiManager`, `BluetoothManager` | Lifecycle operations are serialized internally. One instance per interface or stack; a second is inert and reports `errors::no_device`. |
| `HttpClient`, `MqttClient`, sockets | One owner. Not safe to use from two threads at once. |
| `Button`, `LedPatternPlayer` | Poll from one context. |

## 16. Testing

Two layers, deliberately separated:

- **`tests/host`** builds the Zephyr-independent headers with a plain host
  compiler under `-Werror`. It runs in seconds without a west workspace, which is
  what makes the numeric layers practical to iterate on. Ten suites.
- **`tests/smoke`** is a `ztest` application. Every `*.conf` is a separate
  configuration, and CI discovers them by glob, so adding a `.conf` adds it to the
  matrix and the two cannot drift apart.

`native_sim` produces a real executable, so CI runs the suites rather than only
building them.

`PowerManager` needs a board advertising `HAS_PM`, which `native_sim` does not, so
`power.conf` is built separately on `qemu_cortex_m3` and CI asserts the option
actually took effect. An option that silently resolves to `n` is a test that
passes while covering nothing.

## 17. Buses

`I2cDevice`, `SpiDevice` and `Uart` cover the part that has no Zephyr driver,
which every sensing project eventually has. They follow the library's conventions
--- spans rather than pointer-and-length, `Result` rather than errno, no
allocation --- and expose their native specs for anything they do not cover.

`SpiDevice` offers transfers rather than a register protocol, because the
read/write bit's position varies by part. `Uart` is deliberately the polling API:
no interrupt configuration, no ring buffer sizing, nothing beyond
`CONFIG_SERIAL`. Its reads take a timeout, so they cannot hang a thread the way a
raw `uart_poll_in()` loop does.

## 18. Serialization

`zest/schema.hpp` declares a struct's shape once; `zest/json.hpp` and
`zest/cbor.hpp` are codecs over it, and `zest/serde.hpp` makes the format a
template argument. Declaring the shape twice would guarantee the two descriptions
drift, so there is exactly one.

The field table is Zephyr's `json_obj_descr`. Reusing it rather than inventing a
parallel type is deliberate: it already carries every fact a serializer needs ---
name, offset, width, kind, and sub-tables for nested objects and arrays --- and it
lets the JSON codec hand the table to Zephyr unchanged. The header declares the
struct without requiring `CONFIG_JSON_LIBRARY`, so a CBOR-only image does not drag
in the JSON library.

Deducing the representation from the member type is the point: a hand-written
token against the wrong width decodes incorrectly and silently, and a C macro
cannot check it. The mapping was chosen after confirming Zephyr's
`equivalent_types()` accepts a document `NUMBER` against every numeric descriptor
and treats `TRUE` and `FALSE` as interchangeable for bools.

| C++ member | Representation |
| --- | --- |
| `bool` | boolean |
| `int8/16/32_t`, `uint8/16/32_t` | integer, width from the member |
| `int64_t`, `uint64_t` | 64-bit integer |
| `float`, `double` | floating point |
| `char[N]`, `std::array<char, N>` | text, **copied** |
| `char *`, `const char *` | text, **borrowing** (JSON only on decode) |
| `enum` | its underlying integer type |
| a type with its own schema | nested object |
| array plus a `std::size_t` count | array |

`char[N]` mapping to a copy is a case where the C++ layer offers something the C
API does not: Zephyr implements `JSON_TOK_STRING_BUF` but publishes no macro for
it, so from C the path of least resistance is the borrowing `char *` form, and the
borrowing is easy to miss until the buffer is reused.

### Where the codecs differ, and why

CBOR delegates to zcbor, the library Zephyr already uses for MCUmgr, LwM2M
SenML-CBOR and CoAP. It is the better default where both ends are yours: about
half the bytes, no text parsing, decoding never touches the input buffer, and
nothing depends on NUL termination.

Two asymmetries are deliberate rather than incidental:

- **`char *` decodes under JSON but not CBOR.** JSON's parser can terminate the
  token in place; CBOR text is length-prefixed and not terminated, so a C string
  pointed at it would read past the value into the next item. The CBOR codec
  refuses rather than returning that pointer.
- **Arrays of objects work under JSON but not CBOR.** Zephyr's descriptor records
  an object array's element *descriptor* but not its element *stride*, which
  Zephyr recomputes internally from member alignments. Replicating that arithmetic
  risks striding through the array incorrectly, so the CBOR codec rejects the shape
  with a `static_assert` --- a build error naming the problem, rather than a
  runtime failure or, worse, silently wrong data.

Two implementation constraints are worth recording, because both are invisible
until you try:

- **Zephyr's array macros cannot be used from C++.** `Z_JSON_ELEMENT_DESCR` builds
  its element descriptor with a C99 compound literal, which is not valid C++.
  Element descriptors are named `static constexpr` members of a helper template
  instead, which also places them in read-only memory.
- **Union arms are initialized, never assigned.** `json_obj_descr` holds an
  anonymous union, and activating a member by assigning to a subobject is not
  permitted in a constant expression, so each descriptor is one designated
  initializer chosen with `if constexpr`.

Structural limits are diagnostics rather than silent truncation: offsets exceed
the descriptor's 16-bit field beyond 64 KB, names exceed its 7-bit length beyond
127 characters, and presence is reported in a 64-bit bitmap so a schema is capped
at 64 fields.

`Parsed<T>` carries that bitmap. A field that was absent keeps its
value-initialized contents, indistinguishable from a field that was present and
zero, so `has()` exists for the cases where those differ. Both codecs skip keys
they do not model, so a sender adding fields does not break a receiver.

## Implementation status

Everything described above is implemented. Known gaps, deliberately not yet
covered:

- No flash-backed record store over `stream_flash` or ZMS, so offline logging
  while the network is down remains application code. The codecs cover payload
  encoding; the durable buffer behind them does not exist yet.
- No GATT service or characteristic helper beyond advertising.
- The codecs inherit their libraries' constraints: JSON parsing mutates its
  buffer and borrows into it, every schema field is always encoded, and members
  must be C-compatible. Per-value omission, or parsing into a `std::string_view`,
  would require owning a codec rather than wrapping one.
- The CBOR codec does not support arrays of objects; see above.
- `HttpClient` exposes no response headers and does not follow redirects; Zephyr's
  client offers no user-data slot on its header callback in this version, so the
  `HeaderHandler` overload is accepted for API stability and currently ignored.
