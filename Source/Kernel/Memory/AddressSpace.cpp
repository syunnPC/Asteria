// SPDX-License-Identifier: Apache-2.0

#include "AddressSpace.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 ShootdownAcknowledgedMask{0x0FU};
constexpr Foundation::UInt64 ShootdownTargetMask{0xF0U};
constexpr Foundation::UInt64 ShootdownFailureMask{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt8 ShootdownTargetShift{4U};
constexpr Foundation::UInt8 ShootdownSequenceShift{8U};

[[nodiscard]] constexpr bool TryGetRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                            Foundation::UInt64& end) noexcept
{
    if ((pages == 0U) || (pages > (MaximumUInt64 / MappingPageSizeBytes)))
    {
        return false;
    }
    const Foundation::UInt64 bytes = pages * MappingPageSizeBytes;
    if (start > (MaximumUInt64 - bytes))
    {
        return false;
    }
    end = start + bytes;
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool IsKnownCachePolicy(const MemoryCachePolicy cachePolicy) noexcept
{
    switch (cachePolicy)
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

[[nodiscard]] constexpr bool IsKnownCommitment(const MemoryCommitment commitment) noexcept
{
    switch (commitment)
    {
    case MemoryCommitment::Committed:
    case MemoryCommitment::Discardable:
    case MemoryCommitment::Reconstructible:
    case MemoryCommitment::BestEffort:
    case MemoryCommitment::Pinned:
    case MemoryCommitment::DeviceDma:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool
IsKnownActivationError(const Hal::Memory::AddressSpacePageTableActivationError error) noexcept
{
    using ActivationError = Hal::Memory::AddressSpacePageTableActivationError;
    switch (error)
    {
    case ActivationError::None:
    case ActivationError::NotReady:
    case ActivationError::InvalidOwner:
    case ActivationError::UnsupportedConfiguration:
    case ActivationError::RejectedBeforeCommit:
    case ActivationError::CommittedStateUnknown:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownPageTableError(const Hal::Memory::AddressSpacePageTableError error) noexcept
{
    using PageTableError = Hal::Memory::AddressSpacePageTableError;
    switch (error)
    {
    case PageTableError::None:
    case PageTableError::NotReady:
    case PageTableError::InvalidPlan:
    case PageTableError::MappingConflict:
    case PageTableError::CapacityExceeded:
    case PageTableError::CorruptState:
    case PageTableError::UnsupportedConfiguration:
    case PageTableError::CommittedStateUnknown:
    case PageTableError::InternalFailure:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr AddressSpaceActivationFailure
MakeActivationFailure(const AddressSpaceError error, const Hal::Memory::AddressSpacePageTableActivationError pageTable,
                      const bool processorMayBeResident = false) noexcept
{
    return AddressSpaceActivationFailure{
        .Error = error,
        .PageTable = pageTable,
        .ProcessorMayBeResident = processorMayBeResident,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr bool IsKnownInitialOwner(const InitialMappingOwner owner) noexcept
{
    switch (owner)
    {
    case InitialMappingOwner::NucleusCode:
    case InitialMappingOwner::NucleusReadOnlyData:
    case InitialMappingOwner::NucleusWritableData:
    case InitialMappingOwner::NucleusStack:
    case InitialMappingOwner::PageTable:
    case InitialMappingOwner::BootHandoff:
    case InitialMappingOwner::FirmwareRuntimeCode:
    case InitialMappingOwner::FirmwareRuntimeData:
    case InitialMappingOwner::Device:
    case InitialMappingOwner::NucleusRegisterBackingStore:
    case InitialMappingOwner::NucleusInterruptVectorTable:
    case InitialMappingOwner::NucleusBootstrapRuntimeData:
    case InitialMappingOwner::NucleusDescriptorTables:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr PagePermissions ConvertPermissions(const MappingPermissions permissions) noexcept
{
    PagePermissions result;
    if (permissions.Contains(MappingPermission::Read))
    {
        result = result.With(PagePermission::Read);
    }
    if (permissions.Contains(MappingPermission::Write))
    {
        result = result.With(PagePermission::Write);
    }
    if (permissions.Contains(MappingPermission::Execute))
    {
        result = result.With(PagePermission::Execute);
    }
    return result;
}

[[nodiscard]] bool SeedMappingIsValid(const InitialMapping& mapping) noexcept
{
    Foundation::UInt64 virtualEnd{0U};
    Foundation::UInt64 physicalEnd{0U};
    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 physicalStart = mapping.PhysicalStart.GetValue();
    const Foundation::UInt64 pages = mapping.Pages.GetValue();
    return ((virtualStart % MappingPageSizeBytes) == 0U) && ((physicalStart % MappingPageSizeBytes) == 0U) &&
           TryGetRangeEnd(virtualStart, pages, virtualEnd) && TryGetRangeEnd(physicalStart, pages, physicalEnd) &&
           mapping.Permissions.HasOnlyKnownBits() && mapping.Permissions.Contains(MappingPermission::Read) &&
           !(mapping.Permissions.Contains(MappingPermission::Write) &&
             mapping.Permissions.Contains(MappingPermission::Execute)) &&
           IsKnownCachePolicy(mapping.CachePolicy) && IsKnownInitialOwner(mapping.Owner) && (mapping.Reserved == 0U) &&
           ((mapping.Owner != InitialMappingOwner::Device) ||
            (!mapping.Permissions.Contains(MappingPermission::Execute) &&
             ((mapping.CachePolicy == MemoryCachePolicy::Uncached) ||
              (mapping.CachePolicy == MemoryCachePolicy::WriteCombining))));
}

[[nodiscard]] bool FaultPlanReservedBytesAreZero(const FaultFrameMappingPlan& plan) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(plan.Reserved); ++index)
    {
        if (plan.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool MappingReservedBytesAreZero(const AddressSpaceMappingRecord& mapping) noexcept
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

[[nodiscard]] constexpr AddressSpaceError MapAddressSpacePinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return AddressSpaceError::ObjectNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return AddressSpaceError::ObjectTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return AddressSpaceError::ObjectPinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return AddressSpaceError::InvalidIdentity;
    case Object::ObjectPinError::Ok:
        return AddressSpaceError::None;
    }
    return AddressSpaceError::InvalidIdentity;
}

[[nodiscard]] constexpr AddressSpaceError MapFramePinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return AddressSpaceError::FrameNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return AddressSpaceError::FrameTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return AddressSpaceError::FramePinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return AddressSpaceError::InvalidMappingPlan;
    case Object::ObjectPinError::Ok:
        return AddressSpaceError::None;
    }
    return AddressSpaceError::InvalidMappingPlan;
}

[[nodiscard]] constexpr AddressSpaceError MapMemoryObjectPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return AddressSpaceError::MemoryObjectNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return AddressSpaceError::MemoryObjectTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return AddressSpaceError::MemoryObjectPinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return AddressSpaceError::InvalidMappingPlan;
    case Object::ObjectPinError::Ok:
        return AddressSpaceError::None;
    }
    return AddressSpaceError::InvalidMappingPlan;
}

[[nodiscard]] constexpr AddressSpaceError MapDeviceRegionPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return AddressSpaceError::DeviceRegionNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return AddressSpaceError::DeviceRegionTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return AddressSpaceError::DeviceRegionPinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return AddressSpaceError::InvalidMappingPlan;
    case Object::ObjectPinError::Ok:
        return AddressSpaceError::None;
    }
    return AddressSpaceError::InvalidMappingPlan;
}

[[nodiscard]] constexpr AddressSpaceError MapFramePolicyError(const FrameMappingPolicyError error) noexcept
{
    switch (error)
    {
    case FrameMappingPolicyError::WriteExecuteConflict:
        return AddressSpaceError::PhysicalWriteExecuteAlias;
    case FrameMappingPolicyError::PhysicalOwnershipConflict:
        return AddressSpaceError::PhysicalOwnershipConflict;
    case FrameMappingPolicyError::ReferenceExhausted:
        return AddressSpaceError::MappingCapacityExceeded;
    case FrameMappingPolicyError::None:
    case FrameMappingPolicyError::InvalidPermissions:
        return AddressSpaceError::InvalidMappingPlan;
    }
    return AddressSpaceError::InvalidMappingPlan;
}

void ReleaseDeviceRegionMapping(Object::ObjectPin& pin) noexcept
{
    if (!pin.IsValid())
    {
        return;
    }
    static_cast<void>(static_cast<Device::DeviceRegion&>(pin.GetObject()).ReleaseMapping());
    pin.Reset();
}

[[nodiscard]] constexpr Foundation::UInt64 ShootdownSequence(const Foundation::UInt64 state) noexcept
{
    return (state >> ShootdownSequenceShift) & AddressSpaceMappingSequencePolicy::Maximum;
}

[[nodiscard]] constexpr Foundation::UInt8 ShootdownTargets(const Foundation::UInt64 state) noexcept
{
    return static_cast<Foundation::ProcessorMask>((state & ShootdownTargetMask) >> ShootdownTargetShift);
}

[[nodiscard]] constexpr Foundation::UInt8 ShootdownAcknowledgements(const Foundation::UInt64 state) noexcept
{
    return static_cast<Foundation::ProcessorMask>(state & ShootdownAcknowledgedMask);
}

[[nodiscard]] constexpr bool ShootdownWasRejected(const Foundation::UInt64 state) noexcept
{
    return (state & ShootdownFailureMask) != 0U;
}
} // Namespace

AddressSpaceFailure AddressSpace::MakeFailure(const AddressSpaceError error, const Object::ObjectPinError pinError,
                                              const Hal::Memory::AddressSpacePageTableError pageTableError) noexcept
{
    return AddressSpaceFailure{
        .Error = error,
        .PinError = pinError,
        .PageTableError = pageTableError,
        .Reserved = {},
    };
}

AddressSpaceError AddressSpace::Initialize(const Object::ResourceDomainId chargeOwner,
                                           Hal::Memory::IAddressSpacePageTable& pageTable,
                                           const InitialMappingPlan& seedPlan) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != AddressSpaceState::Uninitialized)
    {
        return AddressSpaceError::AlreadyInitialized;
    }
    if (!chargeOwner.IsValid())
    {
        return AddressSpaceError::InvalidConfiguration;
    }
    if (!seedPlan.IsSealed())
    {
        return AddressSpaceError::InvalidMappingPlan;
    }

    const Foundation::Size mappingCount = seedPlan.GetMappingCount();
    if ((mappingCount == 0U) || (mappingCount > MaximumInitialMappings))
    {
        return AddressSpaceError::InvalidMappingPlan;
    }
    for (Foundation::Size index{0U}; index < mappingCount; ++index)
    {
        const auto mappingResult = seedPlan.GetMapping(index);
        if (!mappingResult.HasValue() || !SeedMappingIsValid(mappingResult.GetValue()))
        {
            return AddressSpaceError::InvalidMappingPlan;
        }
    }

    if (!pageTable.IsReady() ||
        (pageTable.Claim(*this, seedPlan) != Hal::Memory::AddressSpacePageTableOwnershipError::None))
    {
        return AddressSpaceError::PageTableOwnershipRejected;
    }

    for (Foundation::Size index{0U}; index < mappingCount; ++index)
    {
        m_mappings[index] = ConvertSeedMapping(seedPlan.GetMapping(index).GetValue());
    }
    m_pageTable = &pageTable;
    m_seedMappingCount = mappingCount;
    m_runtimeMappingCount = 0U;
    m_pendingRuntimeSlot = MaximumRuntimeAddressSpaceMappings;
    m_mappingSequence = 0U;
    m_activationSequence = 0U;
    m_transitionSequence = 0U;
    m_reservedTransitionSequence = 0U;
    m_residentProcessorMask = 0U;
    m_activatingProcessor = Scheduler::CpuId{};
    m_transitionProcessor = Scheduler::CpuId{};
    m_shootdownState.StoreRelease(0U);
    m_state = AddressSpaceState::Unbound;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return AddressSpaceError::None;
}

AddressSpaceFailure AddressSpace::BindIdentity(Object::ObjectRegistry& registry, const AddressSpaceId identity) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return MakeFailure(AddressSpaceError::NotInitialized);
    }
    if (m_state == AddressSpaceState::Faulted)
    {
        return MakeFailure(AddressSpaceError::Faulted);
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return MakeFailure(AddressSpaceError::Quiesced);
    }
    if (m_state != AddressSpaceState::Unbound)
    {
        return MakeFailure(AddressSpaceError::AlreadyBound);
    }
    if (!identity.IsValid())
    {
        return MakeFailure(AddressSpaceError::InvalidIdentity);
    }
    if (!identity.IsFromRegistry(registry))
    {
        return MakeFailure(AddressSpaceError::RegistryMismatch);
    }

    Object::ObjectPin identityPin;
    const Object::ObjectPinError pinError =
        registry.PinObject(identity.GetObjectId(), Object::ObjectType::AddressSpace, identityPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(MapAddressSpacePinError(pinError), pinError);
    }
    if (&identityPin.GetObject() != static_cast<Object::IKernelObject*>(this))
    {
        identityPin.Reset();
        return MakeFailure(AddressSpaceError::ObjectInstanceMismatch);
    }
    identityPin.Reset();

    m_registry = &registry;
    m_identity = identity;
    m_state = AddressSpaceState::Ready;
    return MakeFailure(AddressSpaceError::None);
}

