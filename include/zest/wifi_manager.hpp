#ifndef ZEST_WIFI_MANAGER_HPP_
#define ZEST_WIFI_MANAGER_HPP_

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

namespace zest
{

class WifiManager
{
      public:
	enum class State : std::uint8_t {
		disconnected,
		connecting,
		connected,
		disconnecting,
	};

	struct Credentials {
		std::string_view ssid;
		std::string_view password;
	};

	struct ConnectionInfo {
		State state{State::disconnected};
		std::array<char, 16> address{};
		std::array<char, 16> netmask{};
		std::array<char, 16> gateway{};
		std::int8_t rssi{};
		std::uint8_t channel{};
	};

	WifiManager() noexcept;
	~WifiManager() noexcept;

	WifiManager(const WifiManager &) = delete;
	WifiManager &operator=(const WifiManager &) = delete;

	[[nodiscard]] std::expected<ConnectionInfo, int>
	connect(const Credentials &credentials,
		std::chrono::milliseconds timeout = std::chrono::seconds{90}) noexcept;

	[[nodiscard]] std::expected<void, int>
	disconnect(std::chrono::milliseconds timeout = std::chrono::seconds{10}) noexcept;
	[[nodiscard]] std::expected<void, int> set_power_save(bool enabled) noexcept;

	[[nodiscard]] ConnectionInfo status() const noexcept;
	[[nodiscard]] State state() const noexcept;
	[[nodiscard]] bool connected() const noexcept
	{
		return state() == State::connected;
	}

      private:
	static void event_handler(struct net_mgmt_event_callback *callback, std::uint64_t event,
				  struct net_if *iface) noexcept;
	void handle_event(std::uint64_t event, struct net_if *iface, const void *info) noexcept;

	struct net_if *iface_{};
	struct net_mgmt_event_callback wifi_callback_{};
	struct net_mgmt_event_callback ipv4_callback_{};
	struct k_sem state_changed_{};
	struct k_mutex mutex_{};
	std::array<std::uint8_t, 33> ssid_{};
	std::array<std::uint8_t, 65> password_{};
	atomic_t state_{ATOMIC_INIT(static_cast<atomic_val_t>(State::disconnected))};
	bool callbacks_registered_{};

	static WifiManager *instance_;
};

[[nodiscard]] const char *to_string(WifiManager::State state) noexcept;

} // namespace zest

#endif
