// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpacePageTable.hpp"

#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};

[[nodiscard]] bool IsRangeInsideRegionZero(const void* const object, const Foundation::Size sizeBytes) noexcept
{
    const Foundation::UInt64 start = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(object));
    const Foundation::UInt64 size = static_cast<Foundation::UInt64>(sizeBytes);
    return (size != 0U) && (start < RegionZeroLimit) && (size <= (RegionZeroLimit - start));
}

[[nodiscard]] bool IsCanonicalSeedPlan(const Kernel::Memory::InitialMappingPlan& plan) noexcept
{
    if (!plan.IsSealed() || !IsBootstrapMappingCountValid(plan.GetMappingCount()))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
    {
        const auto mappingResult = plan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return false;
        }
        const Kernel::Memory::InitialMapping& mapping = mappingResult.GetValue();
        const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
        const Foundation::UInt64 pages = mapping.Pages.GetValue();
        if ((mapping.Reserved != 0U) || (pages == 0U) || (virtualStart != mapping.PhysicalStart.GetValue()) ||
            !mapping.Permissions.Contains(Kernel::Memory::MappingPermission::Global) ||
            (virtualStart >= RegionZeroLimit) ||
            (pages > ((RegionZeroLimit - virtualStart) / Kernel::Memory::MappingPageSizeBytes)))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr TranslationCapabilities
NormalizeCapabilities(const Ia64CommonRuntimeProcessorCapabilities& common) noexcept
{
    return TranslationCapabilities{
        .InsertablePageSizeMask = common.InsertablePageSizeMask,
        .PhysicalAddressBits = common.PhysicalAddressBits,
        .ImplementedVirtualAddressMostSignificantBit = common.ImplementedVirtualAddressMostSignificantBit,
        .RegionIdentifierBits = common.RegionIdentifierBits,
        .ProtectionKeyBits = common.RegionIdentifierBits,
        .MemoryAttributeMask = common.MemoryAttributeMask,
        .HardwareWalkerImplemented = common.HardwareWalkerImplemented,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr bool FailurePayloadIsClear(const Ia64RuntimeTranslationLedgerFailure& failure) noexcept
{
    return (failure.Reserved[0U] == 0U) && (failure.Reserved[1U] == 0U) && (failure.Reserved[2U] == 0U);
}

[[nodiscard]] constexpr Foundation::ProcessorMask ProcessorBit(const Kernel::Scheduler::CpuId processor) noexcept
{
    return Foundation::ProcessorMask{1U} << processor.GetValue();
}
} // namespace

Ia64RuntimeAddressSpacePageTableInitializationError Ia64RuntimeAddressSpacePageTable::Initialize(
    const Ia64RuntimeProcessorCapabilities& capabilities, const Ia64RegionIdentifierAllocationReceipt& receipt,
    const InitialAddressSpacePreparation& preparation, Ia64ActiveRuntimeTranslationBindings& activeBindings) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != Ia64RuntimeAddressSpacePageTableState::Uninitialized)
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::AlreadyInitialized;
    }
    if (!IsRangeInsideRegionZero(this, sizeof(*this)) || !IsRangeInsideRegionZero(&m_ledger, sizeof(m_ledger)) ||
        !IsRangeInsideRegionZero(&activeBindings, sizeof(activeBindings)) ||
        !IsRangeInsideRegionZero(&preparation, sizeof(preparation)))
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::InvalidPlacement;
    }

    const auto admittedMask = capabilities.GetAdmittedProcessorMask();
    const auto commonCapabilities = capabilities.GetCommonCapabilities();
    if (!admittedMask.HasValue() || !commonCapabilities.HasValue())
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::InvalidCapabilities;
    }
    if (!receipt.IsValid())
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::InvalidReceipt;
    }

    const auto seedResult = preparation.TryGetMappingPlan();
    if (!seedResult.HasValue() || (seedResult.GetValue() == nullptr))
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::InvalidPreparation;
    }
    const Kernel::Memory::InitialMappingPlan& seedPlan = *seedResult.GetValue();
    if (!IsRangeInsideRegionZero(&seedPlan, sizeof(seedPlan)))
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::InvalidPlacement;
    }
    if (!IsCanonicalSeedPlan(seedPlan))
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::InvalidPreparation;
    }

    const TranslationCapabilities normalized = NormalizeCapabilities(commonCapabilities.GetValue());
    const Ia64RuntimeRegionSetError regionSetError = Ia64RuntimeRegionSet::Construct(receipt, normalized, m_regionSet);
    if (regionSetError != Ia64RuntimeRegionSetError::None)
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::RegionSetRejected;
    }

    m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
    const Ia64RuntimeTranslationLedgerFailure ledgerError = m_ledger.Initialize(normalized);
    if ((ledgerError.Code != Ia64RuntimeTranslationLedgerError::None) || !FailurePayloadIsClear(ledgerError) ||
        (ledgerError.EncodingError != Ia64RuntimeTranslationEncodingError::None))
    {
        return Ia64RuntimeAddressSpacePageTableInitializationError::LedgerRejected;
    }

    m_activeBindings = &activeBindings;
    m_pendingSeedPlan = &seedPlan;
    m_owner = nullptr;
    m_admittedProcessorMask = admittedMask.GetValue();
    m_physicalBootstrapCompletedProcessorMask = 0U;
    m_physicalBootstrapAdmissionPendingProcessorMask = 0U;
    m_state = Ia64RuntimeAddressSpacePageTableState::Ready;
    return Ia64RuntimeAddressSpacePageTableInitializationError::None;
}

