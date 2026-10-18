// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialRseBackingStorePreparer.hpp"

#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt16 RequiredPermissions{
    static_cast<Foundation::UInt16>(Kernel::Memory::MappingPermission::Read) |
    static_cast<Foundation::UInt16>(Kernel::Memory::MappingPermission::Write) |
    static_cast<Foundation::UInt16>(Kernel::Memory::MappingPermission::Global)};

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

[[nodiscard]] bool StorageMayOverlapObject(const WritableRseBackingStoreView& storage, const void* const object,
                                           const Foundation::Size objectSize) noexcept
{
    if (storage.Data == nullptr)
    {
        return false;
    }

    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(storage.Data);
    const Foundation::Size protectedStorageSize =
        storage.SizeBytes > InitialRseBackingStoreSizeBytes ? storage.SizeBytes : InitialRseBackingStoreSizeBytes;
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
} // namespace

InitialRseBackingStorePreparationError
InitialRseBackingStorePreparer::Validate(const Kernel::Memory::InitialMappingPlan& mappingPlan,
                                         const BootstrapMappingPlanSummary& mappingSummary,
                                         const WritableRseBackingStoreView storage) noexcept
{
    using Kernel::Memory::InitialMappingOwner;
    using Kernel::Memory::MappingPageSizeBytes;
    using Kernel::Memory::MemoryCachePolicy;

    if (!mappingPlan.IsSealed() || !IsBootstrapMappingCountValid(mappingPlan.GetMappingCount()))
    {
        return InitialRseBackingStorePreparationError::InvalidMappingPlan;
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
        return InitialRseBackingStorePreparationError::InvalidMappingSummary;
    }

    const Foundation::UInt64 expectedPhysicalStart = GetBootstrapRseBackingStorePhysicalStart(mappingSummary);
    Foundation::UInt64 expectedPhysicalEnd{0U};
    if (!TryCalculateEnd(expectedPhysicalStart, InitialRseBackingStoreSizeBytes, expectedPhysicalEnd) ||
        ((mappingSummary.MappingCount != BootstrapConsolidatedMappingCount) && (expectedPhysicalEnd > reservedEnd)))
    {
        return InitialRseBackingStorePreparationError::InvalidMappingSummary;
    }

    if ((storage.Data == nullptr) || (storage.SizeBytes != InitialRseBackingStoreSizeBytes))
    {
        return InitialRseBackingStorePreparationError::InvalidStorageView;
    }
    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(storage.Data);
    Foundation::UIntPointer storageEnd{0U};
    if (((storageStart & (MappingPageSizeBytes - 1U)) != 0U) ||
        !TryCalculatePointerEnd(storageStart, storage.SizeBytes, storageEnd))
    {
        return InitialRseBackingStorePreparationError::InvalidStorageView;
    }
    if (storage.PhysicalStart != expectedPhysicalStart)
    {
        return InitialRseBackingStorePreparationError::StorageAddressMismatch;
    }
#if defined(__ia64__)
    if (storageStart != static_cast<Foundation::UIntPointer>(storage.PhysicalStart))
    {
        return InitialRseBackingStorePreparationError::StorageAddressMismatch;
    }
#endif
    if (StorageOverlapsObject(storageStart, storageEnd, &mappingPlan, sizeof(mappingPlan)) ||
        StorageOverlapsObject(storageStart, storageEnd, &mappingSummary, sizeof(mappingSummary)))
    {
        return InitialRseBackingStorePreparationError::StorageAliasesInput;
    }

    if (mappingSummary.MappingCount == BootstrapConsolidatedMappingCount)
    {
        for (Foundation::Size index{0U}; index < mappingPlan.GetMappingCount(); ++index)
        {
            const auto result = mappingPlan.GetMapping(index);
            if (!result.HasValue())
            {
                return InitialRseBackingStorePreparationError::InvalidMappingPlan;
            }
            const auto& mapping = result.GetValue();
            const Foundation::UInt64 start = mapping.PhysicalStart.GetValue();
            if ((mapping.Owner == InitialMappingOwner::NucleusBootstrapRuntimeData) &&
                (start == mappingSummary.StackPhysicalStart) && (mapping.VirtualStart.GetValue() == start) &&
                (mapping.Permissions.GetBits() == RequiredPermissions) &&
                (mapping.CachePolicy == MemoryCachePolicy::WriteBack) && (start <= expectedPhysicalStart) &&
                (mapping.Pages.GetValue() * MappingPageSizeBytes == expectedPhysicalEnd - start))
            {
                return InitialRseBackingStorePreparationError::None;
            }
        }
        return InitialRseBackingStorePreparationError::MissingBackingStoreMapping;
    }

    Foundation::Size backingStoreMappingCount{0U};
    for (Foundation::Size index{0U}; index < mappingPlan.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappingPlan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return InitialRseBackingStorePreparationError::InvalidMappingPlan;
        }
        const Kernel::Memory::InitialMapping& mapping = mappingResult.GetValue();
        Foundation::UInt64 mappingPhysicalEnd{0U};
        if (!TryCalculateEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue() * MappingPageSizeBytes,
                             mappingPhysicalEnd))
        {
            return InitialRseBackingStorePreparationError::InvalidMappingPlan;
        }
        const bool overlapsBackingStore = RangesOverlap(mapping.PhysicalStart.GetValue(), mappingPhysicalEnd,
                                                        expectedPhysicalStart, expectedPhysicalEnd);

        if (mapping.Owner == InitialMappingOwner::NucleusRegisterBackingStore)
        {
            ++backingStoreMappingCount;
            if ((mapping.VirtualStart.GetValue() != expectedPhysicalStart) ||
                (mapping.PhysicalStart.GetValue() != expectedPhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapRseBackingStorePageCount) ||
                (mapping.Permissions.GetBits() != RequiredPermissions) ||
                (mapping.CachePolicy != MemoryCachePolicy::WriteBack) || (mapping.Reserved != 0U) ||
                !overlapsBackingStore)
            {
                return InitialRseBackingStorePreparationError::InvalidBackingStoreMapping;
            }
        }
        else if (overlapsBackingStore)
        {
            return InitialRseBackingStorePreparationError::ConflictingBackingStoreMapping;
        }
    }

    if (backingStoreMappingCount == 0U)
    {
        return InitialRseBackingStorePreparationError::MissingBackingStoreMapping;
    }
    if (backingStoreMappingCount != 1U)
    {
        return InitialRseBackingStorePreparationError::ConflictingBackingStoreMapping;
    }

    return InitialRseBackingStorePreparationError::None;
}

