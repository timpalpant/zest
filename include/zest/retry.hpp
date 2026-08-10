#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>

namespace zest
{

/** Configuration for bounded exponential retry delays. */
struct RetryConfig {
	std::size_t maximum_attempts{5U};
	std::chrono::milliseconds initial_delay{250};
	std::chrono::milliseconds maximum_delay{10'000};
	double multiplier{2.0};
};

/** Deterministic bounded exponential backoff. */
class ExponentialBackoff
{
      public:
	constexpr explicit ExponentialBackoff(RetryConfig config = {}) noexcept
		: config_{normalize(config)}, next_{config_.initial_delay}
	{
	}

	/** Return the current delay and advance the sequence. */
	[[nodiscard]] constexpr std::chrono::milliseconds next_delay() noexcept
	{
		const auto result = next_;
		const auto scaled = static_cast<std::chrono::milliseconds::rep>(
			std::min(static_cast<double>(next_.count()) * config_.multiplier,
				 static_cast<double>(config_.maximum_delay.count())));
		next_ = std::min(std::chrono::milliseconds{scaled}, config_.maximum_delay);
		return result;
	}

	constexpr void reset() noexcept
	{
		next_ = config_.initial_delay;
	}
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
		config.multiplier = std::max(config.multiplier, 1.0);
		return config;
	}

	RetryConfig config_;
	std::chrono::milliseconds next_;
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
	 * Returns no value once maximum_attempts total attempts have been made. A
	 * maximum of zero means unlimited attempts.
	 */
	[[nodiscard]] constexpr std::optional<std::chrono::milliseconds> failure() noexcept
	{
		++attempts_;
		if (maximum_attempts_ != 0U && attempts_ >= maximum_attempts_) {
			return std::nullopt;
		}
		return backoff_.next_delay();
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
