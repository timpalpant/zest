#pragma once

#include <zest/adc_channel.hpp>

#include <cerrno>
#include <cstdint>
#include <expected>

namespace zest
{

/** Reconstructs an input voltage measured through a resistive divider. */
class VoltageDivider
{
      public:
	constexpr VoltageDivider(adc_dt_spec channel, std::int32_t output_ohms,
				 std::int32_t full_ohms) noexcept
		: channel_{channel}, output_ohms_{output_ohms}, full_ohms_{full_ohms}
	{
	}

	[[nodiscard]] std::expected<void, int> init() const noexcept
	{
		if (output_ohms_ <= 0 || full_ohms_ < output_ohms_) {
			return std::unexpected(-EINVAL);
		}
		return channel_.init();
	}

	template <std::size_t Samples = 1U>
	[[nodiscard]] std::expected<std::int32_t, int> read_millivolts() const noexcept
	{
		const auto output = channel_.read_average_millivolts<Samples>();
		if (!output) {
			return std::unexpected(output.error());
		}
		return static_cast<std::int32_t>((static_cast<std::int64_t>(*output) * full_ohms_) /
						 output_ohms_);
	}

	[[nodiscard]] constexpr const AdcChannel &channel() const noexcept
	{
		return channel_;
	}

      private:
	AdcChannel channel_;
	std::int32_t output_ohms_;
	std::int32_t full_ohms_;
};

} /* namespace zest */
