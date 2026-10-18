// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"

#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"

#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size sizeBytes, void* storage) noexcept;
#else
#include <new>
#endif

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageOffsetMask{Kernel::Memory::MappingPageSizeBytes - 1U};
constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};

using PreparationResult = Foundation::Result<Ia64BspRuntimeMemorySummary, Ia64BspRuntimeMemoryError>;
using RootBorrowResult = Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError>;
using ApplicationProcessorAdmissionResult = Foundation::Result<Ia64BspRuntimeApplicationProcessorAdmissionSummary,
                                                               Ia64BspRuntimeApplicationProcessorAdmissionError>;
using ApplicationProcessorAdmissionSnapshotResult =
    Foundation::Result<Ia64BspRuntimeApplicationProcessorAdmissionSnapshot,
                       Ia64BspRuntimeApplicationProcessorAdmissionError>;
using ApplicationProcessorRootBorrowResult =
    Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeApplicationProcessorAdmissionError>;
using ExternalInterruptPublicationSnapshotResult =
    Foundation::Result<Ia64BspRuntimeExternalInterruptPublicationSnapshot, Ia64BspRuntimeExternalInterruptAccessError>;
using PublishedExternalInterruptEvidenceResult =
    Foundation::Result<const Interrupt::Ia64LocalTimerInterruptEvidence*, Ia64BspRuntimeExternalInterruptAccessError>;
using ExternalPreemptionStorageResult =
    Foundation::Result<Ia64BspRuntimeExternalPreemptionStorageView, Ia64BspRuntimeExternalPreemptionError>;
using PageFaultEntryStorageResult =
    Foundation::Result<Ia64BspRuntimePageFaultEntryStorageView, Ia64BspRuntimePageFaultEntryError>;

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr bool TryCalculateEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                             Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool IsRangeInside(const Ia64RuntimeObjectRange corridor, const Foundation::UInt64 rangeStart,
                                           const Foundation::UInt64 sizeBytes) noexcept
{
    Foundation::UInt64 corridorEnd{0U};
    Foundation::UInt64 rangeEnd{0U};
    return TryCalculateEnd(corridor.VirtualStart, corridor.SizeBytes, corridorEnd) &&
           TryCalculateEnd(rangeStart, sizeBytes, rangeEnd) && (rangeStart >= corridor.VirtualStart) &&
           (rangeEnd <= corridorEnd);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool PageFaultEntryContextsAreEqual(const Interrupt::Ia64PageFaultEntryContext& left,
                                                            const Interrupt::Ia64PageFaultEntryContext& right) noexcept
{
    return (left.ControlAddress == right.ControlAddress) && (left.ProcessorIndex == right.ProcessorIndex) &&
           (left.PageNotPresentVectorOffset == right.PageNotPresentVectorOffset) &&
           (left.KeyPermissionVectorOffset == right.KeyPermissionVectorOffset) &&
           (left.InstructionAccessRightsVectorOffset == right.InstructionAccessRightsVectorOffset) &&
           (left.DataAccessRightsVectorOffset == right.DataAccessRightsVectorOffset) &&
           (left.Reserved[0U] == right.Reserved[0U]) && (left.Reserved[1U] == right.Reserved[1U]);
}

[[nodiscard]] constexpr bool
PageFaultControlConfigurationsAreEqual(const Interrupt::Ia64PageFaultEntryControlConfiguration& left,
                                       const Interrupt::Ia64PageFaultEntryControlConfiguration& right) noexcept
{
    return (left.ProcessorIndex == right.ProcessorIndex) && (left.FrameAddress == right.FrameAddress) &&
           (left.InterruptedRseBackingStoreStart == right.InterruptedRseBackingStoreStart) &&
           (left.InterruptedRseBackingStoreEnd == right.InterruptedRseBackingStoreEnd) &&
           (left.CoordinatorMemoryStackStart == right.CoordinatorMemoryStackStart) &&
           (left.CoordinatorMemoryStackEnd == right.CoordinatorMemoryStackEnd) &&
           (left.CoordinatorRseBackingStoreStart == right.CoordinatorRseBackingStoreStart) &&
           (left.CoordinatorRseBackingStoreEnd == right.CoordinatorRseBackingStoreEnd) &&
           (left.CoordinatorEntryPoint == right.CoordinatorEntryPoint) &&
           (left.CoordinatorGlobalPointer == right.CoordinatorGlobalPointer) &&
           (left.CoordinatorArgument == right.CoordinatorArgument);
}

// Keep the second acquire snapshot out of the publication transaction's
// stack frame so its before/after comparison fits the 512-byte target budget.
[[nodiscard, gnu::noinline]] bool
PageFaultControlStillConfigured(const Interrupt::Ia64PageFaultEntryControl& control,
                                const Interrupt::Ia64PageFaultEntryControlConfiguration& expected) noexcept
{
    const Interrupt::Ia64PageFaultEntryControlSnapshot snapshot =
        Interrupt::AcquireIa64PageFaultEntryControlSnapshot(control);
    return (snapshot.State == Interrupt::Ia64PageFaultEntryControlState::Configured) &&
           PageFaultControlConfigurationsAreEqual(snapshot.Configuration, expected);
}

[[nodiscard]] bool IsValidRuntimeObjectRange(const Ia64RuntimeObjectRange range) noexcept
{
    return (range.VirtualStart & PageOffsetMask) == 0U && (range.SizeBytes != 0U) &&
           ((range.SizeBytes & PageOffsetMask) == 0U) && (range.VirtualStart < RegionZeroLimit) &&
           (range.SizeBytes <= (RegionZeroLimit - range.VirtualStart));
}

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Kernel::Scheduler::CpuId processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor.GetValue());
}

[[nodiscard]] bool TranslationEntryCovers(const InitialTranslationResourceEntry& entry, const Foundation::UInt64 cursor,
                                          const bool instruction, Foundation::UInt64& coveredEnd) noexcept
{
    if ((entry.PageShift < 12U) || (entry.PageShift > 60U) || (instruction && !entry.HasInstructionRegister))
    {
        return false;
    }
    const Foundation::UInt64 entryStart = entry.Translation.FaultingAddress;
    const Foundation::UInt64 entrySize = Foundation::UInt64{1U} << entry.PageShift;
    Foundation::UInt64 entryEnd{0U};
    if (!TryCalculateEnd(entryStart, entrySize, entryEnd) || (cursor < entryStart) || (cursor >= entryEnd) ||
        (entry.Translation.TranslationInsertion != (static_cast<Foundation::UInt64>(entry.PageShift) << 2U)) ||
        ((entry.Translation.TranslationWord & PhysicalPageNumberMask) != entryStart))
    {
        return false;
    }
    coveredEnd = entryEnd;
    return true;
}

[[nodiscard]] bool HasTranslationRegisterCoverage(const InitialTranslationResourcePlan& plan,
                                                  const Foundation::UInt64 rangeStart,
                                                  const Foundation::UInt64 rangeEnd, const bool instruction) noexcept
{
    Foundation::UInt64 cursor = rangeStart;
    while (cursor < rangeEnd)
    {
        bool found{false};
        Foundation::UInt64 coveredEnd{cursor};
        for (Foundation::Size index{0U}; index < plan.GetEntryCount(); ++index)
        {
            const auto entryResult = plan.GetEntry(index);
            if (!entryResult.HasValue())
            {
                return false;
            }
            Foundation::UInt64 entryEnd{0U};
            if (TranslationEntryCovers(entryResult.GetValue(), cursor, instruction, entryEnd))
            {
                found = true;
                coveredEnd = entryEnd < rangeEnd ? entryEnd : rangeEnd;
                break;
            }
        }
        if (!found || (coveredEnd <= cursor))
        {
            return false;
        }
        cursor = coveredEnd;
    }
    return cursor == rangeEnd;
}

[[nodiscard]] constexpr Foundation::Size
ApplicationProcessorControllerIndex(const Kernel::Scheduler::CpuId bspProcessor,
                                    const Kernel::Scheduler::CpuId processor) noexcept
{
    if (!bspProcessor.IsValid() || !processor.IsValid() || (bspProcessor.GetValue() == processor.GetValue()))
    {
        return Ia64BspRuntimeMaximumApplicationProcessorCount;
    }

    Foundation::Size controllerIndex{0U};
    for (Foundation::UInt8 candidate{0U}; candidate < processor.GetValue(); ++candidate)
    {
        if (candidate != bspProcessor.GetValue())
        {
            ++controllerIndex;
        }
    }
    return controllerIndex;
}

template <Foundation::Size Size>
[[nodiscard]] constexpr bool ReservedBytesAreZero(const Foundation::UInt8 (&reserved)[Size]) noexcept
{
    for (Foundation::Size index{0U}; index < Size; ++index)
    {
        if (reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Ia64BspRuntimeBindingState ClassifyBinding(const Ia64ActiveRuntimeTranslationBindings& bindings,
                                                         const Kernel::Scheduler::CpuId processor) noexcept
{
    const auto active = bindings.AcquireActiveLedger(processor);
    if (active.HasValue())
    {
        return active.GetValue() != nullptr ? Ia64BspRuntimeBindingState::Active : Ia64BspRuntimeBindingState::Invalid;
    }
    switch (active.GetError())
    {
    case Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted:
        return Ia64BspRuntimeBindingState::Unadmitted;
    case Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger:
        return Ia64BspRuntimeBindingState::AdmittedWithoutLedger;
    case Ia64ActiveRuntimeTranslationBindingError::None:
    case Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor:
    case Ia64ActiveRuntimeTranslationBindingError::ProcessorAlreadyAdmitted:
    case Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding:
    case Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeNotConfigured:
    case Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeMismatch:
    case Ia64ActiveRuntimeTranslationBindingError::AdmissionOwnerMismatch:
    case Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRuntimeObjectRange:
    case Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRuntimeObjectRange:
    case Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet:
    case Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit:
    case Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown:
    case Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch:
        return Ia64BspRuntimeBindingState::Invalid;
    }
    return Ia64BspRuntimeBindingState::Invalid;
}

[[nodiscard]] constexpr Ia64BspRuntimeExternalInterruptAcknowledgementError
MapExternalInterruptAcknowledgementError(const Interrupt::Ia64LocalTimerInterruptAcknowledgementError error) noexcept
{
    switch (error)
    {
    case Interrupt::Ia64LocalTimerInterruptAcknowledgementError::None:
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::None;
    case Interrupt::Ia64LocalTimerInterruptAcknowledgementError::InvalidSequence:
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::InvalidSequence;
    case Interrupt::Ia64LocalTimerInterruptAcknowledgementError::StaleSequence:
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::StaleSequence;
    case Interrupt::Ia64LocalTimerInterruptAcknowledgementError::ForeignSequence:
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::ForeignSequence;
    case Interrupt::Ia64LocalTimerInterruptAcknowledgementError::AlreadyAcknowledged:
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::AlreadyAcknowledged;
    case Interrupt::Ia64LocalTimerInterruptAcknowledgementError::SequenceExhausted:
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::SequenceExhausted;
    case Interrupt::Ia64LocalTimerInterruptAcknowledgementError::InvalidMailboxState:
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::InvalidMailboxState;
    }
    return Ia64BspRuntimeExternalInterruptAcknowledgementError::InvalidMailboxState;
}
} // namespace

Ia64BspRuntimeMemoryError Ia64BspRuntimeMemory::InitializeRuntimeBackend(
    const Kernel::Scheduler::CpuId bspProcessor, const Foundation::UInt8 profiledProcessorMask,
    const PalVirtualMemoryCapabilities (&processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount],
    const InitialAddressSpacePreparation& preparation) noexcept
{
    const Ia64RuntimeProcessorCapabilitiesError capabilitiesError =
        m_processorCapabilities.Initialize(profiledProcessorMask, processorCapabilities);
    if (capabilitiesError != Ia64RuntimeProcessorCapabilitiesError::None)
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::ProcessorCapabilitiesRejected;
        m_failure.ProcessorCapabilitiesError = capabilitiesError;
        return m_failure.Error;
    }

    const auto bspCapabilities = m_processorCapabilities.GetProcessorCapabilities(bspProcessor);
    if (!bspCapabilities.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::ProcessorCapabilitiesRejected;
        m_failure.ProcessorCapabilitiesError = bspCapabilities.GetError();
        return m_failure.Error;
    }

    const auto commonCapabilities = m_processorCapabilities.GetCommonCapabilities();
    if (!commonCapabilities.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::ProcessorCapabilitiesRejected;
        m_failure.ProcessorCapabilitiesError = commonCapabilities.GetError();
        return m_failure.Error;
    }
    m_summary.CommonCapabilities = commonCapabilities.GetValue();
    m_summary.BspProcessorIndex = bspProcessor.GetValue();
    m_summary.ProfiledProcessorMask = profiledProcessorMask;

    const Ia64RegionIdentifierAllocatorError allocatorError =
        m_regionIdentifierAllocator.Initialize(m_summary.CommonCapabilities.RegionIdentifierBits);
    if (allocatorError != Ia64RegionIdentifierAllocatorError::None)
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::RegionIdentifierAllocatorRejected;
        m_failure.RegionIdentifierAllocatorError = allocatorError;
        return m_failure.Error;
    }

    const auto receiptResult = m_regionIdentifierAllocator.Allocate();
    if (!receiptResult.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::RegionIdentifierAllocationRejected;
        m_failure.RegionIdentifierAllocatorError = receiptResult.GetError();
        return m_failure.Error;
    }
    m_regionIdentifierReceipt = receiptResult.GetValue();
    const auto firstRegionIdentifier = m_regionIdentifierReceipt.GetRegionIdentifier(0U);
    const auto lastRegionIdentifier =
        m_regionIdentifierReceipt.GetRegionIdentifier(Ia64RegionIdentifierCountPerAddressSpace - 1U);
    if (!firstRegionIdentifier.HasValue() || !lastRegionIdentifier.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::RegionIdentifierAllocationRejected;
        m_failure.RegionIdentifierAllocatorError =
            !firstRegionIdentifier.HasValue() ? firstRegionIdentifier.GetError() : lastRegionIdentifier.GetError();
        return m_failure.Error;
    }
    m_summary.FirstRegionIdentifier = firstRegionIdentifier.GetValue();
    m_summary.LastRegionIdentifier = lastRegionIdentifier.GetValue();

    const Ia64RuntimeAddressSpacePageTableInitializationError backendError = m_addressSpacePageTable.Initialize(
        m_processorCapabilities, m_regionIdentifierReceipt, preparation, m_activeBindings);
    if (backendError != Ia64RuntimeAddressSpacePageTableInitializationError::None)
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::AddressSpaceBackendRejected;
        m_failure.AddressSpaceBackendError = backendError;
        return m_failure.Error;
    }
    if (!m_addressSpacePageTable.IsReady())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::AddressSpaceBackendRejected;
        return m_failure.Error;
    }
    return Ia64BspRuntimeMemoryError::None;
}

bool Ia64BspRuntimeMemory::HasLiveAddressSpaceRegistryBinding() const noexcept
{
    // This point-in-time probe diagnoses a root quiesced before entry. The
    // class-level boot-lifetime contract externally excludes a concurrent
    // DestroyObject; the temporary pin is not a transaction lock.
    const Kernel::Memory::AddressSpaceId identity = m_summary.AddressSpaceIdentity;
    if ((m_registry == nullptr) || !identity.IsValid() || !identity.IsFromRegistry(*m_registry) ||
        !m_addressSpacePin.IsValid() ||
        (m_addressSpacePin.GetObjectId().GetValue() != identity.GetObjectId().GetValue()) ||
        (m_addressSpacePin.GetObjectType() != Kernel::Object::ObjectType::AddressSpace) ||
        (&m_addressSpacePin.GetObject() != &m_addressSpace))
    {
        return false;
    }

    Kernel::Object::ObjectPin verification;
    const Kernel::Object::ObjectPinError pinError =
        m_registry->PinObject(identity.GetObjectId(), Kernel::Object::ObjectType::AddressSpace, verification);
    return (pinError == Kernel::Object::ObjectPinError::Ok) && verification.IsValid() &&
           (verification.GetObjectId().GetValue() == identity.GetObjectId().GetValue()) &&
           (&verification.GetObject() == &m_addressSpace);
}

