// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_IMPORT_POLICY_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_IMPORT_POLICY_HPP_INCLUDED
#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"

namespace Asteria::Kernel::Memory
{
struct PhysicalPageImportExclusion
{
    Foundation::UInt64 Start;
    Foundation::UInt64 Bytes;
};

/// Physical-page admission follows UEFI 2.11 table 7.10: write-back Conventional
/// memory and, when bootServicesAllowed is set, released BootServices memory.
/// Runtime/special-purpose memory, page zero and every supplied exclusion are
/// rejected. The caller supplies handoff/receipt exclusions and must establish
/// BootServices DMA confinement before allowing those pages.
class PhysicalPageImportPolicy final
{
public:
    static constexpr Foundation::Size MaximumExclusions{256U};
    [[nodiscard]] static constexpr bool Allows(const Boot::PhysicalMemoryRegion& region, const PhysicalAddress page,
                                               const bool bootServicesAllowed,
                                               const PhysicalPageImportExclusion* exclusions,
                                               const Foundation::Size exclusionCount) noexcept
    {
        constexpr Foundation::UInt64 WriteBack{8U};
        constexpr Foundation::UInt64 Runtime{Foundation::UInt64{1U} << 63U};
        constexpr Foundation::UInt64 SpecialPurpose{0x0004'0000U};
        const Foundation::UInt64 address = page.GetValue();
        const bool conventional = region.FirmwareType == 7U && region.Kind == Boot::PhysicalMemoryKind::Conventional;
        const bool services = (region.FirmwareType == 3U || region.FirmwareType == 4U) &&
                              region.Kind == Boot::PhysicalMemoryKind::BootServices && bootServicesAllowed;
        if ((!conventional && !services) || (region.PageCount == 0U) || (address == 0U) ||
            ((address % MappingPageSizeBytes) != 0U) || ((region.PhysicalStart % MappingPageSizeBytes) != 0U) ||
            (region.PageCount > ((~Foundation::UInt64{0U} - region.PhysicalStart) / MappingPageSizeBytes)) ||
            ((region.Attributes & WriteBack) == 0U) || ((region.Attributes & (Runtime | SpecialPurpose)) != 0U) ||
            (exclusionCount > MaximumExclusions) || ((exclusionCount != 0U) && exclusions == nullptr) ||
            (address < region.PhysicalStart) ||
            ((address - region.PhysicalStart) / MappingPageSizeBytes >= region.PageCount))
        {
            return false;
        }
        for (Foundation::Size index{0U}; index < exclusionCount; ++index)
        {
            const auto& excluded = exclusions[index];
            if ((excluded.Bytes == 0U) || (excluded.Bytes > (~Foundation::UInt64{0U} - excluded.Start)))
            {
                return false;
            }
            // Outward rounding without an overflowing rounded exclusive end.
            if (address / MappingPageSizeBytes <= (excluded.Start + excluded.Bytes - 1U) / MappingPageSizeBytes &&
                excluded.Start / MappingPageSizeBytes <= address / MappingPageSizeBytes)
            {
                return false;
            }
        }
        return true;
    }
};
} // Namespace Asteria::Kernel::Memory
#endif
