// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialExecutionContextSwitcher.hpp"

#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Runtime/Memory.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64SwitchInitialExecutionContext(Asteria::Foundation::UInt64 initialStackPointer,
                                         Asteria::Foundation::UInt64 rseBackingStorePhysicalStart) noexcept;

extern "C" [[noreturn]] void AsteriaIa64RunOwnedExecutionContext() noexcept;

// The target entry point is in InitialExecutionContextSwitcher.S.

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

using ValidationResult =
    Foundation::Result<InitialExecutionContextValidationSummary, InitialExecutionContextSwitchFailure>;

[[nodiscard]] constexpr InitialExecutionContextSwitchFailure
Failure(const InitialExecutionContextSwitchError error,
        const InitialMemoryStackPreparationError memoryStackError = InitialMemoryStackPreparationError::None,
        const InitialRseBackingStorePreparationError rseBackingStoreError =
            InitialRseBackingStorePreparationError::None) noexcept
{
    return InitialExecutionContextSwitchFailure{
        .Error = error,
        .MemoryStackError = memoryStackError,
        .RseBackingStoreError = rseBackingStoreError,
        .Reserved = 0U,
    };
}

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

[[nodiscard]] bool IsZeroStorage(const Foundation::UInt8* const data, const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size offset{0U}; offset < sizeBytes; ++offset)
    {
        if (data[offset] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] InitialMemoryStackPreparationError
ValidateMemoryStack(const Kernel::Memory::InitialMappingPlan& mappingPlan,
                    const BootstrapMappingPlanSummary& mappingSummary,
                    const WritableInitialMemoryStackView& memoryStack) noexcept
{
    using Kernel::Memory::InitialMappingOwner;
    using Kernel::Memory::MappingPageSizeBytes;
    using Kernel::Memory::MemoryCachePolicy;

    if (!mappingPlan.IsSealed() || !IsBootstrapMappingCountValid(mappingPlan.GetMappingCount()))
    {
        return InitialMemoryStackPreparationError::InvalidMappingPlan;
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
        return InitialMemoryStackPreparationError::InvalidMappingSummary;
    }

    const Foundation::UInt64 expectedPhysicalStart = mappingSummary.StackPhysicalStart;
    Foundation::UInt64 expectedPhysicalEnd{0U};
    if (!TryCalculateEnd(expectedPhysicalStart, InitialMemoryStackSizeBytes, expectedPhysicalEnd) ||
        (expectedPhysicalEnd > reservedEnd))
    {
        return InitialMemoryStackPreparationError::InvalidMappingSummary;
    }

    if ((memoryStack.Data == nullptr) || (memoryStack.SizeBytes != InitialMemoryStackSizeBytes))
    {
        return InitialMemoryStackPreparationError::InvalidStorageView;
    }
    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(memoryStack.Data);
    Foundation::UIntPointer storageEnd{0U};
    if (((storageStart & (MappingPageSizeBytes - 1U)) != 0U) ||
        !TryCalculatePointerEnd(storageStart, memoryStack.SizeBytes, storageEnd))
    {
        return InitialMemoryStackPreparationError::InvalidStorageView;
    }
    if (memoryStack.PhysicalStart != expectedPhysicalStart)
    {
        return InitialMemoryStackPreparationError::StorageAddressMismatch;
    }
#if defined(__ia64__)
    if (storageStart != static_cast<Foundation::UIntPointer>(memoryStack.PhysicalStart))
    {
        return InitialMemoryStackPreparationError::StorageAddressMismatch;
    }
#endif
    if (StorageOverlapsObject(storageStart, storageEnd, &mappingPlan, sizeof(mappingPlan)) ||
        StorageOverlapsObject(storageStart, storageEnd, &mappingSummary, sizeof(mappingSummary)))
    {
        return InitialMemoryStackPreparationError::StorageAliasesInput;
    }

    if (mappingSummary.MappingCount == BootstrapConsolidatedMappingCount)
    {
        for (Foundation::Size index{0U}; index < mappingPlan.GetMappingCount(); ++index)
        {
            const auto result = mappingPlan.GetMapping(index);
            if (!result.HasValue())
            {
                return InitialMemoryStackPreparationError::InvalidMappingPlan;
            }
            const auto& mapping = result.GetValue();
            const Foundation::UInt64 start = mapping.PhysicalStart.GetValue();
            if ((mapping.Owner == InitialMappingOwner::NucleusBootstrapRuntimeData) &&
                (start == mappingSummary.StackPhysicalStart) && (mapping.VirtualStart.GetValue() == start) &&
                (mapping.Permissions.GetBits() == RequiredPermissions) &&
                (mapping.CachePolicy == MemoryCachePolicy::WriteBack) && (start <= expectedPhysicalStart) &&
                (mapping.Pages.GetValue() * MappingPageSizeBytes >= expectedPhysicalEnd - start))
            {
                return InitialMemoryStackPreparationError::None;
            }
        }
        return InitialMemoryStackPreparationError::MissingStackMapping;
    }

    Foundation::Size stackMappingCount{0U};
    for (Foundation::Size index{0U}; index < mappingPlan.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappingPlan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return InitialMemoryStackPreparationError::InvalidMappingPlan;
        }
        const Kernel::Memory::InitialMapping& mapping = mappingResult.GetValue();
        Foundation::UInt64 mappingPhysicalEnd{0U};
        if (!TryCalculateEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue() * MappingPageSizeBytes,
                             mappingPhysicalEnd))
        {
            return InitialMemoryStackPreparationError::InvalidMappingPlan;
        }
        const bool overlapsStack = RangesOverlap(mapping.PhysicalStart.GetValue(), mappingPhysicalEnd,
                                                 expectedPhysicalStart, expectedPhysicalEnd);
        if (mapping.Owner == InitialMappingOwner::NucleusStack)
        {
            ++stackMappingCount;
            if ((mapping.VirtualStart.GetValue() != expectedPhysicalStart) ||
                (mapping.PhysicalStart.GetValue() != expectedPhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapStackPageCount) ||
                (mapping.Permissions.GetBits() != RequiredPermissions) ||
                (mapping.CachePolicy != MemoryCachePolicy::WriteBack) || (mapping.Reserved != 0U) || !overlapsStack)
            {
                return InitialMemoryStackPreparationError::InvalidStackMapping;
            }
        }
        else if (overlapsStack)
        {
            return InitialMemoryStackPreparationError::ConflictingStackMapping;
        }
    }
    if (stackMappingCount == 0U)
    {
        return InitialMemoryStackPreparationError::MissingStackMapping;
    }
    if (stackMappingCount != 1U)
    {
        return InitialMemoryStackPreparationError::ConflictingStackMapping;
    }
    return InitialMemoryStackPreparationError::None;
}
} // namespace