bool Ia64RuntimeAddressSpacePageTable::IsReady() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state == Ia64RuntimeAddressSpacePageTableState::Ready;
}

bool Ia64RuntimeAddressSpacePageTable::IsCanonicalSeed(const Kernel::Memory::InitialMappingPlan& plan) noexcept
{
    return IsRangeInsideRegionZero(&plan, sizeof(plan)) && IsCanonicalSeedPlan(plan);
}

Ia64RuntimeAddressSpacePageTableState Ia64RuntimeAddressSpacePageTable::GetState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state;
}

bool Ia64RuntimeAddressSpacePageTable::ConfigureTranslationCachePurge(Ia64LocalTranslationCachePurge& purge) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (((m_state != Ia64RuntimeAddressSpacePageTableState::Ready) &&
         (m_state != Ia64RuntimeAddressSpacePageTableState::Claimed)) ||
        (m_translationCachePurge != nullptr))
    {
        return false;
    }
    m_translationCachePurge = &purge;
    return true;
}

bool Ia64RuntimeAddressSpacePageTable::NeedsTranslationCachePurgeLocked(
    const Kernel::Scheduler::CpuId processor) const noexcept
{
    return ((m_ranProcessorMask & ProcessorBit(processor)) != 0U) &&
           (m_ranUnmapEpoch[processor.GetValue()] != m_unmapEpoch);
}

void Ia64RuntimeAddressSpacePageTable::RecordRunLocked(const Kernel::Scheduler::CpuId processor) noexcept
{
    m_ranProcessorMask = static_cast<Foundation::ProcessorMask>(m_ranProcessorMask | ProcessorBit(processor));
    m_ranUnmapEpoch[processor.GetValue()] = m_unmapEpoch;
}

bool Ia64RuntimeAddressSpacePageTable::IsActiveLedgerBound(const Kernel::Scheduler::CpuId processor) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != Ia64RuntimeAddressSpacePageTableState::Claimed) || (m_activeBindings == nullptr))
    {
        return false;
    }

    const auto active = m_activeBindings->AcquireActiveLedger(processor);
    return active.HasValue() && (active.GetValue() == &m_ledger);
}

Hal::Memory::AddressSpacePageTableOwnershipError
Ia64RuntimeAddressSpacePageTable::Claim(const Kernel::Memory::AddressSpace& owner,
                                        const Kernel::Memory::InitialMappingPlan& seedPlan) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == Ia64RuntimeAddressSpacePageTableState::Claimed)
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::AlreadyOwned;
    }
    if (m_state != Ia64RuntimeAddressSpacePageTableState::Ready)
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::NotReady;
    }
    if (!IsRangeInsideRegionZero(&owner, sizeof(owner)))
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::InvalidOwner;
    }
    if (m_pendingSeedPlan != &seedPlan)
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::SeedMismatch;
    }
    m_pendingSeedPlan = nullptr;
    m_owner = &owner;
    m_state = Ia64RuntimeAddressSpacePageTableState::Claimed;
    return Hal::Memory::AddressSpacePageTableOwnershipError::None;
}