Foundation::Result<AddressSpaceTransitionReceipt, AddressSpaceError>
AddressSpace::ReserveProcessorTransition(const Scheduler::CpuId processor) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceTransitionReceipt, AddressSpaceError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!processor.IsValid())
    {
        return ResultType::Failure(AddressSpaceError::InvalidProcessor);
    }
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return ResultType::Failure(AddressSpaceError::NotInitialized);
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return ResultType::Failure(AddressSpaceError::Quiesced);
    }
    if ((m_transitionReservations[processor.GetValue()] != 0U) || (m_mappingIntents != 0U))
    {
        return ResultType::Failure(AddressSpaceError::ProcessorTransitionPending);
    }
    if (m_state == AddressSpaceState::Faulted)
    {
        return ResultType::Failure(AddressSpaceError::Faulted);
    }
    if (m_state == AddressSpaceState::ActivationPending)
    {
        return ResultType::Failure(AddressSpaceError::ActivationPending);
    }
    if (m_state == AddressSpaceState::ShootdownPending &&
        (((m_residentProcessorMask & (Foundation::ProcessorMask{1U} << processor.GetValue())) == 0U) ||
         !m_pageTable->DetachAcknowledgesShootdown(m_pendingUnmap)))
    {
        return ResultType::Failure(AddressSpaceError::ShootdownPending);
    }
    if (m_state != AddressSpaceState::Ready && m_state != AddressSpaceState::ShootdownPending)
    {
        return ResultType::Failure(AddressSpaceError::NotReady);
    }

    Foundation::UInt64 nextSequence{0U};
    if (!AddressSpaceMappingSequencePolicy::TryAdvance(m_transitionSequence, nextSequence))
    {
        return ResultType::Failure(AddressSpaceError::ProcessorTransitionSequenceExhausted);
    }

    m_transitionSequence = nextSequence;
    m_transitionReservations[processor.GetValue()] = nextSequence;
    m_transitionMask |= Foundation::ProcessorMask{1U} << processor.GetValue();
    m_reservedTransitionSequence = nextSequence;
    m_transitionProcessor = processor;
    return ResultType::Success(AddressSpaceTransitionReceipt{*this, nextSequence, processor});
}

bool AddressSpace::IsCurrentTransitionLocked(const AddressSpaceTransitionReceipt transition) const noexcept
{
    return transition.IsValid() && transition.IsFor(*this) && transition.GetProcessor().IsValid() &&
           (transition.GetSequence() == m_transitionReservations[transition.GetProcessor().GetValue()]);
}

bool AddressSpace::IsCurrentProcessorTransition(const AddressSpaceTransitionReceipt transition) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return IsCurrentTransitionLocked(transition);
}

