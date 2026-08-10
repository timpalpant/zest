# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_include_directories(include)

zephyr_library_sources_ifdef(CONFIG_ZEST_BATTERY_MONITOR
  src/battery_monitor.cpp
)
zephyr_library_sources_ifdef(CONFIG_ZEST_HTTP_CLIENT
  src/http_client.cpp
)
zephyr_library_sources_ifdef(CONFIG_ZEST_WIFI_MANAGER
  src/wifi_manager.cpp
)
zephyr_library_sources_ifdef(CONFIG_ZEST_BLUETOOTH_MANAGER
  src/bluetooth_manager.cpp
)
