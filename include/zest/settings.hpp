/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/function.hpp>

#include <zephyr/settings/settings.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace zest
{

namespace detail
{

template <typename T> struct is_view: std::false_type {
};
template <typename C, typename Traits>
struct is_view<std::basic_string_view<C, Traits>>: std::true_type {
};
template <typename T, std::size_t N> struct is_view<std::span<T, N>>: std::true_type {
};

} /* namespace detail */

/**
 * A type whose object representation can be persisted byte for byte.
 *
 * Trivial copyability alone is not enough: a `std::string_view` is trivially
 * copyable, but persisting its bytes stores a pointer and a length that read back
 * dangling. Pointers, references, arrays and view types are rejected here; the
 * string overloads below persist the characters instead.
 *
 * This cannot detect a pointer nested inside a struct, so a persisted aggregate
 * must still hold only values. Prefer an explicit versioned record for anything
 * whose layout may change.
 */
template <typename T>
concept TriviallySerializable =
	std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_array_v<T> &&
	!std::is_member_pointer_v<T> && !std::is_reference_v<T> && !std::is_union_v<T> &&
	!detail::is_view<std::remove_cv_t<T>>::value;

/** Fixed-storage typed access to a subtree of Zephyr settings. */
template <std::size_t MaximumNameLength = 64U> class Settings
{
      public:
	static_assert(MaximumNameLength > 0U, "settings names need at least one character");

	/** Callback for `for_each`, receiving the key relative to the root. */
	using Visitor = FunctionRef<void(std::string_view key, std::span<const std::byte> value)>;

	constexpr explicit Settings(std::string_view root) noexcept
	{
		if (root.size() > MaximumNameLength) {
			valid_ = false;
			return;
		}
		std::copy(root.begin(), root.end(), root_.begin());
		root_length_ = root.size();
		root_[root_length_] = '\0';
	}

	/** Initialize the configured Zephyr settings backend. */
	[[nodiscard]] Result<> init() const noexcept
	{
		if (!valid_) {
			return fail(errors::name_too_long);
		}
		return check(settings_subsys_init());
	}

	/**
	 * Load this subtree through the registered handlers.
	 *
	 * `set()` and `get()` below use `settings_save_one`/`settings_load_one`,
	 * which bypass the handler mechanism. Values owned by other subsystems ---
	 * anything registered with `SETTINGS_STATIC_HANDLER` --- only become visible
	 * after this call.
	 */
	[[nodiscard]] Result<> load() const noexcept
	{
		if (!valid_) {
			return fail(errors::name_too_long);
		}
		return check(settings_load_subtree(root_.data()));
	}

	/** Flush pending changes to the backend, where the backend buffers. */
	[[nodiscard]] Result<> commit() const noexcept
	{
		return check(settings_commit_subtree(root_.data()));
	}

	/** Store raw bytes. */
	[[nodiscard]] Result<> set(std::string_view key,
				   std::span<const std::byte> value) const noexcept
	{
		ZEST_TRY_ASSIGN(name, make_name(key));
		return check(settings_save_one(name.data(), value.data(), value.size()));
	}

	/**
	 * Store text.
	 *
	 * The characters are persisted, not the view. Without this overload the
	 * generic one would serialize the view object itself.
	 */
	[[nodiscard]] Result<> set(std::string_view key, std::string_view value) const noexcept
	{
		return set(key, std::as_bytes(std::span{value.data(), value.size()}));
	}

	/** Store a value byte for byte. */
	template <TriviallySerializable T>
	[[nodiscard]] Result<> set(std::string_view key, const T &value) const noexcept
	{
		return set(key, std::as_bytes(std::span{&value, 1U}));
	}

	/** Read raw bytes, returning the number written to @p destination. */
	[[nodiscard]] Result<std::size_t> get(std::string_view key,
					      std::span<std::byte> destination) const noexcept
	{
		ZEST_TRY_ASSIGN(name, make_name(key));
		const ssize_t size =
			settings_load_one(name.data(), destination.data(), destination.size());
		if (size < 0) {
			return fail(static_cast<int>(size));
		}
		if (static_cast<std::size_t>(size) > destination.size()) {
			return fail(errors::no_buffer_space);
		}
		return static_cast<std::size_t>(size);
	}

	/** Read text into @p destination and return a view of what was read. */
	[[nodiscard]] Result<std::string_view>
	get_string(std::string_view key, std::span<char> destination) const noexcept
	{
		ZEST_TRY_ASSIGN(size, get(key, std::as_writable_bytes(destination)));
		return std::string_view{destination.data(), size};
	}

	/** Read a value byte for byte. */
	template <TriviallySerializable T>
	[[nodiscard]] Result<T> get(std::string_view key) const noexcept
	{
		T value{};
		auto bytes = std::as_writable_bytes(std::span{&value, 1U});
		ZEST_TRY_ASSIGN(size, get(key, bytes));
		if (size != sizeof(T)) {
			return fail(errors::message_size);
		}
		return value;
	}

	/** Whether a key is present. */
	[[nodiscard]] bool contains(std::string_view key) const noexcept
	{
		std::array<std::byte, 1> probe{};
		const auto size = get(key, probe);
		return size.has_value() || size.error() == errors::no_buffer_space;
	}

	[[nodiscard]] Result<> erase(std::string_view key) const noexcept
	{
		ZEST_TRY_ASSIGN(name, make_name(key));
		return check(settings_delete(name.data()));
	}

	/** The configured root, without a trailing separator. */
	[[nodiscard]] constexpr std::string_view root() const noexcept
	{
		return std::string_view{root_.data(), root_length_};
	}

      private:
	using Name = std::array<char, MaximumNameLength + 1U>;

	[[nodiscard]] constexpr Result<Name> make_name(std::string_view key) const noexcept
	{
		if (!valid_) {
			return fail(errors::name_too_long);
		}
		if (key.empty()) {
			return fail(errors::invalid_argument);
		}
		const std::size_t separator = root_length_ == 0U ? 0U : 1U;
		if (root_length_ + separator + key.size() > MaximumNameLength) {
			return fail(errors::name_too_long);
		}

		Name name{};
		std::copy_n(root_.begin(), root_length_, name.begin());
		std::size_t offset = root_length_;
		if (separator != 0U) {
			name[offset++] = '/';
		}
		std::copy(key.begin(), key.end(),
			  name.begin() + static_cast<std::ptrdiff_t>(offset));
		name[offset + key.size()] = '\0';
		return name;
	}

	Name root_{};
	std::size_t root_length_{0U};
	bool valid_{true};
};

} /* namespace zest */
