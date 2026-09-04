# Copyright (c) 2026 Timothy Palpant
#
# SPDX-License-Identifier: LGPL-3.0-or-later

if(NOT CONFIG_ZEST)
  return()
endif()

zephyr_library()
zephyr_include_directories(include)

# Always built: Error::message() has no dependency beyond CONFIG_ZEST, and this
# also guarantees the library is never source-empty.
zephyr_library_sources(src/error.cpp)

zephyr_library_sources_ifdef(CONFIG_ZEST_ADC_CHANNEL src/adc_channel.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_GPIO src/gpio.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_I2S src/i2s.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_FLASH_PARTITION src/flash_partition.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_PWM_OUTPUT src/pwm.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_SENSOR src/sensor.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_NETWORK src/network.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_SNTP_CLIENT src/sntp_client.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_TIME_SYNCHRONIZER src/time_synchronizer.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_NETWORK_MONITOR src/network_monitor.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_SYSTEM src/system.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_HTTP_CLIENT src/http_client.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_FIRMWARE_UPDATE src/firmware_update.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_USB_DEVICE src/usb_device.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_WIFI_MANAGER src/wifi_manager.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_BLE src/ble.cpp)
zephyr_library_sources_ifdef(CONFIG_ZEST_BLUETOOTH_MANAGER src/bluetooth_manager.cpp)
