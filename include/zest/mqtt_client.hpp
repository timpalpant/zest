/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/function.hpp>
#include <zest/network.hpp>

#include <zephyr/net/mqtt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zest
{

struct MqttConnectionOptions {
	std::string_view client_id;
	std::string_view username{};
	std::string_view password{};
	std::chrono::seconds keepalive{60};
	bool clean_session{true};
};

/**
 * Fixed-buffer MQTT client driven from the application's event loop.
 *
 * The loop gets what it needs directly: the pollable descriptor, the time until
 * the next keepalive is due, and a way to service both.
 *
 * ```cpp
 * zest::Poller<1> poller;
 * (void)poller.add(client.poll_fd(), zest::PollEvent::readable);
 * for (;;) {
 *         auto ready = poller.wait(client.keepalive_time_left());
 *         if (ready && *ready > 0) {
 *                 ZEST_TRY(client.input());
 *         }
 *         ZEST_TRY(client.keep_alive());
 * }
 * ```
 */
template <std::size_t ReceiveBufferSize = 1024U, std::size_t TransmitBufferSize = 1024U,
	  std::size_t MaximumCredentialLength = 64U, std::size_t MaximumSecurityTags = 4U>
class MqttClient
{
      public:
	/** Receives every MQTT event. Accepts a capturing lambda. */
	using EventHandler = InplaceFunction<void(const mqtt_evt &) noexcept, 4 * sizeof(void *)>;

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

	template <typename F>
	[[nodiscard]] Result<> configure(const ResolvedAddress &broker,
					 const MqttConnectionOptions &options, F &&handler) noexcept
	{
		handler_ = std::forward<F>(handler);
		return configure_internal(broker, options);
	}

	[[nodiscard]] Result<> configure(const ResolvedAddress &broker,
					 const MqttConnectionOptions &options) noexcept
	{
		handler_.reset();
		return configure_internal(broker, options);
	}

#if defined(CONFIG_MQTT_LIB_TLS)
	[[nodiscard]] Result<> use_tls(std::span<const sec_tag_t> tags, std::string_view hostname,
				       int peer_verify = TLS_PEER_VERIFY_REQUIRED) noexcept
	{
		if (!configured_) {
			return fail(errors::permission_denied);
		}
		if (tags.empty() || tags.size() > security_tags_.size() || hostname.empty() ||
		    hostname.size() >= hostname_.size()) {
			return fail(errors::invalid_argument);
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

	[[nodiscard]] Result<> connect() noexcept
	{
		if (!configured_) {
			return fail(errors::permission_denied);
		}
		return check(mqtt_connect(&client_));
	}

	/**
	 * Publish a payload.
	 *
	 * A @p message_id of 0 draws the next value from an internal counter, so
	 * QoS 1 and 2 acknowledgements stay correlatable.
	 */
	[[nodiscard]] Result<> publish(std::string_view topic, std::span<const std::byte> payload,
				       mqtt_qos qos = MQTT_QOS_0_AT_MOST_ONCE, bool retain = false,
				       std::uint16_t message_id = 0U) noexcept
	{
		if (!configured_) {
			return fail(errors::permission_denied);
		}
		if (topic.empty()) {
			return fail(errors::invalid_argument);
		}
		mqtt_publish_param request{};
		request.message.topic.topic = view(topic);
		request.message.topic.qos = qos;
		request.message.payload.data =
			reinterpret_cast<std::uint8_t *>(const_cast<std::byte *>(payload.data()));
		request.message.payload.len = payload.size();
		request.message_id = message_id != 0U ? message_id : next_message_id();
		request.dup_flag = 0U;
		request.retain_flag = retain ? 1U : 0U;
		return check(mqtt_publish(&client_, &request));
	}

	/** Publish text without the caller reinterpreting it as bytes. */
	[[nodiscard]] Result<> publish(std::string_view topic, std::string_view payload,
				       mqtt_qos qos = MQTT_QOS_0_AT_MOST_ONCE,
				       bool retain = false) noexcept
	{
		return publish(topic, std::as_bytes(std::span{payload.data(), payload.size()}), qos,
			       retain);
	}

	[[nodiscard]] Result<> subscribe(std::string_view topic,
					 mqtt_qos qos = MQTT_QOS_0_AT_MOST_ONCE,
					 std::uint16_t message_id = 0U) noexcept
	{
		if (topic.empty()) {
			return fail(errors::invalid_argument);
		}
		mqtt_topic subscription{.topic = view(topic), .qos = qos};
		mqtt_subscription_list request{
			.list = &subscription,
			.list_count = 1U,
			.message_id = message_id != 0U ? message_id : next_message_id(),
		};
		return check(mqtt_subscribe(&client_, &request));
	}

	[[nodiscard]] Result<> unsubscribe(std::string_view topic,
					   std::uint16_t message_id = 0U) noexcept
	{
		if (topic.empty()) {
			return fail(errors::invalid_argument);
		}
		mqtt_topic subscription{.topic = view(topic), .qos = MQTT_QOS_0_AT_MOST_ONCE};
		mqtt_subscription_list request{
			.list = &subscription,
			.list_count = 1U,
			.message_id = message_id != 0U ? message_id : next_message_id(),
		};
		return check(mqtt_unsubscribe(&client_, &request));
	}

	/** Read and dispatch whatever the broker has sent. */
	[[nodiscard]] Result<> input() noexcept
	{
		return check(mqtt_input(&client_));
	}

	/** Send a ping if the keepalive interval is due. */
	[[nodiscard]] Result<> keep_alive() noexcept
	{
		const int rc = mqtt_live(&client_);
		if (rc == 0 || rc == -EAGAIN) {
			return {};
		}
		return fail(rc);
	}

	/** Retained name for `keep_alive()`. */
	[[nodiscard]] Result<> live() noexcept
	{
		return keep_alive();
	}

	/** Force a ping regardless of the keepalive schedule. */
	[[nodiscard]] Result<> ping() noexcept
	{
		return check(mqtt_ping(&client_));
	}

	[[nodiscard]] Result<> disconnect() noexcept
	{
		if (!configured_) {
			return {};
		}
		const int rc = mqtt_disconnect(&client_, nullptr);
		if (rc == 0 || rc == -ENOTCONN) {
			return {};
		}
		return fail(rc);
	}

	/**
	 * The descriptor to poll for readability.
	 *
	 * Returns -1 before the transport is connected. Without this the caller has
	 * to reach through `native_handle()` into the transport union and know which
	 * arm is live.
	 */
	[[nodiscard]] int poll_fd() const noexcept
	{
		switch (client_.transport.type) {
		case MQTT_TRANSPORT_NON_SECURE:
			return client_.transport.tcp.sock;
#if defined(CONFIG_MQTT_LIB_TLS)
		case MQTT_TRANSPORT_SECURE:
			return client_.transport.tls.sock;
#endif
		default:
			break;
		}
		return -1;
	}

	/**
	 * Time until the next keepalive is due.
	 *
	 * Pass this straight to a poller as its timeout, so the loop wakes exactly
	 * when it must ping and not before.
	 */
	[[nodiscard]] std::chrono::milliseconds keepalive_time_left() const noexcept
	{
		return std::chrono::milliseconds{
			mqtt_keepalive_time_left(const_cast<mqtt_client *>(&client_))};
	}

	/** Next message id from the internal counter. Never returns zero. */
	[[nodiscard]] std::uint16_t next_message_id() noexcept
	{
		if (++message_id_ == 0U) {
			message_id_ = 1U;
		}
		return message_id_;
	}

	[[nodiscard]] bool configured() const noexcept
	{
		return configured_;
	}
	[[nodiscard]] mqtt_client *native_handle() noexcept
	{
		return &client_;
	}

      private:
	[[nodiscard]] Result<> configure_internal(const ResolvedAddress &broker,
						  const MqttConnectionOptions &options) noexcept
	{
		if (options.client_id.empty() ||
		    options.client_id.size() > client_id_.size() - 1U ||
		    options.username.size() > username_.size() - 1U ||
		    options.password.size() > password_.size() - 1U ||
		    (!options.password.empty() && options.username.empty())) {
			return fail(errors::invalid_argument);
		}
		if (options.keepalive < std::chrono::seconds::zero()) {
			return fail(errors::invalid_argument);
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
		client_.keepalive = static_cast<std::uint16_t>(options.keepalive.count());
		client_.clean_session = options.clean_session ? 1U : 0U;
		client_.transport.type = MQTT_TRANSPORT_NON_SECURE;
		client_.evt_cb = event_callback;
		client_.user_data = this;
		configured_ = true;
		return {};
	}

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
		if (self != nullptr && event != nullptr && self->handler_) {
			self->handler_(*event);
		}
	}

	mqtt_client client_{};
	sockaddr_storage broker_{};
	std::array<std::uint8_t, ReceiveBufferSize> receive_buffer_{};
	std::array<std::uint8_t, TransmitBufferSize> transmit_buffer_{};
	std::array<std::uint8_t, MaximumCredentialLength + 1U> client_id_{};
	std::array<std::uint8_t, MaximumCredentialLength + 1U> username_{};
	std::array<std::uint8_t, MaximumCredentialLength + 1U> password_{};
	mqtt_utf8 username_value_{};
	mqtt_utf8 password_value_{};
#if defined(CONFIG_MQTT_LIB_TLS)
	std::array<sec_tag_t, MaximumSecurityTags> security_tags_{};
	std::array<std::uint8_t, 254> hostname_{};
#endif
	EventHandler handler_{};
	std::uint16_t message_id_{0U};
	bool configured_{};
};

} /* namespace zest */
