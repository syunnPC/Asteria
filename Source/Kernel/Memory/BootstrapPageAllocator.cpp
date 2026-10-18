// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/BootstrapPageAllocator.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumDescriptorPageCount{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 MaximumPhysicalPageCount{MaximumDescriptorPageCount - 1U};
constexpr Foundation::UInt64 PhysicalPageOffsetMask{PhysicalPageSizeBytes - 1U};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{1ULL << 63U};
constexpr Foundation::UInt32 MinimumFirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 ExpectedFirmwareDescriptorVersion{1U};
constexpr Foundation::UInt32 MaximumStandardMemoryType{15U};
constexpr Foundation::UInt32 MinimumOemMemoryType{0x7000'0000U};

[[nodiscard]] Boot::PhysicalMemoryKind ConvertFirmwareMemoryType(const Foundation::UInt32 firmwareType) noexcept
{
    switch (firmwareType)
    {
    case 1U:
    case 2U:
        return Boot::PhysicalMemoryKind::Loader;
    case 3U:
    case 4U:
        return Boot::PhysicalMemoryKind::BootServices;
    case 5U:
    case 6U:
        return Boot::PhysicalMemoryKind::RuntimeServices;
    case 7U:
        return Boot::PhysicalMemoryKind::Conventional;
    case 8U:
        return Boot::PhysicalMemoryKind::Unusable;
    case 9U:
        return Boot::PhysicalMemoryKind::AcpiReclaimable;
    case 10U:
        return Boot::PhysicalMemoryKind::AcpiNonVolatile;
    case 11U:
    case 12U:
        return Boot::PhysicalMemoryKind::MemoryMappedIo;
    case 13U:
        return Boot::PhysicalMemoryKind::PalCode;
    case 14U:
        return Boot::PhysicalMemoryKind::Persistent;
    case 15U:
        return Boot::PhysicalMemoryKind::Unaccepted;
    case 0U:
    default:
        return Boot::PhysicalMemoryKind::Reserved;
    }
}

[[nodiscard]] bool IsFirmwareMemoryTypeValid(const Foundation::UInt32 firmwareType) noexcept
{
    return (firmwareType <= MaximumStandardMemoryType) || (firmwareType >= MinimumOemMemoryType);
}

[[nodiscard]] bool TryGetPageRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pageCount,
                                      Foundation::UInt64& end) noexcept
{
    if ((pageCount == 0U) || (pageCount > MaximumDescriptorPageCount) || ((start & PhysicalPageOffsetMask) != 0U))
    {
        return false;
    }
    if (pageCount == MaximumDescriptorPageCount)
    {
        if (start != 0U)
        {
            return false;
        }
        end = MaximumUInt64;
        return true;
    }

    const Foundation::UInt64 lastByteOffset = (pageCount << 12U) - 1U;
    if (start > (MaximumUInt64 - lastByteOffset))
    {
        return false;
    }
    end = start + lastByteOffset;
    return true;
}

[[nodiscard]] bool TryGetRegionEnd(const Boot::PhysicalMemoryRegion& region, Foundation::UInt64& physicalEnd) noexcept
{
    if (!TryGetPageRangeEnd(region.PhysicalStart, region.PageCount, physicalEnd))
    {
        return false;
    }
    Foundation::UInt64 virtualEnd{0U};
    return (region.VirtualStart == 0U) || TryGetPageRangeEnd(region.VirtualStart, region.PageCount, virtualEnd);
}

[[nodiscard]] bool IsBootInfoValid(const Boot::BootInfo& bootInfo) noexcept
{
    if ((bootInfo.Signature != Boot::BootInfoSignature) || (bootInfo.Version != Boot::BootInfoVersion) ||
        (bootInfo.SizeBytes != sizeof(Boot::BootInfo)) || (bootInfo.MemoryRegionCount == 0U) ||
        (bootInfo.MemoryRegionCount > Boot::MaximumMemoryRegions) ||
        (bootInfo.FirmwareDescriptorVersion != ExpectedFirmwareDescriptorVersion) ||
        (bootInfo.FirmwareDescriptorSize < MinimumFirmwareDescriptorSize) || (bootInfo.Reserved != 0U))
    {
        return false;
    }

    const Foundation::UInt64 expectedMapSize = static_cast<Foundation::UInt64>(bootInfo.MemoryRegionCount) *
                                               static_cast<Foundation::UInt64>(bootInfo.FirmwareDescriptorSize);
    if (bootInfo.FirmwareMemoryMapSizeBytes != expectedMapSize)
    {
        return false;
    }

    Foundation::UInt64 previousEnd{0U};
    const Foundation::Size regionCount = static_cast<Foundation::Size>(bootInfo.MemoryRegionCount);
    for (Foundation::Size index{0U}; index < regionCount; ++index)
    {
        const Boot::PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        Foundation::UInt64 regionEnd{0U};
        if (!TryGetRegionEnd(region, regionEnd) || !IsFirmwareMemoryTypeValid(region.FirmwareType) ||
            (region.Kind != ConvertFirmwareMemoryType(region.FirmwareType)) ||
            ((region.Kind == Boot::PhysicalMemoryKind::Conventional) &&
             ((region.Attributes & FirmwareMemoryRuntimeAttribute) != 0U)))
        {
            return false;
        }

        if ((index != 0U) && (region.PhysicalStart <= previousEnd))
        {
            return false;
        }
        previousEnd = regionEnd;
    }
    return true;
}