Ia64BspRuntimeMemoryError Ia64BspRuntimeMemory::VerifyPostconditions(
    const Kernel::Scheduler::CpuId bspProcessor, const Kernel::Memory::InitialMappingPlan& mappingPlan,
    const Kernel::Memory::AddressSpaceId identity,
    const Kernel::Memory::ProcessorAddressSpaceControllerReceipt& controllerReceipt) noexcept
{
    for (Foundation::Size index{0U}; index < Ia64RegionIdentifierCountPerAddressSpace; ++index)
    {
        const auto regionIdentifier = m_regionIdentifierReceipt.GetRegionIdentifier(index);
        if (!regionIdentifier.HasValue() ||
            (regionIdentifier.GetValue() != (m_summary.FirstRegionIdentifier + static_cast<Foundation::UInt32>(index))))
        {
            m_failure.Error = Ia64BspRuntimeMemoryError::PostconditionRejected;
            m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::RegionIdentifierReceiptMismatch;
            return m_failure.Error;
        }
    }

    const Ia64RegionIdentifierAllocatorSnapshot allocatorSnapshot = m_regionIdentifierAllocator.GetSnapshot();
    const Foundation::UInt32 expectedLimit =
        static_cast<Foundation::UInt32>(Foundation::UInt64{1U} << m_summary.CommonCapabilities.RegionIdentifierBits);
    if (!m_regionIdentifierReceipt.IsValid() ||
        (m_summary.LastRegionIdentifier !=
         (m_summary.FirstRegionIdentifier +
          static_cast<Foundation::UInt32>(Ia64RegionIdentifierCountPerAddressSpace - 1U))) ||
        (allocatorSnapshot.State != Ia64RegionIdentifierAllocatorState::Ready) ||
        (allocatorSnapshot.NextRegionIdentifier != (m_summary.LastRegionIdentifier + 1U)) ||
        (allocatorSnapshot.RepresentableLimitExclusive != expectedLimit) ||
        (allocatorSnapshot.AllocatedAddressSpaceCount != 1U) ||
        (allocatorSnapshot.RegionIdentifierBits != m_summary.CommonCapabilities.RegionIdentifierBits) ||
        !ReservedBytesAreZero(allocatorSnapshot.Reserved))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::PostconditionRejected;
        m_failure.PostconditionError =
            Ia64BspRuntimeMemoryPostconditionError::RegionIdentifierAllocatorSnapshotMismatch;
        return m_failure.Error;
    }

    if (!HasLiveAddressSpaceRegistryBinding())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::PostconditionRejected;
        m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::AddressSpaceRegistryBindingMismatch;
        return m_failure.Error;
    }

    const Kernel::Memory::AddressSpaceSnapshot addressSpaceSnapshot = m_addressSpace.GetSnapshot();
    m_summary.MappingCount = addressSpaceSnapshot.MappingCount;
    m_summary.ResidentProcessorMask = addressSpaceSnapshot.ResidentProcessorMask;
    if (!addressSpaceSnapshot.Identity.IsEqualTo(identity) ||
        (addressSpaceSnapshot.MappingCount != mappingPlan.GetMappingCount()) ||
        (addressSpaceSnapshot.ShootdownSequence != 0U) || (addressSpaceSnapshot.TransitionSequence != 0U) ||
        (addressSpaceSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        (addressSpaceSnapshot.ResidentProcessorMask != ProcessorBit(bspProcessor)) ||
        addressSpaceSnapshot.ActivatingProcessor.IsValid() || addressSpaceSnapshot.TransitionProcessor.IsValid() ||
        (addressSpaceSnapshot.TargetProcessorMask != 0U) || (addressSpaceSnapshot.AcknowledgedProcessorMask != 0U) ||
        !ReservedBytesAreZero(addressSpaceSnapshot.Reserved))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::PostconditionRejected;
        m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::AddressSpaceSnapshotMismatch;
        return m_failure.Error;
    }

    const auto profiledProcessorMask = m_processorCapabilities.GetAdmittedProcessorMask();
    if (!profiledProcessorMask.HasValue() || (profiledProcessorMask.GetValue() != m_summary.ProfiledProcessorMask) ||
        ((m_summary.ProfiledProcessorMask & ProcessorBit(bspProcessor)) == 0U) ||
        (m_summary.ResidentProcessorMask != ProcessorBit(bspProcessor)))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::PostconditionRejected;
        m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::ProcessorProfileMismatch;
        return m_failure.Error;
    }

    if (controllerReceipt.Previous.IsValid() || !controllerReceipt.Active.IsEqualTo(identity) ||
        !controllerReceipt.Processor.IsValid() || (controllerReceipt.Processor.GetValue() != bspProcessor.GetValue()) ||
        !controllerReceipt.NativeActivationPerformed || !ReservedBytesAreZero(controllerReceipt.Reserved))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::PostconditionRejected;
        m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::ProcessorControllerReceiptMismatch;
        return m_failure.Error;
    }

    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controllerSnapshot =
        m_addressSpaceController.GetSnapshot();
    m_summary.ControllerState = controllerSnapshot.State;
    if ((controllerSnapshot.IdleStorage != &m_addressSpace) || (controllerSnapshot.ActiveStorage != &m_addressSpace) ||
        (controllerSnapshot.PreviousStorage != nullptr) || !controllerSnapshot.Idle.IsEqualTo(identity) ||
        !controllerSnapshot.Active.IsEqualTo(identity) || controllerSnapshot.Previous.IsValid() ||
        controllerSnapshot.SourceTransition.IsValid() || controllerSnapshot.DestinationTransition.IsValid() ||
        !controllerSnapshot.Processor.IsValid() ||
        (controllerSnapshot.Processor.GetValue() != bspProcessor.GetValue()) ||
        (controllerSnapshot.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        !ReservedBytesAreZero(controllerSnapshot.Reserved))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::PostconditionRejected;
        m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::ProcessorControllerSnapshotMismatch;
        return m_failure.Error;
    }

    if (!m_addressSpacePageTable.IsActiveLedgerBound(bspProcessor))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::LedgerPostconditionRejected;
        m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::ActiveLedgerMismatch;
        return m_failure.Error;
    }
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const Kernel::Scheduler::CpuId candidate{processor};
        const Ia64BspRuntimeBindingState expected = processor == bspProcessor.GetValue()
                                                        ? Ia64BspRuntimeBindingState::Active
                                                        : Ia64BspRuntimeBindingState::Unadmitted;
        if (ClassifyBinding(m_activeBindings, candidate) != expected)
        {
            m_failure.Error = Ia64BspRuntimeMemoryError::LedgerPostconditionRejected;
            m_failure.PostconditionError = Ia64BspRuntimeMemoryPostconditionError::ActiveLedgerMismatch;
            return m_failure.Error;
        }
    }
    m_summary.ExactLedgerVerified = true;
    return Ia64BspRuntimeMemoryError::None;
}

Ia64BspRuntimeMemoryError Ia64BspRuntimeMemory::PrepareInitialComponentAddressSpace(
    const InitialAddressSpacePreparation& preparation, const Kernel::Object::ResourceDomainId chargeOwner,
    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    const auto prepared =
        m_initialComponentAddressSpaceOwner.Prepare(m_processorCapabilities, m_regionIdentifierAllocator, preparation,
                                                    m_activeBindings, chargeOwner, registry, issuer);
    if (!prepared.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::InitialComponentAddressSpaceRejected;
        return m_failure.Error;
    }

    const Ia64RuntimeAddressSpaceOwnerSummary& initialComponent = prepared.GetValue();
    const Ia64RuntimeAddressSpaceOwnerSnapshot initialComponentSnapshot =
        m_initialComponentAddressSpaceOwner.GetSnapshot();
    const Ia64RegionIdentifierAllocatorSnapshot allocatorAfter = m_regionIdentifierAllocator.GetSnapshot();
    if ((initialComponent.State != Ia64RuntimeAddressSpaceOwnerState::Ready) ||
        !initialComponent.AddressSpaceIdentity.IsValid() ||
        initialComponent.AddressSpaceIdentity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (initialComponent.MappingCount != m_summary.MappingCount) ||
        (initialComponent.FirstRegionIdentifier != (m_summary.LastRegionIdentifier + 1U)) ||
        (initialComponent.LastRegionIdentifier !=
         (initialComponent.FirstRegionIdentifier + Ia64RegionIdentifierCountPerAddressSpace - 1U)) ||
        (initialComponent.AllocatedAddressSpaceCount != 2U) ||
        (initialComponentSnapshot.AddressSpace.ResidentProcessorMask != 0U) ||
        (allocatorAfter.AllocatedAddressSpaceCount != 2U) ||
        (allocatorAfter.NextRegionIdentifier != (initialComponent.LastRegionIdentifier + 1U)))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::InitialComponentAddressSpaceRejected;
        return m_failure.Error;
    }
    return Ia64BspRuntimeMemoryError::None;
}

// The boot pager's private root: the region-identifier block after the
// initial component's, over the same seed as both other roots.
Ia64BspRuntimeMemoryError Ia64BspRuntimeMemory::PreparePagerAddressSpace(
    const InitialAddressSpacePreparation& preparation, const Kernel::Object::ResourceDomainId chargeOwner,
    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    const Ia64RuntimeAddressSpaceOwnerSnapshot initialComponent = m_initialComponentAddressSpaceOwner.GetSnapshot();
    const auto prepared =
        m_pagerAddressSpaceOwner.Prepare(m_processorCapabilities, m_regionIdentifierAllocator, preparation,
                                         m_activeBindings, chargeOwner, registry, issuer);
    if (!prepared.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::InitialComponentAddressSpaceRejected;
        return m_failure.Error;
    }
    const Ia64RuntimeAddressSpaceOwnerSummary& pager = prepared.GetValue();
    const Ia64RuntimeAddressSpaceOwnerSnapshot pagerSnapshot = m_pagerAddressSpaceOwner.GetSnapshot();
    const Ia64RegionIdentifierAllocatorSnapshot allocatorAfter = m_regionIdentifierAllocator.GetSnapshot();
    if ((pager.State != Ia64RuntimeAddressSpaceOwnerState::Ready) || !pager.AddressSpaceIdentity.IsValid() ||
        pager.AddressSpaceIdentity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        pager.AddressSpaceIdentity.IsEqualTo(initialComponent.Summary.AddressSpaceIdentity) ||
        (pager.MappingCount != m_summary.MappingCount) ||
        (pager.FirstRegionIdentifier != (initialComponent.Summary.LastRegionIdentifier + 1U)) ||
        (pager.LastRegionIdentifier != (pager.FirstRegionIdentifier + Ia64RegionIdentifierCountPerAddressSpace - 1U)) ||
        (pager.AllocatedAddressSpaceCount != 3U) || (pagerSnapshot.AddressSpace.ResidentProcessorMask != 0U) ||
        (allocatorAfter.AllocatedAddressSpaceCount != 3U) ||
        (allocatorAfter.NextRegionIdentifier != (pager.LastRegionIdentifier + 1U)))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::InitialComponentAddressSpaceRejected;
        return m_failure.Error;
    }
    return Ia64BspRuntimeMemoryError::None;
}

Ia64RuntimeAddressSpaceOwner& Ia64BspRuntimeMemory::UserRootOwner(const Ia64BspRuntimeUserDomain domain) noexcept
{
    return domain == Ia64BspRuntimeUserDomain::Pager ? m_pagerAddressSpaceOwner : m_initialComponentAddressSpaceOwner;
}

const Ia64RuntimeAddressSpaceOwner&
Ia64BspRuntimeMemory::UserRootOwner(const Ia64BspRuntimeUserDomain domain) const noexcept
{
    return domain == Ia64BspRuntimeUserDomain::Pager ? m_pagerAddressSpaceOwner : m_initialComponentAddressSpaceOwner;
}

Ia64BspRuntimeApplicationProcessorAdmissionError
Ia64BspRuntimeMemory::ValidateApplicationProcessorAdmission(const Kernel::Scheduler::CpuId processor,
                                                            const Foundation::Size controllerIndex) const noexcept
{
    if (!processor.IsValid())
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::InvalidProcessor;
    }

    const Kernel::Scheduler::CpuId bspProcessor{m_summary.BspProcessorIndex};
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        const Foundation::Size retainedControllerIndex = ApplicationProcessorControllerIndex(bspProcessor, processor);
        if ((retainedControllerIndex < Ia64BspRuntimeMaximumApplicationProcessorCount) &&
            (m_applicationProcessorAdmissionStates[retainedControllerIndex] ==
             Ia64BspRuntimeApplicationProcessorAdmissionState::Failed))
        {
            return Ia64BspRuntimeApplicationProcessorAdmissionError::PreviousAdmissionFailed;
        }
        return Ia64BspRuntimeApplicationProcessorAdmissionError::NotReady;
    }
    if (!bspProcessor.IsValid())
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::RootInvariantMismatch;
    }
    if (processor.GetValue() == bspProcessor.GetValue())
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::BspProcessor;
    }
    if ((m_summary.ProfiledProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::ProcessorNotProfiled;
    }
    if ((controllerIndex != ApplicationProcessorControllerIndex(bspProcessor, processor)) ||
        (controllerIndex >= Ia64BspRuntimeMaximumApplicationProcessorCount))
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::InvalidProcessor;
    }

    switch (m_applicationProcessorAdmissionStates[controllerIndex])
    {
    case Ia64BspRuntimeApplicationProcessorAdmissionState::Ready:
        return Ia64BspRuntimeApplicationProcessorAdmissionError::AlreadyAdmitted;
    case Ia64BspRuntimeApplicationProcessorAdmissionState::Admitting:
        return Ia64BspRuntimeApplicationProcessorAdmissionError::AdmissionInProgress;
    case Ia64BspRuntimeApplicationProcessorAdmissionState::Failed:
        return Ia64BspRuntimeApplicationProcessorAdmissionError::PreviousAdmissionFailed;
    case Ia64BspRuntimeApplicationProcessorAdmissionState::Unadmitted:
        break;
    }

    for (Foundation::UInt8 candidate{0U}; candidate < Kernel::Scheduler::MaximumProcessorCount; ++candidate)
    {
        const Kernel::Scheduler::CpuId candidateProcessor{candidate};
        if ((candidate == bspProcessor.GetValue()) ||
            ((m_summary.ProfiledProcessorMask & ProcessorBit(candidateProcessor)) == 0U))
        {
            continue;
        }
        const Foundation::Size candidateIndex = ApplicationProcessorControllerIndex(bspProcessor, candidateProcessor);
        if (m_applicationProcessorAdmissionStates[candidateIndex] !=
            Ia64BspRuntimeApplicationProcessorAdmissionState::Ready)
        {
            if (candidate != processor.GetValue())
            {
                return Ia64BspRuntimeApplicationProcessorAdmissionError::OutOfOrder;
            }
            break;
        }
    }

    if (m_preparation == nullptr)
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::RetainedPreparationMissing;
    }
    const Ia64RuntimeObjectRange expectedRuntimeObjects{
        .VirtualStart = AddressOf(this),
        .SizeBytes = sizeof(*this),
    };
    if (!IsValidRuntimeObjectRange(expectedRuntimeObjects) ||
        (m_summary.RuntimeObjects.VirtualStart != expectedRuntimeObjects.VirtualStart) ||
        (m_summary.RuntimeObjects.SizeBytes != expectedRuntimeObjects.SizeBytes))
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::RuntimeObjectRangeMismatch;
    }

    Foundation::ProcessorMask expectedResidentProcessorMask = ProcessorBit(bspProcessor);
    for (Foundation::UInt8 candidate{0U}; candidate < Kernel::Scheduler::MaximumProcessorCount; ++candidate)
    {
        if (candidate == bspProcessor.GetValue())
        {
            continue;
        }
        const Kernel::Scheduler::CpuId candidateProcessor{candidate};
        const Foundation::Size candidateIndex = ApplicationProcessorControllerIndex(bspProcessor, candidateProcessor);
        if (m_applicationProcessorAdmissionStates[candidateIndex] ==
            Ia64BspRuntimeApplicationProcessorAdmissionState::Ready)
        {
            expectedResidentProcessorMask =
                static_cast<Foundation::UInt8>(expectedResidentProcessorMask | ProcessorBit(candidateProcessor));
        }
    }

    const Kernel::Memory::AddressSpaceSnapshot addressSpaceSnapshot = m_addressSpace.GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot bspControllerSnapshot =
        m_addressSpaceController.GetSnapshot();
    if (!HasLiveAddressSpaceRegistryBinding() || !m_summary.AddressSpaceIdentity.IsValid() ||
        !addressSpaceSnapshot.Identity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (addressSpaceSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        (addressSpaceSnapshot.ResidentProcessorMask != expectedResidentProcessorMask) ||
        (addressSpaceSnapshot.TransitionSequence != 0U) || addressSpaceSnapshot.ActivatingProcessor.IsValid() ||
        addressSpaceSnapshot.TransitionProcessor.IsValid() || (addressSpaceSnapshot.TargetProcessorMask != 0U) ||
        (addressSpaceSnapshot.AcknowledgedProcessorMask != 0U) ||
        (m_summary.ResidentProcessorMask != expectedResidentProcessorMask) || !m_summary.ExactLedgerVerified ||
        (bspControllerSnapshot.ActiveStorage != &m_addressSpace) ||
        !bspControllerSnapshot.Active.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (bspControllerSnapshot.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready))
    {
        return Ia64BspRuntimeApplicationProcessorAdmissionError::RootInvariantMismatch;
    }

    for (Foundation::UInt8 candidate{0U}; candidate < Kernel::Scheduler::MaximumProcessorCount; ++candidate)
    {
        const Kernel::Scheduler::CpuId candidateProcessor{candidate};
        const Ia64BspRuntimeBindingState expectedBinding =
            ((expectedResidentProcessorMask & ProcessorBit(candidateProcessor)) != 0U)
                ? Ia64BspRuntimeBindingState::Active
                : Ia64BspRuntimeBindingState::Unadmitted;
        if (ClassifyBinding(m_activeBindings, candidateProcessor) != expectedBinding)
        {
            return Ia64BspRuntimeApplicationProcessorAdmissionError::RootInvariantMismatch;
        }
    }
    return Ia64BspRuntimeApplicationProcessorAdmissionError::None;
}

