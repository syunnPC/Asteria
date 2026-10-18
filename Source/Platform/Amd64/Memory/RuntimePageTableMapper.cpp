// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/RuntimePageTableMapper.hpp"
#include "Platform/Amd64/Memory/UserAddressPolicy.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PagePermission;
using Platform::Synchronization::AtomicUInt64Reference;

constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 LowerCanonicalLast{UserAddressEnd - 1U};
constexpr Foundation::UInt64 UpperCanonicalFirst{0xFFFF'8000'0000'0000ULL};
constexpr Foundation::UInt64 ArchitecturalPhysicalAddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 WritableBit{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 UserBit{Foundation::UInt64{1U} << 2U};
constexpr Foundation::UInt64 PageWriteThroughBit{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 PageCacheDisableBit{Foundation::UInt64{1U} << 4U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 PageAttributeTableBit{Foundation::UInt64{1U} << 7U};
constexpr Foundation::UInt64 GlobalBit{Foundation::UInt64{1U} << 8U};
constexpr Foundation::UInt64 ExecuteDisableBit{Foundation::UInt64{1U} << 63U};

struct MissingPrefix final
{
    Foundation::UInt64 LastKey{0U};
    bool HasKey{false};
};

[[nodiscard]] Foundation::UInt64 LoadEntry(volatile Foundation::UInt64* const entry) noexcept
{
    return AtomicUInt64Reference{*entry}.LoadAcquire();
}

void StoreEntryRelaxed(volatile Foundation::UInt64* const entry, const Foundation::UInt64 value) noexcept
{
    AtomicUInt64Reference{*entry}.StoreRelaxed(value);
}

void StoreEntryRelease(volatile Foundation::UInt64* const entry, const Foundation::UInt64 value) noexcept
{
    AtomicUInt64Reference{*entry}.StoreRelease(value);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool IsCanonicalRange(const Foundation::UInt64 start, const Foundation::UInt64 end) noexcept
{
    if (end <= start)
    {
        return false;
    }
    const Foundation::UInt64 last = end - 1U;
    return ((start <= LowerCanonicalLast) && (last <= LowerCanonicalLast)) ||
           ((start >= UpperCanonicalFirst) && (last >= UpperCanonicalFirst));
}

[[nodiscard]] bool ReservedBytesAreZero(const AddressSpaceMappingRecord& mapping) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(mapping.Reserved); ++index)
    {
        if (mapping.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsKnownCachePolicy(const MemoryCachePolicy policy) noexcept
{
    switch (policy)
    {
    case MemoryCachePolicy::WriteBack:
    case MemoryCachePolicy::Uncached:
    case MemoryCachePolicy::WriteCombining:
    case MemoryCachePolicy::WriteThrough:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownFrameKind(const FrameKind kind) noexcept
{
    switch (kind)
    {
    case FrameKind::Ram:
    case FrameKind::Device:
        return true;
    }
    return false;
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
    return 0U;
}

[[nodiscard]] bool AddMissingPrefix(MissingPrefix& prefix, const Foundation::UInt64 key,
                                    Foundation::Size& requiredPages) noexcept
{
    if (prefix.HasKey)
    {
        if (key == prefix.LastKey)
        {
            return true;
        }
        if (key < prefix.LastKey)
        {
            return false;
        }
    }
    if (requiredPages >= MaximumRuntimePageTablePages)
    {
        return false;
    }
    prefix.LastKey = key;
    prefix.HasKey = true;
    ++requiredPages;
    return true;
}

[[nodiscard]] RuntimePageTableMapError DecodeChildPage(const Foundation::UInt64 entry,
                                                       const Foundation::Size parentPage,
                                                       const Foundation::UInt64 physicalAddressMask,
                                                       const RuntimePageTableStorage& storage,
                                                       Foundation::Size& childPage) noexcept
{
    const Foundation::UInt64 allowedBits = physicalAddressMask | PresentBit | WritableBit | UserBit | AccessedBit;
    if (((entry & PresentBit) == 0U) || ((entry & ~allowedBits) != 0U) || ((entry & WritableBit) == 0U))
    {
        return RuntimePageTableMapError::InvalidTableEntry;
    }

    const Foundation::UInt64 childPhysical = entry & physicalAddressMask;
    const Foundation::UInt64 storagePhysical = storage.PhysicalStart.GetValue();
    if (childPhysical < storagePhysical)
    {
        return RuntimePageTableMapError::InvalidTableEntry;
    }
    const Foundation::UInt64 byteOffset = childPhysical - storagePhysical;
    if ((byteOffset & PageOffsetMask) != 0U)
    {
        return RuntimePageTableMapError::InvalidTableEntry;
    }
    const Foundation::UInt64 pageIndex = byteOffset >> 12U;
    if ((pageIndex <= static_cast<Foundation::UInt64>(parentPage)) ||
        (pageIndex >= static_cast<Foundation::UInt64>(storage.UsedPageCount)))
    {
        return RuntimePageTableMapError::InvalidTableEntry;
    }
    childPage = static_cast<Foundation::Size>(pageIndex);
    return RuntimePageTableMapError::None;
}

[[nodiscard]] RuntimePageTableMapError InspectMapping(const AddressSpaceMappingRecord& mapping,
                                                      const Foundation::UInt64 physicalAddressMask,
                                                      const RuntimePageTableStorage& storage,
                                                      Foundation::Size& requiredPages) noexcept
{
    MissingPrefix pml4Prefixes{};
    MissingPrefix oneGibPrefixes{};
    MissingPrefix twoMibPrefixes{};
    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 pageCount = mapping.Pages.GetValue();

    for (Foundation::UInt64 pageOffset{0U}; pageOffset < pageCount; ++pageOffset)
    {
        const Foundation::UInt64 virtualAddress = virtualStart + (pageOffset << 12U);
        const Foundation::Size pml4Index = static_cast<Foundation::Size>((virtualAddress >> 39U) & 0x1FFU);
        const Foundation::Size pdptIndex = static_cast<Foundation::Size>((virtualAddress >> 30U) & 0x1FFU);
        const Foundation::Size pdIndex = static_cast<Foundation::Size>((virtualAddress >> 21U) & 0x1FFU);
        const Foundation::Size ptIndex = static_cast<Foundation::Size>((virtualAddress >> 12U) & 0x1FFU);

        Foundation::Size tablePage{0U};
        bool missingAncestor{false};

        const Foundation::UInt64 pml4Entry = LoadEntry(&storage.Entries[pml4Index]);
        if (pml4Entry == 0U)
        {
            missingAncestor = true;
            if (!AddMissingPrefix(pml4Prefixes, (virtualAddress >> 39U) & 0x1FFU, requiredPages))
            {
                return RuntimePageTableMapError::InvalidMappingPlan;
            }
        }
        else
        {
            const RuntimePageTableMapError error =
                DecodeChildPage(pml4Entry, tablePage, physicalAddressMask, storage, tablePage);
            if (error != RuntimePageTableMapError::None)
            {
                return error;
            }
        }

        if (missingAncestor)
        {
            if (!AddMissingPrefix(oneGibPrefixes, (virtualAddress >> 30U) & 0x3FFFFU, requiredPages))
            {
                return RuntimePageTableMapError::InvalidMappingPlan;
            }
        }
        else
        {
            const Foundation::UInt64 pdptEntry =
                LoadEntry(&storage.Entries[(tablePage * PageTableEntriesPerPage) + pdptIndex]);
            if (pdptEntry == 0U)
            {
                missingAncestor = true;
                if (!AddMissingPrefix(oneGibPrefixes, (virtualAddress >> 30U) & 0x3FFFFU, requiredPages))
                {
                    return RuntimePageTableMapError::InvalidMappingPlan;
                }
            }
            else
            {
                const RuntimePageTableMapError error =
                    DecodeChildPage(pdptEntry, tablePage, physicalAddressMask, storage, tablePage);
                if (error != RuntimePageTableMapError::None)
                {
                    return error;
                }
            }
        }

        if (missingAncestor)
        {
            if (!AddMissingPrefix(twoMibPrefixes, (virtualAddress >> 21U) & 0x7FF'FFFFU, requiredPages))
            {
                return RuntimePageTableMapError::InvalidMappingPlan;
            }
        }
        else
        {
            const Foundation::UInt64 pdEntry =
                LoadEntry(&storage.Entries[(tablePage * PageTableEntriesPerPage) + pdIndex]);
            if (pdEntry == 0U)
            {
                missingAncestor = true;
                if (!AddMissingPrefix(twoMibPrefixes, (virtualAddress >> 21U) & 0x7FF'FFFFU, requiredPages))
                {
                    return RuntimePageTableMapError::InvalidMappingPlan;
                }
            }
            else
            {
                const RuntimePageTableMapError error =
                    DecodeChildPage(pdEntry, tablePage, physicalAddressMask, storage, tablePage);
                if (error != RuntimePageTableMapError::None)
                {
                    return error;
                }
            }
        }

        if (!missingAncestor)
        {
            const Foundation::UInt64 leaf =
                LoadEntry(&storage.Entries[(tablePage * PageTableEntriesPerPage) + ptIndex]);
            if (leaf != 0U)
            {
                return RuntimePageTableMapError::MappingAlreadyPresent;
            }
        }
    }
    return RuntimePageTableMapError::None;
}

[[nodiscard]] RuntimePageTableMapError ValidateExistingTree(const AddressSpaceMappingRecord& mapping,
                                                            const Foundation::UInt64 physicalAddressMask,
                                                            const bool globalPagesEnabled,
                                                            const RuntimePageTableStorage& storage) noexcept
{
    const Foundation::UInt64 requestedPhysicalStart = mapping.PhysicalStart.GetValue();
    const Foundation::UInt64 requestedPhysicalEnd = requestedPhysicalStart + (mapping.Pages.GetValue() << 12U);
    const bool requestedWritable = mapping.Permissions.Contains(PagePermission::Write);
    const bool requestedExecutable = mapping.Permissions.Contains(PagePermission::Execute);
    // One byte per page keeps 256-page validation within the target stack budget. Zero means
    // unseen; bits 0..2 are the level and bit 3 records a user-accessible path. A second parent
    // fails.
    Foundation::UInt8 state[MaximumRuntimePageTablePages]{};
    state[0U] = 4U | 8U;

    for (Foundation::Size page{0U}; page < storage.UsedPageCount; ++page)
    {
        const Foundation::UInt8 level = state[page] & 7U;
        if ((level == 0U) || (level > 4U))
        {
            return RuntimePageTableMapError::InvalidTableEntry;
        }

        const Foundation::Size firstEntry = page * PageTableEntriesPerPage;
        for (Foundation::Size index{0U}; index < PageTableEntriesPerPage; ++index)
        {
            const Foundation::UInt64 entry = LoadEntry(&storage.Entries[firstEntry + index]);
            if (entry == 0U)
            {
                continue;
            }

            if (level == 1U)
            {
                const Foundation::UInt64 allowedLeafBits = physicalAddressMask | PresentBit | WritableBit | UserBit |
                                                           PageWriteThroughBit | PageCacheDisableBit | AccessedBit |
                                                           DirtyBit | PageAttributeTableBit | GlobalBit |
                                                           ExecuteDisableBit;
                if (((entry & PresentBit) == 0U) || ((entry & ~allowedLeafBits) != 0U) ||
                    (!globalPagesEnabled && ((entry & GlobalBit) != 0U)) ||
                    (((entry & UserBit) != 0U) && ((state[page] & 8U) == 0U)) ||
                    (((entry & WritableBit) != 0U) && ((entry & ExecuteDisableBit) == 0U)))
                {
                    return RuntimePageTableMapError::InvalidTableEntry;
                }
                const Foundation::UInt64 leafPhysical = entry & physicalAddressMask;
                if ((leafPhysical >= requestedPhysicalStart) && (leafPhysical < requestedPhysicalEnd) &&
                    ((requestedWritable && ((entry & ExecuteDisableBit) == 0U)) ||
                     (requestedExecutable && ((entry & WritableBit) != 0U))))
                {
                    return RuntimePageTableMapError::PhysicalWriteExecuteAlias;
                }
                const Foundation::UInt64 storagePhysical = storage.PhysicalStart.GetValue();
                const Foundation::UInt64 storagePhysicalEnd =
                    storagePhysical + (static_cast<Foundation::UInt64>(storage.PageCapacity) << 12U);
                if ((leafPhysical >= storagePhysical) && (leafPhysical < storagePhysicalEnd) &&
                    (((entry & UserBit) != 0U) || ((entry & WritableBit) == 0U) ||
                     ((entry & ExecuteDisableBit) == 0U) ||
                     ((entry & (PageWriteThroughBit | PageCacheDisableBit | PageAttributeTableBit)) != 0U)))
                {
                    return RuntimePageTableMapError::InvalidTableEntry;
                }
                continue;
            }

            Foundation::Size childPage{0U};
            const RuntimePageTableMapError childError =
                DecodeChildPage(entry, page, physicalAddressMask, storage, childPage);
            if ((childError != RuntimePageTableMapError::None) || (state[childPage] != 0U))
            {
                return RuntimePageTableMapError::InvalidTableEntry;
            }
            state[childPage] = static_cast<Foundation::UInt8>(
                (level - 1U) | (((state[page] & 8U) != 0U && (entry & UserBit) != 0U) ? 8U : 0U));
        }
    }
    return RuntimePageTableMapError::None;
}

void ClearTablePage(const RuntimePageTableStorage& storage, const Foundation::Size page) noexcept
{
    const Foundation::Size firstEntry = page * PageTableEntriesPerPage;
    for (Foundation::Size index{0U}; index < PageTableEntriesPerPage; ++index)
    {
        StoreEntryRelaxed(&storage.Entries[firstEntry + index], 0U);
    }
}

[[nodiscard]] Foundation::Size GetOrCreateChildPage(const RuntimePageTableStorage& storage,
                                                    const Foundation::Size parentPage,
                                                    const Foundation::Size entryIndex,
                                                    const Foundation::UInt64 physicalAddressMask,
                                                    const bool userAccessible, Foundation::Size& nextPage) noexcept
{
    volatile Foundation::UInt64* const entry = &storage.Entries[(parentPage * PageTableEntriesPerPage) + entryIndex];
    const Foundation::UInt64 current = LoadEntry(entry);
    if (current != 0U)
    {
        if (userAccessible && ((current & UserBit) == 0U))
        {
            AtomicUInt64Reference{*entry}.FetchOrRelease(UserBit);
        }
        const Foundation::UInt64 childPhysical = current & physicalAddressMask;
        return static_cast<Foundation::Size>((childPhysical - storage.PhysicalStart.GetValue()) >> 12U);
    }

    const Foundation::Size childPage = nextPage;
    ++nextPage;
    ClearTablePage(storage, childPage);
    const Foundation::UInt64 childPhysical =
        storage.PhysicalStart.GetValue() + (static_cast<Foundation::UInt64>(childPage) << 12U);
    const Foundation::UInt64 flags = PresentBit | WritableBit | (userAccessible ? UserBit : 0U);
    StoreEntryRelease(entry, childPhysical | flags);
    return childPage;
}

[[nodiscard]] constexpr Foundation::UInt64 EncodeLeaf(const AddressSpaceMappingRecord& mapping,
                                                      const Foundation::UInt64 physicalAddress) noexcept
{
    Foundation::UInt64 value = physicalAddress | PresentBit | GetCacheBits(mapping.CachePolicy);
    if (mapping.Permissions.Contains(PagePermission::Write))
    {
        value |= WritableBit;
    }
    if (mapping.Permissions.Contains(PagePermission::User))
    {
        value |= UserBit;
    }
    if (!mapping.Permissions.Contains(PagePermission::Execute))
    {
        value |= ExecuteDisableBit;
    }
    return value;
}

void CommitMapping(const AddressSpaceMappingRecord& mapping, const Foundation::UInt64 physicalAddressMask,
                   const RuntimePageTableStorage& storage, Foundation::Size& nextPage) noexcept
{
    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 physicalStart = mapping.PhysicalStart.GetValue();
    const Foundation::UInt64 pageCount = mapping.Pages.GetValue();
    const bool userAccessible = mapping.Permissions.Contains(PagePermission::User);

    for (Foundation::UInt64 pageOffset{0U}; pageOffset < pageCount; ++pageOffset)
    {
        const Foundation::UInt64 virtualAddress = virtualStart + (pageOffset << 12U);
        Foundation::Size tablePage{0U};
        tablePage =
            GetOrCreateChildPage(storage, tablePage, static_cast<Foundation::Size>((virtualAddress >> 39U) & 0x1FFU),
                                 physicalAddressMask, userAccessible, nextPage);
        tablePage =
            GetOrCreateChildPage(storage, tablePage, static_cast<Foundation::Size>((virtualAddress >> 30U) & 0x1FFU),
                                 physicalAddressMask, userAccessible, nextPage);
        tablePage =
            GetOrCreateChildPage(storage, tablePage, static_cast<Foundation::Size>((virtualAddress >> 21U) & 0x1FFU),
                                 physicalAddressMask, userAccessible, nextPage);
        volatile Foundation::UInt64* const leaf =
            &storage.Entries[(tablePage * PageTableEntriesPerPage) +
                             static_cast<Foundation::Size>((virtualAddress >> 12U) & 0x1FFU)];
        const Foundation::UInt64 physicalAddress = physicalStart + (pageOffset << 12U);
        StoreEntryRelease(leaf, EncodeLeaf(mapping, physicalAddress));
    }
}

/// The checks Map and Unmap share: the record, the capabilities and the
/// arena, before any table byte is read; returns the physical-address mask.
[[nodiscard]] RuntimePageTableMapError PreflightRecord(const AddressSpaceMappingRecord& mapping,
                                                       const PagingCapabilities& capabilities,
                                                       const RuntimePageTableStorage& storage,
                                                       Foundation::UInt64& physicalAddressMask) noexcept
{
    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 physicalStart = mapping.PhysicalStart.GetValue();
    const Foundation::UInt64 pageCount = mapping.Pages.GetValue();
    if (!mapping.RuntimeMapping || !mapping.Frame.IsValid() || ((virtualStart & PageOffsetMask) != 0U) ||
        (physicalStart == 0U) || ((physicalStart & PageOffsetMask) != 0U) || (pageCount == 0U) ||
        !mapping.Permissions.HasOnlyKnownBits() || !mapping.Permissions.Contains(PagePermission::Read) ||
        (mapping.Permissions.Contains(PagePermission::Write) &&
         mapping.Permissions.Contains(PagePermission::Execute)) ||
        !IsKnownCachePolicy(mapping.CachePolicy) || !IsKnownFrameKind(mapping.Kind) ||
        ((mapping.Kind == FrameKind::Device) && (mapping.CachePolicy != MemoryCachePolicy::Uncached) &&
         (mapping.CachePolicy != MemoryCachePolicy::WriteCombining)) ||
        ((mapping.Kind == FrameKind::Device) && mapping.Permissions.Contains(PagePermission::Execute)) ||
        !ReservedBytesAreZero(mapping))
    {
        return RuntimePageTableMapError::InvalidMappingPlan;
    }
    if (pageCount > MaximumRuntimeMappingPages)
    {
        return RuntimePageTableMapError::MappingTooLarge;
    }

    const Foundation::UInt64 sizeBytes = pageCount << 12U;
    if ((virtualStart > (MaximumUInt64 - sizeBytes)) || !IsCanonicalRange(virtualStart, virtualStart + sizeBytes))
    {
        return RuntimePageTableMapError::NonCanonicalVirtualAddress;
    }
    if (mapping.Permissions.Contains(PagePermission::User) &&
        !IsUserAddressRange(virtualStart, virtualStart + sizeBytes))
    {
        return RuntimePageTableMapError::InvalidMappingPlan;
    }

    if ((capabilities.PhysicalAddressBits < 32U) || (capabilities.PhysicalAddressBits > 52U) ||
        (capabilities.Reserved16 != 0U) || (capabilities.Reserved != 0U))
    {
        return RuntimePageTableMapError::InvalidCapabilities;
    }
    if (!capabilities.FourLevelPagingEnabled)
    {
        return RuntimePageTableMapError::FourLevelPagingRequired;
    }
    if (!capabilities.SupervisorWriteProtectEnabled)
    {
        return RuntimePageTableMapError::SupervisorWriteProtectRequired;
    }
    if (!capabilities.PatLayoutConfigured)
    {
        return RuntimePageTableMapError::PageAttributeTableRequired;
    }
    if (!capabilities.ExecuteDisableEnabled)
    {
        return RuntimePageTableMapError::ExecuteDisableRequired;
    }

    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << capabilities.PhysicalAddressBits;
    physicalAddressMask = (physicalLimit - 1U) & ArchitecturalPhysicalAddressMask;
    if ((physicalStart >= physicalLimit) || (sizeBytes > (physicalLimit - physicalStart)))
    {
        return RuntimePageTableMapError::PhysicalAddressOutOfRange;
    }

    const Foundation::UInt64 storagePhysical = storage.PhysicalStart.GetValue();
    if ((storage.Entries == nullptr) || (storage.PageCapacity == 0U) ||
        !IsRuntimePageTableCapacity(storage.PageCapacity) || (storage.UsedPageCount == 0U) ||
        (storage.UsedPageCount > storage.PageCapacity) || (storage.Reserved != 0U) || (storagePhysical == 0U) ||
        ((storagePhysical & PageOffsetMask) != 0U) ||
        ((reinterpret_cast<Foundation::UIntPointer>(storage.Entries) & PageOffsetMask) != 0U))
    {
        return RuntimePageTableMapError::InvalidStorage;
    }
    const Foundation::UInt64 storageBytes = static_cast<Foundation::UInt64>(storage.PageCapacity) << 12U;
    if ((storagePhysical >= physicalLimit) || (storageBytes > (physicalLimit - storagePhysical)))
    {
        return RuntimePageTableMapError::InvalidStorage;
    }
    const Foundation::UInt64 storagePhysicalEnd = storagePhysical + storageBytes;
    if (RangesOverlap(physicalStart, physicalStart + sizeBytes, storagePhysical, storagePhysicalEnd))
    {
        return RuntimePageTableMapError::MappingAliasesTableStorage;
    }

    const Foundation::UInt64 storageVirtualStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage.Entries));
    if (storageVirtualStart > (MaximumUInt64 - storageBytes))
    {
        return RuntimePageTableMapError::InvalidStorage;
    }
    return RuntimePageTableMapError::None;
}

/// The leaf entry that maps virtualAddress, through present ancestors only.
[[nodiscard]] RuntimePageTableMapError FindLeaf(const Foundation::UInt64 virtualAddress,
                                                const Foundation::UInt64 physicalAddressMask,
                                                const RuntimePageTableStorage& storage,
                                                volatile Foundation::UInt64*& leaf) noexcept
{
    constexpr Foundation::UInt8 AncestorShifts[3U]{39U, 30U, 21U};
    Foundation::Size tablePage{0U};
    for (const Foundation::UInt8 shift : AncestorShifts)
    {
        const Foundation::UInt64 entry =
            LoadEntry(&storage.Entries[(tablePage * PageTableEntriesPerPage) +
                                       static_cast<Foundation::Size>((virtualAddress >> shift) & 0x1FFU)]);
        if (entry == 0U)
        {
            return RuntimePageTableMapError::MappingAbsent;
        }
        const RuntimePageTableMapError error =
            DecodeChildPage(entry, tablePage, physicalAddressMask, storage, tablePage);
        if (error != RuntimePageTableMapError::None)
        {
            return error;
        }
    }
    leaf = &storage.Entries[(tablePage * PageTableEntriesPerPage) +
                            static_cast<Foundation::Size>((virtualAddress >> 12U) & 0x1FFU)];
    return RuntimePageTableMapError::None;
}

/// Every page of mapping must hold exactly the leaf Map installed for it; the
/// processor may since have set its accessed and dirty bits.
[[nodiscard]] RuntimePageTableMapError InspectInstalledMapping(const AddressSpaceMappingRecord& mapping,
                                                               const Foundation::UInt64 physicalAddressMask,
                                                               const RuntimePageTableStorage& storage) noexcept
{
    for (Foundation::UInt64 pageOffset{0U}; pageOffset < mapping.Pages.GetValue(); ++pageOffset)
    {
        volatile Foundation::UInt64* leaf{nullptr};
        const RuntimePageTableMapError error =
            FindLeaf(mapping.VirtualStart.GetValue() + (pageOffset << 12U), physicalAddressMask, storage, leaf);
        if (error != RuntimePageTableMapError::None)
        {
            return error;
        }
        const Foundation::UInt64 entry = LoadEntry(leaf);
        if (entry == 0U)
        {
            return RuntimePageTableMapError::MappingAbsent;
        }
        if ((entry & ~(AccessedBit | DirtyBit)) !=
            EncodeLeaf(mapping, mapping.PhysicalStart.GetValue() + (pageOffset << 12U)))
        {
            return RuntimePageTableMapError::MappingMismatch;
        }
    }
    return RuntimePageTableMapError::None;
}
} // Namespace

RuntimePageTableMapError RuntimePageTableMapper::Map(const AddressSpaceMappingRecord& mapping,
                                                     const PagingCapabilities& capabilities,
                                                     const RuntimePageTableStorage& storage,
                                                     RuntimePageTableMapResult& output) noexcept
{
    Foundation::UInt64 physicalAddressMask{0U};
    const RuntimePageTableMapError preflightError =
        PreflightRecord(mapping, capabilities, storage, physicalAddressMask);
    if (preflightError != RuntimePageTableMapError::None)
    {
        return preflightError;
    }
    Foundation::Size requiredPages{0U};
    const RuntimePageTableMapError treeError =
        ValidateExistingTree(mapping, physicalAddressMask, capabilities.GlobalPagesEnabled, storage);
    if (treeError != RuntimePageTableMapError::None)
    {
        return treeError;
    }
    const RuntimePageTableMapError inspectionError =
        InspectMapping(mapping, physicalAddressMask, storage, requiredPages);
    if (inspectionError != RuntimePageTableMapError::None)
    {
        return inspectionError;
    }
    if (requiredPages > (storage.PageCapacity - storage.UsedPageCount))
    {
        return RuntimePageTableMapError::InsufficientTableStorage;
    }

    Foundation::Size nextPage = storage.UsedPageCount;
    CommitMapping(mapping, physicalAddressMask, storage, nextPage);
    output = RuntimePageTableMapResult{
        .UsedTablePages = nextPage,
        .MappedPages = mapping.Pages,
        .InvalidationStart = mapping.VirtualStart,
        .Reserved = 0U,
    };
    return RuntimePageTableMapError::None;
}

RuntimePageTableMapError RuntimePageTableMapper::Unmap(const AddressSpaceMappingRecord& mapping,
                                                       const PagingCapabilities& capabilities,
                                                       const RuntimePageTableStorage& storage,
                                                       RuntimePageTableMapResult& output) noexcept
{
    Foundation::UInt64 physicalAddressMask{0U};
    const RuntimePageTableMapError preflightError =
        PreflightRecord(mapping, capabilities, storage, physicalAddressMask);
    if (preflightError != RuntimePageTableMapError::None)
    {
        return preflightError;
    }
    const RuntimePageTableMapError treeError =
        ValidateExistingTree(mapping, physicalAddressMask, capabilities.GlobalPagesEnabled, storage);
    if (treeError != RuntimePageTableMapError::None)
    {
        return treeError;
    }
    const RuntimePageTableMapError inspectionError = InspectInstalledMapping(mapping, physicalAddressMask, storage);
    if (inspectionError != RuntimePageTableMapError::None)
    {
        return inspectionError;
    }

    for (Foundation::UInt64 pageOffset{0U}; pageOffset < mapping.Pages.GetValue(); ++pageOffset)
    {
        volatile Foundation::UInt64* leaf{nullptr};
        static_cast<void>(
            FindLeaf(mapping.VirtualStart.GetValue() + (pageOffset << 12U), physicalAddressMask, storage, leaf));
        StoreEntryRelease(leaf, 0U);
    }
    output = RuntimePageTableMapResult{
        .UsedTablePages = storage.UsedPageCount,
        .MappedPages = mapping.Pages,
        .InvalidationStart = mapping.VirtualStart,
        .Reserved = 0U,
    };
    return RuntimePageTableMapError::None;
}
} // Namespace Asteria::Platform::Amd64::Memory
