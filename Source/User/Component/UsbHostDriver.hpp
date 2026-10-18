// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_USB_HOST_DRIVER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_USB_HOST_DRIVER_HPP_INCLUDED

#include "User/Component/ComponentProgram.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The USB 1.1/2.0 host group driver, the `usb` module's only behaviour: every EHCI, OHCI and UHCI
/// function the root service granted at words 30-33 and 45-56, each taken from firmware through its
/// legacy-support handoff, halted, confined in one IommuDomain with the group's DMA pages (or a
/// trusted identity domain), and driven by its library with HID boot keyboards and mice on its root
/// ports, an EHCI's full- and low-speed devices on its companions. Each controller serves one
/// request at a time; the driver serializes its devices on it and time-slices idle interrupt polls
/// among them, so several devices and composite interfaces share a controller. Interrupts are the
/// functions' (INTx on IA-64; EHCI, OHCI and UHCI have no MSI), with a timed poll as the backstop
/// and as the only source where the kernel offers none. Input reaches the Input Broker through the
/// source the start block names, readiness and `# usb:` diagnostics the root service's report
/// Endpoint. One thread serializes everything; state lives in the image's.bss and the window; no
/// allocation beyond its Frames. A step that fails at start ends the instance for manager
/// quarantine.
[[nodiscard]] Foundation::UInt64 ServeUsbHost(const System::SystemCallClient& client,
                                              const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                              ComponentClock clock) noexcept;

/// The `usb` module's entry: a block with the magic, behaviour Abi::ComponentBehaviorXhci and Abi:
/// UsbHostGroupFlag is served as ServeUsbHost; any other returns Abi::ComponentBadStartExitCode.
[[nodiscard]] Foundation::UInt64 RunUsbHostComponent(System::SystemCallTransport transport,
                                                     const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                                     ComponentClock clock) noexcept;
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_USB_HOST_DRIVER_HPP_INCLUDED
