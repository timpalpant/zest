/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/settings.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <string_view>

namespace zest
{

struct ProvisionedWifi {
	std::array<char, 33> ssid{};
	std::array<char, 65> password{};
	std::uint8_t ssid_length{};
	std::uint8_t password_length{};

	[[nodiscard]] std::string_view ssid_view() const noexcept
	{
		return {ssid.data(), ssid_length};
	}
	[[nodiscard]] std::string_view password_view() const noexcept
	{
		return {password.data(), password_length};
	}
};

/** Persistent, fixed-storage Wi-Fi provisioning state. */
template <std::size_t MaximumNameLength = 64U> class ProvisioningManager
{
      public:
	explicit constexpr ProvisioningManager(std::string_view settings_root = "zest") noexcept
		: settings_{settings_root}
	{
	}

	[[nodiscard]] std::expected<void, int> init() const noexcept
	{
		return settings_.init();
	}

	[[nodiscard]] std::expected<void, int> provision(std::string_view ssid,
							 std::string_view password) const noexcept
	{
		if (ssid.empty() || ssid.size() > 32U || password.size() > 64U) {
			return std::unexpected(-EINVAL);
		}
		ProvisionedWifi value{};
		std::copy(ssid.begin(), ssid.end(), value.ssid.begin());
		std::copy(password.begin(), password.end(), value.password.begin());
		value.ssid_length = static_cast<std::uint8_t>(ssid.size());
		value.password_length = static_cast<std::uint8_t>(password.size());
		return settings_.set("wifi", value);
	}

	[[nodiscard]] std::expected<ProvisionedWifi, int> credentials() const noexcept
	{
		auto result = settings_.template get<ProvisionedWifi>("wifi");
		if (!result) {
			return result;
		}
		if (result->ssid_length == 0U || result->ssid_length > 32U ||
		    result->password_length > 64U) {
			return std::unexpected(-EBADMSG);
		}
		return result;
	}

	[[nodiscard]] std::expected<void, int> clear() const noexcept
	{
		return settings_.erase("wifi");
	}
	[[nodiscard]] bool provisioned() const noexcept
	{
		return credentials().has_value();
	}

      private:
	Settings<MaximumNameLength> settings_;
};

} /* namespace zest */
