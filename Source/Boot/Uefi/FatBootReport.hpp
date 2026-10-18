// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_FAT_BOOT_REPORT_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_FAT_BOOT_REPORT_HPP_INCLUDED

#include "Boot/Uefi/FatReportVolume.hpp"

namespace Asteria::Boot::Uefi
{
/// Retries the report/dump/facts sequence on the loaded FAT32 logical partition
/// when no writable firmware file protocol was available. Buffers are borrowed
/// from the boot report; temporary pages are released before the final map.
[[nodiscard]] bool WriteFatBootReport(Handle device, BootServices& services, const BootInfo& snapshot, char* report,
                                      Foundation::Size capacity, Foundation::Size& bytes) noexcept;
}
#endif
