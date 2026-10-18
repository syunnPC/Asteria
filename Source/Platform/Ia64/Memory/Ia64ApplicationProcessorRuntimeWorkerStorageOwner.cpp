// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeWorkerStorageOwner.hpp"

#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using AccessError = Ia64ApplicationProcessorRuntimeWorkerStorageAccessError;
using AccessResult = Foundation::Result<Kernel::Memory::PhysicalPageRange, AccessError>;
using Descriptor = ApplicationProcessorExecutionStorageDescriptor;
using DescriptorAccessError = ApplicationProcessorExecutionStorageDescriptorAccessError;
using DescriptorView = ApplicationProcessorExecutionStorageDescriptorView;
using OwnerError = Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError;
using OwnerFailure = Ia64ApplicationProcessorRuntimeWorkerStorageFailure;
using OwnerSummary = Ia64ApplicationProcessorRuntimeWorkerStoragePreparationSummary;
using PreparationResult = Foundation::Result<OwnerSummary, OwnerFailure>;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 FirmwareMemoryWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt64 WorkerPoolAlignmentBytes{Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages *
                                                      Kernel::Memory::PhysicalPageSizeBytes};
constexpr Foundation::UInt64 WorkerPoolPhysicalAddressLimit{
    Foundation::UInt64{1U} << Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits};

struct ByteRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 End;
};