Ia64BspRuntimeApplicationProcessorAdmissionError Ia64BspRuntimeMemory::FailApplicationProcessorAdmission(
    const Foundation::Size controllerIndex, const Ia64BspRuntimeApplicationProcessorAdmissionError error,
    const Ia64RuntimeInterruptVectorTableInstallError interruptVectorTableError,
    const Hal::Memory::AddressSpacePageTableActivationError physicalBootstrapActivationError,
    const Kernel::Memory::ProcessorAddressSpaceControllerFailure& controllerFailure,
    const Ia64BspRuntimeApplicationProcessorPostconditionError postconditionError,
    const bool interruptVectorTableCommitted, const bool physicalRootMayBeActive) noexcept
{
    m_applicationProcessorAdmissionFailures[controllerIndex] = Ia64BspRuntimeApplicationProcessorAdmissionFailure{
        .Error = error,
        .InterruptVectorTableError = interruptVectorTableError,
        .PhysicalBootstrapActivationError = physicalBootstrapActivationError,
        .PostconditionError = postconditionError,
        .ControllerFailure = controllerFailure,
        .InterruptVectorTableCommitted = interruptVectorTableCommitted,
        .PhysicalRootMayBeActive = physicalRootMayBeActive,
        .Reserved = {},
    };
    m_applicationProcessorAdmissionStates[controllerIndex] = Ia64BspRuntimeApplicationProcessorAdmissionState::Failed;
    if (interruptVectorTableCommitted || physicalRootMayBeActive)
    {
        m_state = Ia64BspRuntimeMemoryState::Failed;
    }
    return error;
}

Ia64BspRuntimeApplicationProcessorAdmissionError Ia64BspRuntimeMemory::VerifyApplicationProcessorPostconditions(
    const Kernel::Scheduler::CpuId processor, const Foundation::Size controllerIndex,
    const Foundation::ProcessorMask expectedResidentProcessorMask, const Foundation::Size preAdmissionRootMappingCount,
    const Ia64RuntimeInterruptVectorTableInstallSummary& interruptVectorTable,
    const Kernel::Memory::ProcessorAddressSpaceControllerReceipt& controllerReceipt) noexcept
{
    const Kernel::Scheduler::CpuId bspProcessor{m_summary.BspProcessorIndex};
    Foundation::UInt8 expectedInstalledProcessorMask = ProcessorBit(bspProcessor);
    for (Foundation::UInt8 candidate{0U}; candidate < Kernel::Scheduler::MaximumProcessorCount; ++candidate)
    {
        if (candidate == bspProcessor.GetValue())
        {
            continue;
        }
        const Kernel::Scheduler::CpuId candidateProcessor{candidate};
        const Foundation::Size candidateIndex = ApplicationProcessorControllerIndex(bspProcessor, candidateProcessor);
        if ((candidateIndex == controllerIndex) || (m_applicationProcessorAdmissionStates[candidateIndex] ==
                                                    Ia64BspRuntimeApplicationProcessorAdmissionState::Ready))
        {
            expectedInstalledProcessorMask =
                static_cast<Foundation::UInt8>(expectedInstalledProcessorMask | ProcessorBit(candidateProcessor));
        }
    }

    if ((interruptVectorTable.TableAddress != m_summary.InterruptVectorTable.TableAddress) ||
        (interruptVectorTable.SizeBytes != m_summary.InterruptVectorTable.SizeBytes) ||
        (interruptVectorTable.ProcessorIndex != processor.GetValue()) ||
        (interruptVectorTable.InstalledProcessorMask != expectedInstalledProcessorMask) ||
        (interruptVectorTable.Reserved != 0U))
    {
        return FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::PostconditionRejected,
            Ia64RuntimeInterruptVectorTableInstallError::None, Hal::Memory::AddressSpacePageTableActivationError::None,
            Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
            Ia64BspRuntimeApplicationProcessorPostconditionError::InterruptVectorTableSummaryMismatch, true, true);
    }

    const Kernel::Memory::AddressSpaceSnapshot addressSpaceSnapshot = m_addressSpace.GetSnapshot();
    if (!addressSpaceSnapshot.Identity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (addressSpaceSnapshot.MappingCount != preAdmissionRootMappingCount) ||
        (addressSpaceSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        (addressSpaceSnapshot.ResidentProcessorMask != expectedResidentProcessorMask) ||
        (addressSpaceSnapshot.TransitionSequence != 0U) || addressSpaceSnapshot.ActivatingProcessor.IsValid() ||
        addressSpaceSnapshot.TransitionProcessor.IsValid() || (addressSpaceSnapshot.TargetProcessorMask != 0U) ||
        (addressSpaceSnapshot.AcknowledgedProcessorMask != 0U) || !ReservedBytesAreZero(addressSpaceSnapshot.Reserved))
    {
        return FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::PostconditionRejected,
            Ia64RuntimeInterruptVectorTableInstallError::None, Hal::Memory::AddressSpacePageTableActivationError::None,
            Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
            Ia64BspRuntimeApplicationProcessorPostconditionError::AddressSpaceSnapshotMismatch, true, true);
    }

    if (!m_addressSpacePageTable.IsActiveLedgerBound(processor) ||
        (ClassifyBinding(m_activeBindings, processor) != Ia64BspRuntimeBindingState::Active))
    {
        return FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::LedgerPostconditionRejected,
            Ia64RuntimeInterruptVectorTableInstallError::None, Hal::Memory::AddressSpacePageTableActivationError::None,
            Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
            Ia64BspRuntimeApplicationProcessorPostconditionError::BindingMismatch, true, true);
    }
    for (Foundation::UInt8 candidate{0U}; candidate < Kernel::Scheduler::MaximumProcessorCount; ++candidate)
    {
        const Kernel::Scheduler::CpuId candidateProcessor{candidate};
        const Ia64BspRuntimeBindingState expectedBinding =
            ((expectedResidentProcessorMask & ProcessorBit(candidateProcessor)) != 0U)
                ? Ia64BspRuntimeBindingState::Active
                : Ia64BspRuntimeBindingState::Unadmitted;
        if (ClassifyBinding(m_activeBindings, candidateProcessor) != expectedBinding)
        {
            return FailApplicationProcessorAdmission(
                controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::LedgerPostconditionRejected,
                Ia64RuntimeInterruptVectorTableInstallError::None,
                Hal::Memory::AddressSpacePageTableActivationError::None,
                Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
                Ia64BspRuntimeApplicationProcessorPostconditionError::BindingMismatch, true, true);
        }
    }

    if (controllerReceipt.Previous.IsValid() || !controllerReceipt.Active.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        !controllerReceipt.Processor.IsValid() || (controllerReceipt.Processor.GetValue() != processor.GetValue()) ||
        !controllerReceipt.NativeActivationPerformed || !ReservedBytesAreZero(controllerReceipt.Reserved))
    {
        return FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::PostconditionRejected,
            Ia64RuntimeInterruptVectorTableInstallError::None, Hal::Memory::AddressSpacePageTableActivationError::None,
            Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
            Ia64BspRuntimeApplicationProcessorPostconditionError::ControllerReceiptMismatch, true, true);
    }

    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controllerSnapshot =
        m_applicationProcessorControllers[controllerIndex].GetSnapshot();
    if ((controllerSnapshot.IdleStorage != &m_addressSpace) || (controllerSnapshot.ActiveStorage != &m_addressSpace) ||
        (controllerSnapshot.PreviousStorage != nullptr) ||
        !controllerSnapshot.Idle.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        !controllerSnapshot.Active.IsEqualTo(m_summary.AddressSpaceIdentity) || controllerSnapshot.Previous.IsValid() ||
        controllerSnapshot.SourceTransition.IsValid() || controllerSnapshot.DestinationTransition.IsValid() ||
        !controllerSnapshot.Processor.IsValid() || (controllerSnapshot.Processor.GetValue() != processor.GetValue()) ||
        (controllerSnapshot.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        !ReservedBytesAreZero(controllerSnapshot.Reserved))
    {
        return FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::PostconditionRejected,
            Ia64RuntimeInterruptVectorTableInstallError::None, Hal::Memory::AddressSpacePageTableActivationError::None,
            Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
            Ia64BspRuntimeApplicationProcessorPostconditionError::ControllerSnapshotMismatch, true, true);
    }

    m_applicationProcessorAdmissionSummaries[controllerIndex] = Ia64BspRuntimeApplicationProcessorAdmissionSummary{
        .InterruptVectorTable = interruptVectorTable,
        .AddressSpaceIdentity = m_summary.AddressSpaceIdentity,
        .ControllerReceipt = controllerReceipt,
        .Processor = processor,
        .ResidentProcessorMask = expectedResidentProcessorMask,
        .InstalledProcessorMask = expectedInstalledProcessorMask,
        .ControllerState = controllerSnapshot.State,
        .PhysicalBootstrapActivationPerformed = true,
        .ControllerAcknowledgementPerformed = true,
        .ExactLedgerVerified = true,
        .Reserved = 0U,
    };
    m_applicationProcessorAdmissionFailures[controllerIndex] = {};
    m_applicationProcessorAdmissionStates[controllerIndex] = Ia64BspRuntimeApplicationProcessorAdmissionState::Ready;
    m_summary.InterruptVectorTable = interruptVectorTable;
    m_summary.ResidentProcessorMask = expectedResidentProcessorMask;
    return Ia64BspRuntimeApplicationProcessorAdmissionError::None;
}

