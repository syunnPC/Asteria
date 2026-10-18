// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeMappingOwner.hpp"

#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Kernel/Object/ObjectPin.hpp"

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
void* operator new(Asteria::Foundation::Size sizeBytes, void* storage) noexcept;
#else
#include <new>
#endif

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using AccessError = Ia64ApplicationProcessorRuntimeMappingAccessError;
using Failure = Ia64ApplicationProcessorRuntimeMappingFailure;
using MappingKind = Ia64ApplicationProcessorRuntimeMappingKind;
using OwnerError = Ia64ApplicationProcessorRuntimeMappingOwnerError;
using OwnerResult = Foundation::Result<Ia64ApplicationProcessorRuntimeMappingSummary, Failure>;

constexpr Foundation::UInt64 PageSizeBytes{Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

class RejectingRemoteShootdownTransport final : public Hal::Memory::ITlbShootdownTransport
{
public:
    explicit constexpr RejectingRemoteShootdownTransport(Foundation::UInt8& attemptCount) noexcept
        : m_attemptCount{attemptCount}
    {
    }

    [[nodiscard]] Hal::Memory::TlbShootdownTransportError
    Publish(const Kernel::Memory::TlbShootdownBatch&) noexcept override
    {
        if (m_attemptCount != 0xFFU)
        {
            ++m_attemptCount;
        }
        return Hal::Memory::TlbShootdownTransportError::Unavailable;
    }

private:
    Foundation::UInt8& m_attemptCount;
};

[[nodiscard]] constexpr Foundation::UInt8 ExpectedProcessorMask(const Foundation::UInt8 processorCount) noexcept
{
    return static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);
}

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Kernel::Scheduler::CpuId processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor.GetValue());
}

