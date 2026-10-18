// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialInterruptVectorTablePreparer.hpp"

#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Memory/InitialRseBackingStorePreparer.hpp"

#if defined(__ia64__)
extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetBootstrapInterruptVectorTableAddress() noexcept;
#endif

namespace Asteria::Platform::Ia64::Memory
{
Foundation::UInt64 GetStaticBootstrapInterruptVectorTableAddress() noexcept
{
#if defined(__ia64__)
    return AsteriaIa64GetBootstrapInterruptVectorTableAddress();
#else
    // Hosted stand-in has the same immutable bundle bytes and table alignment.
    struct Table
    {
        Foundation::UInt8 Bytes[InitialInterruptVectorTableSizeBytes]{};
        constexpr Table() noexcept
        {
            for (Foundation::Size offset{0U}; offset < sizeof(Bytes); offset += 16U)
            {
                Bytes[offset] = 0x11U;
                Bytes[offset + 4U] = 0x01U;
                Bytes[offset + 9U] = 0x02U;
                Bytes[offset + 15U] = 0x40U;
            }
        }
    };
    alignas(InitialInterruptVectorTableSizeBytes) static constinit Table table{};
    return reinterpret_cast<Foundation::UIntPointer>(table.Bytes);
#endif
}

namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt16 RequiredPermissions{
    static_cast<Foundation::UInt16>(Kernel::Memory::MappingPermission::Read) |
    static_cast<Foundation::UInt16>(Kernel::Memory::MappingPermission::Execute) |
    static_cast<Foundation::UInt16>(Kernel::Memory::MappingPermission::Global)};
constexpr Foundation::UInt8 FailStopBundle[InitialInterruptVectorBundleSizeBytes]{
    0x11U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U,
};

[[nodiscard]] bool TryCalculateEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                   Foundation::UInt64& end) noexcept
{
    if (start > (MaximumUInt64 - sizeBytes))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] bool TryCalculatePointerEnd(const Foundation::UIntPointer start, const Foundation::Size sizeBytes,
                                          Foundation::UIntPointer& end) noexcept
{
    if (start > (MaximumUIntPointer - sizeBytes))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] bool StorageOverlapsObject(const Foundation::UIntPointer storageStart,
                                         const Foundation::UIntPointer storageEnd, const void* const object,
                                         const Foundation::Size objectSize) noexcept
{
    const Foundation::UIntPointer objectStart = reinterpret_cast<Foundation::UIntPointer>(object);
    Foundation::UIntPointer objectEnd{0U};
    return !TryCalculatePointerEnd(objectStart, objectSize, objectEnd) ||
           RangesOverlap(storageStart, storageEnd, objectStart, objectEnd);
}

[[nodiscard]] bool StorageMayOverlapObject(const WritableInterruptVectorTableView& storage, const void* const object,
                                           const Foundation::Size objectSize) noexcept
{
    if (storage.Data == nullptr)
    {
        return false;
    }

    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(storage.Data);
    const Foundation::Size protectedStorageSize = storage.SizeBytes > InitialInterruptVectorTableSizeBytes
                                                      ? storage.SizeBytes
                                                      : InitialInterruptVectorTableSizeBytes;
    const Foundation::UIntPointer objectStart = reinterpret_cast<Foundation::UIntPointer>(object);
    Foundation::UIntPointer objectEnd{0U};
    if (!TryCalculatePointerEnd(objectStart, objectSize, objectEnd))
    {
        return true;
    }

    Foundation::UIntPointer storageEnd{0U};
    if (!TryCalculatePointerEnd(storageStart, protectedStorageSize, storageEnd))
    {
        return storageStart < objectEnd;
    }
    return RangesOverlap(storageStart, storageEnd, objectStart, objectEnd);
}

[[nodiscard]] bool TryGetRseStorageRange(const WritableRseBackingStoreView& storage, Foundation::UIntPointer& start,
                                         Foundation::UIntPointer& end) noexcept
{
    if (storage.Data == nullptr)
    {
        return false;
    }
    start = reinterpret_cast<Foundation::UIntPointer>(storage.Data);
    const Foundation::Size protectedStorageSize =
        storage.SizeBytes > InitialRseBackingStoreSizeBytes ? storage.SizeBytes : InitialRseBackingStoreSizeBytes;
    return TryCalculatePointerEnd(start, protectedStorageSize, end);
}

