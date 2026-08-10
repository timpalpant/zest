/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * A dependency-free check harness for the Zephyr-independent parts of Zest.
 *
 * These tests build with a plain host compiler so the numeric, timing, control
 * and error layers can be exercised without a west workspace or a Zephyr build.
 *
 * Every check evaluates its arguments exactly once. That is not a detail: a
 * macro that expands its operand twice silently double-advances any stateful
 * expression passed to it, which turns a passing filter into a failing one for
 * reasons that have nothing to do with the code under test.
 */

#pragma once

#include <cstdio>
#include <string_view>

namespace zest::test
{

inline int failures = 0;
inline int checks = 0;

inline void report(bool passed, std::string_view expression, std::string_view file,
		   int line) noexcept
{
	++checks;
	if (passed) {
		return;
	}
	++failures;
	std::fprintf(stderr, "%.*s:%d: FAIL  %.*s\n", static_cast<int>(file.size()), file.data(),
		     line, static_cast<int>(expression.size()), expression.data());
}

template <typename Lhs, typename Rhs>
void check_equal(const Lhs &lhs, const Rhs &rhs, std::string_view expression, std::string_view file,
		 int line) noexcept
{
	report(lhs == rhs, expression, file, line);
}

template <typename T>
void check_near(T lhs, T rhs, T tolerance, std::string_view expression, std::string_view file,
		int line) noexcept
{
	const T difference = lhs > rhs ? lhs - rhs : rhs - lhs;
	report(difference <= tolerance, expression, file, line);
}

template <typename Result>
void check_ok(const Result &result, std::string_view expression, std::string_view file,
	      int line) noexcept
{
	report(result.has_value(), expression, file, line);
}

template <typename Result, typename Error>
void check_error(const Result &result, const Error &expected, std::string_view expression,
		 std::string_view file, int line) noexcept
{
	report(!result.has_value() && result.error() == expected, expression, file, line);
}

inline int summary(std::string_view name) noexcept
{
	std::fprintf(stderr, "%.*s: %d checks, %d failed\n", static_cast<int>(name.size()),
		     name.data(), checks, failures);
	return failures == 0 ? 0 : 1;
}

} /* namespace zest::test */

#define CHECK(expr) ::zest::test::report((expr), #expr, __FILE__, __LINE__)

#define CHECK_EQ(lhs, rhs)                                                                         \
	::zest::test::check_equal((lhs), (rhs), #lhs " == " #rhs, __FILE__, __LINE__)

#define CHECK_NEAR(lhs, rhs, tolerance)                                                            \
	::zest::test::check_near((lhs), (rhs), (tolerance), #lhs " ~= " #rhs, __FILE__, __LINE__)

#define CHECK_OK(expr) ::zest::test::check_ok((expr), #expr " succeeded", __FILE__, __LINE__)

#define CHECK_ERR(expr, expected)                                                                  \
	::zest::test::check_error((expr), (expected), #expr " == " #expected, __FILE__, __LINE__)
