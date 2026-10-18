// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_PERIPHERAL_REPORT_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_PERIPHERAL_REPORT_HPP_INCLUDED

#include "Inventory/ReportText.hpp"
#include "Boot/Uefi/UefiTypes.hpp"

namespace Asteria::Boot::Uefi
{
/// Firmware protocol enumeration, BSP only before ExitBootServices. Read-only
/// device operations; all firmware-allocated handle buffers are released.
void ReportPeripherals(Inventory::ReportText& text, BootServices& services) noexcept;
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_PERIPHERAL_REPORT_HPP_INCLUDED
