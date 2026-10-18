// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_BOOT_REPORT_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_BOOT_REPORT_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Platform/Firmware/AhciPciResourceCapture.hpp"

namespace Asteria::Boot::Uefi
{
/// Best-effort firmware-only report on the loaded image's volume. BSP only,
/// before the first ExitBootServices attempt. Uses bounded static scratch,
/// releases firmware handle arrays, and never changes boot success policy.
[[nodiscard]] BootInventoryLocation GetBootInventoryLocation() noexcept;

/// trust is the verified allowlist; the report lists each entry, its module's verdict and the functions it
/// matches. Whether an entry applies is the kernel's later decision (an IOMMU that works leaves it unused).
void WriteBootReport(Handle image, SystemTable& system, const BootDisplay& display, const BootFileRegion& files,
                     const BootTrustPolicy& trust) noexcept;
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_BOOT_REPORT_HPP_INCLUDED
