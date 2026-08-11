/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/settings.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace zest
{

/**
 * Persisted Wi-Fi credentials.
 *
 * The record carries an explicit version so a future layout change is detected
 * rather than misread: without it, reordering a field silently reinterprets
 * whatever is already in flash.
 *
 * The pre-shared key is stored in plaintext, because Zephyr's settings backends
 * do not encrypt. On a part with flash protection or a key store, wrap this or
 * keep the key elsewhere.
 */
struct ProvisionedWifi {
	static constexpr std::uint16_t kVersion = 1U;

	std::uint16_t version{kVersion};
	std::uint8_t ssid_length{};
	std::uint8_t password_length{};
	std::array<char, 33> ssid{};
	std::array<char, 65> password{};

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

	Result<> init() const noexcept
	{
		return settings_.init();
	}

	Result<> provision(std::string_view ssid,
					 std::string_view password) const noexcept
	{
		if (ssid.empty() || ssid.size() > 32U || password.size() > 64U) {
			return fail(errors::invalid_argument);
		}
		ProvisionedWifi value{};
		std::copy(ssid.begin(), ssid.end(), value.ssid.begin());
		std::copy(password.begin(), password.end(), value.password.begin());
		value.ssid_length = static_cast<std::uint8_t>(ssid.size());
		value.password_length = static_cast<std::uint8_t>(password.size());
		return settings_.set("wifi", value);
	}

	Result<ProvisionedWifi> credentials() const noexcept
	{
		ZEST_TRY_ASSIGN(record, settings_.template get<ProvisionedWifi>("wifi"));
		if (record.version != ProvisionedWifi::kVersion) {
			return fail(errors::bad_message);
		}
		if (record.ssid_length == 0U || record.ssid_length > 32U ||
		    record.password_length > 64U) {
			return fail(errors::bad_message);
		}
		return record;
	}

	Result<> clear() const noexcept
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