Hal::Memory::AddressSpacePageTableOwnershipError
Ia64RuntimeAddressSpacePageTable::Release(const Kernel::Memory::AddressSpace& owner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (((m_state != Ia64RuntimeAddressSpacePageTableState::Claimed) &&
         (m_state != Ia64RuntimeAddressSpacePageTableState::Faulted)) ||
        (m_owner != &owner))
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::InvalidOwner;
    }
    if ((m_activeBindings == nullptr) || m_activeBindings->IsBound(m_ledger))
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::NotReady;
    }
    m_owner = nullptr;
    m_state = Ia64RuntimeAddressSpacePageTableState::Retired;
    return Hal::Memory::AddressSpacePageTableOwnershipError::None;
}

ASTERIA_NOINLINE_NOIPA
Hal::Memory::AddressSpacePageTableActivationError
Ia64RuntimeAddressSpacePageTable::ActivatePhysicalBootstrap(const Kernel::Memory::AddressSpace& owner,
                                                            const Kernel::Scheduler::CpuId processor) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != Ia64RuntimeAddressSpacePageTableState::Claimed)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::NotReady;
    }
    if (m_owner != &owner)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::InvalidOwner;
    }
    if (!processor.IsValid())
    {
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    }
    if ((m_admittedProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    }
    const Foundation::ProcessorMask processorBit = ProcessorBit(processor);
    const bool physicalBootstrapCompleted = (m_physicalBootstrapCompletedProcessorMask & processorBit) != 0U;
    const bool physicalBootstrapAdmissionPending =
        (m_physicalBootstrapAdmissionPendingProcessorMask & processorBit) != 0U;
    if (physicalBootstrapAdmissionPending && !physicalBootstrapCompleted)
    {
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    }
    if (physicalBootstrapCompleted)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    }
    if (m_activeBindings == nullptr)
    {
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    }

    const Ia64ActiveRuntimeTranslationBindingError activation =
        m_activeBindings->ActivateAndBindPhysicalBootstrap(processor, m_regionSet, m_ledger);
    switch (activation)
    {
    case Ia64ActiveRuntimeTranslationBindingError::None:
        RecordRunLocked(processor);
        m_physicalBootstrapCompletedProcessorMask =
            static_cast<Foundation::ProcessorMask>(m_physicalBootstrapCompletedProcessorMask | processorBit);
        m_physicalBootstrapAdmissionPendingProcessorMask =
            static_cast<Foundation::ProcessorMask>(m_physicalBootstrapAdmissionPendingProcessorMask | processorBit);
        return Hal::Memory::AddressSpacePageTableActivationError::None;
    case Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor:
    case Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch:
    case Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit:
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    case Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted:
    case Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding:
    case Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeNotConfigured:
    case Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRuntimeObjectRange:
    case Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRuntimeObjectRange:
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    case Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    case Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    case Ia64ActiveRuntimeTranslationBindingError::ProcessorAlreadyAdmitted:
    case Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger:
    case Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeMismatch:
    case Ia64ActiveRuntimeTranslationBindingError::AdmissionOwnerMismatch:
    default:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    }
}