AddressSpaceError AddressSpace::ReleaseProcessorTransition(const AddressSpaceTransitionReceipt transition) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!IsCurrentTransitionLocked(transition))
    {
        return AddressSpaceError::InvalidProcessorTransitionReceipt;
    }

    m_transitionReservations[transition.GetProcessor().GetValue()] = 0U;
    m_transitionMask &= ~(Foundation::ProcessorMask{1U} << transition.GetProcessor().GetValue());
    m_reservedTransitionSequence = 0U;
    m_transitionProcessor = Scheduler::CpuId{};
    for (Foundation::Size cpu{0U}; cpu < Scheduler::MaximumProcessorCount; ++cpu)
    {
        if (m_transitionReservations[cpu] != 0U)
        {
            m_reservedTransitionSequence = m_transitionReservations[cpu];
            m_transitionProcessor = Scheduler::CpuId{static_cast<Foundation::UInt8>(cpu)};
        }
    }
    return AddressSpaceError::None;
}

Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>
AddressSpace::ActivateReservedProcessor(const AddressSpaceTransitionReceipt transition) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>;
    using ActivationError = Hal::Memory::AddressSpacePageTableActivationError;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!IsCurrentTransitionLocked(transition))
    {
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::InvalidProcessorTransitionReceipt,
                                                         ActivationError::RejectedBeforeCommit));
    }
    return ActivateProcessorLocked(transition.GetProcessor());
}

Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>
AddressSpace::ActivateProcessor(const Scheduler::CpuId processor) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>;
    using ActivationError = Hal::Memory::AddressSpacePageTableActivationError;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!processor.IsValid())
    {
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::InvalidProcessor, ActivationError::RejectedBeforeCommit));
    }
    if (m_transitionMask != 0U)
    {
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::ProcessorTransitionPending, ActivationError::NotReady));
    }
    return ActivateProcessorLocked(processor);
}

Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>
AddressSpace::ActivateProcessorLocked(const Scheduler::CpuId processor) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceActivationReceipt, AddressSpaceActivationFailure>;
    using ActivationError = Hal::Memory::AddressSpacePageTableActivationError;

    if (!processor.IsValid())
    {
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::InvalidProcessor, ActivationError::RejectedBeforeCommit));
    }
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::NotInitialized, ActivationError::NotReady));
    }
    if (m_state == AddressSpaceState::Faulted)
    {
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::Faulted, ActivationError::NotReady));
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::Quiesced, ActivationError::NotReady));
    }
    if (m_state == AddressSpaceState::ActivationPending)
    {
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::ActivationPending, ActivationError::NotReady));
    }
    if (m_state == AddressSpaceState::ShootdownPending)
    {
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::ShootdownPending, ActivationError::NotReady));
    }
    if (m_state != AddressSpaceState::Ready)
    {
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::NotReady, ActivationError::NotReady));
    }
    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor.GetValue());
    if ((m_residentProcessorMask & processorBit) != 0U)
    {
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::ProcessorAlreadyResident, ActivationError::RejectedBeforeCommit));
    }
    Foundation::UInt64 nextSequence{0U};
    if (!AddressSpaceMappingSequencePolicy::TryAdvance(m_activationSequence, nextSequence))
    {
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::ActivationSequenceExhausted,
                                                         ActivationError::RejectedBeforeCommit));
    }
    m_activationSequence = nextSequence;
    m_activatingProcessor = processor;
    m_state = AddressSpaceState::ActivationPending;

    if (m_pageTable == nullptr)
    {
        m_residentProcessorMask = static_cast<Foundation::ProcessorMask>(m_residentProcessorMask | processorBit);
        m_activatingProcessor = Scheduler::CpuId{};
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::PageTableProtocolViolation,
                                                         ActivationError::CommittedStateUnknown, true));
    }
    const ActivationError activationError = m_pageTable->Activate(*this, processor);
    if (!IsKnownActivationError(activationError))
    {
        m_residentProcessorMask = static_cast<Foundation::ProcessorMask>(m_residentProcessorMask | processorBit);
        m_activatingProcessor = Scheduler::CpuId{};
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(MakeActivationFailure(AddressSpaceError::PageTableProtocolViolation,
                                                         ActivationError::CommittedStateUnknown, true));
    }
    if (activationError == ActivationError::CommittedStateUnknown)
    {
        m_residentProcessorMask = static_cast<Foundation::ProcessorMask>(m_residentProcessorMask | processorBit);
        m_activatingProcessor = Scheduler::CpuId{};
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::PageTableActivationCommittedStateUnknown, activationError, true));
    }
    if (activationError != ActivationError::None)
    {
        m_activatingProcessor = Scheduler::CpuId{};
        m_state = AddressSpaceState::Ready;
        return ResultType::Failure(
            MakeActivationFailure(AddressSpaceError::PageTableActivationRejected, activationError));
    }

    m_residentProcessorMask = static_cast<Foundation::ProcessorMask>(m_residentProcessorMask | processorBit);
    m_activatingProcessor = Scheduler::CpuId{};
    m_state = AddressSpaceState::Ready;
    return ResultType::Success(AddressSpaceActivationReceipt{
        .Sequence = nextSequence,
        .Processor = processor,
        .Reserved = {},
    });
}

AddressSpaceError AddressSpace::DetachProcessor(const Scheduler::CpuId processor) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!processor.IsValid())
    {
        return AddressSpaceError::InvalidProcessor;
    }
    if (m_transitionMask != 0U)
    {
        return AddressSpaceError::ProcessorTransitionPending;
    }
    return DetachProcessorLocked(processor);
}

AddressSpaceError AddressSpace::DetachReservedProcessor(const AddressSpaceTransitionReceipt transition) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!IsCurrentTransitionLocked(transition))
    {
        return AddressSpaceError::InvalidProcessorTransitionReceipt;
    }
    return DetachProcessorLocked(transition.GetProcessor());
}

AddressSpaceError AddressSpace::DetachProcessorLocked(const Scheduler::CpuId processor) noexcept
{
    if (!processor.IsValid())
    {
        return AddressSpaceError::InvalidProcessor;
    }
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return AddressSpaceError::NotInitialized;
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return AddressSpaceError::Quiesced;
    }
    if (m_state == AddressSpaceState::ShootdownPending && !m_pageTable->DetachAcknowledgesShootdown(m_pendingUnmap))
    {
        return AddressSpaceError::ShootdownPending;
    }
    if (m_state == AddressSpaceState::ActivationPending)
    {
        return AddressSpaceError::ActivationPending;
    }
    if ((m_state != AddressSpaceState::Ready) && (m_state != AddressSpaceState::Faulted) &&
        (m_state != AddressSpaceState::ShootdownPending))
    {
        return AddressSpaceError::NotReady;
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor.GetValue());
    if ((m_residentProcessorMask & processorBit) == 0U)
    {
        return AddressSpaceError::ProcessorNotResident;
    }

    // The native switch completed before this call. Acknowledge only the retiring root's current
    // transaction, retaining its resources until all remaining targets complete.
    if (m_state == AddressSpaceState::ShootdownPending)
    {
        const auto acknowledged = AcknowledgeShootdown(AddressSpaceMappingToken{*this, m_mappingSequence}, processor);
        if (acknowledged != AddressSpaceError::None && acknowledged != AddressSpaceError::ShootdownFailed)
        {
            return acknowledged;
        }
    }
    m_residentProcessorMask = static_cast<Foundation::ProcessorMask>(
        m_residentProcessorMask & static_cast<Foundation::ProcessorMask>(~processorBit));
    return AddressSpaceError::None;
}