ValidationResult InitialExecutionContextSwitcher::Validate(const InitialAddressSpacePreparation& preparation,
                                                           const WritableInitialMemoryStackView& memoryStack,
                                                           const WritableRseBackingStoreView& rseBackingStore) noexcept
{
    const auto mappingPlan = preparation.TryGetMappingPlan();
    const auto summary = preparation.TryGetSummary();
    const auto translationPlan = preparation.TryGetTranslationResourcePlan();
    if (!preparation.IsPrepared() || !mappingPlan.HasValue() || !summary.HasValue() || !translationPlan.HasValue() ||
        !translationPlan.GetValue()->IsBuilt())
    {
        return ValidationResult::Failure(Failure(InitialExecutionContextSwitchError::AddressSpaceNotPrepared));
    }

    const InitialMemoryStackPreparationError memoryStackError =
        ValidateMemoryStack(*mappingPlan.GetValue(), summary.GetValue()->Mapping, memoryStack);
    if (memoryStackError != InitialMemoryStackPreparationError::None)
    {
        return ValidationResult::Failure(
            Failure(InitialExecutionContextSwitchError::MemoryStackRejected, memoryStackError));
    }

    const InitialRseBackingStorePreparationError rseBackingStoreError =
        InitialRseBackingStorePreparer::Validate(*mappingPlan.GetValue(), summary.GetValue()->Mapping, rseBackingStore);
    if (rseBackingStoreError != InitialRseBackingStorePreparationError::None)
    {
        return ValidationResult::Failure(Failure(InitialExecutionContextSwitchError::RseBackingStoreRejected,
                                                 InitialMemoryStackPreparationError::None, rseBackingStoreError));
    }

    const Foundation::UIntPointer stackStart = reinterpret_cast<Foundation::UIntPointer>(memoryStack.Data);
    const Foundation::UIntPointer rseStart = reinterpret_cast<Foundation::UIntPointer>(rseBackingStore.Data);
    Foundation::UIntPointer stackEnd{0U};
    Foundation::UIntPointer rseEnd{0U};
    if (!TryCalculatePointerEnd(stackStart, memoryStack.SizeBytes, stackEnd) ||
        !TryCalculatePointerEnd(rseStart, rseBackingStore.SizeBytes, rseEnd) ||
        RangesOverlap(stackStart, stackEnd, rseStart, rseEnd))
    {
        return ValidationResult::Failure(Failure(InitialExecutionContextSwitchError::StorageRangesOverlap));
    }
    if (StorageOverlapsObject(stackStart, stackEnd, &preparation, sizeof(preparation)) ||
        StorageOverlapsObject(stackStart, stackEnd, &memoryStack, sizeof(memoryStack)) ||
        StorageOverlapsObject(stackStart, stackEnd, &rseBackingStore, sizeof(rseBackingStore)) ||
        StorageOverlapsObject(rseStart, rseEnd, &preparation, sizeof(preparation)) ||
        StorageOverlapsObject(rseStart, rseEnd, &memoryStack, sizeof(memoryStack)) ||
        StorageOverlapsObject(rseStart, rseEnd, &rseBackingStore, sizeof(rseBackingStore)))
    {
        return ValidationResult::Failure(Failure(InitialExecutionContextSwitchError::StorageRangesOverlap));
    }

    if (!IsZeroStorage(rseBackingStore.Data, rseBackingStore.SizeBytes))
    {
        return ValidationResult::Failure(Failure(InitialExecutionContextSwitchError::RseBackingStoreNotZero));
    }

    Foundation::UInt64 stackEndPhysical{0U};
    if (!TryCalculateEnd(memoryStack.PhysicalStart, InitialMemoryStackSizeBytes, stackEndPhysical) ||
        (stackEndPhysical < InitialMemoryStackScratchSizeBytes))
    {
        return ValidationResult::Failure(Failure(InitialExecutionContextSwitchError::MemoryStackRejected,
                                                 InitialMemoryStackPreparationError::InvalidMappingSummary));
    }
    return ValidationResult::Success(InitialExecutionContextValidationSummary{
        .StackPhysicalStart = memoryStack.PhysicalStart,
        .InitialStackPointer = stackEndPhysical - InitialMemoryStackScratchSizeBytes,
        .RseBackingStorePhysicalStart = rseBackingStore.PhysicalStart,
        .StackSizeBytes = static_cast<Foundation::UInt32>(memoryStack.SizeBytes),
        .RseBackingStoreSizeBytes = static_cast<Foundation::UInt32>(rseBackingStore.SizeBytes),
    });
}