Hal::Memory::AddressSpacePageTableActivationError
Ia64RuntimeAddressSpacePageTable::Activate(const Kernel::Memory::AddressSpace& owner,
                                           const Kernel::Scheduler::CpuId processor) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != Ia64RuntimeAddressSpacePageTableState::Claimed)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::NotReady;
    }
    if (m_owner != &owner)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::InvalidOwner;
    }
    if (!processor.IsValid())
    {
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    }
    if ((m_admittedProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    }
    const Foundation::ProcessorMask processorBit = ProcessorBit(processor);
    const bool physicalBootstrapCompleted = (m_physicalBootstrapCompletedProcessorMask & processorBit) != 0U;
    const bool physicalBootstrapAdmissionPending =
        (m_physicalBootstrapAdmissionPendingProcessorMask & processorBit) != 0U;
    if (physicalBootstrapAdmissionPending && !physicalBootstrapCompleted)
    {
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    }
    if (m_activeBindings == nullptr)
    {
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    }

    const auto active = m_activeBindings->AcquireActiveLedger(processor);
    if (physicalBootstrapAdmissionPending)
    {
        if (active.HasValue() && (active.GetValue() == &m_ledger))
        {
            m_physicalBootstrapAdmissionPendingProcessorMask =
                static_cast<Foundation::ProcessorMask>(m_physicalBootstrapAdmissionPendingProcessorMask &
                                                       static_cast<Foundation::ProcessorMask>(~processorBit));
            return Hal::Memory::AddressSpacePageTableActivationError::None;
        }
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    }
    if (!active.HasValue())
    {
        switch (active.GetError())
        {
        case Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger:
            break;
        case Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor:
            return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
        case Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted:
        case Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding:
        case Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero:
        case Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRegionZero:
        case Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero:
            return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
        case Ia64ActiveRuntimeTranslationBindingError::None:
        case Ia64ActiveRuntimeTranslationBindingError::ProcessorAlreadyAdmitted:
        case Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet:
        case Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit:
        case Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown:
        case Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch:
        default:
            m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
            return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
        }
    }

    // A stale translation could survive the switch: refuse before any native
    // work when nothing can purge it here.
    const bool purge = NeedsTranslationCachePurgeLocked(processor);
    if (purge && ((m_translationCachePurge == nullptr) || !m_translationCachePurge->CanPurge(processor)))
    {
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    }

    const Ia64ActiveRuntimeTranslationBindingError activation =
        m_activeBindings->ActivateAndBind(processor, m_regionSet, m_ledger);
    switch (activation)
    {
    case Ia64ActiveRuntimeTranslationBindingError::None:
        // The RRs and the ledger are this space's now, so a refill during
        // the purge inserts only translations the ledger still holds.
        if (purge)
        {
            m_translationCachePurge->Purge(processor);
        }
        RecordRunLocked(processor);
        return Hal::Memory::AddressSpacePageTableActivationError::None;
    case Ia64ActiveRuntimeTranslationBindingError::InvalidProcessor:
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    case Ia64ActiveRuntimeTranslationBindingError::ProcessorNotAdmitted:
    case Ia64ActiveRuntimeTranslationBindingError::MalformedBindingEncoding:
    case Ia64ActiveRuntimeTranslationBindingError::BindingTableOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRegionZero:
    case Ia64ActiveRuntimeTranslationBindingError::RuntimeObjectRangeNotConfigured:
    case Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRuntimeObjectRange:
    case Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRuntimeObjectRange:
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    case Ia64ActiveRuntimeTranslationBindingError::InvalidRegionSet:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    case Ia64ActiveRuntimeTranslationBindingError::ActivationRejectedBeforeCommit:
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    case Ia64ActiveRuntimeTranslationBindingError::ActivationStateUnknown:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    case Ia64ActiveRuntimeTranslationBindingError::ProcessorAlreadyAdmitted:
    case Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger:
    case Ia64ActiveRuntimeTranslationBindingError::ActiveLedgerMismatch:
    default:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    }
}

Hal::Memory::AddressSpacePageTableError
Ia64RuntimeAddressSpacePageTable::Unmap(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                        Hal::Memory::AddressSpacePageTableMapResult& output) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == Ia64RuntimeAddressSpacePageTableState::Faulted)
    {
        return Hal::Memory::AddressSpacePageTableError::CorruptState;
    }
    if (m_state != Ia64RuntimeAddressSpacePageTableState::Claimed)
    {
        return Hal::Memory::AddressSpacePageTableError::NotReady;
    }

    // The owner only removes records this backend's Map appended, so any
    // refusal means the ledger no longer holds what the owner recorded.
    const Ia64RuntimeTranslationLedgerFailure failure = m_ledger.Remove(mapping);
    if (!FailurePayloadIsClear(failure) || ((failure.Code == Ia64RuntimeTranslationLedgerError::None) &&
                                            (failure.EncodingError != Ia64RuntimeTranslationEncodingError::None)))
    {
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
    }
    if (failure.Code != Ia64RuntimeTranslationLedgerError::None)
    {
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableError::CorruptState;
    }
    ++m_unmapEpoch;
    output = Hal::Memory::AddressSpacePageTableMapResult{
        .InvalidationStart = mapping.VirtualStart,
        .InvalidationPages = mapping.Pages,
        .Reserved = 0U,
    };
    return Hal::Memory::AddressSpacePageTableError::None;
}

