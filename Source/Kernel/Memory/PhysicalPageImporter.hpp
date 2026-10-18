// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_IMPORTER_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_IMPORTER_HPP_INCLUDED

#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/PhysicalPageAllocator.hpp"
#include "Kernel/Memory/PhysicalPageImportPolicy.hpp"

namespace Asteria::Kernel::Memory
{
/// Boot-lifetime scratch, outside managed memory. Subtract intervals, never iterate physical pages.
/// An incomplete exclusion inventory refuses the import rather than silently truncating it.
class PhysicalPageImporter final
{
public:
    static constexpr Foundation::Size MaximumRanges{4096U};
    [[nodiscard]] bool Exclude(const Foundation::UInt64 start, const Foundation::UInt64 bytes) noexcept
    {
        if (bytes == 0U)
        {
            return true;
        }
        if (m_exclusionsCount == PhysicalPageImportPolicy::MaximumExclusions || bytes > ~Foundation::UInt64{0U} - start)
        {
            return false;
        }
        m_exclusions[m_exclusionsCount++] = {start, bytes};
        return true;
    }
    [[nodiscard]] bool ExcludePages(const Foundation::UInt64 start, const Foundation::UInt64 pages) noexcept
    {
        return pages <= ~Foundation::UInt64{0U} / MappingPageSizeBytes && Exclude(start, pages * MappingPageSizeBytes);
    }
    [[nodiscard]] bool ExcludeBootInfo(const Boot::BootInfo& info) noexcept
    {
        if (info.MemoryRegionCount > Boot::MaximumMemoryRegions || info.Acpi.SsdtCount > Boot::Acpi::MaximumAcpiSsdts ||
            !Boot::IsValidBootDmaExclusions(info.DmaExclusions) ||
            !Boot::IsCompleteBootDmaExclusions(info.DmaExclusions) ||
            !ExcludeNamed(info, reinterpret_cast<Foundation::UIntPointer>(&info), sizeof(info)) ||
            !ExcludeNamedPages(info, info.Inventory.PhysicalStart, info.Inventory.Pages) ||
            !ExcludeNamedPages(info, info.Files.PhysicalStart, info.Files.PageCount) ||
            !ExcludeNamed(info, info.Display.FrameBufferBase, info.Display.FrameBufferBytes) ||
            !ExcludeNamed(info, info.Kernel.Image.PhysicalStart, info.Kernel.Image.SizeBytes))
        {
            return false;
        }
        for (Foundation::Size i{0U}; i < info.DmaExclusions.Count; ++i)
        {
            const auto& range = info.DmaExclusions.Ranges[i];
            if (range.Limit == ~Foundation::UInt64{0U} || !Exclude(range.Base, range.Limit - range.Base + 1U))
            {
                return false;
            }
        }
        const Boot::BootReservation* const receipts[]{&info.Kernel.Bootstrap, &info.Kernel.RuntimePool,
                                                      &info.Kernel.Startup, &info.Kernel.Ia64Runtime,
                                                      &info.Kernel.MetadataArena};
        for (const auto* const receipt : receipts)
        {
            if (!ExcludeNamedPages(info, receipt->PhysicalStart, receipt->PageCount))
            {
                return false;
            }
        }
        for (const auto& section : info.Kernel.Image.Sections)
        {
            if (!ExcludeNamedPages(info, section.PhysicalStart, section.PageCount))
            {
                return false;
            }
        }
        const Foundation::UInt64 tables[]{info.Acpi.RsdpPhysicalAddress, info.Acpi.RootTablePhysicalAddress,
                                          info.Acpi.MadtPhysicalAddress, info.Acpi.McfgPhysicalAddress,
                                          info.Acpi.DmarPhysicalAddress, info.Kernel.RsdpPhysicalAddress,
                                          info.Acpi.Ivrs.PhysicalAddress};
        for (const auto address : tables)
        {
            if (!ExcludeTable(info, address))
            {
                return false;
            }
        }
        const Boot::BootPciResource* const devices[]{&info.Kernel.Ahci, &info.Kernel.Device, &info.Kernel.Nic};
        for (const auto* const device : devices)
        {
            if (!ExcludeNamed(info, device->BaseAddress, device->LengthBytes))
            {
                return false;
            }
        }
        const Foundation::UInt64 firmware[]{
            info.Kernel.Sal.PalProcedurePhysicalAddress, info.Kernel.Sal.SalProcedurePhysicalAddress,
            info.Kernel.Sal.SalGlobalDataPhysicalAddress, info.Kernel.Sal.CoherenceDomainInformationPhysicalAddress,
            info.Kernel.EvidencePhysicalAddress};
        for (const auto address : firmware)
        {
            if (!ExcludeTable(info, address))
            {
                return false;
            }
        }
        if (!ExcludeNamed(info, info.Acpi.Dsdt.PhysicalAddress, info.Acpi.Dsdt.Length))
        {
            return false;
        }
        for (Foundation::Size i{0U}; i < info.Acpi.SsdtCount && i < Boot::Acpi::MaximumAcpiSsdts; ++i)
        {
            if (!ExcludeNamed(info, info.Acpi.Ssdts[i].PhysicalAddress, info.Acpi.Ssdts[i].Length))
            {
                return false;
            }
        }
        return ExcludeAcpiDevices(info);
    }
    [[nodiscard]] bool Build(const Boot::BootInfo& info, const bool bootServices,
                             const BootstrapPageAllocator* residue = nullptr) noexcept
    {
        if (info.MemoryRegionCount > Boot::MaximumMemoryRegions)
        {
            return false;
        }
        for (Foundation::Size index{0U}; index < info.MemoryRegionCount; ++index)
        {
            const auto& region = info.MemoryRegions[index];
            if (residue != nullptr && region.Kind == Boot::PhysicalMemoryKind::Conventional)
            {
                if (!ExcludeUnavailable(region, *residue))
                {
                    return false;
                }
                continue;
            }
            if (!Add(region, bootServices))
            {
                return false;
            }
        }
        if (residue != nullptr)
        {
            for (Foundation::Size i{0U}; i < residue->GetFreeRangeCount(); ++i)
            {
                const auto free = residue->GetFreeRange(i);
                if (!free.HasValue())
                {
                    return false;
                }
                const auto& range = free.GetValue();
                if (!Add({range.PhysicalStart, 0U, range.PageCount, range.Attributes, range.FirmwareType,
                          Boot::PhysicalMemoryKind::Conventional},
                         false))
                {
                    return false;
                }
            }
        }
        // Bounded insertion sort, then union adjacent admitted intervals.
        for (Foundation::Size i{1U}; i < m_count; ++i)
        {
            const auto value = m_ranges[i];
            Foundation::Size position{i};
            while (position != 0U && m_ranges[position - 1U].Start.GetValue() > value.Start.GetValue())
            {
                m_ranges[position] = m_ranges[position - 1U];
                --position;
            }
            m_ranges[position] = value;
        }
        Foundation::Size written{0U};
        for (Foundation::Size i{0U}; i < m_count; ++i)
        {
            const auto value = m_ranges[i];
            if (written != 0U)
            {
                auto& last = m_ranges[written - 1U];
                const auto end = last.Start.GetValue() + last.Pages * MappingPageSizeBytes;
                const auto nextEnd = value.Start.GetValue() + value.Pages * MappingPageSizeBytes;
                if (value.Start.GetValue() <= end)
                {
                    if (nextEnd > end)
                    {
                        last.Pages = (nextEnd - last.Start.GetValue()) / MappingPageSizeBytes;
                    }
                    continue;
                }
            }
            m_ranges[written++] = value;
        }
        m_count = written;
        return true;
    }
    void Record(PhysicalPageImportEvidence& evidence, const Boot::BootInfo& info, const bool services) const noexcept
    {
        evidence.MapPhysical = reinterpret_cast<Foundation::UIntPointer>(&info.MemoryRegions[0U]);
        evidence.MapRegions = info.MemoryRegionCount;
        evidence.BootServicesAllowed = services ? 1U : 0U;
        evidence.Exclusions = m_exclusionsCount;
        evidence.BootInfoFaultRanges = m_bootInfoFaultRanges;
        for (Foundation::Size i{0U}; i < m_exclusionsCount; ++i)
        {
            evidence.ExclusionStart[i] = m_exclusions[i].Start;
            evidence.ExclusionBytes[i] = m_exclusions[i].Bytes;
        }
    }
    [[nodiscard]] const PhysicalPageImportRange* Ranges() const noexcept
    {
        return m_ranges;
    }
    [[nodiscard]] Foundation::Size Count() const noexcept
    {
        return m_count;
    }

private:
    [[nodiscard]] bool ExcludeNamed(const Boot::BootInfo& info, const Foundation::UInt64 start,
                                    const Foundation::UInt64 bytes) noexcept
    {
        if (!Exclude(start, bytes))
        {
            return false;
        }
        if (bytes == 0U)
        {
            return true;
        }
        for (Foundation::Size i{0U}; i < info.MemoryRegionCount; ++i)
        {
            const auto& region = info.MemoryRegions[i];
            if ((region.FirmwareType == 3U || region.FirmwareType == 4U) &&
                region.PageCount <= (~Foundation::UInt64{0U} - region.PhysicalStart) / MappingPageSizeBytes &&
                start < region.PhysicalStart + region.PageCount * MappingPageSizeBytes &&
                region.PhysicalStart < start + bytes)
            {
                ++m_bootInfoFaultRanges;
                break;
            }
        }
        return true;
    }
    [[nodiscard]] bool ExcludeNamedPages(const Boot::BootInfo& info, const Foundation::UInt64 start,
                                         const Foundation::UInt64 pages) noexcept
    {
        return pages <= ~Foundation::UInt64{0U} / MappingPageSizeBytes &&
               ExcludeNamed(info, start, pages * MappingPageSizeBytes);
    }
    [[nodiscard]] bool ExcludeAcpiDevices(const Boot::BootInfo& info) noexcept
    {
        const auto& acpi = info.Acpi;
        if (acpi.IoControllerCount > Boot::Acpi::MaximumAcpiIoControllers ||
            acpi.PciConfigurationRegionCount > Boot::Acpi::MaximumAcpiPciConfigurationRegions ||
            acpi.IommuHandoffCount > Boot::Acpi::MaximumAcpiIommuHandoffs)
        {
            return false;
        }
        const Foundation::UInt64 pages[]{acpi.LocalControllerPhysicalAddress, acpi.HpetAddress};
        for (const auto address : pages)
        {
            if (address != 0U && !ExcludeNamed(info, address, MappingPageSizeBytes))
            {
                return false;
            }
        }
        if (acpi.PmTimerAddress != 0U && acpi.PmTimerSpace == 0U &&
            !ExcludeNamed(info, acpi.PmTimerAddress, sizeof(Foundation::UInt32)))
        {
            return false;
        }
        for (Foundation::Size i{0U}; i < acpi.IoControllerCount; ++i)
        {
            if (!ExcludeNamed(info, acpi.IoControllers[i].PhysicalAddress, MappingPageSizeBytes))
            {
                return false;
            }
        }
        for (Foundation::Size i{0U}; i < acpi.PciConfigurationRegionCount; ++i)
        {
            const auto& window = acpi.PciConfigurationRegions[i];
            if (window.LastBus < window.FirstBus ||
                !ExcludeNamed(info, window.BaseAddress,
                              (static_cast<Foundation::UInt64>(window.LastBus - window.FirstBus) + 1U) << 20U))
            {
                return false;
            }
        }
        for (Foundation::Size i{0U}; i < acpi.IommuHandoffCount; ++i)
        {
            const auto& unit = acpi.IommuHandoffs[i];
            Foundation::UInt64 bytes{unit.Kind == 2U ? 32U * 1024U : MappingPageSizeBytes};
            for (Foundation::Size n{0U}; n < acpi.DmaRemappingUnitCount && n < Boot::Acpi::MaximumAcpiDmaRemappingUnits;
                 ++n)
            {
                const auto& drhd = acpi.DmaRemappingUnits[n];
                if (drhd.RegisterBaseAddress == unit.RegisterBase && drhd.Size < 52U)
                {
                    bytes = MappingPageSizeBytes << drhd.Size;
                }
            }
            if (!ExcludeNamed(info, unit.RegisterBase, bytes))
            {
                return false;
            }
        }
        if (info.DebugPort.Address != 0U &&
            info.DebugPort.AddressSpace == static_cast<Foundation::UInt8>(Boot::BootDebugPortSpace::SystemMemory) &&
            !ExcludeNamed(info, info.DebugPort.Address,
                          Boot::BootDebugPortRegisterCount * info.DebugPort.RegisterStrideBytes))
        {
            return false;
        }
        return true;
    }
    [[nodiscard]] bool ExcludeUnavailable(const Boot::PhysicalMemoryRegion& region,
                                          const BootstrapPageAllocator& residue) noexcept
    {
        const auto first = region.PhysicalStart + (region.PhysicalStart == 0U ? MappingPageSizeBytes : 0U);
        if (!PhysicalPageImportPolicy::Allows(region, PhysicalAddress{first}, false, nullptr, 0U))
        {
            return true;
        }
        const auto end = region.PhysicalStart + region.PageCount * MappingPageSizeBytes;
        auto cursor = first;
        for (Foundation::Size step{0U}; cursor < end && step <= 2U * Boot::MaximumMemoryRegions; ++step)
        {
            auto next = end;
            auto covered = cursor;
            for (Foundation::Size i{0U}; i < residue.GetFreeRangeCount(); ++i)
            {
                const auto result = residue.GetFreeRange(i);
                if (!result.HasValue())
                {
                    return false;
                }
                const auto& free = result.GetValue();
                const auto finish = free.PhysicalStart + free.PageCount * MappingPageSizeBytes;
                if (free.PhysicalStart <= cursor && finish > covered)
                {
                    covered = finish;
                }
                if (free.PhysicalStart > cursor && free.PhysicalStart < next)
                {
                    next = free.PhysicalStart;
                }
            }
            if (covered > cursor)
            {
                cursor = covered;
                continue;
            }
            if (!Exclude(cursor, next - cursor))
            {
                return false;
            }
            cursor = next;
        }
        return cursor >= end;
    }
    [[nodiscard]] bool ExcludeTable(const Boot::BootInfo& info, const Foundation::UInt64 address) noexcept
    {
        if (address == 0U)
        {
            return true;
        }
        // A pointer without a retained length does not authorize guessing its
        // extent. Keep its containing firmware descriptor out conservatively.
        for (Foundation::Size i{0U}; i < info.MemoryRegionCount; ++i)
        {
            const auto& region = info.MemoryRegions[i];
            if (address >= region.PhysicalStart &&
                (address - region.PhysicalStart) / MappingPageSizeBytes < region.PageCount)
            {
                return ExcludeNamedPages(info, region.PhysicalStart, region.PageCount);
            }
        }
        return ExcludeNamed(info, address, MappingPageSizeBytes);
    }
    [[nodiscard]] bool Add(const Boot::PhysicalMemoryRegion& region, const bool services) noexcept
    {
        const auto start = region.PhysicalStart + (region.PhysicalStart == 0U ? MappingPageSizeBytes : 0U);
        if (!PhysicalPageImportPolicy::Allows(region, PhysicalAddress{start}, services, nullptr, 0U))
        {
            return true;
        }
        const auto end = region.PhysicalStart + region.PageCount * MappingPageSizeBytes;
        auto cursor = start;
        for (Foundation::Size step{0U}; cursor < end && step <= 2U * PhysicalPageImportPolicy::MaximumExclusions;
             ++step)
        {
            auto next = end;
            auto skip = cursor;
            for (Foundation::Size i{0U}; i < m_exclusionsCount; ++i)
            {
                const auto& exclusion = m_exclusions[i];
                const auto first = exclusion.Start & ~(MappingPageSizeBytes - 1U);
                const auto lastPage = (exclusion.Start + exclusion.Bytes - 1U) / MappingPageSizeBytes + 1U;
                const auto last = lastPage > ~Foundation::UInt64{0U} / MappingPageSizeBytes
                                      ? ~Foundation::UInt64{0U}
                                      : lastPage * MappingPageSizeBytes;
                if (first <= cursor && last > skip)
                {
                    skip = last;
                }
                if (first > cursor && first < next)
                {
                    next = first;
                }
            }
            if (skip > cursor)
            {
                cursor = skip;
                continue;
            }
            if (m_count == MaximumRanges)
            {
                return false;
            }
            m_ranges[m_count++] = {PhysicalAddress{cursor}, (next - cursor) / MappingPageSizeBytes};
            cursor = next;
        }
        return cursor >= end;
    }
    PhysicalPageImportExclusion m_exclusions[PhysicalPageImportPolicy::MaximumExclusions]{};
    PhysicalPageImportRange m_ranges[MaximumRanges]{};
    Foundation::UInt64 m_bootInfoFaultRanges{0U};
    Foundation::Size m_count{0U};
    Foundation::Size m_exclusionsCount{0U};
};
} // Namespace Asteria::Kernel::Memory
#endif