void WriteFailStopTable(const WritableInterruptVectorTableView storage) noexcept
{
#if defined(__ia64__)
    constexpr Foundation::UInt64 BundleLow{0x0000'0001'0000'0011U};
    constexpr Foundation::UInt64 BundleHigh{0x4000'0000'0000'0200U};
    Foundation::UInt8* cursor = storage.Data;
    Foundation::UInt8* const end = storage.Data + storage.SizeBytes;
    while (cursor != end)
    {
        __asm__ volatile("st8 [%0]=%1,8\n;;\n"
                         "st8 [%0]=%2,8\n;;"
                         : "+r"(cursor)
                         : "r"(BundleLow), "r"(BundleHigh)
                         : "memory");
    }
#else
    for (Foundation::Size offset{0U}; offset < storage.SizeBytes; offset += InitialInterruptVectorBundleSizeBytes)
    {
        for (Foundation::Size byteIndex{0U}; byteIndex < InitialInterruptVectorBundleSizeBytes; ++byteIndex)
        {
            storage.Data[offset + byteIndex] = FailStopBundle[byteIndex];
        }
    }
#endif
}

void PublishInstructionBytes(const WritableInterruptVectorTableView storage) noexcept
{
#if defined(__ia64__)
    for (Foundation::Size offset{0U}; offset < storage.SizeBytes; offset += InitialInterruptVectorBundleSizeBytes)
    {
        __asm__ volatile("fc.i %0" : : "r"(storage.Data + offset) : "memory");
    }
    __asm__ volatile(";; sync.i\n;; srlz.i\n;;" ::: "memory");
#else
    static_cast<void>(storage);
#endif
}
} // namespace

InitialInterruptVectorTablePreparationError
InitialInterruptVectorTablePreparer::Validate(const Kernel::Memory::InitialMappingPlan& mappingPlan,
                                              const BootstrapMappingPlanSummary& mappingSummary,
                                              const WritableInterruptVectorTableView storage) noexcept
{
    using Kernel::Memory::InitialMappingOwner;
    using Kernel::Memory::MappingPageSizeBytes;
    using Kernel::Memory::MemoryCachePolicy;

    if (!mappingPlan.IsSealed() || !IsBootstrapMappingCountValid(mappingPlan.GetMappingCount()))
    {
        return InitialInterruptVectorTablePreparationError::InvalidMappingPlan;
    }

    Foundation::UInt64 reservedEnd{0U};
    if ((mappingSummary.StackPhysicalStart == 0U) ||
        ((mappingSummary.StackPhysicalStart & ((BootstrapReservedAlignmentPages * MappingPageSizeBytes) - 1U)) != 0U) ||
        !IsBootstrapMappingCountValid(mappingSummary.MappingCount) || (mappingSummary.Reserved16 != 0U) ||
        (mappingSummary.Reserved32 != 0U) ||
        !TryCalculateEnd(mappingSummary.StackPhysicalStart, BootstrapReservedPageCount * MappingPageSizeBytes,
                         reservedEnd) ||
        (mappingSummary.TranslationStoragePhysicalStart !=
         (mappingSummary.StackPhysicalStart + (BootstrapTranslationStoragePageOffset * MappingPageSizeBytes))))
    {
        return InitialInterruptVectorTablePreparationError::InvalidMappingSummary;
    }

    const Foundation::UInt64 expectedPhysicalStart = GetBootstrapInterruptVectorTablePhysicalStart(mappingSummary);
    Foundation::UInt64 expectedPhysicalEnd{0U};
    if (!TryCalculateEnd(expectedPhysicalStart, InitialInterruptVectorTableSizeBytes, expectedPhysicalEnd) ||
        ((mappingSummary.MappingCount != BootstrapConsolidatedMappingCount) && (expectedPhysicalEnd > reservedEnd)) ||
        ((expectedPhysicalStart & (InitialInterruptVectorTableSizeBytes - 1U)) != 0U))
    {
        return InitialInterruptVectorTablePreparationError::InvalidMappingSummary;
    }

    if ((storage.Data == nullptr) || (storage.SizeBytes != InitialInterruptVectorTableSizeBytes))
    {
        return InitialInterruptVectorTablePreparationError::InvalidStorageView;
    }
    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(storage.Data);
    Foundation::UIntPointer storageEnd{0U};
    if (((storageStart & (InitialInterruptVectorTableSizeBytes - 1U)) != 0U) ||
        !TryCalculatePointerEnd(storageStart, storage.SizeBytes, storageEnd))
    {
        return InitialInterruptVectorTablePreparationError::InvalidStorageView;
    }
    if (storage.PhysicalStart != expectedPhysicalStart)
    {
        return InitialInterruptVectorTablePreparationError::StorageAddressMismatch;
    }
#if defined(__ia64__)
    if (storageStart != static_cast<Foundation::UIntPointer>(storage.PhysicalStart))
    {
        return InitialInterruptVectorTablePreparationError::StorageAddressMismatch;
    }
#endif
    if (StorageOverlapsObject(storageStart, storageEnd, &mappingPlan, sizeof(mappingPlan)) ||
        StorageOverlapsObject(storageStart, storageEnd, &mappingSummary, sizeof(mappingSummary)))
    {
        return InitialInterruptVectorTablePreparationError::StorageAliasesInput;
    }

    if (mappingSummary.MappingCount == BootstrapConsolidatedMappingCount)
    {
        if (storageStart != GetStaticBootstrapInterruptVectorTableAddress())
        {
            return InitialInterruptVectorTablePreparationError::StorageAddressMismatch;
        }
        for (Foundation::Size index{0U}; index < mappingPlan.GetMappingCount(); ++index)
        {
            const auto result = mappingPlan.GetMapping(index);
            if (result.HasValue())
            {
                const auto& mapping = result.GetValue();
                const Foundation::UInt64 start = mapping.PhysicalStart.GetValue();
                if ((mapping.Owner == InitialMappingOwner::NucleusCode) && (mapping.VirtualStart.GetValue() == start) &&
                    (mapping.Permissions.GetBits() == RequiredPermissions) &&
                    (mapping.CachePolicy == MemoryCachePolicy::WriteBack) && (start <= expectedPhysicalStart) &&
                    (mapping.Pages.GetValue() * MappingPageSizeBytes >= expectedPhysicalEnd - start))
                {
                    return InitialInterruptVectorTablePreparationError::None;
                }
            }
        }
        return InitialInterruptVectorTablePreparationError::MissingTableMapping;
    }

    Foundation::Size tableMappingCount{0U};
    for (Foundation::Size index{0U}; index < mappingPlan.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappingPlan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return InitialInterruptVectorTablePreparationError::InvalidMappingPlan;
        }
        const Kernel::Memory::InitialMapping& mapping = mappingResult.GetValue();
        Foundation::UInt64 mappingPhysicalEnd{0U};
        if (!TryCalculateEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue() * MappingPageSizeBytes,
                             mappingPhysicalEnd))
        {
            return InitialInterruptVectorTablePreparationError::InvalidMappingPlan;
        }
        const bool overlapsTable = RangesOverlap(mapping.PhysicalStart.GetValue(), mappingPhysicalEnd,
                                                 expectedPhysicalStart, expectedPhysicalEnd);

        if (mapping.Owner == InitialMappingOwner::NucleusInterruptVectorTable)
        {
            ++tableMappingCount;
            if ((mapping.VirtualStart.GetValue() != expectedPhysicalStart) ||
                (mapping.PhysicalStart.GetValue() != expectedPhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapInterruptVectorTablePageCount) ||
                (mapping.Permissions.GetBits() != RequiredPermissions) ||
                (mapping.CachePolicy != MemoryCachePolicy::WriteBack) || (mapping.Reserved != 0U) || !overlapsTable)
            {
                return InitialInterruptVectorTablePreparationError::InvalidTableMapping;
            }
        }
        else if (overlapsTable)
        {
            return InitialInterruptVectorTablePreparationError::ConflictingTableMapping;
        }
    }

    if (tableMappingCount == 0U)
    {
        return InitialInterruptVectorTablePreparationError::MissingTableMapping;
    }
    if (tableMappingCount != 1U)
    {
        return InitialInterruptVectorTablePreparationError::ConflictingTableMapping;
    }

    return InitialInterruptVectorTablePreparationError::None;
}

