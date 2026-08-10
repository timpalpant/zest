/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>

#include <chrono>
#include <cstdint>

namespace zest
{

class WatchdogChannel;

/**
 * One watchdog peripheral.
 *
 * Zephyr's watchdog API is per-device for setup and per-channel for feeding:
 * `wdt_install_timeout()` adds a channel, but `wdt_setup()` starts the whole
 * peripheral and is rejected once it is running. Modelling a channel as if it
 * owned the device --- as an earlier version did --- meant a second channel's
 * `init()` failed with an error that read like a driver fault.
 *
 * So install every channel first, then `start()` once:
 *
 * ```cpp
 * zest::WatchdogDevice watchdog{DEVICE_DT_GET(DT_ALIAS(watchdog0))};
 * auto worker = watchdog.install(2s);
 * auto radio  = watchdog.install(30s);
 * (void)watchdog.start();
 * (void)worker->feed();
 * ```
 */
class WatchdogDevice
{
      public:
	constexpr explicit WatchdogDevice(const struct device *device) noexcept : device_{device}
	{
	}
	WatchdogDevice(const WatchdogDevice &) = delete;
	WatchdogDevice &operator=(const WatchdogDevice &) = delete;

	/**
	 * Install a timeout channel. Must be called before `start()`.
	 *
	 * @p timeout is the window within which the channel must be fed.
	 */
	[[nodiscard]] Result<WatchdogChannel>
	install(std::chrono::milliseconds timeout,
		std::uint8_t reset_flags = WDT_FLAG_RESET_SOC) noexcept;

	/** Start the peripheral. Call once, after installing every channel. */
	[[nodiscard]] Result<> start(std::uint8_t options = 0U) noexcept;

	/** Stop the peripheral, where the hardware permits it. */
	[[nodiscard]] Result<> stop() noexcept;

	[[nodiscard]] bool running() const noexcept
	{
		return running_;
	}
	[[nodiscard]] const struct device *native_handle() const noexcept
	{
		return device_;
	}

      private:
	const struct device *device_{};
	bool running_{false};
};

/**
 * A single installed watchdog channel.
 *
 * Obtained from `WatchdogDevice::install()`. Feeding a channel before the device
 * has been started reports `errors::permission_denied` rather than silently
 * doing nothing.
 */
class WatchdogChannel
{
      public:
	/** Reset the channel's timer. */
	[[nodiscard]] Result<> feed() const noexcept;

	[[nodiscard]] constexpr int channel_id() const noexcept
	{
		return channel_;
	}

      private:
	friend class WatchdogDevice;

	constexpr WatchdogChannel(const WatchdogDevice *owner, int channel) noexcept
		: owner_{owner}, channel_{channel}
	{
	}

	const WatchdogDevice *owner_{nullptr};
	int channel_{-1};
};

} /* namespace zest */
