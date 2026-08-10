#pragma once

#include <zephyr/settings/settings.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

namespace zest
{

/** Fixed-storage typed access to a subtree of Zephyr settings. */
template <std::size_t MaximumNameLength = 64U> class Settings
{
      public:
	static_assert(MaximumNameLength > 0U, "settings names need at least one character");

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
	[[nodiscard]] std::expected<void, int> init() const noexcept
	{
		if (!valid_) {
			return std::unexpected(-ENAMETOOLONG);
		}
		const int rc = settings_subsys_init();
		if (rc != 0) {
			return std::unexpected(rc < 0 ? rc : -EIO);
		}
		return {};
	}

	[[nodiscard]] std::expected<void, int> set(std::string_view key,
						   std::span<const std::byte> value) const noexcept
	{
		auto name = make_name(key);
		if (!name) {
			return std::unexpected(name.error());
		}
		const int rc = settings_save_one(name->data(), value.data(), value.size());
		if (rc != 0) {
			return std::unexpected(rc < 0 ? rc : -EIO);
		}
		return {};
	}

	[[nodiscard]] std::expected<std::size_t, int>
	get(std::string_view key, std::span<std::byte> destination) const noexcept
	{
		auto name = make_name(key);
		if (!name) {
			return std::unexpected(name.error());
		}
		const ssize_t size =
			settings_load_one(name->data(), destination.data(), destination.size());
		if (size < 0) {
			return std::unexpected(static_cast<int>(size));
		}
		if (static_cast<std::size_t>(size) > destination.size()) {
			return std::unexpected(-ENOBUFS);
		}
		return static_cast<std::size_t>(size);
	}

	template <typename T>
		requires std::is_trivially_copyable_v<T>
	[[nodiscard]] std::expected<void, int> set(std::string_view key,
						   const T &value) const noexcept
	{
		return set(key, std::as_bytes(std::span{&value, 1U}));
	}

	template <typename T>
		requires std::is_trivially_copyable_v<T>
	[[nodiscard]] std::expected<T, int> get(std::string_view key) const noexcept
	{
		T value{};
		auto bytes = std::as_writable_bytes(std::span{&value, 1U});
		const auto size = get(key, bytes);
		if (!size) {
			return std::unexpected(size.error());
		}
		if (*size != sizeof(T)) {
			return std::unexpected(-EMSGSIZE);
		}
		return value;
	}

	[[nodiscard]] std::expected<void, int> erase(std::string_view key) const noexcept
	{
		auto name = make_name(key);
		if (!name) {
			return std::unexpected(name.error());
		}
		const int rc = settings_delete(name->data());
		if (rc != 0) {
			return std::unexpected(rc < 0 ? rc : -EIO);
		}
		return {};
	}

      private:
	using Name = std::array<char, MaximumNameLength + 1U>;

	[[nodiscard]] constexpr std::expected<Name, int>
	make_name(std::string_view key) const noexcept
	{
		if (!valid_ || key.empty()) {
			return std::unexpected(valid_ ? -EINVAL : -ENAMETOOLONG);
		}
		const std::size_t separator = root_length_ == 0U ? 0U : 1U;
		if (root_length_ + separator + key.size() > MaximumNameLength) {
			return std::unexpected(-ENAMETOOLONG);
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