AddressSpaceError AddressSpace::ValidateMappingLocked(const AddressSpaceMappingRecord& mapping,
                                                      const bool readOnlyBootstrapAlias) const noexcept
{
    if (m_registry == nullptr)
    {
        return AddressSpaceError::AddressSpaceMismatch;
    }
    if (!mapping.RuntimeMapping || !mapping.MemoryObject.IsValid() || !mapping.Frame.IsValid())
    {
        return AddressSpaceError::InvalidMappingPlan;
    }
    if (!mapping.MemoryObject.IsFromRegistry(*m_registry))
    {
        return AddressSpaceError::MemoryObjectRegistryMismatch;
    }
    if (!mapping.Frame.IsFromRegistry(*m_registry))
    {
        return AddressSpaceError::FrameRegistryMismatch;
    }
    if (mapping.Kind == FrameKind::Device)
    {
        if (!mapping.DeviceRegion.IsValid())
        {
            return AddressSpaceError::InvalidMappingPlan;
        }
        if (!mapping.DeviceRegion.IsFromRegistry(*m_registry))
        {
            return AddressSpaceError::DeviceRegionRegistryMismatch;
        }
    }
    else if (mapping.DeviceRegion.IsValid())
    {
        return AddressSpaceError::InvalidMappingPlan;
    }

    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 physicalStart = mapping.PhysicalStart.GetValue();
    const Foundation::UInt64 pages = mapping.Pages.GetValue();
    Foundation::UInt64 virtualEnd{0U};
    Foundation::UInt64 physicalEnd{0U};
    if (((virtualStart % MappingPageSizeBytes) != 0U) || ((physicalStart % MappingPageSizeBytes) != 0U) ||
        !TryGetRangeEnd(virtualStart, pages, virtualEnd) || !TryGetRangeEnd(physicalStart, pages, physicalEnd) ||
        !mapping.Permissions.HasOnlyKnownBits() || !mapping.Permissions.Contains(PagePermission::Read) ||
        (mapping.Permissions.Contains(PagePermission::Write) &&
         mapping.Permissions.Contains(PagePermission::Execute)) ||
        !IsKnownCachePolicy(mapping.CachePolicy) || !IsKnownFrameKind(mapping.Kind) ||
        !IsKnownCommitment(mapping.Commitment) || !MappingReservedBytesAreZero(mapping) ||
        ((mapping.Kind == FrameKind::Device) && (mapping.Permissions.Contains(PagePermission::Execute) ||
                                                 ((mapping.CachePolicy != MemoryCachePolicy::Uncached) &&
                                                  (mapping.CachePolicy != MemoryCachePolicy::WriteCombining)))))
    {
        return AddressSpaceError::InvalidMappingPlan;
    }

    if ((m_runtimeMappingCount >= MaximumRuntimeAddressSpaceMappings) ||
        (FindFreeRuntimeSlotLocked() == MaximumRuntimeAddressSpaceMappings))
    {
        return AddressSpaceError::MappingCapacityExceeded;
    }
    for (Foundation::Size index{0U}; index < (m_seedMappingCount + MaximumRuntimeAddressSpaceMappings); ++index)
    {
        const AddressSpaceMappingRecord& existing = m_mappings[index];
        if (existing.Pages.GetValue() == 0U)
        {
            continue;
        }
        Foundation::UInt64 existingVirtualEnd{0U};
        Foundation::UInt64 existingPhysicalEnd{0U};
        if (!TryGetRangeEnd(existing.VirtualStart.GetValue(), existing.Pages.GetValue(), existingVirtualEnd) ||
            !TryGetRangeEnd(existing.PhysicalStart.GetValue(), existing.Pages.GetValue(), existingPhysicalEnd))
        {
            return AddressSpaceError::Faulted;
        }
        if (RangesOverlap(virtualStart, virtualEnd, existing.VirtualStart.GetValue(), existingVirtualEnd))
        {
            return AddressSpaceError::VirtualOverlap;
        }
        if (!RangesOverlap(physicalStart, physicalEnd, existing.PhysicalStart.GetValue(), existingPhysicalEnd))
        {
            continue;
        }
        // The kernel may publish a dedicated, frozen bootstrap snapshot
        // within its seed RAM as a read-only Frame. Only the trusted Frame
        // initializer establishes this authority; ordinary pool Frames and
        // every writable/executable request retain the ownership check.
        if (!existing.RuntimeMapping && existing.Kind == FrameKind::Ram && readOnlyBootstrapAlias &&
            mapping.Kind == FrameKind::Ram && mapping.CachePolicy == existing.CachePolicy &&
            !mapping.Permissions.Contains(PagePermission::Write) &&
            !mapping.Permissions.Contains(PagePermission::Execute))
        {
            continue;
        }
        // Overlapping records must share the exact Frame generation. Both
        // preparations stamp that Frame's single immutable cache policy into
        // the record, so a surviving overlap can differ only in W^X.
        if (!existing.RuntimeMapping || !existing.Frame.IsEqualTo(mapping.Frame) || (existing.Kind != mapping.Kind) ||
            ((mapping.Kind == FrameKind::Device) && !existing.DeviceRegion.IsEqualTo(mapping.DeviceRegion)))
        {
            return AddressSpaceError::PhysicalOwnershipConflict;
        }
        const bool existingWritable = existing.Permissions.Contains(PagePermission::Write);
        const bool existingExecutable = existing.Permissions.Contains(PagePermission::Execute);
        const bool requestedWritable = mapping.Permissions.Contains(PagePermission::Write);
        const bool requestedExecutable = mapping.Permissions.Contains(PagePermission::Execute);
        if ((existingWritable && requestedExecutable) || (existingExecutable && requestedWritable))
        {
            return AddressSpaceError::PhysicalWriteExecuteAlias;
        }
    }
    return AddressSpaceError::None;
}

