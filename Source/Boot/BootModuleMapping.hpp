// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_MODULE_MAPPING_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_MODULE_MAPPING_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Boot/BootInfo.hpp"

namespace Asteria::Boot
{
/// Validates a page-isolated LoaderData slice, its file extents and provenance.
/// No allocation, copying or mapping; callable before translation takeover.
[[nodiscard]] inline bool ValidateBootModules(const BootInfo& info) noexcept
{
    const BootFileRegion& region = info.Files;
    constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};
    if (region.ModuleCount == 0U || region.ModuleCount > BootModuleMaximumCount || region.PhysicalStart == 0U ||
        (region.PhysicalStart & 4095U) != 0U || region.PageCount == 0U || region.PageCount > Maximum / 4096U ||
        region.PhysicalStart > Maximum - region.PageCount * 4096U || region.ModuleOffset < BootManifestMaximumBytes ||
        (region.ModuleOffset & 4095U) != 0U || region.ModuleBytes == 0U || (region.ModuleBytes & 4095U) != 0U ||
        region.ModuleBytes > Abi::PagerModuleMaximumBytes || region.ModuleOffset > region.PageCount * 4096U ||
        region.ModuleBytes > region.PageCount * 4096U - region.ModuleOffset)
    {
        return false;
    }
    for (const BootFileRecord& file : region.Files)
    {
        if (file.SizeBytes != 0U &&
            (file.Offset > region.ModuleOffset || file.SizeBytes > region.ModuleOffset - file.Offset))
        {
            return false;
        }
    }
    Foundation::UInt64 end{region.ModuleOffset + 16U + region.ModuleCount * 32U};
    for (Foundation::Size index{0U}; index < region.ModuleCount; ++index)
    {
        const BootFileRecord& file = region.Modules[index].File;
        if ((file.Offset & 15U) != 0U || file.Offset < end || file.SizeBytes == 0U ||
            file.Offset > region.ModuleOffset + region.ModuleBytes ||
            file.SizeBytes > region.ModuleOffset + region.ModuleBytes - file.Offset)
        {
            return false;
        }
        end = file.Offset + file.SizeBytes;
    }
    if (info.MemoryRegionCount > MaximumMemoryRegions)
    {
        return false;
    }
    const Foundation::UInt64 start{region.PhysicalStart + region.ModuleOffset};
    for (Foundation::Size index{0U}; index < info.MemoryRegionCount; ++index)
    {
        const PhysicalMemoryRegion& memory = info.MemoryRegions[index];
        if (memory.PhysicalStart <= start && memory.PageCount <= Maximum / 4096U &&
            start - memory.PhysicalStart <= memory.PageCount * 4096U &&
            region.ModuleBytes <= memory.PageCount * 4096U - (start - memory.PhysicalStart))
        {
            // UEFI LoaderData, write-back, no runtime, write-protect or read-protect.
            return memory.Kind == PhysicalMemoryKind::Loader && memory.FirmwareType == 2U &&
                   (memory.Attributes & 8U) != 0U &&
                   (memory.Attributes & (0x8000'0000'0000'0000ULL | 0x1000U | 0x2000U)) == 0U;
        }
    }
    return false;
}
} // namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_MODULE_MAPPING_HPP_INCLUDED
