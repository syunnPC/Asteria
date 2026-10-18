// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using Descriptor = ApplicationProcessorExecutionStorageDescriptor;
using DescriptorAccessError = ApplicationProcessorExecutionStorageDescriptorAccessError;
using DescriptorResult = Foundation::Result<Descriptor, DescriptorAccessError>;
using DescriptorViewResult =
    Foundation::Result<ApplicationProcessorExecutionStorageDescriptorView, DescriptorAccessError>;
using OwnerError = ApplicationProcessorExecutionStorageOwnerError;
using OwnerFailure = ApplicationProcessorExecutionStorageOwnerFailure;
using OwnerSummary = ApplicationProcessorExecutionStorageOwnerPreparationSummary;
using PreparationResult = Foundation::Result<OwnerSummary, OwnerFailure>;
using Snapshot = Interrupt::Ia64LocalSapicIpiControllerSnapshot;

constexpr Foundation::UInt64 ProcessorInterruptBlockSizeBytes{2U * 1024U * 1024U};
constexpr Foundation::UInt64 ProcessorInterruptBlockAlignmentMask{ProcessorInterruptBlockSizeBytes - 1U};
constexpr Foundation::UInt64 PhysicalAddressUncachedBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 MaximumPhysicalAddress{PhysicalAddressUncachedBit - 1U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr OwnerFailure
MakeFailure(const OwnerError error, const Kernel::Memory::BootstrapPageAllocatorError allocatorError,
            const ApplicationProcessorExecutionStoragePreparationError preparationError,
            const Kernel::Scheduler::CpuId failingProcessor) noexcept
{
    return OwnerFailure{
        .Error = error,
        .AllocatorError = allocatorError,
        .PreparationError = preparationError,
        .FailingProcessor = failingProcessor,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr PreparationResult Fail(const OwnerFailure& failure) noexcept
{
    return PreparationResult::Failure(failure);
}

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
}

[[nodiscard]] constexpr bool IsPhysicalPibBaseValid(const Foundation::UInt64 baseAddress) noexcept
{
    return (baseAddress != 0U) && ((baseAddress & ProcessorInterruptBlockAlignmentMask) == 0U) &&
           (baseAddress <= (MaximumPhysicalAddress - ProcessorInterruptBlockAlignmentMask));
}

[[nodiscard]] constexpr bool IsAccessPibBaseValid(const Foundation::UInt64 baseAddress) noexcept
{
    return (baseAddress != 0U) && ((baseAddress & ProcessorInterruptBlockAlignmentMask) == 0U) &&
           (baseAddress <= (MaximumUInt64 - ProcessorInterruptBlockAlignmentMask));
}

[[nodiscard]] constexpr Foundation::UInt64 ExpectedInterruptAddress(const Foundation::UInt64 accessBaseAddress,
                                                                    const Foundation::UInt8 hardwareId,
                                                                    const Foundation::UInt8 hardwareEid) noexcept
{
    return accessBaseAddress + (static_cast<Foundation::UInt64>(hardwareId) << 12U) +
           (static_cast<Foundation::UInt64>(hardwareEid) << 4U);
}

[[nodiscard]] constexpr bool IsProcessorRecordZero(const Interrupt::Ia64LocalSapicLogicalProcessor& processor) noexcept
{
    return (processor.InterruptAddress == 0U) && (processor.AcpiUid == 0U) && (processor.HardwareId == 0U) &&
           (processor.HardwareEid == 0U) && (processor.Reserved[0U] == 0U) && (processor.Reserved[1U] == 0U);
}

[[nodiscard]] bool IsSnapshotValid(const Snapshot& snapshot, Kernel::Scheduler::CpuId& failingProcessor) noexcept
{
    failingProcessor = Kernel::Scheduler::CpuId{};
    if ((snapshot.State != Interrupt::Ia64LocalSapicIpiControllerState::Ready) || (snapshot.ProcessorCount == 0U) ||
        (snapshot.ProcessorCount > Kernel::Scheduler::MaximumProcessorCount) ||
        (snapshot.EnabledProcessorMask != static_cast<Foundation::UInt8>(ProcessorBit(snapshot.ProcessorCount) - 1U)) ||
        !IsPhysicalPibBaseValid(snapshot.PhysicalBaseAddress) || !IsAccessPibBaseValid(snapshot.AccessBaseAddress) ||
        ((snapshot.BaseKind != Interrupt::Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical) &&
         (snapshot.BaseKind != Interrupt::Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual)))
    {
        return false;
    }
    if ((snapshot.BaseKind == Interrupt::Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical) &&
        (snapshot.AccessBaseAddress != (snapshot.PhysicalBaseAddress | PhysicalAddressUncachedBit)))
    {
        return false;
    }
    for (const Foundation::UInt8 reserved : snapshot.Reserved)
    {
        if (reserved != 0U)
        {
            return false;
        }
    }

    for (Foundation::UInt8 index{0U}; index < snapshot.ProcessorCount; ++index)
    {
        const Kernel::Scheduler::CpuId processor{index};
        failingProcessor = processor;
        if (!processor.IsValid() || (processor.GetValue() != index))
        {
            return false;
        }

        const Interrupt::Ia64LocalSapicLogicalProcessor& record = snapshot.Processors[index];
        if ((record.InterruptAddress !=
             ExpectedInterruptAddress(snapshot.AccessBaseAddress, record.HardwareId, record.HardwareEid)) ||
            (record.Reserved[0U] != 0U) || (record.Reserved[1U] != 0U))
        {
            return false;
        }
        for (Foundation::UInt8 previous{0U}; previous < index; ++previous)
        {
            const Interrupt::Ia64LocalSapicLogicalProcessor& prior = snapshot.Processors[previous];
            if ((record.AcpiUid == prior.AcpiUid) ||
                ((record.HardwareId == prior.HardwareId) && (record.HardwareEid == prior.HardwareEid)))
            {
                return false;
            }
        }
    }

    for (Foundation::UInt8 index{snapshot.ProcessorCount}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        failingProcessor = Kernel::Scheduler::CpuId{index};
        if (!IsProcessorRecordZero(snapshot.Processors[index]))
        {
            return false;
        }
    }
    failingProcessor = Kernel::Scheduler::CpuId{};
    return true;
}

[[nodiscard]] constexpr ApplicationProcessorExecutionStorageIdentity
MakeIdentity(const Kernel::Scheduler::CpuId processor, const Interrupt::Ia64LocalSapicLogicalProcessor& record) noexcept
{
    return ApplicationProcessorExecutionStorageIdentity{
        .Processor = processor,
        .HardwareId = record.HardwareId,
        .HardwareEid = record.HardwareEid,
        .Reserved = 0U,
    };
}
} // namespace

PreparationResult ApplicationProcessorExecutionStorageOwner::Prepare(Kernel::Memory::BootstrapPageAllocator& allocator,
                                                                     const Snapshot& snapshot) noexcept
{
    if (m_state != ApplicationProcessorExecutionStorageOwnerState::Fresh)
    {
        return Fail(MakeFailure(OwnerError::AlreadyPrepared, Kernel::Memory::BootstrapPageAllocatorError::None,
                                ApplicationProcessorExecutionStoragePreparationError::None,
                                Kernel::Scheduler::CpuId{}));
    }

    Kernel::Scheduler::CpuId failingProcessor{};
    if (!IsSnapshotValid(snapshot, failingProcessor))
    {
        m_state = ApplicationProcessorExecutionStorageOwnerState::Failed;
        return Fail(MakeFailure(OwnerError::InvalidControllerSnapshot,
                                Kernel::Memory::BootstrapPageAllocatorError::None,
                                ApplicationProcessorExecutionStoragePreparationError::None, failingProcessor));
    }

    Foundation::UInt8 descriptorCount{0U};
    for (Foundation::UInt8 processorValue{1U}; processorValue < snapshot.ProcessorCount; ++processorValue)
    {
        const Kernel::Scheduler::CpuId processor{processorValue};
        const auto allocation = allocator.Allocate(ApplicationProcessorExecutionStoragePageCount,
                                                   ApplicationProcessorExecutionStorageAlignmentPages);
        if (!allocation.HasValue())
        {
            m_state = ApplicationProcessorExecutionStorageOwnerState::Failed;
            return Fail(MakeFailure(OwnerError::AllocationFailed, allocation.GetError(),
                                    ApplicationProcessorExecutionStoragePreparationError::None, processor));
        }

        const Kernel::Memory::PhysicalPageRange& pages = allocation.GetValue();
        const Foundation::UInt64 rsePhysicalStart = pages.PhysicalStart + ApplicationProcessorMemoryStackSizeBytes;
        const WritableInitialMemoryStackView memoryStack{
            .Data = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(pages.PhysicalStart)),
            .SizeBytes = ApplicationProcessorMemoryStackSizeBytes,
            .PhysicalStart = pages.PhysicalStart,
        };
        const WritableRseBackingStoreView rseBackingStore{
            .Data = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(rsePhysicalStart)),
            .SizeBytes = ApplicationProcessorRseBackingStoreSizeBytes,
            .PhysicalStart = rsePhysicalStart,
        };
        const ApplicationProcessorExecutionStorageDescriptorView priorDescriptors{
            .Data = (descriptorCount == 0U) ? nullptr : m_descriptors,
            .Count = descriptorCount,
        };
        const auto prepared = ApplicationProcessorExecutionStoragePreparer::Prepare(
            MakeIdentity(processor, snapshot.Processors[processorValue]), pages, memoryStack, rseBackingStore,
            priorDescriptors);
        if (!prepared.HasValue())
        {
            m_state = ApplicationProcessorExecutionStorageOwnerState::Failed;
            return Fail(MakeFailure(OwnerError::StoragePreparationFailed,
                                    Kernel::Memory::BootstrapPageAllocatorError::None, prepared.GetError(), processor));
        }

        m_descriptors[descriptorCount] = prepared.GetValue();
        ++descriptorCount;
    }

    m_processorCount = snapshot.ProcessorCount;
    m_enabledProcessorMask = snapshot.EnabledProcessorMask;
    m_descriptorCount = descriptorCount;
    m_state = ApplicationProcessorExecutionStorageOwnerState::Ready;
    return PreparationResult::Success(OwnerSummary{
        .ProcessorCount = m_processorCount,
        .EnabledProcessorMask = m_enabledProcessorMask,
        .DescriptorCount = m_descriptorCount,
        .Reserved = 0U,
    });
}