AddressSpaceError AddressSpace::ValidateBeginMappingStateLocked() const noexcept
{
    if (m_transitionMask != 0U)
    {
        return AddressSpaceError::ProcessorTransitionPending;
    }
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return AddressSpaceError::NotInitialized;
    }
    if (m_state == AddressSpaceState::Faulted)
    {
        return AddressSpaceError::Faulted;
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return AddressSpaceError::Quiesced;
    }
    if (m_state == AddressSpaceState::ShootdownPending)
    {
        return AddressSpaceError::ShootdownPending;
    }
    if (m_state == AddressSpaceState::ActivationPending)
    {
        return AddressSpaceError::ActivationPending;
    }
    if (m_state != AddressSpaceState::Ready)
    {
        return AddressSpaceError::NotReady;
    }
    return AddressSpaceError::None;
}

Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
AddressSpace::BeginFaultMapping(const FaultFrameMappingPreparation& preparation) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const AddressSpaceError stateError = ValidateBeginMappingStateLocked();
    if (stateError != AddressSpaceError::None)
    {
        return ResultType::Failure(MakeFailure(stateError));
    }
    if (!preparation.IsPrepared())
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::InvalidMappingPlan));
    }

    const FaultFrameMappingPlan& plan = preparation.GetPlan();
    if (!plan.Claim.IsValid() || !plan.AddressSpace.IsValid() || !plan.AddressSpace.IsEqualTo(m_identity))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::AddressSpaceMismatch));
    }
    if (!FaultPlanReservedBytesAreZero(plan))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::InvalidMappingPlan));
    }

    const AddressSpaceMappingRecord mapping = ConvertFaultMapping(plan);
    const AddressSpaceError validationError = ValidateMappingLocked(mapping);
    if (validationError != AddressSpaceError::None)
    {
        if (validationError == AddressSpaceError::Faulted)
        {
            m_state = AddressSpaceState::Faulted;
        }
        return ResultType::Failure(MakeFailure(validationError));
    }

    return BeginMappingLocked(mapping);
}

Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
AddressSpace::BeginPinnedFrameMapping(const AddressSpaceId target,
                                      const PinnedFrameMappingPreparation& preparation) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const AddressSpaceError stateError = ValidateBeginMappingStateLocked();
    if (stateError != AddressSpaceError::None)
    {
        return ResultType::Failure(MakeFailure(stateError));
    }
    if (!preparation.IsPrepared())
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::InvalidMappingPlan));
    }
    if (!target.IsValid() || !target.IsEqualTo(m_identity))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::AddressSpaceMismatch));
    }

    // A device Frame's mapping carries its region, which BeginMappingLocked retains; RAM carries
    // none.
    const AddressSpaceMappingRecord& mapping = preparation.GetMapping();
    if ((mapping.Commitment != MemoryCommitment::Pinned) ||
        ((mapping.Kind == FrameKind::Ram) == mapping.DeviceRegion.IsValid()))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::InvalidMappingPlan));
    }
    const AddressSpaceError validationError = ValidateMappingLocked(mapping, preparation.IsReadOnlyBootstrapFrame());
    if (validationError != AddressSpaceError::None)
    {
        if (validationError == AddressSpaceError::Faulted)
        {
            m_state = AddressSpaceState::Faulted;
        }
        return ResultType::Failure(MakeFailure(validationError));
    }

    return BeginMappingLocked(mapping);
}

Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
AddressSpace::BeginMappingLocked(const AddressSpaceMappingRecord& mapping) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>;

    if (!AddressSpaceMappingSequencePolicy::TryAdvance(m_mappingSequence, m_pendingMappingSequence))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::MappingSequenceExhausted));
    }

    const Foundation::Size slot = FindFreeRuntimeSlotLocked();
    if (slot == MaximumRuntimeAddressSpaceMappings)
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::MappingCapacityExceeded));
    }
    Object::ObjectPin& memoryObjectPin = m_runtimeMemoryObjectPins[slot];
    Object::ObjectPin& deviceRegionPin = m_runtimeDeviceRegionPins[slot];
    Object::ObjectPin& framePin = m_runtimeFramePins[slot];
    if (memoryObjectPin.IsValid() || deviceRegionPin.IsValid() || framePin.IsValid())
    {
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(MakeFailure(AddressSpaceError::Faulted));
    }

    const Object::ObjectPinError memoryObjectPinError =
        m_registry->PinObject(mapping.MemoryObject.GetObjectId(), Object::ObjectType::MemoryObject, memoryObjectPin);
    if (memoryObjectPinError != Object::ObjectPinError::Ok)
    {
        return ResultType::Failure(MakeFailure(MapMemoryObjectPinError(memoryObjectPinError), memoryObjectPinError));
    }
    MemoryObject& memoryObject = static_cast<MemoryObject&>(memoryObjectPin.GetObject());
    const MemoryObjectError memoryObjectError = memoryObject.RetainMapping(
        mapping.MemoryObjectOffsetPages, mapping.Pages, mapping.Permissions, mapping.CachePolicy, mapping.Commitment);
    if (memoryObjectError != MemoryObjectError::None)
    {
        memoryObjectPin.Reset();
        return ResultType::Failure(MakeFailure(AddressSpaceError::MemoryObjectRejected));
    }

    if (mapping.Kind == FrameKind::Device)
    {
        const Object::ObjectPinError deviceRegionPinError = m_registry->PinObject(
            mapping.DeviceRegion.GetObjectId(), Object::ObjectType::DeviceRegion, deviceRegionPin);
        if (deviceRegionPinError != Object::ObjectPinError::Ok)
        {
            static_cast<void>(memoryObject.ReleaseMapping());
            memoryObjectPin.Reset();
            return ResultType::Failure(
                MakeFailure(MapDeviceRegionPinError(deviceRegionPinError), deviceRegionPinError));
        }
        Device::DeviceRegion& deviceRegion = static_cast<Device::DeviceRegion&>(deviceRegionPin.GetObject());
        const Device::DeviceRegionError deviceRegionError =
            deviceRegion.RetainMapping(mapping.PhysicalStart, mapping.Pages, mapping.Permissions, mapping.CachePolicy);
        if (deviceRegionError != Device::DeviceRegionError::None)
        {
            deviceRegionPin.Reset();
            static_cast<void>(memoryObject.ReleaseMapping());
            memoryObjectPin.Reset();
            return ResultType::Failure(MakeFailure((deviceRegionError == Device::DeviceRegionError::ReferenceExhausted)
                                                       ? AddressSpaceError::MappingCapacityExceeded
                                                       : AddressSpaceError::DeviceRegionRejected));
        }
    }

    const Object::ObjectPinError pinError =
        m_registry->PinObject(mapping.Frame.GetObjectId(), Object::ObjectType::Frame, framePin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        ReleaseDeviceRegionMapping(deviceRegionPin);
        static_cast<void>(memoryObject.ReleaseMapping());
        memoryObjectPin.Reset();
        return ResultType::Failure(MakeFailure(MapFramePinError(pinError), pinError));
    }

    Frame& frame = static_cast<Frame&>(framePin.GetObject());
    {
        const FrameSnapshot frameSnapshot = frame.GetSnapshot();
        Foundation::UInt64 frameEnd{0U};
        Foundation::UInt64 mappingEnd{0U};
        if ((frameSnapshot.State != FrameState::Ready) ||
            ((frameSnapshot.Descriptor.CachePolicy != mapping.CachePolicy) &&
             !((mapping.Kind == FrameKind::Device) &&
               IsDeviceCachePolicyCompatible(frameSnapshot.Descriptor.CachePolicy, mapping.CachePolicy))) ||
            (frameSnapshot.Descriptor.Kind != mapping.Kind) ||
            !TryGetRangeEnd(frameSnapshot.Descriptor.PhysicalStart.GetValue(),
                            frameSnapshot.Descriptor.Pages.GetValue(), frameEnd) ||
            !TryGetRangeEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue(), mappingEnd) ||
            (mapping.PhysicalStart.GetValue() < frameSnapshot.Descriptor.PhysicalStart.GetValue()) ||
            (mappingEnd > frameEnd))
        {
            framePin.Reset();
            ReleaseDeviceRegionMapping(deviceRegionPin);
            static_cast<void>(memoryObject.ReleaseMapping());
            memoryObjectPin.Reset();
            return ResultType::Failure(MakeFailure(AddressSpaceError::InvalidMappingPlan));
        }
    }

    const FrameMappingPolicyError framePolicyError =
        frame.ReserveMappingPolicy(FrameMappingKey{}, *m_registry, mapping.Frame.GetObjectId(), mapping.PhysicalStart,
                                   mapping.Pages, mapping.Permissions);
    if (framePolicyError != FrameMappingPolicyError::None)
    {
        framePin.Reset();
        ReleaseDeviceRegionMapping(deviceRegionPin);
        static_cast<void>(memoryObject.ReleaseMapping());
        memoryObjectPin.Reset();
        return ResultType::Failure(MakeFailure(MapFramePolicyError(framePolicyError)));
    }

    // The written record stays pending, and invisible to TryGetMapping,
    // until CompleteMapping; a terminal failure keeps it pending.
    const Foundation::Size recordIndex = m_seedMappingCount + slot;
    m_mappings[recordIndex] = mapping;
    m_pendingRuntimeSlot = slot;

    m_pageTableMapResult = Hal::Memory::AddressSpacePageTableMapResult{};
    const Hal::Memory::AddressSpacePageTableError pageTableError = m_pageTable->Map(mapping, m_pageTableMapResult);
    if (pageTableError != Hal::Memory::AddressSpacePageTableError::None)
    {
        if (!IsKnownPageTableError(pageTableError) ||
            (pageTableError == Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown))
        {
            const Hal::Memory::AddressSpacePageTableError diagnosticError =
                IsKnownPageTableError(pageTableError) ? pageTableError
                                                      : Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
            m_mappingSequence = m_pendingMappingSequence;
            m_shootdownState.StoreRelease(0U);
            m_state = AddressSpaceState::Faulted;
            return ResultType::Failure(MakeFailure(AddressSpaceError::PageTableProtocolViolation,
                                                   Object::ObjectPinError::Ok, diagnosticError));
        }
        m_mappings[recordIndex] = AddressSpaceMappingRecord{};
        m_pendingRuntimeSlot = MaximumRuntimeAddressSpaceMappings;
        frame.ReleaseMappingPolicy(FrameMappingKey{}, *m_registry, mapping.Frame.GetObjectId(), mapping.PhysicalStart,
                                   mapping.Pages, mapping.Permissions);
        framePin.Reset();
        ReleaseDeviceRegionMapping(deviceRegionPin);
        static_cast<void>(memoryObject.ReleaseMapping());
        memoryObjectPin.Reset();
        if ((pageTableError == Hal::Memory::AddressSpacePageTableError::CorruptState) ||
            (pageTableError == Hal::Memory::AddressSpacePageTableError::InternalFailure) ||
            (pageTableError == Hal::Memory::AddressSpacePageTableError::NotReady))
        {
            m_state = AddressSpaceState::Faulted;
        }
        return ResultType::Failure(
            MakeFailure(AddressSpaceError::PageTableRejected, Object::ObjectPinError::Ok, pageTableError));
    }

    if ((m_pageTableMapResult.InvalidationStart.GetValue() != mapping.VirtualStart.GetValue()) ||
        (m_pageTableMapResult.InvalidationPages.GetValue() != mapping.Pages.GetValue()) ||
        (m_pageTableMapResult.Reserved != 0U))
    {
        m_mappingSequence = m_pendingMappingSequence;
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(MakeFailure(AddressSpaceError::PageTableProtocolViolation));
    }

    const Foundation::ProcessorMask targetMask = m_residentProcessorMask;
    m_mappingSequence = m_pendingMappingSequence;
    m_shootdownState.StoreRelease(PackShootdownState(m_pendingMappingSequence, targetMask, 0U));
    m_state = AddressSpaceState::ShootdownPending;
    return ResultType::Success(AddressSpaceShootdownRequest{AddressSpaceMappingToken{*this, m_pendingMappingSequence},
                                                            m_pageTableMapResult.InvalidationStart,
                                                            m_pageTableMapResult.InvalidationPages});
}

