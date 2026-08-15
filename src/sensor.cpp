/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/sensor.hpp>

#include <zephyr/drivers/sensor.h>
#include <zephyr/rtio/rtio.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace zest
{

Result<SensorFrame> SensorReader::read(std::span<std::uint8_t> encoded_buffer) const noexcept
{
	if (encoded_buffer.empty()) {
		return fail(-EINVAL);
	}
	const int rc = sensor_read(iodev_, context_, encoded_buffer.data(), encoded_buffer.size());
	if (rc < 0) {
		return fail(rc);
	}
	return SensorFrame{*decoder_, encoded_buffer};
}

Result<> SensorBatch::configure(const device &sensor,
				std::span<const sensor_chan_spec> channels) const noexcept
{
	if (channels.empty()) {
		return fail(-EINVAL);
	}
	const int rc =
		sensor_reconfigure_read_iodev(iodev_, &sensor, channels.data(), channels.size());
	if (rc < 0) {
		return fail(rc);
	}
	return {};
}

AsyncSensorFrame::AsyncSensorFrame(AsyncSensorFrame &&other) noexcept
	: context_{std::exchange(other.context_, nullptr)}, decoder_{other.decoder_},
	  buffer_{std::exchange(other.buffer_, nullptr)}, length_{std::exchange(other.length_, 0U)},
	  userdata_{std::exchange(other.userdata_, nullptr)}
{
}

AsyncSensorFrame &AsyncSensorFrame::operator=(AsyncSensorFrame &&other) noexcept
{
	if (this != &other) {
		release();
		context_ = std::exchange(other.context_, nullptr);
		decoder_ = other.decoder_;
		buffer_ = std::exchange(other.buffer_, nullptr);
		length_ = std::exchange(other.length_, 0U);
		userdata_ = std::exchange(other.userdata_, nullptr);
	}
	return *this;
}

AsyncSensorFrame::~AsyncSensorFrame() noexcept
{
	release();
}

void AsyncSensorFrame::release() noexcept
{
	if (context_ != nullptr && buffer_ != nullptr) {
		rtio_release_buffer(context_, buffer_, length_);
	}
	context_ = nullptr;
	buffer_ = nullptr;
	length_ = 0U;
}

Result<> AsyncSensorReader::submit(void *userdata) const noexcept
{
	const int rc = sensor_read_async_mempool(iodev_, context_, userdata);
	if (rc < 0) {
		return fail(rc);
	}
	return {};
}

Result<AsyncSensorFrame> AsyncSensorReader::consume(rtio_cqe &completion) const noexcept
{
	const int result = completion.result;
	void *userdata = completion.userdata;
	std::uint8_t *buffer = nullptr;
	std::uint32_t length = 0U;

	const int buffer_result =
		rtio_cqe_get_mempool_buffer(context_, &completion, &buffer, &length);

	rtio_cqe_release(context_, &completion);
	if (result < 0) {
		if (buffer_result == 0 && buffer != nullptr) {
			rtio_release_buffer(context_, buffer, length);
		}
		return fail(result);
	}
	if (buffer_result < 0) {
		return fail(buffer_result);
	}
	return AsyncSensorFrame{*context_, *decoder_, buffer, length, userdata};
}

Result<AsyncSensorFrame> AsyncSensorReader::try_receive() const noexcept
{
	rtio_cqe *completion = rtio_cqe_consume(context_);
	if (completion == nullptr) {
		return fail(errors::would_block);
	}
	return consume(*completion);
}

Result<AsyncSensorFrame> AsyncSensorReader::receive() const noexcept
{
	rtio_cqe *completion = rtio_cqe_consume_block(context_);
	return consume(*completion);
}

} /* namespace zest */
