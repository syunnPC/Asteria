// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MemoryCachePolicy;

constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 LowerCanonicalLast{0x0000'7FFF'FFFF'FFFFULL};
constexpr Foundation::UInt64 UpperCanonicalFirst{0xFFFF'8000'0000'0000ULL};
constexpr Foundation::UInt64 ArchitecturalPhysicalAddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 WritableBit{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 PageWriteThroughBit{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 PageCacheDisableBit{Foundation::UInt64{1U} << 4U};
constexpr Foundation::UInt64 PageAttributeTableBit{Foundation::UInt64{1U} << 7U};
constexpr Foundation::UInt64 GlobalBit{Foundation::UInt64{1U} << 8U};
constexpr Foundation::UInt64 ExecuteDisableBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 NonLeafFlags{PresentBit | WritableBit};

struct PrefixCounter final
{
    Foundation::UInt64 Count{0U};
    Foundation::UInt64 LastKey{0U};
    bool HasKey{false};
};

[[nodiscard]] constexpr InitialPageTableBuildError Fail(const InitialPageTableBuildError error) noexcept
{
    return error;
}

[[nodiscard]] bool TryGetEnd(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                             Foundation::UInt64& end) noexcept
{
    constexpr Foundation::UInt64 MaximumPageCount{MaximumUInt64 >> 12U};
    if ((pages == 0U) || (pages > MaximumPageCount) || ((start & PageOffsetMask) != 0U))
    {
        return false;
    }
    const Foundation::UInt64 bytes = pages << 12U;
    if (start > (MaximumUInt64 - bytes))
    {
        return false;
    }
    end = start + bytes;
    return true;
}

[[nodiscard]] bool IsCanonicalRange(const Foundation::UInt64 start, const Foundation::UInt64 end) noexcept
{
    if (end <= start)
    {
        return false;
    }
    const Foundation::UInt64 last = end - 1U;
    return ((start <= LowerCanonicalLast) && (last <= LowerCanonicalLast)) ||
           ((start >= UpperCanonicalFirst) && (last >= UpperCanonicalFirst));
}

[[nodiscard]] bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                 const Foundation::UInt64 secondStart, const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] bool AddPrefixRange(PrefixCounter& counter, const Foundation::UInt64 firstKey,
                                  const Foundation::UInt64 lastKey) noexcept
{
    if (firstKey > lastKey)
    {
        return false;
    }

    Foundation::UInt64 firstNewKey = firstKey;
    if (counter.HasKey)
    {
        if (lastKey <= counter.LastKey)
        {
            return true;
        }
        if (firstNewKey <= counter.LastKey)
        {
            firstNewKey = counter.LastKey + 1U;
        }
    }

    const Foundation::UInt64 added = (lastKey - firstNewKey) + 1U;
    if (counter.Count > (MaximumUInt64 - added))
    {
        return false;
    }
    counter.Count += added;
    counter.LastKey = lastKey;
    counter.HasKey = true;
    return true;
}

[[nodiscard]] bool AddMappingPrefixes(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                      PrefixCounter& pml4Prefixes, PrefixCounter& oneGibPrefixes,
                                      PrefixCounter& twoMibPrefixes) noexcept
{
    const Foundation::UInt64 last = end - 1U;
    return AddPrefixRange(pml4Prefixes, (start >> 39U) & 0x1FFU, (last >> 39U) & 0x1FFU) &&
           AddPrefixRange(oneGibPrefixes, (start >> 30U) & 0x3FFFFU, (last >> 30U) & 0x3FFFFU) &&
           AddPrefixRange(twoMibPrefixes, (start >> 21U) & 0x7FFFFFFU, (last >> 21U) & 0x7FFFFFFU);
}

[[nodiscard]] Foundation::UInt64 GetCacheBits(const MemoryCachePolicy policy) noexcept
{
    switch (policy)
    {
    case MemoryCachePolicy::WriteBack:
        return 0U;
    case MemoryCachePolicy::WriteThrough:
        return PageWriteThroughBit;
    case MemoryCachePolicy::Uncached:
        return PageWriteThroughBit | PageCacheDisableBit;
    case MemoryCachePolicy::WriteCombining:
        return PageAttributeTableBit;
    default:
        return 0U;
    }
}

class TableWriter final
{
public:
    explicit TableWriter(const PageTableStorage& storage) noexcept : m_storage{storage}, m_nextPage{1U}
    {
    }

    void Clear() noexcept
    {
        const Foundation::Size entryCapacity = m_storage.PageCapacity * PageTableEntriesPerPage;
        for (Foundation::Size index{0U}; index < entryCapacity; ++index)
        {
            m_storage.Entries[index] = 0U;
        }
        m_nextPage = 1U;
    }

    [[nodiscard]] bool GetOrCreateChild(const Foundation::Size parentPage, const Foundation::Size entryIndex,
                                        Foundation::Size& childPage) noexcept
    {
        if ((parentPage >= m_storage.PageCapacity) || (entryIndex >= PageTableEntriesPerPage))
        {
            return false;
        }
        Foundation::UInt64& entry = m_storage.Entries[(parentPage * PageTableEntriesPerPage) + entryIndex];
        if (entry == 0U)
        {
            if (m_nextPage >= m_storage.PageCapacity)
            {
                return false;
            }
            childPage = m_nextPage;
            ++m_nextPage;
            const Foundation::UInt64 childPhysical =
                m_storage.PhysicalStart.GetValue() + (static_cast<Foundation::UInt64>(childPage) << 12U);
            entry = childPhysical | NonLeafFlags;
            return true;
        }

        if ((entry & ~ArchitecturalPhysicalAddressMask) != NonLeafFlags)
        {
            return false;
        }
        const Foundation::UInt64 childPhysical = entry & ArchitecturalPhysicalAddressMask;
        const Foundation::UInt64 storagePhysical = m_storage.PhysicalStart.GetValue();
        if (childPhysical < storagePhysical)
        {
            return false;
        }
        const Foundation::UInt64 byteOffset = childPhysical - storagePhysical;
        if ((byteOffset & PageOffsetMask) != 0U)
        {
            return false;
        }
        childPage = static_cast<Foundation::Size>(byteOffset >> 12U);
        return childPage < m_nextPage;
    }

    [[nodiscard]] bool WriteLeaf(const Foundation::Size tablePage, const Foundation::Size entryIndex,
                                 const Foundation::UInt64 value) noexcept
    {
        if ((tablePage >= m_storage.PageCapacity) || (entryIndex >= PageTableEntriesPerPage))
        {
            return false;
        }
        Foundation::UInt64& entry = m_storage.Entries[(tablePage * PageTableEntriesPerPage) + entryIndex];
        if (entry != 0U)
        {
            return false;
        }
        entry = value;
        return true;
    }

    [[nodiscard]] Foundation::Size GetUsedPageCount() const noexcept
    {
        return m_nextPage;
    }

private:
    const PageTableStorage& m_storage;
    Foundation::Size m_nextPage;
};

[[nodiscard]] Foundation::UInt64 EncodeLeaf(const InitialMapping& mapping,
                                            const Foundation::UInt64 physicalAddress) noexcept
{
    Foundation::UInt64 entry = physicalAddress | PresentBit | GetCacheBits(mapping.CachePolicy);
    if (mapping.Permissions.Contains(MappingPermission::Write))
    {
        entry |= WritableBit;
    }
    if (mapping.Permissions.Contains(MappingPermission::Global))
    {
        entry |= GlobalBit;
    }
    if (!mapping.Permissions.Contains(MappingPermission::Execute))
    {
        entry |= ExecuteDisableBit;
    }
    return entry;
}
} // namespace

InitialPageTableBuildError InitialPageTableBuilder::Build(const Kernel::Memory::InitialMappingPlan& plan,
                                                          const PagingCapabilities& capabilities,
                                                          const PageTableStorage& storage,
                                                          InitialPageTableBuildResult& output) noexcept
{
    if (!plan.IsSealed())
    {
        return Fail(InitialPageTableBuildError::PlanNotSealed);
    }
    if ((capabilities.PhysicalAddressBits < 32U) || (capabilities.PhysicalAddressBits > 52U) ||
        (capabilities.Reserved16 != 0U) || (capabilities.Reserved != 0U))
    {
        return Fail(InitialPageTableBuildError::InvalidCapabilities);
    }
    if (!capabilities.FourLevelPagingEnabled)
    {
        return Fail(InitialPageTableBuildError::FourLevelPagingRequired);
    }
    if (!capabilities.SupervisorWriteProtectEnabled)
    {
        return Fail(InitialPageTableBuildError::SupervisorWriteProtectRequired);
    }
    if (!capabilities.PatLayoutConfigured)
    {
        return Fail(InitialPageTableBuildError::PageAttributeTableRequired);
    }

    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << capabilities.PhysicalAddressBits;
    if ((storage.Entries == nullptr) || (storage.PageCapacity == 0U) ||
        (storage.PageCapacity > MaximumInitialPageTablePages) || (storage.Reserved != 0U) ||
        (storage.PhysicalStart.GetValue() == 0U) || ((storage.PhysicalStart.GetValue() & PageOffsetMask) != 0U) ||
        ((reinterpret_cast<Foundation::UIntPointer>(storage.Entries) & PageOffsetMask) != 0U))
    {
        return Fail(InitialPageTableBuildError::InvalidStorage);
    }
    const Foundation::UInt64 storageBytes = static_cast<Foundation::UInt64>(storage.PageCapacity) << 12U;
    const Foundation::UInt64 storageVirtualStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage.Entries));
    if (storageVirtualStart > (MaximumUInt64 - storageBytes))
    {
        return Fail(InitialPageTableBuildError::InvalidStorage);
    }
    const Foundation::UInt64 storageVirtualEnd = storageVirtualStart + storageBytes;
    const Foundation::UInt64 planStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&plan));
    const Foundation::UInt64 capabilitiesStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&capabilities));
    const Foundation::UInt64 storageDescriptorStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&storage));
    if (RangesOverlap(storageVirtualStart, storageVirtualEnd, planStart, planStart + sizeof(plan)) ||
        RangesOverlap(storageVirtualStart, storageVirtualEnd, capabilitiesStart,
                      capabilitiesStart + sizeof(capabilities)) ||
        RangesOverlap(storageVirtualStart, storageVirtualEnd, storageDescriptorStart,
                      storageDescriptorStart + sizeof(storage)))
    {
        return Fail(InitialPageTableBuildError::PageTableStorageInputAlias);
    }
    const Foundation::UInt64 outputStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&output));
    if (outputStart > (MaximumUInt64 - sizeof(output)))
    {
        return Fail(InitialPageTableBuildError::BuildResultAlias);
    }
    const Foundation::UInt64 outputEnd = outputStart + sizeof(output);
    if (RangesOverlap(outputStart, outputEnd, storageVirtualStart, storageVirtualEnd) ||
        RangesOverlap(outputStart, outputEnd, planStart, planStart + sizeof(plan)) ||
        RangesOverlap(outputStart, outputEnd, capabilitiesStart, capabilitiesStart + sizeof(capabilities)) ||
        RangesOverlap(outputStart, outputEnd, storageDescriptorStart, storageDescriptorStart + sizeof(storage)))
    {
        return Fail(InitialPageTableBuildError::BuildResultAlias);
    }
    if (storage.PhysicalStart.GetValue() > (physicalLimit - storageBytes))
    {
        return Fail(InitialPageTableBuildError::InvalidStorage);
    }
    const Foundation::UInt64 storageEnd = storage.PhysicalStart.GetValue() + storageBytes;

    PrefixCounter pml4Prefixes{};
    PrefixCounter oneGibPrefixes{};
    PrefixCounter twoMibPrefixes{};
    Foundation::UInt64 mappedPages{0U};
    bool storageOwned{false};
    bool storageAliased{false};
    for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
    {
        const auto mappingResult = plan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return Fail(InitialPageTableBuildError::InternalInvariantViolation);
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 virtualEnd{0U};
        Foundation::UInt64 physicalEnd{0U};
        if (!TryGetEnd(mapping.VirtualStart.GetValue(), mapping.Pages.GetValue(), virtualEnd) ||
            !TryGetEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue(), physicalEnd))
        {
            return Fail(InitialPageTableBuildError::InternalInvariantViolation);
        }
        if (!IsCanonicalRange(mapping.VirtualStart.GetValue(), virtualEnd))
        {
            return Fail(InitialPageTableBuildError::NonCanonicalVirtualAddress);
        }
        if (physicalEnd > physicalLimit)
        {
            return Fail(InitialPageTableBuildError::PhysicalAddressOutOfRange);
        }
        if (!mapping.Permissions.Contains(MappingPermission::Execute) && !capabilities.ExecuteDisableEnabled)
        {
            return Fail(InitialPageTableBuildError::ExecuteDisableRequired);
        }
        if (mapping.Permissions.Contains(MappingPermission::Global) && !capabilities.GlobalPagesEnabled)
        {
            return Fail(InitialPageTableBuildError::GlobalPagesRequired);
        }
        if (!AddMappingPrefixes(mapping.VirtualStart.GetValue(), virtualEnd, pml4Prefixes, oneGibPrefixes,
                                twoMibPrefixes))
        {
            return Fail(InitialPageTableBuildError::InternalInvariantViolation);
        }
        mappedPages += mapping.Pages.GetValue();

        if (RangesOverlap(mapping.PhysicalStart.GetValue(), physicalEnd, storage.PhysicalStart.GetValue(), storageEnd))
        {
            if (mapping.Owner != InitialMappingOwner::PageTable)
            {
                storageAliased = true;
            }
            else if ((mapping.PhysicalStart.GetValue() <= storage.PhysicalStart.GetValue()) &&
                     (physicalEnd >= storageEnd))
            {
                storageOwned = true;
            }
        }
    }

    if (storageAliased)
    {
        return Fail(InitialPageTableBuildError::PageTableStorageAlias);
    }
    if (!storageOwned)
    {
        return Fail(InitialPageTableBuildError::PageTableStorageNotOwned);
    }

    const Foundation::UInt64 requiredTablePages =
        Foundation::UInt64{1U} + pml4Prefixes.Count + oneGibPrefixes.Count + twoMibPrefixes.Count;
    if (requiredTablePages > static_cast<Foundation::UInt64>(storage.PageCapacity))
    {
        return Fail(InitialPageTableBuildError::InsufficientTableStorage);
    }

    TableWriter writer{storage};
    writer.Clear();
    for (Foundation::Size mappingIndex{0U}; mappingIndex < plan.GetMappingCount(); ++mappingIndex)
    {
        const auto mappingResult = plan.GetMapping(mappingIndex);
        if (!mappingResult.HasValue())
        {
            writer.Clear();
            return Fail(InitialPageTableBuildError::InternalInvariantViolation);
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 virtualAddress = mapping.VirtualStart.GetValue();
        Foundation::UInt64 physicalAddress = mapping.PhysicalStart.GetValue();
        for (Foundation::UInt64 page{0U}; page < mapping.Pages.GetValue(); ++page)
        {
            const Foundation::Size pml4Index = static_cast<Foundation::Size>((virtualAddress >> 39U) & 0x1FFU);
            const Foundation::Size pdptIndex = static_cast<Foundation::Size>((virtualAddress >> 30U) & 0x1FFU);
            const Foundation::Size pageDirectoryIndex = static_cast<Foundation::Size>((virtualAddress >> 21U) & 0x1FFU);
            const Foundation::Size pageTableIndex = static_cast<Foundation::Size>((virtualAddress >> 12U) & 0x1FFU);

            Foundation::Size pdptPage{0U};
            Foundation::Size pageDirectoryPage{0U};
            Foundation::Size pageTablePage{0U};
            if (!writer.GetOrCreateChild(0U, pml4Index, pdptPage) ||
                !writer.GetOrCreateChild(pdptPage, pdptIndex, pageDirectoryPage) ||
                !writer.GetOrCreateChild(pageDirectoryPage, pageDirectoryIndex, pageTablePage) ||
                !writer.WriteLeaf(pageTablePage, pageTableIndex, EncodeLeaf(mapping, physicalAddress)))
            {
                writer.Clear();
                return Fail(InitialPageTableBuildError::InternalInvariantViolation);
            }

            virtualAddress += PageSizeBytes;
            physicalAddress += PageSizeBytes;
        }
    }

    if (writer.GetUsedPageCount() != static_cast<Foundation::Size>(requiredTablePages))
    {
        writer.Clear();
        return Fail(InitialPageTableBuildError::InternalInvariantViolation);
    }
    output = InitialPageTableBuildResult{
        .RootPhysicalAddress = storage.PhysicalStart,
        .UsedTablePages = writer.GetUsedPageCount(),
        .MappedPages = mappedPages,
        .Reserved = 0U,
    };
    return InitialPageTableBuildError::None;
}
} // namespace Asteria::Platform::Amd64::Memory