AddressSpaceError AddressSpace::AcknowledgeShootdown(const AddressSpaceMappingToken token,
                                                     const Scheduler::CpuId processor) noexcept
{
    if (!processor.IsValid())
    {
        return AddressSpaceError::InvalidProcessor;
    }
    if (!token.IsValid() || (token.m_addressSpace != this) ||
        (token.GetSequence() > AddressSpaceMappingSequencePolicy::Maximum))
    {
        return AddressSpaceError::InvalidShootdownToken;
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor.GetValue());
    Foundation::UInt64 expected = m_shootdownState.LoadAcquire();
    for (Foundation::Size attempt{0U}; attempt <= Scheduler::MaximumProcessorCount; ++attempt)
    {
        if (ShootdownSequence(expected) != token.GetSequence())
        {
            return AddressSpaceError::InvalidShootdownToken;
        }
        if ((ShootdownTargets(expected) & processorBit) == 0U)
        {
            return AddressSpaceError::ProcessorNotTargeted;
        }
        if ((ShootdownAcknowledgements(expected) & processorBit) != 0U)
        {
            return ShootdownWasRejected(expected) ? AddressSpaceError::ShootdownFailed : AddressSpaceError::None;
        }
        const Foundation::UInt64 desired = expected | processorBit;
        if (m_shootdownState.CompareExchangeStrongAcquireRelease(expected, desired))
        {
            return ShootdownWasRejected(desired) ? AddressSpaceError::ShootdownFailed : AddressSpaceError::None;
        }
    }
    return AddressSpaceError::ShootdownContention;
}

AddressSpaceError AddressSpace::RejectShootdown(const AddressSpaceMappingToken token,
                                                const Scheduler::CpuId processor) noexcept
{
    if (!processor.IsValid())
    {
        return AddressSpaceError::InvalidProcessor;
    }
    if (!token.IsValid() || (token.m_addressSpace != this) ||
        (token.GetSequence() > AddressSpaceMappingSequencePolicy::Maximum))
    {
        return AddressSpaceError::InvalidShootdownToken;
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor.GetValue());
    Foundation::UInt64 expected = m_shootdownState.LoadAcquire();
    for (Foundation::Size attempt{0U}; attempt <= Scheduler::MaximumProcessorCount; ++attempt)
    {
        if (ShootdownSequence(expected) != token.GetSequence())
        {
            return AddressSpaceError::InvalidShootdownToken;
        }
        if ((ShootdownTargets(expected) & processorBit) == 0U)
        {
            return AddressSpaceError::ProcessorNotTargeted;
        }
        if ((ShootdownAcknowledgements(expected) & processorBit) != 0U)
        {
            return ShootdownWasRejected(expected) ? AddressSpaceError::ShootdownFailed : AddressSpaceError::None;
        }
        const Foundation::UInt64 desired = expected | ShootdownFailureMask | processorBit;
        if (m_shootdownState.CompareExchangeStrongAcquireRelease(expected, desired))
        {
            return AddressSpaceError::None;
        }
    }
    return AddressSpaceError::ShootdownContention;
}

Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure>
AddressSpace::CompleteMapping(const AddressSpaceMappingToken token) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::NotInitialized));
    }
    if (m_state == AddressSpaceState::Faulted)
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::Faulted));
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::Quiesced));
    }
    if (m_state != AddressSpaceState::ShootdownPending)
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::NotReady));
    }
    if (!token.IsValid() || (token.m_addressSpace != this) || (token.GetSequence() != m_mappingSequence))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::InvalidShootdownToken));
    }

    const Foundation::UInt64 shootdownState = m_shootdownState.LoadAcquire();
    if (ShootdownSequence(shootdownState) != token.GetSequence())
    {
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(MakeFailure(AddressSpaceError::InvalidShootdownToken));
    }
    if (ShootdownAcknowledgements(shootdownState) != ShootdownTargets(shootdownState))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::ShootdownIncomplete));
    }
    if (ShootdownWasRejected(shootdownState))
    {
        m_shootdownState.StoreRelease(0U);
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(MakeFailure(AddressSpaceError::ShootdownFailed));
    }

    if (m_pendingUnmap)
    {
        return CompleteUnmapLocked();
    }
    const Foundation::Size recordIndex = m_seedMappingCount + m_pendingRuntimeSlot;
    ++m_runtimeMappingCount;
    m_pendingRuntimeSlot = MaximumRuntimeAddressSpaceMappings;
    m_shootdownState.StoreRelease(0U);
    m_state = AddressSpaceState::Ready;
    return ResultType::Success(AddressSpaceMappingReceipt{
        .MappingSequence = m_mappingSequence,
        .MappingCount = m_seedMappingCount + m_runtimeMappingCount,
        .VirtualStart = m_mappings[recordIndex].VirtualStart,
        .Pages = m_mappings[recordIndex].Pages,
    });
}

Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure>
AddressSpace::CompleteFaultMapping(const AddressSpaceMappingToken token) noexcept
{
    return CompleteMapping(token);
}

AddressSpaceError AddressSpace::FailShootdown(const AddressSpaceMappingToken token) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return AddressSpaceError::NotInitialized;
    }
    if (m_state == AddressSpaceState::Faulted)
    {
        return AddressSpaceError::Faulted;
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return AddressSpaceError::Quiesced;
    }
    if (m_state != AddressSpaceState::ShootdownPending)
    {
        return AddressSpaceError::NotReady;
    }
    if (!token.IsValid() || (token.m_addressSpace != this) || (token.GetSequence() != m_mappingSequence))
    {
        return AddressSpaceError::InvalidShootdownToken;
    }

    m_shootdownState.StoreRelease(0U);
    m_state = AddressSpaceState::Faulted;
    return AddressSpaceError::None;
}

AddressSpaceError AddressSpace::Quiesce() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return AddressSpaceError::NotInitialized;
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return AddressSpaceError::Quiesced;
    }
    if (m_state == AddressSpaceState::ShootdownPending)
    {
        return AddressSpaceError::ShootdownPending;
    }
    if (m_state == AddressSpaceState::ActivationPending)
    {
        return AddressSpaceError::ActivationPending;
    }
    if ((m_transitionMask != 0U) && (m_residentProcessorMask != 0U))
    {
        return AddressSpaceError::ProcessorTransitionPending;
    }
    for (const auto use : m_mappingUses)
    {
        if (use != 0U)
        {
            return AddressSpaceError::UserMappingInUse;
        }
    }
    if (m_residentProcessorMask != 0U)
    {
        return AddressSpaceError::ProcessorsStillResident;
    }
    if ((m_pageTable != nullptr) &&
        (m_pageTable->Release(*this) != Hal::Memory::AddressSpacePageTableOwnershipError::None))
    {
        return AddressSpaceError::PageTableProtocolViolation;
    }

    for (Foundation::Size slot{0U}; slot < MaximumRuntimeAddressSpaceMappings; ++slot)
    {
        ReleaseRuntimeSlotLocked(slot);
    }
    for (AddressSpaceMappingRecord& mapping : m_mappings)
    {
        mapping = AddressSpaceMappingRecord{};
    }
    m_registry = nullptr;
    m_pageTable = nullptr;
    m_pageTableMapResult = Hal::Memory::AddressSpacePageTableMapResult{};
    m_pendingMappingSequence = 0U;
    m_identity = AddressSpaceId{};
    m_seedMappingCount = 0U;
    m_runtimeMappingCount = 0U;
    m_pendingRuntimeSlot = MaximumRuntimeAddressSpaceMappings;
    m_pendingUnmap = false;
    m_activationSequence = 0U;
    m_transitionSequence = 0U;
    m_reservedTransitionSequence = 0U;
    for (auto& reservation : m_transitionReservations)
    {
        reservation = 0U;
    }
    m_transitionMask = 0U;
    m_activatingProcessor = Scheduler::CpuId{};
    m_transitionProcessor = Scheduler::CpuId{};
    m_shootdownState.StoreRelease(0U);
    m_state = AddressSpaceState::Quiesced;
    return AddressSpaceError::None;
}

AddressSpaceId AddressSpace::GetIdentity() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_identity;
}
AddressSpaceState AddressSpace::GetState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state;
}
Foundation::ProcessorMask AddressSpace::GetShootdownTargets() const noexcept
{
    return ShootdownTargets(m_shootdownState.LoadAcquire());
}