Hal::Memory::AddressSpacePageTableError
Ia64RuntimeAddressSpacePageTable::Map(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                      Hal::Memory::AddressSpacePageTableMapResult& output) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == Ia64RuntimeAddressSpacePageTableState::Faulted)
    {
        return Hal::Memory::AddressSpacePageTableError::CorruptState;
    }
    if (m_state != Ia64RuntimeAddressSpacePageTableState::Claimed)
    {
        return Hal::Memory::AddressSpacePageTableError::NotReady;
    }

    const Ia64RuntimeTranslationLedgerFailure failure = m_ledger.Append(mapping);
    if (!FailurePayloadIsClear(failure))
    {
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
    }
    switch (failure.Code)
    {
    case Ia64RuntimeTranslationLedgerError::None:
        if (failure.EncodingError != Ia64RuntimeTranslationEncodingError::None)
        {
            m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
            return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
        }
        output = Hal::Memory::AddressSpacePageTableMapResult{
            .InvalidationStart = mapping.VirtualStart,
            .InvalidationPages = mapping.Pages,
            .Reserved = 0U,
        };
        return Hal::Memory::AddressSpacePageTableError::None;

    case Ia64RuntimeTranslationLedgerError::CapacityExhausted:
        if (failure.EncodingError != Ia64RuntimeTranslationEncodingError::None)
        {
            m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
            return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
        }
        return Hal::Memory::AddressSpacePageTableError::CapacityExceeded;

    case Ia64RuntimeTranslationLedgerError::OverlappingVirtualRange:
        if (failure.EncodingError != Ia64RuntimeTranslationEncodingError::None)
        {
            m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
            return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
        }
        return Hal::Memory::AddressSpacePageTableError::MappingConflict;

    case Ia64RuntimeTranslationLedgerError::EncodingRejected:
        switch (failure.EncodingError)
        {
        case Ia64RuntimeTranslationEncodingError::InvalidMappingPlan:
        case Ia64RuntimeTranslationEncodingError::MappingTooLarge:
        case Ia64RuntimeTranslationEncodingError::RegionZeroReserved:
        case Ia64RuntimeTranslationEncodingError::CrossRegionMapping:
        case Ia64RuntimeTranslationEncodingError::UnimplementedVirtualAddress:
        case Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange:
        case Ia64RuntimeTranslationEncodingError::UnsupportedPermissions:
        case Ia64RuntimeTranslationEncodingError::DeviceExecutionDenied:
            return Hal::Memory::AddressSpacePageTableError::InvalidPlan;
        case Ia64RuntimeTranslationEncodingError::UnsupportedCachePolicy:
        case Ia64RuntimeTranslationEncodingError::UnsupportedMemoryAttribute:
            return Hal::Memory::AddressSpacePageTableError::UnsupportedConfiguration;
        case Ia64RuntimeTranslationEncodingError::None:
        case Ia64RuntimeTranslationEncodingError::PageIndexOutOfBounds:
        case Ia64RuntimeTranslationEncodingError::InvalidCapabilities:
        case Ia64RuntimeTranslationEncodingError::UnsupportedPageSize:
            m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
            return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
        default:
            m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
            return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
        }

    case Ia64RuntimeTranslationLedgerError::NotInitialized:
    case Ia64RuntimeTranslationLedgerError::AlreadyInitialized:
    case Ia64RuntimeTranslationLedgerError::MappingNotFound:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
    default:
        m_state = Ia64RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
    }
}
} // namespace Asteria::Platform::Ia64::Memory
