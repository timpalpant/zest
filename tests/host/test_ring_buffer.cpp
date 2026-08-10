/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/ring_buffer.hpp>

#include <array>
#include <cstdint>

using namespace zest;

int main()
{
	SpscRingBuffer<int, 4> buffer;

	CHECK(buffer.empty());
	CHECK(!buffer.full());
	CHECK_EQ(buffer.size(), 0U);
	CHECK_EQ(buffer.capacity(), 4U);
	CHECK_EQ(buffer.available(), 4U);
	CHECK(!buffer.try_pop().has_value());
	CHECK(!buffer.peek().has_value());

	/* Fill to capacity: the reserved slot must not be usable. */
	for (int i = 1; i <= 4; ++i) {
		CHECK(buffer.try_push(i));
	}
	CHECK(buffer.full());
	CHECK_EQ(buffer.size(), 4U);
	CHECK_EQ(buffer.available(), 0U);
	CHECK(!buffer.try_push(5));

	/* Order is oldest first, and peek does not consume. */
	CHECK_EQ(buffer.peek().value(), 1);
	CHECK_EQ(buffer.size(), 4U);
	CHECK_EQ(buffer.try_pop().value(), 1);
	CHECK_EQ(buffer.try_pop().value(), 2);
	CHECK_EQ(buffer.size(), 2U);

	/* Indices must wrap without corruption. */
	CHECK(buffer.try_push(5));
	CHECK(buffer.try_push(6));
	CHECK(buffer.full());
	CHECK_EQ(buffer.try_pop().value(), 3);
	CHECK_EQ(buffer.try_pop().value(), 4);
	CHECK_EQ(buffer.try_pop().value(), 5);
	CHECK_EQ(buffer.try_pop().value(), 6);
	CHECK(buffer.empty());

	/* Many wraps must stay consistent. */
	for (int round = 0; round < 100; ++round) {
		CHECK(buffer.try_push(round));
		CHECK_EQ(buffer.try_pop().value(), round);
	}
	CHECK(buffer.empty());

	/* Overwrite keeps the newest samples. */
	{
		SpscRingBuffer<int, 3> newest;
		for (int i = 1; i <= 5; ++i) {
			CHECK(newest.push_overwrite(i));
		}
		CHECK_EQ(newest.size(), 3U);
		CHECK_EQ(newest.try_pop().value(), 3);
		CHECK_EQ(newest.try_pop().value(), 4);
		CHECK_EQ(newest.try_pop().value(), 5);
	}

	/* Bulk drain reports how much it moved and empties the buffer. */
	{
		SpscRingBuffer<std::int32_t, 8> samples;
		for (std::int32_t i = 0; i < 6; ++i) {
			CHECK(samples.try_push(i * 10));
		}
		std::array<std::int32_t, 4> window{};
		CHECK_EQ(samples.drain(window), 4U);
		CHECK_EQ(window[0], 0);
		CHECK_EQ(window[3], 30);
		CHECK_EQ(samples.size(), 2U);

		std::array<std::int32_t, 8> rest{};
		CHECK_EQ(samples.drain(rest), 2U);
		CHECK_EQ(rest[0], 40);
		CHECK_EQ(rest[1], 50);
		CHECK(samples.empty());
		/* Draining an empty buffer moves nothing. */
		CHECK_EQ(samples.drain(rest), 0U);
	}

	/* clear() discards everything. */
	{
		SpscRingBuffer<int, 4> scratch;
		CHECK(scratch.try_push(1));
		CHECK(scratch.try_push(2));
		scratch.clear();
		CHECK(scratch.empty());
		CHECK_EQ(scratch.size(), 0U);
	}

	/* A struct payload round-trips intact. */
	{
		struct Reading {
			std::int32_t millivolts;
			std::uint32_t uptime_ms;
		};
		SpscRingBuffer<Reading, 2> readings;
		CHECK(readings.try_push(Reading{3700, 1234}));
		const auto popped = readings.try_pop();
		CHECK(popped.has_value());
		CHECK_EQ(popped->millivolts, 3700);
		CHECK_EQ(popped->uptime_ms, 1234U);
	}

	return zest::test::summary("ring_buffer");
}
