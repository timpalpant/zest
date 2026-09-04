/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <zest/usb_device.hpp>

#include <zest/kernel.hpp>

namespace zest
{

/**
 * The library's single USB message callback and the devices it feeds.
 *
 * Registration and dispatch are serialized. Zephyr's `k_mutex` is recursive for
 * its owner, so a device that enables or disables itself from inside the
 * callback re-enters rather than deadlocking.
 */
struct UsbMessageDispatch {
	static Mutex &lock() noexcept
	{
		static Mutex mutex;
		return mutex;
	}

	static UsbDevice *&head() noexcept
	{
		static UsbDevice *devices = nullptr;
		return devices;
	}

	static void link(UsbDevice &device) noexcept
	{
		ScopedLock guard{lock()};
		for (auto *existing = head(); existing != nullptr; existing = existing->next_) {
			if (existing == &device) {
				return;
			}
		}
		device.next_ = head();
		head() = &device;
	}

	static void unlink(UsbDevice &device) noexcept
	{
		ScopedLock guard{lock()};
		for (auto **slot = &head(); *slot != nullptr; slot = &(*slot)->next_) {
			if (*slot == &device) {
				*slot = device.next_;
				break;
			}
		}
		device.next_ = nullptr;
	}

	static void on_message(usbd_context *const context, const usbd_msg *const message) noexcept
	{
		if (context == nullptr || message == nullptr) {
			return;
		}
		ScopedLock guard{lock()};
		for (auto *device = head(); device != nullptr; device = device->next_) {
			if (device->context_ != context || !device->follow_vbus_) {
				continue;
			}
			/* Only a controller that reports VBUS raises these, so a
			 * device on one that does not is already enabled and must
			 * not be disabled by a message it will never see. */
			if (!usbd_can_detect_vbus(context)) {
				continue;
			}
			if (message->type == USBD_MSG_VBUS_READY) {
				(void)device->enable();
			} else if (message->type == USBD_MSG_VBUS_REMOVED) {
				(void)device->disable();
			}
		}
	}
};

UsbDevice::~UsbDevice() noexcept
{
	if (enabled_) {
		(void)disable();
	}
	if (started_) {
		UsbMessageDispatch::unlink(*this);
		started_ = false;
	}
}

bool UsbDevice::can_detect_vbus() const noexcept
{
	return context_ != nullptr && usbd_can_detect_vbus(context_);
}

Result<> UsbDevice::enable() noexcept
{
	if (context_ == nullptr) {
		return fail(errors::no_device);
	}
	if (enabled_) {
		return {};
	}
	ZEST_TRY(check(usbd_enable(context_)));
	enabled_ = true;
	return {};
}

Result<> UsbDevice::disable() noexcept
{
	if (context_ == nullptr) {
		return fail(errors::no_device);
	}
	if (!enabled_) {
		return {};
	}
	ZEST_TRY(check(usbd_disable(context_)));
	enabled_ = false;
	return {};
}

Result<> UsbDevice::start(const UsbDescriptors &descriptors, usbd_config_node &configuration,
			  const UsbDeviceOptions &options) noexcept
{
	if (context_ == nullptr) {
		return fail(errors::no_device);
	}
	if (started_) {
		return fail(errors::already);
	}

	/* The language descriptor has to come first: the others are indexed
	 * against it, and a host asking for a string before one exists gets a
	 * stall rather than an empty answer. */
	for (usbd_desc_node *descriptor : {descriptors.language, descriptors.manufacturer,
					   descriptors.product, descriptors.serial_number}) {
		if (descriptor == nullptr) {
			continue;
		}
		ZEST_TRY(check(usbd_add_descriptor(context_, descriptor)));
	}

	ZEST_TRY(check(usbd_add_configuration(context_, options.speed, &configuration)));
	ZEST_TRY(check(usbd_register_all_classes(
		context_, options.speed, options.configuration_number, options.class_blocklist)));

	if (options.code_triple.has_value()) {
		ZEST_TRY(check(usbd_device_set_code_triple(
			context_, options.speed, options.code_triple->base_class,
			options.code_triple->subclass, options.code_triple->protocol)));
	}
	/* Returns void: it only records what the configuration descriptor reports. */
	usbd_self_powered(context_, options.self_powered);

	follow_vbus_ = options.follow_vbus;
	UsbMessageDispatch::link(*this);
	started_ = true;

	if (const auto registered =
		    check(usbd_msg_register_cb(context_, UsbMessageDispatch::on_message));
	    !registered) {
		UsbMessageDispatch::unlink(*this);
		started_ = false;
		return registered;
	}
	if (const auto initialized = check(usbd_init(context_)); !initialized) {
		UsbMessageDispatch::unlink(*this);
		started_ = false;
		return initialized;
	}

	/*
	 * A controller that reports VBUS enables itself from the message callback
	 * once VBUS actually appears. One that cannot detect it has no such event
	 * coming, so it is enabled here and simply assumes the cable is present.
	 */
	if (!options.follow_vbus || !usbd_can_detect_vbus(context_)) {
		return enable();
	}
	return {};
}

} /* namespace zest */