Foundation::Result<Ia64BspRuntimeMemorySummary, Ia64BspRuntimeMemoryError> Ia64BspRuntimeMemory::PrepareAndAdmit(
    const Kernel::Scheduler::CpuId bspProcessor, const Foundation::UInt8 profiledProcessorMask,
    const PalVirtualMemoryCapabilities (&processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount],
    const InitialAddressSpacePreparation& preparation, const Kernel::Object::ResourceDomainId chargeOwner,
    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    if (m_state == Ia64BspRuntimeMemoryState::Ready)
    {
        return PreparationResult::Failure(Ia64BspRuntimeMemoryError::AlreadyPrepared);
    }
    if (m_state == Ia64BspRuntimeMemoryState::Failed)
    {
        return PreparationResult::Failure(Ia64BspRuntimeMemoryError::PreviousPreparationFailed);
    }

    const Ia64RuntimeObjectRange runtimeObjects{
        .VirtualStart = AddressOf(this),
        .SizeBytes = sizeof(*this),
    };
    if (!IsValidRuntimeObjectRange(runtimeObjects))
    {
        return PreparationResult::Failure(Ia64BspRuntimeMemoryError::InvalidPlacement);
    }

    const auto installPreflight =
        m_interruptVectorTable.ValidateInstall(bspProcessor, preparation, m_activeBindings, runtimeObjects);
    if (!installPreflight.HasValue())
    {
        return PreparationResult::Failure(Ia64BspRuntimeMemoryError::InterruptVectorTablePreflightRejected);
    }

    m_state = Ia64BspRuntimeMemoryState::Failed;
    m_interruptVectorTable.ConfigureStopStorage(AddressOf(&m_stopStorage));
    ::new (static_cast<void*>(&m_failure)) Ia64BspRuntimeMemoryFailure{};
    ::new (static_cast<void*>(&m_summary)) Ia64BspRuntimeMemorySummary{};
    m_summary.RuntimeObjects = runtimeObjects;

    const Ia64BspRuntimeMemoryError backendPreparation =
        InitializeRuntimeBackend(bspProcessor, profiledProcessorMask, processorCapabilities, preparation);
    if (backendPreparation != Ia64BspRuntimeMemoryError::None)
    {
        return PreparationResult::Failure(backendPreparation);
    }

    const auto mappingPlanResult = preparation.TryGetMappingPlan();
    if (!mappingPlanResult.HasValue() || (mappingPlanResult.GetValue() == nullptr))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::MappingPlanRetrievalRejected;
        if (!mappingPlanResult.HasValue())
        {
            m_failure.MappingPlanError = mappingPlanResult.GetError();
        }
        return PreparationResult::Failure(m_failure.Error);
    }
    const Kernel::Memory::InitialMappingPlan& mappingPlan = *mappingPlanResult.GetValue();
    if (!mappingPlan.IsSealed())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::MappingPlanRetrievalRejected;
        return PreparationResult::Failure(m_failure.Error);
    }

    const Kernel::Memory::AddressSpaceError addressSpaceError =
        m_addressSpace.Initialize(chargeOwner, m_addressSpacePageTable, mappingPlan);
    if (addressSpaceError != Kernel::Memory::AddressSpaceError::None)
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::AddressSpaceInitializationRejected;
        m_failure.AddressSpaceInitializationError = addressSpaceError;
        return PreparationResult::Failure(m_failure.Error);
    }
    m_summary.BackendClaimed = true;
    m_summary.MappingCount = mappingPlan.GetMappingCount();

    const auto installed =
        m_interruptVectorTable.InstallAndAdmit(bspProcessor, preparation, m_activeBindings, runtimeObjects);
    if (!installed.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::InterruptVectorTableInstallRejected;
        m_failure.InterruptVectorTableError = installed.GetError();
        return PreparationResult::Failure(m_failure.Error);
    }
    m_summary.InterruptVectorTable = installed.GetValue();

    const auto registered = issuer.CreateObject(m_addressSpace);
    if (!registered.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::ObjectRegistrationRejected;
        m_failure.ObjectRegistryError = registered.GetError();
        return PreparationResult::Failure(m_failure.Error);
    }

    const Kernel::Memory::AddressSpaceId identity{registry, registered.GetValue()};
    m_registry = &registry;
    const Kernel::Object::ObjectPinError addressSpacePinError =
        registry.PinObject(identity.GetObjectId(), Kernel::Object::ObjectType::AddressSpace, m_addressSpacePin);
    if ((addressSpacePinError != Kernel::Object::ObjectPinError::Ok) || !m_addressSpacePin.IsValid() ||
        (&m_addressSpacePin.GetObject() != &m_addressSpace))
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::AddressSpaceIdentityBindingRejected;
        m_failure.AddressSpaceIdentityBindingFailure.Error =
            addressSpacePinError == Kernel::Object::ObjectPinError::Ok
                ? Kernel::Memory::AddressSpaceError::ObjectInstanceMismatch
                : Kernel::Memory::AddressSpaceError::ObjectPinUnavailable;
        m_failure.AddressSpaceIdentityBindingFailure.PinError = addressSpacePinError;
        return PreparationResult::Failure(m_failure.Error);
    }
    const Kernel::Memory::AddressSpaceFailure identityBinding = m_addressSpace.BindIdentity(registry, identity);
    if (identityBinding.Error != Kernel::Memory::AddressSpaceError::None)
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::AddressSpaceIdentityBindingRejected;
        m_failure.AddressSpaceIdentityBindingFailure = identityBinding;
        return PreparationResult::Failure(m_failure.Error);
    }
    m_summary.AddressSpaceIdentity = identity;

    const auto controllerActivation = m_addressSpaceController.Initialize(bspProcessor, m_addressSpace);
    if (!controllerActivation.HasValue())
    {
        m_failure.Error = Ia64BspRuntimeMemoryError::ProcessorControllerActivationRejected;
        m_failure.ControllerFailure = controllerActivation.GetError();
        return PreparationResult::Failure(m_failure.Error);
    }
    m_summary.ControllerReceipt = controllerActivation.GetValue();

    const Ia64BspRuntimeMemoryError postcondition =
        VerifyPostconditions(bspProcessor, mappingPlan, identity, controllerActivation.GetValue());
    if (postcondition != Ia64BspRuntimeMemoryError::None)
    {
        return PreparationResult::Failure(postcondition);
    }

    const Ia64BspRuntimeMemoryError initialComponentPreparation =
        PrepareInitialComponentAddressSpace(preparation, chargeOwner, registry, issuer);
    if (initialComponentPreparation != Ia64BspRuntimeMemoryError::None)
    {
        return PreparationResult::Failure(initialComponentPreparation);
    }
    const Ia64BspRuntimeMemoryError pagerPreparation =
        PreparePagerAddressSpace(preparation, chargeOwner, registry, issuer);
    if (pagerPreparation != Ia64BspRuntimeMemoryError::None)
    {
        return PreparationResult::Failure(pagerPreparation);
    }
    ::new (static_cast<void*>(&m_failure)) Ia64BspRuntimeMemoryFailure{};
    m_preparation = &preparation;
    m_state = Ia64BspRuntimeMemoryState::Ready;
    return PreparationResult::Success(m_summary);
}

bool Ia64BspRuntimeMemory::OtherUserRootIsActive(
    const Ia64BspRuntimeUserDomain domain,
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot& controller) const noexcept
{
    const auto other =
        UserRootOwner(domain == Ia64BspRuntimeUserDomain::Pager ? Ia64BspRuntimeUserDomain::InitialComponent
                                                                : Ia64BspRuntimeUserDomain::Pager)
            .GetSnapshot();
    return other.AddressSpaceStorage != nullptr && controller.ActiveStorage == other.AddressSpaceStorage &&
           controller.Active.IsEqualTo(other.Summary.AddressSpaceIdentity);
}

Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError>
Ia64BspRuntimeMemory::BorrowReadyUserRoot(const Ia64BspRuntimeUserDomain domain) noexcept
{
    using ResultType = Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError>;
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return ResultType::Failure(Ia64BspRuntimeRootBorrowError::NotReady);
    }
    const auto privateRoot = UserRootOwner(domain).BorrowReadyAddressSpace();
    if (!privateRoot.HasValue())
    {
        return ResultType::Failure(Ia64BspRuntimeRootBorrowError::InvariantMismatch);
    }

    const Kernel::Scheduler::CpuId processor{m_summary.BspProcessorIndex};
    if (!processor.IsValid())
    {
        return ResultType::Failure(Ia64BspRuntimeRootBorrowError::InvariantMismatch);
    }
    const Ia64RuntimeAddressSpaceOwnerView root = privateRoot.GetValue();
    const Kernel::Memory::AddressSpaceSnapshot addressSpace = root.AddressSpaceStorage->GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controller = m_addressSpaceController.GetSnapshot();
    const Foundation::UInt8 processorBit =
        static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor.GetValue());
    const bool commonActive = (controller.ActiveStorage == &m_addressSpace) &&
                              controller.Active.IsEqualTo(m_summary.AddressSpaceIdentity) &&
                              (addressSpace.ResidentProcessorMask == 0U);
    const bool privateActive = (controller.ActiveStorage == root.AddressSpaceStorage) &&
                               controller.Active.IsEqualTo(root.AddressSpaceIdentity) &&
                               (addressSpace.ResidentProcessorMask == processorBit);
    const bool otherActive = addressSpace.ResidentProcessorMask == 0U && OtherUserRootIsActive(domain, controller);
    if ((controller.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        !controller.Processor.IsValid() || (controller.Processor.GetValue() != processor.GetValue()) ||
        (controller.IdleStorage != &m_addressSpace) || !controller.Idle.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (addressSpace.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !addressSpace.Identity.IsEqualTo(root.AddressSpaceIdentity) ||
        (!commonActive && !privateActive && !otherActive))
    {
        return ResultType::Failure(Ia64BspRuntimeRootBorrowError::InvariantMismatch);
    }

    return ResultType::Success(Ia64BspRuntimeRootView{
        .AddressSpaceStorage = root.AddressSpaceStorage,
        .AddressSpaceIdentity = root.AddressSpaceIdentity,
        .ControllerStorage = &m_addressSpaceController,
        .Processor = processor,
    });
}

Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError>
Ia64BspRuntimeMemory::BorrowReadyInitialComponentRoot() noexcept
{
    return BorrowReadyUserRoot(Ia64BspRuntimeUserDomain::InitialComponent);
}

Ia64RuntimeAddressSpaceOwnerSnapshot Ia64BspRuntimeMemory::GetInitialComponentAddressSpaceSnapshot() const noexcept
{
    return m_initialComponentAddressSpaceOwner.GetSnapshot();
}

Ia64RuntimeAddressSpaceOwnerSnapshot
Ia64BspRuntimeMemory::GetUserAddressSpaceSnapshot(const Ia64BspRuntimeUserDomain domain) const noexcept
{
    return UserRootOwner(domain).GetSnapshot();
}

Foundation::Result<Ia64BspRuntimeUserEntryStorageView, Ia64BspRuntimeRootBorrowError>
Ia64BspRuntimeMemory::GetEntryStorage(const Ia64BspRuntimeEntrySlot slot) noexcept
{
    using ResultType = Foundation::Result<Ia64BspRuntimeUserEntryStorageView, Ia64BspRuntimeRootBorrowError>;
    const Foundation::Size index = static_cast<Foundation::Size>(slot);
    if ((m_state != Ia64BspRuntimeMemoryState::Ready) || (index >= Ia64BspRuntimeEntrySlotCount))
    {
        return ResultType::Failure(Ia64BspRuntimeRootBorrowError::NotReady);
    }
    const bool initial = slot == Ia64BspRuntimeEntrySlot::InitialComponent;
    Foundation::UInt8* const memoryStack = initial ? m_systemCallMemoryStack : m_slotSystemCallMemoryStacks[index - 1U];
    Foundation::UInt8* const rseBackingStore =
        initial ? m_systemCallRseBackingStore : m_slotSystemCallRseBackingStores[index - 1U];
    return ResultType::Success(Ia64BspRuntimeUserEntryStorageView{
        .MemoryStack = memoryStack,
        .MemoryStackStart = AddressOf(memoryStack),
        .MemoryStackSizeBytes = System::Ia64SystemCallMemoryStackSizeBytes,
        .RseBackingStore = rseBackingStore,
        .RseBackingStoreStart = AddressOf(rseBackingStore),
        .RseBackingStoreSizeBytes = System::Ia64SystemCallRseBackingStoreSizeBytes,
        .FaultFrame = &m_userFaultFrames[index],
    });
}

Ia64BspRuntimeSystemCallPublicationError
Ia64BspRuntimeMemory::AdmitSystemCallEntryContext(const Kernel::Scheduler::CpuId processor,
                                                  const Ia64BspRuntimeEntrySlot slot,
                                                  const Foundation::UInt64 bridgeContextAddress) noexcept
{
    const Foundation::Size index = static_cast<Foundation::Size>(slot);
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return Ia64BspRuntimeSystemCallPublicationError::NotReady;
    }
    if ((index == 0U) || (index >= Ia64BspRuntimeEntrySlotCount))
    {
        return Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress;
    }
    if (!processor.IsValid() || (processor.GetValue() != m_summary.BspProcessorIndex))
    {
        return Ia64BspRuntimeSystemCallPublicationError::ProcessorMismatch;
    }
    if (!System::IsIa64SystemCallBridgeContextAddressValid(bridgeContextAddress))
    {
        return Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress;
    }
    const Foundation::Size admittedIndex = index - 1U;
    if (m_slotSystemCallEntryAdmitted[admittedIndex])
    {
        return Ia64BspRuntimeSystemCallPublicationError::AlreadyPublished;
    }
    const Foundation::UInt64 memoryStackStart = AddressOf(m_slotSystemCallMemoryStacks[admittedIndex]);
    const Foundation::UInt64 rseBackingStoreStart = AddressOf(m_slotSystemCallRseBackingStores[admittedIndex]);
    System::Ia64SystemCallEntryContext& context = m_slotSystemCallEntryContexts[admittedIndex];
    context = System::Ia64SystemCallEntryContext{
        .FrameAddress = AddressOf(&m_slotSystemCallEntryFrames[admittedIndex]),
        .MemoryStackStart = memoryStackStart,
        .MemoryStackEnd = memoryStackStart + sizeof(m_slotSystemCallMemoryStacks[admittedIndex]),
        .RseBackingStoreStart = rseBackingStoreStart,
        .RseBackingStoreEnd = rseBackingStoreStart + sizeof(m_slotSystemCallRseBackingStores[admittedIndex]),
        .BridgeContextAddress = bridgeContextAddress,
        .ProcessorIndex = processor.GetValue(),
        .Reserved = 0U,
    };
    const Ia64RuntimeInterruptVectorTablePublicationError admitted =
        m_interruptVectorTable.AdmitSystemCallEntryContext(processor, &context);
    if (admitted != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        context = {};
        m_systemCallInterruptVectorTableError = admitted;
        return Ia64BspRuntimeSystemCallPublicationError::InterruptVectorTableRejected;
    }
    m_slotSystemCallEntryAdmitted[admittedIndex] = true;
    return Ia64BspRuntimeSystemCallPublicationError::None;
}

Ia64RuntimeInterruptVectorTablePublicationError
Ia64BspRuntimeMemory::AdmitUserFaultEntryContext(const Kernel::Scheduler::CpuId processor,
                                                 const Ia64BspRuntimeEntrySlot slot,
                                                 const Foundation::UInt64 handlerAddress) noexcept
{
    const auto storage = GetEntryStorage(slot);
    if (!storage.HasValue())
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ProcessorNotInstalled;
    }
    const Foundation::Size index = static_cast<Foundation::Size>(slot);
    if (m_userFaultEntryAdmitted[index])
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::AlreadyPublished;
    }
    const Ia64BspRuntimeUserEntryStorageView& view = storage.GetValue();
    m_userFaultEntryContexts[index] = Interrupt::Ia64UserFaultEntryContext{
        .FrameAddress = AddressOf(view.FaultFrame),
        .MemoryStackStart = view.MemoryStackStart,
        .MemoryStackEnd = view.MemoryStackStart + view.MemoryStackSizeBytes,
        .RseBackingStoreStart = view.RseBackingStoreStart,
        .RseBackingStoreEnd = view.RseBackingStoreStart + view.RseBackingStoreSizeBytes,
        .HandlerAddress = handlerAddress,
        .ProcessorIndex = processor.IsValid() ? processor.GetValue() : 0U,
        .Reserved = 0U,
    };
    const Ia64RuntimeInterruptVectorTablePublicationError admitted =
        m_interruptVectorTable.AdmitUserFaultEntryContext(processor, &m_userFaultEntryContexts[index]);
    if (admitted != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        m_userFaultEntryContexts[index] = {};
        return admitted;
    }
    m_userFaultEntryAdmitted[index] = true;
    return Ia64RuntimeInterruptVectorTablePublicationError::None;
}

bool Ia64BspRuntimeMemory::SelectEntryContexts(const Kernel::Scheduler::CpuId processor,
                                               const Ia64BspRuntimeEntrySlot slot) noexcept
{
    const Foundation::Size index = static_cast<Foundation::Size>(slot);
    const bool initial = slot == Ia64BspRuntimeEntrySlot::InitialComponent;
    if ((m_state != Ia64BspRuntimeMemoryState::Ready) || (index >= Ia64BspRuntimeEntrySlotCount) ||
        (initial ? !m_systemCallEntryPublished : !m_slotSystemCallEntryAdmitted[index - 1U]))
    {
        return false;
    }
    if (m_interruptVectorTable.SelectSystemCallEntryContext(
            processor, initial ? &m_systemCallEntryContext : &m_slotSystemCallEntryContexts[index - 1U]) !=
        Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        return false;
    }
    // A user-fault word left naming another slot's context would capture
    // this thread's fault onto that slot's storage: the selected slot's fault
    // context is admitted, or none is and the word stays zero.
    if (!m_userFaultEntryAdmitted[index])
    {
        for (const bool admitted : m_userFaultEntryAdmitted)
        {
            if (admitted)
            {
                return false;
            }
        }
        return true;
    }
    return (m_interruptVectorTable.SelectUserFaultEntryContext(processor, &m_userFaultEntryContexts[index]) ==
            Ia64RuntimeInterruptVectorTablePublicationError::None);
}

bool Ia64BspRuntimeMemory::ResetEntryFrames(const Ia64BspRuntimeEntrySlot slot) noexcept
{
    const Foundation::Size index = static_cast<Foundation::Size>(slot);
    if ((m_state != Ia64BspRuntimeMemoryState::Ready) ||
        (index < static_cast<Foundation::Size>(Ia64BspRuntimeEntrySlot::Thread0)) ||
        (index >= Ia64BspRuntimeEntrySlotCount))
    {
        return false;
    }
    m_slotSystemCallEntryFrames[index - 1U] = System::Ia64SystemCallEntryFrame{};
    m_userFaultFrames[index] = Interrupt::Ia64PageFaultFrame{};
    return true;
}

