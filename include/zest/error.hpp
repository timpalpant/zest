/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <cerrno>
#include <expected>
#include <string_view>

namespace zest
{

/**
 * A failure reported by a Zephyr C API, as a negative errno value.
 *
 * `Error` is a zero-overhead wrapper: it is trivially copyable and
 * `sizeof(Error) == sizeof(int)`, so returning one costs exactly what returning
 * a bare `int` costs. It exists so a failure cannot be mistaken for a count, a
 * length, or a file descriptor, and so every failure has a `message()`.
 *
 * Construction is deliberately `explicit`. An `int` in this library is a value;
 * only code that has decided an `int` *is* a failure may say so.
 *
 * `Error` covers the errno domain and nothing else. Failures that are not errno
 * values keep their own types, so they can never be confused with one --- see
 * `DnsError`, `CurveError`, `TransformError` and `HttpError`. This matters
 * concretely: Zephyr's `DNS_EAI_NONAME` is `-2`, which as an errno would read as
 * `-ENOENT`, and `DNS_EAI_MEMORY` is `-12`, which would read as `-ENOMEM`.
 */
class Error
{
      public:
	/**
	 * Wrap a negative errno value.
	 *
	 * A positive @p code is negated, so a C function that reports `EINVAL`
	 * rather than `-EINVAL` still compares equal to `errors::invalid_argument`.
	 */
	explicit constexpr Error(int code) noexcept : code_{code > 0 ? -code : code}
	{
	}

	/** The wrapped value: always negative. */
	[[nodiscard]] constexpr int value() const noexcept
	{
		return code_;
	}

	/** The positive errno number, as `<cerrno>` spells it. */
	[[nodiscard]] constexpr int number() const noexcept
	{
		return -code_;
	}

	/**
	 * A short, static description such as `"invalid argument"`.
	 *
	 * Never empty and never allocates; the view points at storage with static
	 * lifetime. When `CONFIG_ZEST_ERROR_STRINGS=n` the table is compiled out
	 * of the image entirely and this returns `"error"` for every value.
	 */
	[[nodiscard]] std::string_view message() const noexcept;

	friend constexpr bool operator==(Error, Error) noexcept = default;

      private:
	int code_;
};

/**
 * The result of a fallible Zest operation.
 *
 * `Result<>` spells the void case, so an operation that either succeeds or
 * reports a failure returns `Result<>`, and one that yields a value returns
 * `Result<T>`.
 */
template <typename T = void> using Result = std::expected<T, Error>;

/** Construct the failure half of a `Result`. */
[[nodiscard]] constexpr std::unexpected<Error> fail(Error error) noexcept
{
	return std::unexpected<Error>{error};
}

/** Construct the failure half of a `Result` from a negative errno value. */
[[nodiscard]] constexpr std::unexpected<Error> fail(int code) noexcept
{
	return std::unexpected<Error>{Error{code}};
}

/**
 * Translate a Zephyr C return value into a `Result<>`.
 *
 * Negative is failure; zero or positive is success. This is the shape almost
 * every Zephyr call has, so it keeps wrappers to one line:
 *
 * ```cpp
 * return zest::check(gpio_pin_set_dt(&spec_, value));
 * ```
 */
[[nodiscard]] constexpr Result<> check(int rc) noexcept
{
	if (rc < 0) {
		return fail(rc);
	}
	return {};
}

/**
 * Translate a Zephyr C return value that carries a non-negative result.
 *
 * Negative is failure; otherwise the value is returned unchanged.
 */
[[nodiscard]] constexpr Result<int> check_value(int rc) noexcept
{
	if (rc < 0) {
		return fail(rc);
	}
	return rc;
}

/**
 * Translate a call that reports failure by returning a negative value *or* zero
 * where zero is not meaningful, mapping zero to @p zero_error.
 */
[[nodiscard]] constexpr Result<int> check_positive(int rc, Error zero_error) noexcept
{
	if (rc < 0) {
		return fail(rc);
	}
	if (rc == 0) {
		return fail(zero_error);
	}
	return rc;
}

/** Named errno values, so callers need neither `<cerrno>` nor a sign convention. */
namespace errors
{

inline constexpr Error invalid_argument{-EINVAL};
inline constexpr Error no_device{-ENODEV};
inline constexpr Error no_memory{-ENOMEM};
inline constexpr Error no_buffer_space{-ENOBUFS};
inline constexpr Error io_error{-EIO};
inline constexpr Error busy{-EBUSY};
inline constexpr Error permission_denied{-EACCES};
inline constexpr Error already{-EALREADY};
inline constexpr Error timed_out{-ETIMEDOUT};
inline constexpr Error not_connected{-ENOTCONN};
inline constexpr Error connection_reset{-ECONNRESET};
inline constexpr Error connection_refused{-ECONNREFUSED};
inline constexpr Error connection_aborted{-ECONNABORTED};
inline constexpr Error host_unreachable{-EHOSTUNREACH};
inline constexpr Error network_down{-ENETDOWN};
inline constexpr Error name_too_long{-ENAMETOOLONG};
inline constexpr Error too_big{-E2BIG};
inline constexpr Error message_size{-EMSGSIZE};
inline constexpr Error bad_message{-EBADMSG};
inline constexpr Error illegal_sequence{-EILSEQ};
inline constexpr Error no_data{-ENODATA};
inline constexpr Error not_found{-ENOENT};
inline constexpr Error not_supported{-ENOTSUP};
inline constexpr Error would_block{-EAGAIN};
inline constexpr Error in_progress{-EINPROGRESS};
inline constexpr Error bad_descriptor{-EBADF};
inline constexpr Error no_message{-ENOMSG};
inline constexpr Error overflow{-EOVERFLOW};
inline constexpr Error out_of_range{-ERANGE};
inline constexpr Error exists{-EEXIST};
inline constexpr Error no_space{-ENOSPC};
inline constexpr Error broken_pipe{-EPIPE};
inline constexpr Error shutdown{-ESHUTDOWN};
inline constexpr Error protocol_not_supported{-EPROTONOSUPPORT};
inline constexpr Error interrupted{-EINTR};
inline constexpr Error not_permitted{-EPERM};

} /* namespace errors */

} /* namespace zest */

/**
 * Return early from a `Result`-returning function when @p expr fails.
 *
 * Propagates the error unchanged and discards a successful value, so it suits
 * calls whose only interesting outcome is failure:
 *
 * ```cpp
 * zest::Result<> start() noexcept
 * {
 *     ZEST_TRY(channel.init());
 *     ZEST_TRY(pump.init());
 *     return {};
 * }
 * ```
 */
#define ZEST_TRY(expr)                                                                             \
	do {                                                                                       \
		if (auto _zest_result = (expr); !_zest_result) {                                    \
			return ::zest::fail(_zest_result.error());                                  \
		}                                                                                  \
	} while (false)

/**
 * Bind the value of @p expr to a new reference @p name, or return its error.
 *
 * ```cpp
 * ZEST_TRY_ASSIGN(millivolts, battery.read_millivolts());
 * ```
 */
#define ZEST_TRY_ASSIGN(name, expr)                                                                \
	auto _zest_result_##name = (expr);                                                         \
	if (!_zest_result_##name) {                                                                \
		return ::zest::fail(_zest_result_##name.error());                                  \
	}                                                                                          \
	auto &name = *_zest_result_##name
