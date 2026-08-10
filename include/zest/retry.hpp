/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace zest
{

/**
 * Configuration for bounded exponential retry delays.
 *
 * The growth factor is a percentage rather than a `double`, so the whole backoff
 * sequence is computed in integer arithmetic. `multiplier_percent = 200` doubles
 * the delay each attempt, `150` grows it by half.
 */
struct RetryConfig {
	/** Total attempts permitted, or zero for unlimited. */
	std::size_t maximum_attempts{5U};
	std::chrono::milliseconds initial_delay{250};
	std::chrono::milliseconds maximum_delay{10'000};
	/** Growth per attempt, in percent. Values below 100 are raised to 100. */
	std::uint16_t multiplier_percent{200U};
	/**
	 * Random spread applied to each delay, in percent of that delay.
	 *
	 * Zero keeps the sequence exactly deterministic. A non-zero value
	 * subtracts up to `jitter_percent` of the delay, which stops a fleet of
	 * devices that failed together from retrying in lockstep and hammering a
	 * recovering server. Randomness comes from a seeded, self-contained
	 * generator, so a given seed still replays identically.
	 */
	std::uint16_t jitter_percent{0U};
	/** Seed for the jitter generator. Ignored when `jitter_percent` is zero. */
	std::uint32_t jitter_seed{0x9e3779b9U};
};

/** Deterministic bounded exponential backoff using integer arithmetic. */
class ExponentialBackoff
{
      public:
	constexpr explicit ExponentialBackoff(RetryConfig config = {}) noexcept
		: config_{normalize(config)}, next_{config_.initial_delay},
		  state_{config_.jitter_seed | 1U}
	{
	}

	/** Return the delay to wait now, and advance the sequence. */
	[[nodiscard]] constexpr std::chrono::milliseconds next_delay() noexcept
	{
		const auto result = spread(next_);

		const auto grown = static_cast<std::int64_t>(next_.count()) *
				   static_cast<std::int64_t>(config_.multiplier_percent) / 100;
		next_ = std::min(std::chrono::milliseconds{grown}, config_.maximum_delay);
		if (next_ < config_.initial_delay) {
			/* Guard against a zero initial delay never growing. */
			next_ = config_.initial_delay;
		}
		return result;
	}

	constexpr void reset() noexcept
	{
		next_ = config_.initial_delay;
		state_ = config_.jitter_seed | 1U;
	}

	/** The delay the next call to `next_delay()` will be based on, before jitter. */
	[[nodiscard]] constexpr std::chrono::milliseconds current_delay() const noexcept
	{
		return next_;
	}

      private:
	[[nodiscard]] static constexpr RetryConfig normalize(RetryConfig config) noexcept
	{
		config.initial_delay =
			std::max(config.initial_delay, std::chrono::milliseconds::zero());
		config.maximum_delay = std::max(config.maximum_delay, config.initial_delay);
		config.multiplier_percent = std::max<std::uint16_t>(config.multiplier_percent, 100U);
		config.jitter_percent = std::min<std::uint16_t>(config.jitter_percent, 100U);
		return config;
	}

	/** Subtract up to `jitter_percent` of @p delay, using an xorshift generator. */
	[[nodiscard]] constexpr std::chrono::milliseconds
	spread(std::chrono::milliseconds delay) noexcept
	{
		if (config_.jitter_percent == 0U || delay.count() == 0) {
			return delay;
		}
		state_ ^= state_ << 13U;
		state_ ^= state_ >> 17U;
		state_ ^= state_ << 5U;

		const auto window = static_cast<std::int64_t>(delay.count()) *
				    static_cast<std::int64_t>(config_.jitter_percent) / 100;
		if (window <= 0) {
			return delay;
		}
		const auto reduction = static_cast<std::int64_t>(state_ % static_cast<std::uint32_t>(
			window + 1));
		return std::chrono::milliseconds{delay.count() - reduction};
	}

	RetryConfig config_;
	std::chrono::milliseconds next_;
	std::uint32_t state_;
};

/** Track retry attempts and produce delays until a configured limit is reached. */
class RetryPolicy
{
      public:
	constexpr explicit RetryPolicy(RetryConfig config = {}) noexcept
		: maximum_attempts_{config.maximum_attempts}, backoff_{config}
	{
	}

	/**
	 * Record a failed attempt and return the delay before retrying.
	 *
	 * Returns no value once `maximum_attempts` total attempts have been made,
	 * which is the caller's signal to give up. A maximum of zero means
	 * unlimited attempts, in which case this never returns `nullopt`.
	 */
	[[nodiscard]] constexpr std::optional<std::chrono::milliseconds> failure() noexcept
	{
		++attempts_;
		if (maximum_attempts_ != 0U && attempts_ >= maximum_attempts_) {
			return std::nullopt;
		}
		return backoff_.next_delay();
	}

	/** Whether another attempt is permitted. */
	[[nodiscard]] constexpr bool exhausted() const noexcept
	{
		return maximum_attempts_ != 0U && attempts_ >= maximum_attempts_;
	}

	constexpr void reset() noexcept
	{
		attempts_ = 0U;
		backoff_.reset();
	}

	[[nodiscard]] constexpr std::size_t attempts() const noexcept
	{
		return attempts_;
	}

      private:
	std::size_t maximum_attempts_;
	std::size_t attempts_{0U};
	ExponentialBackoff backoff_;
};

} /* namespace zest */