[[nodiscard]] PhysicalPageRange MakeRange(const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount,
                                          const Foundation::UInt64 attributes,
                                          const Foundation::UInt32 firmwareType) noexcept
{
    return PhysicalPageRange{
        .PhysicalStart = physicalStart,
        .PageCount = pageCount,
        .Attributes = attributes,
        .FirmwareType = firmwareType,
        .Reserved = 0U,
    };
}
} // namespace

BootstrapPageAllocatorError BootstrapPageAllocator::Initialize(const Boot::BootInfo& bootInfo) noexcept
{
    if (m_initialized)
    {
        return BootstrapPageAllocatorError::AlreadyInitialized;
    }
    if (!IsBootInfoValid(bootInfo))
    {
        return BootstrapPageAllocatorError::InvalidBootInfo;
    }

    m_rangeCount = 0U;
    m_freePageCount = 0U;
    m_allocatedPageCount = 0U;
    const Foundation::Size regionCount = static_cast<Foundation::Size>(bootInfo.MemoryRegionCount);
    for (Foundation::Size index{0U}; index < regionCount; ++index)
    {
        const Boot::PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        if (region.Kind != Boot::PhysicalMemoryKind::Conventional)
        {
            continue;
        }

        Foundation::UInt64 physicalStart = region.PhysicalStart;
        Foundation::UInt64 pageCount = region.PageCount;
        if (physicalStart == 0U)
        {
            physicalStart = PhysicalPageSizeBytes;
            --pageCount;
            if (pageCount == 0U)
            {
                continue;
            }
        }

        bool merged{false};
        if (m_rangeCount != 0U)
        {
            PhysicalPageRange& previous = m_ranges[m_rangeCount - 1U];
            Foundation::UInt64 previousEnd{0U};
            const bool isAdjacent = TryGetPageRangeEnd(previous.PhysicalStart, previous.PageCount, previousEnd) &&
                                    (previousEnd != MaximumUInt64) && ((previousEnd + 1U) == physicalStart);
            if (isAdjacent && (previous.Attributes == region.Attributes) &&
                (previous.FirmwareType == region.FirmwareType))
            {
                previous.PageCount += pageCount;
                merged = true;
            }
        }

        if (!merged)
        {
            if (m_rangeCount >= MaximumFreePhysicalRanges)
            {
                m_rangeCount = 0U;
                m_freePageCount = 0U;
                return BootstrapPageAllocatorError::RangeCapacityExceeded;
            }
            m_ranges[m_rangeCount] = MakeRange(physicalStart, pageCount, region.Attributes, region.FirmwareType);
            ++m_rangeCount;
        }
        m_freePageCount += pageCount;
    }

    if (m_rangeCount == 0U)
    {
        m_freePageCount = 0U;
        return BootstrapPageAllocatorError::NoUsableMemory;
    }

    m_initialized = true;
    return BootstrapPageAllocatorError::None;
}

Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
BootstrapPageAllocator::Allocate(const Foundation::UInt64 pageCount, const Foundation::UInt64 alignmentPages) noexcept
{
    return AllocateLimited(pageCount, alignmentPages, 0U);
}

Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
BootstrapPageAllocator::AllocateBelow(const Foundation::UInt64 pageCount, const Foundation::UInt64 alignmentPages,
                                      const Foundation::UInt64 endLimit) noexcept
{
    // A limit below one page admits nothing; one beyond the physical page
    // space limits nothing.
    if (endLimit < PhysicalPageSizeBytes)
    {
        return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Failure(
            BootstrapPageAllocatorError::InsufficientMemory);
    }
    return AllocateLimited(pageCount, alignmentPages, endLimit >> 12U);
}

Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
BootstrapPageAllocator::AllocateLimited(const Foundation::UInt64 pageCount, const Foundation::UInt64 alignmentPages,
                                        const Foundation::UInt64 endPageLimit) noexcept
{
    if (!m_initialized)
    {
        return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Failure(
            BootstrapPageAllocatorError::NotInitialized);
    }
    if ((pageCount == 0U) || (pageCount > MaximumPhysicalPageCount) || (alignmentPages == 0U) ||
        (alignmentPages > MaximumPhysicalPageCount) || ((alignmentPages & (alignmentPages - 1U)) != 0U))
    {
        return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Failure(
            BootstrapPageAllocatorError::InvalidRequest);
    }

    const Foundation::UInt64 alignmentMask = alignmentPages - 1U;
    for (Foundation::Size index{0U}; index < m_rangeCount; ++index)
    {
        const PhysicalPageRange current = m_ranges[index];
        const Foundation::UInt64 startPage = current.PhysicalStart >> 12U;
        const Foundation::UInt64 endPage = startPage + current.PageCount;
        if (startPage > (MaximumPhysicalPageCount - alignmentMask))
        {
            continue;
        }

        const Foundation::UInt64 allocationStartPage = (startPage + alignmentMask) & ~alignmentMask;
        if ((allocationStartPage > endPage) || (pageCount > (endPage - allocationStartPage)))
        {
            continue;
        }
        const Foundation::UInt64 allocationEndPage = allocationStartPage + pageCount;
        if ((endPageLimit != 0U) && (allocationEndPage > endPageLimit))
        {
            continue;
        }
        const Foundation::UInt64 leadingPages = allocationStartPage - startPage;
        const Foundation::UInt64 trailingPages = endPage - allocationEndPage;
        if ((leadingPages != 0U) && (trailingPages != 0U) && (m_rangeCount >= MaximumFreePhysicalRanges))
        {
            return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Failure(
                BootstrapPageAllocatorError::RangeCapacityExceeded);
        }

        const PhysicalPageRange allocation =
            MakeRange(allocationStartPage << 12U, pageCount, current.Attributes, current.FirmwareType);
        if ((leadingPages == 0U) && (trailingPages == 0U))
        {
            for (Foundation::Size moveIndex{index + 1U}; moveIndex < m_rangeCount; ++moveIndex)
            {
                m_ranges[moveIndex - 1U] = m_ranges[moveIndex];
            }
            --m_rangeCount;
        }
        else if (leadingPages == 0U)
        {
            m_ranges[index] =
                MakeRange(allocationEndPage << 12U, trailingPages, current.Attributes, current.FirmwareType);
        }
        else if (trailingPages == 0U)
        {
            m_ranges[index].PageCount = leadingPages;
        }
        else
        {
            for (Foundation::Size moveIndex{m_rangeCount}; moveIndex > (index + 1U); --moveIndex)
            {
                m_ranges[moveIndex] = m_ranges[moveIndex - 1U];
            }
            m_ranges[index].PageCount = leadingPages;
            m_ranges[index + 1U] =
                MakeRange(allocationEndPage << 12U, trailingPages, current.Attributes, current.FirmwareType);
            ++m_rangeCount;
        }

        m_freePageCount -= pageCount;
        m_allocatedPageCount += pageCount;
        return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Success(allocation);
    }

    return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Failure(
        BootstrapPageAllocatorError::InsufficientMemory);
}

Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
BootstrapPageAllocator::GetFreeRange(const Foundation::Size index) const noexcept
{
    if (!m_initialized)
    {
        return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Failure(
            BootstrapPageAllocatorError::NotInitialized);
    }
    if (index >= m_rangeCount)
    {
        return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Failure(
            BootstrapPageAllocatorError::RangeIndexOutOfBounds);
    }
    return Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>::Success(m_ranges[index]);
}

bool BootstrapPageAllocator::IsInitialized() const noexcept
{
    return m_initialized;
}

Foundation::Size BootstrapPageAllocator::GetFreeRangeCount() const noexcept
{
    return m_rangeCount;
}

Foundation::UInt64 BootstrapPageAllocator::GetFreePageCount() const noexcept
{
    return m_freePageCount;
}

Foundation::UInt64 BootstrapPageAllocator::GetAllocatedPageCount() const noexcept
{
    return m_allocatedPageCount;
}
} // namespace Asteria::Kernel::Memory