Foundation::Result<Ia64BspRuntimeMemorySummary, Ia64BspRuntimeMemoryError> Ia64BspRuntimeMemory::PrepareAndAdmit(
    const Kernel::Scheduler::CpuId bspProcessor, const PalVirtualMemoryCapabilities& bspCapabilities,
    const InitialAddressSpacePreparation& preparation, const Kernel::Object::ResourceDomainId chargeOwner,
    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    PalVirtualMemoryCapabilities compatibilityCapabilityInputs[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::UInt8 profiledProcessorMask{0U};
    if (bspProcessor.IsValid())
    {
        compatibilityCapabilityInputs[bspProcessor.GetValue()] = bspCapabilities;
        profiledProcessorMask = ProcessorBit(bspProcessor);
    }
    return PrepareAndAdmit(bspProcessor, profiledProcessorMask, compatibilityCapabilityInputs, preparation, chargeOwner,
                           registry, issuer);
}

ASTERIA_NOINLINE_NOIPA ApplicationProcessorAdmissionResult
Ia64BspRuntimeMemory::AdmitApplicationProcessorPhysicalRoot(const Kernel::Scheduler::CpuId processor) noexcept
{
    const Kernel::Scheduler::CpuId bspProcessor{m_summary.BspProcessorIndex};
    const Foundation::Size controllerIndex = ApplicationProcessorControllerIndex(bspProcessor, processor);
    const Ia64BspRuntimeApplicationProcessorAdmissionError validation =
        ValidateApplicationProcessorAdmission(processor, controllerIndex);
    if (validation != Ia64BspRuntimeApplicationProcessorAdmissionError::None)
    {
        return ApplicationProcessorAdmissionResult::Failure(validation);
    }

    const auto installPreflight =
        m_interruptVectorTable.ValidateInstall(processor, *m_preparation, m_activeBindings, m_summary.RuntimeObjects);
    if (!installPreflight.HasValue())
    {
        return ApplicationProcessorAdmissionResult::Failure(
            Ia64BspRuntimeApplicationProcessorAdmissionError::InterruptVectorTablePreflightRejected);
    }

    const Kernel::Memory::AddressSpaceSnapshot beforeAdmission = m_addressSpace.GetSnapshot();
    const Foundation::ProcessorMask expectedResidentProcessorMask =
        static_cast<Foundation::UInt8>(beforeAdmission.ResidentProcessorMask | ProcessorBit(processor));
    m_applicationProcessorAdmissionSummaries[controllerIndex] = {};
    m_applicationProcessorAdmissionFailures[controllerIndex] = {};
    m_applicationProcessorAdmissionStates[controllerIndex] =
        Ia64BspRuntimeApplicationProcessorAdmissionState::Admitting;

    const auto installed =
        m_interruptVectorTable.InstallAndAdmit(processor, *m_preparation, m_activeBindings, m_summary.RuntimeObjects);
    if (!installed.HasValue())
    {
        const Ia64BspRuntimeApplicationProcessorAdmissionError failure = FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::InterruptVectorTableInstallRejected,
            installed.GetError(), Hal::Memory::AddressSpacePageTableActivationError::None,
            Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
            Ia64BspRuntimeApplicationProcessorPostconditionError::None, false, false);
        return ApplicationProcessorAdmissionResult::Failure(failure);
    }
    m_applicationProcessorAdmissionSummaries[controllerIndex].InterruptVectorTable = installed.GetValue();
    m_applicationProcessorAdmissionSummaries[controllerIndex].Processor = processor;
    m_applicationProcessorAdmissionFailures[controllerIndex].InterruptVectorTableCommitted = true;

    const Hal::Memory::AddressSpacePageTableActivationError physicalActivation =
        m_addressSpacePageTable.ActivatePhysicalBootstrap(m_addressSpace, processor);
    if (physicalActivation != Hal::Memory::AddressSpacePageTableActivationError::None)
    {
        const bool physicalRootMayBeActive =
            physicalActivation == Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
        const Ia64BspRuntimeApplicationProcessorAdmissionError failure = FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::PhysicalBootstrapActivationRejected,
            Ia64RuntimeInterruptVectorTableInstallError::None, physicalActivation,
            Kernel::Memory::ProcessorAddressSpaceControllerFailure{},
            Ia64BspRuntimeApplicationProcessorPostconditionError::None, true, physicalRootMayBeActive);
        return ApplicationProcessorAdmissionResult::Failure(failure);
    }
    m_applicationProcessorAdmissionSummaries[controllerIndex].PhysicalBootstrapActivationPerformed = true;
    m_applicationProcessorAdmissionFailures[controllerIndex].PhysicalRootMayBeActive = true;

    const auto controllerActivation =
        m_applicationProcessorControllers[controllerIndex].Initialize(processor, m_addressSpace);
    if (!controllerActivation.HasValue())
    {
        const Ia64BspRuntimeApplicationProcessorAdmissionError failure = FailApplicationProcessorAdmission(
            controllerIndex, Ia64BspRuntimeApplicationProcessorAdmissionError::ProcessorControllerActivationRejected,
            Ia64RuntimeInterruptVectorTableInstallError::None, Hal::Memory::AddressSpacePageTableActivationError::None,
            controllerActivation.GetError(), Ia64BspRuntimeApplicationProcessorPostconditionError::None, true, true);
        return ApplicationProcessorAdmissionResult::Failure(failure);
    }

    const Ia64BspRuntimeApplicationProcessorAdmissionError postcondition = VerifyApplicationProcessorPostconditions(
        processor, controllerIndex, expectedResidentProcessorMask, beforeAdmission.MappingCount, installed.GetValue(),
        controllerActivation.GetValue());
    if (postcondition != Ia64BspRuntimeApplicationProcessorAdmissionError::None)
    {
        return ApplicationProcessorAdmissionResult::Failure(postcondition);
    }
    return ApplicationProcessorAdmissionResult::Success(m_applicationProcessorAdmissionSummaries[controllerIndex]);
}

ApplicationProcessorAdmissionSnapshotResult
Ia64BspRuntimeMemory::GetApplicationProcessorAdmissionSnapshot(const Kernel::Scheduler::CpuId processor) const noexcept
{
    if (!processor.IsValid())
    {
        return ApplicationProcessorAdmissionSnapshotResult::Failure(
            Ia64BspRuntimeApplicationProcessorAdmissionError::InvalidProcessor);
    }
    const Kernel::Scheduler::CpuId bspProcessor{m_summary.BspProcessorIndex};
    if (!bspProcessor.IsValid() || (processor.GetValue() == bspProcessor.GetValue()))
    {
        return ApplicationProcessorAdmissionSnapshotResult::Failure(
            Ia64BspRuntimeApplicationProcessorAdmissionError::BspProcessor);
    }
    if ((m_summary.ProfiledProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return ApplicationProcessorAdmissionSnapshotResult::Failure(
            Ia64BspRuntimeApplicationProcessorAdmissionError::ProcessorNotProfiled);
    }

    const Foundation::Size controllerIndex = ApplicationProcessorControllerIndex(bspProcessor, processor);
    if (controllerIndex >= Ia64BspRuntimeMaximumApplicationProcessorCount)
    {
        return ApplicationProcessorAdmissionSnapshotResult::Failure(
            Ia64BspRuntimeApplicationProcessorAdmissionError::InvalidProcessor);
    }
    return ApplicationProcessorAdmissionSnapshotResult::Success(Ia64BspRuntimeApplicationProcessorAdmissionSnapshot{
        .Summary = m_applicationProcessorAdmissionSummaries[controllerIndex],
        .Failure = m_applicationProcessorAdmissionFailures[controllerIndex],
        .Controller = m_applicationProcessorControllers[controllerIndex].GetSnapshot(),
        .State = m_applicationProcessorAdmissionStates[controllerIndex],
        .Reserved = {},
    });
}

ApplicationProcessorRootBorrowResult
Ia64BspRuntimeMemory::BorrowReadyApplicationProcessorRoot(const Kernel::Scheduler::CpuId processor) noexcept
{
    using AdmissionError = Ia64BspRuntimeApplicationProcessorAdmissionError;
    using AdmissionState = Ia64BspRuntimeApplicationProcessorAdmissionState;

    if (!processor.IsValid())
    {
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::InvalidProcessor);
    }

    const Kernel::Scheduler::CpuId bspProcessor{m_summary.BspProcessorIndex};
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        if (bspProcessor.IsValid() && (processor.GetValue() != bspProcessor.GetValue()))
        {
            const Foundation::Size retainedControllerIndex =
                ApplicationProcessorControllerIndex(bspProcessor, processor);
            if ((retainedControllerIndex < Ia64BspRuntimeMaximumApplicationProcessorCount) &&
                (m_applicationProcessorAdmissionStates[retainedControllerIndex] == AdmissionState::Failed))
            {
                return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::PreviousAdmissionFailed);
            }
        }
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::NotReady);
    }
    if (!bspProcessor.IsValid())
    {
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
    }
    if (processor.GetValue() == bspProcessor.GetValue())
    {
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::BspProcessor);
    }
    if ((m_summary.ProfiledProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::ProcessorNotProfiled);
    }

    const Foundation::Size controllerIndex = ApplicationProcessorControllerIndex(bspProcessor, processor);
    if (controllerIndex >= Ia64BspRuntimeMaximumApplicationProcessorCount)
    {
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::InvalidProcessor);
    }
    switch (m_applicationProcessorAdmissionStates[controllerIndex])
    {
    case AdmissionState::Unadmitted:
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::NotReady);
    case AdmissionState::Admitting:
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::AdmissionInProgress);
    case AdmissionState::Failed:
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::PreviousAdmissionFailed);
    case AdmissionState::Ready:
        break;
    }

    Foundation::ProcessorMask expectedResidentProcessorMask = ProcessorBit(bspProcessor);
    Foundation::ProcessorMask expectedAdmissionProcessorMask{0U};
    Kernel::Scheduler::CpuId lastAdmittedProcessor{bspProcessor};
    bool selectedAdmissionObserved{false};
    bool admissionBarrierObserved{false};
    for (Foundation::UInt8 candidate{0U}; candidate < Kernel::Scheduler::MaximumProcessorCount; ++candidate)
    {
        if (candidate == bspProcessor.GetValue())
        {
            continue;
        }
        const Kernel::Scheduler::CpuId candidateProcessor{candidate};
        const Foundation::Size candidateIndex = ApplicationProcessorControllerIndex(bspProcessor, candidateProcessor);
        if (candidateIndex >= Ia64BspRuntimeMaximumApplicationProcessorCount)
        {
            return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
        }

        const AdmissionState candidateState = m_applicationProcessorAdmissionStates[candidateIndex];
        const bool candidateIsProfiled = (m_summary.ProfiledProcessorMask & ProcessorBit(candidateProcessor)) != 0U;
        if (!candidateIsProfiled)
        {
            if (candidateState != AdmissionState::Unadmitted)
            {
                return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
            }
            continue;
        }

        switch (candidateState)
        {
        case AdmissionState::Unadmitted:
        case AdmissionState::Failed:
            admissionBarrierObserved = true;
            break;
        case AdmissionState::Admitting:
            return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
        case AdmissionState::Ready:
            if (admissionBarrierObserved)
            {
                return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
            }
            expectedResidentProcessorMask =
                static_cast<Foundation::UInt8>(expectedResidentProcessorMask | ProcessorBit(candidateProcessor));
            lastAdmittedProcessor = candidateProcessor;
            if (candidate == processor.GetValue())
            {
                expectedAdmissionProcessorMask = expectedResidentProcessorMask;
                selectedAdmissionObserved = true;
            }
            break;
        }
    }
    if (!selectedAdmissionObserved)
    {
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
    }

    const Ia64BspRuntimeApplicationProcessorAdmissionSummary& admission =
        m_applicationProcessorAdmissionSummaries[controllerIndex];
    const Ia64BspRuntimeApplicationProcessorAdmissionFailure& failure =
        m_applicationProcessorAdmissionFailures[controllerIndex];
    const Kernel::Memory::AddressSpaceSnapshot addressSpaceSnapshot = m_addressSpace.GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controllerSnapshot =
        m_applicationProcessorControllers[controllerIndex].GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerFailure& controllerFailure = failure.ControllerFailure;
    if (!HasLiveAddressSpaceRegistryBinding() || !m_summary.AddressSpaceIdentity.IsValid() ||
        ((m_summary.ProfiledProcessorMask & ProcessorBit(bspProcessor)) == 0U) ||
        (m_summary.ResidentProcessorMask != expectedResidentProcessorMask) || !m_summary.BackendClaimed ||
        !m_summary.ExactLedgerVerified ||
        (m_summary.ControllerState != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        (m_summary.InterruptVectorTable.TableAddress == 0U) || (m_summary.InterruptVectorTable.ContextAddress == 0U) ||
        (m_summary.InterruptVectorTable.InstalledProcessorMask != expectedResidentProcessorMask) ||
        (m_summary.InterruptVectorTable.ProcessorIndex != lastAdmittedProcessor.GetValue()) ||
        (m_summary.InterruptVectorTable.Reserved != 0U) ||
        !addressSpaceSnapshot.Identity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (addressSpaceSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        (addressSpaceSnapshot.ResidentProcessorMask != expectedResidentProcessorMask) ||
        (addressSpaceSnapshot.TransitionSequence != 0U) || addressSpaceSnapshot.ActivatingProcessor.IsValid() ||
        addressSpaceSnapshot.TransitionProcessor.IsValid() || (addressSpaceSnapshot.TargetProcessorMask != 0U) ||
        (addressSpaceSnapshot.AcknowledgedProcessorMask != 0U) ||
        !ReservedBytesAreZero(addressSpaceSnapshot.Reserved) ||
        !m_addressSpacePageTable.IsActiveLedgerBound(processor) ||
        (ClassifyBinding(m_activeBindings, processor) != Ia64BspRuntimeBindingState::Active) ||
        !admission.AddressSpaceIdentity.IsEqualTo(m_summary.AddressSpaceIdentity) || !admission.Processor.IsValid() ||
        (admission.Processor.GetValue() != processor.GetValue()) ||
        (admission.ResidentProcessorMask != expectedAdmissionProcessorMask) ||
        (admission.InstalledProcessorMask != expectedAdmissionProcessorMask) ||
        (admission.ControllerState != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        !admission.PhysicalBootstrapActivationPerformed || !admission.ControllerAcknowledgementPerformed ||
        !admission.ExactLedgerVerified || (admission.Reserved != 0U) ||
        (admission.InterruptVectorTable.TableAddress != m_summary.InterruptVectorTable.TableAddress) ||
        (admission.InterruptVectorTable.ContextAddress == 0U) ||
        (admission.InterruptVectorTable.SizeBytes != m_summary.InterruptVectorTable.SizeBytes) ||
        (admission.InterruptVectorTable.ProcessorIndex != processor.GetValue()) ||
        (admission.InterruptVectorTable.InstalledProcessorMask != expectedAdmissionProcessorMask) ||
        (admission.InterruptVectorTable.Reserved != 0U) || admission.ControllerReceipt.Previous.IsValid() ||
        !admission.ControllerReceipt.Active.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        !admission.ControllerReceipt.Processor.IsValid() ||
        (admission.ControllerReceipt.Processor.GetValue() != processor.GetValue()) ||
        !admission.ControllerReceipt.NativeActivationPerformed ||
        !ReservedBytesAreZero(admission.ControllerReceipt.Reserved) || (failure.Error != AdmissionError::None) ||
        (failure.InterruptVectorTableError != Ia64RuntimeInterruptVectorTableInstallError::None) ||
        (failure.PhysicalBootstrapActivationError != Hal::Memory::AddressSpacePageTableActivationError::None) ||
        (failure.PostconditionError != Ia64BspRuntimeApplicationProcessorPostconditionError::None) ||
        failure.InterruptVectorTableCommitted || failure.PhysicalRootMayBeActive ||
        !ReservedBytesAreZero(failure.Reserved) ||
        (controllerFailure.Error != Kernel::Memory::ProcessorAddressSpaceControllerError::None) ||
        (controllerFailure.Activation.Error != Kernel::Memory::AddressSpaceError::None) ||
        (controllerFailure.Activation.PageTable != Hal::Memory::AddressSpacePageTableActivationError::None) ||
        controllerFailure.Activation.ProcessorMayBeResident ||
        !ReservedBytesAreZero(controllerFailure.Activation.Reserved) ||
        (controllerFailure.Transition != Kernel::Memory::AddressSpaceError::None) ||
        (controllerFailure.Detach != Kernel::Memory::AddressSpaceError::None) ||
        controllerFailure.AdmissionRollbackSafe || !ReservedBytesAreZero(controllerFailure.Reserved) ||
        (controllerSnapshot.IdleStorage != &m_addressSpace) || (controllerSnapshot.ActiveStorage != &m_addressSpace) ||
        (controllerSnapshot.PreviousStorage != nullptr) ||
        !controllerSnapshot.Idle.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        !controllerSnapshot.Active.IsEqualTo(m_summary.AddressSpaceIdentity) || controllerSnapshot.Previous.IsValid() ||
        controllerSnapshot.SourceTransition.IsValid() || controllerSnapshot.DestinationTransition.IsValid() ||
        !controllerSnapshot.Processor.IsValid() || (controllerSnapshot.Processor.GetValue() != processor.GetValue()) ||
        (controllerSnapshot.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        !ReservedBytesAreZero(controllerSnapshot.Reserved))
    {
        return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
    }

    for (Foundation::UInt8 candidate{0U}; candidate < Kernel::Scheduler::MaximumProcessorCount; ++candidate)
    {
        const Kernel::Scheduler::CpuId candidateProcessor{candidate};
        const Ia64BspRuntimeBindingState expectedBinding =
            (expectedResidentProcessorMask & ProcessorBit(candidateProcessor)) != 0U
                ? Ia64BspRuntimeBindingState::Active
                : Ia64BspRuntimeBindingState::Unadmitted;
        if (ClassifyBinding(m_activeBindings, candidateProcessor) != expectedBinding ||
            ((expectedBinding == Ia64BspRuntimeBindingState::Active) &&
             !m_addressSpacePageTable.IsActiveLedgerBound(candidateProcessor)))
        {
            return ApplicationProcessorRootBorrowResult::Failure(AdmissionError::RootInvariantMismatch);
        }
    }

    return ApplicationProcessorRootBorrowResult::Success(Ia64BspRuntimeRootView{
        .AddressSpaceStorage = &m_addressSpace,
        .AddressSpaceIdentity = m_summary.AddressSpaceIdentity,
        .ControllerStorage = &m_applicationProcessorControllers[controllerIndex],
        .Processor = processor,
    });
}

Foundation::Result<Ia64BspRuntimeRootView, Ia64BspRuntimeRootBorrowError>
Ia64BspRuntimeMemory::BorrowReadyRoot() noexcept
{
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return RootBorrowResult::Failure(Ia64BspRuntimeRootBorrowError::NotReady);
    }

    const Kernel::Memory::AddressSpaceId identity = m_summary.AddressSpaceIdentity;
    const Kernel::Scheduler::CpuId processor{m_summary.BspProcessorIndex};
    const Kernel::Memory::AddressSpaceSnapshot addressSpaceSnapshot = m_addressSpace.GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controllerSnapshot =
        m_addressSpaceController.GetSnapshot();
    if (!HasLiveAddressSpaceRegistryBinding() || !identity.IsValid() || !processor.IsValid() ||
        !addressSpaceSnapshot.Identity.IsEqualTo(identity) ||
        (addressSpaceSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        (controllerSnapshot.IdleStorage != &m_addressSpace) || (controllerSnapshot.ActiveStorage != &m_addressSpace) ||
        (controllerSnapshot.PreviousStorage != nullptr) || !controllerSnapshot.Idle.IsEqualTo(identity) ||
        !controllerSnapshot.Active.IsEqualTo(identity) || controllerSnapshot.Previous.IsValid() ||
        controllerSnapshot.SourceTransition.IsValid() || controllerSnapshot.DestinationTransition.IsValid() ||
        !controllerSnapshot.Processor.IsValid() || (controllerSnapshot.Processor.GetValue() != processor.GetValue()) ||
        (controllerSnapshot.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready))
    {
        return RootBorrowResult::Failure(Ia64BspRuntimeRootBorrowError::InvariantMismatch);
    }

    return RootBorrowResult::Success(Ia64BspRuntimeRootView{
        .AddressSpaceStorage = &m_addressSpace,
        .AddressSpaceIdentity = identity,
        .ControllerStorage = &m_addressSpaceController,
        .Processor = processor,
    });
}

Ia64BspRuntimeSystemCallPublicationError
Ia64BspRuntimeMemory::PublishSystemCallEntryContext(const Kernel::Scheduler::CpuId processor,
                                                    const Foundation::UInt64 bridgeContextAddress) noexcept
{
    const auto reject = [this](const Ia64BspRuntimeSystemCallPublicationError error,
                               const System::Ia64SystemCallContextValidationError validationError =
                                   System::Ia64SystemCallContextValidationError::None,
                               const Ia64RuntimeInterruptVectorTablePublicationError interruptVectorTableError =
                                   Ia64RuntimeInterruptVectorTablePublicationError::None) noexcept
    {
        m_systemCallPublicationError = error;
        m_systemCallContextValidationError = validationError;
        m_systemCallInterruptVectorTableError = interruptVectorTableError;
        return error;
    };

    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::NotReady);
    }
    if (!processor.IsValid())
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::InvalidProcessor);
    }
    const Kernel::Scheduler::CpuId admittedProcessor{m_summary.BspProcessorIndex};
    if (!admittedProcessor.IsValid() || (processor.GetValue() != admittedProcessor.GetValue()) ||
        ((m_summary.ProfiledProcessorMask & ProcessorBit(processor)) == 0U) ||
        ((m_summary.ResidentProcessorMask & ProcessorBit(processor)) == 0U))
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::ProcessorMismatch);
    }
    if (!System::IsIa64SystemCallBridgeContextAddressValid(bridgeContextAddress))
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress);
    }

    const Foundation::UInt64 contextAddress = AddressOf(&m_systemCallEntryContext);
    const Foundation::UInt64 frameAddress = AddressOf(&m_systemCallEntryFrame);
    const Foundation::UInt64 memoryStackStart = AddressOf(m_systemCallMemoryStack);
    const Foundation::UInt64 rseBackingStoreStart = AddressOf(m_systemCallRseBackingStore);
    Foundation::UInt64 memoryStackEnd{0U};
    Foundation::UInt64 rseBackingStoreEnd{0U};
    if (!TryCalculateEnd(memoryStackStart, sizeof(m_systemCallMemoryStack), memoryStackEnd) ||
        !TryCalculateEnd(rseBackingStoreStart, sizeof(m_systemCallRseBackingStore), rseBackingStoreEnd))
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::ReferencedStorageOutsideRuntimeObjectRange);
    }

    const System::Ia64SystemCallEntryContext candidate{
        .FrameAddress = frameAddress,
        .MemoryStackStart = memoryStackStart,
        .MemoryStackEnd = memoryStackEnd,
        .RseBackingStoreStart = rseBackingStoreStart,
        .RseBackingStoreEnd = rseBackingStoreEnd,
        .BridgeContextAddress = bridgeContextAddress,
        .ProcessorIndex = processor.GetValue(),
        .Reserved = 0U,
    };
    const System::Ia64SystemCallContextValidationError validationError =
        System::ValidateIa64SystemCallTrustedContext(candidate);
    if (validationError != System::Ia64SystemCallContextValidationError::None)
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::ContextValidationRejected, validationError);
    }
    if (!IsRangeInside(m_summary.RuntimeObjects, contextAddress, sizeof(m_systemCallEntryContext)))
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::ContextOutsideRuntimeObjectRange);
    }
    if (!IsRangeInside(m_summary.RuntimeObjects, frameAddress, sizeof(m_systemCallEntryFrame)) ||
        !IsRangeInside(m_summary.RuntimeObjects, memoryStackStart, sizeof(m_systemCallMemoryStack)) ||
        !IsRangeInside(m_summary.RuntimeObjects, rseBackingStoreStart, sizeof(m_systemCallRseBackingStore)))
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::ReferencedStorageOutsideRuntimeObjectRange);
    }
    if (m_systemCallEntryPublished)
    {
        return reject(Ia64BspRuntimeSystemCallPublicationError::AlreadyPublished);
    }

    const System::Ia64SystemCallEntryContext previousContext = m_systemCallEntryContext;
    m_systemCallEntryContext = candidate;
    const Ia64RuntimeInterruptVectorTablePublicationError publicationError =
        m_interruptVectorTable.PublishSystemCallEntryContext(processor, &m_systemCallEntryContext);
    if (publicationError != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        m_systemCallEntryContext = previousContext;
        return reject(Ia64BspRuntimeSystemCallPublicationError::InterruptVectorTableRejected,
                      System::Ia64SystemCallContextValidationError::None, publicationError);
    }

    m_systemCallEntryPublished = true;
    m_systemCallPublicationError = Ia64BspRuntimeSystemCallPublicationError::None;
    m_systemCallContextValidationError = System::Ia64SystemCallContextValidationError::None;
    m_systemCallInterruptVectorTableError = Ia64RuntimeInterruptVectorTablePublicationError::None;
    return Ia64BspRuntimeSystemCallPublicationError::None;
}