AddressSpaceSnapshot AddressSpace::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const Foundation::UInt64 shootdownState = m_shootdownState.LoadAcquire();
    return AddressSpaceSnapshot{
        .Identity = m_identity,
        .MappingCount = m_seedMappingCount + m_runtimeMappingCount,
        .ShootdownSequence = ShootdownSequence(shootdownState),
        .TransitionSequence = m_reservedTransitionSequence,
        .State = m_state,
        .ResidentProcessorMask = m_residentProcessorMask,
        .ActivatingProcessor = m_activatingProcessor,
        .TransitionProcessor = m_transitionProcessor,
        .TargetProcessorMask = ShootdownTargets(shootdownState),
        .AcknowledgedProcessorMask = ShootdownAcknowledgements(shootdownState),
        .Reserved = {},
    };
}

Foundation::Result<AddressSpaceMappingRecord, AddressSpaceError>
AddressSpace::TryGetMapping(const Foundation::Size index) const noexcept
{
    using ResultType = Foundation::Result<AddressSpaceMappingRecord, AddressSpaceError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpaceState::Uninitialized)
    {
        return ResultType::Failure(AddressSpaceError::NotInitialized);
    }
    if (m_state == AddressSpaceState::Quiesced)
    {
        return ResultType::Failure(AddressSpaceError::Quiesced);
    }
    // A runtime slot holds a mapping once its transaction completed.
    if ((index >= (m_seedMappingCount + MaximumRuntimeAddressSpaceMappings)) ||
        (m_mappings[index].Pages.GetValue() == 0U) || (index == (m_seedMappingCount + m_pendingRuntimeSlot)))
    {
        return ResultType::Failure(AddressSpaceError::InvalidMappingPlan);
    }
    return ResultType::Success(m_mappings[index]);
}

Foundation::Result<AddressSpaceMappingRecord, AddressSpaceError>
AddressSpace::FindUserMapping(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                              const bool write) const noexcept
{
    using ResultType = Foundation::Result<AddressSpaceMappingRecord, AddressSpaceError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != AddressSpaceState::Ready) && (m_state != AddressSpaceState::Unbound))
    {
        return ResultType::Failure(AddressSpaceError::NotReady);
    }
    if ((sizeBytes == 0U) || (start > (~Foundation::UInt64{0U} - sizeBytes)))
    {
        return ResultType::Failure(AddressSpaceError::InvalidConfiguration);
    }
    for (Foundation::Size slot{0U}; slot < MaximumRuntimeAddressSpaceMappings; ++slot)
    {
        const AddressSpaceMappingRecord& mapping = m_mappings[m_seedMappingCount + slot];
        if ((slot == m_pendingRuntimeSlot) || (mapping.Pages.GetValue() == 0U) || !mapping.RuntimeMapping)
        {
            continue;
        }
        const Foundation::UInt64 mappingStart = mapping.VirtualStart.GetValue();
        const Foundation::UInt64 mappingEnd = mappingStart + (mapping.Pages.GetValue() * MappingPageSizeBytes);
        if ((start < mappingStart) || ((start + sizeBytes) > mappingEnd))
        {
            continue;
        }
        if (!mapping.Permissions.Contains(PagePermission::User) ||
            !mapping.Permissions.Contains(PagePermission::Read) ||
            (write && !mapping.Permissions.Contains(PagePermission::Write)) || (mapping.Kind != FrameKind::Ram))
        {
            return ResultType::Failure(AddressSpaceError::ObjectNotFound);
        }
        return ResultType::Success(mapping);
    }
    return ResultType::Failure(AddressSpaceError::ObjectNotFound);
}

namespace
{
template <typename Begin>
[[nodiscard]] Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure> WaitForMapping(AddressSpace& space,
                                                                                                   Begin begin) noexcept
{
    MappingIntentGuard intent{space};
    auto result = begin();
    for (Scheduler::CrossProcessorWait wait;
         !result.HasValue() && (result.GetError().Error == AddressSpaceError::ProcessorTransitionPending ||
                                result.GetError().Error == AddressSpaceError::UserMappingInUse);)
    {
        if (!wait.Poll())
        {
            Scheduler::CrossProcessorWait::Timeout(space.GetSnapshot().ResidentProcessorMask);
        }
        result = begin();
    }
    return result;
}
} // Namespace
Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
AddressSpace::WaitForFaultMapping(const FaultFrameMappingPreparation& preparation) noexcept
{
    return WaitForMapping(*this,
                          [&]() noexcept
                          {
                              return BeginFaultMapping(preparation);
                          });
}
Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
AddressSpace::WaitForPinnedFrameMapping(const AddressSpaceId target,
                                        const PinnedFrameMappingPreparation& preparation) noexcept
{
    return WaitForMapping(*this,
                          [&]() noexcept
                          {
                              return BeginPinnedFrameMapping(target, preparation);
                          });
}
Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
AddressSpace::WaitForUnmap(const VirtualAddress start, const PageCount pages) noexcept
{
    return WaitForMapping(*this,
                          [&]() noexcept
                          {
                              return BeginUnmap(start, pages);
                          });
}

void AddressSpace::BeginMappingIntent() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    ++m_mappingIntents;
}

void AddressSpace::EndMappingIntent() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    --m_mappingIntents;
}

Foundation::Result<UserMappingUse, AddressSpaceError>
AddressSpace::PinUserMapping(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes, const bool write,
                             const bool includeSeed) const noexcept
{
    using ResultType = Foundation::Result<UserMappingUse, AddressSpaceError>;
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_mappingIntents != 0U || ((m_state != AddressSpaceState::Ready) && (m_state != AddressSpaceState::Unbound)))
    {
        return ResultType::Failure(AddressSpaceError::NotReady);
    }
    if (sizeBytes == 0U)
    {
        return ResultType::Success(UserMappingUse{});
    }
    if (sizeBytes > MappingPageSizeBytes || start > (~Foundation::UInt64{0U} - sizeBytes))
    {
        return ResultType::Failure(AddressSpaceError::InvalidConfiguration);
    }
    UserMappingUse use{};
    Foundation::UInt64 cursor = start;
    const Foundation::UInt64 end = start + sizeBytes;
    for (Foundation::Size part{0U}; part < sizeof(use.Slots) / sizeof(use.Slots[0U]) && cursor < end; ++part)
    {
        bool found{false};
        for (Foundation::Size slot = includeSeed ? 0U : m_seedMappingCount;
             slot < m_seedMappingCount + MaximumRuntimeAddressSpaceMappings; ++slot)
        {
            const auto& mapping = m_mappings[slot];
            const auto limit = mapping.VirtualStart.GetValue() + mapping.Pages.GetValue() * MappingPageSizeBytes;
            if (mapping.Pages.GetValue() == 0U || slot == m_seedMappingCount + m_pendingRuntimeSlot ||
                cursor < mapping.VirtualStart.GetValue() || cursor >= limit)
            {
                continue;
            }
            if (!mapping.Permissions.Contains(PagePermission::User) ||
                !mapping.Permissions.Contains(PagePermission::Read) ||
                (write && !mapping.Permissions.Contains(PagePermission::Write)) || mapping.Kind != FrameKind::Ram ||
                m_mappingUses[slot] == ~Foundation::UInt32{0U})
            {
                break;
            }
            use.Slots[use.Count++] = slot;
            cursor = limit < end ? limit : end;
            found = true;
            break;
        }
        if (!found)
        {
            return ResultType::Failure(AddressSpaceError::ObjectNotFound);
        }
    }
    if (cursor != end)
    {
        return ResultType::Failure(AddressSpaceError::ObjectNotFound);
    }
    for (Foundation::Size part{0U}; part < use.Count; ++part)
    {
        ++m_mappingUses[use.Slots[part]];
    }
    return ResultType::Success(use);
}

void AddressSpace::ReleaseUserMapping(const UserMappingUse use) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (Foundation::Size part{0U}; part < use.Count; ++part)
    {
        --m_mappingUses[use.Slots[part]];
    }
}

