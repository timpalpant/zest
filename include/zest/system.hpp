/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace zest
{

/** Hardware reset-cause snapshot. */
struct RebootReason {
	std::uint32_t causes{};
	std::uint32_t supported{};
	[[nodiscard]] bool has(std::uint32_t cause) const noexcept
	{
		return (causes & cause) != 0U;
	}

	[[nodiscard]] static std::expected<RebootReason, int> read() noexcept;
	[[nodiscard]] static std::expected<void, int> clear() noexcept;
};

/** Allocation-free access to the SoC's stable hardware identifier. */
class DeviceIdentity
{
      public:
	template <std::size_t Capacity = 16U>
	[[nodiscard]] static std::expected<std::array<std::byte, Capacity>, int> read() noexcept
	{
		std::array<std::byte, Capacity> id{};
		const auto size = hwinfo_get_device_id(reinterpret_cast<std::uint8_t *>(id.data()),
						       id.size());
		if (size < 0) {
			return std::unexpected(static_cast<int>(size));
		}
		if (static_cast<std::size_t>(size) < id.size()) {
			std::fill(id.begin() + size, id.end(), std::byte{});
		}
		return id;
	}

	[[nodiscard]] static std::expected<std::array<std::byte, 8>, int> eui64() noexcept;
};

/** One installed watchdog channel. */
class Watchdog
{
      public:
	constexpr explicit Watchdog(const struct device *device) noexcept : device_{device}
	{
	}
	[[nodiscard]] std::expected<void, int> init(std::chrono::milliseconds timeout,
						    std::uint8_t reset_flags = WDT_FLAG_RESET_SOC,
						    std::uint8_t options = 0U) noexcept;
	[[nodiscard]] std::expected<void, int> feed() const noexcept;
	[[nodiscard]] std::expected<void, int> disable() noexcept;
	[[nodiscard]] const struct device *native_handle() const noexcept
	{
		return device_;
	}

      private:
	const struct device *device_{};
	int channel_{-1};
	bool active_{};
};

/** Typed access to a Zephyr RTC device. */
class Rtc
{
      public:
	constexpr explicit Rtc(const struct device *device) noexcept : device_{device}
	{
	}
	[[nodiscard]] std::expected<void, int> init() const noexcept;
	[[nodiscard]] std::expected<rtc_time, int> get() const noexcept;
	[[nodiscard]] std::expected<void, int> set(const rtc_time &time) const noexcept;
	[[nodiscard]] const struct device *native_handle() const noexcept
	{
		return device_;
	}

      private:
	const struct device *device_{};
};

/** Explicit Zephyr power-state policy requests. */
class PowerManager
{
      public:
	[[nodiscard]] static std::expected<void, int>
	force_next(std::uint8_t cpu, pm_state state, std::uint8_t substate_id = 0U,
		   std::uint32_t minimum_residency_us = 0U,
		   std::uint32_t exit_latency_us = 0U) noexcept;
};

/** Thread-context sleeps with chrono durations. */
class SleepController
{
      public:
	template <typename Rep, typename Period>
	static void sleep_for(std::chrono::duration<Rep, Period> duration) noexcept
	{
		k_sleep(K_MSEC(
			std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()));
	}
};

} /* namespace zest */
