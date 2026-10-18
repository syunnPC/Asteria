// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_XHCI_BOOT_REPORT_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_XHCI_BOOT_REPORT_HPP_INCLUDED

#include "Boot/Uefi/PciIoProtocol.hpp"
#include "Inventory/XhciReport.hpp"

namespace Asteria::Boot::Uefi
{
/// Firmware-only identity-mapped, read-only BAR snapshot. Requires the PCI
/// command MSE bit and an authoritative firmware BAR descriptor. Never probes.
[[nodiscard]] Foundation::UInt64 ReportXhci(Inventory::ReportText& text, PciIoProtocol& protocol,
                                            BootServices& services, const Inventory::PciFunction& function,
                                            Foundation::UInt32 command, Foundation::UInt16& scratchpads) noexcept;
/// The firmware's decoded extent of an EHCI's or OHCI's memory BAR 0 (class
/// 0C/03/20 or 0C/03/10), so the kernel never sizes the BAR of a controller
/// firmware may still use;
/// zero for another function or when the firmware describes none. Requires
/// the PCI command MSE bit. Never probes and touches no register.
[[nodiscard]] Foundation::UInt64 CaptureUsbHostBar(PciIoProtocol& protocol, BootServices& services,
                                                   const Inventory::PciFunction& function,
                                                   Foundation::UInt32 command) noexcept;
}
#endif // ASTERIA_BOOT_UEFI_XHCI_BOOT_REPORT_HPP_INCLUDED
