// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorTakeoverCoordinator.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
using ClaimError = Ia64ApplicationProcessorTakeoverClaimError;
using ClaimResult = Foundation::Result<const Ia64ApplicationProcessorTakeoverRecord*, ClaimError>;
using CoordinatorState = Ia64ApplicationProcessorTakeoverCoordinatorState;
using Descriptor = Memory::ApplicationProcessorExecutionStorageDescriptor;
using InitializationError = Ia64ApplicationProcessorTakeoverInitializationError;
using Lifecycle = Ia64ApplicationProcessorTakeoverLifecycle;
using ProcessorSnapshot = Ia64ApplicationProcessorTakeoverProcessorSnapshot;
using ProcessorSnapshotResult = Foundation::Result<ProcessorSnapshot, Ia64ApplicationProcessorTakeoverSnapshotError>;
using PublicationError = Ia64ApplicationProcessorTakeoverPublicationError;

constexpr Foundation::UInt64 PublishingPalCapabilitiesState{0x8000'0000'0000'0001ULL};
constexpr Foundation::UInt64 PublishingPalFailureState{0x8000'0000'0000'0002ULL};
constexpr Foundation::UInt64 PublishingInterruptVectorTableFailureState{0x8000'0000'0000'0003ULL};
constexpr Foundation::UInt64 PublishingInitialTranslationFailureState{0x8000'0000'0000'0004ULL};
constexpr Foundation::UInt64 Ia64RegionZeroSizeBytes{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 ExecutionStorageAlignmentBytes{Memory::ApplicationProcessorExecutionStorageAlignmentPages *
                                                            Kernel::Memory::PhysicalPageSizeBytes};

constinit Ia64ApplicationProcessorTakeoverCoordinator g_nativeCoordinator;

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
}

[[nodiscard]] constexpr Lifecycle DecodeLifecycle(const Foundation::UInt64 state) noexcept
{
    switch (state)
    {
    case static_cast<Foundation::UInt64>(Lifecycle::Unused):
        return Lifecycle::Unused;

    case static_cast<Foundation::UInt64>(Lifecycle::Prepared):
        return Lifecycle::Prepared;

    case static_cast<Foundation::UInt64>(Lifecycle::Claimed):
        return Lifecycle::Claimed;

    case static_cast<Foundation::UInt64>(Lifecycle::OwnedPhysical):
    case PublishingPalCapabilitiesState:
    case PublishingPalFailureState:
        return Lifecycle::OwnedPhysical;

    case PublishingInterruptVectorTableFailureState:
        return Lifecycle::Claimed;

    case static_cast<Foundation::UInt64>(Lifecycle::PalCapabilitiesPublished):
    case PublishingInitialTranslationFailureState:
        return Lifecycle::PalCapabilitiesPublished;

    case static_cast<Foundation::UInt64>(Lifecycle::InitialTranslationsInstalled):
        return Lifecycle::InitialTranslationsInstalled;

    case static_cast<Foundation::UInt64>(Lifecycle::Failed):
        return Lifecycle::Failed;
    }
    return Lifecycle::Unused;
}

static_assert(DecodeLifecycle(PublishingPalCapabilitiesState) == Lifecycle::OwnedPhysical);
static_assert(DecodeLifecycle(PublishingPalFailureState) == Lifecycle::OwnedPhysical);
static_assert(DecodeLifecycle(PublishingInterruptVectorTableFailureState) == Lifecycle::Claimed);
static_assert(DecodeLifecycle(PublishingInitialTranslationFailureState) == Lifecycle::PalCapabilitiesPublished);
static_assert(DecodeLifecycle(0xFFFF'FFFF'FFFF'FFFFULL) == Lifecycle::Unused);

[[nodiscard]] constexpr bool AreReservedBytesZero(const Descriptor& descriptor) noexcept
{
    for (const Foundation::UInt8 reserved : descriptor.Reserved)
    {
        if (reserved != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsDescriptorGeometryValid(const Descriptor& descriptor) noexcept
{
    if ((descriptor.AllocationPhysicalStart == 0U) ||
        ((descriptor.AllocationPhysicalStart % ExecutionStorageAlignmentBytes) != 0U) ||
        (descriptor.AllocationPhysicalStart >
         (Ia64RegionZeroSizeBytes - Memory::ApplicationProcessorExecutionStorageSizeBytes)) ||
        (descriptor.AllocationSizeBytes != Memory::ApplicationProcessorExecutionStorageSizeBytes) ||
        (descriptor.MemoryStackPhysicalStart != descriptor.AllocationPhysicalStart) ||
        (descriptor.MemoryStackSizeBytes != Memory::ApplicationProcessorMemoryStackSizeBytes) ||
        (descriptor.RseBackingStoreSizeBytes != Memory::ApplicationProcessorRseBackingStoreSizeBytes))
    {
        return false;
    }

    const Foundation::UInt64 expectedRseStart =
        descriptor.AllocationPhysicalStart + Memory::ApplicationProcessorMemoryStackSizeBytes;
    const Foundation::UInt64 expectedStackPointer =
        expectedRseStart - Memory::ApplicationProcessorInitialStackScratchSizeBytes;
    return (descriptor.InitialStackPointer == expectedStackPointer) &&
           (descriptor.RseBackingStorePhysicalStart == expectedRseStart) && AreReservedBytesZero(descriptor);
}

[[nodiscard]] constexpr Descriptor
MakeGeometryValidationDescriptor(const Foundation::UInt64 allocationPhysicalStart) noexcept
{
    const Foundation::UInt64 rsePhysicalStart =
        allocationPhysicalStart + Memory::ApplicationProcessorMemoryStackSizeBytes;
    return Descriptor{
        .AllocationPhysicalStart = allocationPhysicalStart,
        .AllocationSizeBytes = Memory::ApplicationProcessorExecutionStorageSizeBytes,
        .MemoryStackPhysicalStart = allocationPhysicalStart,
        .InitialStackPointer = rsePhysicalStart - Memory::ApplicationProcessorInitialStackScratchSizeBytes,
        .MemoryStackSizeBytes = Memory::ApplicationProcessorMemoryStackSizeBytes,
        .RseBackingStorePhysicalStart = rsePhysicalStart,
        .RseBackingStoreSizeBytes = Memory::ApplicationProcessorRseBackingStoreSizeBytes,
        .Processor = Kernel::Scheduler::CpuId{1U},
        .HardwareId = 1U,
        .HardwareEid = 1U,
        .Reserved = {},
    };
}

constexpr Foundation::UInt64 LastRegionZeroStorageStart{Ia64RegionZeroSizeBytes -
                                                        Memory::ApplicationProcessorExecutionStorageSizeBytes};
static_assert(IsDescriptorGeometryValid(MakeGeometryValidationDescriptor(LastRegionZeroStorageStart)));
static_assert(!IsDescriptorGeometryValid(MakeGeometryValidationDescriptor(LastRegionZeroStorageStart +
                                                                          ExecutionStorageAlignmentBytes)));

[[nodiscard]] constexpr bool DoAllocationsOverlap(const Descriptor& first, const Descriptor& second) noexcept
{
    const Foundation::UInt64 firstEnd = first.AllocationPhysicalStart + first.AllocationSizeBytes;
    const Foundation::UInt64 secondEnd = second.AllocationPhysicalStart + second.AllocationSizeBytes;
    return (first.AllocationPhysicalStart < secondEnd) && (second.AllocationPhysicalStart < firstEnd);
}

[[nodiscard]] InitializationError
ValidateReadyOwner(const Memory::ApplicationProcessorExecutionStorageOwner& owner,
                   const Memory::ApplicationProcessorExecutionStorageDescriptorView& view) noexcept
{
    const Foundation::UInt8 processorCount = owner.GetProcessorCount();
    if ((processorCount == 0U) || (processorCount > Kernel::Scheduler::MaximumProcessorCount))
    {
        return InitializationError::InvalidOwnerProcessorCount;
    }

    const Foundation::UInt8 expectedMask = static_cast<Foundation::UInt8>(ProcessorBit(processorCount) - 1U);
    if (owner.GetEnabledProcessorMask() != expectedMask)
    {
        return InitializationError::InvalidOwnerProcessorMask;
    }

    const Foundation::Size expectedDescriptorCount = static_cast<Foundation::Size>(processorCount - 1U);
    if ((view.Count != expectedDescriptorCount) || (owner.GetDescriptorCount() != expectedDescriptorCount) ||
        ((expectedDescriptorCount == 0U) && (view.Data != nullptr)) ||
        ((expectedDescriptorCount != 0U) && (view.Data == nullptr)))
    {
        return InitializationError::InvalidOwnerDescriptorView;
    }

    for (Foundation::Size index{0U}; index < view.Count; ++index)
    {
        const Descriptor& descriptor = view.Data[index];
        const Foundation::UInt8 expectedProcessor = static_cast<Foundation::UInt8>(index + 1U);
        if (!descriptor.Processor.IsValid() || (descriptor.Processor.GetValue() != expectedProcessor) ||
            !IsDescriptorGeometryValid(descriptor))
        {
            return InitializationError::InvalidOwnerDescriptor;
        }

        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            const Descriptor& prior = view.Data[previous];
            if (((descriptor.HardwareId == prior.HardwareId) && (descriptor.HardwareEid == prior.HardwareEid)) ||
                DoAllocationsOverlap(descriptor, prior))
            {
                return InitializationError::InvalidOwnerDescriptor;
            }
        }
    }
    return InitializationError::None;
}

[[nodiscard]] constexpr Ia64ApplicationProcessorTakeoverFailure
MakeInterruptVectorTableFailure(const Memory::InitialInterruptVectorTableApInstallStatus status) noexcept
{
    return Ia64ApplicationProcessorTakeoverFailure{
        .Phase = Ia64ApplicationProcessorTakeoverFailurePhase::InterruptVectorTableInstallation,
        .InterruptVectorTableInstallationStatus = status,
        .PalCapabilityCapture = {},
        .InitialTranslationInstallationError = {},
    };
}

[[nodiscard]] constexpr Ia64ApplicationProcessorTakeoverFailure
MakePalCapabilityFailure(const Firmware::PalBootstrapCapabilityCaptureError& failure) noexcept
{
    return Ia64ApplicationProcessorTakeoverFailure{
        .Phase = Ia64ApplicationProcessorTakeoverFailurePhase::PalCapabilityCapture,
        .InterruptVectorTableInstallationStatus = {},
        .PalCapabilityCapture = failure,
        .InitialTranslationInstallationError = {},
    };
}

[[nodiscard]] constexpr Ia64ApplicationProcessorTakeoverFailure
MakeInitialTranslationFailure(const Memory::InitialTranslationInstallError error) noexcept
{
    return Ia64ApplicationProcessorTakeoverFailure{
        .Phase = Ia64ApplicationProcessorTakeoverFailurePhase::InitialTranslationInstallation,
        .InterruptVectorTableInstallationStatus = {},
        .PalCapabilityCapture = {},
        .InitialTranslationInstallationError = error,
    };
}

[[nodiscard]] constexpr bool
IsInterruptVectorTableFailureStatusValid(const Memory::InitialInterruptVectorTableApInstallStatus status) noexcept
{
    switch (status)
    {
    case Memory::InitialInterruptVectorTableApInstallStatus::Installed:
        return false;

    case Memory::InitialInterruptVectorTableApInstallStatus::InvalidSummary:
    case Memory::InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState:
    case Memory::InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch:
        return true;

    default:
        return false;
    }
}

[[nodiscard]] constexpr bool IsPalAdapterFailureValid(const Firmware::PalStaticCallAdapterError error) noexcept
{
    switch (error)
    {
    case Firmware::PalStaticCallAdapterError::None:
        return false;

    case Firmware::PalStaticCallAdapterError::InvalidBinding:
    case Firmware::PalStaticCallAdapterError::UnsupportedProcessorState:
        return true;

    default:
        return false;
    }
}

[[nodiscard]] constexpr bool IsPalVirtualMemoryFailureValid(const Memory::PalVirtualMemoryDecodeError error) noexcept
{
    switch (error)
    {
    case Memory::PalVirtualMemoryDecodeError::None:
        return false;

    case Memory::PalVirtualMemoryDecodeError::SummaryCallFailed:
    case Memory::PalVirtualMemoryDecodeError::PageSizeCallFailed:
    case Memory::PalVirtualMemoryDecodeError::MemoryAttributeCallFailed:
    case Memory::PalVirtualMemoryDecodeError::SummaryReservedResult:
    case Memory::PalVirtualMemoryDecodeError::PageSizeReservedResult:
    case Memory::PalVirtualMemoryDecodeError::MemoryAttributeReservedResult:
    case Memory::PalVirtualMemoryDecodeError::InvalidAddressCapabilities:
    case Memory::PalVirtualMemoryDecodeError::InvalidProtectionCapabilities:
    case Memory::PalVirtualMemoryDecodeError::InvalidTranslationRegisterCapabilities:
    case Memory::PalVirtualMemoryDecodeError::InvalidTranslationCacheTopology:
    case Memory::PalVirtualMemoryDecodeError::MissingMandatoryInsertablePageSize:
    case Memory::PalVirtualMemoryDecodeError::MissingMandatoryPurgePageSize:
    case Memory::PalVirtualMemoryDecodeError::PageSizeBelowArchitectedMinimum:
    case Memory::PalVirtualMemoryDecodeError::InsertablePageSizeNotPurgeable:
    case Memory::PalVirtualMemoryDecodeError::InvalidMemoryAttributeCapabilities:
    case Memory::PalVirtualMemoryDecodeError::MissingRequiredMemoryAttribute:
        return true;

    default:
        return false;
    }
}

[[nodiscard]] constexpr bool
IsPalTranslationCachePurgeFailureValid(const Memory::PalTranslationCachePurgeDecodeError error) noexcept
{
    switch (error)
    {
    case Memory::PalTranslationCachePurgeDecodeError::None:
        return false;

    case Memory::PalTranslationCachePurgeDecodeError::CallFailed:
    case Memory::PalTranslationCachePurgeDecodeError::EmptyPurgeLoop:
    case Memory::PalTranslationCachePurgeDecodeError::OperationLimitExceeded:
    case Memory::PalTranslationCachePurgeDecodeError::AddressComputationOverflow:
        return true;

    default:
        return false;
    }
}

[[nodiscard]] constexpr bool
IsPalCaptureFailureValid(const Firmware::PalBootstrapCapabilityCaptureError& failure) noexcept
{
    using CapturePhase = Firmware::PalBootstrapCapabilityCapturePhase;
    switch (failure.Phase)
    {
    case CapturePhase::None:
        return false;

    case CapturePhase::VirtualMemorySummaryInvocation:
    case CapturePhase::VirtualMemoryPageSizeInvocation:
    case CapturePhase::MemoryAttributeInvocation:
    case CapturePhase::TranslationCachePurgeInvocation:
    case CapturePhase::RseInformationInvocation:
        return IsPalAdapterFailureValid(failure.AdapterError) &&
               (failure.VirtualMemoryError == Memory::PalVirtualMemoryDecodeError::None) &&
               (failure.TranslationCachePurgeError == Memory::PalTranslationCachePurgeDecodeError::None);

    case CapturePhase::VirtualMemoryDecoding:
        return (failure.AdapterError == Firmware::PalStaticCallAdapterError::None) &&
               IsPalVirtualMemoryFailureValid(failure.VirtualMemoryError) &&
               (failure.TranslationCachePurgeError == Memory::PalTranslationCachePurgeDecodeError::None);

    case CapturePhase::TranslationCachePurgeDecoding:
        return (failure.AdapterError == Firmware::PalStaticCallAdapterError::None) &&
               (failure.VirtualMemoryError == Memory::PalVirtualMemoryDecodeError::None) &&
               IsPalTranslationCachePurgeFailureValid(failure.TranslationCachePurgeError);

    case CapturePhase::RseInformationDecoding:
        return (failure.AdapterError == Firmware::PalStaticCallAdapterError::None) &&
               (failure.VirtualMemoryError == Memory::PalVirtualMemoryDecodeError::None) &&
               (failure.TranslationCachePurgeError == Memory::PalTranslationCachePurgeDecodeError::None);

    default:
        return false;
    }
}

[[nodiscard]] constexpr bool
IsInitialTranslationFailureValid(const Memory::InitialTranslationInstallError error) noexcept
{
    switch (error)
    {
    case Memory::InitialTranslationInstallError::None:
        return false;

    case Memory::InitialTranslationInstallError::InvalidPurgePlan:
    case Memory::InitialTranslationInstallError::ResourcePlanNotBuilt:
    case Memory::InitialTranslationInstallError::InvalidCapabilities:
    case Memory::InitialTranslationInstallError::InvalidRegionConfiguration:
    case Memory::InitialTranslationInstallError::InvalidFirmwareTranslation:
    case Memory::InitialTranslationInstallError::InvalidResourceEntry:
    case Memory::InitialTranslationInstallError::OverlappingResourceEntries:
    case Memory::InitialTranslationInstallError::UnsupportedProcessorState:
        return true;

    default:
        return false;
    }
}
} // namespace

InitializationError Ia64ApplicationProcessorTakeoverCoordinator::Initialize(
    const Memory::ApplicationProcessorExecutionStorageOwner& owner) noexcept
{
    if (static_cast<CoordinatorState>(m_state.LoadAcquire()) != CoordinatorState::Uninitialized)
    {
        return InitializationError::AlreadyInitialized;
    }

    const Memory::ApplicationProcessorExecutionStorageOwnerState ownerState = owner.GetState();
    if (ownerState == Memory::ApplicationProcessorExecutionStorageOwnerState::Fresh)
    {
        m_state.StoreRelease(static_cast<Foundation::UInt64>(CoordinatorState::InitializationFailed));
        return InitializationError::OwnerNotReady;
    }
    if (ownerState == Memory::ApplicationProcessorExecutionStorageOwnerState::Failed)
    {
        m_state.StoreRelease(static_cast<Foundation::UInt64>(CoordinatorState::InitializationFailed));
        return InitializationError::OwnerFailed;
    }

    const auto descriptorView = owner.GetDescriptorView();
    if (!descriptorView.HasValue())
    {
        m_state.StoreRelease(static_cast<Foundation::UInt64>(CoordinatorState::InitializationFailed));
        return InitializationError::InvalidOwnerDescriptorView;
    }

    const InitializationError validation = ValidateReadyOwner(owner, descriptorView.GetValue());
    if (validation != InitializationError::None)
    {
        m_state.StoreRelease(static_cast<Foundation::UInt64>(CoordinatorState::InitializationFailed));
        return validation;
    }

    const Memory::ApplicationProcessorExecutionStorageDescriptorView& view = descriptorView.GetValue();
    for (Foundation::Size index{0U}; index < view.Count; ++index)
    {
        const Descriptor& descriptor = view.Data[index];
        const Foundation::UInt8 processor = descriptor.Processor.GetValue();
        m_records[processor] = Ia64ApplicationProcessorTakeoverRecord{
            .InitialStackPointer = descriptor.InitialStackPointer,
            .RseBackingStorePhysicalStart = descriptor.RseBackingStorePhysicalStart,
            .ProcessorValue = processor,
        };
    }
    for (Foundation::UInt8 processor{1U}; processor < owner.GetProcessorCount(); ++processor)
    {
        m_lifecycles[processor].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::Prepared));
    }

    m_processorCount = owner.GetProcessorCount();
    m_enabledProcessorMask = owner.GetEnabledProcessorMask();
    m_state.StoreRelease(static_cast<Foundation::UInt64>(CoordinatorState::Ready));
    return InitializationError::None;
}

ClaimResult Ia64ApplicationProcessorTakeoverCoordinator::Claim(const Kernel::Scheduler::CpuId processor) noexcept
{
    if (static_cast<CoordinatorState>(m_state.LoadAcquire()) != CoordinatorState::Ready)
    {
        return ClaimResult::Failure(ClaimError::CoordinatorNotReady);
    }
    if (!processor.IsValid())
    {
        return ClaimResult::Failure(ClaimError::UnknownProcessor);
    }

    const Foundation::UInt8 processorValue = processor.GetValue();
    if (processorValue == 0U)
    {
        return ClaimResult::Failure(ClaimError::BootstrapProcessor);
    }
    if ((processorValue >= m_processorCount) || ((m_enabledProcessorMask & ProcessorBit(processorValue)) == 0U))
    {
        return ClaimResult::Failure(ClaimError::UnknownProcessor);
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Lifecycle::Prepared);
    if (!m_lifecycles[processorValue].CompareExchangeStrongAcquireRelease(
            expected, static_cast<Foundation::UInt64>(Lifecycle::Claimed)))
    {
        return ClaimResult::Failure(ClaimError::DuplicateClaim);
    }
    return ClaimResult::Success(&m_records[processorValue]);
}

PublicationError Ia64ApplicationProcessorTakeoverCoordinator::ValidatePublicationProcessor(
    const Kernel::Scheduler::CpuId processor, Foundation::UInt8& processorValue) const noexcept
{
    processorValue = 0U;
    if (static_cast<CoordinatorState>(m_state.LoadAcquire()) != CoordinatorState::Ready)
    {
        return PublicationError::CoordinatorNotReady;
    }
    if (!processor.IsValid())
    {
        return PublicationError::UnknownProcessor;
    }

    processorValue = processor.GetValue();
    if (processorValue == 0U)
    {
        return PublicationError::BootstrapProcessor;
    }
    if ((processorValue >= m_processorCount) || ((m_enabledProcessorMask & ProcessorBit(processorValue)) == 0U))
    {
        return PublicationError::UnknownProcessor;
    }
    return PublicationError::None;
}

PublicationError Ia64ApplicationProcessorTakeoverCoordinator::ReservePublication(
    const Kernel::Scheduler::CpuId processor, const Lifecycle precedingLifecycle,
    const Foundation::UInt64 publishingState, Foundation::UInt8& processorValue) noexcept
{
    const PublicationError validation = ValidatePublicationProcessor(processor, processorValue);
    if (validation != PublicationError::None)
    {
        return validation;
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(precedingLifecycle);
    if (!m_lifecycles[processorValue].CompareExchangeStrongAcquireRelease(expected, publishingState))
    {
        return PublicationError::InvalidPrecedingState;
    }
    return PublicationError::None;
}

PublicationError
Ia64ApplicationProcessorTakeoverCoordinator::PublishOwnedPhysical(const Kernel::Scheduler::CpuId processor) noexcept
{
    Foundation::UInt8 processorValue{0U};
    const PublicationError validation = ValidatePublicationProcessor(processor, processorValue);
    if (validation != PublicationError::None)
    {
        return validation;
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Lifecycle::Claimed);
    if (!m_lifecycles[processorValue].CompareExchangeStrongAcquireRelease(
            expected, static_cast<Foundation::UInt64>(Lifecycle::OwnedPhysical)))
    {
        return PublicationError::InvalidPrecedingState;
    }
    return PublicationError::None;
}

PublicationError Ia64ApplicationProcessorTakeoverCoordinator::PublishPalCaptureSuccess(
    const Kernel::Scheduler::CpuId processor, const Firmware::PalBootstrapCapabilities& capabilities) noexcept
{
    Foundation::UInt8 processorValue{0U};
    const PublicationError reservation =
        ReservePublication(processor, Lifecycle::OwnedPhysical, PublishingPalCapabilitiesState, processorValue);
    if (reservation != PublicationError::None)
    {
        return reservation;
    }

    m_palCapabilities[processorValue] = capabilities;
    m_lifecycles[processorValue].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::PalCapabilitiesPublished));
    return PublicationError::None;
}

PublicationError Ia64ApplicationProcessorTakeoverCoordinator::PublishInterruptVectorTableFailure(
    const Kernel::Scheduler::CpuId processor, const Memory::InitialInterruptVectorTableApInstallStatus status) noexcept
{
    if (!IsInterruptVectorTableFailureStatusValid(status))
    {
        return PublicationError::InvalidFailurePayload;
    }

    Foundation::UInt8 processorValue{0U};
    const PublicationError validation = ValidatePublicationProcessor(processor, processorValue);
    if (validation != PublicationError::None)
    {
        return validation;
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Lifecycle::Claimed);
    if (!m_lifecycles[processorValue].CompareExchangeStrongAcquireRelease(expected,
                                                                          PublishingInterruptVectorTableFailureState))
    {
        return PublicationError::InvalidPrecedingState;
    }

    m_failures[processorValue] = MakeInterruptVectorTableFailure(status);
    m_lifecycles[processorValue].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::Failed));
    return PublicationError::None;
}

PublicationError Ia64ApplicationProcessorTakeoverCoordinator::PublishPalCaptureFailure(
    const Kernel::Scheduler::CpuId processor, const Firmware::PalBootstrapCapabilityCaptureError& failure) noexcept
{
    if (!IsPalCaptureFailureValid(failure))
    {
        return PublicationError::InvalidFailurePayload;
    }

    Foundation::UInt8 processorValue{0U};
    const PublicationError reservation =
        ReservePublication(processor, Lifecycle::OwnedPhysical, PublishingPalFailureState, processorValue);
    if (reservation != PublicationError::None)
    {
        return reservation;
    }

    m_failures[processorValue] = MakePalCapabilityFailure(failure);
    m_lifecycles[processorValue].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::Failed));
    return PublicationError::None;
}

