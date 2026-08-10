/*
 * Battery state-of-charge for the Adafruit HUZZAH32 / Feather ESP32.
 *
 * The board permanently ties the LiPo connector to A13 through a 100k/100k
 * divider, so the ADC always sees exactly half the pack voltage.
 */

#include <zest/battery_monitor.hpp>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>

#include <array>
#include <cstdint>
#include <expected>

namespace zest
{
namespace
{

[[nodiscard]] constexpr bool curve_is_valid(std::span<const CurvePoint> curve) noexcept
{
	if (curve.size() < 2U || curve.front().percent > 100U) {
		return false;
	}
	for (std::size_t i = 1; i < curve.size(); ++i) {
		const CurvePoint &previous = curve[i - 1];
		const CurvePoint &current = curve[i];
		if (previous.millivolts <= current.millivolts ||
		    previous.percent < current.percent || current.percent > 100U) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] constexpr std::uint8_t estimate_percent(std::int32_t millivolts,
						      std::span<const CurvePoint> curve) noexcept
{
	if (millivolts >= curve.front().millivolts) {
		return curve.front().percent;
	}

	for (std::size_t i = 1; i < curve.size(); ++i) {
		const CurvePoint &hi = curve[i - 1];
		const CurvePoint &lo = curve[i];

		if (millivolts >= lo.millivolts) {
			const std::int64_t rise = hi.percent - lo.percent;
			const std::int64_t run = hi.millivolts - lo.millivolts;
			const std::int64_t offset = millivolts - lo.millivolts;

			return static_cast<std::uint8_t>(lo.percent + offset * rise / run);
		}
	}

	return curve.back().percent;
}

constexpr std::array test_curve{
	CurvePoint{4200, 100},
	CurvePoint{3700, 10},
	CurvePoint{3300, 0},
};
static_assert(curve_is_valid(test_curve));
static_assert(estimate_percent(4300, test_curve) == 100);
static_assert(estimate_percent(3950, test_curve) == 55);
static_assert(estimate_percent(3000, test_curve) == 0);

} // namespace

std::expected<void, Error> BatteryMonitor::init() const noexcept
{
	if (output_ohms_ <= 0 || full_ohms_ < output_ohms_) {
		return std::unexpected(-EINVAL);
	}
	if (!curve_is_valid(discharge_curve_)) {
		return std::unexpected(-EINVAL);
	}
	if (!adc_is_ready_dt(&channel_)) {
		return std::unexpected(-ENODEV);
	}

	if (const int rc = adc_channel_setup_dt(&channel_); rc < 0) {
		return std::unexpected(rc);
	}

	return {};
}

std::expected<std::int32_t, Error> BatteryMonitor::sample_mv() const noexcept
{
	std::uint16_t raw;

	adc_sequence seq{};
	seq.buffer = &raw;
	seq.buffer_size = sizeof(raw);

	if (const int rc = adc_sequence_init_dt(&channel_, &seq); rc < 0) {
		return std::unexpected(rc);
	}

	if (const int rc = adc_read_dt(&channel_, &seq); rc < 0) {
		return std::unexpected(rc);
	}

	std::int32_t val_mv = raw;
	if (const int rc = adc_raw_to_millivolts_dt(&channel_, &val_mv); rc < 0) {
		return std::unexpected(rc);
	}

	/* Undo the divider to recover the pack voltage. */
	return static_cast<std::int32_t>((static_cast<std::int64_t>(val_mv) * full_ohms_) /
					 output_ohms_);
}

std::expected<Reading, Error> BatteryMonitor::read() const noexcept
{
	if (!curve_is_valid(discharge_curve_)) {
		return std::unexpected(-EINVAL);
	}

	std::int32_t total = 0;

	for (int i = 0; i < kOversample; ++i) {
		const auto mv = sample_mv();

		if (!mv) {
			return std::unexpected(mv.error());
		}

		total += *mv;
	}

	const std::int32_t millivolts = total / kOversample;

	return Reading{millivolts, estimate_percent(millivolts, discharge_curve_)};
}

} /* namespace zest */
