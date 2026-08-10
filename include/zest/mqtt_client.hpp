/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/network.hpp>

#include <zephyr/net/mqtt.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace zest
{

struct MqttConnectionOptions {
	std::string_view client_id;
	std::string_view username{};
	std::string_view password{};
	std::uint16_t keepalive_seconds{60U};
	bool clean_session{true};
};

/** Fixed-buffer MQTT client. Call input() from the application's poll loop. */
template <std::size_t ReceiveBufferSize = 1024U, std::size_t TransmitBufferSize = 1024U>
class MqttClient
{
      public:
	using EventHandler = void (*)(const mqtt_evt &, void *) noexcept;

	MqttClient() noexcept
	{
		mqtt_client_init(&client_);
	}
	~MqttClient() noexcept
	{
		(void)disconnect();
	}
	MqttClient(const MqttClient &) = delete;
	MqttClient &operator=(const MqttClient &) = delete;
	MqttClient(MqttClient &&) = delete;
	MqttClient &operator=(MqttClient &&) = delete;

	[[nodiscard]] std::expected<void, int> configure(const ResolvedAddress &broker,
							 const MqttConnectionOptions &options,
							 EventHandler handler = nullptr,
							 void *context = nullptr) noexcept
	{
		if (options.client_id.empty() || options.client_id.size() > client_id_.size() ||
		    options.username.size() > username_.size() ||
		    options.password.size() > password_.size() ||
		    (!options.password.empty() && options.username.empty())) {
			return std::unexpected(-EINVAL);
		}
		mqtt_client_init(&client_);
		broker_ = broker.storage;
		copy(options.client_id, client_id_);
		copy(options.username, username_);
		copy(options.password, password_);
		client_.broker = &broker_;
		client_.client_id = utf8(client_id_, options.client_id.size());
		if (!options.username.empty()) {
			username_value_ = utf8(username_, options.username.size());
			client_.user_name = &username_value_;
		}
		if (!options.password.empty()) {
			password_value_ = utf8(password_, options.password.size());
			client_.password = &password_value_;
		}
		client_.rx_buf = receive_buffer_.data();
		client_.rx_buf_size = receive_buffer_.size();
		client_.tx_buf = transmit_buffer_.data();
		client_.tx_buf_size = transmit_buffer_.size();
		client_.keepalive = options.keepalive_seconds;
		client_.clean_session = options.clean_session ? 1U : 0U;
		client_.transport.type = MQTT_TRANSPORT_NON_SECURE;
		client_.evt_cb = event_callback;
		client_.user_data = this;
		handler_ = handler;
		context_ = context;
		configured_ = true;
		return {};
	}

#if defined(CONFIG_MQTT_LIB_TLS)
	[[nodiscard]] std::expected<void, int>
	use_tls(std::span<const sec_tag_t> tags, std::string_view hostname,
		int peer_verify = TLS_PEER_VERIFY_REQUIRED) noexcept
	{
		if (!configured_ || tags.empty() || tags.size() > security_tags_.size() ||
		    hostname.empty() || hostname.size() >= hostname_.size()) {
			return std::unexpected(-EINVAL);
		}
		std::copy(tags.begin(), tags.end(), security_tags_.begin());
		copy(hostname, hostname_);
		client_.transport.type = MQTT_TRANSPORT_SECURE;
		client_.transport.tls.config.peer_verify = peer_verify;
		client_.transport.tls.config.sec_tag_list = security_tags_.data();
		client_.transport.tls.config.sec_tag_count = tags.size();
		client_.transport.tls.config.hostname =
			reinterpret_cast<const char *>(hostname_.data());
		return {};
	}
#endif

	[[nodiscard]] std::expected<void, int> connect() noexcept
	{
		if (!configured_) {
			return std::unexpected(-EACCES);
		}
		const int rc = mqtt_connect(&client_);
		return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
	}

	[[nodiscard]] std::expected<void, int> publish(std::string_view topic,
						       std::span<const std::byte> payload,
						       std::uint16_t message_id = 1U,
						       mqtt_qos qos = MQTT_QOS_0_AT_MOST_ONCE,
						       bool retain = false) noexcept
	{
		if (topic.empty()) {
			return std::unexpected(-EINVAL);
		}
		mqtt_publish_param request{};
		request.message.topic.topic = view(topic);
		request.message.topic.qos = qos;
		request.message.payload.data =
			reinterpret_cast<std::uint8_t *>(const_cast<std::byte *>(payload.data()));
		request.message.payload.len = payload.size();
		request.message_id = message_id;
		request.retain_flag = retain ? 1U : 0U;
		const int rc = mqtt_publish(&client_, &request);
		return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
	}

	[[nodiscard]] std::expected<void, int>
	subscribe(std::string_view topic, std::uint16_t message_id = 1U,
		  mqtt_qos qos = MQTT_QOS_0_AT_MOST_ONCE) noexcept
	{
		if (topic.empty()) {
			return std::unexpected(-EINVAL);
		}
		mqtt_topic subscription{.topic = view(topic), .qos = qos};
		mqtt_subscription_list request{
			.list = &subscription,
			.list_count = 1U,
			.message_id = message_id,
		};
		const int rc = mqtt_subscribe(&client_, &request);
		return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
	}

	[[nodiscard]] std::expected<void, int> input() noexcept
	{
		const int rc = mqtt_input(&client_);
		return rc == 0 ? std::expected<void, int>{} : std::unexpected(rc);
	}
	[[nodiscard]] std::expected<void, int> live() noexcept
	{
		const int rc = mqtt_live(&client_);
		return rc == 0 || rc == -EAGAIN ? std::expected<void, int>{} : std::unexpected(rc);
	}
	[[nodiscard]] std::expected<void, int> disconnect() noexcept
	{
		if (!configured_) {
			return {};
		}
		const int rc = mqtt_disconnect(&client_, nullptr);
		return rc == 0 || rc == -ENOTCONN ? std::expected<void, int>{}
						  : std::unexpected(rc);
	}
	[[nodiscard]] mqtt_client *native_handle() noexcept
	{
		return &client_;
	}

      private:
	template <std::size_t N>
	static void copy(std::string_view source, std::array<std::uint8_t, N> &destination) noexcept
	{
		std::fill(destination.begin(), destination.end(), 0U);
		std::copy(source.begin(), source.end(), destination.begin());
	}
	template <std::size_t N>
	static mqtt_utf8 utf8(std::array<std::uint8_t, N> &value, std::size_t size) noexcept
	{
		return {.utf8 = value.data(), .size = static_cast<std::uint32_t>(size)};
	}
	static mqtt_utf8 view(std::string_view value) noexcept
	{
		return {.utf8 = reinterpret_cast<const std::uint8_t *>(value.data()),
			.size = static_cast<std::uint32_t>(value.size())};
	}
	static void event_callback(mqtt_client *client, const mqtt_evt *event) noexcept
	{
		auto *self = static_cast<MqttClient *>(client->user_data);
		if (self != nullptr && self->handler_ != nullptr && event != nullptr) {
			self->handler_(*event, self->context_);
		}
	}

	mqtt_client client_{};
	sockaddr_storage broker_{};
	std::array<std::uint8_t, ReceiveBufferSize> receive_buffer_{};
	std::array<std::uint8_t, TransmitBufferSize> transmit_buffer_{};
	std::array<std::uint8_t, 64> client_id_{};
	std::array<std::uint8_t, 64> username_{};
	std::array<std::uint8_t, 64> password_{};
	mqtt_utf8 username_value_{};
	mqtt_utf8 password_value_{};
#if defined(CONFIG_MQTT_LIB_TLS)
	std::array<sec_tag_t, 4> security_tags_{};
	std::array<std::uint8_t, 254> hostname_{};
#endif
	EventHandler handler_{};
	void *context_{};
	bool configured_{};
};

} /* namespace zest */