Ia64BspRuntimeSystemCallPublicationSnapshot Ia64BspRuntimeMemory::GetSystemCallPublicationSnapshot() const noexcept
{
    return Ia64BspRuntimeSystemCallPublicationSnapshot{
        .Context = m_systemCallEntryContext,
        .ContextAddress = AddressOf(&m_systemCallEntryContext),
        .LastError = m_systemCallPublicationError,
        .LastContextValidationError = m_systemCallContextValidationError,
        .LastInterruptVectorTableError = m_systemCallInterruptVectorTableError,
        .Published = m_systemCallEntryPublished,
        .Reserved = {},
    };
}

Ia64BspRuntimeExternalInterruptPublicationError
Ia64BspRuntimeMemory::PublishExternalInterruptEntryContext(const Kernel::Scheduler::CpuId processor) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64BspRuntimeExternalInterruptPublicationError::InvalidProcessor;
    }

    Ia64BspRuntimeExternalInterruptTransportSlot& slot = m_externalInterruptTransportSlots[processor.GetValue()];
    const auto reject = [&slot](const Ia64BspRuntimeExternalInterruptPublicationError error,
                                const Ia64RuntimeExternalInterruptPublicationError interruptVectorTableError =
                                    Ia64RuntimeExternalInterruptPublicationError::None) noexcept
    {
        slot.LastError = error;
        slot.LastInterruptVectorTableError = interruptVectorTableError;
        return error;
    };

    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return reject(Ia64BspRuntimeExternalInterruptPublicationError::NotReady);
    }
    const Foundation::UInt8 processorBit = ProcessorBit(processor);
    if (((m_summary.ProfiledProcessorMask & processorBit) == 0U) ||
        ((m_summary.ResidentProcessorMask & processorBit) == 0U) ||
        ((m_summary.InterruptVectorTable.InstalledProcessorMask & processorBit) == 0U))
    {
        return reject(Ia64BspRuntimeExternalInterruptPublicationError::ProcessorMismatch);
    }

    const Foundation::UInt64 contextAddress = AddressOf(&slot.Context);
    const Foundation::UInt64 evidenceAddress = AddressOf(&slot.Evidence);
    if (!IsRangeInside(m_summary.RuntimeObjects, contextAddress, sizeof(slot.Context)))
    {
        return reject(Ia64BspRuntimeExternalInterruptPublicationError::ContextOutsideRuntimeObjectRange);
    }
    if (!IsRangeInside(m_summary.RuntimeObjects, evidenceAddress, sizeof(slot.Evidence)))
    {
        return reject(Ia64BspRuntimeExternalInterruptPublicationError::EvidenceOutsideRuntimeObjectRange);
    }
    if (slot.Published)
    {
        return reject(Ia64BspRuntimeExternalInterruptPublicationError::AlreadyPublished);
    }

    const Interrupt::Ia64ExternalInterruptEntryContext candidate{
        .EvidenceAddress = evidenceAddress,
        .ProcessorIndex = processor.GetValue(),
        .TimerVector = Interrupt::Ia64ExternalInterruptTimerVector,
        .ExternalPreemptionControlAddress = 0U,
    };
    const Interrupt::Ia64ExternalInterruptEntryContext previousContext = slot.Context;
    slot.Context = candidate;
    const Ia64RuntimeExternalInterruptPublicationError publicationError =
        m_interruptVectorTable.PublishExternalInterruptEntryContext(processor, &slot.Context);
    if (publicationError != Ia64RuntimeExternalInterruptPublicationError::None)
    {
        slot.Context = previousContext;
        return reject(Ia64BspRuntimeExternalInterruptPublicationError::InterruptVectorTableRejected, publicationError);
    }

    // The boot coordinator keeps PSR.i clear through this release publication.
    // Both owned ranges already inherit the runtime owner's locked DTR coverage
    // and remain stable for the complete boot lifetime.
    slot.Published = true;
    slot.LastError = Ia64BspRuntimeExternalInterruptPublicationError::None;
    slot.LastInterruptVectorTableError = Ia64RuntimeExternalInterruptPublicationError::None;
    slot.LastAcknowledgementError = Ia64BspRuntimeExternalInterruptAcknowledgementError::None;
    return Ia64BspRuntimeExternalInterruptPublicationError::None;
}

ExternalPreemptionStorageResult
Ia64BspRuntimeMemory::BorrowExternalPreemptionStorage(const Kernel::Scheduler::CpuId processor) noexcept
{
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return ExternalPreemptionStorageResult::Failure(Ia64BspRuntimeExternalPreemptionError::NotReady);
    }
    if (!processor.IsValid())
    {
        return ExternalPreemptionStorageResult::Failure(Ia64BspRuntimeExternalPreemptionError::InvalidProcessor);
    }
    if (processor.GetValue() != m_summary.BspProcessorIndex)
    {
        return ExternalPreemptionStorageResult::Failure(Ia64BspRuntimeExternalPreemptionError::NotBspProcessor);
    }
    if (m_externalPreemptionStorageBorrowed)
    {
        return ExternalPreemptionStorageResult::Failure(Ia64BspRuntimeExternalPreemptionError::StorageAlreadyBorrowed);
    }

    m_externalPreemptionStorageBorrowed = true;
    return ExternalPreemptionStorageResult::Success(Ia64BspRuntimeExternalPreemptionStorageView{
        .Control = &m_externalPreemptionControl,
        .CoordinatorMemoryStack = m_externalPreemptionCoordinatorMemoryStack,
        .CoordinatorRseBackingStore = m_externalPreemptionCoordinatorRseBackingStore,
        .CoordinatorMemoryStackAddress = AddressOf(m_externalPreemptionCoordinatorMemoryStack),
        .CoordinatorMemoryStackSizeBytes = sizeof(m_externalPreemptionCoordinatorMemoryStack),
        .CoordinatorRseBackingStoreAddress = AddressOf(m_externalPreemptionCoordinatorRseBackingStore),
        .CoordinatorRseBackingStoreSizeBytes = sizeof(m_externalPreemptionCoordinatorRseBackingStore),
    });
}

