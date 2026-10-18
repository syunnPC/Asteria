// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStoragePreparer.hpp"

#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using Descriptor = ApplicationProcessorExecutionStorageDescriptor;
using Error = ApplicationProcessorExecutionStoragePreparationError;
using PreparationResult = Foundation::Result<Descriptor, Error>;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt64 Ia64VirtualRegionZeroSizeBytes{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 FirmwareMemoryWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt64 StorageAlignmentBytes{ApplicationProcessorExecutionStorageAlignmentPages *
                                                   Kernel::Memory::PhysicalPageSizeBytes};

[[nodiscard]] constexpr bool TryCalculateEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
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

[[nodiscard]] constexpr bool IsIdentityValid(const ApplicationProcessorExecutionStorageIdentity identity) noexcept
{
    return identity.Processor.IsValid() && (identity.Processor.GetValue() != 0U) && (identity.Reserved == 0U);
}

[[nodiscard]] constexpr bool IsDescriptorIdentityValid(const Descriptor& descriptor) noexcept
{
    if (!descriptor.Processor.IsValid() || (descriptor.Processor.GetValue() == 0U))
    {
        return false;
    }
    for (const Foundation::UInt8 reserved : descriptor.Reserved)
    {
        if (reserved != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool
IdentitiesDuplicate(const Kernel::Scheduler::CpuId firstProcessor, const Foundation::UInt8 firstHardwareId,
                    const Foundation::UInt8 firstHardwareEid, const Kernel::Scheduler::CpuId secondProcessor,
                    const Foundation::UInt8 secondHardwareId, const Foundation::UInt8 secondHardwareEid) noexcept
{
    return (firstProcessor.GetValue() == secondProcessor.GetValue()) ||
           ((firstHardwareId == secondHardwareId) && (firstHardwareEid == secondHardwareEid));
}

[[nodiscard]] constexpr bool IsDescriptorGeometryValid(const Descriptor& descriptor) noexcept
{
    Foundation::UInt64 allocationEnd{0U};
    Foundation::UInt64 expectedRseStart{0U};
    Foundation::UInt64 expectedStackEnd{0U};
    if ((descriptor.AllocationPhysicalStart == 0U) ||
        ((descriptor.AllocationPhysicalStart & (StorageAlignmentBytes - 1U)) != 0U) ||
        (descriptor.AllocationSizeBytes != ApplicationProcessorExecutionStorageSizeBytes) ||
        !TryCalculateEnd(descriptor.AllocationPhysicalStart, descriptor.AllocationSizeBytes, allocationEnd) ||
        (allocationEnd > Ia64VirtualRegionZeroSizeBytes) ||
        (descriptor.MemoryStackPhysicalStart != descriptor.AllocationPhysicalStart) ||
        (descriptor.MemoryStackSizeBytes != ApplicationProcessorMemoryStackSizeBytes) ||
        !TryCalculateEnd(descriptor.MemoryStackPhysicalStart, descriptor.MemoryStackSizeBytes, expectedStackEnd) ||
        (descriptor.InitialStackPointer != (expectedStackEnd - ApplicationProcessorInitialStackScratchSizeBytes)) ||
        !TryCalculateEnd(descriptor.AllocationPhysicalStart, ApplicationProcessorMemoryStackSizeBytes,
                         expectedRseStart) ||
        (descriptor.RseBackingStorePhysicalStart != expectedRseStart) ||
        (descriptor.RseBackingStoreSizeBytes != ApplicationProcessorRseBackingStoreSizeBytes))
    {
        return false;
    }
    Foundation::UInt64 rseEnd{0U};
    return TryCalculateEnd(descriptor.RseBackingStorePhysicalStart, descriptor.RseBackingStoreSizeBytes, rseEnd) &&
           (rseEnd == allocationEnd);
}

[[nodiscard]] constexpr Descriptor MakeDescriptor(const ApplicationProcessorExecutionStorageIdentity identity,
                                                  const Kernel::Memory::PhysicalPageRange allocation) noexcept
{
    const Foundation::UInt64 stackEnd = allocation.PhysicalStart + ApplicationProcessorMemoryStackSizeBytes;
    return Descriptor{
        .AllocationPhysicalStart = allocation.PhysicalStart,
        .AllocationSizeBytes = ApplicationProcessorExecutionStorageSizeBytes,
        .MemoryStackPhysicalStart = allocation.PhysicalStart,
        .InitialStackPointer = stackEnd - ApplicationProcessorInitialStackScratchSizeBytes,
        .MemoryStackSizeBytes = ApplicationProcessorMemoryStackSizeBytes,
        .RseBackingStorePhysicalStart = stackEnd,
        .RseBackingStoreSizeBytes = ApplicationProcessorRseBackingStoreSizeBytes,
        .Processor = identity.Processor,
        .HardwareId = identity.HardwareId,
        .HardwareEid = identity.HardwareEid,
        .Reserved = {},
    };
}

[[nodiscard]] PreparationResult Fail(const Error error) noexcept
{
    return PreparationResult::Failure(error);
}
} // namespace

PreparationResult ApplicationProcessorExecutionStoragePreparer::Prepare(
    const ApplicationProcessorExecutionStorageIdentity identity, const Kernel::Memory::PhysicalPageRange allocation,
    const WritableInitialMemoryStackView memoryStack, const WritableRseBackingStoreView rseBackingStore,
    const ApplicationProcessorExecutionStorageDescriptorView priorDescriptors) noexcept
{
    if (!IsIdentityValid(identity))
    {
        return Fail(Error::InvalidProcessorIdentity);
    }

    Foundation::UInt64 allocationEnd{0U};
    if ((allocation.PhysicalStart == 0U) || (allocation.PageCount != ApplicationProcessorExecutionStoragePageCount) ||
        ((allocation.PhysicalStart & (StorageAlignmentBytes - 1U)) != 0U) ||
        !TryCalculateEnd(allocation.PhysicalStart, ApplicationProcessorExecutionStorageSizeBytes, allocationEnd) ||
        (allocationEnd > Ia64VirtualRegionZeroSizeBytes))
    {
        return Fail(Error::InvalidAllocationGeometry);
    }
    if ((allocation.Attributes & FirmwareMemoryWriteBackAttribute) == 0U ||
        (allocation.Attributes & FirmwareMemoryRuntimeAttribute) != 0U ||
        (allocation.FirmwareType != ConventionalMemoryType) || (allocation.Reserved != 0U))
    {
        return Fail(Error::InvalidAllocationAttributes);
    }

    if ((memoryStack.Data == nullptr) || (memoryStack.SizeBytes != ApplicationProcessorMemoryStackSizeBytes))
    {
        return Fail(Error::InvalidMemoryStackView);
    }
    if ((rseBackingStore.Data == nullptr) ||
        (rseBackingStore.SizeBytes != ApplicationProcessorRseBackingStoreSizeBytes))
    {
        return Fail(Error::InvalidRseBackingStoreView);
    }

    const Foundation::UIntPointer stackStart = reinterpret_cast<Foundation::UIntPointer>(memoryStack.Data);
    const Foundation::UIntPointer rseStart = reinterpret_cast<Foundation::UIntPointer>(rseBackingStore.Data);
    Foundation::UIntPointer stackEnd{0U};
    Foundation::UIntPointer rseEnd{0U};
    if (((stackStart & (Kernel::Memory::PhysicalPageSizeBytes - 1U)) != 0U) ||
        !TryCalculatePointerEnd(stackStart, memoryStack.SizeBytes, stackEnd))
    {
        return Fail(Error::InvalidMemoryStackView);
    }
    if (((rseStart & (Kernel::Memory::PhysicalPageSizeBytes - 1U)) != 0U) ||
        !TryCalculatePointerEnd(rseStart, rseBackingStore.SizeBytes, rseEnd))
    {
        return Fail(Error::InvalidRseBackingStoreView);
    }

    const Foundation::UInt64 expectedRsePhysicalStart =
        allocation.PhysicalStart + ApplicationProcessorMemoryStackSizeBytes;
    if ((memoryStack.PhysicalStart != allocation.PhysicalStart) ||
        (rseBackingStore.PhysicalStart != expectedRsePhysicalStart))
    {
        return Fail(Error::StorageAddressMismatch);
    }
    if ((stackEnd != rseStart) || (rseEnd - stackStart != ApplicationProcessorExecutionStorageSizeBytes))
    {
        return Fail(Error::StorageViewsNotContiguous);
    }
#if defined(__ia64__)
    if ((stackStart != static_cast<Foundation::UIntPointer>(memoryStack.PhysicalStart)) ||
        (rseStart != static_cast<Foundation::UIntPointer>(rseBackingStore.PhysicalStart)))
    {
        return Fail(Error::StorageAddressMismatch);
    }
#endif

    if (((priorDescriptors.Data == nullptr) != (priorDescriptors.Count == 0U)) ||
        (priorDescriptors.Count > MaximumPriorApplicationProcessorStorageDescriptors))
    {
        return Fail(Error::InvalidPriorDescriptorView);
    }

    if (priorDescriptors.Count != 0U)
    {
        const Foundation::UIntPointer inputStart = reinterpret_cast<Foundation::UIntPointer>(priorDescriptors.Data);
        Foundation::UIntPointer inputEnd{0U};
        if (((inputStart & (alignof(Descriptor) - 1U)) != 0U) ||
            !TryCalculatePointerEnd(inputStart, priorDescriptors.Count * sizeof(Descriptor), inputEnd))
        {
            return Fail(Error::InvalidPriorDescriptorView);
        }
        if (RangesOverlap(stackStart, rseEnd, inputStart, inputEnd))
        {
            return Fail(Error::StorageAliasesInput);
        }
    }

    for (Foundation::Size index{0U}; index < priorDescriptors.Count; ++index)
    {
        const Descriptor& prior = priorDescriptors.Data[index];
        if (!IsDescriptorIdentityValid(prior) || !IsDescriptorGeometryValid(prior))
        {
            return Fail(Error::InvalidPriorDescriptor);
        }
        if (IdentitiesDuplicate(identity.Processor, identity.HardwareId, identity.HardwareEid, prior.Processor,
                                prior.HardwareId, prior.HardwareEid))
        {
            return Fail(Error::DuplicateProcessorIdentity);
        }
        Foundation::UInt64 priorEnd{0U};
        if (!TryCalculateEnd(prior.AllocationPhysicalStart, prior.AllocationSizeBytes, priorEnd))
        {
            return Fail(Error::InvalidPriorDescriptor);
        }
        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            const Descriptor& other = priorDescriptors.Data[previous];
            if (IdentitiesDuplicate(prior.Processor, prior.HardwareId, prior.HardwareEid, other.Processor,
                                    other.HardwareId, other.HardwareEid))
            {
                return Fail(Error::InvalidPriorDescriptor);
            }
            Foundation::UInt64 otherEnd{0U};
            if (!TryCalculateEnd(other.AllocationPhysicalStart, other.AllocationSizeBytes, otherEnd) ||
                RangesOverlap(prior.AllocationPhysicalStart, priorEnd, other.AllocationPhysicalStart, otherEnd))
            {
                return Fail(Error::InvalidPriorDescriptor);
            }
        }
        if (RangesOverlap(allocation.PhysicalStart, allocationEnd, prior.AllocationPhysicalStart, priorEnd))
        {
            return Fail(Error::StorageOverlapsPriorAllocation);
        }
    }

    const Descriptor descriptor = MakeDescriptor(identity, allocation);
    static_cast<void>(memset(memoryStack.Data, 0, memoryStack.SizeBytes));
    static_cast<void>(memset(rseBackingStore.Data, 0, rseBackingStore.SizeBytes));
#if defined(__ia64__)
    __asm__ volatile("mf" ::: "memory");
#endif
    return PreparationResult::Success(descriptor);
}
} // namespace Asteria::Platform::Ia64::Memory