DescriptorResult
ApplicationProcessorExecutionStorageOwner::GetDescriptor(const Kernel::Scheduler::CpuId processor) const noexcept
{
    if (m_state != ApplicationProcessorExecutionStorageOwnerState::Ready)
    {
        return DescriptorResult::Failure(DescriptorAccessError::NotReady);
    }
    const Foundation::UInt8 processorValue = processor.GetValue();
    if (!processor.IsValid() || (processorValue == 0U) || (processorValue >= m_processorCount))
    {
        return DescriptorResult::Failure(DescriptorAccessError::InvalidCpuId);
    }

    const Foundation::Size descriptorIndex = static_cast<Foundation::Size>(processorValue - 1U);
    const Descriptor& descriptor = m_descriptors[descriptorIndex];
    if (descriptor.Processor.GetValue() != processorValue)
    {
        return DescriptorResult::Failure(DescriptorAccessError::InvalidCpuId);
    }
    return DescriptorResult::Success(descriptor);
}

DescriptorViewResult ApplicationProcessorExecutionStorageOwner::GetDescriptorView() const noexcept
{
    if (m_state != ApplicationProcessorExecutionStorageOwnerState::Ready)
    {
        return DescriptorViewResult::Failure(DescriptorAccessError::NotReady);
    }
    return DescriptorViewResult::Success(ApplicationProcessorExecutionStorageDescriptorView{
        .Data = (m_descriptorCount == 0U) ? nullptr : m_descriptors,
        .Count = m_descriptorCount,
    });
}
} // namespace Asteria::Platform::Ia64::Memory
