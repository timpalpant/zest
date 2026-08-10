#include <zest/adc_channel.hpp>

#include <zephyr/drivers/adc.h>

#include <cerrno>
#include <cstdint>
#include <expected>

namespace zest
{

std::expected<void, int> AdcChannel::init() const noexcept
{
	if (!adc_is_ready_dt(&spec_)) {
		return std::unexpected(-ENODEV);
	}

	if (const int rc = adc_channel_setup_dt(&spec_); rc < 0) {
		return std::unexpected(rc);
	}

	return {};
}

std::expected<std::int32_t, int> AdcChannel::read_raw() const noexcept
{
	std::int16_t raw = 0;
	adc_sequence sequence{};
	sequence.buffer = &raw;
	sequence.buffer_size = sizeof(raw);

	if (const int rc = adc_sequence_init_dt(&spec_, &sequence); rc < 0) {
		return std::unexpected(rc);
	}
	if (const int rc = adc_read_dt(&spec_, &sequence); rc < 0) {
		return std::unexpected(rc);
	}

	return raw;
}

std::expected<std::int32_t, int> AdcChannel::read_millivolts() const noexcept
{
	auto raw = read_raw();
	if (!raw) {
		return std::unexpected(raw.error());
	}

	std::int32_t millivolts = *raw;
	if (const int rc = adc_raw_to_millivolts_dt(&spec_, &millivolts); rc < 0) {
		return std::unexpected(rc);
	}

	return millivolts;
}

} /* namespace zest */