InitialRseBackingStorePreparationError
InitialRseBackingStorePreparer::Prepare(const Kernel::Memory::InitialMappingPlan& mappingPlan,
                                        const BootstrapMappingPlanSummary& mappingSummary,
                                        const WritableRseBackingStoreView storage) noexcept
{
    const InitialRseBackingStorePreparationError validationError = Validate(mappingPlan, mappingSummary, storage);
    if (validationError != InitialRseBackingStorePreparationError::None)
    {
        return validationError;
    }

    static_cast<void>(memset(storage.Data, 0, storage.SizeBytes));
#if defined(__ia64__)
    __asm__ volatile("mf" ::: "memory");
#endif
    return InitialRseBackingStorePreparationError::None;
}

InitialRseBackingStorePreparationError InitialRseBackingStorePreparer::ValidatePreparationStorage(
    const InitialAddressSpacePreparation& preparation, const Firmware::Ia64LoadedImageLayout& image,
    const Boot::BootInfo& finalBootInfo, const Kernel::Memory::PhysicalPageRange& reservedPages,
    const Kernel::Memory::PhysicalPageRange& runtimeMemoryPages, const PalVirtualMemoryCapabilities& capabilities,
    const Firmware::SalSystemTableInformation& firmware, const InitialTranslationRegionSet& regions,
    const WritableRseBackingStoreView& storage) noexcept
{
    using Kernel::Memory::MappingPageSizeBytes;

    if (StorageMayOverlapObject(storage, &preparation, sizeof(preparation)))
    {
        return InitialRseBackingStorePreparationError::StorageAliasesPreparation;
    }

    if ((storage.Data == nullptr) || (storage.SizeBytes != InitialRseBackingStoreSizeBytes))
    {
        return InitialRseBackingStorePreparationError::InvalidStorageView;
    }
    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(storage.Data);
    Foundation::UIntPointer storageEnd{0U};
    if (((storageStart & (MappingPageSizeBytes - 1U)) != 0U) ||
        !TryCalculatePointerEnd(storageStart, storage.SizeBytes, storageEnd))
    {
        return InitialRseBackingStorePreparationError::InvalidStorageView;
    }

    Foundation::UInt64 expectedPhysicalStart{0U};
    if (reservedPages.PhysicalStart == runtimeMemoryPages.PhysicalStart)
    {
        Foundation::UInt64 runtimeEnd{0U};
        if ((runtimeMemoryPages.PageCount > (~Foundation::UInt64{0U} / MappingPageSizeBytes)) ||
            (runtimeMemoryPages.PageCount <= BootstrapReservedPageCount) ||
            !TryCalculateEnd(runtimeMemoryPages.PhysicalStart, runtimeMemoryPages.PageCount * MappingPageSizeBytes,
                             runtimeEnd))
        {
            return InitialRseBackingStorePreparationError::StorageAddressMismatch;
        }
        expectedPhysicalStart = runtimeEnd - InitialRseBackingStoreSizeBytes;
    }
    else if (!TryCalculateEnd(reservedPages.PhysicalStart, BootstrapRseBackingStorePageOffset * MappingPageSizeBytes,
                              expectedPhysicalStart))
    {
        return InitialRseBackingStorePreparationError::StorageAddressMismatch;
    }
    if (storage.PhysicalStart != expectedPhysicalStart)
    {
        return InitialRseBackingStorePreparationError::StorageAddressMismatch;
    }
#if defined(__ia64__)
    if (storageStart != static_cast<Foundation::UIntPointer>(storage.PhysicalStart))
    {
        return InitialRseBackingStorePreparationError::StorageAddressMismatch;
    }
#endif

    if (StorageOverlapsObject(storageStart, storageEnd, &image, sizeof(image)) ||
        StorageOverlapsObject(storageStart, storageEnd, &finalBootInfo, sizeof(finalBootInfo)) ||
        StorageOverlapsObject(storageStart, storageEnd, &reservedPages, sizeof(reservedPages)) ||
        StorageOverlapsObject(storageStart, storageEnd, &runtimeMemoryPages, sizeof(runtimeMemoryPages)) ||
        StorageOverlapsObject(storageStart, storageEnd, &capabilities, sizeof(capabilities)) ||
        StorageOverlapsObject(storageStart, storageEnd, &firmware, sizeof(firmware)) ||
        StorageOverlapsObject(storageStart, storageEnd, &regions, sizeof(regions)) ||
        StorageOverlapsObject(storageStart, storageEnd, &storage, sizeof(storage)))
    {
        return InitialRseBackingStorePreparationError::StorageAliasesInput;
    }

    return InitialRseBackingStorePreparationError::None;
}
} // namespace Asteria::Platform::Ia64::Memory