InitialExecutionContextSwitchFailure
InitialExecutionContextSwitcher::PrepareAndSwitch(const InitialAddressSpacePreparation& preparation,
                                                  const WritableInitialMemoryStackView& memoryStack,
                                                  const WritableRseBackingStoreView& rseBackingStore) noexcept
{
    const ValidationResult validation = Validate(preparation, memoryStack, rseBackingStore);
    if (!validation.HasValue())
    {
        return validation.GetError();
    }

    static_cast<void>(memset(memoryStack.Data, 0, memoryStack.SizeBytes));
#if defined(__ia64__)
    __asm__ volatile("mf" ::: "memory");
#endif
    if (!IsZeroStorage(memoryStack.Data, memoryStack.SizeBytes))
    {
        return Failure(InitialExecutionContextSwitchError::MemoryStackNotZero);
    }
    if (!IsZeroStorage(rseBackingStore.Data, rseBackingStore.SizeBytes))
    {
        return Failure(InitialExecutionContextSwitchError::RseBackingStoreNotZero);
    }

    const InitialExecutionContextValidationSummary& summary = validation.GetValue();
    const Foundation::UInt64 adapterStatus =
        AsteriaIa64SwitchInitialExecutionContext(summary.InitialStackPointer, summary.RseBackingStorePhysicalStart);
    return Failure(adapterStatus == 0U ? InitialExecutionContextSwitchError::UnexpectedAdapterReturn
                                       : InitialExecutionContextSwitchError::UnsupportedProcessorState);
}
} // namespace Asteria::Platform::Ia64::Memory