InitialInterruptVectorTablePreparationError
InitialInterruptVectorTablePreparer::Prepare(const Kernel::Memory::InitialMappingPlan& mappingPlan,
                                             const BootstrapMappingPlanSummary& mappingSummary,
                                             const WritableInterruptVectorTableView storage) noexcept
{
    const InitialInterruptVectorTablePreparationError validationError = Validate(mappingPlan, mappingSummary, storage);
    if (validationError != InitialInterruptVectorTablePreparationError::None)
    {
        return validationError;
    }

    if (mappingSummary.MappingCount == BootstrapConsolidatedMappingCount)
    {
        return InitialInterruptVectorTablePreparationError::None;
    }
    WriteFailStopTable(storage);
    PublishInstructionBytes(storage);
    return InitialInterruptVectorTablePreparationError::None;
}

InitialInterruptVectorTablePreparationError InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
    const InitialAddressSpacePreparation& preparation, const Firmware::Ia64LoadedImageLayout& image,
    const Boot::BootInfo& finalBootInfo, const Kernel::Memory::PhysicalPageRange& reservedPages,
    const Kernel::Memory::PhysicalPageRange& runtimeMemoryPages, const PalVirtualMemoryCapabilities& capabilities,
    const Firmware::SalSystemTableInformation& firmware, const InitialTranslationRegionSet& regions,
    const WritableInterruptVectorTableView& storage, const WritableRseBackingStoreView& rseStorage) noexcept
{
    if (StorageMayOverlapObject(storage, &preparation, sizeof(preparation)))
    {
        return InitialInterruptVectorTablePreparationError::StorageAliasesPreparation;
    }

    if ((storage.Data == nullptr) || (storage.SizeBytes != InitialInterruptVectorTableSizeBytes))
    {
        return InitialInterruptVectorTablePreparationError::InvalidStorageView;
    }
    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(storage.Data);
    Foundation::UIntPointer storageEnd{0U};
    if (((storageStart & (InitialInterruptVectorTableSizeBytes - 1U)) != 0U) ||
        !TryCalculatePointerEnd(storageStart, storage.SizeBytes, storageEnd))
    {
        return InitialInterruptVectorTablePreparationError::InvalidStorageView;
    }

    Foundation::UInt64 expectedPhysicalStart{0U};
    if (!TryCalculateEnd(reservedPages.PhysicalStart,
                         (reservedPages.PhysicalStart == runtimeMemoryPages.PhysicalStart)
                             ? Foundation::UInt64{0U}
                             : BootstrapInterruptVectorTablePageOffset * Kernel::Memory::MappingPageSizeBytes,
                         expectedPhysicalStart) ||
        (storage.PhysicalStart != ((reservedPages.PhysicalStart == runtimeMemoryPages.PhysicalStart)
                                       ? GetStaticBootstrapInterruptVectorTableAddress()
                                       : expectedPhysicalStart)) ||
        ((expectedPhysicalStart & (InitialInterruptVectorTableSizeBytes - 1U)) != 0U))
    {
        return InitialInterruptVectorTablePreparationError::StorageAddressMismatch;
    }
#if defined(__ia64__)
    if (storageStart != static_cast<Foundation::UIntPointer>(storage.PhysicalStart))
    {
        return InitialInterruptVectorTablePreparationError::StorageAddressMismatch;
    }
#endif

    Foundation::UIntPointer rseStorageStart{0U};
    Foundation::UIntPointer rseStorageEnd{0U};
    const bool hasRseStorageRange = TryGetRseStorageRange(rseStorage, rseStorageStart, rseStorageEnd);

    if (StorageOverlapsObject(storageStart, storageEnd, &image, sizeof(image)) ||
        StorageOverlapsObject(storageStart, storageEnd, &finalBootInfo, sizeof(finalBootInfo)) ||
        StorageOverlapsObject(storageStart, storageEnd, &reservedPages, sizeof(reservedPages)) ||
        StorageOverlapsObject(storageStart, storageEnd, &runtimeMemoryPages, sizeof(runtimeMemoryPages)) ||
        StorageOverlapsObject(storageStart, storageEnd, &capabilities, sizeof(capabilities)) ||
        StorageOverlapsObject(storageStart, storageEnd, &firmware, sizeof(firmware)) ||
        StorageOverlapsObject(storageStart, storageEnd, &regions, sizeof(regions)) ||
        StorageOverlapsObject(storageStart, storageEnd, &storage, sizeof(storage)) ||
        StorageOverlapsObject(storageStart, storageEnd, &rseStorage, sizeof(rseStorage)) ||
        (hasRseStorageRange && (StorageOverlapsObject(rseStorageStart, rseStorageEnd, &storage, sizeof(storage)) ||
                                RangesOverlap(storageStart, storageEnd, rseStorageStart, rseStorageEnd))))
    {
        return InitialInterruptVectorTablePreparationError::StorageAliasesInput;
    }

    return InitialInterruptVectorTablePreparationError::None;
}
} // namespace Asteria::Platform::Ia64::Memory
