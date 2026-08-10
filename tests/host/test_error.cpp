/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "check.hpp"

#include <zest/error.hpp>

#include <cerrno>
#include <type_traits>

using zest::Error;
using zest::Result;

/* Error must cost exactly what a bare int costs. */
static_assert(sizeof(Error) == sizeof(int));
static_assert(std::is_trivially_copyable_v<Error>);
static_assert(std::is_trivially_destructible_v<Error>);

/* Construction is explicit: an int must never silently become a failure. */
static_assert(!std::is_convertible_v<int, Error>);
static_assert(std::is_constructible_v<Error, int>);

/* The whole type is usable at compile time. */
static_assert(Error{-EINVAL} == zest::errors::invalid_argument);
static_assert(Error{-EINVAL}.value() == -EINVAL);
static_assert(Error{-EINVAL}.number() == EINVAL);

/* A C API that reports a positive errno still compares equal. */
static_assert(Error{EINVAL} == zest::errors::invalid_argument);
static_assert(Error{EINVAL}.value() == -EINVAL);

/* check() maps the shape almost every Zephyr call has. */
static_assert(zest::check(0).has_value());
static_assert(zest::check(7).has_value());
static_assert(!zest::check(-ENODEV).has_value());
static_assert(zest::check(-ENODEV).error() == zest::errors::no_device);

static_assert(zest::check_value(42).value() == 42);
static_assert(zest::check_value(0).value() == 0);
static_assert(zest::check_value(-EIO).error() == zest::errors::io_error);

static_assert(zest::check_positive(3, zest::errors::no_data).value() == 3);
static_assert(zest::check_positive(0, zest::errors::no_data).error() == zest::errors::no_data);
static_assert(zest::check_positive(-EIO, zest::errors::no_data).error() == zest::errors::io_error);

namespace
{

Result<int> propagating(bool fail) noexcept
{
	if (fail) {
		return zest::fail(zest::errors::timed_out);
	}
	return 11;
}

Result<> try_macro(bool fail) noexcept
{
	ZEST_TRY(propagating(fail));
	return {};
}

Result<int> try_assign_macro(bool fail) noexcept
{
	ZEST_TRY_ASSIGN(value, propagating(fail));
	return value * 2;
}

} /* namespace */

int main()
{
	/* Every named error has a distinct, non-empty description. */
	CHECK(!Error{-EINVAL}.message().empty());
	CHECK_EQ(Error{-EINVAL}.message(), "invalid argument");
	CHECK_EQ(Error{-ETIMEDOUT}.message(), "timed out");
	CHECK_EQ(Error{-ENODEV}.message(), "no such device");
	CHECK_EQ(Error{0}.message(), "success");

	/* An errno the table does not know still yields something printable. */
	CHECK(!Error{-31337}.message().empty());

	/* The macros propagate and bind as documented. */
	CHECK_OK(try_macro(false));
	CHECK_ERR(try_macro(true), zest::errors::timed_out);
	CHECK_EQ(try_assign_macro(false).value(), 22);
	CHECK_ERR(try_assign_macro(true), zest::errors::timed_out);

	/* Distinct errno values stay distinct. */
	CHECK(zest::errors::invalid_argument != zest::errors::no_device);
	CHECK(zest::errors::timed_out != zest::errors::would_block);

	return zest::test::summary("error");
}