PublicationError Ia64ApplicationProcessorTakeoverCoordinator::PublishInitialTranslationInstallSuccess(
    const Kernel::Scheduler::CpuId processor) noexcept
{
    Foundation::UInt8 processorValue{0U};
    const PublicationError validation = ValidatePublicationProcessor(processor, processorValue);
    if (validation != PublicationError::None)
    {
        return validation;
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Lifecycle::PalCapabilitiesPublished);
    if (!m_lifecycles[processorValue].CompareExchangeStrongAcquireRelease(
            expected, static_cast<Foundation::UInt64>(Lifecycle::InitialTranslationsInstalled)))
    {
        return PublicationError::InvalidPrecedingState;
    }
    return PublicationError::None;
}

PublicationError Ia64ApplicationProcessorTakeoverCoordinator::PublishInitialTranslationInstallFailure(
    const Kernel::Scheduler::CpuId processor, const Memory::InitialTranslationInstallError error) noexcept
{
    if (!IsInitialTranslationFailureValid(error))
    {
        return PublicationError::InvalidFailurePayload;
    }

    Foundation::UInt8 processorValue{0U};
    const PublicationError reservation = ReservePublication(processor, Lifecycle::PalCapabilitiesPublished,
                                                            PublishingInitialTranslationFailureState, processorValue);
    if (reservation != PublicationError::None)
    {
        return reservation;
    }

    m_failures[processorValue] = MakeInitialTranslationFailure(error);
    m_lifecycles[processorValue].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::Failed));
    return PublicationError::None;
}

