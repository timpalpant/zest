/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/i2s.hpp>

#include <zest/kernel.hpp>

#include <cstring>

namespace zest
{

namespace
{

[[nodiscard]] constexpr i2s_dir native_direction(I2sDirection direction) noexcept
{
	switch (direction) {
	case I2sDirection::receive:
		return I2S_DIR_RX;
	case I2sDirection::transmit:
		return I2S_DIR_TX;
	case I2sDirection::both:
		return I2S_DIR_BOTH;
	}
	return I2S_DIR_RX;
}

/**
 * Zephyr's I2S timeout field is milliseconds, with `SYS_FOREVER_MS` for no
 * bound. A negative wait means "do not wait", which is zero here.
 */
[[nodiscard]] constexpr std::int32_t native_timeout(std::chrono::milliseconds wait) noexcept
{
	if (wait >= std::chrono::milliseconds::max()) {
		return SYS_FOREVER_MS;
	}
	if (wait <= std::chrono::milliseconds::zero()) {
		return 0;
	}
	return static_cast<std::int32_t>(wait.count());
}

} /* namespace */

/* ------------------------------------------------------------------- block --- */

void I2sBlock::release() noexcept
{
	if (slab_ != nullptr && block_ != nullptr) {
		k_mem_slab_free(slab_, block_);
	}
	slab_ = nullptr;
	block_ = nullptr;
	size_ = 0U;
}

/* ------------------------------------------------------------------ stream --- */

Result<> I2sStream::init() const noexcept
{
	if (device_ == nullptr || !device_is_ready(device_)) {
		return fail(errors::no_device);
	}
	return {};
}

Result<> I2sStream::apply(const I2sFormat &format, k_mem_slab &slab,
			  std::size_t block_size) const noexcept
{
	ZEST_TRY(init());
	if (block_size == 0U || format.word_size == 0U || format.frame_clock.count() <= 0) {
		return fail(errors::invalid_argument);
	}

	const i2s_config config{
		.word_size = format.word_size,
		.channels = format.channels,
		.format = format.format,
		.options = format.options,
		.frame_clk_freq = static_cast<std::uint32_t>(format.frame_clock.count()),
		.mem_slab = &slab,
		.block_size = block_size,
		.timeout = native_timeout(format.timeout),
	};
	return check(i2s_configure(device_, native_direction(direction_), &config));
}

Result<> I2sStream::trigger(i2s_trigger_cmd command) const noexcept
{
	if (device_ == nullptr) {
		return fail(errors::no_device);
	}
	return check(i2s_trigger(device_, native_direction(direction_), command));
}

Result<> I2sStream::start() const noexcept
{
	return trigger(I2S_TRIGGER_START);
}

Result<> I2sStream::stop() const noexcept
{
	return trigger(I2S_TRIGGER_STOP);
}

Result<> I2sStream::drain() const noexcept
{
	return trigger(I2S_TRIGGER_DRAIN);
}

Result<> I2sStream::drop() const noexcept
{
	return trigger(I2S_TRIGGER_DROP);
}

Result<> I2sStream::prepare() const noexcept
{
	return trigger(I2S_TRIGGER_PREPARE);
}

Result<i2s_config> I2sStream::applied_config() const noexcept
{
	ZEST_TRY(init());
	const i2s_config *config = i2s_config_get(device_, native_direction(direction_));
	if (config == nullptr) {
		return fail(errors::not_supported);
	}
	return *config;
}

/* ------------------------------------------------------------------- input --- */

Result<> I2sInput::configure(const I2sFormat &format, k_mem_slab &slab,
			     std::size_t block_size) noexcept
{
	ZEST_TRY(apply(format, slab, block_size));
	slab_ = &slab;
	return {};
}

Result<I2sBlock> I2sInput::read() noexcept
{
	if (slab_ == nullptr) {
		return fail(errors::bad_descriptor);
	}
	void *block = nullptr;
	std::size_t size = 0U;
	if (const int rc = i2s_read(device_, &block, &size); rc != 0) {
		/*
		 * -EAGAIN is the queue being empty within the configured timeout,
		 * which for a polled reader is the ordinary case and not a stream
		 * failure. Reporting it as would_block keeps a caller from tearing
		 * the stream down every time it arrives early.
		 */
		return fail(rc);
	}
	if (block == nullptr) {
		return fail(errors::io_error);
	}
	return I2sBlock{slab_, block, size};
}

Result<std::size_t> I2sInput::discard(std::size_t blocks) noexcept
{
	if (slab_ == nullptr) {
		return fail(errors::bad_descriptor);
	}
	std::size_t dropped = 0U;
	while (dropped < blocks) {
		auto block = read();
		if (!block) {
			/* Running out early is the normal end of a settling loop,
			 * not a failure; anything else is the caller's to see. */
			if (block.error() == errors::would_block) {
				break;
			}
			return fail(block.error());
		}
		/* The block frees itself here, which is the whole point. */
		++dropped;
	}
	return dropped;
}

/* ------------------------------------------------------------------ output --- */

Result<> I2sOutput::configure(const I2sFormat &format, k_mem_slab &slab,
			      std::size_t block_size) noexcept
{
	ZEST_TRY(apply(format, slab, block_size));
	slab_ = &slab;
	block_size_ = block_size;
	allocation_timeout_ = format.timeout;
	return {};
}

Result<void *> I2sOutput::allocate() noexcept
{
	if (slab_ == nullptr || block_size_ == 0U) {
		return fail(errors::bad_descriptor);
	}
	void *block = nullptr;
	if (const int rc = k_mem_slab_alloc(slab_, &block, detail::timeout(allocation_timeout_));
	    rc != 0) {
		return fail(rc);
	}
	return block;
}

Result<> I2sOutput::write(std::span<const std::byte> data) noexcept
{
	if (data.size() != block_size_) {
		return fail(errors::invalid_argument);
	}
	auto block = allocate();
	if (!block) {
		return fail(block.error());
	}
	std::memcpy(*block, data.data(), data.size());
	return submit(*block);
}

Result<> I2sOutput::write_silence() noexcept
{
	auto block = allocate();
	if (!block) {
		return fail(block.error());
	}
	std::memset(*block, 0, block_size_);
	return submit(*block);
}

/*
 * i2s_write() transfers ownership of the block only when it succeeds: every
 * failure path returns before the block is queued, leaving it the caller's. So
 * a failed submit has to give it back, or the slab drains one block per failure
 * until the stream stops for want of anywhere to put samples.
 */
Result<> I2sOutput::submit(void *block) noexcept
{
	if (const int rc = i2s_write(device_, block, block_size_); rc != 0) {
		k_mem_slab_free(slab_, block);
		return fail(rc);
	}
	return {};
}

} /* namespace zest */
