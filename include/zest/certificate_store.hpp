/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/net/tls_credentials.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace zest
{

/**
 * Registers a credential that already lives in read-only memory.
 *
 * `tls_credential_add()` retains the caller's pointer rather than copying, so a
 * CA bundle compiled into the image can be registered where it sits. Copying it
 * into RAM first --- as `OwnedCredential` must, for a credential that arrives at
 * run time --- spends the credential's size in RAM for no benefit, which on a
 * small part is a meaningful fraction of the budget.
 *
 * The referenced bytes must outlive this object. A `static constexpr` array or a
 * linker-provided symbol satisfies that; a local buffer does not.
 *
 * PEM lengths include the trailing NUL; DER lengths do not.
 */
class StaticCredential
{
      public:
	StaticCredential() noexcept = default;
	~StaticCredential() noexcept
	{
		(void)remove();
	}
	StaticCredential(const StaticCredential &) = delete;
	StaticCredential &operator=(const StaticCredential &) = delete;
	StaticCredential(StaticCredential &&) = delete;
	StaticCredential &operator=(StaticCredential &&) = delete;

	/** Register @p credential, which must have static lifetime. */
	[[nodiscard]] Result<> add(sec_tag_t tag, tls_credential_type type,
				   std::span<const std::byte> credential) noexcept
	{
		if (registered_) {
			return fail(errors::already);
		}
		if (credential.empty()) {
			return fail(errors::invalid_argument);
		}
		ZEST_TRY(check(tls_credential_add(tag, type, credential.data(), credential.size())));
		tag_ = tag;
		type_ = type;
		size_ = credential.size();
		registered_ = true;
		return {};
	}

	[[nodiscard]] Result<> remove() noexcept
	{
		if (!registered_) {
			return {};
		}
		const int rc = tls_credential_delete(tag_, type_);
		if (rc < 0 && rc != -ENOENT) {
			return fail(rc);
		}
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
	sec_tag_t tag_{};
	tls_credential_type type_{};
	std::size_t size_{};
	bool registered_{};
};

/**
 * Owns a copy of one TLS credential for as long as Zephyr's registry uses it.
 *
 * Use this only for a credential that arrives at run time --- provisioned over
 * the air, or read from storage into a buffer that is about to be reused. For a
 * credential compiled into the image, prefer `StaticCredential`, which costs no
 * RAM.
 */
template <std::size_t Capacity> class OwnedCredential
{
      public:
	OwnedCredential() noexcept = default;
	~OwnedCredential() noexcept
	{
		(void)remove();
	}
	OwnedCredential(const OwnedCredential &) = delete;
	OwnedCredential &operator=(const OwnedCredential &) = delete;
	OwnedCredential(OwnedCredential &&) = delete;
	OwnedCredential &operator=(OwnedCredential &&) = delete;

	[[nodiscard]] Result<> add(sec_tag_t tag, tls_credential_type type,
				   std::span<const std::byte> credential) noexcept
	{
		if (registered_) {
			return fail(errors::already);
		}
		if (credential.empty()) {
			return fail(errors::invalid_argument);
		}
		if (credential.size() > Capacity) {
			return fail(errors::no_buffer_space);
		}
		std::copy(credential.begin(), credential.end(), storage_.begin());

		if (const int rc =
			    tls_credential_add(tag, type, storage_.data(), credential.size());
		    rc < 0) {
			std::fill(storage_.begin(), storage_.end(), std::byte{});
			return fail(rc);
		}
		tag_ = tag;
		type_ = type;
		size_ = credential.size();
		registered_ = true;
		return {};
	}

	[[nodiscard]] Result<> remove() noexcept
	{
		if (!registered_) {
			return {};
		}
		const int rc = tls_credential_delete(tag_, type_);
		if (rc < 0 && rc != -ENOENT) {
			return fail(rc);
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
	[[nodiscard]] static constexpr std::size_t capacity() noexcept
	{
		return Capacity;
	}

      private:
	std::array<std::byte, Capacity> storage_{};
	sec_tag_t tag_{};
	tls_credential_type type_{};
	std::size_t size_{};
	bool registered_{};
};

/** Retained name for `OwnedCredential`. */
template <std::size_t Capacity> using CertificateStore = OwnedCredential<Capacity>;

} /* namespace zest */
