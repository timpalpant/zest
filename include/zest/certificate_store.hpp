/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zephyr/net/tls_credentials.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <span>

namespace zest
{

/** Owns one TLS credential for as long as Zephyr's credential registry uses it. */
template <std::size_t Capacity> class CertificateStore
{
      public:
	CertificateStore() noexcept = default;
	~CertificateStore() noexcept
	{
		(void)remove();
	}
	CertificateStore(const CertificateStore &) = delete;
	CertificateStore &operator=(const CertificateStore &) = delete;
	CertificateStore(CertificateStore &&) = delete;
	CertificateStore &operator=(CertificateStore &&) = delete;

	[[nodiscard]] std::expected<void, int> add(sec_tag_t tag, tls_credential_type type,
						   std::span<const std::byte> credential) noexcept
	{
		if (registered_) {
			return std::unexpected(-EALREADY);
		}
		if (credential.empty() || credential.size() > Capacity) {
			return std::unexpected(credential.empty() ? -EINVAL : -ENOBUFS);
		}
		std::copy(credential.begin(), credential.end(), storage_.begin());
		const int rc = tls_credential_add(tag, type, storage_.data(), credential.size());
		if (rc < 0) {
			std::fill(storage_.begin(), storage_.end(), std::byte{});
			return std::unexpected(rc);
		}
		tag_ = tag;
		type_ = type;
		size_ = credential.size();
		registered_ = true;
		return {};
	}

	[[nodiscard]] std::expected<void, int> remove() noexcept
	{
		if (!registered_) {
			return {};
		}
		const int rc = tls_credential_delete(tag_, type_);
		if (rc < 0 && rc != -ENOENT) {
			return std::unexpected(rc);
		}
		std::fill(storage_.begin(), storage_.end(), std::byte{});
		size_ = 0U;
		registered_ = false;
		return {};
	}

	[[nodiscard]] bool registered() const noexcept
	{
		return registered_;
	}
	[[nodiscard]] sec_tag_t tag() const noexcept
	{
		return tag_;
	}
	[[nodiscard]] std::size_t size() const noexcept
	{
		return size_;
	}

      private:
	std::array<std::byte, Capacity> storage_{};
	sec_tag_t tag_{};
	tls_credential_type type_{};
	std::size_t size_{};
	bool registered_{};
};

} /* namespace zest */
