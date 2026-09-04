/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/flash_partition.hpp>

#include <limits>

namespace zest
{

Result<> FlashPartition::open() noexcept
{
	if (area_ != nullptr) {
		return {};
	}
	const struct flash_area *area = nullptr;
	ZEST_TRY(check(flash_area_open(id_, &area)));
	if (area == nullptr) {
		return fail(errors::no_device);
	}
	area_ = area;
	return {};
}

void FlashPartition::close() noexcept
{
	if (area_ != nullptr) {
		flash_area_close(area_);
		area_ = nullptr;
	}
}

Result<std::size_t> FlashPartition::size() const noexcept
{
	if (area_ == nullptr) {
		return fail(errors::bad_descriptor);
	}
	return static_cast<std::size_t>(area_->fa_size);
}

Result<std::size_t> FlashPartition::offset() const noexcept
{
	if (area_ == nullptr) {
		return fail(errors::bad_descriptor);
	}
	return static_cast<std::size_t>(area_->fa_off);
}

Result<> FlashPartition::check_range(std::size_t offset, std::size_t length) const noexcept
{
	if (area_ == nullptr) {
		return fail(errors::bad_descriptor);
	}
	const auto partition_size = static_cast<std::size_t>(area_->fa_size);
	/* Tested as a subtraction rather than as `offset + length`, which would
	 * wrap for a large length and let the access through. */
	if (offset > partition_size || length > partition_size - offset) {
		return fail(errors::out_of_range);
	}
	if (offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
		return fail(errors::out_of_range);
	}
	return {};
}

Result<> FlashPartition::read(std::size_t offset, std::span<std::byte> destination) const noexcept
{
	if (destination.empty()) {
		return {};
	}
	ZEST_TRY(check_range(offset, destination.size()));
	return check(flash_area_read(area_, static_cast<off_t>(offset), destination.data(),
				     destination.size()));
}

Result<> FlashPartition::erase(std::size_t offset, std::size_t length) noexcept
{
	if (length == 0U) {
		return {};
	}
	ZEST_TRY(check_range(offset, length));
	return check(flash_area_erase(area_, static_cast<off_t>(offset), length));
}

Result<> FlashPartition::write(std::size_t offset, std::span<const std::byte> data) noexcept
{
	if (data.empty()) {
		return {};
	}
	ZEST_TRY(check_range(offset, data.size()));
	return check(flash_area_write(area_, static_cast<off_t>(offset), data.data(), data.size()));
}

} /* namespace zest */
