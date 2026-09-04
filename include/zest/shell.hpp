/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * Argument parsing and error reporting for Zephyr shell commands.
 *
 * The `SHELL_CMD` macros stay: they build a command tree in a linker section,
 * which nothing in C++ improves on. The boilerplate is on either side of them —
 * `ARG_UNUSED` for the parameters a command does not use, a hand-written
 * `strtol` with range checks for each numeric argument, an if-chain to turn a
 * word into an enum, and an ad-hoc line to print a `Result`'s error.
 *
 * That is what this covers. @ref zest::ShellArgs wraps `(argc, argv)` in something
 * range-checked and typed, so a missing or malformed argument is one `if`
 * instead of five lines that are subtly different in every command.
 */

#include <zest/error.hpp>

#include <zephyr/shell/shell.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace zest
{

/**
 * A shell command's arguments, bounds-checked.
 *
 * Index 0 is the command name, exactly as `argv` has it, so the first real
 * argument is index 1 and the numbering matches the usage strings people write.
 *
 * Every accessor reports a missing argument as `errors::invalid_argument` rather
 * than reading past the end, which is what makes `eq set-band` with no arguments
 * a usage message instead of a fault.
 */
class ShellArgs
{
      public:
	ShellArgs(std::size_t argc, char **argv) noexcept : argc_{argc}, argv_{argv}
	{
	}

	/** Total count, including the command name. */
	[[nodiscard]] std::size_t size() const noexcept
	{
		return argc_;
	}

	/** Arguments after the command name. */
	[[nodiscard]] std::size_t count() const noexcept
	{
		return argc_ > 0U ? argc_ - 1U : 0U;
	}

	[[nodiscard]] bool has(std::size_t index) const noexcept
	{
		return index < argc_ && argv_ != nullptr && argv_[index] != nullptr;
	}

	/** The raw argument, or an empty view when there is none. */
	[[nodiscard]] std::string_view operator[](std::size_t index) const noexcept
	{
		return has(index) ? std::string_view{argv_[index]} : std::string_view{};
	}

	[[nodiscard]] Result<std::string_view> text(std::size_t index) const noexcept
	{
		if (!has(index)) {
			return fail(errors::invalid_argument);
		}
		return std::string_view{argv_[index]};
	}

	/**
	 * Parse argument @p index as an integer within [@p minimum, @p maximum].
	 *
	 * Trailing rubbish is rejected rather than ignored, so `set-band 2x` is an
	 * error instead of a silent 2 — `strtol` accepts the prefix and the caller
	 * almost never checks its end pointer.
	 */
	template <typename T = long>
		requires std::is_integral_v<T>
	[[nodiscard]] Result<T> integer(std::size_t index,
					T minimum = std::numeric_limits<T>::min(),
					T maximum = std::numeric_limits<T>::max()) const noexcept
	{
		const auto argument = text(index);
		if (!argument) {
			return fail(argument.error());
		}
		const std::string_view value = *argument;
		if (value.empty()) {
			return fail(errors::invalid_argument);
		}

		T parsed{};
		const auto *first = value.data();
		const auto *last = value.data() + value.size();
		const auto [end, code] = std::from_chars(first, last, parsed);
		if (code != std::errc{} || end != last) {
			return fail(errors::invalid_argument);
		}
		if (parsed < minimum || parsed > maximum) {
			return fail(errors::out_of_range);
		}
		return parsed;
	}

	/**
	 * Match argument @p index against a table of words.
	 *
	 * ```cpp
	 * static constexpr std::array kModes{
	 *     zest::ShellKeyword<Mode>{"off", Mode::off},
	 *     zest::ShellKeyword<Mode>{"both", Mode::both},
	 * };
	 * const auto mode = args.keyword(1, std::span{kModes});
	 * ```
	 *
	 * Reports `errors::not_found` for a word that is not in the table, which a
	 * command can tell from a missing argument and answer differently.
	 */
	template <typename T>
	[[nodiscard]] Result<T>
	keyword(std::size_t index,
		std::span<const std::pair<std::string_view, T>> table) const noexcept
	{
		const auto argument = text(index);
		if (!argument) {
			return fail(argument.error());
		}
		for (const auto &[word, value] : table) {
			if (word == *argument) {
				return value;
			}
		}
		return fail(errors::not_found);
	}

	/** The argument at @p index, or @p fallback when absent. */
	[[nodiscard]] std::string_view text_or(std::size_t index,
					       std::string_view fallback) const noexcept
	{
		return has(index) ? std::string_view{argv_[index]} : fallback;
	}

      private:
	std::size_t argc_;
	char **argv_;
};

/** One entry in a @ref ShellArgs::keyword table. */
template <typename T> using ShellKeyword = std::pair<std::string_view, T>;

/**
 * Print @p error on @p shell as a message, not a number.
 *
 * `Error::message()` is what makes a shell answer "Invalid argument" rather than
 * "failed (-22)", and it costs nothing that a bare errno did not already cost.
 */
inline void shell_report(const struct shell *shell, Error error,
			 std::string_view context = {}) noexcept
{
	if (shell == nullptr) {
		return;
	}
	const auto message = error.message();
	if (context.empty()) {
		shell_error(shell, "%.*s (%d)", static_cast<int>(message.size()), message.data(),
			    error.value());
		return;
	}
	shell_error(shell, "%.*s: %.*s (%d)", static_cast<int>(context.size()), context.data(),
		    static_cast<int>(message.size()), message.data(), error.value());
}

/**
 * Report @p result on @p shell and turn it into a command return code.
 *
 * A shell command returns an errno, and Zephyr prints its own message for a
 * non-zero one on top of anything already printed. Returning 0 after reporting
 * keeps the output to the one line that actually says what happened.
 */
template <typename T>
[[nodiscard]] int shell_finish(const struct shell *shell, const Result<T> &result,
			       std::string_view context = {}) noexcept
{
	if (!result) {
		shell_report(shell, result.error(), context);
	}
	return 0;
}

} /* namespace zest */