[[nodiscard]] constexpr bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool TryGetEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                       Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool TryGetPageRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                                Foundation::UInt64& end) noexcept
{
    if ((pages == 0U) || (pages > (MaximumUInt64 / PageSizeBytes)))
    {
        return false;
    }
    return TryGetEnd(start, pages * PageSizeBytes, end);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool ResourceUsageIsBounded(const Kernel::Resource::ResourceDomainSnapshot& snapshot) noexcept
{
    return (snapshot.Usage.CommittedMemoryPages <= snapshot.Limits.CommittedMemoryPages) &&
           (snapshot.Usage.ResidentMemoryPages <= snapshot.Limits.ResidentMemoryPages) &&
           (snapshot.Usage.KernelMetadataBytes <= snapshot.Limits.KernelMetadataBytes) &&
           (snapshot.Usage.CapabilityHandles <= snapshot.Limits.CapabilityHandles) &&
           (snapshot.Usage.IpcQueueBytes <= snapshot.Limits.IpcQueueBytes) &&
           (snapshot.Usage.Threads <= snapshot.Limits.Threads) &&
           (snapshot.Usage.ChildDomains <= snapshot.Limits.ChildDomains);
}

[[nodiscard]] constexpr bool ResourceCapacityIsSufficient(const Kernel::Resource::ResourceDomainSnapshot& snapshot,
                                                          const Foundation::UInt64 pages,
                                                          const Foundation::UInt64 metadataBytes) noexcept
{
    return ResourceUsageIsBounded(snapshot) &&
           (pages <= (snapshot.Limits.CommittedMemoryPages - snapshot.Usage.CommittedMemoryPages)) &&
           (pages <= (snapshot.Limits.ResidentMemoryPages - snapshot.Usage.ResidentMemoryPages)) &&
           (metadataBytes <= (snapshot.Limits.KernelMetadataBytes - snapshot.Usage.KernelMetadataBytes));
}

[[nodiscard]] constexpr bool RootSnapshotIsBspOnly(const Kernel::Memory::AddressSpaceSnapshot& snapshot,
                                                   const Kernel::Memory::AddressSpaceId identity,
                                                   const Kernel::Scheduler::CpuId bspProcessor) noexcept
{
    return snapshot.Identity.IsEqualTo(identity) && (snapshot.State == Kernel::Memory::AddressSpaceState::Ready) &&
           (snapshot.ResidentProcessorMask == ProcessorBit(bspProcessor)) && !snapshot.ActivatingProcessor.IsValid() &&
           !snapshot.TransitionProcessor.IsValid() && (snapshot.TargetProcessorMask == 0U) &&
           (snapshot.AcknowledgedProcessorMask == 0U) && BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] bool ApplicationProcessorsAreUnadmitted(Ia64BspRuntimeMemory& runtimeMemory,
                                                      const Foundation::UInt8 processorCount) noexcept
{
    for (Foundation::UInt8 processorValue{1U}; processorValue < processorCount; ++processorValue)
    {
        const auto snapshot =
            runtimeMemory.GetApplicationProcessorAdmissionSnapshot(Kernel::Scheduler::CpuId{processorValue});
        if (!snapshot.HasValue() ||
            (snapshot.GetValue().State != Ia64BspRuntimeApplicationProcessorAdmissionState::Unadmitted) ||
            (snapshot.GetValue().Controller.State !=
             Kernel::Memory::ProcessorAddressSpaceControllerState::Uninitialized) ||
            (snapshot.GetValue().Controller.IdleStorage != nullptr) ||
            (snapshot.GetValue().Controller.ActiveStorage != nullptr) ||
            (snapshot.GetValue().Controller.PreviousStorage != nullptr))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool ResourceUsageHasExactIncrease(const Kernel::Resource::ResourceDomainSnapshot& before,
                                                           const Kernel::Resource::ResourceDomainSnapshot& after,
                                                           const Foundation::UInt64 pages,
                                                           const Foundation::UInt64 metadataBytes) noexcept
{
    return (after.Identity.GetValue() == before.Identity.GetValue()) && after.Parent.IsEqualTo(before.Parent) &&
           (after.Usage.CommittedMemoryPages == (before.Usage.CommittedMemoryPages + pages)) &&
           (after.Usage.ResidentMemoryPages == (before.Usage.ResidentMemoryPages + pages)) &&
           (after.Usage.KernelMetadataBytes == (before.Usage.KernelMetadataBytes + metadataBytes)) &&
           (after.Usage.CapabilityHandles == before.Usage.CapabilityHandles) &&
           (after.Usage.IpcQueueBytes == before.Usage.IpcQueueBytes) && (after.Usage.Threads == before.Usage.Threads) &&
           (after.Usage.ChildDomains == before.Usage.ChildDomains) && (after.Depth == before.Depth) &&
           (after.State == before.State) && BytesAreZero(after.Reserved, sizeof(after.Reserved));
}

[[nodiscard]] constexpr Kernel::Memory::PagePermissions SupervisorReadWritePermissions() noexcept
{
    return Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}.With(
        Kernel::Memory::PagePermission::Write);
}

[[nodiscard]] constexpr bool MappingRecordMatches(const Kernel::Memory::AddressSpaceMappingRecord& record,
                                                  const Kernel::Memory::MemoryObjectId memoryObject,
                                                  const Kernel::Memory::FrameId frame,
                                                  const Foundation::UInt64 virtualStart,
                                                  const Foundation::UInt64 physicalStart,
                                                  const Foundation::UInt64 pages) noexcept
{
    const Kernel::Memory::PagePermissions permissions = SupervisorReadWritePermissions();
    return record.MemoryObject.IsEqualTo(memoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(frame) && (record.VirtualStart.GetValue() == virtualStart) &&
           (record.PhysicalStart.GetValue() == physicalStart) && (record.Pages.GetValue() == pages) &&
           (record.MemoryObjectOffsetPages == 0U) && (record.Permissions.GetBits() == permissions.GetBits()) &&
           (record.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (record.Kind == Kernel::Memory::FrameKind::Ram) &&
           (record.Commitment == Kernel::Memory::MemoryCommitment::Pinned) && record.RuntimeMapping &&
           BytesAreZero(record.Reserved, sizeof(record.Reserved));
}

[[nodiscard]] constexpr bool FrameSnapshotMatches(const Kernel::Memory::FrameSnapshot& snapshot,
                                                  const Foundation::UInt64 physicalStart,
                                                  const Foundation::UInt64 pages) noexcept
{
    return (snapshot.State == Kernel::Memory::FrameState::Ready) &&
           (snapshot.Descriptor.PhysicalStart.GetValue() == physicalStart) &&
           (snapshot.Descriptor.Pages.GetValue() == pages) &&
           (snapshot.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.Kind == Kernel::Memory::FrameKind::Ram) &&
           BytesAreZero(snapshot.Descriptor.Reserved, sizeof(snapshot.Descriptor.Reserved)) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] constexpr bool
MemoryObjectSnapshotMatches(const Kernel::Memory::MemoryObjectSnapshot& snapshot,
                            const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                            const Kernel::Object::ResourceDomainId resourceDomainIdentifier,
                            const Foundation::UInt64 pages) noexcept
{
    const Kernel::Memory::PagePermissions permissions = SupervisorReadWritePermissions();
    return (snapshot.State == Kernel::Memory::MemoryObjectState::Ready) &&
           (snapshot.Descriptor.Pages.GetValue() == pages) &&
           (snapshot.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.MaximumPermissions.GetBits() == permissions.GetBits()) &&
           (snapshot.Descriptor.Commitment == Kernel::Memory::MemoryCommitment::Pinned) &&
           BytesAreZero(snapshot.Descriptor.Reserved, sizeof(snapshot.Descriptor.Reserved)) &&
           snapshot.ResourceDomainIdentity.IsEqualTo(resourceDomainIdentity) &&
           (snapshot.ResourceDomain.GetValue() == resourceDomainIdentifier.GetValue()) &&
           (snapshot.ActiveMappings == 1U) && BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}
} // namespace

OwnerResult Ia64ApplicationProcessorRuntimeMappingOwner::Fail(const OwnerError error) noexcept
{
    m_failure.Error = error;
    if (m_failure.RootMayBeFaulted || (m_finalRootSnapshot.State == Kernel::Memory::AddressSpaceState::Faulted))
    {
        m_failure.RootMayBeFaulted = true;
        m_state = Ia64ApplicationProcessorRuntimeMappingOwnerState::RootFaulted;
    }
    else
    {
        m_state = Ia64ApplicationProcessorRuntimeMappingOwnerState::Failed;
    }
    ClearPrepareInputs();
    return OwnerResult::Failure(m_failure);
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::PreflightBspAndTopology() noexcept
{
    OwnerError error = CaptureBspRuntimeSnapshot();
    if (error == OwnerError::None)
    {
        error = BorrowAndValidateBspRoot();
    }
    if (error == OwnerError::None)
    {
        error = ValidateStorageTopology();
    }
    if (error == OwnerError::None)
    {
        error = ValidateApplicationProcessorState();
    }
    return error;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::CaptureBspRuntimeSnapshot() noexcept
{
    // Construct the large passive snapshot directly in retained owner storage;
    // the target ABI would otherwise reserve its 592-byte return temporary on
    // the firmware stack.
    ::new (static_cast<void*>(&m_bspSnapshot)) Ia64BspRuntimeMemorySnapshot{m_bspRuntimeMemory->GetSnapshot()};
    if (m_bspSnapshot.State != Ia64BspRuntimeMemoryState::Ready)
    {
        return OwnerError::BspRuntimeMemoryNotReady;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::BorrowAndValidateBspRoot() noexcept
{
    const auto borrowedRoot = m_bspRuntimeMemory->BorrowReadyRoot();
    if (!borrowedRoot.HasValue())
    {
        m_failure.RootBorrowError = borrowedRoot.GetError();
        return OwnerError::BspRootBorrowRejected;
    }
    const Ia64BspRuntimeRootView root = borrowedRoot.GetValue();
    if ((root.AddressSpaceStorage == nullptr) || !root.AddressSpaceIdentity.IsValid() ||
        !root.AddressSpaceIdentity.IsFromRegistry(*m_registry) || !root.Processor.IsValid() ||
        (root.Processor.GetValue() != 0U))
    {
        return OwnerError::InvalidBspRoot;
    }

    m_rootAddressSpace = root.AddressSpaceStorage;
    m_bspProcessor = root.Processor;
    m_summary.AddressSpaceIdentity = root.AddressSpaceIdentity;
    m_initialRootSnapshot = m_rootAddressSpace->GetSnapshot();
    m_finalRootSnapshot = m_initialRootSnapshot;
    if (!RootSnapshotIsBspOnly(m_initialRootSnapshot, m_summary.AddressSpaceIdentity, m_bspProcessor) ||
        (m_bspSnapshot.AddressSpaceStorage != m_rootAddressSpace) ||
        !m_bspSnapshot.Summary.AddressSpaceIdentity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (m_bspSnapshot.Summary.BspProcessorIndex != m_bspProcessor.GetValue()) ||
        (m_bspSnapshot.Summary.ResidentProcessorMask != ProcessorBit(m_bspProcessor)) ||
        (m_bspSnapshot.Summary.ControllerState != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        !m_bspSnapshot.Summary.BackendClaimed || !m_bspSnapshot.Summary.ExactLedgerVerified ||
        (m_bspSnapshot.Summary.MappingCount != m_initialRootSnapshot.MappingCount))
    {
        return OwnerError::InvalidBspRoot;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::ValidateStorageTopology() noexcept
{
    if (m_executionStorageOwner->GetState() != ApplicationProcessorExecutionStorageOwnerState::Ready)
    {
        return OwnerError::ExecutionStorageOwnerNotReady;
    }
    if (m_workerStorageOwner->GetState() != Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Ready)
    {
        return OwnerError::WorkerStorageOwnerNotReady;
    }

    const Foundation::UInt8 processorCount = m_executionStorageOwner->GetProcessorCount();
    const Foundation::UInt8 enabledProcessorMask = m_executionStorageOwner->GetEnabledProcessorMask();
    if ((processorCount == 0U) || (processorCount > Kernel::Scheduler::MaximumProcessorCount) ||
        (enabledProcessorMask != ExpectedProcessorMask(processorCount)) ||
        (m_executionStorageOwner->GetDescriptorCount() != static_cast<Foundation::UInt8>(processorCount - 1U)) ||
        (m_bspSnapshot.Summary.ProfiledProcessorMask != enabledProcessorMask))
    {
        return OwnerError::InvalidExecutionStorageOwner;
    }
    if ((m_workerStorageOwner->GetProcessorCount() != processorCount) ||
        (m_workerStorageOwner->GetEnabledProcessorMask() != enabledProcessorMask) ||
        (m_workerStorageOwner->GetWorkerStorageCount() != static_cast<Foundation::UInt8>(processorCount - 1U)))
    {
        return OwnerError::InvalidWorkerStorageOwner;
    }

    m_summary.InitialMappingCount = m_initialRootSnapshot.MappingCount;
    m_summary.FinalMappingCount = m_initialRootSnapshot.MappingCount;
    m_summary.ProcessorCount = processorCount;
    m_summary.EnabledProcessorMask = enabledProcessorMask;
    m_summary.ApplicationProcessorCount = static_cast<Foundation::UInt8>(processorCount - 1U);
    m_summary.ResidentProcessorMask = ProcessorBit(m_bspProcessor);
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::ValidateApplicationProcessorState() noexcept
{
    for (Foundation::UInt8 processorValue{1U}; processorValue < Kernel::Scheduler::MaximumProcessorCount;
         ++processorValue)
    {
        if (m_bspSnapshot.Bindings[processorValue] != Ia64BspRuntimeBindingState::Unadmitted)
        {
            return OwnerError::ApplicationProcessorAlreadyAdmitted;
        }
    }
    if ((m_bspSnapshot.Bindings[0U] != Ia64BspRuntimeBindingState::Active) ||
        !ApplicationProcessorsAreUnadmitted(*m_bspRuntimeMemory, m_summary.ProcessorCount))
    {
        return OwnerError::ApplicationProcessorAlreadyAdmitted;
    }
    if (!m_shootdownDispatcher->GetProcessor().IsValid() ||
        (m_shootdownDispatcher->GetProcessor().GetValue() != m_bspProcessor.GetValue()))
    {
        return OwnerError::InvalidDispatcher;
    }

    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::PreflightResourceDomain() noexcept
{
    if (!m_resourceDomainIdentity.IsValid() || !m_resourceDomainIdentity.IsFromRegistry(*m_registry) ||
        !m_resourceDomain->IsReady())
    {
        return OwnerError::InvalidResourceDomain;
    }
    Kernel::Object::ObjectPin verification;
    m_failure.PinError = m_registry->PinObject(m_resourceDomainIdentity.GetObjectId(),
                                               Kernel::Object::ObjectType::ResourceDomain, verification);
    if ((m_failure.PinError != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != m_resourceDomain))
    {
        return OwnerError::InvalidResourceDomain;
    }
    verification.Reset();

    m_initialResourceDomainSnapshot = m_resourceDomain->GetSnapshot();
    m_finalResourceDomainSnapshot = m_initialResourceDomainSnapshot;
    m_resourceDomainIdentifier = m_resourceDomain->GetIdentifier();
    if (!m_resourceDomainIdentifier.IsValid() ||
        (m_initialResourceDomainSnapshot.Identity.GetValue() != m_resourceDomainIdentifier.GetValue()) ||
        m_initialResourceDomainSnapshot.Parent.IsValid() || (m_initialResourceDomainSnapshot.Depth != 0U) ||
        (m_initialResourceDomainSnapshot.State != Kernel::Resource::ResourceDomainState::Ready) ||
        !BytesAreZero(m_initialResourceDomainSnapshot.Reserved, sizeof(m_initialResourceDomainSnapshot.Reserved)))
    {
        return OwnerError::InvalidResourceDomain;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::PreflightLayoutAndCapacity() noexcept
{
    const auto idleStorage = m_executionStorageOwner->GetDescriptorView();
    if (!idleStorage.HasValue())
    {
        return OwnerError::InvalidExecutionStorageOwner;
    }
    const auto workerPool = m_workerStorageOwner->GetWorkerPool();
    if (!workerPool.HasValue())
    {
        return OwnerError::InvalidWorkerStorageOwner;
    }
    m_workerPool = workerPool.GetValue();
    m_failure.LayoutError = Ia64ApplicationProcessorRuntimeLayoutPlanner::Plan(
        Ia64ApplicationProcessorRuntimeLayoutRequest{
            .IdleStorage = idleStorage.GetValue(),
            .WorkerPool = m_workerPool,
            .ProcessorCount = m_summary.ProcessorCount,
            .EnabledProcessorMask = m_summary.EnabledProcessorMask,
            .CommonPhysicalAddressBits = m_bspSnapshot.Summary.CommonCapabilities.PhysicalAddressBits,
            .CommonImplementedVirtualAddressMostSignificantBit =
                m_bspSnapshot.Summary.CommonCapabilities.ImplementedVirtualAddressMostSignificantBit,
        },
        m_plan);
    if (m_failure.LayoutError != Ia64ApplicationProcessorRuntimeLayoutError::None)
    {
        return OwnerError::LayoutRejected;
    }
    if (m_plan.LayoutCount != m_summary.ApplicationProcessorCount)
    {
        return OwnerError::LayoutRejected;
    }

    const Foundation::Size applicationProcessorCount{m_plan.LayoutCount};
    const Foundation::Size requiredMappingCount{applicationProcessorCount * 2U};
    const Foundation::Size requiredFrameCount{applicationProcessorCount == 0U ? 0U : applicationProcessorCount + 1U};
    const Foundation::Size requiredMemoryObjectCount{applicationProcessorCount * 2U};
    const Foundation::Size requiredObjectCount{requiredFrameCount + requiredMemoryObjectCount};
    // Active count is only a necessary lower bound. Quiescing or retired
    // entries remain occupied, so actual registration can still fail after
    // persistent initialization and is therefore a terminal failure.
    const Foundation::Size registryCapacity = (m_registry != nullptr) ? m_registry->GetCapacity() : 0U;
    if ((m_initialRegistryObjectCount > registryCapacity) ||
        (requiredObjectCount > (registryCapacity - m_initialRegistryObjectCount)))
    {
        return OwnerError::RegistryCapacityExceeded;
    }
    if ((m_initialRootSnapshot.MappingCount > Kernel::Memory::MaximumAddressSpaceMappings) ||
        (requiredMappingCount > (Kernel::Memory::MaximumAddressSpaceMappings - m_initialRootSnapshot.MappingCount)))
    {
        return OwnerError::MappingCapacityExceeded;
    }
    const Foundation::UInt64 requiredPages = static_cast<Foundation::UInt64>(applicationProcessorCount) *
                                             (16U + Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor);
    const Foundation::UInt64 requiredMetadataBytes =
        static_cast<Foundation::UInt64>(requiredMemoryObjectCount) * sizeof(Kernel::Memory::MemoryObject);
    if (!ResourceCapacityIsSufficient(m_initialResourceDomainSnapshot, requiredPages, requiredMetadataBytes))
    {
        return OwnerError::ResourceCapacityExceeded;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::PreflightExistingMappings() noexcept
{
    for (Foundation::Size existingIndex{0U}; existingIndex < m_initialRootSnapshot.MappingCount; ++existingIndex)
    {
        const auto existingResult = m_rootAddressSpace->TryGetMapping(existingIndex);
        if (!existingResult.HasValue())
        {
            return OwnerError::InvalidExistingMapping;
        }
        const Kernel::Memory::AddressSpaceMappingRecord& existing = existingResult.GetValue();
        Foundation::UInt64 existingVirtualEnd{0U};
        Foundation::UInt64 existingPhysicalEnd{0U};
        if (((existing.VirtualStart.GetValue() & (PageSizeBytes - 1U)) != 0U) ||
            ((existing.PhysicalStart.GetValue() & (PageSizeBytes - 1U)) != 0U) ||
            !TryGetPageRangeEnd(existing.VirtualStart.GetValue(), existing.Pages.GetValue(), existingVirtualEnd) ||
            !TryGetPageRangeEnd(existing.PhysicalStart.GetValue(), existing.Pages.GetValue(), existingPhysicalEnd) ||
            !BytesAreZero(existing.Reserved, sizeof(existing.Reserved)))
        {
            return OwnerError::InvalidExistingMapping;
        }

        for (Foundation::Size layoutIndex{0U}; layoutIndex < m_plan.LayoutCount; ++layoutIndex)
        {
            const Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[layoutIndex];
            if (RangesOverlap(existing.VirtualStart.GetValue(), existingVirtualEnd, layout.SlotVirtualBase,
                              layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeProcessorStride))
            {
                m_failure.FailingProcessor = layout.Processor;
                return OwnerError::GuardedVirtualRangeOccupied;
            }
            if (RangesOverlap(existing.PhysicalStart.GetValue(), existingPhysicalEnd, layout.IdlePhysicalBase,
                              layout.IdlePhysicalBase + Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes) ||
                RangesOverlap(existing.PhysicalStart.GetValue(), existingPhysicalEnd, layout.WorkerPhysicalBase,
                              layout.WorkerPhysicalBase + Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes))
            {
                m_failure.FailingProcessor = layout.Processor;
                return OwnerError::PhysicalStorageAlreadyMapped;
            }
        }
    }
    return OwnerError::None;
}

void Ia64ApplicationProcessorRuntimeMappingOwner::CaptureFinalDiagnostics() noexcept
{
    if (m_rootAddressSpace != nullptr)
    {
        m_finalRootSnapshot = m_rootAddressSpace->GetSnapshot();
    }
    if (m_resourceDomain != nullptr)
    {
        m_finalResourceDomainSnapshot = m_resourceDomain->GetSnapshot();
    }
    if (m_registry != nullptr)
    {
        m_finalRegistryObjectCount = m_registry->GetActiveCount();
    }
}

void Ia64ApplicationProcessorRuntimeMappingOwner::ClearPrepareInputs() noexcept
{
    m_bspRuntimeMemory = nullptr;
    m_executionStorageOwner = nullptr;
    m_workerStorageOwner = nullptr;
    m_registry = nullptr;
    m_issuer = nullptr;
    m_resourceDomain = nullptr;
    m_shootdownDispatcher = nullptr;
    m_rootAddressSpace = nullptr;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::FinishZeroApplicationProcessorResult() noexcept
{
    CaptureFinalDiagnostics();
    if (!RootSnapshotIsBspOnly(m_finalRootSnapshot, m_summary.AddressSpaceIdentity, m_bspProcessor) ||
        (m_finalRootSnapshot.MappingCount != m_initialRootSnapshot.MappingCount) ||
        !ResourceUsageHasExactIncrease(m_initialResourceDomainSnapshot, m_finalResourceDomainSnapshot, 0U, 0U) ||
        (m_finalRegistryObjectCount != m_initialRegistryObjectCount) || (m_initializedFrameCount != 0U) ||
        (m_initializedMemoryObjectCount != 0U) || (m_registeredObjectCount != 0U) ||
        (m_remotePublishAttemptCount != 0U))
    {
        return OwnerError::PostconditionRejected;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::InitializeObjects() noexcept
{
    const Kernel::Memory::PagePermissions permissions = SupervisorReadWritePermissions();
    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        const Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
        m_ownership[index].Processor = layout.Processor;
        m_failure.FrameError = m_idleFrames[index].Initialize(
            m_resourceDomainIdentifier, Kernel::Memory::FrameDescriptor{
                                            .PhysicalStart = Kernel::Memory::PhysicalAddress{layout.IdlePhysicalBase},
                                            .Pages = Kernel::Memory::PageCount{16U},
                                            .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                            .Kind = Kernel::Memory::FrameKind::Ram,
                                            .Reserved = {},
                                        });
        if (m_failure.FrameError != Kernel::Memory::FrameError::None)
        {
            m_failure.FailingProcessor = layout.Processor;
            m_failure.MappingKind = MappingKind::Idle;
            return OwnerError::FrameInitializationRejected;
        }
        ++m_initializedFrameCount;
    }

    m_failure.FrameError = m_workerFrame.Initialize(
        m_resourceDomainIdentifier, Kernel::Memory::FrameDescriptor{
                                        .PhysicalStart = Kernel::Memory::PhysicalAddress{m_workerPool.PhysicalStart},
                                        .Pages = Kernel::Memory::PageCount{m_workerPool.PageCount},
                                        .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                        .Kind = Kernel::Memory::FrameKind::Ram,
                                        .Reserved = {},
                                    });
    if (m_failure.FrameError != Kernel::Memory::FrameError::None)
    {
        m_failure.MappingKind = MappingKind::Worker;
        return OwnerError::FrameInitializationRejected;
    }
    ++m_initializedFrameCount;

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        m_failure.MemoryObjectFailure =
            m_idleMemoryObjects[index].Initialize(*m_registry, m_resourceDomainIdentity, *m_resourceDomain,
                                                  Kernel::Memory::MemoryObjectDescriptor{
                                                      .Pages = Kernel::Memory::PageCount{16U},
                                                      .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                      .MaximumPermissions = permissions,
                                                      .Commitment = Kernel::Memory::MemoryCommitment::Pinned,
                                                      .Reserved = {},
                                                  });
        if (m_failure.MemoryObjectFailure.Error != Kernel::Memory::MemoryObjectError::None)
        {
            m_failure.FailingProcessor = m_plan.Processors[index].Processor;
            m_failure.MappingKind = MappingKind::Idle;
            return OwnerError::MemoryObjectInitializationRejected;
        }
        ++m_initializedMemoryObjectCount;

        m_failure.MemoryObjectFailure = m_workerMemoryObjects[index].Initialize(
            *m_registry, m_resourceDomainIdentity, *m_resourceDomain,
            Kernel::Memory::MemoryObjectDescriptor{
                .Pages = Kernel::Memory::PageCount{Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor},
                .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                .MaximumPermissions = permissions,
                .Commitment = Kernel::Memory::MemoryCommitment::Pinned,
                .Reserved = {},
            });
        if (m_failure.MemoryObjectFailure.Error != Kernel::Memory::MemoryObjectError::None)
        {
            m_failure.FailingProcessor = m_plan.Processors[index].Processor;
            m_failure.MappingKind = MappingKind::Worker;
            return OwnerError::MemoryObjectInitializationRejected;
        }
        ++m_initializedMemoryObjectCount;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::RegisterAndVerify(Kernel::Object::IKernelObject& object,
                                                                          const Kernel::Object::ObjectType expectedType,
                                                                          Kernel::Object::ObjectId& identity) noexcept
{
    const auto registered = m_issuer->CreateObject(object);
    if (!registered.HasValue())
    {
        m_failure.RegistryError = registered.GetError();
        return OwnerError::ObjectRegistrationRejected;
    }
    ++m_registeredObjectCount;
    Kernel::Object::ObjectPin verification;
    m_failure.PinError = m_registry->PinObject(registered.GetValue(), expectedType, verification);
    if ((m_failure.PinError != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != &object))
    {
        return OwnerError::RegistryBindingRejected;
    }
    identity = registered.GetValue();
    verification.Reset();
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::RegisterObjects() noexcept
{
    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        Kernel::Object::ObjectId identity{};
        const OwnerError error = RegisterAndVerify(m_idleFrames[index], Kernel::Object::ObjectType::Frame, identity);
        if (error != OwnerError::None)
        {
            m_failure.FailingProcessor = m_plan.Processors[index].Processor;
            m_failure.MappingKind = MappingKind::Idle;
            return error;
        }
        m_ownership[index].IdleFrame = Kernel::Memory::FrameId{*m_registry, identity};
    }

    Kernel::Object::ObjectId workerIdentity{};
    OwnerError error = RegisterAndVerify(m_workerFrame, Kernel::Object::ObjectType::Frame, workerIdentity);
    if (error != OwnerError::None)
    {
        m_failure.MappingKind = MappingKind::Worker;
        return error;
    }
    m_summary.WorkerFrame = Kernel::Memory::FrameId{*m_registry, workerIdentity};

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        Kernel::Object::ObjectId idleIdentity{};
        error = RegisterAndVerify(m_idleMemoryObjects[index], Kernel::Object::ObjectType::MemoryObject, idleIdentity);
        if (error != OwnerError::None)
        {
            m_failure.FailingProcessor = m_plan.Processors[index].Processor;
            m_failure.MappingKind = MappingKind::Idle;
            return error;
        }
        m_ownership[index].IdleMemoryObject = Kernel::Memory::MemoryObjectId{*m_registry, idleIdentity};

        Kernel::Object::ObjectId workerObjectIdentity{};
        error = RegisterAndVerify(m_workerMemoryObjects[index], Kernel::Object::ObjectType::MemoryObject,
                                  workerObjectIdentity);
        if (error != OwnerError::None)
        {
            m_failure.FailingProcessor = m_plan.Processors[index].Processor;
            m_failure.MappingKind = MappingKind::Worker;
            return error;
        }
        m_ownership[index].WorkerMemoryObject = Kernel::Memory::MemoryObjectId{*m_registry, workerObjectIdentity};
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::InstallMapping(const Foundation::Size index,
                                                                       const MappingKind kind) noexcept
{
    const bool idle = kind == MappingKind::Idle;
    const Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    Kernel::Memory::Frame& frame = idle ? m_idleFrames[index] : m_workerFrame;
    const Kernel::Memory::FrameId frameIdentity = idle ? m_ownership[index].IdleFrame : m_summary.WorkerFrame;
    Kernel::Memory::MemoryObject& memoryObject = idle ? m_idleMemoryObjects[index] : m_workerMemoryObjects[index];
    const Kernel::Memory::MemoryObjectId memoryObjectIdentity =
        idle ? m_ownership[index].IdleMemoryObject : m_ownership[index].WorkerMemoryObject;
    const Foundation::UInt64 virtualStart =
        idle ? layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeIdleAliasOffset
             : layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeWorkerAliasOffset;
    const Foundation::UInt64 pages = idle ? 16U : Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
    const Foundation::UInt64 frameOffsetPages =
        idle ? 0U : index * Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
    const Foundation::Size preparationIndex = (index * 2U) + (idle ? 0U : 1U);
    Kernel::Memory::PinnedFrameMappingPreparation& preparation = m_mappingPreparations[preparationIndex];

    m_failure.MappingPreparationError =
        preparation.Prepare(*m_registry, memoryObjectIdentity, memoryObject, frameIdentity, frame,
                            Kernel::Memory::PinnedFrameMappingRequest{
                                .VirtualStart = Kernel::Memory::VirtualAddress{virtualStart},
                                .Pages = Kernel::Memory::PageCount{pages},
                                .MemoryObjectOffsetPages = 0U,
                                .FrameOffsetPages = frameOffsetPages,
                                .Permissions = SupervisorReadWritePermissions(),
                                .Reserved = {},
                            });
    if (m_failure.MappingPreparationError != Kernel::Memory::PinnedFrameMappingError::None)
    {
        return OwnerError::MappingPreparationRejected;
    }

    const auto begin = m_rootAddressSpace->BeginPinnedFrameMapping(m_summary.AddressSpaceIdentity, preparation);
    if (!begin.HasValue())
    {
        m_failure.AddressSpaceFailure = begin.GetError();
        if (begin.GetError().PageTableError == Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown)
        {
            m_failure.RootMayBeFaulted = true;
        }
        return OwnerError::AddressSpaceMappingRejected;
    }
    const Kernel::Memory::AddressSpaceShootdownRequest request = begin.GetValue();
    RejectingRemoteShootdownTransport transport{m_remotePublishAttemptCount};
    const auto dispatched = m_shootdownDispatcher->Dispatch(*m_rootAddressSpace, request, transport);
    if (!dispatched.HasValue())
    {
        m_failure.ShootdownFailure = dispatched.GetError();
        m_failure.RootMayBeFaulted = true;
        return OwnerError::ShootdownRejected;
    }
    const Kernel::Memory::TlbShootdownReceipt& shootdown = dispatched.GetValue();
    const Foundation::UInt8 bspBit = ProcessorBit(m_bspProcessor);
    if (!shootdown.Token.IsEqualTo(request.GetToken()) || (shootdown.QueuedProcessorMask != 0U) ||
        (shootdown.AcknowledgedProcessorMask != bspBit) || !shootdown.Processor.IsValid() ||
        (shootdown.Processor.GetValue() != m_bspProcessor.GetValue()) || shootdown.RejectionObserved ||
        !BytesAreZero(shootdown.Reserved, sizeof(shootdown.Reserved)))
    {
        m_failure.AddressSpaceFailure.Error = m_rootAddressSpace->FailShootdown(request.GetToken());
        m_failure.RootMayBeFaulted = true;
        return OwnerError::ShootdownReceiptMismatch;
    }

    const auto completed = m_rootAddressSpace->CompleteMapping(request.GetToken());
    if (!completed.HasValue())
    {
        m_failure.AddressSpaceFailure = completed.GetError();
        m_failure.RootMayBeFaulted = true;
        return OwnerError::MappingCompletionRejected;
    }
    if (idle)
    {
        m_ownership[index].IdleMapping = completed.GetValue();
    }
    else
    {
        m_ownership[index].WorkerMapping = completed.GetValue();
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::InstallMappings() noexcept
{
    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        m_failure.FailingProcessor = m_plan.Processors[index].Processor;
        m_failure.MappingKind = MappingKind::Idle;
        OwnerError error = InstallMapping(index, MappingKind::Idle);
        if (error != OwnerError::None)
        {
            return error;
        }
        m_failure.MappingKind = MappingKind::Worker;
        error = InstallMapping(index, MappingKind::Worker);
        if (error != OwnerError::None)
        {
            return error;
        }
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::ValidateOwnedObjects(const Foundation::Size index) noexcept
{
    const Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    if (!FrameSnapshotMatches(m_idleFrames[index].GetSnapshot(), layout.IdlePhysicalBase, 16U) ||
        !MemoryObjectSnapshotMatches(m_idleMemoryObjects[index].GetSnapshot(), m_resourceDomainIdentity,
                                     m_resourceDomainIdentifier, 16U) ||
        !MemoryObjectSnapshotMatches(m_workerMemoryObjects[index].GetSnapshot(), m_resourceDomainIdentity,
                                     m_resourceDomainIdentifier,
                                     Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor))
    {
        m_failure.FailingProcessor = layout.Processor;
        return OwnerError::PostconditionRejected;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::ValidateCommittedMapping(
    const Foundation::Size index, const MappingKind kind, const Foundation::Size installedIndex,
    const Foundation::UInt64 priorSequence) noexcept
{
    const bool idle = kind == MappingKind::Idle;
    const Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    const Kernel::Memory::AddressSpaceMappingReceipt& receipt =
        idle ? m_ownership[index].IdleMapping : m_ownership[index].WorkerMapping;
    const Foundation::UInt64 virtualStart =
        idle ? layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeIdleAliasOffset
             : layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeWorkerAliasOffset;
    const Foundation::UInt64 physicalStart = idle ? layout.IdlePhysicalBase : layout.WorkerPhysicalBase;
    const Foundation::UInt64 pages = idle ? 16U : Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
    const Kernel::Memory::MemoryObjectId memoryObject =
        idle ? m_ownership[index].IdleMemoryObject : m_ownership[index].WorkerMemoryObject;
    const Kernel::Memory::FrameId frame = idle ? m_ownership[index].IdleFrame : m_summary.WorkerFrame;
    const auto record = m_rootAddressSpace->TryGetMapping(m_initialRootSnapshot.MappingCount + installedIndex);
    if (!record.HasValue() || (receipt.MappingSequence == 0U) ||
        ((priorSequence != 0U) && (receipt.MappingSequence != (priorSequence + 1U))) ||
        (receipt.MappingCount != (m_initialRootSnapshot.MappingCount + installedIndex + 1U)) ||
        (receipt.VirtualStart.GetValue() != virtualStart) || (receipt.Pages.GetValue() != pages) ||
        !MappingRecordMatches(record.HasValue() ? record.GetValue() : Kernel::Memory::AddressSpaceMappingRecord{},
                              memoryObject, frame, virtualStart, physicalStart, pages))
    {
        m_failure.FailingProcessor = layout.Processor;
        m_failure.MappingKind = kind;
        return OwnerError::PostconditionRejected;
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::ValidateGuardVacancy() noexcept
{
    for (Foundation::Size recordIndex{0U}; recordIndex < m_finalRootSnapshot.MappingCount; ++recordIndex)
    {
        const auto recordResult = m_rootAddressSpace->TryGetMapping(recordIndex);
        if (!recordResult.HasValue())
        {
            return OwnerError::PostconditionRejected;
        }
        const Kernel::Memory::AddressSpaceMappingRecord& record = recordResult.GetValue();
        Foundation::UInt64 recordEnd{0U};
        if (!TryGetPageRangeEnd(record.VirtualStart.GetValue(), record.Pages.GetValue(), recordEnd))
        {
            return OwnerError::PostconditionRejected;
        }
        for (Foundation::Size layoutIndex{0U}; layoutIndex < m_plan.LayoutCount; ++layoutIndex)
        {
            const Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[layoutIndex];
            const Foundation::UInt64 firstGuardStart =
                Ia64ApplicationProcessorRuntimeLayoutPlanner::GetFirstGuardVirtualStart(layout);
            const Foundation::UInt64 secondGuardStart =
                Ia64ApplicationProcessorRuntimeLayoutPlanner::GetSecondGuardVirtualStart(layout);
            if (RangesOverlap(record.VirtualStart.GetValue(), recordEnd, firstGuardStart,
                              firstGuardStart + Ia64ApplicationProcessorRuntimeFirstGuardSizeBytes) ||
                RangesOverlap(record.VirtualStart.GetValue(), recordEnd, secondGuardStart,
                              secondGuardStart + Ia64ApplicationProcessorRuntimeSecondGuardSizeBytes))
            {
                m_failure.FailingProcessor = layout.Processor;
                return OwnerError::PostconditionRejected;
            }
        }
    }
    return OwnerError::None;
}

OwnerError Ia64ApplicationProcessorRuntimeMappingOwner::VerifyFinalPostconditions() noexcept
{
    CaptureFinalDiagnostics();
    const Foundation::Size applicationProcessorCount{m_plan.LayoutCount};
    const Foundation::Size requiredMappingCount{applicationProcessorCount * 2U};
    const Foundation::Size requiredFrameCount{applicationProcessorCount + 1U};
    const Foundation::Size requiredMemoryObjectCount{applicationProcessorCount * 2U};
    const Foundation::Size requiredObjectCount{requiredFrameCount + requiredMemoryObjectCount};
    const Foundation::UInt64 requiredPages = static_cast<Foundation::UInt64>(applicationProcessorCount) *
                                             (16U + Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor);
    const Foundation::UInt64 requiredMetadataBytes =
        static_cast<Foundation::UInt64>(requiredMemoryObjectCount) * sizeof(Kernel::Memory::MemoryObject);
    const Foundation::Size expectedFinalMappingCount = m_initialRootSnapshot.MappingCount + requiredMappingCount;
    if (!RootSnapshotIsBspOnly(m_finalRootSnapshot, m_summary.AddressSpaceIdentity, m_bspProcessor) ||
        (m_finalRootSnapshot.MappingCount != expectedFinalMappingCount) ||
        (m_finalRegistryObjectCount != (m_initialRegistryObjectCount + requiredObjectCount)) ||
        !ResourceUsageHasExactIncrease(m_initialResourceDomainSnapshot, m_finalResourceDomainSnapshot, requiredPages,
                                       requiredMetadataBytes) ||
        (m_initializedFrameCount != requiredFrameCount) ||
        (m_initializedMemoryObjectCount != requiredMemoryObjectCount) ||
        (m_registeredObjectCount != requiredObjectCount) || (m_remotePublishAttemptCount != 0U) ||
        !FrameSnapshotMatches(m_workerFrame.GetSnapshot(), m_workerPool.PhysicalStart, m_workerPool.PageCount))
    {
        return OwnerError::PostconditionRejected;
    }

    Foundation::Size installedIndex{0U};
    Foundation::UInt64 priorSequence{0U};
    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        OwnerError error = ValidateOwnedObjects(index);
        if (error != OwnerError::None)
        {
            return error;
        }
        error = ValidateCommittedMapping(index, MappingKind::Idle, installedIndex, priorSequence);
        if (error != OwnerError::None)
        {
            return error;
        }
        priorSequence = m_ownership[index].IdleMapping.MappingSequence;
        ++installedIndex;
        error = ValidateCommittedMapping(index, MappingKind::Worker, installedIndex, priorSequence);
        if (error != OwnerError::None)
        {
            return error;
        }
        priorSequence = m_ownership[index].WorkerMapping.MappingSequence;
        ++installedIndex;
    }
    const OwnerError guardError = ValidateGuardVacancy();
    if (guardError != OwnerError::None)
    {
        return guardError;
    }

    m_summary.FinalMappingCount = expectedFinalMappingCount;
    m_summary.OwnedFrameCount = static_cast<Foundation::UInt8>(requiredFrameCount);
    m_summary.OwnedMemoryObjectCount = static_cast<Foundation::UInt8>(requiredMemoryObjectCount);
    m_summary.InstalledMappingCount = static_cast<Foundation::UInt8>(requiredMappingCount);
    return OwnerError::None;
}

OwnerResult Ia64ApplicationProcessorRuntimeMappingOwner::Prepare(
    Ia64BspRuntimeMemory& bspRuntimeMemory, const ApplicationProcessorExecutionStorageOwner& executionStorageOwner,
    const Ia64ApplicationProcessorRuntimeWorkerStorageOwner& workerStorageOwner,
    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
    const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain,
    Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher) noexcept
{
    if (m_state != Ia64ApplicationProcessorRuntimeMappingOwnerState::Fresh)
    {
        Failure repeated{};
        repeated.Error = OwnerError::AlreadyPrepared;
        return OwnerResult::Failure(repeated);
    }

    m_state = Ia64ApplicationProcessorRuntimeMappingOwnerState::Failed;
    m_failure = {};
    m_summary = {};
    m_bspRuntimeMemory = &bspRuntimeMemory;
    m_executionStorageOwner = &executionStorageOwner;
    m_workerStorageOwner = &workerStorageOwner;
    m_registry = &registry;
    m_issuer = &issuer;
    m_resourceDomain = &resourceDomain;
    m_shootdownDispatcher = &shootdownDispatcher;
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_initialRegistryObjectCount = registry.GetActiveCount();
    m_finalRegistryObjectCount = m_initialRegistryObjectCount;

    if (!issuer.IsFromRegistry(registry))
    {
        return Fail(OwnerError::IssuerRegistryMismatch);
    }

    OwnerError error = PreflightBspAndTopology();
    if (error != OwnerError::None)
    {
        return Fail(error);
    }
    error = PreflightResourceDomain();
    if (error != OwnerError::None)
    {
        return Fail(error);
    }
    error = PreflightLayoutAndCapacity();
    if (error != OwnerError::None)
    {
        return Fail(error);
    }
    error = PreflightExistingMappings();
    if (error != OwnerError::None)
    {
        return Fail(error);
    }

    if (m_plan.LayoutCount == 0U)
    {
        error = FinishZeroApplicationProcessorResult();
        if (error != OwnerError::None)
        {
            return Fail(error);
        }
        m_failure = {};
        m_state = Ia64ApplicationProcessorRuntimeMappingOwnerState::Ready;
        ClearPrepareInputs();
        return OwnerResult::Success(m_summary);
    }

    error = InitializeObjects();
    if (error == OwnerError::None)
    {
        error = RegisterObjects();
    }
    if (error == OwnerError::None)
    {
        error = InstallMappings();
    }
    if (error == OwnerError::None)
    {
        error = VerifyFinalPostconditions();
    }
    if (error != OwnerError::None)
    {
        CaptureFinalDiagnostics();
        return Fail(error);
    }

    m_failure = {};
    m_state = Ia64ApplicationProcessorRuntimeMappingOwnerState::Ready;
    ClearPrepareInputs();
    return OwnerResult::Success(m_summary);
}

Foundation::Result<Ia64ApplicationProcessorRuntimeLayoutPlan, AccessError>
Ia64ApplicationProcessorRuntimeMappingOwner::GetPlan() const noexcept
{
    using ResultType = Foundation::Result<Ia64ApplicationProcessorRuntimeLayoutPlan, AccessError>;
    if (m_state != Ia64ApplicationProcessorRuntimeMappingOwnerState::Ready)
    {
        return ResultType::Failure(AccessError::NotReady);
    }
    return ResultType::Success(m_plan);
}

Foundation::Result<Ia64ApplicationProcessorRuntimeMappingSummary, AccessError>
Ia64ApplicationProcessorRuntimeMappingOwner::GetSummary() const noexcept
{
    using ResultType = Foundation::Result<Ia64ApplicationProcessorRuntimeMappingSummary, AccessError>;
    if (m_state != Ia64ApplicationProcessorRuntimeMappingOwnerState::Ready)
    {
        return ResultType::Failure(AccessError::NotReady);
    }
    return ResultType::Success(m_summary);
}

Foundation::Result<Ia64ApplicationProcessorRuntimeMappingOwnership, AccessError>
Ia64ApplicationProcessorRuntimeMappingOwner::GetOwnership(const Kernel::Scheduler::CpuId processor) const noexcept
{
    using ResultType = Foundation::Result<Ia64ApplicationProcessorRuntimeMappingOwnership, AccessError>;
    if (m_state != Ia64ApplicationProcessorRuntimeMappingOwnerState::Ready)
    {
        return ResultType::Failure(AccessError::NotReady);
    }
    if (!processor.IsValid() || (processor.GetValue() == 0U) || (processor.GetValue() >= m_plan.ProcessorCount))
    {
        return ResultType::Failure(AccessError::InvalidProcessor);
    }
    const Foundation::Size index = static_cast<Foundation::Size>(processor.GetValue() - 1U);
    if ((index >= m_plan.LayoutCount) || (m_ownership[index].Processor.GetValue() != processor.GetValue()))
    {
        return ResultType::Failure(AccessError::InvalidProcessor);
    }
    return ResultType::Success(m_ownership[index]);
}

Ia64ApplicationProcessorRuntimeMappingSnapshot Ia64ApplicationProcessorRuntimeMappingOwner::GetSnapshot() const noexcept
{
    return Ia64ApplicationProcessorRuntimeMappingSnapshot{
        .Summary = m_summary,
        .Failure = m_failure,
        .InitialRoot = m_initialRootSnapshot,
        .FinalRoot = m_finalRootSnapshot,
        .InitialResourceDomain = m_initialResourceDomainSnapshot,
        .FinalResourceDomain = m_finalResourceDomainSnapshot,
        .InitialRegistryObjectCount = m_initialRegistryObjectCount,
        .FinalRegistryObjectCount = m_finalRegistryObjectCount,
        .InitializedFrameCount = m_initializedFrameCount,
        .InitializedMemoryObjectCount = m_initializedMemoryObjectCount,
        .RegisteredObjectCount = m_registeredObjectCount,
        .RemotePublishAttemptCount = m_remotePublishAttemptCount,
        .State = m_state,
        .Reserved = {},
    };
}
} // namespace Asteria::Platform::Ia64::Memory