[[nodiscard]] constexpr OwnerFailure MakeFailure(const OwnerError error,
                                                 const Kernel::Memory::BootstrapPageAllocatorError allocatorError,
                                                 const DescriptorAccessError executionStorageAccessError,
                                                 const Kernel::Scheduler::CpuId failingProcessor) noexcept
{
    return OwnerFailure{
        .Error = error,
        .AllocatorError = allocatorError,
        .ExecutionStorageAccessError = executionStorageAccessError,
        .FailingProcessor = failingProcessor,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr PreparationResult Fail(const OwnerFailure& failure) noexcept
{
    return PreparationResult::Failure(failure);
}

[[nodiscard]] constexpr Foundation::UInt8 ExpectedProcessorMask(const Foundation::UInt8 processorCount) noexcept
{
    return static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);
}

[[nodiscard]] constexpr bool ReservedBytesAreZero(const Descriptor& descriptor) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(descriptor.Reserved); ++index)
    {
        if (descriptor.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool TryGetByteRange(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                             ByteRange& range) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    range = ByteRange{
        .Start = start,
        .End = start + sizeBytes,
    };
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const ByteRange first, const ByteRange second) noexcept
{
    return (first.Start < second.End) && (second.Start < first.End);
}

[[nodiscard]] constexpr bool DescriptorsEqual(const Descriptor& first, const Descriptor& second) noexcept
{
    bool reservedEqual{true};
    for (Foundation::Size index{0U}; index < sizeof(first.Reserved); ++index)
    {
        reservedEqual = reservedEqual && (first.Reserved[index] == second.Reserved[index]);
    }
    return (first.AllocationPhysicalStart == second.AllocationPhysicalStart) &&
           (first.AllocationSizeBytes == second.AllocationSizeBytes) &&
           (first.MemoryStackPhysicalStart == second.MemoryStackPhysicalStart) &&
           (first.InitialStackPointer == second.InitialStackPointer) &&
           (first.MemoryStackSizeBytes == second.MemoryStackSizeBytes) &&
           (first.RseBackingStorePhysicalStart == second.RseBackingStorePhysicalStart) &&
           (first.RseBackingStoreSizeBytes == second.RseBackingStoreSizeBytes) &&
           (first.Processor.GetValue() == second.Processor.GetValue()) && (first.HardwareId == second.HardwareId) &&
           (first.HardwareEid == second.HardwareEid) && reservedEqual;
}

[[nodiscard]] constexpr bool IsIdleDescriptorExact(const Descriptor& descriptor, const Foundation::Size index,
                                                   ByteRange& allocation) noexcept
{
    constexpr Foundation::UInt64 idleAlignmentBytes{ApplicationProcessorExecutionStorageAlignmentPages *
                                                    Kernel::Memory::PhysicalPageSizeBytes};
    if ((descriptor.AllocationPhysicalStart == 0U) ||
        ((descriptor.AllocationPhysicalStart & (idleAlignmentBytes - 1U)) != 0U) ||
        !TryGetByteRange(descriptor.AllocationPhysicalStart, ApplicationProcessorExecutionStorageSizeBytes, allocation))
    {
        return false;
    }
    const Foundation::UInt64 expectedRseStart =
        descriptor.AllocationPhysicalStart + Ia64ApplicationProcessorRuntimeIdleStackSizeBytes;
    return (descriptor.AllocationSizeBytes == ApplicationProcessorExecutionStorageSizeBytes) &&
           (descriptor.MemoryStackPhysicalStart == descriptor.AllocationPhysicalStart) &&
           (descriptor.MemoryStackSizeBytes == Ia64ApplicationProcessorRuntimeIdleStackSizeBytes) &&
           (descriptor.InitialStackPointer ==
            expectedRseStart - Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes) &&
           (descriptor.RseBackingStorePhysicalStart == expectedRseStart) &&
           (descriptor.RseBackingStoreSizeBytes == Ia64ApplicationProcessorRuntimeIdleRseSizeBytes) &&
           descriptor.Processor.IsValid() &&
           (descriptor.Processor.GetValue() == static_cast<Foundation::UInt8>(index + 1U)) &&
           ReservedBytesAreZero(descriptor);
}

[[nodiscard]] bool ValidateExecutionStorage(const ApplicationProcessorExecutionStorageOwner& owner,
                                            Foundation::UInt8& processorCount, Foundation::UInt8& enabledProcessorMask,
                                            DescriptorView& descriptorView, DescriptorAccessError& accessError,
                                            Kernel::Scheduler::CpuId& failingProcessor) noexcept
{
    processorCount = owner.GetProcessorCount();
    enabledProcessorMask = owner.GetEnabledProcessorMask();
    descriptorView = DescriptorView{};
    accessError = DescriptorAccessError::None;
    failingProcessor = Kernel::Scheduler::CpuId{};

    if ((processorCount == 0U) || (processorCount > Kernel::Scheduler::MaximumProcessorCount) ||
        (enabledProcessorMask != ExpectedProcessorMask(processorCount)) ||
        (owner.GetDescriptorCount() != static_cast<Foundation::UInt8>(processorCount - 1U)))
    {
        return false;
    }

    const auto viewResult = owner.GetDescriptorView();
    if (!viewResult.HasValue())
    {
        accessError = viewResult.GetError();
        return false;
    }
    descriptorView = viewResult.GetValue();
    const Foundation::Size expectedDescriptorCount = static_cast<Foundation::Size>(processorCount - 1U);
    if ((descriptorView.Count != expectedDescriptorCount) ||
        ((descriptorView.Data == nullptr) != (expectedDescriptorCount == 0U)))
    {
        return false;
    }

    for (Foundation::Size index{0U}; index < descriptorView.Count; ++index)
    {
        failingProcessor = Kernel::Scheduler::CpuId{static_cast<Foundation::UInt8>(index + 1U)};
        ByteRange currentRange{};
        const Descriptor& current = descriptorView.Data[index];
        if (!IsIdleDescriptorExact(current, index, currentRange))
        {
            return false;
        }

        const auto descriptorResult = owner.GetDescriptor(failingProcessor);
        if (!descriptorResult.HasValue())
        {
            accessError = descriptorResult.GetError();
            return false;
        }
        if (!DescriptorsEqual(current, descriptorResult.GetValue()))
        {
            return false;
        }

        for (Foundation::Size priorIndex{0U}; priorIndex < index; ++priorIndex)
        {
            ByteRange priorRange{};
            const Descriptor& prior = descriptorView.Data[priorIndex];
            if (!IsIdleDescriptorExact(prior, priorIndex, priorRange) || RangesOverlap(currentRange, priorRange) ||
                ((current.HardwareId == prior.HardwareId) && (current.HardwareEid == prior.HardwareEid)))
            {
                return false;
            }
        }
    }

    failingProcessor = Kernel::Scheduler::CpuId{};
    return true;
}

[[nodiscard]] constexpr bool IsWorkerPoolExact(const Kernel::Memory::PhysicalPageRange& workerPool,
                                               const Foundation::UInt64 expectedPageCount, ByteRange& range) noexcept
{
    if ((workerPool.PhysicalStart == 0U) || ((workerPool.PhysicalStart & (WorkerPoolAlignmentBytes - 1U)) != 0U) ||
        (workerPool.PageCount != expectedPageCount) ||
        ((workerPool.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
        ((workerPool.Attributes & FirmwareMemoryRuntimeAttribute) != 0U) ||
        (workerPool.FirmwareType != ConventionalMemoryType) || (workerPool.Reserved != 0U) ||
        (workerPool.PageCount > (MaximumUInt64 / Kernel::Memory::PhysicalPageSizeBytes)))
    {
        return false;
    }
    const Foundation::UInt64 sizeBytes = workerPool.PageCount * Kernel::Memory::PhysicalPageSizeBytes;
    return TryGetByteRange(workerPool.PhysicalStart, sizeBytes, range) && (range.End <= WorkerPoolPhysicalAddressLimit);
}

[[nodiscard]] constexpr bool OverlapsIdleStorage(const ByteRange workerRange, const DescriptorView idleStorage,
                                                 Kernel::Scheduler::CpuId& failingProcessor) noexcept
{
    for (Foundation::Size index{0U}; index < idleStorage.Count; ++index)
    {
        ByteRange idleRange{};
        if (!TryGetByteRange(idleStorage.Data[index].AllocationPhysicalStart,
                             idleStorage.Data[index].AllocationSizeBytes, idleRange) ||
            RangesOverlap(workerRange, idleRange))
        {
            failingProcessor = idleStorage.Data[index].Processor;
            return true;
        }
    }
    failingProcessor = Kernel::Scheduler::CpuId{};
    return false;
}
} // namespace

PreparationResult Ia64ApplicationProcessorRuntimeWorkerStorageOwner::Prepare(
    Kernel::Memory::BootstrapPageAllocator& allocator,
    const ApplicationProcessorExecutionStorageOwner& executionStorageOwner) noexcept
{
    if (m_state != Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Fresh)
    {
        return Fail(MakeFailure(OwnerError::AlreadyPrepared, Kernel::Memory::BootstrapPageAllocatorError::None,
                                DescriptorAccessError::None, Kernel::Scheduler::CpuId{}));
    }

    if (executionStorageOwner.GetState() != ApplicationProcessorExecutionStorageOwnerState::Ready)
    {
        m_state = Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed;
        return Fail(MakeFailure(OwnerError::ExecutionStorageOwnerNotReady,
                                Kernel::Memory::BootstrapPageAllocatorError::None, DescriptorAccessError::NotReady,
                                Kernel::Scheduler::CpuId{}));
    }

    Foundation::UInt8 processorCount{0U};
    Foundation::UInt8 enabledProcessorMask{0U};
    DescriptorView idleStorage{};
    DescriptorAccessError accessError{DescriptorAccessError::None};
    Kernel::Scheduler::CpuId failingProcessor{};
    if (!ValidateExecutionStorage(executionStorageOwner, processorCount, enabledProcessorMask, idleStorage, accessError,
                                  failingProcessor))
    {
        m_state = Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed;
        return Fail(MakeFailure(OwnerError::InvalidExecutionStorageOwner,
                                Kernel::Memory::BootstrapPageAllocatorError::None, accessError, failingProcessor));
    }

    const Foundation::UInt8 workerStorageCount = static_cast<Foundation::UInt8>(processorCount - 1U);
    const Foundation::UInt64 workerPageCount =
        static_cast<Foundation::UInt64>(workerStorageCount) * Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
    Kernel::Memory::PhysicalPageRange workerPool{};
    if (workerStorageCount != 0U)
    {
        const auto allocation =
            allocator.Allocate(workerPageCount, Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages);
        if (!allocation.HasValue())
        {
            m_state = Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed;
            return Fail(MakeFailure(OwnerError::AllocationFailed, allocation.GetError(), DescriptorAccessError::None,
                                    Kernel::Scheduler::CpuId{}));
        }
        workerPool = allocation.GetValue();

        ByteRange workerRange{};
        if (!IsWorkerPoolExact(workerPool, workerPageCount, workerRange))
        {
            m_state = Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed;
            return Fail(MakeFailure(OwnerError::InvalidWorkerPool, Kernel::Memory::BootstrapPageAllocatorError::None,
                                    DescriptorAccessError::None, Kernel::Scheduler::CpuId{}));
        }
        if (OverlapsIdleStorage(workerRange, idleStorage, failingProcessor))
        {
            m_state = Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed;
            return Fail(MakeFailure(OwnerError::WorkerPoolOverlapsIdleStorage,
                                    Kernel::Memory::BootstrapPageAllocatorError::None, DescriptorAccessError::None,
                                    failingProcessor));
        }

        Foundation::UInt8* const workerBytes =
            reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(workerPool.PhysicalStart));
        static_cast<void>(memset(
            workerBytes, 0, static_cast<Foundation::Size>(workerPageCount * Kernel::Memory::PhysicalPageSizeBytes)));
    }

#if defined(__ia64__)
    __asm__ volatile("mf" ::: "memory");
#endif
    m_workerPool = workerPool;
    m_processorCount = processorCount;
    m_enabledProcessorMask = enabledProcessorMask;
    m_workerStorageCount = workerStorageCount;
    m_state = Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Ready;
    return PreparationResult::Success(OwnerSummary{
        .WorkerPageCount = workerPageCount,
        .ProcessorCount = processorCount,
        .EnabledProcessorMask = enabledProcessorMask,
        .WorkerStorageCount = workerStorageCount,
        .Reserved = {},
    });
}

AccessResult Ia64ApplicationProcessorRuntimeWorkerStorageOwner::GetWorkerPool() const noexcept
{
    if (m_state != Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Ready)
    {
        return AccessResult::Failure(AccessError::NotReady);
    }
    return AccessResult::Success(m_workerPool);
}
} // namespace Asteria::Platform::Ia64::Memory
