/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/function.hpp>

#include <cstdint>
#include <type_traits>

using namespace zest;

struct CallableObject {
	int state{};
	int operator()(int value) noexcept
	{
		return value + state;
	}
};

/* A reference is two pointers and never allocates. */
static_assert(sizeof(FunctionRef<int(int) noexcept>) == 2 * sizeof(void *));
static_assert(!std::is_constructible_v<FunctionRef<int(int) noexcept>, CallableObject>);

namespace
{

int apply(FunctionRef<int(int) noexcept> operation, int value) noexcept
{
	return operation(value);
}

int plain_function(int value) noexcept
{
	return value + 1;
}

} /* namespace */

int main()
{
	/* A capturing lambda works, which a bare function pointer cannot do. */
	int offset = 10;
	auto add_offset = [&](int value) noexcept { return value + offset; };
	CHECK_EQ(apply(add_offset, 5), 15);

	offset = 100;
	CHECK_EQ(apply(add_offset, 5), 105);

	/* A plain function and a stateless lambda work too. */
	CHECK_EQ(apply(plain_function, 5), 6);
	auto times_two = [](int value) noexcept { return value * 2; };
	CHECK_EQ(apply(times_two, 5), 10);

	/* A mutable callable can accumulate through the reference. */
	{
		int total = 0;
		auto accumulate = [&](int value) noexcept {
			total += value;
			return total;
		};
		FunctionRef<int(int) noexcept> sink{accumulate};
		CHECK_EQ(sink(1), 1);
		CHECK_EQ(sink(2), 3);
		CHECK_EQ(sink(3), 6);
		CHECK_EQ(total, 6);
	}

	/* The throwing signature is available for host-side use. */
	{
		auto doubler = [](int value) { return value * 2; };
		FunctionRef<int(int)> reference{doubler};
		CHECK_EQ(reference(21), 42);
	}

	/* InplaceFunction owns its callable. */
	{
		InplaceFunction<int(int) noexcept, 4 * sizeof(void *)> stored;
		CHECK(!static_cast<bool>(stored));

		int base = 7;
		stored = [base](int value) noexcept { return value + base; };
		CHECK(static_cast<bool>(stored));
		CHECK_EQ(stored(3), 10);

		/* Reassignment destroys the previous callable. */
		stored = [](int value) noexcept { return value * value; };
		CHECK_EQ(stored(5), 25);

		/* Moving transfers ownership and empties the source. */
		auto moved = std::move(stored);
		CHECK(static_cast<bool>(moved));
		CHECK(!static_cast<bool>(stored));
		CHECK_EQ(moved(6), 36);

		moved.reset();
		CHECK(!static_cast<bool>(moved));
	}

	/* A stored callable's destructor must run exactly once. */
	{
		static int live = 0;
		struct Tracked {
			Tracked() noexcept
			{
				++live;
			}
			Tracked(const Tracked &) noexcept
			{
				++live;
			}
			Tracked(Tracked &&) noexcept
			{
				++live;
			}
			~Tracked() noexcept
			{
				--live;
			}
			int operator()(int value) const noexcept
			{
				return value;
			}
		};
		{
			InplaceFunction<int(int) noexcept, 4 * sizeof(void *)> holder{Tracked{}};
			CHECK(live >= 1);
			CHECK_EQ(holder(4), 4);
		}
		CHECK_EQ(live, 0);
	}

	return zest::test::summary("function");
}
