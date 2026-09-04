/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * I2S capture and playback.
 *
 * Zephyr's I2S API is a small state machine plus a memory slab, and both halves
 * are easy to get subtly wrong. The state machine is driven by `i2s_trigger()`
 * with commands that are only legal in particular states, and a driver that is
 * given an illegal one reports `-EIO` rather than saying which state it was in.
 * The slab is worse: `i2s_read()` hands back a block the caller must return with
 * `k_mem_slab_free()`, and a path that forgets — an error return, an early
 * `continue`, a discarded settling block — starves the RX queue a few blocks
 * later, which presents as the stream simply stopping rather than as a leak.
 *
 * @ref I2sBlock makes the block's ownership a scope, and @ref I2sInput /
 * @ref I2sOutput name the state transitions.
 */

#include <zest/error.hpp>
#include <zest/units.hpp>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace zest
{

/** Which direction of an I2S device an operation applies to. */
enum class I2sDirection : std::uint8_t {
	receive,
	transmit,
	both,
};

/**
 * How a stream is configured.
 *
 * `block_size` and `mem_slab` are deliberately not here: they are properties of
 * the buffer pool, which the caller owns and sizes, and they are passed to
 * @ref I2sInput::configure / @ref I2sOutput::configure alongside this.
 */
struct I2sFormat {
	/** Sampling rate — the frame clock (WS) frequency. */
	Hertz frame_clock{48'000};
	/** Bits per sample. */
	std::uint8_t word_size{16U};
	/** Words per frame. Ignored by the driver when the format is I2S. */
	std::uint8_t channels{2U};
	/** `I2S_FMT_*` flags. */
	i2s_fmt_t format{I2S_FMT_DATA_FORMAT_I2S};
	/** `I2S_OPT_*` flags — clock ownership lives here. */
	i2s_opt_t options{I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER};
	/**
	 * How long a read or write may wait for the queue.
	 *
	 * `duration::max()` waits forever. Zero polls, which is what a real-time
	 * audio thread wants: it would rather substitute silence than block past
	 * its deadline.
	 */
	std::chrono::milliseconds timeout{std::chrono::milliseconds::max()};
};

/**
 * One received block, returned to its slab on destruction.
 *
 * `i2s_read()` transfers ownership of a slab block to the caller and says so
 * only in prose. Holding it in a scope makes every exit path — including the
 * error returns and the discard loops that settle a microphone after a restart
 * — give the block back, which is what keeps the RX queue from running dry
 * several seconds later for no visible reason.
 */
class I2sBlock
{
      public:
	I2sBlock() noexcept = default;

	/** Take ownership of @p block, which must have come from @p slab. */
	I2sBlock(k_mem_slab *slab, void *block, std::size_t size) noexcept
		: slab_{slab}, block_{block}, size_{size}
	{
	}

	I2sBlock(I2sBlock &&other) noexcept
		: slab_{std::exchange(other.slab_, nullptr)},
		  block_{std::exchange(other.block_, nullptr)},
		  size_{std::exchange(other.size_, 0U)}
	{
	}

	I2sBlock &operator=(I2sBlock &&other) noexcept
	{
		if (this != &other) {
			release();
			slab_ = std::exchange(other.slab_, nullptr);
			block_ = std::exchange(other.block_, nullptr);
			size_ = std::exchange(other.size_, 0U);
		}
		return *this;
	}

	I2sBlock(const I2sBlock &) = delete;
	I2sBlock &operator=(const I2sBlock &) = delete;

	~I2sBlock() noexcept
	{
		release();
	}

	/** Return the block to its slab early. Idempotent. */
	void release() noexcept;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return block_ != nullptr;
	}

	[[nodiscard]] std::size_t size_bytes() const noexcept
	{
		return size_;
	}

	[[nodiscard]] std::span<const std::byte> bytes() const noexcept
	{
		return {static_cast<const std::byte *>(block_), block_ != nullptr ? size_ : 0U};
	}

	/**
	 * The block as samples of @p T.
	 *
	 * Empty when the block does not hold a whole number of them, so a
	 * word-size mismatch surfaces as no samples rather than as a misaligned
	 * reinterpretation of the tail.
	 */
	template <typename T>
		requires std::is_trivially_copyable_v<T>
	[[nodiscard]] std::span<const T> samples() const noexcept
	{
		if (block_ == nullptr || size_ % sizeof(T) != 0U) {
			return {};
		}
		return {static_cast<const T *>(block_), size_ / sizeof(T)};
	}

      private:
	k_mem_slab *slab_{};
	void *block_{};
	std::size_t size_{};
};

/**
 * The shared half of an I2S stream: the device and its state transitions.
 *
 * Zephyr's trigger commands are named for what they do to the queue rather than
 * for when they are legal, so the methods here say which state each needs.
 */
class I2sStream
{
      public:
	constexpr I2sStream(const struct device *device, I2sDirection direction) noexcept
		: device_{device}, direction_{direction}
	{
	}

	/** Verify the device is present and ready. */
	[[nodiscard]] Result<> init() const noexcept;

	/**
	 * Begin streaming. Legal in the ready state only.
	 *
	 * For a transmitter the queue should already hold a block or two, because
	 * starting with an empty queue underruns immediately.
	 */
	[[nodiscard]] Result<> start() const noexcept;

	/** Stop at the end of the current block, keeping what is queued. */
	[[nodiscard]] Result<> stop() const noexcept;

	/** Transmit everything queued, then stop. Transmitters only. */
	[[nodiscard]] Result<> drain() const noexcept;

	/**
	 * Stop now and discard the queue.
	 *
	 * Also the way out of the error state: a stream that has underrun or
	 * overrun stays in error until it is dropped, and every operation on it
	 * fails until then.
	 */
	[[nodiscard]] Result<> drop() const noexcept;

	/** Return an error state to ready without discarding anything else. */
	[[nodiscard]] Result<> prepare() const noexcept;

	/** The configuration the driver actually applied, or an error. */
	[[nodiscard]] Result<i2s_config> applied_config() const noexcept;

	[[nodiscard]] constexpr const struct device *device() const noexcept
	{
		return device_;
	}

	[[nodiscard]] constexpr I2sDirection direction() const noexcept
	{
		return direction_;
	}

      protected:
	[[nodiscard]] Result<> apply(const I2sFormat &format, k_mem_slab &slab,
				     std::size_t block_size) const noexcept;

	[[nodiscard]] Result<> trigger(i2s_trigger_cmd command) const noexcept;

	const struct device *device_;
	I2sDirection direction_;
};

/**
 * The receiving half of an I2S device.
 *
 * The slab is the caller's: its block size sets the capture latency and its
 * block count sets how much jitter the consumer may introduce before the driver
 * runs out of somewhere to put samples.
 */
class I2sInput: public I2sStream
{
      public:
	constexpr explicit I2sInput(const struct device *device) noexcept
		: I2sStream{device, I2sDirection::receive}
	{
	}

	/**
	 * Configure the receiver against a caller-owned slab.
	 *
	 * @p block_size must divide the slab's block size; the driver rejects a
	 * larger one.
	 */
	[[nodiscard]] Result<> configure(const I2sFormat &format, k_mem_slab &slab,
					 std::size_t block_size) noexcept;

	/**
	 * Take the next received block.
	 *
	 * Reports `errors::would_block` when the queue is empty and the configured
	 * timeout elapsed — the ordinary case for a poll — so a caller can tell
	 * "nothing yet" from a stream that has actually failed.
	 */
	[[nodiscard]] Result<I2sBlock> read() noexcept;

	/**
	 * Take and discard up to @p blocks, stopping early if the queue empties.
	 *
	 * A microphone needs a moment after a start or a restart before its output
	 * means anything, and the settling blocks still have to be returned to the
	 * slab. Returns how many were dropped.
	 */
	[[nodiscard]] Result<std::size_t> discard(std::size_t blocks) noexcept;

      private:
	k_mem_slab *slab_{};
};

/**
 * The transmitting half of an I2S device.
 *
 * `i2s_write()` takes ownership of the block it is given — including when it
 * fails — so this allocates from the slab, hands the caller a span to fill, and
 * only then submits it. That is the shape that cannot double-free.
 */
class I2sOutput: public I2sStream
{
      public:
	constexpr explicit I2sOutput(const struct device *device) noexcept
		: I2sStream{device, I2sDirection::transmit}
	{
	}

	[[nodiscard]] Result<> configure(const I2sFormat &format, k_mem_slab &slab,
					 std::size_t block_size) noexcept;

	/**
	 * Copy @p data into a fresh block and queue it.
	 *
	 * @p data must be exactly the configured block size: the driver transmits
	 * whole blocks, so a short one would send whatever the slab happened to
	 * hold in the remainder.
	 */
	[[nodiscard]] Result<> write(std::span<const std::byte> data) noexcept;

	/** As @ref write, for a span of samples. */
	template <typename T>
		requires std::is_trivially_copyable_v<T>
	[[nodiscard]] Result<> write_samples(std::span<const T> samples) noexcept
	{
		return write(std::as_bytes(samples));
	}

	/**
	 * Queue a block of silence.
	 *
	 * Cheaper than zeroing a caller-side buffer first, and the thing a
	 * real-time path wants when its producer missed a deadline: an underrun
	 * puts the stream in the error state, from which only @ref drop recovers.
	 */
	[[nodiscard]] Result<> write_silence() noexcept;

	/** The configured block size in bytes, or zero before @ref configure. */
	[[nodiscard]] std::size_t block_size() const noexcept
	{
		return block_size_;
	}

      private:
	/** Allocate a block, waiting up to the configured timeout. */
	[[nodiscard]] Result<void *> allocate() noexcept;

	k_mem_slab *slab_{};
	std::size_t block_size_{};
	/* Kept as a duration rather than a k_timeout_t so this header does not
	 * have to pull in the whole kernel API for one field. */
	std::chrono::milliseconds allocation_timeout_{std::chrono::milliseconds::max()};
};

} /* namespace zest */
