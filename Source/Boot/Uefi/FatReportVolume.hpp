// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_FAT_REPORT_VOLUME_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_FAT_REPORT_VOLUME_HPP_INCLUDED

#include "Boot/Uefi/AcpiReportVolume.hpp"

namespace Asteria::Boot::Uefi
{
/// Page-backed FAT32 report adapter for firmware with no writable EFI file
/// protocol. Uses only Block I/O on the loaded device's logical partition.
/// Requires pre-existing EFI/ASTERIA/ACPI directories, 512-byte sectors, two
/// mirrored FATs, a clean volume, and at most 1,048,576 clusters. Never formats.
/// Serialized BSP/pre-exit only. New file chains publish after a device flush;
/// failure fences the adapter and may leak fresh clusters. Old entries survive
/// failures before publication; failures after publication may expose the new file. The owner releases all pages before returning to the boot path.
class FatReportVolume final
{
public:
    explicit FatReportVolume(BootServices& services) noexcept;
    FatReportVolume(const FatReportVolume&) = delete;
    FatReportVolume& operator=(const FatReportVolume&) = delete;
    ~FatReportVolume() noexcept;
    [[nodiscard]] IAcpiReportVolume* Open(Handle device) noexcept;
    [[nodiscard]] bool Release() noexcept;

private:
    BootServices& m_services;
    Foundation::UInt64 m_address{0U};
};
}
#endif
