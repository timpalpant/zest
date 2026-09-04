/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file
 * Bringing up a USB device stack.
 *
 * The `USBD_*_DEFINE` macros stay in the application: they place objects in
 * linker sections and take string literals, and nothing in C++ improves on
 * that. What this owns is the sequence *after* them — add each descriptor, add
 * the configuration, register the classes, set the device code triple, hook the
 * message callback, initialize, and enable — which is a fixed order of nine
 * calls where an early return on any one leaves the stack half-built, and where
 * the last step depends on whether the controller can see VBUS at all.
 */

#include <zest/error.hpp>

#include <zephyr/usb/usbd.h>

#include <cstdint>
#include <optional>

namespace zest
{

/**
 * The USB device class, subclass and protocol reported at device level.
 *
 * A device whose function spans more than one interface — CDC ACM, which pairs
 * a control interface with a data one — must report the interface-association
 * class here rather than claim its function's class, or a host binds the first
 * interface and never sees the second.
 */
struct UsbCodeTriple {
	std::uint8_t base_class{};
	std::uint8_t subclass{};
	std::uint8_t protocol{};

	/** The triple for a device whose interfaces are grouped by an IAD. */
	[[nodiscard]] static constexpr UsbCodeTriple interface_association() noexcept
	{
		return {USB_BCC_MISCELLANEOUS, 0x02U, 0x01U};
	}
};

/**
 * String descriptors, as produced by the `USBD_DESC_*_DEFINE` macros.
 *
 * All optional. A device with no language descriptor simply reports no strings,
 * which is legal and makes it harder to tell two boards apart on one host — so
 * a serial number is worth having even when nothing else is.
 */
struct UsbDescriptors {
	usbd_desc_node *language{};
	usbd_desc_node *manufacturer{};
	usbd_desc_node *product{};
	usbd_desc_node *serial_number{};
};

/** How the device presents itself and how it is brought up. */
struct UsbDeviceOptions {
	usbd_speed speed{USBD_SPEED_FS};
	/** Configuration number to register classes against. */
	std::uint8_t configuration_number{1U};
	/** Reported in the configuration descriptor's attributes. */
	bool self_powered{false};
	/** Left unset, the device reports no class of its own. */
	std::optional<UsbCodeTriple> code_triple{};
	/** Class instances to leave unregistered, `nullptr`-terminated. */
	const char *const *class_blocklist{nullptr};
	/**
	 * Enable and disable the controller as VBUS comes and goes.
	 *
	 * Only meaningful where the controller reports VBUS state. Where it does
	 * not, @ref UsbDevice::start enables unconditionally, because there is no
	 * event that would ever do it later.
	 */
	bool follow_vbus{true};
};

/**
 * A USB device stack built around one `usbd_context`.
 *
 * Scoped to the object: the destructor disables the controller, so a device
 * brought up in a function that returns does not leave a half-live stack behind.
 */
class UsbDevice
{
      public:
	/** Take @p context from `USBD_DEVICE_DEFINE`. */
	constexpr explicit UsbDevice(usbd_context *context) noexcept : context_{context}
	{
	}

	UsbDevice(const UsbDevice &) = delete;
	UsbDevice &operator=(const UsbDevice &) = delete;

	~UsbDevice() noexcept;

	/**
	 * Build the stack and bring it up.
	 *
	 * Enumeration is asynchronous: this returns once the controller is
	 * enabled, or once it is armed to enable when VBUS appears — not once a
	 * host has attached.
	 */
	[[nodiscard]] Result<> start(const UsbDescriptors &descriptors,
				     usbd_config_node &configuration,
				     const UsbDeviceOptions &options = {}) noexcept;

	[[nodiscard]] Result<> enable() noexcept;
	[[nodiscard]] Result<> disable() noexcept;

	/** Whether the controller reports VBUS state. */
	[[nodiscard]] bool can_detect_vbus() const noexcept;

	/** Whether the controller is enabled right now. */
	[[nodiscard]] bool enabled() const noexcept
	{
		return enabled_;
	}

	[[nodiscard]] constexpr usbd_context *native_context() const noexcept
	{
		return context_;
	}

      private:
	/*
	 * Zephyr's message callback is passed the usbd_context and nothing else,
	 * so the device is found by matching on it. One registration serves every
	 * UsbDevice in the build, the same way zest/ble.hpp handles the connection
	 * callbacks.
	 */
	friend struct UsbMessageDispatch;

	usbd_context *context_;
	UsbDevice *next_{nullptr};
	bool follow_vbus_{false};
	bool started_{false};
	bool enabled_{false};
};

} /* namespace zest */
