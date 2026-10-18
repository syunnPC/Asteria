// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/InitialPageTableVerifier.hpp"
#include "Platform/Amd64/Memory/RuntimePageTableMapper.hpp"

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
using Kernel::Memory::InitialMapping;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MemoryCachePolicy;

constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 ArchitecturalPhysicalAddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 WritableBit{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 PageWriteThroughBit{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 PageCacheDisableBit{Foundation::UInt64{1U} << 4U};
constexpr Foundation::UInt64 PageAttributeTableBit{Foundation::UInt64{1U} << 7U};
constexpr Foundation::UInt64 GlobalBit{Foundation::UInt64{1U} << 8U};
constexpr Foundation::UInt64 ExecuteDisableBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 NonLeafFlags{PresentBit | WritableBit};

[[nodiscard]] constexpr InitialPageTableVerificationError Fail(const InitialPageTableVerificationError error) noexcept
{
    return error;
}

[[nodiscard]] constexpr Foundation::UInt64 GetCacheBits(const MemoryCachePolicy policy) noexcept
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
    }
    return ~Foundation::UInt64{0U};
}

[[nodiscard]] constexpr Foundation::UInt64 EncodeLeaf(const InitialMapping& mapping,
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

[[nodiscard]] bool TryGetMappingEnd(const InitialMapping& mapping, Foundation::UInt64& virtualEnd) noexcept
{
    const Foundation::UInt64 pages = mapping.Pages.GetValue();
    if ((pages == 0U) || (pages > ((~Foundation::UInt64{0U}) >> 12U)))
    {
        return false;
    }
    const Foundation::UInt64 bytes = pages << 12U;
    const Foundation::UInt64 start = mapping.VirtualStart.GetValue();
    if (((start & PageOffsetMask) != 0U) || (start > ((~Foundation::UInt64{0U}) - bytes)))
    {
        return false;
    }
    virtualEnd = start + bytes;
    return true;
}

[[nodiscard]] Foundation::UInt64 ComposeVirtualAddress(const Foundation::Size pml4Index,
                                                       const Foundation::Size pdptIndex,
                                                       const Foundation::Size pageDirectoryIndex,
                                                       const Foundation::Size pageTableIndex) noexcept
{
    Foundation::UInt64 address = (static_cast<Foundation::UInt64>(pml4Index) << 39U) |
                                 (static_cast<Foundation::UInt64>(pdptIndex) << 30U) |
                                 (static_cast<Foundation::UInt64>(pageDirectoryIndex) << 21U) |
                                 (static_cast<Foundation::UInt64>(pageTableIndex) << 12U);
    if ((address & (Foundation::UInt64{1U} << 47U)) != 0U)
    {
        address |= 0xFFFF'0000'0000'0000ULL;
    }
    return address;
}

[[nodiscard]] InitialPageTableVerificationError ResolveChildPage(const Foundation::UInt64 entry,
                                                                 const InitialPageTablePreparationSummary& summary,
                                                                 Foundation::UInt64& referencedPages,
                                                                 Foundation::Size& childPage) noexcept
{
    if ((entry & ~ArchitecturalPhysicalAddressMask) != NonLeafFlags)
    {
        return Fail(InitialPageTableVerificationError::InvalidNonLeafEntry);
    }
    const Foundation::UInt64 root = summary.Storage.RootPhysicalAddress.GetValue();
    const Foundation::UInt64 childPhysical = entry & ArchitecturalPhysicalAddressMask;
    if ((childPhysical < root) || (((childPhysical - root) & PageOffsetMask) != 0U))
    {
        return Fail(InitialPageTableVerificationError::InvalidNonLeafEntry);
    }
    childPage = static_cast<Foundation::Size>((childPhysical - root) >> 12U);
    if ((childPage == 0U) || (childPage >= summary.Storage.UsedTablePages) || (childPage >= 64U))
    {
        return Fail(InitialPageTableVerificationError::InvalidNonLeafEntry);
    }
    const Foundation::UInt64 childBit = Foundation::UInt64{1U} << childPage;
    if ((referencedPages & childBit) != 0U)
    {
        return Fail(InitialPageTableVerificationError::DuplicateTablePage);
    }
    referencedPages |= childBit;
    return InitialPageTableVerificationError::None;
}

[[nodiscard]] InitialPageTableVerificationError VerifyLeaf(const Kernel::Memory::InitialMappingPlan& plan,
                                                           const Foundation::UInt64 virtualAddress,
                                                           const Foundation::UInt64 entry) noexcept
{
    for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
    {
        const auto mappingResult = plan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return Fail(InitialPageTableVerificationError::InvalidMapping);
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 virtualEnd{0U};
        if (!TryGetMappingEnd(mapping, virtualEnd))
        {
            return Fail(InitialPageTableVerificationError::InvalidMapping);
        }
        const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
        if ((virtualAddress < virtualStart) || (virtualAddress >= virtualEnd))
        {
            continue;
        }
        const Foundation::UInt64 expectedPhysical = mapping.PhysicalStart.GetValue() + (virtualAddress - virtualStart);
        return entry == EncodeLeaf(mapping, expectedPhysical)
                   ? InitialPageTableVerificationError::None
                   : Fail(InitialPageTableVerificationError::UnexpectedLeafEntry);
    }
    return Fail(InitialPageTableVerificationError::UnexpectedLeafEntry);
}
} // Namespace