void AddressSpace::ReleaseRuntimeSlotLocked(const Foundation::Size slot) noexcept
{
    Object::ObjectPin& memoryObjectPin = m_runtimeMemoryObjectPins[slot];
    Object::ObjectPin& deviceRegionPin = m_runtimeDeviceRegionPins[slot];
    Object::ObjectPin& framePin = m_runtimeFramePins[slot];
    AddressSpaceMappingRecord& record = m_mappings[m_seedMappingCount + slot];
    if (framePin.IsValid() && record.RuntimeMapping)
    {
        static_cast<Frame&>(framePin.GetObject())
            .ReleaseMappingPolicy(FrameMappingKey{}, *m_registry, record.Frame.GetObjectId(), record.PhysicalStart,
                                  record.Pages, record.Permissions);
    }
    framePin.Reset();
    ReleaseDeviceRegionMapping(deviceRegionPin);
    if (memoryObjectPin.IsValid())
    {
        static_cast<void>(static_cast<MemoryObject&>(memoryObjectPin.GetObject()).ReleaseMapping());
    }
    memoryObjectPin.Reset();
    record = AddressSpaceMappingRecord{};
}

Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>
AddressSpace::BeginUnmap(const VirtualAddress virtualStart, const PageCount pages) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceShootdownRequest, AddressSpaceFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const AddressSpaceError stateError = ValidateBeginMappingStateLocked();
    if (stateError != AddressSpaceError::None)
    {
        return ResultType::Failure(MakeFailure(stateError));
    }
    Foundation::Size slot{MaximumRuntimeAddressSpaceMappings};
    for (Foundation::Size candidate{0U};
         (candidate < MaximumRuntimeAddressSpaceMappings) && (slot == MaximumRuntimeAddressSpaceMappings); ++candidate)
    {
        const AddressSpaceMappingRecord& record = m_mappings[m_seedMappingCount + candidate];
        if (record.RuntimeMapping && (record.Pages.GetValue() != 0U) &&
            (record.VirtualStart.GetValue() == virtualStart.GetValue()) &&
            (record.Pages.GetValue() == pages.GetValue()))
        {
            slot = candidate;
        }
    }
    if (slot == MaximumRuntimeAddressSpaceMappings)
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::MappingNotFound));
    }
    if (m_mappingUses[m_seedMappingCount + slot] != 0U)
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::UserMappingInUse));
    }
    if (!AddressSpaceMappingSequencePolicy::TryAdvance(m_mappingSequence, m_pendingMappingSequence))
    {
        return ResultType::Failure(MakeFailure(AddressSpaceError::MappingSequenceExhausted));
    }

    const AddressSpaceMappingRecord& record = m_mappings[m_seedMappingCount + slot];
    m_pageTableMapResult = Hal::Memory::AddressSpacePageTableMapResult{};
    const Hal::Memory::AddressSpacePageTableError pageTableError = m_pageTable->Unmap(record, m_pageTableMapResult);
    if (pageTableError != Hal::Memory::AddressSpacePageTableError::None)
    {
        if (!IsKnownPageTableError(pageTableError) ||
            (pageTableError == Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown))
        {
            m_mappingSequence = m_pendingMappingSequence;
            m_pendingRuntimeSlot = slot;
            m_pendingUnmap = true;
            m_shootdownState.StoreRelease(0U);
            m_state = AddressSpaceState::Faulted;
            return ResultType::Failure(
                MakeFailure(AddressSpaceError::PageTableProtocolViolation, Object::ObjectPinError::Ok,
                            IsKnownPageTableError(pageTableError)
                                ? pageTableError
                                : Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown));
        }
        if ((pageTableError == Hal::Memory::AddressSpacePageTableError::CorruptState) ||
            (pageTableError == Hal::Memory::AddressSpacePageTableError::InternalFailure) ||
            (pageTableError == Hal::Memory::AddressSpacePageTableError::NotReady))
        {
            m_state = AddressSpaceState::Faulted;
        }
        return ResultType::Failure(
            MakeFailure(AddressSpaceError::PageTableRejected, Object::ObjectPinError::Ok, pageTableError));
    }
    m_mappingSequence = m_pendingMappingSequence;
    m_pendingRuntimeSlot = slot;
    m_pendingUnmap = true;
    if ((m_pageTableMapResult.InvalidationStart.GetValue() != record.VirtualStart.GetValue()) ||
        (m_pageTableMapResult.InvalidationPages.GetValue() != record.Pages.GetValue()) ||
        (m_pageTableMapResult.Reserved != 0U))
    {
        m_state = AddressSpaceState::Faulted;
        return ResultType::Failure(MakeFailure(AddressSpaceError::PageTableProtocolViolation));
    }
    m_shootdownState.StoreRelease(PackShootdownState(m_pendingMappingSequence, m_residentProcessorMask, 0U));
    m_state = AddressSpaceState::ShootdownPending;
    return ResultType::Success(AddressSpaceShootdownRequest{AddressSpaceMappingToken{*this, m_pendingMappingSequence},
                                                            m_pageTableMapResult.InvalidationStart,
                                                            m_pageTableMapResult.InvalidationPages});
}

// Every resident processor acknowledged the invalidation, so nothing can
// still reach the pages through this root: the objects are released.
Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure> AddressSpace::CompleteUnmapLocked() noexcept
{
    using ResultType = Foundation::Result<AddressSpaceMappingReceipt, AddressSpaceFailure>;

    const AddressSpaceMappingRecord record = m_mappings[m_seedMappingCount + m_pendingRuntimeSlot];
    ReleaseRuntimeSlotLocked(m_pendingRuntimeSlot);
    --m_runtimeMappingCount;
    m_pendingRuntimeSlot = MaximumRuntimeAddressSpaceMappings;
    m_pendingUnmap = false;
    m_shootdownState.StoreRelease(0U);
    m_state = AddressSpaceState::Ready;
    return ResultType::Success(AddressSpaceMappingReceipt{
        .MappingSequence = m_mappingSequence,
        .MappingCount = m_seedMappingCount + m_runtimeMappingCount,
        .VirtualStart = record.VirtualStart,
        .Pages = record.Pages,
    });
}

Foundation::Size AddressSpace::FindFreeRuntimeSlotLocked() const noexcept
{
    for (Foundation::Size slot{0U}; slot < MaximumRuntimeAddressSpaceMappings; ++slot)
    {
        if (m_mappings[m_seedMappingCount + slot].Pages.GetValue() == 0U)
        {
            return slot;
        }
    }
    return MaximumRuntimeAddressSpaceMappings;
}

Object::ObjectType AddressSpace::GetObjectType() const noexcept
{
    return Object::ObjectType::AddressSpace;
}

Object::ResourceDomainId AddressSpace::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

AddressSpaceMappingRecord AddressSpace::ConvertFaultMapping(const FaultFrameMappingPlan& plan) noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = plan.MemoryObject,
        .DeviceRegion = plan.DeviceRegion,
        .Frame = plan.Frame,
        .VirtualStart = plan.VirtualStart,
        .PhysicalStart = plan.PhysicalStart,
        .Pages = plan.Pages,
        .MemoryObjectOffsetPages = plan.MemoryObjectOffsetPages,
        .Permissions = plan.Permissions,
        .CachePolicy = plan.CachePolicy,
        .Kind = plan.Kind,
        .Commitment = plan.Commitment,
        .RuntimeMapping = true,
        .Reserved = {},
    };
}

AddressSpaceMappingRecord AddressSpace::ConvertSeedMapping(const InitialMapping& mapping) noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = MemoryObjectId{},
        .DeviceRegion = Device::DeviceRegionId{},
        .Frame = FrameId{},
        .VirtualStart = mapping.VirtualStart,
        .PhysicalStart = mapping.PhysicalStart,
        .Pages = mapping.Pages,
        .MemoryObjectOffsetPages = 0U,
        .Permissions = ConvertPermissions(mapping.Permissions),
        .CachePolicy = mapping.CachePolicy,
        .Kind = (mapping.Owner == InitialMappingOwner::Device) ? FrameKind::Device : FrameKind::Ram,
        .Commitment = static_cast<MemoryCommitment>(0U),
        .RuntimeMapping = false,
        .Reserved = {},
    };
}

Foundation::UInt64 AddressSpace::PackShootdownState(const Foundation::UInt64 sequence,
                                                    const Foundation::ProcessorMask targetMask,
                                                    const Foundation::ProcessorMask acknowledgedMask) noexcept
{
    return (sequence << ShootdownSequenceShift) |
           (static_cast<Foundation::UInt64>(targetMask) << ShootdownTargetShift) |
           static_cast<Foundation::UInt64>(acknowledgedMask);
}
} // Namespace Asteria::Kernel::Memory
