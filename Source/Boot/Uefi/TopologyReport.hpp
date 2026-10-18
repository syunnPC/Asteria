// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_TOPOLOGY_REPORT_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_TOPOLOGY_REPORT_HPP_INCLUDED

#include "Inventory/ReportText.hpp"
#include "Boot/BootInfo.hpp"
#include "Boot/Uefi/UefiTypes.hpp"

namespace Asteria::Boot::Uefi
{
/// BSP-only firmware snapshot, valid only until ExitBootServices.
[[nodiscard]] const BootInfo& GetReportSnapshot() noexcept;

/// Snapshot before report file I/O, not the final handoff memory map.
void ReportBootRouting(Inventory::ReportText& text, SystemTable& system,
                       const Abi::DeviceInventory& inventory) noexcept;

void ReportTopology(Inventory::ReportText& text, SystemTable& system, Abi::DeviceInventory& inventory) noexcept;
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_TOPOLOGY_REPORT_HPP_INCLUDED
