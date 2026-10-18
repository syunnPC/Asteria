// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_DEVICE_INVENTORY_HPP_INCLUDED
#define ASTERIA_USER_PAGER_DEVICE_INVENTORY_HPP_INCLUDED

#include "Inventory/ReportText.hpp"
#include "User/Pager/RootContext.hpp"

namespace Asteria::User::Pager
{
/// Reads the boot's immutable Frame, formats and prints it once, then unmaps.
/// The resulting counted ASCII text remains in the pager's static storage.
void ReadDeviceInventory(ScenarioContext& context) noexcept;
[[nodiscard]] Foundation::UInt64 XhciDmaPageQuota(Foundation::Size controller) noexcept;
/// `# init: xhci failed, continuing <bdf> step=... err=0x.. code=0x...` (or `usb` for
/// the USB host group, without an address); `...: killed` for a driver its
/// supervisor killed (Abi::KilledExitCode).
void ReportXhciFailure(ScenarioContext& context, Foundation::Size device, Foundation::UInt64 code) noexcept;
/// Writes which USB input driver device names: `usb` for the USB host group
/// (ComponentUsbHostGroup or ComponentNoDevice), `xhci` and the controller's
/// segment, bus, device and function for an xHCI device (Abi::PagerDeviceXhci
/// onward).
void AppendInputDriver(Inventory::ReportText& text, Foundation::Size device) noexcept;
/// The PCI programming interface of the USB host table's controller slot
/// (0x00 UHCI, 0x10 OHCI, 0x20 EHCI) as the inventory names it; 0xFF where
/// the inventory names none.
[[nodiscard]] Foundation::UInt8 UsbHostInterface(Foundation::Size slot) noexcept;
} // namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_DEVICE_INVENTORY_HPP_INCLUDED
