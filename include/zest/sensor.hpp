#pragma once

#include <zephyr/drivers/sensor.h>
#include <zephyr/rtio/rtio.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace zest
{

/** A decoded value tagged with the sampling schedule's timestamp. */
template <typename T> struct Sample {
	T value;
	std::chrono::nanoseconds timestamp;
};

/** A non-owning decoder view over caller-owned encoded sensor bytes. */
class SensorFrame
{
      public:
	constexpr SensorFrame(const sensor_decoder_api &decoder,
			      std::span<const std::uint8_t> encoded) noexcept
		: decoder_{&decoder}, encoded_{encoded}
	{
	}

	/** Decode one frame for a channel into its Zephyr sensor data type. */
	template <typename Data>
	[[nodiscard]] std::expected<Data, int> decode(sensor_chan_spec channel) const noexcept
	{
		Data output{};
		std::uint32_t fit = 0U;
		const int count = decoder_->decode(encoded_.data(), channel, &fit, 1U, &output);
		if (count < 0) {
			return std::unexpected(count);
		}
		if (count == 0) {
			return std::unexpected(-ENODATA);
		}
		return output;
	}

	[[nodiscard]] constexpr std::span<const std::uint8_t> encoded() const noexcept
	{
		return encoded_;
	}

      private:
	const sensor_decoder_api *decoder_;
	std::span<const std::uint8_t> encoded_;
};

/** Blocking Read-and-Decode sensor access using a caller-provided buffer. */
class SensorReader
{
      public:
	constexpr SensorReader(const rtio_iodev &iodev, rtio &context,
			       const sensor_decoder_api &decoder) noexcept
		: iodev_{&iodev}, context_{&context}, decoder_{&decoder}
	{
	}

	[[nodiscard]] std::expected<SensorFrame, int>
	read(std::span<std::uint8_t> encoded_buffer) const noexcept;

      private:
	const rtio_iodev *iodev_;
	rtio *context_;
	const sensor_decoder_api *decoder_;
};

/** Reconfigurable multi-channel reader backed by one sensor Read IODev. */
class SensorBatch
{
      public:
	constexpr SensorBatch(const rtio_iodev &iodev, rtio &context,
			      const sensor_decoder_api &decoder) noexcept
		: reader_{iodev, context, decoder}, iodev_{&iodev}
	{
	}

	/** Configure channels. The IODev copies them into its statically allocated capacity. */
	[[nodiscard]] std::expected<void, int>
	configure(const device &sensor, std::span<const sensor_chan_spec> channels) const noexcept;

	[[nodiscard]] std::expected<SensorFrame, int>
	read(std::span<std::uint8_t> encoded_buffer) const noexcept
	{
		return reader_.read(encoded_buffer);
	}

      private:
	SensorReader reader_;
	const rtio_iodev *iodev_;
};

/** Move-only ownership of one RTIO mempool-backed sensor frame. */
class AsyncSensorFrame
{
      public:
	AsyncSensorFrame(const AsyncSensorFrame &) = delete;
	AsyncSensorFrame &operator=(const AsyncSensorFrame &) = delete;

	AsyncSensorFrame(AsyncSensorFrame &&other) noexcept;
	AsyncSensorFrame &operator=(AsyncSensorFrame &&other) noexcept;
	~AsyncSensorFrame() noexcept;

	template <typename Data>
	[[nodiscard]] std::expected<Data, int> decode(sensor_chan_spec channel) const noexcept
	{
		return SensorFrame{*decoder_, {buffer_, length_}}.decode<Data>(channel);
	}

	[[nodiscard]] constexpr std::span<const std::uint8_t> encoded() const noexcept
	{
		return {buffer_, length_};
	}
	[[nodiscard]] constexpr void *userdata() const noexcept
	{
		return userdata_;
	}

      private:
	friend class AsyncSensorReader;

	constexpr AsyncSensorFrame(rtio &context, const sensor_decoder_api &decoder,
				   std::uint8_t *buffer, std::uint32_t length,
				   void *userdata) noexcept
		: context_{&context}, decoder_{&decoder}, buffer_{buffer}, length_{length},
		  userdata_{userdata}
	{
	}

	void release() noexcept;

	rtio *context_{nullptr};
	const sensor_decoder_api *decoder_{nullptr};
	std::uint8_t *buffer_{nullptr};
	std::uint32_t length_{0U};
	void *userdata_{nullptr};
};

/** Non-blocking sensor submission and completion consumption using an RTIO mempool. */
class AsyncSensorReader
{
      public:
	constexpr AsyncSensorReader(const rtio_iodev &iodev, rtio &context,
				    const sensor_decoder_api &decoder) noexcept
		: iodev_{&iodev}, context_{&context}, decoder_{&decoder}
	{
	}

	[[nodiscard]] std::expected<void, int> submit(void *userdata = nullptr) const noexcept;
	[[nodiscard]] std::expected<std::optional<AsyncSensorFrame>, int>
	try_receive() const noexcept;
	[[nodiscard]] std::expected<AsyncSensorFrame, int> receive() const noexcept;

      private:
	[[nodiscard]] std::expected<AsyncSensorFrame, int>
	consume(rtio_cqe &completion) const noexcept;

	const rtio_iodev *iodev_;
	rtio *context_;
	const sensor_decoder_api *decoder_;
};

/** Fixed encoded storage for one typed sensor channel. */
template <typename Data, std::size_t EncodedBufferSize> class SensorChannel
{
      public:
	using value_type = Data;
	using error_type = int;

	static_assert(EncodedBufferSize > 0U, "a sensor channel needs an encoded buffer");

	constexpr SensorChannel(const rtio_iodev &iodev, rtio &context,
				const sensor_decoder_api &decoder,
				sensor_chan_spec channel) noexcept
		: reader_{iodev, context, decoder}, channel_{channel}
	{
	}

	[[nodiscard]] std::expected<Data, int> read() noexcept
	{
		const auto frame = reader_.read(buffer_);
		if (!frame) {
			return std::unexpected(frame.error());
		}
		return frame->template decode<Data>(channel_);
	}

      private:
	SensorReader reader_;
	sensor_chan_spec channel_;
	std::array<std::uint8_t, EncodedBufferSize> buffer_{};
};

/** Periodically invoke a value source whose read() returns std::expected. */
template <typename Source, typename Clock = std::chrono::steady_clock> class PeriodicSampler
{
      public:
	using source_type = Source;
	using value_type = typename Source::value_type;
	using error_type = typename Source::error_type;
	using clock = Clock;
	using duration = typename clock::duration;
	using time_point = typename clock::time_point;

	constexpr PeriodicSampler(Source source, duration interval) noexcept(
		std::is_nothrow_move_constructible_v<Source>)
		: source_{std::move(source)},
		  interval_{interval < duration::zero() ? duration::zero() : interval}
	{
	}

	/** Return no sample until due; the first call samples immediately. */
	[[nodiscard]] std::expected<std::optional<Sample<value_type>>, error_type>
	poll(time_point now = clock::now()) noexcept
	{
		if (started_ && now < next_) {
			return std::optional<Sample<value_type>>{};
		}

		auto value = source_.read();
		if (!value) {
			return std::unexpected(value.error());
		}

		started_ = true;
		next_ = now + interval_;
		return std::optional<Sample<value_type>>{Sample<value_type>{
			std::move(*value), std::chrono::duration_cast<std::chrono::nanoseconds>(
						   now.time_since_epoch())}};
	}

	[[nodiscard]] constexpr Source &source() noexcept
	{
		return source_;
	}

      private:
	Source source_;
	duration interval_;
	time_point next_{};
	bool started_{false};
};

} /* namespace zest */
