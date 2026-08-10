# Zest

Zest is an allocation-conscious C++23 service library for
[Zephyr RTOS](https://zephyrproject.org/). It wraps common embedded operations
in small, synchronous APIs built around `std::expected`, `std::span`, and
`std::chrono`.

- `zest::BatteryMonitor` samples a voltage-divider-backed battery and estimates
  state of charge using an application-supplied discharge curve.
- `zest::WifiManager` connects, retries transient association failures, waits
  for DHCP, reports status, and controls power saving.
- `zest::HttpClient` provides session-style HTTP/1.1 defaults, HTTPS/SNI, and
  caller-owned response storage.
- `zest::BluetoothManager` manages Zephyr BLE lifecycle and central-role
  connections.

[API reference](https://timpalpant.github.io/zest/api/) ·
[project website](https://timpalpant.github.io/zest/) ·
[source](https://github.com/timpalpant/zest)

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
CONFIG_ZEST_BATTERY_MONITOR=y
CONFIG_ZEST_WIFI_MANAGER=y
CONFIG_ZEST_HTTP_CLIENT=y
CONFIG_ZEST_BLUETOOTH_MANAGER=y
```

The corresponding Zephyr facilities remain application policy:

- `BatteryMonitor` requires ADC.
- `WifiManager` requires networking, IPv4, and Wi-Fi.
- `HttpClient` requires sockets, TCP, DNS, Zephyr's HTTP client, and TLS
  sockets.
- `BluetoothManager` selects the Zephyr BLE host, central role, and dynamic
  device names. The board must still provide a Bluetooth controller.

## Examples

### Battery

```cpp
#include <array>
#include <zest/battery_monitor.hpp>

constexpr std::array discharge_curve{
    zest::CurvePoint{4200, 100},
    zest::CurvePoint{3700, 10},
    zest::CurvePoint{3300, 0},
};
constexpr adc_dt_spec battery_adc = ADC_DT_SPEC_GET(DT_NODELABEL(vbatt));
zest::BatteryMonitor battery{
    battery_adc,
    DT_PROP(DT_NODELABEL(vbatt), output_ohms),
    DT_PROP(DT_NODELABEL(vbatt), full_ohms),
    discharge_curve,
};

if (auto initialized = battery.init(); initialized) {
    auto reading = battery.read();
}
```

Curve points must be ordered from highest to lowest voltage with
non-increasing percentages. The caller owns the curve storage, which must
outlive the monitor.

### Wi-Fi

```cpp
#include <zest/wifi_manager.hpp>

zest::WifiManager wifi;
auto connection = wifi.connect({.ssid = "network", .password = "password"});
if (connection) {
    printk("IPv4: %s\n", connection->address.data());
}
```

`connect()` waits for a usable DHCP address. Transient association and stale
disconnect events are retried with bounded exponential backoff until the
caller's overall timeout expires.

### HTTP

```cpp
#include <zest/http_client.hpp>

using namespace std::chrono_literals;

std::array<std::byte, 2048> body;
constexpr sec_tag_t ca_tags[] = {42};
zest::HttpClient client{zest::HttpClient::Options{
    .timeout = 10s,
    .user_agent = "my-device/1.0",
    .peer_verification = zest::HttpClient::PeerVerification::required,
    .security_tags = ca_tags,
}};

auto response = client.get("https://example.com/", body);
```

Register each CA with `tls_credential_add()` before the request. PEM lengths
include the trailing NUL; DER lengths do not. Required certificate validation
also needs a valid system clock.

### Bluetooth LE

```cpp
#include <zest/bluetooth_manager.hpp>

zest::BluetoothManager bluetooth;
if (auto enabled = bluetooth.enable("sensor-node"); enabled) {
    auto connection = bluetooth.connect({
        .address = "12:34:56:78:9A:BC",
        .type = zest::BluetoothManager::AddressType::random,
    });
}
```

## Design constraints

The APIs avoid owning dynamic buffers: strings and byte storage are supplied by
the caller and must remain alive for the documented operation or client
lifetime. Managers serialize lifecycle operations and expose Zephyr failures as
negative errno values through `std::expected`.

Platform configuration remains outside the library. Network pools, TLS
algorithms and credentials, DHCP policy, Wi-Fi drivers, Bluetooth controllers,
and devicetree wiring belong to the application and board.

## Documentation

Install Doxygen and run:

```sh
mkdir -p build/docs
doxygen Doxyfile
```

Open `build/docs/html/index.html`. GitHub Pages publishes the landing page and
generated API reference automatically from `master`.