Ia64BspRuntimeExternalPreemptionError
Ia64BspRuntimeMemory::PublishExternalPreemptionControl(const Kernel::Scheduler::CpuId processor,
                                                       Interrupt::Ia64ExternalPreemptionControl& control) noexcept
{
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return Ia64BspRuntimeExternalPreemptionError::NotReady;
    }
    if (!processor.IsValid())
    {
        return Ia64BspRuntimeExternalPreemptionError::InvalidProcessor;
    }
    if (processor.GetValue() != m_summary.BspProcessorIndex)
    {
        return Ia64BspRuntimeExternalPreemptionError::NotBspProcessor;
    }

    Ia64BspRuntimeExternalInterruptTransportSlot& slot = m_externalInterruptTransportSlots[processor.GetValue()];
    if (!slot.Published)
    {
        return Ia64BspRuntimeExternalPreemptionError::ExternalInterruptNotPublished;
    }
    if (!m_externalPreemptionStorageBorrowed || (&control != &m_externalPreemptionControl) ||
        (slot.Context.EvidenceAddress != AddressOf(&slot.Evidence)) ||
        (slot.Context.ProcessorIndex != processor.GetValue()) ||
        (slot.Context.TimerVector != Interrupt::Ia64ExternalInterruptTimerVector))
    {
        return Ia64BspRuntimeExternalPreemptionError::PublicationMismatch;
    }
    Synchronization::AtomicUInt64Reference publication{slot.Context.ExternalPreemptionControlAddress};
    if (publication.LoadAcquire() != 0U)
    {
        return Ia64BspRuntimeExternalPreemptionError::ControlAlreadyPublished;
    }
    if ((Interrupt::AcquireIa64ExternalPreemptionMode(control) !=
         Interrupt::Ia64ExternalPreemptionMode::LegacyEvidence) ||
        (Interrupt::AcquireIa64ExternalPreemptionControlState(control) !=
         Interrupt::Ia64ExternalPreemptionControlState::Configured))
    {
        return Ia64BspRuntimeExternalPreemptionError::ControlNotConfigured;
    }
    const Foundation::UInt64 sequence = Interrupt::AcquireIa64LocalTimerInterruptEvidenceSequence(slot.Evidence);
    const Interrupt::Ia64LocalTimerInterruptEvidence evidence =
        Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(slot.Evidence, sequence);
    if (sequence != evidence.AcknowledgedSequence)
    {
        return Ia64BspRuntimeExternalPreemptionError::PublicationMismatch;
    }

    publication.StoreRelease(AddressOf(&control));
    return Ia64BspRuntimeExternalPreemptionError::None;
}

bool Ia64BspRuntimeMemory::PageFaultEntryStorageInvariantHolds() const noexcept
{
    const Ia64RuntimeObjectRange expectedRuntimeObjects{
        .VirtualStart = AddressOf(this),
        .SizeBytes = sizeof(*this),
    };
    if (!IsValidRuntimeObjectRange(expectedRuntimeObjects) ||
        (m_summary.RuntimeObjects.VirtualStart != expectedRuntimeObjects.VirtualStart) ||
        (m_summary.RuntimeObjects.SizeBytes != expectedRuntimeObjects.SizeBytes))
    {
        return false;
    }

    const Foundation::UInt64 pageFaultStarts[5U]{
        AddressOf(&m_pageFaultEntryContext),
        AddressOf(&m_pageFaultEntryControl),
        AddressOf(&m_pageFaultFrame),
        AddressOf(m_pageFaultCoordinatorMemoryStack),
        AddressOf(m_pageFaultCoordinatorRseBackingStore),
    };
    const Foundation::UInt64 pageFaultSizes[5U]{
        sizeof(m_pageFaultEntryContext),
        sizeof(m_pageFaultEntryControl),
        sizeof(m_pageFaultFrame),
        sizeof(m_pageFaultCoordinatorMemoryStack),
        sizeof(m_pageFaultCoordinatorRseBackingStore),
    };
    const Foundation::UInt64 requiredAlignments[5U]{
        alignof(Interrupt::Ia64PageFaultEntryContext), alignof(Interrupt::Ia64PageFaultEntryControl),
        Kernel::Memory::MappingPageSizeBytes,          Kernel::Memory::MappingPageSizeBytes,
        Kernel::Memory::MappingPageSizeBytes,
    };
    Foundation::UInt64 pageFaultEnds[5U]{};
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        if ((pageFaultStarts[index] & (requiredAlignments[index] - 1U)) != 0U ||
            !TryCalculateEnd(pageFaultStarts[index], pageFaultSizes[index], pageFaultEnds[index]) ||
            !IsRangeInside(m_summary.RuntimeObjects, pageFaultStarts[index], pageFaultSizes[index]) ||
            !HasLockedDataTranslationCoverage(pageFaultStarts[index], pageFaultSizes[index]))
        {
            return false;
        }
    }

    for (Foundation::Size first{0U}; first < 5U; ++first)
    {
        for (Foundation::Size second{first + 1U}; second < 5U; ++second)
        {
            if (RangesOverlap(pageFaultStarts[first], pageFaultEnds[first], pageFaultStarts[second],
                              pageFaultEnds[second]))
            {
                return false;
            }
        }
    }

    const Foundation::UInt64 retainedStarts[8U]{
        AddressOf(&m_systemCallEntryContext),
        AddressOf(&m_systemCallEntryFrame),
        AddressOf(m_systemCallMemoryStack),
        AddressOf(m_systemCallRseBackingStore),
        AddressOf(m_externalInterruptTransportSlots),
        AddressOf(&m_externalPreemptionControl),
        AddressOf(m_externalPreemptionCoordinatorMemoryStack),
        AddressOf(m_externalPreemptionCoordinatorRseBackingStore),
    };
    const Foundation::UInt64 retainedSizes[8U]{
        sizeof(m_systemCallEntryContext),
        sizeof(m_systemCallEntryFrame),
        sizeof(m_systemCallMemoryStack),
        sizeof(m_systemCallRseBackingStore),
        sizeof(m_externalInterruptTransportSlots),
        sizeof(m_externalPreemptionControl),
        sizeof(m_externalPreemptionCoordinatorMemoryStack),
        sizeof(m_externalPreemptionCoordinatorRseBackingStore),
    };
    for (Foundation::Size retained{0U}; retained < 8U; ++retained)
    {
        Foundation::UInt64 retainedEnd{0U};
        if (!TryCalculateEnd(retainedStarts[retained], retainedSizes[retained], retainedEnd))
        {
            return false;
        }
        for (Foundation::Size pageFault{0U}; pageFault < 5U; ++pageFault)
        {
            if (RangesOverlap(pageFaultStarts[pageFault], pageFaultEnds[pageFault], retainedStarts[retained],
                              retainedEnd))
            {
                return false;
            }
        }
    }
    return true;
}

PageFaultEntryStorageResult
Ia64BspRuntimeMemory::BorrowPageFaultEntryStorage(const Kernel::Scheduler::CpuId processor) noexcept
{
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return PageFaultEntryStorageResult::Failure(Ia64BspRuntimePageFaultEntryError::NotReady);
    }
    if (!processor.IsValid())
    {
        return PageFaultEntryStorageResult::Failure(Ia64BspRuntimePageFaultEntryError::InvalidProcessor);
    }
    if (processor.GetValue() != m_summary.BspProcessorIndex)
    {
        return PageFaultEntryStorageResult::Failure(Ia64BspRuntimePageFaultEntryError::NotBspProcessor);
    }
    if (m_pageFaultEntryState != Ia64BspRuntimePageFaultEntryState::Fresh)
    {
        return PageFaultEntryStorageResult::Failure(Ia64BspRuntimePageFaultEntryError::StorageAlreadyBorrowed);
    }
    if (!PageFaultEntryStorageInvariantHolds())
    {
        return PageFaultEntryStorageResult::Failure(Ia64BspRuntimePageFaultEntryError::StorageInvariantMismatch);
    }
    if (!Interrupt::Ia64PageFaultFrameIsPristine(m_pageFaultFrame))
    {
        return PageFaultEntryStorageResult::Failure(Ia64BspRuntimePageFaultEntryError::FrameNotPristine);
    }
    if (Interrupt::AcquireIa64PageFaultEntryControlState(m_pageFaultEntryControl) !=
        Interrupt::Ia64PageFaultEntryControlState::Fresh)
    {
        return PageFaultEntryStorageResult::Failure(Ia64BspRuntimePageFaultEntryError::StorageInvariantMismatch);
    }

    m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Borrowed;
    m_pageFaultEntryError = Ia64BspRuntimePageFaultEntryError::None;
    return PageFaultEntryStorageResult::Success(Ia64BspRuntimePageFaultEntryStorageView{
        .Control = &m_pageFaultEntryControl,
        .Context = &m_pageFaultEntryContext,
        .Frame = &m_pageFaultFrame,
        .CoordinatorMemoryStack = m_pageFaultCoordinatorMemoryStack,
        .CoordinatorRseBackingStore = m_pageFaultCoordinatorRseBackingStore,
        .ControlAddress = AddressOf(&m_pageFaultEntryControl),
        .ContextAddress = AddressOf(&m_pageFaultEntryContext),
        .FrameAddress = AddressOf(&m_pageFaultFrame),
        .CoordinatorMemoryStackAddress = AddressOf(m_pageFaultCoordinatorMemoryStack),
        .CoordinatorMemoryStackSizeBytes = sizeof(m_pageFaultCoordinatorMemoryStack),
        .CoordinatorRseBackingStoreAddress = AddressOf(m_pageFaultCoordinatorRseBackingStore),
        .CoordinatorRseTransitionSizeBytes = Interrupt::Ia64PageFaultCoordinatorRseTransitionSizeBytes,
        .CoordinatorRseBackingStoreSizeBytes = sizeof(m_pageFaultCoordinatorRseBackingStore),
    });
}

Ia64BspRuntimePageFaultEntryError Ia64BspRuntimeMemory::PublishPageFaultEntryContext(
    const Kernel::Scheduler::CpuId processor, Interrupt::Ia64PageFaultEntryControl& control,
    Interrupt::Ia64PageFaultEntryContext& context, Interrupt::Ia64PageFaultFrame& frame) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64BspRuntimePageFaultEntryError::InvalidProcessor;
    }
    const auto reject = [this](const Ia64BspRuntimePageFaultEntryError error,
                               const Ia64RuntimePageFaultPublicationError publicationError =
                                   Ia64RuntimePageFaultPublicationError::None) noexcept
    {
        m_pageFaultEntryError = error;
        m_pageFaultInterruptVectorTableError = publicationError;
        return error;
    };

    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return reject(Ia64BspRuntimePageFaultEntryError::NotReady);
    }
    const Kernel::Scheduler::CpuId bspProcessor{m_summary.BspProcessorIndex};
    if (!bspProcessor.IsValid() || (processor.GetValue() != bspProcessor.GetValue()))
    {
        return reject(Ia64BspRuntimePageFaultEntryError::NotBspProcessor);
    }
    const Foundation::UInt8 processorBit = ProcessorBit(processor);
    if (((m_summary.ProfiledProcessorMask & processorBit) == 0U) ||
        ((m_summary.ResidentProcessorMask & processorBit) == 0U) ||
        ((m_summary.InterruptVectorTable.InstalledProcessorMask & processorBit) == 0U))
    {
        return reject(Ia64BspRuntimePageFaultEntryError::ConfigurationMismatch);
    }

    switch (m_pageFaultEntryState)
    {
    case Ia64BspRuntimePageFaultEntryState::Fresh:
        return reject(Ia64BspRuntimePageFaultEntryError::StorageNotBorrowed);
    case Ia64BspRuntimePageFaultEntryState::Borrowed:
        break;
    case Ia64BspRuntimePageFaultEntryState::Published:
        return reject(Ia64BspRuntimePageFaultEntryError::AlreadyPublished);
    case Ia64BspRuntimePageFaultEntryState::Faulted:
        return reject(Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch);
    }

    if (!PageFaultEntryStorageInvariantHolds())
    {
        return reject(Ia64BspRuntimePageFaultEntryError::StorageInvariantMismatch);
    }
    const Interrupt::Ia64PageFaultEntryControlState controlState =
        Interrupt::AcquireIa64PageFaultEntryControlState(m_pageFaultEntryControl);
    if (controlState == Interrupt::Ia64PageFaultEntryControlState::Fresh)
    {
        if ((&control != &m_pageFaultEntryControl) || (&context != &m_pageFaultEntryContext) ||
            (&frame != &m_pageFaultFrame))
        {
            return reject(Ia64BspRuntimePageFaultEntryError::SubstitutedStorage);
        }
        if (!Interrupt::Ia64PageFaultFrameIsPristine(m_pageFaultFrame))
        {
            return reject(Ia64BspRuntimePageFaultEntryError::FrameNotPristine);
        }
        return reject(Ia64BspRuntimePageFaultEntryError::ControlNotConfigured);
    }
    if (controlState != Interrupt::Ia64PageFaultEntryControlState::Configured)
    {
        m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Faulted;
        return reject(Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch);
    }
    if (Interrupt::ValidateConfiguredIa64PageFaultEntryControl(m_pageFaultEntryControl) !=
        Interrupt::Ia64PageFaultEntryControlError::None)
    {
        m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Faulted;
        const Interrupt::Ia64PageFaultEntryControlState rejectedState =
            Interrupt::AcquireIa64PageFaultEntryControlState(m_pageFaultEntryControl);
        return reject(rejectedState == Interrupt::Ia64PageFaultEntryControlState::Configured
                          ? Ia64BspRuntimePageFaultEntryError::ConfigurationMismatch
                          : Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch);
    }

    const Interrupt::Ia64PageFaultEntryControlSnapshot controlSnapshot =
        Interrupt::AcquireIa64PageFaultEntryControlSnapshot(m_pageFaultEntryControl);
    Foundation::UInt64 expectedMemoryStackEnd{0U};
    Foundation::UInt64 expectedRseBackingStoreEnd{0U};
    Foundation::UInt64 runtimeObjectsEnd{0U};
    const Foundation::UInt64 expectedMemoryStackStart = AddressOf(m_pageFaultCoordinatorMemoryStack);
    const Foundation::UInt64 expectedRseBackingStoreStart = AddressOf(m_pageFaultCoordinatorRseBackingStore);
    if (!TryCalculateEnd(expectedMemoryStackStart, sizeof(m_pageFaultCoordinatorMemoryStack), expectedMemoryStackEnd) ||
        !TryCalculateEnd(expectedRseBackingStoreStart, sizeof(m_pageFaultCoordinatorRseBackingStore),
                         expectedRseBackingStoreEnd) ||
        !TryCalculateEnd(m_summary.RuntimeObjects.VirtualStart, m_summary.RuntimeObjects.SizeBytes, runtimeObjectsEnd))
    {
        m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Faulted;
        return reject(Ia64BspRuntimePageFaultEntryError::StorageInvariantMismatch);
    }
    if (controlSnapshot.State != Interrupt::Ia64PageFaultEntryControlState::Configured)
    {
        m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Faulted;
        return reject(Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch);
    }
    if ((controlSnapshot.Configuration.ProcessorIndex != processor.GetValue()) ||
        (controlSnapshot.Configuration.FrameAddress != AddressOf(&m_pageFaultFrame)) ||
        (controlSnapshot.Configuration.CoordinatorMemoryStackStart != expectedMemoryStackStart) ||
        (controlSnapshot.Configuration.CoordinatorMemoryStackEnd != expectedMemoryStackEnd) ||
        (controlSnapshot.Configuration.CoordinatorRseBackingStoreStart != expectedRseBackingStoreStart) ||
        (controlSnapshot.Configuration.CoordinatorRseBackingStoreEnd != expectedRseBackingStoreEnd) ||
        RangesOverlap(controlSnapshot.Configuration.InterruptedRseBackingStoreStart,
                      controlSnapshot.Configuration.InterruptedRseBackingStoreEnd,
                      m_summary.RuntimeObjects.VirtualStart, runtimeObjectsEnd))
    {
        m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Faulted;
        return reject(Ia64BspRuntimePageFaultEntryError::ConfigurationMismatch);
    }
    if ((&control != &m_pageFaultEntryControl) || (&context != &m_pageFaultEntryContext) ||
        (&frame != &m_pageFaultFrame))
    {
        return reject(Ia64BspRuntimePageFaultEntryError::SubstitutedStorage);
    }
    if (!Interrupt::Ia64PageFaultFrameIsPristine(m_pageFaultFrame))
    {
        return reject(Ia64BspRuntimePageFaultEntryError::FrameNotPristine);
    }

    const Interrupt::Ia64PageFaultEntryContext candidate{
        .ControlAddress = AddressOf(&control),
        .ProcessorIndex = processor.GetValue(),
        .PageNotPresentVectorOffset = Ia64RuntimePageNotPresentOffset,
        .KeyPermissionVectorOffset = Ia64RuntimeKeyPermissionOffset,
        .InstructionAccessRightsVectorOffset = Ia64RuntimeInstructionAccessRightsOffset,
        .DataAccessRightsVectorOffset = Ia64RuntimeDataAccessRightsOffset,
        .Reserved = {},
    };
    const Interrupt::Ia64PageFaultEntryContext previousContext = context;
    context = candidate;
    const Ia64RuntimePageFaultPublicationError publicationError =
        m_interruptVectorTable.PublishPageFaultEntryContext(processor, &context);
    if (publicationError != Ia64RuntimePageFaultPublicationError::None)
    {
        if (publicationError == Ia64RuntimePageFaultPublicationError::AlreadyPublished)
        {
            m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Faulted;
            return reject(Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch, publicationError);
        }
        context = previousContext;
        return reject(Ia64BspRuntimePageFaultEntryError::InterruptVectorTableRejected, publicationError);
    }

    if (!PageFaultEntryContextsAreEqual(context, candidate) ||
        !PageFaultControlStillConfigured(control, controlSnapshot.Configuration) ||
        !Interrupt::Ia64PageFaultFrameIsPristine(frame))
    {
        m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Faulted;
        return reject(Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch);
    }

    m_pageFaultEntryState = Ia64BspRuntimePageFaultEntryState::Published;
    m_pageFaultEntryError = Ia64BspRuntimePageFaultEntryError::None;
    m_pageFaultInterruptVectorTableError = Ia64RuntimePageFaultPublicationError::None;
    return Ia64BspRuntimePageFaultEntryError::None;
}

