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
 * Setup is per-device and feeding is per-channel, and the peripheral rejects
 * further channels once it is running. Install every channel first, then
 * `start()` once:
 *
 * ```cpp
 * zest::WatchdogDevice watchdog{DEVICE_DT_GET(DT_ALIAS(watchdog0))};
 * auto worker = watchdog.install(2s);
 * auto radio  = watchdog.install(30s);
 * (void)watchdog.start();
 * (void)worker->feed();
 * ```
 *
 * Installed channels do not borrow this wrapper. They retain the native Zephyr
 * device pointer, whose lifetime is static, and may safely outlive the
 * `WatchdogDevice` used to install them.
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
 * Obtained from `WatchdogDevice::install()`. This is a self-contained value
 * handle: it does not borrow the `WatchdogDevice` used to install it.
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

	constexpr WatchdogChannel(const struct device *device, int channel) noexcept
		: device_{device}, channel_{channel}
	{
	}

	const struct device *device_{nullptr};
	int channel_{-1};
};

} /* namespace zest */