InitialPageTableVerificationError
InitialPageTableVerifier::Verify(const Kernel::Memory::InitialMappingPlan& plan, const PagingCapabilities& capabilities,
                                 const InitialPageTablePreparationSummary& summary) noexcept
{
    if (!plan.IsSealed())
    {
        return Fail(InitialPageTableVerificationError::PlanNotSealed);
    }
    if ((capabilities.PhysicalAddressBits < 32U) || (capabilities.PhysicalAddressBits > 52U) ||
        !capabilities.FourLevelPagingEnabled || !capabilities.ExecuteDisableEnabled ||
        !capabilities.SupervisorWriteProtectEnabled || !capabilities.GlobalPagesEnabled ||
        !capabilities.PatLayoutConfigured || (capabilities.Reserved16 != 0U) || (capabilities.Reserved != 0U))
    {
        return Fail(InitialPageTableVerificationError::InvalidCapabilities);
    }
    if ((summary.Storage.Entries == nullptr) || (summary.Storage.RootPhysicalAddress.GetValue() == 0U) ||
        ((summary.Storage.RootPhysicalAddress.GetValue() & PageOffsetMask) != 0U) ||
        (summary.Storage.PageCapacity == 0U) || !IsRuntimePageTableCapacity(summary.Storage.PageCapacity) ||
        (summary.Storage.UsedTablePages == 0U)
        // Seed stays below 64.
        || (summary.Storage.UsedTablePages > Bootstrap::RootArenaPages) ||
        (summary.Storage.UsedTablePages > summary.Storage.PageCapacity) ||
        (summary.MappingCount != plan.GetMappingCount()) || (summary.MappingCount == 0U) ||
        (summary.Reserved16 != 0U) || (summary.Reserved32 != 0U))
    {
        return Fail(InitialPageTableVerificationError::InvalidSummary);
    }
    if ((reinterpret_cast<Foundation::UIntPointer>(summary.Storage.Entries) & PageOffsetMask) != 0U)
    {
        return Fail(InitialPageTableVerificationError::InvalidStorage);
    }

    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << capabilities.PhysicalAddressBits;
    const Foundation::UInt64 storageBytes = static_cast<Foundation::UInt64>(summary.Storage.PageCapacity) << 12U;
    const Foundation::UInt64 storageStart = summary.Storage.RootPhysicalAddress.GetValue();
    const Foundation::UInt64 storageVirtualStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(summary.Storage.Entries));
    if ((storageStart > (physicalLimit - storageBytes)) ||
        (storageVirtualStart > ((~Foundation::UInt64{0U}) - storageBytes)))
    {
        return Fail(InitialPageTableVerificationError::InvalidStorage);
    }
    const Foundation::UInt64 storageEnd = storageStart + storageBytes;

    Foundation::UInt64 expectedMappedPages{0U};
    Foundation::Size storageOwners{0U};
    for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
    {
        const auto mappingResult = plan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return Fail(InitialPageTableVerificationError::InvalidMapping);
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 virtualEnd{0U};
        const Foundation::UInt64 mappingBytes = mapping.Pages.GetValue() << 12U;
        if (!TryGetMappingEnd(mapping, virtualEnd) || (mappingBytes > physicalLimit) ||
            (mapping.PhysicalStart.GetValue() > (physicalLimit - mappingBytes)))
        {
            return Fail(InitialPageTableVerificationError::InvalidMapping);
        }
        const Foundation::UInt64 mappingPhysicalEnd = mapping.PhysicalStart.GetValue() + mappingBytes;
        const bool overlapsStorage =
            (mapping.PhysicalStart.GetValue() < storageEnd) && (storageStart < mappingPhysicalEnd);
        if (overlapsStorage)
        {
            if ((mapping.Owner != Kernel::Memory::InitialMappingOwner::PageTable) ||
                (mapping.PhysicalStart.GetValue() > storageStart) || (mappingPhysicalEnd < storageEnd))
            {
                return Fail(InitialPageTableVerificationError::InvalidStorage);
            }
            ++storageOwners;
        }
        expectedMappedPages += mapping.Pages.GetValue();
    }
    if (storageOwners != 1U)
    {
        return Fail(InitialPageTableVerificationError::InvalidStorage);
    }
    if (expectedMappedPages != summary.MappedPages)
    {
        return Fail(InitialPageTableVerificationError::MappedPageCountMismatch);
    }

    Foundation::UInt64 referencedPages{1U};
    Foundation::UInt64 observedMappedPages{0U};
    const Foundation::UInt64* const entries = summary.Storage.Entries;
    for (Foundation::Size pml4Index{0U}; pml4Index < PageTableEntriesPerPage; ++pml4Index)
    {
        const Foundation::UInt64 pml4Entry = entries[pml4Index];
        if (pml4Entry == 0U)
        {
            continue;
        }
        Foundation::Size pdptPage{0U};
        InitialPageTableVerificationError error = ResolveChildPage(pml4Entry, summary, referencedPages, pdptPage);
        if (error != InitialPageTableVerificationError::None)
        {
            return error;
        }
        for (Foundation::Size pdptIndex{0U}; pdptIndex < PageTableEntriesPerPage; ++pdptIndex)
        {
            const Foundation::UInt64 pdptEntry = entries[(pdptPage * PageTableEntriesPerPage) + pdptIndex];
            if (pdptEntry == 0U)
            {
                continue;
            }
            Foundation::Size pageDirectoryPage{0U};
            error = ResolveChildPage(pdptEntry, summary, referencedPages, pageDirectoryPage);
            if (error != InitialPageTableVerificationError::None)
            {
                return error;
            }
            for (Foundation::Size pageDirectoryIndex{0U}; pageDirectoryIndex < PageTableEntriesPerPage;
                 ++pageDirectoryIndex)
            {
                const Foundation::UInt64 pageDirectoryEntry =
                    entries[(pageDirectoryPage * PageTableEntriesPerPage) + pageDirectoryIndex];
                if (pageDirectoryEntry == 0U)
                {
                    continue;
                }
                Foundation::Size pageTablePage{0U};
                error = ResolveChildPage(pageDirectoryEntry, summary, referencedPages, pageTablePage);
                if (error != InitialPageTableVerificationError::None)
                {
                    return error;
                }
                for (Foundation::Size pageTableIndex{0U}; pageTableIndex < PageTableEntriesPerPage; ++pageTableIndex)
                {
                    const Foundation::UInt64 leafEntry =
                        entries[(pageTablePage * PageTableEntriesPerPage) + pageTableIndex];
                    const Foundation::UInt64 virtualAddress =
                        ComposeVirtualAddress(pml4Index, pdptIndex, pageDirectoryIndex, pageTableIndex);
                    if (leafEntry == 0U)
                    {
                        continue;
                    }
                    error = VerifyLeaf(plan, virtualAddress, leafEntry);
                    if (error != InitialPageTableVerificationError::None)
                    {
                        return error;
                    }
                    ++observedMappedPages;
                }
            }
        }
    }

    const Foundation::UInt64 expectedReferencedPages =
        summary.Storage.UsedTablePages == 64U ? ~Foundation::UInt64{0U}
                                              : (Foundation::UInt64{1U} << summary.Storage.UsedTablePages) - 1U;
    if (referencedPages != expectedReferencedPages)
    {
        return Fail(InitialPageTableVerificationError::MissingTablePage);
    }
    if (observedMappedPages != expectedMappedPages)
    {
        return Fail(InitialPageTableVerificationError::MissingLeafEntry);
    }
    for (Foundation::Size page{summary.Storage.UsedTablePages}; page < summary.Storage.PageCapacity; ++page)
    {
        for (Foundation::Size index{0U}; index < PageTableEntriesPerPage; ++index)
        {
            if (entries[(page * PageTableEntriesPerPage) + index] != 0U)
            {
                return Fail(InitialPageTableVerificationError::NonZeroUnusedStorage);
            }
        }
    }
    return InitialPageTableVerificationError::None;
}
} // Namespace Asteria::Platform::Amd64::Memory