Ia64BspRuntimePageFaultEntrySnapshot Ia64BspRuntimeMemory::GetPageFaultEntrySnapshot() const noexcept
{
    return Ia64BspRuntimePageFaultEntrySnapshot{
        .Context = m_pageFaultEntryContext,
        .ContextAddress = AddressOf(&m_pageFaultEntryContext),
        .ControlAddress = AddressOf(&m_pageFaultEntryControl),
        .FrameAddress = AddressOf(&m_pageFaultFrame),
        .LastError = m_pageFaultEntryError,
        .LastInterruptVectorTableError = m_pageFaultInterruptVectorTableError,
        .State = m_pageFaultEntryState,
        .Reserved = {},
    };
}

bool Ia64BspRuntimeMemory::HasLockedDataTranslationCoverage(const Foundation::UInt64 rangeStart,
                                                            const Foundation::UInt64 sizeBytes) const noexcept
{
    Foundation::UInt64 rangeEnd{0U};
    if ((m_state != Ia64BspRuntimeMemoryState::Ready) || (m_preparation == nullptr) ||
        !TryCalculateEnd(rangeStart, sizeBytes, rangeEnd) || (rangeEnd > RegionZeroLimit))
    {
        return false;
    }
    const auto plan = m_preparation->TryGetTranslationResourcePlan();
    return plan.HasValue() && (plan.GetValue() != nullptr) && plan.GetValue()->IsBuilt() &&
           HasTranslationRegisterCoverage(*plan.GetValue(), rangeStart, rangeEnd, false);
}

bool Ia64BspRuntimeMemory::HasLockedInstructionTranslationCoverage(const Foundation::UInt64 rangeStart,
                                                                   const Foundation::UInt64 sizeBytes) const noexcept
{
    Foundation::UInt64 rangeEnd{0U};
    if ((m_state != Ia64BspRuntimeMemoryState::Ready) || (m_preparation == nullptr) ||
        !TryCalculateEnd(rangeStart, sizeBytes, rangeEnd) || (rangeEnd > RegionZeroLimit))
    {
        return false;
    }
    const auto plan = m_preparation->TryGetTranslationResourcePlan();
    return plan.HasValue() && (plan.GetValue() != nullptr) && plan.GetValue()->IsBuilt() &&
           HasTranslationRegisterCoverage(*plan.GetValue(), rangeStart, rangeEnd, true);
}

Ia64BspRuntimeExternalInterruptPublicationSnapshot
Ia64BspRuntimeMemory::GetExternalInterruptPublicationSnapshot() const noexcept
{
    Kernel::Scheduler::CpuId processor{m_summary.BspProcessorIndex};
    if (!processor.IsValid())
    {
        processor = Kernel::Scheduler::CpuId{0U};
    }
    const Ia64BspRuntimeExternalInterruptTransportSlot& slot = m_externalInterruptTransportSlots[processor.GetValue()];
    const Foundation::UInt64 sequence = Interrupt::AcquireIa64LocalTimerInterruptEvidenceSequence(slot.Evidence);
    return Ia64BspRuntimeExternalInterruptPublicationSnapshot{
        .Context = slot.Context,
        .Evidence = Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(slot.Evidence, sequence),
        .ContextAddress = AddressOf(&slot.Context),
        .EvidenceAddress = AddressOf(&slot.Evidence),
        .LastError = slot.LastError,
        .LastInterruptVectorTableError = slot.LastInterruptVectorTableError,
        .LastAcknowledgementError = slot.LastAcknowledgementError,
        .Published = slot.Published,
        .Reserved = {},
    };
}

ExternalInterruptPublicationSnapshotResult
Ia64BspRuntimeMemory::GetExternalInterruptPublicationSnapshot(const Kernel::Scheduler::CpuId processor) const noexcept
{
    if (!processor.IsValid())
    {
        return ExternalInterruptPublicationSnapshotResult::Failure(
            Ia64BspRuntimeExternalInterruptAccessError::InvalidProcessor);
    }
    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return ExternalInterruptPublicationSnapshotResult::Failure(
            Ia64BspRuntimeExternalInterruptAccessError::NotReady);
    }
    const Foundation::UInt8 processorBit = ProcessorBit(processor);
    if ((m_summary.ProfiledProcessorMask & processorBit) == 0U)
    {
        return ExternalInterruptPublicationSnapshotResult::Failure(
            Ia64BspRuntimeExternalInterruptAccessError::NotProfiled);
    }
    if (((m_summary.ResidentProcessorMask & processorBit) == 0U) ||
        ((m_summary.InterruptVectorTable.InstalledProcessorMask & processorBit) == 0U))
    {
        return ExternalInterruptPublicationSnapshotResult::Failure(
            Ia64BspRuntimeExternalInterruptAccessError::NotResident);
    }

    const Ia64BspRuntimeExternalInterruptTransportSlot& slot = m_externalInterruptTransportSlots[processor.GetValue()];
    if (!slot.Published)
    {
        return ExternalInterruptPublicationSnapshotResult::Failure(
            Ia64BspRuntimeExternalInterruptAccessError::NotPublished);
    }
    const Foundation::UInt64 externalPreemptionControlAddress =
        __atomic_load_n(&slot.Context.ExternalPreemptionControlAddress, __ATOMIC_ACQUIRE);
    const bool externalPreemptionControlIsValid =
        (externalPreemptionControlAddress == 0U) ||
        ((processor.GetValue() == m_summary.BspProcessorIndex) && m_externalPreemptionStorageBorrowed &&
         (externalPreemptionControlAddress == AddressOf(&m_externalPreemptionControl)));
    if ((slot.Context.EvidenceAddress != AddressOf(&slot.Evidence)) ||
        (slot.Context.ProcessorIndex != processor.GetValue()) ||
        (slot.Context.TimerVector != Interrupt::Ia64ExternalInterruptTimerVector) || !externalPreemptionControlIsValid)
    {
        return ExternalInterruptPublicationSnapshotResult::Failure(
            Ia64BspRuntimeExternalInterruptAccessError::PublicationMismatch);
    }

    const Foundation::UInt64 sequence = Interrupt::AcquireIa64LocalTimerInterruptEvidenceSequence(slot.Evidence);
    return ExternalInterruptPublicationSnapshotResult::Success(Ia64BspRuntimeExternalInterruptPublicationSnapshot{
        .Context = slot.Context,
        .Evidence = Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(slot.Evidence, sequence),
        .ContextAddress = AddressOf(&slot.Context),
        .EvidenceAddress = AddressOf(&slot.Evidence),
        .LastError = slot.LastError,
        .LastInterruptVectorTableError = slot.LastInterruptVectorTableError,
        .LastAcknowledgementError = slot.LastAcknowledgementError,
        .Published = slot.Published,
        .Reserved = {},
    });
}

PublishedExternalInterruptEvidenceResult
Ia64BspRuntimeMemory::GetPublishedExternalInterruptEvidence(const Kernel::Scheduler::CpuId processor) const noexcept
{
    const ExternalInterruptPublicationSnapshotResult snapshot = GetExternalInterruptPublicationSnapshot(processor);
    if (!snapshot.HasValue())
    {
        return PublishedExternalInterruptEvidenceResult::Failure(snapshot.GetError());
    }
    return PublishedExternalInterruptEvidenceResult::Success(
        &m_externalInterruptTransportSlots[processor.GetValue()].Evidence);
}

Ia64BspRuntimeExternalInterruptAcknowledgementError
Ia64BspRuntimeMemory::AcknowledgeExternalInterruptEvidence(const Foundation::UInt64 sequence) noexcept
{
    return AcknowledgeExternalInterruptEvidence(Kernel::Scheduler::CpuId{m_summary.BspProcessorIndex}, sequence);
}

Ia64BspRuntimeExternalInterruptAcknowledgementError
Ia64BspRuntimeMemory::AcknowledgeExternalInterruptEvidence(const Kernel::Scheduler::CpuId processor,
                                                           const Foundation::UInt64 sequence) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64BspRuntimeExternalInterruptAcknowledgementError::InvalidProcessor;
    }

    Ia64BspRuntimeExternalInterruptTransportSlot& slot = m_externalInterruptTransportSlots[processor.GetValue()];
    const auto retain = [&slot](const Ia64BspRuntimeExternalInterruptAcknowledgementError error) noexcept
    {
        slot.LastAcknowledgementError = error;
        return error;
    };

    if (m_state != Ia64BspRuntimeMemoryState::Ready)
    {
        return retain(Ia64BspRuntimeExternalInterruptAcknowledgementError::NotReady);
    }
    const Foundation::UInt8 processorBit = ProcessorBit(processor);
    if ((m_summary.ProfiledProcessorMask & processorBit) == 0U)
    {
        return retain(Ia64BspRuntimeExternalInterruptAcknowledgementError::NotProfiled);
    }
    if (((m_summary.ResidentProcessorMask & processorBit) == 0U) ||
        ((m_summary.InterruptVectorTable.InstalledProcessorMask & processorBit) == 0U))
    {
        return retain(Ia64BspRuntimeExternalInterruptAcknowledgementError::NotResident);
    }
    if (!slot.Published)
    {
        return retain(Ia64BspRuntimeExternalInterruptAcknowledgementError::NotPublished);
    }
    const Foundation::UInt64 externalPreemptionControlAddress =
        __atomic_load_n(&slot.Context.ExternalPreemptionControlAddress, __ATOMIC_ACQUIRE);
    const bool externalPreemptionControlIsValid =
        (externalPreemptionControlAddress == 0U) ||
        ((processor.GetValue() == m_summary.BspProcessorIndex) && m_externalPreemptionStorageBorrowed &&
         (externalPreemptionControlAddress == AddressOf(&m_externalPreemptionControl)));
    if ((slot.Context.EvidenceAddress != AddressOf(&slot.Evidence)) ||
        (slot.Context.ProcessorIndex != processor.GetValue()) ||
        (slot.Context.TimerVector != Interrupt::Ia64ExternalInterruptTimerVector) || !externalPreemptionControlIsValid)
    {
        return retain(Ia64BspRuntimeExternalInterruptAcknowledgementError::PublicationMismatch);
    }

    return retain(MapExternalInterruptAcknowledgementError(
        Interrupt::AcknowledgeIa64LocalTimerInterruptEvidence(slot.Evidence, sequence)));
}

bool Ia64BspRuntimeMemory::ConfigureTranslationCachePurge(const Kernel::Scheduler::CpuId bspProcessor,
                                                          const PalTranslationCachePurgePlan& plan) noexcept
{
    return (m_state == Ia64BspRuntimeMemoryState::Ready) && m_translationCachePurge.Configure(bspProcessor, plan) &&
           m_addressSpacePageTable.ConfigureTranslationCachePurge(m_translationCachePurge) &&
           m_initialComponentAddressSpaceOwner.ConfigureTranslationCachePurge(m_translationCachePurge) &&
           m_pagerAddressSpaceOwner.ConfigureTranslationCachePurge(m_translationCachePurge);
}

Ia64LocalTranslationCachePurgeSnapshot Ia64BspRuntimeMemory::GetTranslationCachePurgeSnapshot() const noexcept
{
    return m_translationCachePurge.GetSnapshot();
}

Kernel::Memory::IAddressSpaceProvider* Ia64BspRuntimeMemory::ConfigureRuntimeAddressSpaces() noexcept
{
    if ((m_state != Ia64BspRuntimeMemoryState::Ready) || m_runtimeAddressSpacesConfigured ||
        (m_preparation == nullptr) ||
        !m_translationCachePurge.CanPurge(Kernel::Scheduler::CpuId{m_summary.BspProcessorIndex}) ||
        !m_runtimeAddressSpaces.Configure(m_processorCapabilities, m_regionIdentifierAllocator, *m_preparation,
                                          m_activeBindings, m_translationCachePurge))
    {
        return nullptr;
    }
    m_runtimeAddressSpacesConfigured = true;
    return &m_runtimeAddressSpaces;
}

Ia64RuntimeAddressSpaceSlotsSnapshot Ia64BspRuntimeMemory::GetRuntimeAddressSpaceSnapshot() const noexcept
{
    return m_runtimeAddressSpaces.GetSnapshot();
}

Foundation::UInt32 Ia64BspRuntimeMemory::GetAllocatedAddressSpaceCount() const noexcept
{
    return m_regionIdentifierAllocator.GetSnapshot().AllocatedAddressSpaceCount;
}

Ia64BspRuntimeMemorySnapshot Ia64BspRuntimeMemory::GetSnapshot() const noexcept
{
    Ia64BspRuntimeMemorySnapshot snapshot{
        .Summary = m_summary,
        .Failure = m_failure,
        .SystemCallPublication = GetSystemCallPublicationSnapshot(),
        .ExternalInterruptPublication = GetExternalInterruptPublicationSnapshot(),
        .PageFaultEntry = GetPageFaultEntrySnapshot(),
        .RegionIdentifierAllocator = m_regionIdentifierAllocator.GetSnapshot(),
        .AddressSpaceStorage = &m_addressSpace,
        .AddressSpace = m_addressSpace.GetSnapshot(),
        .Controller = m_addressSpaceController.GetSnapshot(),
        .Bindings = {},
        .State = m_state,
        .Reserved = {},
    };
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        snapshot.Bindings[processor] = ClassifyBinding(m_activeBindings, Kernel::Scheduler::CpuId{processor});
    }
    return snapshot;
}
} // namespace Asteria::Platform::Ia64::Memory