Ia64ApplicationProcessorTakeoverProgressSnapshot
Ia64ApplicationProcessorTakeoverCoordinator::GetProgressSnapshot() const noexcept
{
    Ia64ApplicationProcessorTakeoverProgressSnapshot snapshot{};
    const CoordinatorState state = static_cast<CoordinatorState>(m_state.LoadAcquire());
    snapshot.State = state;
    if (state != CoordinatorState::Ready)
    {
        return snapshot;
    }

    snapshot.ProcessorCount = m_processorCount;
    snapshot.EnabledProcessorMask = m_enabledProcessorMask;
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const Lifecycle lifecycle = DecodeLifecycle(m_lifecycles[processor].LoadAcquire());
        snapshot.Processors[processor] = lifecycle;
        if ((lifecycle == Lifecycle::InitialTranslationsInstalled) || (lifecycle == Lifecycle::Failed))
        {
            snapshot.TerminalMask = static_cast<Foundation::UInt8>(snapshot.TerminalMask | ProcessorBit(processor));
        }
        if (lifecycle == Lifecycle::Failed)
        {
            snapshot.FailedMask = static_cast<Foundation::UInt8>(snapshot.FailedMask | ProcessorBit(processor));
        }
    }
    return snapshot;
}

ProcessorSnapshotResult Ia64ApplicationProcessorTakeoverCoordinator::GetProcessorSnapshot(
    const Kernel::Scheduler::CpuId processor) const noexcept
{
    if (static_cast<CoordinatorState>(m_state.LoadAcquire()) != CoordinatorState::Ready)
    {
        return ProcessorSnapshotResult::Failure(Ia64ApplicationProcessorTakeoverSnapshotError::CoordinatorNotReady);
    }
    if (!processor.IsValid())
    {
        return ProcessorSnapshotResult::Failure(Ia64ApplicationProcessorTakeoverSnapshotError::InvalidProcessorSlot);
    }

    const Foundation::UInt8 processorValue = processor.GetValue();
    const Lifecycle lifecycle = DecodeLifecycle(m_lifecycles[processorValue].LoadAcquire());
    ProcessorSnapshot snapshot{};
    snapshot.Takeover = m_records[processorValue];
    snapshot.Lifecycle = lifecycle;
    if ((lifecycle == Lifecycle::PalCapabilitiesPublished) || (lifecycle == Lifecycle::InitialTranslationsInstalled))
    {
        snapshot.PalCapabilities = m_palCapabilities[processorValue];
    }
    else if (lifecycle == Lifecycle::Failed)
    {
        snapshot.Failure = m_failures[processorValue];
        if (snapshot.Failure.Phase == Ia64ApplicationProcessorTakeoverFailurePhase::InitialTranslationInstallation)
        {
            snapshot.PalCapabilities = m_palCapabilities[processorValue];
        }
    }
    return ProcessorSnapshotResult::Success(snapshot);
}

Ia64ApplicationProcessorTakeoverCoordinator& GetNativeIa64ApplicationProcessorTakeoverCoordinator() noexcept
{
    return g_nativeCoordinator;
}
} // namespace Asteria::Platform::Ia64::Interrupt
