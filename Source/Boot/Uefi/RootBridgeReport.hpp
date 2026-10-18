// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_ROOT_BRIDGE_REPORT_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_ROOT_BRIDGE_REPORT_HPP_INCLUDED

#include "Boot/Uefi/UefiTypes.hpp"
#include "Inventory/ReportText.hpp"

namespace Asteria::Boot::Uefi
{
/// PCI root bridges expose functions without an individual PCI I/O handle.
void ReportRootBridgeFunctions(Inventory::ReportText& text, BootServices& services,
                               Abi::DeviceInventory& inventory) noexcept;
}
#endif
