// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Scheduler/Ia64ApplicationProcessorKernelRuntime.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"
#include "Platform/Ia64/Diagnostics/Ia64StopStage.hpp"

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
void* operator new(Asteria::Foundation::Size sizeBytes, void* storage) noexcept;
#else
#include <new>
#endif

extern "C" void AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence(
    Asteria::Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeRegisterEvidence* evidence) noexcept;

extern "C" void
AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor(Asteria::Foundation::UInt64* descriptor) noexcept;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority() noexcept;

extern "C" [[noreturn]] void AsteriaIa64ApplicationProcessorKernelRuntimeWorkerEntry(void* argument) noexcept;

// The target entry points are in Ia64ApplicationProcessorKernelRuntime.S.
#if !defined(__ia64__)
extern "C" void AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence(
    Asteria::Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeRegisterEvidence* const evidence) noexcept
{
    if (evidence != nullptr)
    {
        *evidence = Asteria::Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeRegisterEvidence{
            .StackPointer = 0U,
            .RseBackingStorePointer = 0U,
            .RseBackingStoreBase = 0U,
            .RseNatCollection = 0U,
            .RseConfiguration = 0U,
            .ProcessorStatus = 0U,
            .CaptureComplete = 1U,
            .Reserved = {},
        };
    }
}

extern "C" void
AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor(Asteria::Foundation::UInt64* const descriptor) noexcept
{
    if (descriptor != nullptr)
    {
        descriptor[0U] = static_cast<Asteria::Foundation::UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(
            &AsteriaIa64ApplicationProcessorKernelRuntimeWorkerEntry));
        descriptor[1U] = 0U;
    }
}
#endif

namespace Asteria::Platform::Ia64::Scheduler
{
namespace
{
using AccessError = Ia64ApplicationProcessorKernelRuntimeAccessError;
using Coordinator = Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinator;
using CoordinatorSnapshot = Interrupt::Ia64ApplicationProcessorRuntimeReleaseSnapshot;
using CoordinatorState = Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinatorState;
using Error = Ia64ApplicationProcessorKernelRuntimeError;
using Failure = Ia64ApplicationProcessorKernelRuntimeFailure;
using FailurePoll = Ia64ApplicationProcessorKernelRuntimeFailurePoll;
using Lifecycle = Ia64ApplicationProcessorKernelRuntimeProcessorState;
using Operation = Ia64ApplicationProcessorKernelRuntimeOperation;
using PhysicalFailure = Ia64ApplicationProcessorKernelRuntimePhysicalFailure;
using PollRange = Interrupt::Ia64ExternalInterruptPollCodeRange;
using PollStatus = Interrupt::Ia64ExternalInterruptPollStatus;
using PortableFailure = Ia64ApplicationProcessorPortableSchedulerFailure;
using PortablePhase = Ia64ApplicationProcessorPortableSchedulerPhase;
using ServiceRequest = Ia64ApplicationProcessorPortableSchedulerServiceRequest;
using ServiceResult = Ia64ApplicationProcessorPortableSchedulerServiceResult;
using ServiceState = Ia64ApplicationProcessorPortableSchedulerServiceState;
using TimerDeliveryRequest = Ia64ApplicationProcessorTimerDeliveryRequest;
using TimerDeliveryResult = Ia64ApplicationProcessorTimerDeliveryResult;
using TimerDeliveryState = Ia64ApplicationProcessorTimerDeliveryState;
using ProofPhase = Ia64ApplicationProcessorLocalTimerProofPhase;
using PreparationResult = Foundation::Result<Ia64ApplicationProcessorKernelRuntimeSummary, Failure>;
using ReleaseError = Interrupt::Ia64ApplicationProcessorRuntimeTransitionError;
using ReleaseStage = Interrupt::Ia64ApplicationProcessorRuntimeStage;
using Runtime = Ia64ApplicationProcessorKernelRuntime;
using TimerError = Hal::Time::LocalTimerError;
using TimerInitializationError = Time::Ia64LocalTimerInitializationError;
using TimerState = Time::Ia64LocalTimerState;
using TlbDrainSummary = Kernel::Memory::TlbShootdownDrainSummary;
using TlbPollResult = Memory::Ia64PolledTlbShootdownPollResult;
using ValidationError = Interrupt::Ia64LocalTimerInterruptEvidenceValidationError;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt8 MinimumPhysicalAddressBits{32U};
constexpr Foundation::UInt8 MaximumPhysicalAddressBits{63U};
constexpr Foundation::UInt8 MaximumImplementedVirtualAddressBit{60U};
constexpr Foundation::UInt8 InvalidProcessorValue{0xFFU};
constexpr Foundation::UInt64 TranslationStateMask{(Foundation::UInt64{1U} << 36U) | (Foundation::UInt64{1U} << 27U) |
                                                  (Foundation::UInt64{1U} << 17U)};
constexpr Foundation::UInt64 InstructionCollectionMask{Foundation::UInt64{1U} << 13U};
constexpr Foundation::UInt64 InterruptionEnableMask{Foundation::UInt64{1U} << 14U};
// Interval-time bounds of the AP timer proofs, in ITC ticks (the emulator's Montecito ITC runs at
// 400 MHz: 10'000'000 ticks are 25 ms). The emulator runs every CPU in turn on one host thread, 100
// ms each, and derives ITC from host time, so an AP can lose hundreds of milliseconds, and more on
// a loaded host, anywhere between sampling ITC and programming cr.itm. A lead that elapsed in that
// window proves nothing about the timer: - the local-timer proof samples again and doubles its
// lead, up to LocalTimerArmAttemptLimit attempts (25 ms to 3.2 s at 400 MHz); - the timer delivery
// arms its Timer once, as its evidence records, so its lead covers several round-robin rotations
// outright (1 s at 400 MHz). The poll grace after a deadline covers the emulator raising the
// interrupt and the AP's next turn: 10 s at 400 MHz, a hundred times what they take, yet a lost
// interrupt is reported well before the BSP's wait for this AP (Ia64ApWaitMinimumTicks, twice as
// long) gives up without the AP's record.
constexpr Foundation::UInt64 LocalTimerDeadlineLeadTicks{10'000'000U};
constexpr Foundation::Size LocalTimerArmAttemptLimit{8U};
constexpr Foundation::UInt64 TimerDeliveryDeadlineLeadTicks{400'000'000U};
constexpr Foundation::UInt64 LocalTimerPollGraceTicks{4'000'000'000U};
constexpr Foundation::UInt64 LocalTimerTaskPriority{0xD0U};
constexpr PortableFailure EmptyPortableFailure{};
constexpr Ia64ApplicationProcessorTimerDeliveryRecord EmptyTimerDeliveryRecord{};

[[nodiscard]] constexpr TlbDrainSummary EmptyTlbDrainSummary() noexcept
{
    return TlbDrainSummary{
        .ReceivedCount = 0U,
        .CompletedCount = 0U,
        .RejectedCount = 0U,
        .ReceiveError = Kernel::Memory::TlbShootdownReceiveError::None,
        .FirstRemoteError = Kernel::Memory::TlbShootdownError::None,
        .FirstLocalError = Hal::Memory::LocalTlbInvalidationError::None,
        .FirstAddressSpaceError = Kernel::Memory::AddressSpaceError::None,
        .Processor = Kernel::Scheduler::CpuId{},
        .Reserved = {},
    };
}

[[nodiscard]] constexpr Foundation::UInt64 EncodeTlbDrainFailureDetail(const TlbDrainSummary& summary) noexcept
{
    return static_cast<Foundation::UInt64>(summary.ReceivedCount) |
           (static_cast<Foundation::UInt64>(summary.CompletedCount) << 8U) |
           (static_cast<Foundation::UInt64>(summary.RejectedCount) << 16U) |
           (static_cast<Foundation::UInt64>(summary.ReceiveError) << 24U) |
           (static_cast<Foundation::UInt64>(summary.FirstRemoteError) << 32U) |
           (static_cast<Foundation::UInt64>(summary.FirstLocalError) << 40U) |
           (static_cast<Foundation::UInt64>(summary.FirstAddressSpaceError) << 48U) |
           (static_cast<Foundation::UInt64>(summary.Processor.GetValue()) << 56U);
}

ASTERIA_NOINLINE_NOIPA void ClearPortableFailure(PortableFailure& failure) noexcept
{
    failure = EmptyPortableFailure;
}

[[nodiscard]] constexpr bool ObjectIdMatches(const Kernel::Object::ObjectId first,
                                             const Kernel::Object::ObjectId second) noexcept
{
    return first.GetValue() == second.GetValue();
}

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
}

[[nodiscard]] constexpr bool TryGetExclusiveEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                                Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool IsAligned(const Foundation::UInt64 value, const Foundation::UInt64 alignment) noexcept
{
    return (alignment != 0U) && ((value & (alignment - 1U)) == 0U);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumImplementedVirtualAddressBit)
    {
        return true;
    }
    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumImplementedVirtualAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] constexpr bool ReservedBytesAreZero(const Foundation::UInt8* const bytes,
                                                  const Foundation::Size count) noexcept
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

[[nodiscard]] constexpr bool
FixedRootAddressSpaceMatches(const Kernel::Memory::AddressSpaceSnapshot& snapshot,
                             const Memory::Ia64BspRuntimeRootView& root,
                             const Foundation::ProcessorMask expectedProcessorMask) noexcept
{
    if (!snapshot.Identity.IsEqualTo(root.AddressSpaceIdentity) ||
        (snapshot.ResidentProcessorMask != expectedProcessorMask) || (snapshot.TransitionSequence != 0U) ||
        snapshot.ActivatingProcessor.IsValid() || snapshot.TransitionProcessor.IsValid() ||
        !ReservedBytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved)))
    {
        return false;
    }
    if (snapshot.State == Kernel::Memory::AddressSpaceState::Ready)
    {
        return (snapshot.ShootdownSequence == 0U) && (snapshot.TargetProcessorMask == 0U) &&
               (snapshot.AcknowledgedProcessorMask == 0U);
    }
    return (snapshot.State == Kernel::Memory::AddressSpaceState::ShootdownPending) &&
           (snapshot.ShootdownSequence != 0U) && (snapshot.TargetProcessorMask == expectedProcessorMask) &&
           ((snapshot.AcknowledgedProcessorMask & ~expectedProcessorMask) == 0U);
}

[[nodiscard]] constexpr bool
EvidenceIsZero(const Interrupt::Ia64ApplicationProcessorRuntimeServingEvidence& evidence) noexcept
{
    return (evidence.RuntimeEntryValue == 0U) && (evidence.FirstWorkerEntryValue == 0U) &&
           (evidence.ResumedWorkerEntryValue == 0U) && (evidence.ServingIdleValue == 0U) &&
           ReservedBytesAreZero(evidence.Reserved, sizeof(evidence.Reserved));
}

[[nodiscard]] constexpr bool
LocalTimerEvidenceIsZero(const Interrupt::Ia64LocalTimerInterruptEvidence& evidence) noexcept
{
    return (evidence.Sequence == 0U) && (evidence.ProgrammedDeadlineTicks == 0U) &&
           (evidence.ObservedIntervalTimeCounter == 0U) && (evidence.InterruptedInstructionPointer == 0U) &&
           (evidence.InterruptedProcessorStatus == 0U) && (evidence.InterruptedIfsValid == 0U) &&
           (evidence.ProcessorIndex == 0U) && (evidence.Vector == 0U) &&
           ReservedBytesAreZero(evidence.Reserved, sizeof(evidence.Reserved)) && (evidence.AcknowledgedSequence == 0U);
}

[[nodiscard]] constexpr bool
AcknowledgedTimerDeliveryGateIsValid(const Interrupt::Ia64LocalTimerInterruptEvidence& evidence) noexcept
{
    return (evidence.Sequence == 1U) && (evidence.AcknowledgedSequence == 1U) &&
           (evidence.ProgrammedDeadlineTicks == 0U) && (evidence.ObservedIntervalTimeCounter == 0U) &&
           (evidence.InterruptedInstructionPointer == 0U) && (evidence.InterruptedProcessorStatus == 0U) &&
           (evidence.InterruptedIfsValid == 0U) && (evidence.ProcessorIndex == 0U) && (evidence.Vector == 0U) &&
           ReservedBytesAreZero(evidence.Reserved, sizeof(evidence.Reserved));
}

[[nodiscard]] constexpr bool
AcknowledgedTimerDeliveryCompletionIsValid(const Interrupt::Ia64LocalTimerInterruptEvidence& evidence) noexcept
{
    return (evidence.Sequence == 2U) && (evidence.AcknowledgedSequence == 2U) &&
           (evidence.ProgrammedDeadlineTicks == 0U) && (evidence.ObservedIntervalTimeCounter == 0U) &&
           (evidence.InterruptedInstructionPointer == 0U) && (evidence.InterruptedProcessorStatus == 0U) &&
           (evidence.InterruptedIfsValid == 0U) && (evidence.ProcessorIndex == 0U) && (evidence.Vector == 0U) &&
           ReservedBytesAreZero(evidence.Reserved, sizeof(evidence.Reserved));
}

[[nodiscard]] constexpr bool FailureIsZero(const Interrupt::Ia64ApplicationProcessorRuntimeFault& failure) noexcept
{
    return (failure.ExpectedGeneration == 0U) && (failure.ObservedGeneration == 0U) &&
           (failure.Error == ReleaseError::None) &&
           (failure.Operation == Interrupt::Ia64ApplicationProcessorRuntimeFaultOperation::None) &&
           (failure.ExpectedStage == ReleaseStage::Unused) && (failure.ObservedStage == ReleaseStage::Unused) &&
           (failure.ProcessorValue == 0U) && ReservedBytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

[[nodiscard]] constexpr bool
IsCanonicalUnusedLayout(const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
{
    return (layout.SlotVirtualBase == 0U) && (layout.IdlePhysicalBase == 0U) && (layout.WorkerPhysicalBase == 0U) &&
           !layout.Processor.IsValid() && (layout.Processor.GetValue() == InvalidProcessorValue) &&
           ReservedBytesAreZero(layout.Reserved, sizeof(layout.Reserved));
}

[[nodiscard]] constexpr Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] Foundation::UInt8* AliasPointer(const Foundation::UInt64 address) noexcept
{
    return reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(address));
}
} // namespace

namespace Detail
{
bool FixedRootControllerMatches(const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot& snapshot,
                                const Memory::Ia64BspRuntimeRootView& root,
                                const Kernel::Scheduler::CpuId processor) noexcept
{
    return (snapshot.State == Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) &&
           snapshot.Processor.IsValid() && (snapshot.Processor.GetValue() == processor.GetValue()) &&
           (snapshot.IdleStorage == root.AddressSpaceStorage) && (snapshot.ActiveStorage == root.AddressSpaceStorage) &&
           (snapshot.PreviousStorage == nullptr) && snapshot.Idle.IsEqualTo(root.AddressSpaceIdentity) &&
           snapshot.Active.IsEqualTo(root.AddressSpaceIdentity) && !snapshot.Previous.IsValid() &&
           !snapshot.SourceTransition.IsValid() && !snapshot.DestinationTransition.IsValid() &&
           ReservedBytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

bool TimerDeliveryPublicationIsCoherent(const Foundation::UInt64 completionSequence, const Foundation::UInt64 state,
                                        const Foundation::UInt64 requestSequence,
                                        const Foundation::UInt64 result) noexcept
{
    constexpr Foundation::UInt64 Ready{static_cast<Foundation::UInt64>(TimerDeliveryState::Ready)};
    constexpr Foundation::UInt64 Publishing{static_cast<Foundation::UInt64>(TimerDeliveryState::Publishing)};
    constexpr Foundation::UInt64 Pending{static_cast<Foundation::UInt64>(TimerDeliveryState::Pending)};
    constexpr Foundation::UInt64 Executing{static_cast<Foundation::UInt64>(TimerDeliveryState::Executing)};
    constexpr Foundation::UInt64 Completed{static_cast<Foundation::UInt64>(TimerDeliveryState::Completed)};
    constexpr Foundation::UInt64 Failed{static_cast<Foundation::UInt64>(TimerDeliveryState::Failed)};
    constexpr Foundation::UInt64 NoResult{static_cast<Foundation::UInt64>(TimerDeliveryResult::None)};
    constexpr Foundation::UInt64 Delivered{static_cast<Foundation::UInt64>(TimerDeliveryResult::Delivered)};

    const bool knownState = state <= Failed;
    const bool knownResult = (result == NoResult) || (result == Delivered);
    const bool coherentIncompletePublication =
        (completionSequence == 0U) && (result == NoResult) &&
        (((state == Ready) && (requestSequence == 0U)) || ((state == Publishing) && (requestSequence <= 1U)) ||
         (((state == Pending) || (state == Executing)) && (requestSequence == 1U)) ||
         ((state == Failed) && (requestSequence <= 1U)));
    if (!knownState || !knownResult || ((completionSequence == 0U) && !coherentIncompletePublication) ||
        ((completionSequence == 1U) && ((requestSequence != 1U) || (state != Completed) || (result != Delivered))) ||
        (completionSequence > 1U))
    {
        return false;
    }
    return true;
}
} // namespace Detail

Ia64ApplicationProcessorKernelRuntimeState Runtime::GetState() const noexcept
{
    const Foundation::UInt64 state = m_state.LoadAcquire();
    switch (state)
    {
    case static_cast<Foundation::UInt64>(Ia64ApplicationProcessorKernelRuntimeState::Fresh):
        return Ia64ApplicationProcessorKernelRuntimeState::Fresh;

    case static_cast<Foundation::UInt64>(Ia64ApplicationProcessorKernelRuntimeState::Ready):
        return Ia64ApplicationProcessorKernelRuntimeState::Ready;

    case static_cast<Foundation::UInt64>(Ia64ApplicationProcessorKernelRuntimeState::Failed):
        return Ia64ApplicationProcessorKernelRuntimeState::Failed;
    }
    return Ia64ApplicationProcessorKernelRuntimeState::Failed;
}

Lifecycle Runtime::DecodeProcessorState(const Foundation::UInt64 state) const noexcept
{
    switch (state)
    {
    case static_cast<Foundation::UInt64>(Lifecycle::Unused):
        return Lifecycle::Unused;

    case static_cast<Foundation::UInt64>(Lifecycle::AwaitingRelease):
    case PublishingPhysicalClaim:
        return Lifecycle::AwaitingRelease;

    case static_cast<Foundation::UInt64>(Lifecycle::PhysicalClaimed):
    case PublishingRuntimeEntry:
        return Lifecycle::PhysicalClaimed;

    case static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered):
    case DispatchingPortableWorker:
    case PublishingWorkerEntry:
    case PublishingSchedulerWaiting:
        return Lifecycle::RuntimeEntered;

    case static_cast<Foundation::UInt64>(Lifecycle::SchedulerWaiting):
    case PublishingIdleServing:
        return Lifecycle::SchedulerWaiting;

    case static_cast<Foundation::UInt64>(Lifecycle::IdleServing):
        return Lifecycle::IdleServing;

    case static_cast<Foundation::UInt64>(Lifecycle::Failed):
        return Lifecycle::Failed;
    }
    return Lifecycle::Failed;
}

ProofPhase Runtime::DecodeLocalTimerProofPhase(const Foundation::UInt64 state) const noexcept
{
    switch (state)
    {
    case static_cast<Foundation::UInt64>(ProofPhase::Unconfigured):
    case PublishingLocalTimerConfiguration:
        return ProofPhase::Unconfigured;

    case static_cast<Foundation::UInt64>(ProofPhase::Configured):
    case PublishingLocalTimerArm:
        return ProofPhase::Configured;

    case static_cast<Foundation::UInt64>(ProofPhase::Armed):
    case PublishingLocalTimerCompletion:
        return ProofPhase::Armed;

    case static_cast<Foundation::UInt64>(ProofPhase::Completed):
        return ProofPhase::Completed;

    case static_cast<Foundation::UInt64>(ProofPhase::Failed):
        return ProofPhase::Failed;
    }
    return ProofPhase::Failed;
}

PortablePhase Runtime::DecodePortableSchedulerPhase(const Foundation::UInt64 state) const noexcept
{
    switch (state)
    {
    case static_cast<Foundation::UInt64>(PortablePhase::Unconfigured):
    case PublishingPortableSchedulerPreparation:
        return PortablePhase::Unconfigured;

    case static_cast<Foundation::UInt64>(PortablePhase::Prepared):
    case PublishingPortableSchedulerDispatch:
        return PortablePhase::Prepared;

    case static_cast<Foundation::UInt64>(PortablePhase::Starting):
    case PublishingPortableSchedulerWaiting:
        return PortablePhase::Starting;

    case static_cast<Foundation::UInt64>(PortablePhase::Waiting):
    case PublishingPortableSchedulerOnline:
        return PortablePhase::Waiting;

    case static_cast<Foundation::UInt64>(PortablePhase::Online):
        return PortablePhase::Online;

    case static_cast<Foundation::UInt64>(PortablePhase::Failed):
        return PortablePhase::Failed;
    }
    return PortablePhase::Failed;
}

ServiceState Runtime::DecodePortableSchedulerServiceState(const Foundation::UInt64 state) const noexcept
{
    switch (state)
    {
    case ServiceMailboxIdle:
        return ServiceState::Idle;

    case ServiceMailboxPublishing:
        return ServiceState::Publishing;

    case ServiceMailboxPending:
        return ServiceState::Pending;

    case ServiceMailboxExecuting:
        return ServiceState::Executing;

    case ServiceMailboxFailed:
        return ServiceState::Failed;
    }
    return ServiceState::Failed;
}

TimerDeliveryState Runtime::DecodeTimerDeliveryState(const Foundation::UInt64 state) const noexcept
{
    // Detail::TimerDeliveryPublicationIsCoherent validates these words
    // through the public encodings.
    static_assert(TimerDeliveryReady == static_cast<Foundation::UInt64>(TimerDeliveryState::Ready));
    static_assert(TimerDeliveryPublishing == static_cast<Foundation::UInt64>(TimerDeliveryState::Publishing));
    static_assert(TimerDeliveryPending == static_cast<Foundation::UInt64>(TimerDeliveryState::Pending));
    static_assert(TimerDeliveryExecuting == static_cast<Foundation::UInt64>(TimerDeliveryState::Executing));
    static_assert(TimerDeliveryCompleted == static_cast<Foundation::UInt64>(TimerDeliveryState::Completed));
    static_assert(TimerDeliveryFailed == static_cast<Foundation::UInt64>(TimerDeliveryState::Failed));
    switch (state)
    {
    case TimerDeliveryReady:
        return TimerDeliveryState::Ready;

    case TimerDeliveryPublishing:
        return TimerDeliveryState::Publishing;

    case TimerDeliveryPending:
        return TimerDeliveryState::Pending;

    case TimerDeliveryExecuting:
        return TimerDeliveryState::Executing;

    case TimerDeliveryCompleted:
        return TimerDeliveryState::Completed;

    case TimerDeliveryFailed:
        return TimerDeliveryState::Failed;
    }
    return TimerDeliveryState::Failed;
}

bool Runtime::ValidateLocalTimerEvidenceStorage(
    const Interrupt::Ia64LocalTimerInterruptEvidence* const evidence) const noexcept
{
    if (evidence == nullptr)
    {
        return false;
    }
    const Foundation::UInt64 address = AddressOf(evidence);
    constexpr Foundation::UInt64 sizeBytes = sizeof(Interrupt::Ia64LocalTimerInterruptEvidence);
    Foundation::UInt64 end{0U};
    return IsAligned(address, alignof(Interrupt::Ia64LocalTimerInterruptEvidence)) &&
           TryGetExclusiveEnd(address, sizeBytes, end) && (end <= Interrupt::Ia64ExternalInterruptRegionZeroLimit) &&
           LocalTimerEvidenceIsZero(*evidence);
}

bool Runtime::RegistrationMatches(Kernel::Object::ObjectRegistry& registry, const Kernel::Object::ObjectId identity,
                                  const Kernel::Object::ObjectType type, Kernel::Object::IKernelObject& storage,
                                  Kernel::Object::ObjectPinError& pinError) noexcept
{
    Kernel::Object::ObjectPin pin;
    pinError = registry.PinObject(identity, type, pin);
    if (pinError != Kernel::Object::ObjectPinError::Ok)
    {
        return false;
    }
    const bool matches = ObjectIdMatches(pin.GetObjectId(), identity) && (&pin.GetObject() == &storage);
    pin.Reset();
    return matches;
}

bool Runtime::ValidateResourceDomain(Kernel::Object::ObjectRegistry& registry,
                                     const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                     Kernel::Resource::ResourceDomain& resourceDomain,
                                     Kernel::Object::ObjectPinError& pinError) noexcept
{
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry))
    {
        return false;
    }
    const Kernel::Resource::ResourceDomainSnapshot snapshot = resourceDomain.GetSnapshot();
    return (snapshot.State == Kernel::Resource::ResourceDomainState::Ready) && snapshot.Identity.IsValid() &&
           (snapshot.Identity.GetValue() == resourceDomain.GetIdentifier().GetValue()) && (snapshot.Depth == 0U) &&
           !snapshot.Parent.IsValid() &&
           RegistrationMatches(registry, resourceDomainIdentity.GetObjectId(),
                               Kernel::Object::ObjectType::ResourceDomain, resourceDomain, pinError);
}

Ia64ApplicationProcessorKernelRuntimeSummary Runtime::MakeSummary() const noexcept
{
    return Ia64ApplicationProcessorKernelRuntimeSummary{
        .WorkerEntryPoint = m_workerEntryDescriptor[0U],
        .WorkerEntryGlobalPointer = m_workerEntryDescriptor[1U],
        .ProcessorCount = m_plan.ProcessorCount,
        .EnabledProcessorMask = m_plan.EnabledProcessorMask,
        .PreparedContextCount = m_preparedContextCount,
        .CommonPhysicalAddressBits = m_commonPhysicalAddressBits,
        .CommonImplementedVirtualAddressMostSignificantBit = m_commonImplementedVirtualAddressMostSignificantBit,
        .State = GetState(),
        .Reserved = {},
    };
}

Failure Runtime::MakeFailure(const Error error, const Operation operation, const Foundation::UInt8 processorValue,
                             const Foundation::UInt64 generation, const Lifecycle observedState,
                             const ReleaseError releaseError, const Ia64KernelContextError contextError,
                             const Foundation::UInt64 detail) const noexcept
{
    return Failure{
        .Generation = generation,
        .Detail = detail,
        .Error = error,
        .Operation = operation,
        .ObservedState = observedState,
        .ReleaseError = releaseError,
        .ContextError = contextError,
        .ProcessorValue = processorValue,
        .Reserved = {},
    };
}

void Runtime::PublishFirstFailure(const Failure& failure) noexcept
{
    Foundation::UInt64 expected{0U};
    if (!m_firstFailurePublication.CompareExchangeStrongAcquireRelease(expected, PublishingFailure))
    {
        return;
    }
    m_firstFailure = failure;
    m_firstFailurePublication.StoreRelease(PublishedFailure);
}

void Runtime::PublishPortableSchedulerFailureSnapshot(const Foundation::Size index) noexcept
{
    Synchronization::AtomicUInt64& publication = m_portableSchedulerFailurePublicationSequences[index];
    const Foundation::UInt64 sequence = publication.LoadAcquire();
    if ((sequence & 1U) != 0U)
    {
        return;
    }
    if (sequence == (MaximumUInt64 - 1U))
    {
        publication.StoreRelease(MaximumUInt64);
        return;
    }

    // The published type is trivially copyable, UInt8 is unsigned char, and
    // clearing padding makes every byte consumed by the word packing defined.
    ASTERIA_CLEAR_PADDING(&m_portableSchedulerFailures[index]);
    publication.StoreRelease(sequence + 1U);
    const auto* const source = reinterpret_cast<const Foundation::UInt8*>(&m_portableSchedulerFailures[index]);
    for (Foundation::Size wordIndex{0U}; wordIndex < PortableSchedulerFailureWordCount; ++wordIndex)
    {
        Foundation::UInt64 word{0U};
        const Foundation::Size offset = wordIndex * sizeof(Foundation::UInt64);
        for (Foundation::Size byteIndex{0U}; byteIndex < sizeof(Foundation::UInt64); ++byteIndex)
        {
            word |= static_cast<Foundation::UInt64>(source[offset + byteIndex]) << (byteIndex * 8U);
        }
        m_portableSchedulerFailureWords[index][wordIndex].StoreRelease(word);
    }
    publication.StoreRelease(sequence + 2U);
}

Error Runtime::FailProcessor(const Foundation::Size index, const Error error, const Operation operation,
                             const ReleaseError releaseError, const Ia64KernelContextError contextError,
                             const Foundation::UInt64 detail) noexcept
{
    const Foundation::UInt64 observed = m_processorStates[index].LoadAcquire();
    const Failure failure =
        MakeFailure(error, operation, static_cast<Foundation::UInt8>(index + 1U), m_generations[index],
                    DecodeProcessorState(observed), releaseError, contextError, detail);
    m_serviceMailboxStates[index].StoreRelease(ServiceMailboxFailed);
    if (m_timerDeliveryStates[index].LoadAcquire() != TimerDeliveryCompleted)
    {
        m_timerDeliveryStates[index].StoreRelease(TimerDeliveryFailed);
    }
    m_idleWorkOwners[index].StoreRelease(IdleWorkFailed);
    m_processorStates[index].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::Failed));
    PublishPortableSchedulerFailureSnapshot(index);
    PublishFirstFailure(failure);
    return error;
}

Error Runtime::FailLocalTimerProof(const Foundation::Size index, const Error error, const Operation operation,
                                   const Foundation::UInt64 detail) noexcept
{
    m_localTimerProofStates[index].StoreRelease(static_cast<Foundation::UInt64>(ProofPhase::Failed));
    return FailProcessor(index, error, operation, ReleaseError::None, Ia64KernelContextError::None, detail);
}

PreparationResult Runtime::FailPreparation(const Error error, const Foundation::UInt8 processorValue,
                                           const ReleaseError releaseError, const Ia64KernelContextError contextError,
                                           const Foundation::UInt64 detail) noexcept
{
    const Failure failure =
        MakeFailure(error, Operation::Prepare, processorValue,
                    processorValue == InvalidProcessorValue ? Foundation::UInt64{0U}
                                                            : static_cast<Foundation::UInt64>(processorValue),
                    Lifecycle::Unused, releaseError, contextError, detail);
    PublishFirstFailure(failure);
    m_state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ApplicationProcessorKernelRuntimeState::Failed));
    return PreparationResult::Failure(failure);
}

bool Runtime::ValidatePlan(const Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& plan,
                           const Foundation::UInt8 commonPhysicalAddressBits,
                           const Foundation::UInt8 commonImplementedVirtualAddressMostSignificantBit) const noexcept
{
    if ((plan.ProcessorCount == 0U) || (plan.ProcessorCount > Kernel::Scheduler::MaximumProcessorCount) ||
        (commonPhysicalAddressBits < MinimumPhysicalAddressBits) ||
        (commonPhysicalAddressBits > MaximumPhysicalAddressBits) ||
        (commonImplementedVirtualAddressMostSignificantBit <
         Memory::Ia64ApplicationProcessorRuntimeMinimumVirtualAddressMostSignificantBit) ||
        (commonImplementedVirtualAddressMostSignificantBit > MaximumImplementedVirtualAddressBit) ||
        !ReservedBytesAreZero(plan.Reserved, sizeof(plan.Reserved)))
    {
        return false;
    }

    const Foundation::UInt8 expectedMask = static_cast<Foundation::UInt8>(ProcessorBit(plan.ProcessorCount) - 1U);
    const Foundation::Size expectedLayoutCount = static_cast<Foundation::Size>(plan.ProcessorCount - 1U);
    if ((plan.EnabledProcessorMask != expectedMask) || (plan.LayoutCount != expectedLayoutCount))
    {
        return false;
    }

    const Foundation::UInt8 encoderBits =
        commonPhysicalAddressBits < Memory::Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits
            ? commonPhysicalAddressBits
            : Memory::Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits;
    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << encoderBits;
    constexpr Foundation::UInt64 idleAlignment =
        Memory::ApplicationProcessorExecutionStorageAlignmentPages * Kernel::Memory::PhysicalPageSizeBytes;
    constexpr Foundation::UInt64 workerAlignment =
        Memory::Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages * Kernel::Memory::PhysicalPageSizeBytes;

    for (Foundation::Size index{0U}; index < Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount; ++index)
    {
        const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& layout = plan.Processors[index];
        if (index >= expectedLayoutCount)
        {
            if (!IsCanonicalUnusedLayout(layout))
            {
                return false;
            }
            continue;
        }

        const Foundation::UInt64 expectedSlot =
            Memory::Ia64ApplicationProcessorRuntimeRegionOneBase +
            (static_cast<Foundation::UInt64>(index) * Memory::Ia64ApplicationProcessorRuntimeProcessorStride);
        Foundation::UInt64 slotEnd{0U};
        Foundation::UInt64 idleEnd{0U};
        Foundation::UInt64 workerEnd{0U};
        if ((layout.SlotVirtualBase != expectedSlot) || !layout.Processor.IsValid() ||
            (layout.Processor.GetValue() != static_cast<Foundation::UInt8>(index + 1U)) ||
            !ReservedBytesAreZero(layout.Reserved, sizeof(layout.Reserved)) ||
            !IsAligned(layout.IdlePhysicalBase, idleAlignment) ||
            !IsAligned(layout.WorkerPhysicalBase, workerAlignment) ||
            !TryGetExclusiveEnd(layout.SlotVirtualBase, Memory::Ia64ApplicationProcessorRuntimeProcessorStride,
                                slotEnd) ||
            !TryGetExclusiveEnd(layout.IdlePhysicalBase, Memory::Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes,
                                idleEnd) ||
            !TryGetExclusiveEnd(layout.WorkerPhysicalBase, Memory::Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes,
                                workerEnd) ||
            (layout.IdlePhysicalBase == 0U) || (layout.WorkerPhysicalBase == 0U) || (idleEnd > physicalLimit) ||
            (workerEnd > physicalLimit) ||
            RangesOverlap(layout.IdlePhysicalBase, idleEnd, layout.WorkerPhysicalBase, workerEnd) ||
            !IsVirtualAddressImplemented(layout.SlotVirtualBase, commonImplementedVirtualAddressMostSignificantBit) ||
            !IsVirtualAddressImplemented(slotEnd - 1U, commonImplementedVirtualAddressMostSignificantBit))
        {
            return false;
        }

        if ((index != 0U) &&
            (layout.WorkerPhysicalBase !=
             plan.Processors[0U].WorkerPhysicalBase + (static_cast<Foundation::UInt64>(index) *
                                                       Memory::Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes)))
        {
            return false;
        }

        for (Foundation::Size prior{0U}; prior < index; ++prior)
        {
            const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& priorLayout = plan.Processors[prior];
            const Foundation::UInt64 priorIdleEnd =
                priorLayout.IdlePhysicalBase + Memory::Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes;
            const Foundation::UInt64 priorWorkerEnd =
                priorLayout.WorkerPhysicalBase + Memory::Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes;
            if (RangesOverlap(layout.IdlePhysicalBase, idleEnd, priorLayout.IdlePhysicalBase, priorIdleEnd) ||
                RangesOverlap(layout.IdlePhysicalBase, idleEnd, priorLayout.WorkerPhysicalBase, priorWorkerEnd) ||
                RangesOverlap(layout.WorkerPhysicalBase, workerEnd, priorLayout.IdlePhysicalBase, priorIdleEnd) ||
                RangesOverlap(layout.WorkerPhysicalBase, workerEnd, priorLayout.WorkerPhysicalBase, priorWorkerEnd))
            {
                return false;
            }
        }
    }
    return true;
}

bool Runtime::CaptureAndValidateReleaseCoordinator(Coordinator& coordinator) noexcept
{
    m_releasePreparationSnapshot = coordinator.GetSnapshot();
    const CoordinatorSnapshot& snapshot = m_releasePreparationSnapshot;
    const CoordinatorState expectedState =
        m_plan.ProcessorCount == 1U ? CoordinatorState::AllServing : CoordinatorState::Ready;
    if ((snapshot.State != expectedState) || (snapshot.ProcessorCount != m_plan.ProcessorCount) ||
        (snapshot.EnabledProcessorMask != m_plan.EnabledProcessorMask) || (snapshot.ReleasedProcessorMask != 0U) ||
        (snapshot.OnlineProcessorMask != 0U) || (snapshot.ObservedProcessorMask != 0U) ||
        (snapshot.ActiveGeneration != 0U) || snapshot.ActiveProcessor.IsValid() ||
        (snapshot.ActiveProcessor.GetValue() != InvalidProcessorValue) || !FailureIsZero(snapshot.Fault) ||
        !ReservedBytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved)))
    {
        return false;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const Interrupt::Ia64ApplicationProcessorRuntimeProcessorSnapshot& processorSnapshot =
            snapshot.Processors[processor];
        const ReleaseStage expectedStage = ((processor != 0U) && (processor < snapshot.ProcessorCount))
                                               ? ReleaseStage::AwaitingRelease
                                               : ReleaseStage::Unused;
        if ((processorSnapshot.Stage != expectedStage) || (processorSnapshot.Generation != 0U) ||
            processorSnapshot.ServingObserved || !EvidenceIsZero(processorSnapshot.Evidence) ||
            !ReservedBytesAreZero(processorSnapshot.Reserved, sizeof(processorSnapshot.Reserved)))
        {
            return false;
        }
    }
    return true;
}

Ia64KernelContextError Runtime::PrepareWorkerContext(const Foundation::Size index) noexcept
{
    const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    const Foundation::UInt64 workerStackStart =
        Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerAliasVirtualStart(layout);
    const Foundation::UInt64 workerRseStart =
        Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout);
    const WritableIa64KernelStorageView memoryStack{
        .Data = AliasPointer(workerStackStart),
        .SizeBytes = Memory::Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes,
        .VirtualStart = workerStackStart,
    };
    const WritableIa64KernelStorageView rseBackingStore{
        .Data = AliasPointer(workerRseStart),
        .SizeBytes = Memory::Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes,
        .VirtualStart = workerRseStart,
    };

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    const Ia64KernelContextEntry workerEntry = reinterpret_cast<Ia64KernelContextEntry>(
        static_cast<Foundation::UIntPointer>(AddressOf(m_workerEntryDescriptor)));
#else
    const Ia64KernelContextEntry workerEntry{&AsteriaIa64ApplicationProcessorKernelRuntimeWorkerEntry};
#endif
    return m_workerContexts[index].Prepare(m_commonImplementedVirtualAddressMostSignificantBit, memoryStack,
                                           rseBackingStore, workerEntry, &m_workerArguments[index]);
}

PreparationResult Runtime::Prepare(const Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& plan,
                                   const Foundation::UInt8 commonPhysicalAddressBits,
                                   const Foundation::UInt8 commonImplementedVirtualAddressMostSignificantBit,
                                   Coordinator& coordinator) noexcept
{
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Fresh)
    {
        return PreparationResult::Failure(MakeFailure(Error::AlreadyPrepared, Operation::Prepare, InvalidProcessorValue,
                                                      0U, Lifecycle::Unused, ReleaseError::None,
                                                      Ia64KernelContextError::None, 0U));
    }
    if ((commonPhysicalAddressBits < MinimumPhysicalAddressBits) ||
        (commonPhysicalAddressBits > MaximumPhysicalAddressBits) ||
        (commonImplementedVirtualAddressMostSignificantBit <
         Memory::Ia64ApplicationProcessorRuntimeMinimumVirtualAddressMostSignificantBit) ||
        (commonImplementedVirtualAddressMostSignificantBit > MaximumImplementedVirtualAddressBit))
    {
        return FailPreparation(Error::InvalidAddressProfile, InvalidProcessorValue, ReleaseError::None,
                               Ia64KernelContextError::None, 0U);
    }
    if (!ValidatePlan(plan, commonPhysicalAddressBits, commonImplementedVirtualAddressMostSignificantBit))
    {
        return FailPreparation(Error::InvalidLayout, InvalidProcessorValue, ReleaseError::None,
                               Ia64KernelContextError::None, 0U);
    }

    m_plan = plan;
    m_commonPhysicalAddressBits = commonPhysicalAddressBits;
    m_commonImplementedVirtualAddressMostSignificantBit = commonImplementedVirtualAddressMostSignificantBit;
    if (!CaptureAndValidateReleaseCoordinator(coordinator))
    {
        return FailPreparation(Error::ReleaseCoordinatorRejected, InvalidProcessorValue,
                               ReleaseError::CoordinatorNotReady, Ia64KernelContextError::None,
                               static_cast<Foundation::UInt64>(m_releasePreparationSnapshot.State));
    }
    m_releaseCoordinator = &coordinator;

    if (m_plan.LayoutCount == 0U)
    {
        m_state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ApplicationProcessorKernelRuntimeState::Ready));
        return PreparationResult::Success(MakeSummary());
    }

    AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor(m_workerEntryDescriptor);
    if (m_workerEntryDescriptor[0U] == 0U)
    {
        return FailPreparation(Error::WorkerContextRejected, 1U, ReleaseError::None,
                               Ia64KernelContextError::InvalidEntry, 0U);
    }

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        const Foundation::UInt8 processorValue = static_cast<Foundation::UInt8>(index + 1U);
        m_generations[index] = processorValue;
        m_workerArguments[index] = WorkerArgument{
            .Owner = this,
            .Generation = processorValue,
            .Processor = Kernel::Scheduler::CpuId{processorValue},
            .Reserved = {},
        };
        const Ia64KernelContextError contextError = PrepareWorkerContext(index);
        if (contextError != Ia64KernelContextError::None)
        {
            return FailPreparation(Error::WorkerContextRejected, processorValue, ReleaseError::None, contextError, 0U);
        }
        m_workerContextSnapshots[index] = m_workerContexts[index].GetSnapshot();
        ++m_preparedContextCount;
    }

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        m_processorStates[index].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::AwaitingRelease));
    }
    m_state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ApplicationProcessorKernelRuntimeState::Ready));
    return PreparationResult::Success(MakeSummary());
}

Error Runtime::ConfigurePortableSchedulerDependencies(
    Memory::Ia64BspRuntimeMemory& runtimeMemory, Kernel::Object::ObjectRegistry& objectRegistry,
    const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain, Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return Error::RuntimeNotReady;
    }
    if (m_portableSchedulerConfiguration.LoadAcquire() == PortableSchedulerDependenciesReady)
    {
        return Error::PortableSchedulerAlreadyConfigured;
    }

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        if (m_processorStates[index].LoadAcquire() != static_cast<Foundation::UInt64>(Lifecycle::AwaitingRelease))
        {
            return Error::PortableSchedulerDependenciesRejected;
        }
    }

    Kernel::Object::ObjectPinError pinError{Kernel::Object::ObjectPinError::Ok};
    if (!issuer.IsFromRegistry(objectRegistry) ||
        !ValidateResourceDomain(objectRegistry, resourceDomainIdentity, resourceDomain, pinError))
    {
        if (m_plan.LayoutCount != 0U)
        {
            m_portableSchedulerFailures[0U].ObjectPin = pinError;
            PublishPortableSchedulerFailureSnapshot(0U);
        }
        return Error::PortableSchedulerDependenciesRejected;
    }

    const Foundation::Size requiredObjectSlots = m_plan.LayoutCount * PortableSchedulerObjectsPerProcessor;
    const Foundation::Size activeObjectCount = objectRegistry.GetActiveCount();
    const Foundation::Size registryCapacity = objectRegistry.GetCapacity();
    if ((requiredObjectSlots > registryCapacity) || (activeObjectCount > (registryCapacity - requiredObjectSlots)))
    {
        return Error::PortableSchedulerObjectCapacityRejected;
    }

    const auto borrowed = runtimeMemory.BorrowReadyRoot();
    if (!borrowed.HasValue())
    {
        return Error::PortableSchedulerRootBorrowRejected;
    }
    const Memory::Ia64BspRuntimeRootView& root = borrowed.GetValue();
    if ((root.AddressSpaceStorage == nullptr) || !root.AddressSpaceIdentity.IsValid() ||
        (root.ControllerStorage == nullptr) || !root.Processor.IsValid())
    {
        return Error::PortableSchedulerRootRejected;
    }
    const Kernel::Memory::AddressSpaceSnapshot addressSpace = root.AddressSpaceStorage->GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controller = root.ControllerStorage->GetSnapshot();
    if ((addressSpace.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !addressSpace.Identity.IsEqualTo(root.AddressSpaceIdentity) ||
        (controller.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        (controller.Processor.GetValue() != root.Processor.GetValue()) ||
        (controller.IdleStorage != root.AddressSpaceStorage) ||
        (controller.ActiveStorage != root.AddressSpaceStorage) ||
        !controller.Idle.IsEqualTo(root.AddressSpaceIdentity) ||
        !controller.Active.IsEqualTo(root.AddressSpaceIdentity))
    {
        return Error::PortableSchedulerRootRejected;
    }

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        ClearPortableFailure(m_portableSchedulerFailures[index]);
        PublishPortableSchedulerFailureSnapshot(index);
    }
    m_runtimeMemory = &runtimeMemory;
    m_objectRegistry = &objectRegistry;
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_resourceDomain = &resourceDomain;
    m_issuer = &issuer;
    m_expectedSharedRoot = root;
    m_portableSchedulerConfiguration.StoreRelease(PortableSchedulerDependenciesReady);
    return Error::None;
}

Error Runtime::ConfigureTlbShootdownDependencies(
    Memory::Ia64PolledTlbShootdownNotifier& notifier, Kernel::Memory::TlbShootdownMailboxTransport& mailbox,
    const Memory::PalVirtualMemoryCapabilities (
        &processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount]) noexcept
{
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return Error::RuntimeNotReady;
    }
    if (m_tlbShootdownConfiguration.LoadAcquire() == TlbShootdownDependenciesReady)
    {
        return Error::TlbShootdownAlreadyConfigured;
    }

    for (Foundation::Size index{0U}; index < Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount; ++index)
    {
        const Foundation::UInt64 expectedState = index < m_plan.LayoutCount
                                                     ? static_cast<Foundation::UInt64>(Lifecycle::AwaitingRelease)
                                                     : static_cast<Foundation::UInt64>(Lifecycle::Unused);
        if (m_processorStates[index].LoadAcquire() != expectedState)
        {
            return Error::TlbShootdownDependenciesRejected;
        }
    }

    const Memory::Ia64PolledTlbShootdownNotifierSnapshot notifierSnapshot = notifier.GetSnapshot();
    if ((notifierSnapshot.State != Memory::Ia64PolledTlbShootdownNotifierState::Ready) ||
        (notifierSnapshot.ProfiledProcessorMask != m_plan.EnabledProcessorMask) ||
        (notifierSnapshot.PendingProcessorMask != 0U) || (notifierSnapshot.Reserved != 0U) ||
        (mailbox.GetOnlineProcessorMask() != m_plan.EnabledProcessorMask))
    {
        return Error::TlbShootdownDependenciesRejected;
    }

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        const Foundation::UInt8 processor = m_plan.Processors[index].Processor.GetValue();
        Memory::Ia64LocalTlbInvalidator validationInvalidator;
        if (validationInvalidator.Initialize(processorCapabilities[processor]) !=
            Memory::Ia64LocalTlbInvalidatorInitializationError::None)
        {
            return Error::TlbShootdownDependenciesRejected;
        }
    }

    for (Foundation::Size index{0U}; index < m_plan.LayoutCount; ++index)
    {
        const Foundation::UInt8 processor = m_plan.Processors[index].Processor.GetValue();
        m_tlbShootdownCapabilities[index] = processorCapabilities[processor];
    }
    m_tlbShootdownNotifier = &notifier;
    m_tlbShootdownMailbox = &mailbox;
    m_tlbShootdownConfiguration.StoreRelease(TlbShootdownDependenciesReady);
    return Error::None;
}

AccessError Runtime::GetSummary(Ia64ApplicationProcessorKernelRuntimeSummary& output) const noexcept
{
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return AccessError::NotReady;
    }
    output = MakeSummary();
    return AccessError::None;
}

AccessError Runtime::GetPlan(Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& output) const noexcept
{
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return AccessError::NotReady;
    }
    output = m_plan;
    return AccessError::None;
}

bool Runtime::TryGetProcessorIndex(const Kernel::Scheduler::CpuId processor, Foundation::Size& index) const noexcept
{
    index = 0U;
    if (!processor.IsValid() || (processor.GetValue() == 0U))
    {
        return false;
    }
    const Foundation::Size candidate = static_cast<Foundation::Size>(processor.GetValue() - 1U);
    if ((candidate >= m_plan.LayoutCount) ||
        (m_plan.Processors[candidate].Processor.GetValue() != processor.GetValue()))
    {
        return false;
    }
    index = candidate;
    return true;
}

Error Runtime::ValidateProcessorGeneration(const Kernel::Scheduler::CpuId processor,
                                           const Foundation::UInt64 generation, Foundation::Size& index,
                                           const Operation operation) noexcept
{
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return Error::RuntimeNotReady;
    }
    if (!TryGetProcessorIndex(processor, index))
    {
        const Failure failure = MakeFailure(Error::InvalidProcessor, operation, processor.GetValue(), generation,
                                            Lifecycle::Unused, ReleaseError::None, Ia64KernelContextError::None, 0U);
        PublishFirstFailure(failure);
        return Error::InvalidProcessor;
    }
    if (generation != m_generations[index])
    {
        // Generation rejection is terminal for the resolved processor slot.
        // If its timer transaction already armed, settle only that retained
        // slot-owned token before publishing the original InvalidGeneration;
        // no caller-supplied object identity participates in cleanup.
        if (((operation == Operation::BeginTimerDelivery) || (operation == Operation::CompleteTimerDelivery)) &&
            m_timerDeliveryArmReceipts[index].Timer.Token.IsValid())
        {
            SettleArmedTimerDelivery(index);
        }
        return FailProcessor(index, Error::InvalidGeneration, operation, ReleaseError::InvalidGeneration,
                             Ia64KernelContextError::None, generation);
    }
    return Error::None;
}

AccessError Runtime::GetProcessorSnapshot(const Kernel::Scheduler::CpuId processor,
                                          Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& output) const noexcept
{
    output = {};
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return AccessError::NotReady;
    }
    Foundation::Size index{0U};
    if (!TryGetProcessorIndex(processor, index))
    {
        return AccessError::InvalidProcessor;
    }

    const Foundation::UInt64 state = m_processorStates[index].LoadAcquire();
    const Foundation::UInt64 localTimerProofState = m_localTimerProofStates[index].LoadAcquire();
    const Foundation::UInt64 portableSchedulerState = m_portableSchedulerStates[index].LoadAcquire();
    const Foundation::UInt64 evidence = m_evidencePublications[index].LoadAcquire();
    const Foundation::UInt64 serviceState = m_serviceMailboxStates[index].LoadAcquire();
    const Foundation::UInt64 completionSequence = m_serviceCompletionSequences[index].LoadAcquire();
    output.Layout = m_plan.Processors[index];
    if (m_idleContextPreparedPublications[index].LoadAcquire() != 0U)
    {
        output.IdleContext.State = Ia64KernelContextState::Prepared;
    }
    output.WorkerContext = m_workerContextSnapshots[index];
    output.Generation = m_generations[index];
    output.Processor = processor;
    output.State = DecodeProcessorState(state);
    output.LocalTimerProof.Phase = DecodeLocalTimerProofPhase(localTimerProofState);
    output.PortableScheduler.Phase = DecodePortableSchedulerPhase(portableSchedulerState);
    output.EvidenceMask =
        static_cast<Foundation::UInt8>(evidence & (RuntimeEntryEvidencePublished | FirstWorkerEvidencePublished |
                                                   ResumedWorkerEvidencePublished | ServingIdleEvidencePublished));
    if ((evidence & RuntimeEntryEvidencePublished) != 0U)
    {
        output.RuntimeEntryEvidence = m_runtimeEntryEvidence[index];
    }
    if ((evidence & FirstWorkerEvidencePublished) != 0U)
    {
        output.FirstWorkerEntryEvidence = m_firstWorkerEntryEvidence[index];
    }
    if ((evidence & ResumedWorkerEvidencePublished) != 0U)
    {
        output.ResumedWorkerEntryEvidence = m_resumedWorkerEntryEvidence[index];
    }
    if ((evidence & ServingIdleEvidencePublished) != 0U)
    {
        output.ServingIdleEvidence = m_servingIdleEvidence[index];
    }
    if (localTimerProofState == static_cast<Foundation::UInt64>(ProofPhase::Completed))
    {
        output.LocalTimerProof = m_localTimerProofRecords[index];
    }
    if ((portableSchedulerState == static_cast<Foundation::UInt64>(PortablePhase::Online)) ||
        ((evidence & (ResumedWorkerEvidencePublished | ServingIdleEvidencePublished)) ==
         (ResumedWorkerEvidencePublished | ServingIdleEvidencePublished)))
    {
        output.PortableScheduler = m_portableSchedulerRecords[index];
        output.PortableScheduler.Phase = DecodePortableSchedulerPhase(portableSchedulerState);
    }
    output.Service.RequestSequence = m_serviceRequestSequences[index].LoadAcquire();
    output.Service.CompletionSequence = completionSequence;
    output.Service.RequestCookie = m_serviceRequestCookies[index].LoadAcquire();
    output.Service.CompletionCookie = m_serviceCompletionCookies[index].LoadAcquire();
    output.Service.CompletionEvidenceValue = m_serviceCompletionEvidenceValues[index].LoadAcquire();
    output.Service.CompletionRunSequence = m_serviceCompletionRunSequences[index].LoadAcquire();
    output.Service.DispatchCount = m_serviceDispatchCounts[index].LoadAcquire();
    output.Service.WakeCount = m_serviceWakeCounts[index].LoadAcquire();
    output.Service.ServiceCount = m_serviceCounts[index].LoadAcquire();
    output.Service.State = DecodePortableSchedulerServiceState(serviceState);
    const Foundation::UInt64 result = m_serviceCompletionResults[index].LoadAcquire();
    output.Service.Result = result == static_cast<Foundation::UInt64>(ServiceResult::Completed)
                                ? ServiceResult::Completed
                                : ServiceResult::None;

    const Foundation::UInt64 timerCompletion = m_timerDeliveryCompletionSequences[index].LoadAcquire();
    const Foundation::UInt64 timerState = m_timerDeliveryStates[index].LoadAcquire();
    const Foundation::UInt64 timerRequest = m_timerDeliveryRequestSequences[index].LoadAcquire();
    const Foundation::UInt64 timerResult = m_timerDeliveryResults[index].LoadAcquire();
    if (!Detail::TimerDeliveryPublicationIsCoherent(timerCompletion, timerState, timerRequest, timerResult))
    {
        output = {};
        return AccessError::PublicationPending;
    }
    output.TimerDelivery.RequestSequence = timerRequest;
    output.TimerDelivery.CompletionSequence = timerCompletion;
    output.TimerDelivery.State = DecodeTimerDeliveryState(timerState);
    output.TimerDelivery.Result = timerResult == static_cast<Foundation::UInt64>(TimerDeliveryResult::Delivered)
                                      ? TimerDeliveryResult::Delivered
                                      : TimerDeliveryResult::None;
    if (timerCompletion == 1U)
    {
        output.TimerDeliveryRecord = m_timerDeliveryRecords[index];
        if ((m_timerDeliveryCompletionSequences[index].LoadAcquire() != 1U) ||
            (m_timerDeliveryRequestSequences[index].LoadAcquire() != 1U) ||
            (m_timerDeliveryStates[index].LoadAcquire() != TimerDeliveryCompleted) ||
            (m_timerDeliveryResults[index].LoadAcquire() !=
             static_cast<Foundation::UInt64>(TimerDeliveryResult::Delivered)))
        {
            output = {};
            return AccessError::PublicationPending;
        }
    }
    return AccessError::None;
}

AccessError Runtime::GetPortableSchedulerFailure(const Kernel::Scheduler::CpuId processor,
                                                 PortableFailure& output) const noexcept
{
    ClearPortableFailure(output);
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return AccessError::NotReady;
    }
    Foundation::Size index{0U};
    if (!TryGetProcessorIndex(processor, index))
    {
        return AccessError::InvalidProcessor;
    }
    for (Foundation::Size attempt{0U}; attempt < PortableSchedulerFailureSnapshotAttemptLimit; ++attempt)
    {
        const Foundation::UInt64 initial = m_portableSchedulerFailurePublicationSequences[index].LoadAcquire();
        if (initial == MaximumUInt64)
        {
            return AccessError::PublicationSequenceExhausted;
        }
        if ((initial & 1U) != 0U)
        {
            continue;
        }
        if (initial == 0U)
        {
            // No object representation has been packed yet. Confirm the
            // zero generation is stable and retain ClearPortableFailure's
            // Value-initialized pointer representations.
            if (m_portableSchedulerFailurePublicationSequences[index].LoadAcquire() == 0U)
            {
                return AccessError::None;
            }
            continue;
        }

        auto* const destination = reinterpret_cast<Foundation::UInt8*>(&output);
        for (Foundation::Size wordIndex{0U}; wordIndex < PortableSchedulerFailureWordCount; ++wordIndex)
        {
            const Foundation::UInt64 word = m_portableSchedulerFailureWords[index][wordIndex].LoadAcquire();
            const Foundation::Size offset = wordIndex * sizeof(Foundation::UInt64);
            for (Foundation::Size byteIndex{0U}; byteIndex < sizeof(Foundation::UInt64); ++byteIndex)
            {
                destination[offset + byteIndex] = static_cast<Foundation::UInt8>(word >> (byteIndex * 8U));
            }
        }

        const Foundation::UInt64 final = m_portableSchedulerFailurePublicationSequences[index].LoadAcquire();
        if ((initial == final) && ((final & 1U) == 0U))
        {
            return AccessError::None;
        }
        ClearPortableFailure(output);
    }
    return AccessError::PublicationPending;
}

FailurePoll Runtime::PollFirstFailure(Failure& output) const noexcept
{
    output = {};
    const Foundation::UInt64 publication = m_firstFailurePublication.LoadAcquire();
    if (publication == 0U)
    {
        return FailurePoll::None;
    }
    if (publication != PublishedFailure)
    {
        return FailurePoll::PublicationPending;
    }
    output = m_firstFailure;
    return FailurePoll::Available;
}

Error Runtime::ClaimPhysicalRelease(const Interrupt::Ia64ApplicationProcessorRuntimeRelease& release) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(release.Processor, release.Generation, index, Operation::ClaimPhysicalRelease);
    if (validation != Error::None)
    {
        return validation;
    }
    if (!ReservedBytesAreZero(release.Reserved, sizeof(release.Reserved)))
    {
        return FailProcessor(index, Error::NonzeroReserved, Operation::ClaimPhysicalRelease, ReleaseError::None,
                             Ia64KernelContextError::None, 0U);
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Lifecycle::AwaitingRelease);
    if (!m_processorStates[index].CompareExchangeStrongAcquireRelease(expected, PublishingPhysicalClaim))
    {
        return FailProcessor(index, Error::InvalidProcessorState, Operation::ClaimPhysicalRelease,
                             ReleaseError::InvalidPrecedingStage, Ia64KernelContextError::None, expected);
    }

    const auto claimed = m_releaseCoordinator->Claim(release.Processor, release.Generation);
    if (!claimed.HasValue())
    {
        return FailProcessor(index, Error::ReleaseClaimRejected, Operation::ClaimPhysicalRelease, claimed.GetError(),
                             Ia64KernelContextError::None, 0U);
    }
    const Interrupt::Ia64ApplicationProcessorRuntimeRelease& receipt = claimed.GetValue();
    if ((receipt.Generation != release.Generation) || (receipt.Processor.GetValue() != release.Processor.GetValue()) ||
        !ReservedBytesAreZero(receipt.Reserved, sizeof(receipt.Reserved)))
    {
        return FailProcessor(index, Error::ReleaseClaimRejected, Operation::ClaimPhysicalRelease,
                             ReleaseError::InvalidEvidence, Ia64KernelContextError::None, receipt.Generation);
    }

    m_processorStates[index].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::PhysicalClaimed));
    return Error::None;
}

Error Runtime::PublishPhysicalFailure(const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation,
                                      const PhysicalFailure failure, const Foundation::UInt64 detail) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::PublishPhysicalFailure);
    if (validation != Error::None)
    {
        return validation;
    }

    Error error{Error::InvalidPhysicalFailure};
    switch (failure)
    {
    case PhysicalFailure::None:
        break;

    case PhysicalFailure::RootAdmissionRejected:
        error = Error::PhysicalRootAdmissionRejected;
        break;

    case PhysicalFailure::LocalTimerProofContextPublicationRejected:
        error = Error::LocalTimerProofContextPublicationRejected;
        break;

    case PhysicalFailure::LocalTimerProofContextAccessRejected:
        error = Error::LocalTimerProofContextAccessRejected;
        break;

    case PhysicalFailure::PivotRejected:
        error = Error::RuntimePivotRejected;
        break;

    case PhysicalFailure::TranslatedContinuationReturned:
        error = Error::TranslatedContinuationReturned;
        break;
    }

    const Foundation::UInt64 state = m_processorStates[index].LoadAcquire();
    if (state != static_cast<Foundation::UInt64>(Lifecycle::PhysicalClaimed))
    {
        return FailProcessor(index, Error::InvalidProcessorState, Operation::PublishPhysicalFailure, ReleaseError::None,
                             Ia64KernelContextError::None, state);
    }
    return FailProcessor(index, error, Operation::PublishPhysicalFailure, ReleaseError::None,
                         Ia64KernelContextError::None, detail);
}

bool Runtime::ValidateEvidence(const Foundation::Size index,
                               const Ia64ApplicationProcessorKernelRuntimeRegisterEvidence& evidence,
                               const EvidenceKind kind) const noexcept
{
    const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    const Foundation::UInt64 stackStart =
        kind == EvidenceKind::Worker
            ? Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerAliasVirtualStart(layout)
            : Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleMemoryStackVirtualStart(layout);
    const Foundation::UInt64 stackSize = kind == EvidenceKind::Worker
                                             ? Memory::Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes
                                             : Memory::Ia64ApplicationProcessorRuntimeIdleStackSizeBytes;
    const Foundation::UInt64 rseStart =
        kind == EvidenceKind::Worker
            ? Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout)
            : Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleRseBackingStoreVirtualStart(layout);
    const Foundation::UInt64 rseSize = kind == EvidenceKind::Worker
                                           ? Memory::Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes
                                           : Memory::Ia64ApplicationProcessorRuntimeIdleRseSizeBytes;
    const Foundation::UInt64 stackEnd = stackStart + stackSize;
    const Foundation::UInt64 rseEnd = rseStart + rseSize;
    return (evidence.CaptureComplete == 1U) && ReservedBytesAreZero(evidence.Reserved, sizeof(evidence.Reserved)) &&
           IsAligned(evidence.StackPointer, 16U) && (evidence.StackPointer >= stackStart) &&
           (evidence.StackPointer < stackEnd) && IsAligned(evidence.RseBackingStorePointer, 8U) &&
           (evidence.RseBackingStorePointer >= rseStart) && (evidence.RseBackingStorePointer < rseEnd) &&
           IsAligned(evidence.RseBackingStoreBase, 8U) && (evidence.RseBackingStoreBase >= rseStart) &&
           (evidence.RseBackingStoreBase < rseEnd) &&
           (evidence.RseBackingStorePointer >= evidence.RseBackingStoreBase) && (evidence.RseConfiguration == 0U) &&
           ((evidence.ProcessorStatus & TranslationStateMask) == TranslationStateMask) &&
           ((evidence.ProcessorStatus & InstructionCollectionMask) == InstructionCollectionMask) &&
           ((evidence.ProcessorStatus & InterruptionEnableMask) == 0U);
}

Error Runtime::EnterTranslatedIdle(const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation,
                                   const Ia64ApplicationProcessorKernelRuntimeRegisterEvidence& evidence) noexcept
{
    Foundation::Size index{0U};
    const Error validation = ValidateProcessorGeneration(processor, generation, index, Operation::EnterTranslatedIdle);
    if (validation != Error::None)
    {
        return validation;
    }
    if (!ValidateEvidence(index, evidence, EvidenceKind::Idle))
    {
        return FailProcessor(index, Error::InvalidRegisterEvidence, Operation::EnterTranslatedIdle, ReleaseError::None,
                             Ia64KernelContextError::None, 0U);
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Lifecycle::PhysicalClaimed);
    if (!m_processorStates[index].CompareExchangeStrongAcquireRelease(expected, PublishingRuntimeEntry))
    {
        return FailProcessor(index, Error::InvalidProcessorState, Operation::EnterTranslatedIdle,
                             ReleaseError::InvalidPrecedingStage, Ia64KernelContextError::None, expected);
    }

    m_runtimeEntryEvidence[index] = evidence;
    m_evidencePublications[index].StoreRelease(RuntimeEntryEvidencePublished);
    const ReleaseError releaseError = m_releaseCoordinator->PublishRuntimeEntered(processor, generation);
    if (releaseError != ReleaseError::None)
    {
        return FailProcessor(index, Error::RuntimeEnteredPublicationRejected, Operation::EnterTranslatedIdle,
                             releaseError, Ia64KernelContextError::None, 0U);
    }
    m_processorStates[index].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered));
    return Error::None;
}

Error Runtime::PublishWorkerEntry(const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation,
                                  const Ia64ApplicationProcessorKernelRuntimeRegisterEvidence& evidence) noexcept
{
    Foundation::Size index{0U};
    const Error validation = ValidateProcessorGeneration(processor, generation, index, Operation::PublishWorkerEntry);
    if (validation != Error::None)
    {
        return validation;
    }
    if (!ValidateEvidence(index, evidence, EvidenceKind::Worker))
    {
        return FailProcessor(index, Error::InvalidRegisterEvidence, Operation::PublishWorkerEntry, ReleaseError::None,
                             Ia64KernelContextError::None, 0U);
    }

    Foundation::UInt64 expected = DispatchingPortableWorker;
    if (!m_processorStates[index].CompareExchangeStrongAcquireRelease(expected, PublishingWorkerEntry))
    {
        return FailProcessor(index, Error::InvalidProcessorState, Operation::PublishWorkerEntry,
                             ReleaseError::InvalidPrecedingStage, Ia64KernelContextError::None, expected);
    }

    m_firstWorkerEntryEvidence[index] = evidence;
    m_evidencePublications[index].StoreRelease(RuntimeEntryEvidencePublished | FirstWorkerEvidencePublished);
    m_processorStates[index].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered));
    return Error::None;
}

Error Runtime::ConfigureLocalTimerInterruptProof(
    const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation,
    const Interrupt::Ia64LocalTimerInterruptEvidence* const evidence) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::ConfigureLocalTimerInterruptProof);
    if (validation != Error::None)
    {
        if (TryGetProcessorIndex(processor, index))
        {
            m_localTimerProofStates[index].StoreRelease(static_cast<Foundation::UInt64>(ProofPhase::Failed));
        }
        return validation;
    }

    const Foundation::UInt64 lifecycle = m_processorStates[index].LoadAcquire();
    if (lifecycle != static_cast<Foundation::UInt64>(Lifecycle::PhysicalClaimed))
    {
        return FailLocalTimerProof(index, Error::InvalidProcessorState, Operation::ConfigureLocalTimerInterruptProof,
                                   lifecycle);
    }
    if (!ValidateLocalTimerEvidenceStorage(evidence))
    {
        return FailLocalTimerProof(index, Error::InvalidLocalTimerProofEvidence,
                                   Operation::ConfigureLocalTimerInterruptProof, AddressOf(evidence));
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(ProofPhase::Unconfigured);
    if (!m_localTimerProofStates[index].CompareExchangeStrongAcquireRelease(expected,
                                                                            PublishingLocalTimerConfiguration))
    {
        return FailLocalTimerProof(index, Error::InvalidLocalTimerProofState,
                                   Operation::ConfigureLocalTimerInterruptProof, expected);
    }

    m_localTimerEvidence[index] = evidence;
    m_localTimerProofStates[index].StoreRelease(static_cast<Foundation::UInt64>(ProofPhase::Configured));
    return Error::None;
}

Error Runtime::BeginLocalTimerInterruptProof(const Kernel::Scheduler::CpuId processor,
                                             const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::BeginLocalTimerInterruptProof);
    if (validation != Error::None)
    {
        if (TryGetProcessorIndex(processor, index))
        {
            m_localTimerProofStates[index].StoreRelease(static_cast<Foundation::UInt64>(ProofPhase::Failed));
        }
        return validation;
    }

    const Foundation::UInt64 lifecycle = m_processorStates[index].LoadAcquire();
    if (lifecycle != static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered))
    {
        return FailLocalTimerProof(index, Error::InvalidProcessorState, Operation::BeginLocalTimerInterruptProof,
                                   lifecycle);
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(ProofPhase::Configured);
    if (!m_localTimerProofStates[index].CompareExchangeStrongAcquireRelease(expected, PublishingLocalTimerArm))
    {
        return FailLocalTimerProof(index, Error::InvalidLocalTimerProofState, Operation::BeginLocalTimerInterruptProof,
                                   expected);
    }

    // These same-CPU operations execute at CPL0 with PSR.vm=0, PSR.ic=1 and
    // PSR.i=0. Initialize masks E8 before the SAL scratch cr.tpr value is
    // replaced. Delivery is admitted only later by the native poll. The proof
    // assumes no unrelated admitted source at a priority above the timer proof.
    const TimerInitializationError initialization =
        m_localTimers[index].Initialize(Interrupt::Ia64ExternalInterruptTimerVector);
    if (initialization != TimerInitializationError::None)
    {
        return FailLocalTimerProof(index, Error::LocalTimerInitializationRejected,
                                   Operation::BeginLocalTimerInterruptProof,
                                   static_cast<Foundation::UInt64>(initialization));
    }
    const Foundation::UInt64 taskPriorityStatus = AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority();
    if (taskPriorityStatus != 0U)
    {
        return FailLocalTimerProof(index, Error::LocalTimerTaskPriorityRejected,
                                   Operation::BeginLocalTimerInterruptProof, taskPriorityStatus);
    }

    // A deadline that has already passed when cr.itm is programmed (the AP
    // lost its lead between the sample and the write) programs nothing that
    // can fire: ProgramDeadline rejects it before the write, or the native
    // adapter restores the masked, disarmed timer. Sample again with twice
    // the lead; only a timer that rejects every attempt fails the proof.
    Foundation::UInt64 programmedDeadline{0U};
    Foundation::UInt64 pollDeadline{0U};
    TimerError programError{TimerError::DeadlineReached};
    for (Foundation::Size attempt{0U};
         (attempt < LocalTimerArmAttemptLimit) && (programError == TimerError::DeadlineReached); ++attempt)
    {
        const Foundation::UInt64 leadTicks = LocalTimerDeadlineLeadTicks << attempt;
        const Foundation::UInt64 ticks = m_localTimers[index].GetTicks();
        if (ticks == MaximumUInt64)
        {
            return FailLocalTimerProof(index, Error::LocalTimerCounterUnavailable,
                                       Operation::BeginLocalTimerInterruptProof, ticks);
        }
        if (ticks > (MaximumUInt64 - leadTicks))
        {
            return FailLocalTimerProof(index, Error::LocalTimerDeadlineOverflow,
                                       Operation::BeginLocalTimerInterruptProof, ticks);
        }
        programmedDeadline = ticks + leadTicks;
        if (programmedDeadline > (MaximumUInt64 - LocalTimerPollGraceTicks))
        {
            return FailLocalTimerProof(index, Error::LocalTimerDeadlineOverflow,
                                       Operation::BeginLocalTimerInterruptProof, programmedDeadline);
        }
        pollDeadline = programmedDeadline + LocalTimerPollGraceTicks;
        programError = m_localTimers[index].ProgramDeadline(programmedDeadline);
    }
    if (programError != TimerError::None)
    {
        // Preserve the first observable failure. HardwareFailure may mean the
        // arm was transiently accepted, so cleanup is still attempted even
        // though a faulted timer can only report HardwareFailure from Cancel.
        static_cast<void>(m_localTimers[index].CancelDeadline());
        return FailLocalTimerProof(index, Error::LocalTimerProgramRejected, Operation::BeginLocalTimerInterruptProof,
                                   static_cast<Foundation::UInt64>(programError));
    }

    m_localTimerProgrammedDeadlines[index] = programmedDeadline;
    m_localTimerPollDeadlines[index] = pollDeadline;
    m_localTimerProofStates[index].StoreRelease(static_cast<Foundation::UInt64>(ProofPhase::Armed));
    return Error::None;
}

Error Runtime::CompleteLocalTimerInterruptProof(const Kernel::Scheduler::CpuId processor,
                                                const Foundation::UInt64 generation, const PollStatus pollStatus,
                                                const PollRange pollCodeRange) noexcept
{
    Foundation::Size index{0U};
    if ((GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready) || !TryGetProcessorIndex(processor, index))
    {
        // An invalid processor has no safely resolvable AP-owned timer slot.
        return ValidateProcessorGeneration(processor, generation, index, Operation::CompleteLocalTimerInterruptProof);
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(ProofPhase::Armed);
    if (!m_localTimerProofStates[index].CompareExchangeStrongAcquireRelease(expected, PublishingLocalTimerCompletion))
    {
        return FailLocalTimerProof(index, Error::InvalidLocalTimerProofState,
                                   Operation::CompleteLocalTimerInterruptProof, expected);
    }

    // Generation is checked only after claiming the accepted arm. From this
    // point, every C++ return first masks the timer and snapshots its terminal
    // state; no generation failure may bypass AP-local cleanup.
    if (generation != m_generations[index])
    {
        const TimerError cancelError = m_localTimers[index].CancelDeadline();
        const Time::Ia64LocalTimerSnapshot finalSnapshot = m_localTimers[index].GetSnapshot();
        static_cast<void>(finalSnapshot);
        if (cancelError != TimerError::None)
        {
            return FailLocalTimerProof(index, Error::LocalTimerCancelRejected,
                                       Operation::CompleteLocalTimerInterruptProof,
                                       static_cast<Foundation::UInt64>(cancelError));
        }
        m_localTimerProofStates[index].StoreRelease(static_cast<Foundation::UInt64>(ProofPhase::Failed));
        return FailProcessor(index, Error::InvalidGeneration, Operation::CompleteLocalTimerInterruptProof,
                             ReleaseError::InvalidGeneration, Ia64KernelContextError::None, generation);
    }

    const Foundation::UInt64 programmedDeadline = m_localTimerProgrammedDeadlines[index];
    const Foundation::UInt64 pollDeadline = m_localTimerPollDeadlines[index];
    const Interrupt::Ia64LocalTimerInterruptEvidence* const evidence = m_localTimerEvidence[index];
    Interrupt::Ia64LocalTimerInterruptEvidence acquiredEvidence{};
    ValidationError evidenceValidation{ValidationError::Incomplete};
    if (pollStatus == PollStatus::Completed)
    {
        acquiredEvidence = Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(*evidence, 1U);
        evidenceValidation = Interrupt::ValidateIa64LocalTimerInterruptEvidenceForSequence(
            acquiredEvidence, 1U, processor.GetValue(), programmedDeadline, pollCodeRange.Start, pollCodeRange.End);
    }

    // The poll returns with PSR.i=0. Once ProgramDeadline was accepted, every
    // C++-observable completion path attempts to mask/cancel the source before
    // publishing either terminal success or failure. The external entry owns
    // its single EOI; this path deliberately does not acknowledge the mailbox.
    const TimerError cancelError = m_localTimers[index].CancelDeadline();
    const Time::Ia64LocalTimerSnapshot finalSnapshot = m_localTimers[index].GetSnapshot();
    if (cancelError != TimerError::None)
    {
        return FailLocalTimerProof(index, Error::LocalTimerCancelRejected, Operation::CompleteLocalTimerInterruptProof,
                                   static_cast<Foundation::UInt64>(cancelError));
    }
    if (pollStatus != PollStatus::Completed)
    {
        return FailLocalTimerProof(index, Error::LocalTimerPollRejected, Operation::CompleteLocalTimerInterruptProof,
                                   static_cast<Foundation::UInt64>(pollStatus));
    }
    if (evidenceValidation != ValidationError::None)
    {
        return FailLocalTimerProof(index, Error::LocalTimerEvidenceRejected,
                                   Operation::CompleteLocalTimerInterruptProof,
                                   static_cast<Foundation::UInt64>(evidenceValidation));
    }
    if ((finalSnapshot.State != TimerState::Ready) ||
        (finalSnapshot.Vector != Interrupt::Ia64ExternalInterruptTimerVector) ||
        (finalSnapshot.ProgrammedDeadlineTicks != 0U) || (finalSnapshot.Armed != 0U) ||
        !ReservedBytesAreZero(finalSnapshot.Reserved, sizeof(finalSnapshot.Reserved)))
    {
        return FailLocalTimerProof(index, Error::LocalTimerPostconditionRejected,
                                   Operation::CompleteLocalTimerInterruptProof, 0U);
    }

    m_localTimerProofRecords[index] = Ia64ApplicationProcessorLocalTimerProofRecord{
        .InterruptEvidence = acquiredEvidence,
        .FinalTimerSnapshot = finalSnapshot,
        .PollCodeRange = pollCodeRange,
        .EvidenceAddress = AddressOf(evidence),
        .ProgrammedDeadlineTicks = programmedDeadline,
        .PollDeadlineTicks = pollDeadline,
        .TaskPriority = LocalTimerTaskPriority,
        .PollStatus = pollStatus,
        .ValidationResult = evidenceValidation,
        .Phase = ProofPhase::Completed,
        .Vector = Interrupt::Ia64ExternalInterruptTimerVector,
        .Reserved = {},
    };
    m_localTimerProofStates[index].StoreRelease(static_cast<Foundation::UInt64>(ProofPhase::Completed));
    return Error::None;
}

Error Runtime::FailPortableScheduler(const Foundation::Size index, const Error error, const Operation operation,
                                     const Foundation::UInt64 detail) noexcept
{
    m_portableSchedulerStates[index].StoreRelease(static_cast<Foundation::UInt64>(PortablePhase::Failed));
    return FailProcessor(index, error, operation, ReleaseError::None, Ia64KernelContextError::None, detail);
}

bool Runtime::ValidatePortableSchedulerRoot(const Foundation::Size index,
                                            const Memory::Ia64BspRuntimeRootView& root) const noexcept
{
    if ((m_portableSchedulerConfiguration.LoadAcquire() != PortableSchedulerDependenciesReady) ||
        (m_runtimeMemory == nullptr) || (m_objectRegistry == nullptr) || (m_resourceDomain == nullptr) ||
        (m_issuer == nullptr) || (root.AddressSpaceStorage == nullptr) || !root.AddressSpaceIdentity.IsValid() ||
        (root.ControllerStorage == nullptr) || !root.Processor.IsValid() ||
        (root.Processor.GetValue() != static_cast<Foundation::UInt8>(index + 1U)) ||
        (root.AddressSpaceStorage != m_expectedSharedRoot.AddressSpaceStorage) ||
        !root.AddressSpaceIdentity.IsEqualTo(m_expectedSharedRoot.AddressSpaceIdentity))
    {
        return false;
    }

    const Kernel::Memory::AddressSpaceSnapshot addressSpace = root.AddressSpaceStorage->GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controller = root.ControllerStorage->GetSnapshot();
    return (addressSpace.State == Kernel::Memory::AddressSpaceState::Ready) &&
           addressSpace.Identity.IsEqualTo(root.AddressSpaceIdentity) &&
           (controller.State == Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) &&
           (controller.Processor.GetValue() == root.Processor.GetValue()) &&
           (controller.IdleStorage == root.AddressSpaceStorage) &&
           (controller.ActiveStorage == root.AddressSpaceStorage) &&
           controller.Idle.IsEqualTo(root.AddressSpaceIdentity) &&
           controller.Active.IsEqualTo(root.AddressSpaceIdentity);
}

void Runtime::ConstructPortableSchedulerDispatchers(const Foundation::Size index,
                                                    const Memory::Ia64BspRuntimeRootView& root) noexcept
{
    m_schedulerTimerQueues[index] = ::new (static_cast<void*>(m_schedulerTimerQueueStorage[index]))
        Kernel::Time::TimerQueue{*m_objectRegistry, m_localTimers[index]};
    m_timerDeliveryTimers[index] =
        ::new (static_cast<void*>(m_timerDeliveryTimerStorage[index])) Kernel::Time::Timer{m_localTimers[index]};
    m_timerDeliveryNotifications[index] =
        ::new (static_cast<void*>(m_timerDeliveryNotificationStorage[index])) Kernel::Ipc::Notification{};
    m_deferredTimerWorkers[index] = ::new (static_cast<void*>(m_deferredTimerWorkerStorage[index]))
        Kernel::Time::DeferredTimerWorker{*m_objectRegistry, *m_schedulerTimerQueues[index]};
    m_schedulerControllers[index] = ::new (static_cast<void*>(m_schedulerControllerStorage[index]))
        Kernel::Scheduler::ProcessorRunController{*m_objectRegistry, *m_schedulerTimerQueues[index],
                                                  m_localTimers[index], root.Processor,
                                                  Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline};
    m_schedulerRunLoops[index] = ::new (static_cast<void*>(m_schedulerRunLoopStorage[index]))
        Ia64ProcessorRunLoop{*m_schedulerControllers[index], *root.ControllerStorage, m_idleContexts[index]};
}

bool Runtime::InitializePortableTimerDeliveryObjects(const Foundation::Size index) noexcept
{
    PortableFailure& failure = m_portableSchedulerFailures[index];
    const Kernel::Object::ResourceDomainId chargeOwner = m_resourceDomain->GetIdentifier();

    failure.Timer = m_timerDeliveryTimers[index]->Initialize(chargeOwner);
    if (failure.Timer != Kernel::Time::TimerError::None)
    {
        return false;
    }
    const auto registeredTimer = m_issuer->CreateObject(*m_timerDeliveryTimers[index]);
    if (!registeredTimer.HasValue())
    {
        failure.ObjectRegistry = registeredTimer.GetError();
        return false;
    }
    if (!RegistrationMatches(*m_objectRegistry, registeredTimer.GetValue(), Kernel::Object::ObjectType::Timer,
                             *m_timerDeliveryTimers[index], failure.ObjectPin))
    {
        return false;
    }
    m_timerDeliveryTimerIdentities[index] = Kernel::Time::TimerId{*m_objectRegistry, registeredTimer.GetValue()};

    const Kernel::Ipc::NotificationError notificationError =
        m_timerDeliveryNotifications[index]->Initialize(chargeOwner);
    failure.Notification = Kernel::Ipc::NotificationFailure{
        .Error = notificationError,
        .Reserved = {},
    };
    if (notificationError != Kernel::Ipc::NotificationError::None)
    {
        return false;
    }
    const auto registeredNotification = m_issuer->CreateObject(*m_timerDeliveryNotifications[index]);
    if (!registeredNotification.HasValue())
    {
        failure.ObjectRegistry = registeredNotification.GetError();
        return false;
    }
    if (!RegistrationMatches(*m_objectRegistry, registeredNotification.GetValue(),
                             Kernel::Object::ObjectType::Notification, *m_timerDeliveryNotifications[index],
                             failure.ObjectPin))
    {
        return false;
    }
    m_timerDeliveryNotificationIdentities[index] =
        Kernel::Ipc::NotificationId{*m_objectRegistry, registeredNotification.GetValue()};

    const auto registeredBinding = m_deferredTimerWorkers[index]->Register(
        m_timerDeliveryTimerIdentities[index], *m_timerDeliveryTimers[index],
        m_timerDeliveryNotificationIdentities[index], *m_timerDeliveryNotifications[index]);
    if (!registeredBinding.HasValue())
    {
        failure.DeferredTimerWorker = registeredBinding.GetErrorReference();
        return false;
    }
    m_timerDeliveryRegistrations[index] = registeredBinding.GetValue();
    return true;
}

Error Runtime::InitializeTlbShootdownObjects(const Foundation::Size index,
                                             const Kernel::Scheduler::CpuId processor) noexcept
{
    PortableFailure& failure = m_portableSchedulerFailures[index];
    if ((m_tlbShootdownConfiguration.LoadAcquire() != TlbShootdownDependenciesReady) ||
        (m_tlbShootdownNotifier == nullptr) || (m_tlbShootdownMailbox == nullptr) ||
        (m_tlbShootdownDispatchers[index] != nullptr) || (m_tlbShootdownDrains[index] != nullptr))
    {
        return Error::TlbShootdownNotConfigured;
    }

    failure.TlbShootdownInvalidatorInitializationError =
        m_tlbShootdownInvalidators[index].Initialize(m_tlbShootdownCapabilities[index]);
    if (failure.TlbShootdownInvalidatorInitializationError != Memory::Ia64LocalTlbInvalidatorInitializationError::None)
    {
        return Error::TlbShootdownLocalInvalidatorInitializationRejected;
    }

    m_tlbShootdownDispatchers[index] = ::new (static_cast<void*>(m_tlbShootdownDispatcherStorage[index]))
        Kernel::Memory::TlbShootdownDispatcher{processor, m_tlbShootdownInvalidators[index]};
    m_tlbShootdownDrains[index] = ::new (static_cast<void*>(m_tlbShootdownDrainStorage[index]))
        Kernel::Memory::TlbShootdownDrain{*m_tlbShootdownMailbox, *m_tlbShootdownDispatchers[index]};

    const Memory::Ia64LocalTlbInvalidatorSnapshot invalidator = m_tlbShootdownInvalidators[index].GetSnapshot();
    if ((invalidator.State != Memory::Ia64LocalTlbInvalidatorState::Ready) ||
        (invalidator.ImplementedVirtualAddressMostSignificantBit !=
         m_tlbShootdownCapabilities[index].Translation.ImplementedVirtualAddressMostSignificantBit) ||
        !ReservedBytesAreZero(invalidator.Reserved, sizeof(invalidator.Reserved)) ||
        (m_tlbShootdownDispatchers[index]->GetProcessor().GetValue() != processor.GetValue()) ||
        (m_tlbShootdownDrains[index]->GetProcessor().GetValue() != processor.GetValue()))
    {
        return Error::TlbShootdownDependenciesRejected;
    }
    return Error::None;
}

bool Runtime::InitializePortableSchedulerObjects(const Foundation::Size index,
                                                 const Memory::Ia64BspRuntimeRootView& root) noexcept
{
    PortableFailure& failure = m_portableSchedulerFailures[index];
    const Kernel::Object::ResourceDomainId chargeOwner = m_resourceDomain->GetIdentifier();

    failure.ProtectionDomain = m_schedulerProtectionDomains[index].InitializeKernelOnly(
        *m_objectRegistry, m_resourceDomainIdentity, *m_resourceDomain, root.AddressSpaceIdentity);
    failure.ObjectPin = failure.ProtectionDomain.Pin;
    if (failure.ProtectionDomain.Error != Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }

    const auto registeredProtectionDomain = m_issuer->CreateObject(m_schedulerProtectionDomains[index]);
    if (!registeredProtectionDomain.HasValue())
    {
        failure.ObjectRegistry = registeredProtectionDomain.GetError();
        return false;
    }
    if (!RegistrationMatches(*m_objectRegistry, registeredProtectionDomain.GetValue(),
                             Kernel::Object::ObjectType::ProtectionDomain, m_schedulerProtectionDomains[index],
                             failure.ObjectPin))
    {
        return false;
    }
    m_schedulerProtectionDomainIdentities[index] =
        Kernel::Protection::ProtectionDomainId{*m_objectRegistry, registeredProtectionDomain.GetValue()};
    failure.ProtectionDomain = m_schedulerProtectionDomains[index].BindIdentity(
        *m_objectRegistry, m_schedulerProtectionDomainIdentities[index]);
    failure.ObjectPin = failure.ProtectionDomain.Pin;
    if (failure.ProtectionDomain.Error != Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }

    failure.Thread = m_schedulerThreads[index].Initialize(chargeOwner);
    if (failure.Thread != Kernel::Scheduler::ThreadError::None)
    {
        return false;
    }
    const auto registeredThread = m_issuer->CreateObject(m_schedulerThreads[index]);
    if (!registeredThread.HasValue())
    {
        failure.ObjectRegistry = registeredThread.GetError();
        return false;
    }
    if (!RegistrationMatches(*m_objectRegistry, registeredThread.GetValue(), Kernel::Object::ObjectType::Thread,
                             m_schedulerThreads[index], failure.ObjectPin))
    {
        return false;
    }
    m_schedulerThreadIdentities[index] = Kernel::Scheduler::ThreadId{*m_objectRegistry, registeredThread.GetValue()};

    m_schedulerContexts[index] = ::new (static_cast<void*>(m_schedulerContextStorage[index]))
        Kernel::Scheduler::SchedulingContext{*m_objectRegistry};
    const Foundation::UInt64 initialTicks = m_localTimers[index].GetTicks();
    if (initialTicks >= (MaximumUInt64 - 1U))
    {
        failure.SchedulingContext = Kernel::Scheduler::SchedulingContextError::InvalidConfiguration;
        return false;
    }
    const Foundation::UInt64 windowTicks = MaximumUInt64 - initialTicks - 1U;
    const Kernel::Scheduler::SchedulingContextConfiguration configuration{
        .BudgetTicks = windowTicks,
        .PeriodTicks = windowTicks,
        .RelativeDeadlineTicks = windowTicks,
        .Weight = 1U,
        .Priority = 0xFFU,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Emergency,
        .CpuAffinityMask = ProcessorBit(root.Processor.GetValue()),
        .Class = Kernel::Scheduler::SchedulingClass::EmergencyRecovery,
        .Reserved = 0U,
    };
    failure.SchedulingContext =
        m_schedulerContexts[index]->Initialize(configuration, chargeOwner, Kernel::Object::MonotonicTime{initialTicks});
    if (failure.SchedulingContext != Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }
    const auto registeredContext = m_issuer->CreateObject(*m_schedulerContexts[index]);
    if (!registeredContext.HasValue())
    {
        failure.ObjectRegistry = registeredContext.GetError();
        return false;
    }
    if (!RegistrationMatches(*m_objectRegistry, registeredContext.GetValue(),
                             Kernel::Object::ObjectType::SchedulingContext, *m_schedulerContexts[index],
                             failure.ObjectPin))
    {
        return false;
    }
    m_schedulerContextIdentities[index] =
        Kernel::Scheduler::SchedulingContextId{*m_objectRegistry, registeredContext.GetValue()};
    failure.SchedulingContext = m_schedulerContexts[index]->Bind(m_schedulerThreadIdentities[index]);
    if (failure.SchedulingContext != Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }

    ConstructPortableSchedulerDispatchers(index, root);
    if (!InitializePortableTimerDeliveryObjects(index))
    {
        return false;
    }
    failure.RunLoopBinding = m_schedulerRunLoops[index]->Bind(m_schedulerThreads[index], m_workerContexts[index],
                                                              m_schedulerProtectionDomains[index]);
    return failure.RunLoopBinding == Kernel::Scheduler::ProcessorKernelRunLoopError::None;
}

bool Runtime::ValidatePreparedTimerDeliveryPostconditions(const Foundation::Size index) const noexcept
{
    const Kernel::Time::TimerSnapshot timer = m_timerDeliveryTimers[index]->GetSnapshot();
    const Kernel::Ipc::NotificationSnapshot notification = m_timerDeliveryNotifications[index]->GetSnapshot();
    const Kernel::Time::DeferredTimerWorkerSnapshot worker = m_deferredTimerWorkers[index]->GetSnapshot();
    const Kernel::Time::DeferredTimerRegistrationReceipt& registration = m_timerDeliveryRegistrations[index];
    return m_timerDeliveryTimerIdentities[index].IsValid() &&
           m_timerDeliveryTimerIdentities[index].IsFromRegistry(*m_objectRegistry) &&
           m_timerDeliveryNotificationIdentities[index].IsValid() &&
           m_timerDeliveryNotificationIdentities[index].IsFromRegistry(*m_objectRegistry) &&
           registration.Registration.IsValid() && (registration.Registration.GetSequence() == 1U) &&
           registration.TimerIdentity.IsEqualTo(m_timerDeliveryTimerIdentities[index]) &&
           registration.NotificationIdentity.IsEqualTo(m_timerDeliveryNotificationIdentities[index]) &&
           (timer.State == Kernel::Time::TimerState::Disarmed) && !timer.Token.IsValid() &&
           (timer.DeadlineTicks == 0U) && (notification.State == Kernel::Ipc::NotificationState::Ready) &&
           (notification.Pending == 0U) && (notification.LastClaimSequence == 0U) &&
           (worker.State == Kernel::Time::DeferredTimerWorkerState::Ready) && (worker.RegisteredBindings == 1U) &&
           (worker.LastRegistrationSequence == 1U) && (worker.QueueState.NextDeadlineTicks == 0U) &&
           (worker.QueueState.NextClaimSequence == 1U) && (worker.QueueState.ScheduledEntries == 0U) &&
           (worker.QueueState.PendingEntries == 0U) && (worker.QueueState.ClaimedEntries == 0U) &&
           (m_timerDeliveryStates[index].LoadAcquire() == TimerDeliveryReady) &&
           (m_timerDeliveryRequestSequences[index].LoadAcquire() == 0U) &&
           (m_timerDeliveryCompletionSequences[index].LoadAcquire() == 0U) &&
           (m_timerDeliveryResults[index].LoadAcquire() ==
            static_cast<Foundation::UInt64>(TimerDeliveryResult::None)) &&
           (m_idleWorkOwners[index].LoadAcquire() == IdleWorkAvailable);
}

bool Runtime::ValidatePreparedPortableSchedulerDispatcherPostconditions(const Foundation::Size index) const noexcept
{
    const Kernel::Time::TimerQueueState queue = m_schedulerTimerQueues[index]->GetState();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = m_schedulerControllers[index]->GetSnapshot();
    const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = m_schedulerRunLoops[index]->GetSnapshot();
    return (queue.NextDeadlineTicks == 0U) && (queue.NextClaimSequence == 1U) && (queue.ScheduledEntries == 0U) &&
           (queue.PendingEntries == 0U) && (queue.ClaimedEntries == 0U) &&
           (controller.State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (controller.Mode == Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (runLoop.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) && (runLoop.BindingCount == 1U) &&
           (runLoop.ActiveThread == nullptr) && (runLoop.ActiveAddressSpace == nullptr);
}

Error Runtime::PreparePortableScheduler(const Kernel::Scheduler::CpuId processor,
                                        const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::PreparePortableScheduler);
    if (validation != Error::None)
    {
        return validation;
    }
    if (m_portableSchedulerConfiguration.LoadAcquire() != PortableSchedulerDependenciesReady)
    {
        return FailPortableScheduler(index, Error::PortableSchedulerNotConfigured, Operation::PreparePortableScheduler,
                                     0U);
    }
    if (m_tlbShootdownConfiguration.LoadAcquire() != TlbShootdownDependenciesReady)
    {
        return FailPortableScheduler(index, Error::TlbShootdownNotConfigured, Operation::PrepareTlbShootdown, 0U);
    }
    const Foundation::UInt64 lifecycle = m_processorStates[index].LoadAcquire();
    const Foundation::UInt64 proof = m_localTimerProofStates[index].LoadAcquire();
    const Time::Ia64LocalTimerSnapshot timer = m_localTimers[index].GetSnapshot();
    if (lifecycle != static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered) ||
        proof != static_cast<Foundation::UInt64>(ProofPhase::Completed) || (timer.State != TimerState::Ready) ||
        (timer.Vector != Interrupt::Ia64ExternalInterruptTimerVector) || (timer.ProgrammedDeadlineTicks != 0U) ||
        (timer.Armed != 0U))
    {
        return FailPortableScheduler(index,
                                     proof != static_cast<Foundation::UInt64>(ProofPhase::Completed)
                                         ? Error::LocalTimerProofNotCompleted
                                         : Error::PortableSchedulerPostconditionRejected,
                                     Operation::PreparePortableScheduler, lifecycle);
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(PortablePhase::Unconfigured);
    if (!m_portableSchedulerStates[index].CompareExchangeStrongAcquireRelease(expected,
                                                                              PublishingPortableSchedulerPreparation))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerPostconditionRejected,
                                     Operation::PreparePortableScheduler, expected);
    }

    const auto borrowed = m_runtimeMemory->BorrowReadyApplicationProcessorRoot(processor);
    if (!borrowed.HasValue())
    {
        m_portableSchedulerFailures[index].RootBorrow = borrowed.GetError();
        return FailPortableScheduler(index, Error::PortableSchedulerRootBorrowRejected,
                                     Operation::PreparePortableScheduler,
                                     static_cast<Foundation::UInt64>(borrowed.GetError()));
    }
    const Memory::Ia64BspRuntimeRootView root = borrowed.GetValue();
    if (!ValidatePortableSchedulerRoot(index, root))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerRootRejected, Operation::PreparePortableScheduler,
                                     0U);
    }
    m_schedulerRoots[index] = root;
    const Error tlbShootdownInitialization = InitializeTlbShootdownObjects(index, processor);
    if (tlbShootdownInitialization != Error::None)
    {
        return FailPortableScheduler(
            index, tlbShootdownInitialization, Operation::PrepareTlbShootdown,
            static_cast<Foundation::UInt64>(
                m_portableSchedulerFailures[index].TlbShootdownInvalidatorInitializationError));
    }
    if (!InitializePortableSchedulerObjects(index, root))
    {
        const PortableFailure& failure = m_portableSchedulerFailures[index];
        Error error{Error::PortableSchedulerPostconditionRejected};
        if (failure.ProtectionDomain.Error != Kernel::Protection::ProtectionDomainError::None)
        {
            error = m_schedulerProtectionDomainIdentities[index].IsValid()
                        ? Error::PortableSchedulerProtectionDomainIdentityRejected
                        : Error::PortableSchedulerProtectionDomainInitializationRejected;
        }
        else if (!m_schedulerProtectionDomainIdentities[index].IsValid())
        {
            error = Error::PortableSchedulerProtectionDomainRegistrationRejected;
        }
        else if (failure.Thread != Kernel::Scheduler::ThreadError::None)
        {
            error = Error::PortableSchedulerThreadInitializationRejected;
        }
        else if (!m_schedulerThreadIdentities[index].IsValid())
        {
            error = Error::PortableSchedulerThreadRegistrationRejected;
        }
        else if (failure.SchedulingContext != Kernel::Scheduler::SchedulingContextError::None)
        {
            error = m_schedulerContextIdentities[index].IsValid()
                        ? Error::PortableSchedulerSchedulingContextBindingRejected
                        : Error::PortableSchedulerSchedulingContextInitializationRejected;
        }
        else if (!m_schedulerContextIdentities[index].IsValid())
        {
            error = Error::PortableSchedulerSchedulingContextRegistrationRejected;
        }
        else if (failure.Timer != Kernel::Time::TimerError::None)
        {
            error = Error::PortableSchedulerTimerInitializationRejected;
        }
        else if (!m_timerDeliveryTimerIdentities[index].IsValid())
        {
            error = Error::PortableSchedulerTimerRegistrationRejected;
        }
        else if (failure.Notification.Error != Kernel::Ipc::NotificationError::None)
        {
            error = Error::PortableSchedulerNotificationInitializationRejected;
        }
        else if (!m_timerDeliveryNotificationIdentities[index].IsValid())
        {
            error = Error::PortableSchedulerNotificationRegistrationRejected;
        }
        else if (failure.DeferredTimerWorker.Error != Kernel::Time::DeferredTimerWorkerError::None)
        {
            error = Error::PortableSchedulerDeferredTimerBindingRejected;
        }
        else if (failure.RunLoopBinding != Kernel::Scheduler::ProcessorKernelRunLoopError::None)
        {
            error = Error::PortableSchedulerRunLoopBindingRejected;
        }
        return FailPortableScheduler(index, error, Operation::PreparePortableScheduler, 0U);
    }

    const Kernel::Protection::ProtectionDomainSnapshot protection = m_schedulerProtectionDomains[index].GetSnapshot();
    const Kernel::Scheduler::ThreadSnapshot thread = m_schedulerThreads[index].GetSnapshot();
    const Kernel::Scheduler::SchedulingContextSnapshot context = m_schedulerContexts[index]->GetSnapshot();
    if ((protection.Kind != Kernel::Protection::ProtectionDomainKind::KernelOnly) ||
        (protection.State != Kernel::Protection::ProtectionDomainState::Ready) ||
        !protection.Identity.IsEqualTo(m_schedulerProtectionDomainIdentities[index]) ||
        !protection.AddressSpace.IsEqualTo(root.AddressSpaceIdentity) ||
        (thread.State != Kernel::Scheduler::ThreadState::Dormant) ||
        (context.State != Kernel::Scheduler::SchedulingContextState::Bound) ||
        !context.BaseThread.IsEqualTo(m_schedulerThreadIdentities[index]) ||
        !context.CurrentThread.IsEqualTo(m_schedulerThreadIdentities[index]) ||
        !ValidatePreparedPortableSchedulerDispatcherPostconditions(index) ||
        !ValidatePreparedTimerDeliveryPostconditions(index))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerPostconditionRejected,
                                     Operation::PreparePortableScheduler, 0U);
    }

    PublishPortableSchedulerFailureSnapshot(index);
    m_portableSchedulerStates[index].StoreRelease(static_cast<Foundation::UInt64>(PortablePhase::Prepared));
    return Error::None;
}

bool Runtime::ValidateExpectedPortableSchedulerIdleFailure(const Foundation::Size index) const noexcept
{
    const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure = m_schedulerIdleFailures[index];
    return (failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected) &&
           (failure.ControllerError == Kernel::Scheduler::ProcessorRunError::NoRunnableThread) &&
           (failure.SchedulerError == Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread) &&
           (failure.DeadlineError == Kernel::Time::ProcessorDeadlineError::None) &&
           (failure.DeadlineReceipt.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (failure.DeadlineReceipt.SelectedDeadlineTicks == 0U) && (failure.DeadlineReceipt.SelectedSources == 0U) &&
           (failure.DeadlineReceipt.Reprogrammed == 1U) && (failure.ActiveRunRetained == 0U) &&
           (failure.NativeError == 0U) &&
           (failure.AddressSpaceController.Error == Kernel::Memory::ProcessorAddressSpaceControllerError::None);
}

bool Runtime::ValidatePortableSchedulerIdleTimerPostconditions(const Foundation::Size index) const noexcept
{
    const Time::Ia64LocalTimerSnapshot timer = m_localTimers[index].GetSnapshot();
    const Kernel::Time::TimerQueueState queue = m_schedulerTimerQueues[index]->GetState();
    return (timer.State == TimerState::Ready) && (timer.Vector == Interrupt::Ia64ExternalInterruptTimerVector) &&
           (timer.Armed == 0U) && (timer.ProgrammedDeadlineTicks == 0U) && (queue.NextDeadlineTicks == 0U) &&
           (queue.ScheduledEntries == 0U) && (queue.PendingEntries == 0U) && (queue.ClaimedEntries == 0U);
}

bool Runtime::ValidatePortableSchedulerIdleRunPostconditions(const Foundation::Size index) const noexcept
{
    const Kernel::Scheduler::ProcessorRunSnapshot controller = m_schedulerControllers[index]->GetSnapshot();
    const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = m_schedulerRunLoops[index]->GetSnapshot();
    return (controller.State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (controller.Mode == Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (controller.Deadline.SelectedDeadlineTicks == 0U) && (controller.Deadline.SelectedSources == 0U) &&
           (runLoop.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) &&
           (runLoop.IdleFailurePending == 0U) && (runLoop.ActiveThread == nullptr) &&
           (runLoop.ActiveProtectionDomain == nullptr) && (runLoop.ActiveAddressSpace == nullptr) &&
           (runLoop.BindingCount == 1U);
}

bool Runtime::ValidatePortableSchedulerBlockedThreadPostconditions(const Foundation::Size index,
                                                                   const Foundation::UInt64 expectedRunSequence,
                                                                   const bool requireDistinctToken) noexcept
{
    const Kernel::Scheduler::ThreadSnapshot thread = m_schedulerThreads[index].GetSnapshot();
    const Kernel::Scheduler::SchedulingContextSnapshot context = m_schedulerContexts[index]->GetSnapshot();
    if ((thread.State != Kernel::Scheduler::ThreadState::Blocked) || !thread.BlockToken.IsValid() ||
        (thread.RunSequence != expectedRunSequence) || (thread.ExitCode != 0U) ||
        (context.State != Kernel::Scheduler::SchedulingContextState::Bound))
    {
        return false;
    }
    if (!requireDistinctToken)
    {
        m_initialSchedulerBlockTokens[index] = thread.BlockToken;
    }
    if (requireDistinctToken && thread.BlockToken.IsEqualTo(m_schedulerBlockTokens[index]))
    {
        return false;
    }
    m_schedulerBlockTokens[index] = thread.BlockToken;
    return true;
}

bool Runtime::ValidatePortableSchedulerBlockedPostconditions(const Foundation::Size index,
                                                             const Foundation::UInt64 expectedRunSequence,
                                                             const bool requireDistinctToken) noexcept
{
    return ValidatePortableSchedulerRoot(index, m_schedulerRoots[index]) &&
           ValidatePortableSchedulerIdleTimerPostconditions(index) &&
           ValidatePortableSchedulerIdleRunPostconditions(index) &&
           ValidatePortableSchedulerBlockedThreadPostconditions(index, expectedRunSequence, requireDistinctToken);
}

Error Runtime::PublishSchedulerWaiting(const Foundation::Size index, const Kernel::Scheduler::CpuId processor,
                                       const Foundation::UInt64 generation) noexcept
{
    Foundation::UInt64 expectedPhase = static_cast<Foundation::UInt64>(PortablePhase::Starting);
    if (!m_portableSchedulerStates[index].CompareExchangeStrongAcquireRelease(expectedPhase,
                                                                              PublishingPortableSchedulerWaiting))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerPostconditionRejected,
                                     Operation::PublishSchedulerWaiting, expectedPhase);
    }
    Foundation::UInt64 expectedLifecycle = static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered);
    if (!m_processorStates[index].CompareExchangeStrongAcquireRelease(expectedLifecycle, PublishingSchedulerWaiting))
    {
        return FailPortableScheduler(index, Error::InvalidProcessorState, Operation::PublishSchedulerWaiting,
                                     expectedLifecycle);
    }

    m_serviceDispatchCounts[index].StoreRelease(1U);
    m_portableSchedulerStates[index].StoreRelease(static_cast<Foundation::UInt64>(PortablePhase::Waiting));
    const ReleaseError releaseError = m_releaseCoordinator->PublishSchedulerWaiting(processor, generation);
    if (releaseError != ReleaseError::None)
    {
        m_portableSchedulerStates[index].StoreRelease(static_cast<Foundation::UInt64>(PortablePhase::Failed));
        return FailProcessor(index, Error::SchedulerWaitingPublicationRejected, Operation::PublishSchedulerWaiting,
                             releaseError, Ia64KernelContextError::None, 0U);
    }
    m_processorStates[index].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::SchedulerWaiting));
    return Error::None;
}

Error Runtime::DispatchPortableSchedulerFromIdle(const Kernel::Scheduler::CpuId processor,
                                                 const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::DispatchPortableScheduler);
    if (validation != Error::None)
    {
        return validation;
    }
    if (m_portableSchedulerStates[index].LoadAcquire() != static_cast<Foundation::UInt64>(PortablePhase::Prepared))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerPostconditionRejected,
                                     Operation::DispatchPortableScheduler, 0U);
    }

    PortableFailure& failure = m_portableSchedulerFailures[index];
    failure.Thread = m_schedulerThreads[index].MakeReady();
    if (failure.Thread != Kernel::Scheduler::ThreadError::None)
    {
        return FailPortableScheduler(index, Error::PortableSchedulerThreadReadyRejected,
                                     Operation::DispatchPortableScheduler,
                                     static_cast<Foundation::UInt64>(failure.Thread));
    }
    failure.RunLoop = {};
    if (!m_schedulerRunLoops[index]->EnqueueBoundThread(m_schedulerContextIdentities[index],
                                                        *m_schedulerContexts[index], m_schedulerThreadIdentities[index],
                                                        m_schedulerThreads[index], failure.RunLoop))
    {
        failure.Controller = {};
        failure.Controller.Error = failure.RunLoop.ControllerError;
        failure.Controller.Scheduler.Error = failure.RunLoop.SchedulerError;
        failure.Controller.Deadline.Error = failure.RunLoop.DeadlineError;
        failure.Controller.DeadlineReceipt = failure.RunLoop.DeadlineReceipt;
        return FailPortableScheduler(index, Error::PortableSchedulerEnqueueRejected,
                                     Operation::DispatchPortableScheduler,
                                     static_cast<Foundation::UInt64>(failure.RunLoop.Error));
    }

    Foundation::UInt64 expectedPhase = static_cast<Foundation::UInt64>(PortablePhase::Prepared);
    if (!m_portableSchedulerStates[index].CompareExchangeStrongAcquireRelease(expectedPhase,
                                                                              PublishingPortableSchedulerDispatch))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerPostconditionRejected,
                                     Operation::DispatchPortableScheduler, expectedPhase);
    }
    Foundation::UInt64 expectedLifecycle = static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered);
    if (!m_processorStates[index].CompareExchangeStrongAcquireRelease(expectedLifecycle, DispatchingPortableWorker))
    {
        return FailPortableScheduler(index, Error::InvalidProcessorState, Operation::DispatchPortableScheduler,
                                     expectedLifecycle);
    }
    m_portableSchedulerStates[index].StoreRelease(static_cast<Foundation::UInt64>(PortablePhase::Starting));

    m_schedulerIdleReceipts[index] = {};
    m_schedulerIdleFailures[index] = {};
    const bool dispatch =
        m_schedulerRunLoops[index]->DispatchFromIdle(m_schedulerIdleReceipts[index], m_schedulerIdleFailures[index]);
    if (m_idleContexts[index].IsPrepared())
    {
        m_idleContextPreparedPublications[index].StoreRelease(1U);
    }
    failure.RunLoop = m_schedulerIdleFailures[index];
    PublishPortableSchedulerFailureSnapshot(index);
    if (dispatch ||
        (m_processorStates[index].LoadAcquire() != static_cast<Foundation::UInt64>(Lifecycle::RuntimeEntered)) ||
        !ValidateExpectedPortableSchedulerIdleFailure(index) ||
        !ValidatePortableSchedulerBlockedPostconditions(index, 1U, false))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerDispatchRejected,
                                     Operation::DispatchPortableScheduler,
                                     static_cast<Foundation::UInt64>(failure.RunLoop.Error));
    }
    return PublishSchedulerWaiting(index, processor, generation);
}

Foundation::Result<ServiceRequest, Error>
Runtime::RequestPortableSchedulerService(const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation,
                                         const Foundation::UInt64 cookie) noexcept
{
    using ResultType = Foundation::Result<ServiceRequest, Error>;
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return ResultType::Failure(Error::RuntimeNotReady);
    }
    Foundation::Size index{0U};
    if (!TryGetProcessorIndex(processor, index))
    {
        return ResultType::Failure(Error::InvalidProcessor);
    }
    if (generation != m_generations[index])
    {
        return ResultType::Failure(Error::InvalidGeneration);
    }
    const Foundation::UInt64 lifecycle = m_processorStates[index].LoadAcquire();
    const Foundation::UInt64 phase = m_portableSchedulerStates[index].LoadAcquire();
    if (!((lifecycle == static_cast<Foundation::UInt64>(Lifecycle::SchedulerWaiting) &&
           phase == static_cast<Foundation::UInt64>(PortablePhase::Waiting)) ||
          (lifecycle == static_cast<Foundation::UInt64>(Lifecycle::IdleServing) &&
           phase == static_cast<Foundation::UInt64>(PortablePhase::Online))))
    {
        return ResultType::Failure(Error::PortableSchedulerServiceUnavailable);
    }

    const Foundation::UInt64 timerState = m_timerDeliveryStates[index].LoadAcquire();
    if ((timerState == TimerDeliveryPublishing) || (timerState == TimerDeliveryPending) ||
        (timerState == TimerDeliveryExecuting))
    {
        return ResultType::Failure(Error::TimerDeliveryGateConflict);
    }
    if ((timerState != TimerDeliveryReady) && (timerState != TimerDeliveryCompleted))
    {
        return ResultType::Failure(Error::PortableSchedulerServiceUnavailable);
    }
    if ((timerState == TimerDeliveryCompleted) && !ValidateAcknowledgedTimerDeliveryCompletion(index))
    {
        return ResultType::Failure(Error::TimerDeliveryGateConflict);
    }

    Foundation::UInt64 expectedOwner = IdleWorkAvailable;
    if (!m_idleWorkOwners[index].CompareExchangeStrongAcquireRelease(expectedOwner, IdleWorkService))
    {
        return ResultType::Failure(expectedOwner == IdleWorkTimerDelivery
                                       ? Error::TimerDeliveryGateConflict
                                       : Error::PortableSchedulerServiceAlreadyPending);
    }
    const Foundation::UInt64 retainedTimerState = m_timerDeliveryStates[index].LoadAcquire();
    if ((retainedTimerState != TimerDeliveryReady) && (retainedTimerState != TimerDeliveryCompleted))
    {
        m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
        return ResultType::Failure(retainedTimerState == TimerDeliveryFailed
                                       ? Error::PortableSchedulerServiceUnavailable
                                       : Error::TimerDeliveryGateConflict);
    }
    if ((retainedTimerState == TimerDeliveryCompleted) && !ValidateAcknowledgedTimerDeliveryCompletion(index))
    {
        m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
        return ResultType::Failure(Error::TimerDeliveryGateConflict);
    }

    Foundation::UInt64 expectedState = ServiceMailboxIdle;
    if (!m_serviceMailboxStates[index].CompareExchangeStrongAcquireRelease(expectedState, ServiceMailboxPublishing))
    {
        m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
        return ResultType::Failure(expectedState == ServiceMailboxFailed
                                       ? Error::PortableSchedulerServiceUnavailable
                                       : Error::PortableSchedulerServiceAlreadyPending);
    }
    const Foundation::UInt64 priorSequence = m_serviceRequestSequences[index].LoadAcquire();
    const Foundation::UInt64 completionSequence = m_serviceCompletionSequences[index].LoadAcquire();
    if (priorSequence != completionSequence)
    {
        m_serviceMailboxStates[index].StoreRelease(ServiceMailboxFailed);
        if (m_timerDeliveryStates[index].LoadAcquire() != TimerDeliveryCompleted)
        {
            m_timerDeliveryStates[index].StoreRelease(TimerDeliveryFailed);
        }
        m_idleWorkOwners[index].StoreRelease(IdleWorkFailed);
        return ResultType::Failure(Error::PortableSchedulerServiceUnavailable);
    }
    Foundation::UInt64 sequence{0U};
    if (!Detail::TryAdvancePortableSchedulerServiceSequence(priorSequence, sequence))
    {
        m_serviceMailboxStates[index].StoreRelease(ServiceMailboxIdle);
        m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
        return ResultType::Failure(Error::PortableSchedulerServiceSequenceExhausted);
    }

    m_serviceRequestCookies[index].StoreRelease(cookie);
    m_serviceRequestSequences[index].StoreRelease(sequence);
    m_serviceMailboxStates[index].StoreRelease(ServiceMailboxPending);
    return ResultType::Success(ServiceRequest{
        .Sequence = sequence,
        .Cookie = cookie,
        .Processor = processor,
        .Reserved = {},
    });
}

bool Runtime::ValidateAcknowledgedTimerDeliveryGate(const Foundation::Size index) const noexcept
{
    const Interrupt::Ia64LocalTimerInterruptEvidence* const storage = m_localTimerEvidence[index];
    if (storage == nullptr)
    {
        return false;
    }
    const Interrupt::Ia64LocalTimerInterruptEvidence first =
        Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(*storage, 2U);
    if (!AcknowledgedTimerDeliveryGateIsValid(first))
    {
        return false;
    }
    const Interrupt::Ia64LocalTimerInterruptEvidence second =
        Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(*storage, 2U);
    return AcknowledgedTimerDeliveryGateIsValid(second);
}

bool Runtime::ValidateAcknowledgedTimerDeliveryCompletion(const Foundation::Size index) const noexcept
{
    const Interrupt::Ia64LocalTimerInterruptEvidence* const storage = m_localTimerEvidence[index];
    if (storage == nullptr)
    {
        return false;
    }
    // Sequence three is intentionally requested here. A stable acknowledged
    // 2/2 mailbox is returned as a mismatch containing only its two atomic
    // words; a concurrently published sequence three cannot be mistaken for
    // the acknowledged timer-delivery generation.
    const Interrupt::Ia64LocalTimerInterruptEvidence first =
        Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(*storage, 3U);
    if (!AcknowledgedTimerDeliveryCompletionIsValid(first))
    {
        return false;
    }
    const Interrupt::Ia64LocalTimerInterruptEvidence second =
        Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(*storage, 3U);
    return AcknowledgedTimerDeliveryCompletionIsValid(second);
}

bool Runtime::ValidateTimerDeliverySchedulerState(const Foundation::Size index,
                                                  const Foundation::UInt64 expectedNextClaimSequence) const noexcept
{
    const Kernel::Scheduler::ThreadSnapshot thread = m_schedulerThreads[index].GetSnapshot();
    const Kernel::Scheduler::SchedulingContextSnapshot context = m_schedulerContexts[index]->GetSnapshot();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = m_schedulerControllers[index]->GetSnapshot();
    const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = m_schedulerRunLoops[index]->GetSnapshot();
    const Time::Ia64LocalTimerSnapshot localTimer = m_localTimers[index].GetSnapshot();
    const Ia64ApplicationProcessorPortableSchedulerRecord& online = m_portableSchedulerRecords[index];
    return (thread.State == Kernel::Scheduler::ThreadState::Blocked) && (thread.RunSequence == 2U) &&
           thread.BlockToken.IsValid() && (thread.BlockToken.GetValue() == online.ServingBlockToken) &&
           thread.BlockToken.IsEqualTo(m_schedulerBlockTokens[index]) && (thread.ExitCode == 0U) &&
           (context.State == Kernel::Scheduler::SchedulingContextState::Bound) &&
           context.BaseThread.IsEqualTo(m_schedulerThreadIdentities[index]) &&
           context.CurrentThread.IsEqualTo(m_schedulerThreadIdentities[index]) && !context.RunningProcessor.IsValid() &&
           (context.RunStartTicks == 0U) && (context.RemainingBudgetTicks != 0U) &&
           (context.NextReplenishmentTicks != 0U) &&
           (context.NextReplenishmentTicks == context.AbsoluteDeadlineTicks) && (context.DonationDepth == 0U) &&
           (context.IsBudgetExhausted == 0U) && ReservedBytesAreZero(context.Reserved, sizeof(context.Reserved)) &&
           (controller.State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (controller.Mode == Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) &&
           (controller.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Idle) &&
           (controller.Scheduler.Processor.GetValue() == static_cast<Foundation::UInt8>(index + 1U)) &&
           !controller.Scheduler.ActiveThread.IsValid() && !controller.Scheduler.ActiveContext.IsValid() &&
           (controller.Scheduler.Queue.NextSequence == 3U) && (controller.Scheduler.Queue.ReadyEntries == 0U) &&
           (controller.Scheduler.Queue.ClaimedEntries == 0U) && (controller.Scheduler.Queue.ReservedEntries == 0U) &&
           (controller.Scheduler.Queue.Processor.GetValue() == static_cast<Foundation::UInt8>(index + 1U)) &&
           ReservedBytesAreZero(controller.Scheduler.Queue.Reserved, sizeof(controller.Scheduler.Queue.Reserved)) &&
           (controller.TimerQueue.NextDeadlineTicks == 0U) &&
           (controller.TimerQueue.NextClaimSequence == expectedNextClaimSequence) &&
           (controller.TimerQueue.ScheduledEntries == 0U) && (controller.TimerQueue.PendingEntries == 0U) &&
           (controller.TimerQueue.ClaimedEntries == 0U) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (controller.Deadline.SelectedDeadlineTicks == 0U) && (controller.Deadline.SelectedSources == 0U) &&
           (runLoop.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) && (runLoop.BindingCount == 1U) &&
           (runLoop.IdleFailurePending == 0U) && (runLoop.ActiveThread == nullptr) &&
           (runLoop.ActiveProtectionDomain == nullptr) && (runLoop.ActiveAddressSpace == nullptr) &&
           (localTimer.State == TimerState::Ready) &&
           (localTimer.Vector == Interrupt::Ia64ExternalInterruptTimerVector) && (localTimer.Armed == 0U) &&
           (localTimer.ProgrammedDeadlineTicks == 0U) && (online.Phase == PortablePhase::Online) &&
           (online.FirstResumedRunSequence == 2U) && (online.ServingBlockToken == thread.BlockToken.GetValue());
}

bool Runtime::ValidateTimerDeliveryRequestPreconditions(const Foundation::Size index) const noexcept
{
    return (m_processorStates[index].LoadAcquire() == static_cast<Foundation::UInt64>(Lifecycle::IdleServing)) &&
           (m_portableSchedulerStates[index].LoadAcquire() == static_cast<Foundation::UInt64>(PortablePhase::Online)) &&
           (m_serviceMailboxStates[index].LoadAcquire() == ServiceMailboxIdle) &&
           (m_serviceRequestSequences[index].LoadAcquire() == 1U) &&
           (m_serviceCompletionSequences[index].LoadAcquire() == 1U) &&
           (m_serviceCompletionResults[index].LoadAcquire() ==
            static_cast<Foundation::UInt64>(ServiceResult::Completed)) &&
           (m_serviceCompletionRunSequences[index].LoadAcquire() == 2U) &&
           (m_serviceDispatchCounts[index].LoadAcquire() == 2U) && (m_serviceWakeCounts[index].LoadAcquire() == 1U) &&
           (m_serviceCounts[index].LoadAcquire() == 1U) && ValidateTimerDeliverySchedulerState(index, 1U);
}

Foundation::Result<TimerDeliveryRequest, Error>
Runtime::RequestTimerDelivery(const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation) noexcept
{
    using ResultType = Foundation::Result<TimerDeliveryRequest, Error>;
    if (GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready)
    {
        return ResultType::Failure(Error::RuntimeNotReady);
    }
    Foundation::Size index{0U};
    if (!TryGetProcessorIndex(processor, index))
    {
        return ResultType::Failure(Error::InvalidProcessor);
    }
    if (generation != m_generations[index])
    {
        return ResultType::Failure(Error::InvalidGeneration);
    }
    const Foundation::UInt64 timerState = m_timerDeliveryStates[index].LoadAcquire();
    if (timerState != TimerDeliveryReady)
    {
        return ResultType::Failure(timerState == TimerDeliveryFailed ? Error::TimerDeliveryUnavailable
                                                                     : Error::TimerDeliveryAlreadyRequested);
    }
    if (!ValidateTimerDeliveryRequestPreconditions(index))
    {
        return ResultType::Failure(Error::TimerDeliveryUnavailable);
    }
    if (!ValidateAcknowledgedTimerDeliveryGate(index))
    {
        return ResultType::Failure(Error::TimerDeliveryEvidenceRejected);
    }

    Foundation::UInt64 expectedOwner = IdleWorkAvailable;
    if (!m_idleWorkOwners[index].CompareExchangeStrongAcquireRelease(expectedOwner, IdleWorkTimerDelivery))
    {
        return ResultType::Failure(Error::TimerDeliveryGateConflict);
    }
    if (!ValidateTimerDeliveryRequestPreconditions(index) || !ValidateAcknowledgedTimerDeliveryGate(index))
    {
        m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
        return ResultType::Failure(Error::TimerDeliveryUnavailable);
    }

    Foundation::UInt64 expectedState = TimerDeliveryReady;
    if (!m_timerDeliveryStates[index].CompareExchangeStrongAcquireRelease(expectedState, TimerDeliveryPublishing))
    {
        m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
        return ResultType::Failure(Error::TimerDeliveryAlreadyRequested);
    }
    if ((m_timerDeliveryRequestSequences[index].LoadAcquire() != 0U) ||
        (m_timerDeliveryCompletionSequences[index].LoadAcquire() != 0U))
    {
        return ResultType::Failure(FailPortableScheduler(index, Error::TimerDeliverySequenceExhausted,
                                                         Operation::RequestTimerDelivery,
                                                         m_timerDeliveryRequestSequences[index].LoadAcquire()));
    }

    m_timerDeliveryRequestSequences[index].StoreRelease(1U);
    m_timerDeliveryStates[index].StoreRelease(TimerDeliveryPending);
    return ResultType::Success(TimerDeliveryRequest{
        .Sequence = 1U,
        .Processor = processor,
        .Reserved = {},
    });
}

Error Runtime::ValidatePortableSchedulerActiveBinding(const Foundation::Size index,
                                                      const Kernel::Scheduler::CpuId processor,
                                                      const Foundation::UInt64 expectedRunSequence,
                                                      const Operation operation) noexcept
{
    const auto active = m_schedulerRunLoops[index]->GetActiveExecutionBinding();
    if (!active.HasValue())
    {
        m_portableSchedulerFailures[index].ActiveBinding = active.GetErrorReference();
        return FailPortableScheduler(index, Error::PortableSchedulerActiveBindingRejected, operation,
                                     static_cast<Foundation::UInt64>(active.GetErrorReference().Error));
    }
    const Kernel::Scheduler::ProcessorActiveExecutionBinding& binding = active.GetValue();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = m_schedulerControllers[index]->GetSnapshot();
    if ((binding.ThreadStorage != &m_schedulerThreads[index]) ||
        !binding.ThreadIdentity.IsEqualTo(m_schedulerThreadIdentities[index]) ||
        (binding.SchedulingContextStorage != m_schedulerContexts[index]) ||
        !binding.SchedulingContextIdentity.IsEqualTo(m_schedulerContextIdentities[index]) ||
        (binding.Processor.GetValue() != processor.GetValue()) || (binding.RunSequence != expectedRunSequence) ||
        (controller.State != Kernel::Scheduler::ProcessorRunState::Active) ||
        (controller.Mode != Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Armed) ||
        (controller.Deadline.SelectedDeadlineTicks == 0U))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerActiveBindingRejected, operation,
                                     binding.RunSequence);
    }
    return Error::None;
}

Error Runtime::WakePortableSchedulerWorker(const Foundation::Size index) noexcept
{
    PortableFailure& failure = m_portableSchedulerFailures[index];
    const Kernel::Scheduler::ThreadSnapshot thread = m_schedulerThreads[index].GetSnapshot();
    if ((thread.State != Kernel::Scheduler::ThreadState::Blocked) || !thread.BlockToken.IsValid() ||
        !thread.BlockToken.IsEqualTo(m_schedulerBlockTokens[index]))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceWakeRejected,
                                     Operation::ServicePortableScheduler, thread.BlockToken.GetValue());
    }

    auto& wake = m_schedulerControllers[index]->GetWakeFacade();
    const auto reserved =
        wake.ReserveBlockedEnqueue(m_schedulerContextIdentities[index], *m_schedulerContexts[index],
                                   m_schedulerThreadIdentities[index], m_schedulerThreads[index], thread.BlockToken);
    if (!reserved.HasValue())
    {
        failure.WakeQueue = reserved.GetError();
        return FailPortableScheduler(index, Error::PortableSchedulerServiceWakeReservationRejected,
                                     Operation::ServicePortableScheduler,
                                     static_cast<Foundation::UInt64>(failure.WakeQueue.Error));
    }

    failure.Thread = m_schedulerThreads[index].Wake(thread.BlockToken);
    if (failure.Thread != Kernel::Scheduler::ThreadError::None)
    {
        failure.WakeCancellation = wake.CancelReservedEnqueue(reserved.GetValue());
        return FailPortableScheduler(index, Error::PortableSchedulerServiceWakeRejected,
                                     Operation::ServicePortableScheduler,
                                     static_cast<Foundation::UInt64>(failure.Thread));
    }
    const auto committed = wake.CommitReservedEnqueue(reserved.GetValue());
    if (!committed.HasValue())
    {
        failure.WakeQueue = committed.GetError();
        return FailPortableScheduler(index, Error::PortableSchedulerServiceWakeCommitRejected,
                                     Operation::ServicePortableScheduler,
                                     static_cast<Foundation::UInt64>(failure.WakeQueue.Error));
    }

    const Foundation::UInt64 wakeCount = m_serviceWakeCounts[index].LoadAcquire();
    if (wakeCount == MaximumUInt64)
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceSequenceExhausted,
                                     Operation::ServicePortableScheduler, wakeCount);
    }
    m_serviceWakeCounts[index].StoreRelease(wakeCount + 1U);
    return Error::None;
}

Error Runtime::RecordPortableSchedulerServiceCompletion(const Foundation::Size index,
                                                        const Kernel::Scheduler::CpuId processor) noexcept
{
    const Foundation::UInt64 sequence = m_serviceRequestSequences[index].LoadAcquire();
    const Foundation::UInt64 priorCompletion = m_serviceCompletionSequences[index].LoadAcquire();
    if ((m_serviceMailboxStates[index].LoadAcquire() != ServiceMailboxExecuting) || (sequence == 0U) ||
        (priorCompletion == MaximumUInt64) || (sequence != priorCompletion + 1U))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceCompletionRejected,
                                     Operation::ServicePortableScheduler, sequence);
    }
    if (ValidatePortableSchedulerActiveBinding(index, processor, sequence + 1U, Operation::ServicePortableScheduler) !=
        Error::None)
    {
        return Error::PortableSchedulerActiveBindingRejected;
    }

    PendingServiceCompletion& pending = m_pendingServiceCompletions[index];
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence(&pending.Evidence);
#else
    const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    pending.Evidence = Ia64ApplicationProcessorKernelRuntimeRegisterEvidence{
        .StackPointer =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerAliasVirtualStart(layout) + 0x200U,
        .RseBackingStorePointer =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout) + 0x200U,
        .RseBackingStoreBase =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout),
        .RseNatCollection = 0U,
        .RseConfiguration = 0U,
        .ProcessorStatus = TranslationStateMask | InstructionCollectionMask,
        .CaptureComplete = 1U,
        .Reserved = {},
    };
#endif
    if (!ValidateEvidence(index, pending.Evidence, EvidenceKind::Worker))
    {
        return FailPortableScheduler(index, Error::InvalidRegisterEvidence, Operation::ServicePortableScheduler,
                                     sequence);
    }
    pending.Sequence = sequence;
    pending.Cookie = m_serviceRequestCookies[index].LoadAcquire();
    pending.RunSequence = sequence + 1U;
    pending.Result = ServiceResult::Completed;
    return Error::None;
}

bool Runtime::CaptureServingIdleEvidence(const Foundation::Size index) noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence(&m_servingIdleEvidence[index]);
#else
    const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    m_servingIdleEvidence[index] = Ia64ApplicationProcessorKernelRuntimeRegisterEvidence{
        .StackPointer =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleMemoryStackVirtualStart(layout) + 0x200U,
        .RseBackingStorePointer =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleRseBackingStoreVirtualStart(layout) + 0x200U,
        .RseBackingStoreBase =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleRseBackingStoreVirtualStart(layout),
        .RseNatCollection = 0U,
        .RseConfiguration = 0U,
        .ProcessorStatus = TranslationStateMask | InstructionCollectionMask,
        .CaptureComplete = 1U,
        .Reserved = {},
    };
#endif
    return ValidateEvidence(index, m_servingIdleEvidence[index], EvidenceKind::Idle);
}

bool Runtime::CapturePortableSchedulerOnlineThreadRecord(const Foundation::Size index) noexcept
{
    const Kernel::Scheduler::ThreadSnapshot thread = m_schedulerThreads[index].GetSnapshot();
    const Kernel::Scheduler::SchedulingContextSnapshot context = m_schedulerContexts[index]->GetSnapshot();
    if ((thread.State != Kernel::Scheduler::ThreadState::Blocked) || !thread.BlockToken.IsValid() ||
        !thread.BlockToken.IsEqualTo(m_schedulerBlockTokens[index]) || (thread.RunSequence != 2U) ||
        (thread.ExitCode != 0U) || (context.State != Kernel::Scheduler::SchedulingContextState::Bound))
    {
        return false;
    }
    Ia64ApplicationProcessorPortableSchedulerRecord& record = m_portableSchedulerRecords[index];
    record.ServingBlockToken = thread.BlockToken.GetValue();
    record.ThreadState = thread.State;
    record.SchedulingContextState = context.State;
    return true;
}

bool Runtime::CapturePortableSchedulerOnlineRunRecord(const Foundation::Size index) noexcept
{
    const Kernel::Scheduler::ProcessorRunSnapshot controller = m_schedulerControllers[index]->GetSnapshot();
    const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = m_schedulerRunLoops[index]->GetSnapshot();
    if ((controller.State != Kernel::Scheduler::ProcessorRunState::Idle) ||
        (controller.Mode != Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (controller.Deadline.SelectedDeadlineTicks != 0U) || (controller.Deadline.SelectedSources != 0U) ||
        (runLoop.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) || (runLoop.IdleFailurePending != 0U) ||
        (runLoop.ActiveThread != nullptr) || (runLoop.ActiveProtectionDomain != nullptr) ||
        (runLoop.ActiveAddressSpace != nullptr) || (runLoop.BindingCount != 1U))
    {
        return false;
    }
    Ia64ApplicationProcessorPortableSchedulerRecord& record = m_portableSchedulerRecords[index];
    record.ControllerState = controller.State;
    record.RunLoopState = runLoop.State;
    record.DeadlineState = controller.Deadline.State;
    record.RunLoopBindingCount = static_cast<Foundation::UInt8>(runLoop.BindingCount);
    return true;
}

bool Runtime::CapturePortableSchedulerOnlineTimerRecord(const Foundation::Size index) noexcept
{
    const Kernel::Time::TimerQueueState queue = m_schedulerTimerQueues[index]->GetState();
    const Time::Ia64LocalTimerSnapshot timer = m_localTimers[index].GetSnapshot();
    if ((timer.State != TimerState::Ready) || (timer.Vector != Interrupt::Ia64ExternalInterruptTimerVector) ||
        (timer.Armed != 0U) || (timer.ProgrammedDeadlineTicks != 0U) || (queue.NextDeadlineTicks != 0U) ||
        (queue.ScheduledEntries != 0U) || (queue.PendingEntries != 0U) || (queue.ClaimedEntries != 0U))
    {
        return false;
    }
    Ia64ApplicationProcessorPortableSchedulerRecord& record = m_portableSchedulerRecords[index];
    record.LocalTimerState = timer.State;
    record.TimerQueueScheduledEntries = queue.ScheduledEntries;
    record.TimerQueuePendingEntries = queue.PendingEntries;
    record.TimerQueueClaimedEntries = queue.ClaimedEntries;
    return true;
}

Error Runtime::PublishIdleServing(const Foundation::Size index, const Kernel::Scheduler::CpuId processor,
                                  const Foundation::UInt64 generation) noexcept
{
    PendingServiceCompletion& pending = m_pendingServiceCompletions[index];
    if ((pending.Sequence != 1U) || (pending.RunSequence != 2U) || (pending.Result != ServiceResult::Completed) ||
        !m_initialSchedulerBlockTokens[index].IsValid() || !m_schedulerBlockTokens[index].IsValid() ||
        m_initialSchedulerBlockTokens[index].IsEqualTo(m_schedulerBlockTokens[index]))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceCompletionRejected,
                                     Operation::PublishIdleServing, pending.Sequence);
    }
    if (!CaptureServingIdleEvidence(index))
    {
        return FailPortableScheduler(index, Error::InvalidRegisterEvidence, Operation::PublishIdleServing,
                                     pending.Sequence);
    }

    m_resumedWorkerEntryEvidence[index] = pending.Evidence;
    Ia64ApplicationProcessorPortableSchedulerRecord& record = m_portableSchedulerRecords[index];
    record = {};
    record.AddressSpaceIdentity = m_schedulerRoots[index].AddressSpaceIdentity;
    record.ProtectionDomainIdentity = m_schedulerProtectionDomainIdentities[index];
    record.ThreadIdentity = m_schedulerThreadIdentities[index];
    record.SchedulingContextIdentity = m_schedulerContextIdentities[index];
    record.InitialRunSequence = 1U;
    record.FirstResumedRunSequence = pending.RunSequence;
    record.InitialBlockToken = m_initialSchedulerBlockTokens[index].GetValue();
    record.Processor = processor;
    record.Phase = PortablePhase::Online;
    if (!CapturePortableSchedulerOnlineThreadRecord(index) || !CapturePortableSchedulerOnlineRunRecord(index) ||
        !CapturePortableSchedulerOnlineTimerRecord(index))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerPostconditionRejected,
                                     Operation::PublishIdleServing, pending.Sequence);
    }

    Foundation::UInt64 expectedPhase = static_cast<Foundation::UInt64>(PortablePhase::Waiting);
    if (!m_portableSchedulerStates[index].CompareExchangeStrongAcquireRelease(expectedPhase,
                                                                              PublishingPortableSchedulerOnline))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerPostconditionRejected,
                                     Operation::PublishIdleServing, expectedPhase);
    }
    Foundation::UInt64 expectedLifecycle = static_cast<Foundation::UInt64>(Lifecycle::SchedulerWaiting);
    if (!m_processorStates[index].CompareExchangeStrongAcquireRelease(expectedLifecycle, PublishingIdleServing))
    {
        return FailPortableScheduler(index, Error::InvalidProcessorState, Operation::PublishIdleServing,
                                     expectedLifecycle);
    }

    m_evidencePublications[index].StoreRelease(RuntimeEntryEvidencePublished | FirstWorkerEvidencePublished |
                                               ResumedWorkerEvidencePublished | ServingIdleEvidencePublished);
    m_portableSchedulerStates[index].StoreRelease(static_cast<Foundation::UInt64>(PortablePhase::Online));
    m_processorStates[index].StoreRelease(static_cast<Foundation::UInt64>(Lifecycle::IdleServing));
    const Interrupt::Ia64ApplicationProcessorRuntimeServingEvidence evidence{
        .RuntimeEntryValue = m_runtimeEntryEvidence[index].StackPointer,
        .FirstWorkerEntryValue = m_firstWorkerEntryEvidence[index].StackPointer,
        .ResumedWorkerEntryValue = m_resumedWorkerEntryEvidence[index].StackPointer,
        .ServingIdleValue = m_servingIdleEvidence[index].StackPointer,
        .Reserved = {},
    };
    const ReleaseError releaseError = m_releaseCoordinator->PublishIdleServing(processor, generation, evidence);
    if (releaseError != ReleaseError::None)
    {
        m_portableSchedulerStates[index].StoreRelease(static_cast<Foundation::UInt64>(PortablePhase::Failed));
        return FailProcessor(index, Error::IdleServingPublicationRejected, Operation::PublishIdleServing, releaseError,
                             Ia64KernelContextError::None, pending.Sequence);
    }
    return Error::None;
}

Error Runtime::PublishPortableSchedulerServiceCompletion(const Foundation::Size index,
                                                         const Kernel::Scheduler::CpuId processor,
                                                         const Foundation::UInt64 generation) noexcept
{
    PendingServiceCompletion& pending = m_pendingServiceCompletions[index];
    const Foundation::UInt64 completion = m_serviceCompletionSequences[index].LoadAcquire();
    if ((m_serviceMailboxStates[index].LoadAcquire() != ServiceMailboxExecuting) || (pending.Sequence == 0U) ||
        (completion == MaximumUInt64) || (pending.Sequence != completion + 1U) ||
        (pending.Result != ServiceResult::Completed))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceCompletionRejected,
                                     Operation::ServicePortableScheduler, pending.Sequence);
    }

    if (pending.Sequence == 1U)
    {
        const Error online = PublishIdleServing(index, processor, generation);
        if (online != Error::None)
        {
            return online;
        }
    }
    else if ((m_processorStates[index].LoadAcquire() != static_cast<Foundation::UInt64>(Lifecycle::IdleServing)) ||
             (m_portableSchedulerStates[index].LoadAcquire() != static_cast<Foundation::UInt64>(PortablePhase::Online)))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceCompletionRejected,
                                     Operation::ServicePortableScheduler, pending.Sequence);
    }

    m_serviceCompletionCookies[index].StoreRelease(pending.Cookie);
    m_serviceCompletionResults[index].StoreRelease(static_cast<Foundation::UInt64>(pending.Result));
    m_serviceCompletionEvidenceValues[index].StoreRelease(pending.Evidence.StackPointer);
    m_serviceCompletionRunSequences[index].StoreRelease(pending.RunSequence);
    m_serviceCounts[index].StoreRelease(pending.Sequence);
    m_serviceCompletionSequences[index].StoreRelease(pending.Sequence);
    m_serviceMailboxStates[index].StoreRelease(ServiceMailboxIdle);
    m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
    return Error::None;
}

Error Runtime::ServicePortableSchedulerFromIdle(const Kernel::Scheduler::CpuId processor,
                                                const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::ServicePortableScheduler);
    if (validation != Error::None)
    {
        return validation;
    }
    const Foundation::UInt64 lifecycle = m_processorStates[index].LoadAcquire();
    const Foundation::UInt64 phase = m_portableSchedulerStates[index].LoadAcquire();
    if (!((lifecycle == static_cast<Foundation::UInt64>(Lifecycle::SchedulerWaiting) &&
           phase == static_cast<Foundation::UInt64>(PortablePhase::Waiting)) ||
          (lifecycle == static_cast<Foundation::UInt64>(Lifecycle::IdleServing) &&
           phase == static_cast<Foundation::UInt64>(PortablePhase::Online))))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceUnavailable,
                                     Operation::ServicePortableScheduler, lifecycle);
    }

    const Foundation::UInt64 observedServiceState = m_serviceMailboxStates[index].LoadAcquire();
    const Foundation::UInt64 observedTimerState = m_timerDeliveryStates[index].LoadAcquire();
    if ((observedServiceState == ServiceMailboxPending) && (observedTimerState == TimerDeliveryPending))
    {
        return FailPortableScheduler(index, Error::TimerDeliveryGateConflict, Operation::ServicePortableScheduler,
                                     observedTimerState);
    }
    if ((observedTimerState == TimerDeliveryPublishing) || (observedTimerState == TimerDeliveryPending) ||
        (observedTimerState == TimerDeliveryExecuting))
    {
        return Error::None;
    }

    Foundation::UInt64 expectedState = ServiceMailboxPending;
    if (!m_serviceMailboxStates[index].CompareExchangeStrongAcquireRelease(expectedState, ServiceMailboxExecuting))
    {
        if ((expectedState == ServiceMailboxIdle) || (expectedState == ServiceMailboxPublishing))
        {
            return Error::None;
        }
        return FailPortableScheduler(index, Error::PortableSchedulerServiceUnavailable,
                                     Operation::ServicePortableScheduler, expectedState);
    }

    const Foundation::UInt64 retainedTimerState = m_timerDeliveryStates[index].LoadAcquire();
    if ((m_idleWorkOwners[index].LoadAcquire() != IdleWorkService) ||
        ((retainedTimerState != TimerDeliveryReady) && (retainedTimerState != TimerDeliveryCompleted)) ||
        ((retainedTimerState == TimerDeliveryCompleted) && !ValidateAcknowledgedTimerDeliveryCompletion(index)))
    {
        return FailPortableScheduler(index, Error::TimerDeliveryGateConflict, Operation::ServicePortableScheduler,
                                     m_idleWorkOwners[index].LoadAcquire());
    }

    const Foundation::UInt64 sequence = m_serviceRequestSequences[index].LoadAcquire();
    const Foundation::UInt64 completion = m_serviceCompletionSequences[index].LoadAcquire();
    if ((sequence == 0U) || (completion == MaximumUInt64) || (sequence != completion + 1U) ||
        (sequence == MaximumUInt64))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceCompletionRejected,
                                     Operation::ServicePortableScheduler, sequence);
    }
    const Error woke = WakePortableSchedulerWorker(index);
    if (woke != Error::None)
    {
        return woke;
    }

    m_schedulerIdleReceipts[index] = {};
    m_schedulerIdleFailures[index] = {};
    const bool dispatch =
        m_schedulerRunLoops[index]->DispatchFromIdle(m_schedulerIdleReceipts[index], m_schedulerIdleFailures[index]);
    m_portableSchedulerFailures[index].RunLoop = m_schedulerIdleFailures[index];
    PublishPortableSchedulerFailureSnapshot(index);
    if (dispatch || !ValidateExpectedPortableSchedulerIdleFailure(index) ||
        !ValidatePortableSchedulerBlockedPostconditions(index, sequence + 1U, true))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerDispatchRejected,
                                     Operation::ServicePortableScheduler,
                                     static_cast<Foundation::UInt64>(m_schedulerIdleFailures[index].Error));
    }

    const Foundation::UInt64 dispatchCount = m_serviceDispatchCounts[index].LoadAcquire();
    if (dispatchCount == MaximumUInt64)
    {
        return FailPortableScheduler(index, Error::PortableSchedulerServiceSequenceExhausted,
                                     Operation::ServicePortableScheduler, dispatchCount);
    }
    m_serviceDispatchCounts[index].StoreRelease(dispatchCount + 1U);
    return PublishPortableSchedulerServiceCompletion(index, processor, generation);
}

Error Runtime::ServiceTlbShootdownFromIdle(const Kernel::Scheduler::CpuId processor,
                                           const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation = ValidateProcessorGeneration(processor, generation, index, Operation::ServiceTlbShootdown);
    if (validation != Error::None)
    {
        return validation;
    }
    if ((m_processorStates[index].LoadAcquire() == static_cast<Foundation::UInt64>(Lifecycle::Failed)) ||
        (m_portableSchedulerStates[index].LoadAcquire() == static_cast<Foundation::UInt64>(PortablePhase::Failed)))
    {
        return Error::TlbShootdownServiceUnavailable;
    }

    PortableFailure& failure = m_portableSchedulerFailures[index];
    ::new (static_cast<void*>(&failure.TlbShootdownController))
        Kernel::Memory::ProcessorAddressSpaceControllerSnapshot{};
    ::new (static_cast<void*>(&failure.TlbShootdownControllerAfter))
        Kernel::Memory::ProcessorAddressSpaceControllerSnapshot{};
    ::new (static_cast<void*>(&failure.TlbShootdownAddressSpaceBefore)) Kernel::Memory::AddressSpaceSnapshot{};
    ::new (static_cast<void*>(&failure.TlbShootdownAddressSpaceAfter)) Kernel::Memory::AddressSpaceSnapshot{};
    failure.TlbShootdownDrain = EmptyTlbDrainSummary();
    failure.TlbShootdownPollAttempted = 0U;
    if ((m_processorStates[index].LoadAcquire() != static_cast<Foundation::UInt64>(Lifecycle::IdleServing)) ||
        (m_portableSchedulerStates[index].LoadAcquire() != static_cast<Foundation::UInt64>(PortablePhase::Online)) ||
        (m_tlbShootdownConfiguration.LoadAcquire() != TlbShootdownDependenciesReady) ||
        (m_tlbShootdownNotifier == nullptr) || (m_tlbShootdownMailbox == nullptr) ||
        (m_tlbShootdownDispatchers[index] == nullptr) || (m_tlbShootdownDrains[index] == nullptr) ||
        (m_tlbShootdownMailbox->GetOnlineProcessorMask() != m_plan.EnabledProcessorMask) ||
        (m_schedulerRoots[index].AddressSpaceStorage == nullptr) ||
        (m_schedulerRoots[index].ControllerStorage == nullptr) ||
        (m_schedulerRoots[index].AddressSpaceStorage != m_expectedSharedRoot.AddressSpaceStorage) ||
        !m_schedulerRoots[index].AddressSpaceIdentity.IsEqualTo(m_expectedSharedRoot.AddressSpaceIdentity) ||
        (m_schedulerRoots[index].Processor.GetValue() != processor.GetValue()) ||
        !m_portableSchedulerRecords[index].AddressSpaceIdentity.IsEqualTo(m_expectedSharedRoot.AddressSpaceIdentity) ||
        (m_portableSchedulerRecords[index].Processor.GetValue() != processor.GetValue()) ||
        (m_tlbShootdownDispatchers[index]->GetProcessor().GetValue() != processor.GetValue()) ||
        (m_tlbShootdownDrains[index]->GetProcessor().GetValue() != processor.GetValue()))
    {
        return FailPortableScheduler(index, Error::TlbShootdownServiceUnavailable, Operation::ServiceTlbShootdown,
                                     m_processorStates[index].LoadAcquire());
    }

    const Memory::Ia64LocalTlbInvalidatorSnapshot invalidator = m_tlbShootdownInvalidators[index].GetSnapshot();
    if ((invalidator.State != Memory::Ia64LocalTlbInvalidatorState::Ready) ||
        (invalidator.ImplementedVirtualAddressMostSignificantBit !=
         m_tlbShootdownCapabilities[index].Translation.ImplementedVirtualAddressMostSignificantBit) ||
        !ReservedBytesAreZero(invalidator.Reserved, sizeof(invalidator.Reserved)))
    {
        return FailPortableScheduler(index, Error::TlbShootdownServiceUnavailable, Operation::ServiceTlbShootdown,
                                     static_cast<Foundation::UInt64>(invalidator.State));
    }

    const auto& controller = *::new (static_cast<void*>(&failure.TlbShootdownController))
                                 Kernel::Memory::ProcessorAddressSpaceControllerSnapshot{
                                     m_schedulerRoots[index].ControllerStorage->GetSnapshot()};
    if (!Detail::FixedRootControllerMatches(controller, m_schedulerRoots[index], processor))
    {
        return FailPortableScheduler(index, Error::TlbShootdownFixedRootRejected, Operation::ServiceTlbShootdown,
                                     static_cast<Foundation::UInt64>(controller.State));
    }

    failure.TlbShootdownPollAttempted = 1U;
    const TlbPollResult poll = m_tlbShootdownNotifier->Poll(processor);
    failure.TlbShootdownPollResult = poll;
    if (poll == TlbPollResult::Empty)
    {
        return Error::None;
    }
    if (poll != TlbPollResult::Pending)
    {
        return FailPortableScheduler(index, Error::TlbShootdownPollRejected, Operation::ServiceTlbShootdown,
                                     static_cast<Foundation::UInt64>(poll));
    }
    const auto& addressSpaceBefore =
        *::new (static_cast<void*>(&failure.TlbShootdownAddressSpaceBefore))
            Kernel::Memory::AddressSpaceSnapshot{m_schedulerRoots[index].AddressSpaceStorage->GetSnapshot()};
    // Common progress may already have drained this notification's work. The initiator can then
    // change roots, so an empty coalesced notification cannot require its old
    // residency/reservation.
    if (m_tlbShootdownMailbox->HasPendingWork(processor) &&
        !FixedRootAddressSpaceMatches(addressSpaceBefore, m_schedulerRoots[index], m_plan.EnabledProcessorMask))
    {
        return FailPortableScheduler(index, Error::TlbShootdownFixedRootRejected, Operation::ServiceTlbShootdown,
                                     static_cast<Foundation::UInt64>(addressSpaceBefore.State));
    }

    // The notifier coalesces notifications and may leave one latched after
    // an earlier pass already drained its work, so neither the shootdown
    // state nor the sequence the root shows here names the work this pass
    // finds: the pass may be empty or cover several back-to-back shootdowns.
    // The mailbox is authoritative for the work; the initiator, not this AP,
    // owns completion.
    const TlbDrainSummary drain = m_tlbShootdownDrains[index]->Drain();
    failure.TlbShootdownDrain = drain;
    if (!Detail::ValidateTlbShootdownDrainSummary(drain, processor))
    {
        return FailPortableScheduler(index, Error::TlbShootdownDrainRejected, Operation::ServiceTlbShootdown,
                                     EncodeTlbDrainFailureDetail(drain));
    }
    const auto& controllerAfter = *::new (static_cast<void*>(&failure.TlbShootdownControllerAfter))
                                      Kernel::Memory::ProcessorAddressSpaceControllerSnapshot{
                                          m_schedulerRoots[index].ControllerStorage->GetSnapshot()};
    if (!Detail::FixedRootControllerMatches(controllerAfter, m_schedulerRoots[index], processor))
    {
        return FailPortableScheduler(index, Error::TlbShootdownFixedRootRejected, Operation::ServiceTlbShootdown,
                                     static_cast<Foundation::UInt64>(controllerAfter.State));
    }
    const auto& addressSpaceAfter =
        *::new (static_cast<void*>(&failure.TlbShootdownAddressSpaceAfter))
            Kernel::Memory::AddressSpaceSnapshot{m_schedulerRoots[index].AddressSpaceStorage->GetSnapshot()};
    // Once this CPU acknowledges, another CPU may complete and detach before
    // this snapshot. The dispatcher validated the work and the controller
    // above still proves our own active root; other CPUs' state is diagnostic.
    static_cast<void>(addressSpaceAfter);
    PublishPortableSchedulerFailureSnapshot(index);
    return Error::None;
}

namespace Detail
{
bool TryComputeTimerDeliveryDeadlines(const Foundation::UInt64 observedTicks, Foundation::UInt64& deadlineTicks,
                                      Foundation::UInt64& pollDeadlineTicks) noexcept
{
    if (observedTicks >= (MaximumUInt64 - TimerDeliveryDeadlineLeadTicks - LocalTimerPollGraceTicks))
    {
        return false;
    }
    deadlineTicks = observedTicks + TimerDeliveryDeadlineLeadTicks;
    pollDeadlineTicks = deadlineTicks + LocalTimerPollGraceTicks;
    return true;
}
} // Namespace Detail

bool Runtime::ArmTimerDelivery(const Foundation::Size index) noexcept
{
    PortableFailure& failure = m_portableSchedulerFailures[index];
    Ia64ApplicationProcessorTimerDeliveryRecord& record = m_timerDeliveryRecords[index];
    const Foundation::UInt64 observedTicks = m_localTimers[index].GetTicks();
    if (observedTicks == MaximumUInt64)
    {
        failure.Timer = Kernel::Time::TimerError::ClockExhausted;
        return false;
    }
    Foundation::UInt64 deadlineTicks{0U};
    Foundation::UInt64 pollDeadlineTicks{0U};
    if (!Detail::TryComputeTimerDeliveryDeadlines(observedTicks, deadlineTicks, pollDeadlineTicks))
    {
        failure.Timer = Kernel::Time::TimerError::InvalidDeadline;
        return false;
    }

    const auto armed =
        m_schedulerTimerQueues[index]->Arm(m_timerDeliveryTimerIdentities[index], *m_timerDeliveryTimers[index],
                                           Kernel::Object::MonotonicTime{deadlineTicks});
    if (!armed.HasValue())
    {
        failure.TimerQueue = armed.GetErrorReference();
        return false;
    }
    m_timerDeliveryArmReceipts[index] = armed.GetValue();
    record.Arm = m_timerDeliveryArmReceipts[index];
    record.ArmedQueue = m_schedulerTimerQueues[index]->GetState();

    const auto programmed = m_schedulerControllers[index]->RefreshTimerQueueDeadline(record.Arm.Timer.ObservedTicks);
    if (!programmed.HasValue())
    {
        failure.Controller = programmed.GetErrorReference();
        return false;
    }
    m_timerDeliveryArmedDeadlineReceipts[index] = programmed.GetValue();
    record.ArmedDeadline = m_timerDeliveryArmedDeadlineReceipts[index];
    record.ArmedLocalTimer = m_localTimers[index].GetSnapshot();
    m_timerDeliveryPollDeadlines[index] = pollDeadlineTicks;
    record.PollDeadlineTicks = pollDeadlineTicks;

    const Kernel::Time::TimerSnapshot timer = m_timerDeliveryTimers[index]->GetSnapshot();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = m_schedulerControllers[index]->GetSnapshot();
    return record.Arm.Timer.Token.IsValid() && (record.Arm.Timer.Token.GetSequence() == 1U) &&
           (record.Arm.Timer.DeadlineTicks == deadlineTicks) && (record.Arm.Timer.ObservedTicks >= observedTicks) &&
           (record.Arm.Timer.ObservedTicks < deadlineTicks) && !record.Arm.Timer.Replaced &&
           (record.Arm.NextDeadlineTicks == deadlineTicks) && (record.ArmedQueue.NextDeadlineTicks == deadlineTicks) &&
           (record.ArmedQueue.NextClaimSequence == 1U) && (record.ArmedQueue.ScheduledEntries == 1U) &&
           (record.ArmedQueue.PendingEntries == 0U) && (record.ArmedQueue.ClaimedEntries == 0U) &&
           timer.Token.IsEqualTo(record.Arm.Timer.Token) && (timer.DeadlineTicks == deadlineTicks) &&
           (timer.State == Kernel::Time::TimerState::Armed) &&
           (record.ArmedDeadline.SelectedDeadlineTicks == deadlineTicks) &&
           (record.ArmedDeadline.State == Kernel::Time::ProcessorDeadlineState::Armed) &&
           (record.ArmedDeadline.SelectedSources ==
            static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue)) &&
           (record.ArmedDeadline.Reprogrammed == 1U) &&
           (record.ArmedDeadline.ObservedTicks >= record.Arm.Timer.ObservedTicks) &&
           (record.ArmedDeadline.ObservedTicks < deadlineTicks) &&
           (record.ArmedLocalTimer.State == TimerState::Ready) &&
           (record.ArmedLocalTimer.Vector == Interrupt::Ia64ExternalInterruptTimerVector) &&
           (record.ArmedLocalTimer.Armed == 1U) && (record.ArmedLocalTimer.ProgrammedDeadlineTicks == deadlineTicks) &&
           (controller.State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (controller.Mode == Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) &&
           (controller.TimerQueue.NextDeadlineTicks == deadlineTicks) &&
           (controller.TimerQueue.ScheduledEntries == 1U) && (controller.TimerQueue.PendingEntries == 0U) &&
           (controller.TimerQueue.ClaimedEntries == 0U) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Armed) &&
           (controller.Deadline.SelectedDeadlineTicks == deadlineTicks) &&
           (controller.Deadline.SelectedSources ==
            static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue));
}

Error Runtime::BeginTimerDeliveryFromIdle(const Kernel::Scheduler::CpuId processor,
                                          const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation = ValidateProcessorGeneration(processor, generation, index, Operation::BeginTimerDelivery);
    if (validation != Error::None)
    {
        return validation;
    }
    const Foundation::UInt64 serviceState = m_serviceMailboxStates[index].LoadAcquire();
    const Foundation::UInt64 timerState = m_timerDeliveryStates[index].LoadAcquire();
    if ((serviceState == ServiceMailboxPending) && (timerState == TimerDeliveryPending))
    {
        return FailArmedTimerDelivery(index, Error::TimerDeliveryGateConflict, Operation::BeginTimerDelivery,
                                      serviceState);
    }
    if ((m_idleWorkOwners[index].LoadAcquire() != IdleWorkTimerDelivery) || (serviceState != ServiceMailboxIdle) ||
        !ValidateTimerDeliveryRequestPreconditions(index) || !ValidateAcknowledgedTimerDeliveryGate(index))
    {
        return FailArmedTimerDelivery(index, Error::TimerDeliveryUnavailable, Operation::BeginTimerDelivery,
                                      timerState);
    }

    Foundation::UInt64 expectedState = TimerDeliveryPending;
    if (!m_timerDeliveryStates[index].CompareExchangeStrongAcquireRelease(expectedState, TimerDeliveryExecuting))
    {
        if ((expectedState == TimerDeliveryReady) || (expectedState == TimerDeliveryPublishing) ||
            (expectedState == TimerDeliveryCompleted))
        {
            return Error::None;
        }
        return FailArmedTimerDelivery(index, Error::TimerDeliveryUnavailable, Operation::BeginTimerDelivery,
                                      expectedState);
    }
    if ((m_timerDeliveryRequestSequences[index].LoadAcquire() != 1U) ||
        (m_timerDeliveryCompletionSequences[index].LoadAcquire() != 0U))
    {
        return FailArmedTimerDelivery(index, Error::TimerDeliverySequenceExhausted, Operation::BeginTimerDelivery,
                                      m_timerDeliveryRequestSequences[index].LoadAcquire());
    }

    Ia64ApplicationProcessorTimerDeliveryRecord& record = m_timerDeliveryRecords[index];
    record = EmptyTimerDeliveryRecord;
    record.Registration = m_timerDeliveryRegistrations[index];
    record.TimerStorageAddress = AddressOf(m_timerDeliveryTimers[index]);
    record.NotificationStorageAddress = AddressOf(m_timerDeliveryNotifications[index]);
    record.EvidenceAddress = AddressOf(m_localTimerEvidence[index]);
    record.Processor = processor;
    if (!ArmTimerDelivery(index))
    {
        Error error{Error::TimerDeliveryPostconditionRejected};
        if (m_portableSchedulerFailures[index].TimerQueue.Error != Kernel::Time::TimerQueueError::None)
        {
            error = Error::TimerDeliveryArmRejected;
        }
        else if (m_portableSchedulerFailures[index].Controller.Error != Kernel::Scheduler::ProcessorRunError::None)
        {
            error = Error::TimerDeliveryDeadlineProgramRejected;
        }
        else if (m_portableSchedulerFailures[index].Timer == Kernel::Time::TimerError::ClockExhausted)
        {
            error = Error::TimerDeliveryCounterUnavailable;
        }
        else if (m_portableSchedulerFailures[index].Timer == Kernel::Time::TimerError::InvalidDeadline)
        {
            error = Error::TimerDeliveryDeadlineOverflow;
        }
        if (m_timerDeliveryArmReceipts[index].Timer.Token.IsValid())
        {
            return FailArmedTimerDelivery(index, error, Operation::BeginTimerDelivery, record.PollDeadlineTicks);
        }
        return FailPortableScheduler(index, error, Operation::BeginTimerDelivery, record.PollDeadlineTicks);
    }
    return Error::None;
}

void Runtime::SettleArmedTimerDelivery(const Foundation::Size index) noexcept
{
    Ia64ApplicationProcessorTimerDeliverySettlement& settlement =
        m_portableSchedulerFailures[index].TimerDeliverySettlement;
    settlement = {};

    const Kernel::Time::TimerQueueState initialQueue = m_schedulerTimerQueues[index]->GetState();
    const Kernel::Time::TimerSnapshot initialTimer = m_timerDeliveryTimers[index]->GetSnapshot();
    if ((initialQueue.NextDeadlineTicks != 0U) || (initialQueue.ScheduledEntries != 0U) ||
        (initialQueue.PendingEntries != 0U) || (initialQueue.ClaimedEntries != 0U) || initialTimer.Token.IsValid() ||
        (initialTimer.State != Kernel::Time::TimerState::Disarmed))
    {
        settlement.QueueCancelAttempted = 1U;
        const auto cancelled =
            m_schedulerTimerQueues[index]->Cancel(m_timerDeliveryTimerIdentities[index], *m_timerDeliveryTimers[index],
                                                  m_timerDeliveryArmReceipts[index].Timer.Token);
        if (!cancelled.HasValue())
        {
            settlement.QueueCancelError = cancelled.GetErrorReference().Error;
        }
    }

    bool controllerRefreshed{false};
    settlement.ControllerRefreshAttempted = 1U;
    {
        const auto refreshed = m_schedulerControllers[index]->RefreshTimerQueueDeadline(
            m_timerDeliveryArmReceipts[index].Timer.ObservedTicks);
        if (refreshed.HasValue())
        {
            settlement.ControllerRefresh = refreshed.GetValue();
            controllerRefreshed = true;
        }
        else
        {
            settlement.ControllerRefreshError = refreshed.GetErrorReference().Error;
        }
    }

    const Time::Ia64LocalTimerSnapshot afterRefresh = m_localTimers[index].GetSnapshot();
    if (!controllerRefreshed || (afterRefresh.State != TimerState::Ready) || (afterRefresh.Armed != 0U) ||
        (afterRefresh.ProgrammedDeadlineTicks != 0U))
    {
        settlement.LocalTimerCancelAttempted = 1U;
        settlement.LocalTimerCancelError = m_localTimers[index].CancelDeadline();
    }

    settlement.FinalQueue = m_schedulerTimerQueues[index]->GetState();
    settlement.FinalTimer = m_timerDeliveryTimers[index]->GetSnapshot();
    settlement.FinalLocalTimer = m_localTimers[index].GetSnapshot();
    settlement.QueueEmptyProven = static_cast<Foundation::UInt8>(
        (settlement.FinalQueue.NextDeadlineTicks == 0U) && (settlement.FinalQueue.ScheduledEntries == 0U) &&
        (settlement.FinalQueue.PendingEntries == 0U) && (settlement.FinalQueue.ClaimedEntries == 0U));
    settlement.TimerDisarmedProven = static_cast<Foundation::UInt8>(
        (settlement.FinalTimer.State == Kernel::Time::TimerState::Disarmed) && !settlement.FinalTimer.Token.IsValid() &&
        (settlement.FinalTimer.DeadlineTicks == 0U));
    settlement.ControllerDisarmedProven = static_cast<Foundation::UInt8>(
        controllerRefreshed && (settlement.ControllerRefresh.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
        (settlement.ControllerRefresh.SelectedDeadlineTicks == 0U) &&
        (settlement.ControllerRefresh.SelectedSources == 0U));
    settlement.LocalTimerDisarmedProven = static_cast<Foundation::UInt8>(
        (settlement.FinalLocalTimer.State == TimerState::Ready) && (settlement.FinalLocalTimer.Armed == 0U) &&
        (settlement.FinalLocalTimer.ProgrammedDeadlineTicks == 0U));
}

Error Runtime::FailArmedTimerDelivery(const Foundation::Size index, const Error error, const Operation operation,
                                      const Foundation::UInt64 detail) noexcept
{
    if (m_timerDeliveryArmReceipts[index].Timer.Token.IsValid())
    {
        SettleArmedTimerDelivery(index);
    }
    return FailPortableScheduler(index, error, operation, detail);
}

bool Runtime::CaptureTimerDeliveryInterruptEvidence(const Foundation::Size index, const PollStatus pollStatus,
                                                    const PollRange pollCodeRange) noexcept
{
    if ((pollStatus != PollStatus::Completed) || (pollCodeRange.Start >= pollCodeRange.End))
    {
        return false;
    }
    Ia64ApplicationProcessorTimerDeliveryRecord& record = m_timerDeliveryRecords[index];
    const Interrupt::Ia64LocalTimerInterruptEvidence acquired =
        Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(*m_localTimerEvidence[index], 2U);
    const ValidationError validation = Interrupt::ValidateIa64LocalTimerInterruptEvidenceForSequence(
        acquired, 2U, static_cast<Foundation::UInt8>(index + 1U), record.Arm.Timer.DeadlineTicks, pollCodeRange.Start,
        pollCodeRange.End);
    if (validation != ValidationError::None)
    {
        return false;
    }
    record.InterruptEvidence = acquired;
    record.PollCodeRange = pollCodeRange;
    record.PollStatus = pollStatus;
    return (acquired.Sequence == 2U) && (acquired.AcknowledgedSequence == 1U) &&
           (acquired.ProgrammedDeadlineTicks == record.Arm.Timer.DeadlineTicks) &&
           (acquired.ObservedIntervalTimeCounter >= acquired.ProgrammedDeadlineTicks) &&
           (acquired.InterruptedInstructionPointer >= pollCodeRange.Start) &&
           (acquired.InterruptedInstructionPointer < pollCodeRange.End) && (acquired.InterruptedIfsValid == 0U) &&
           (acquired.ProcessorIndex == static_cast<Foundation::UInt8>(index + 1U)) &&
           (acquired.Vector == Interrupt::Ia64ExternalInterruptTimerVector) &&
           ReservedBytesAreZero(acquired.Reserved, sizeof(acquired.Reserved));
}

bool Runtime::DrainTimerDelivery(const Foundation::Size index) noexcept
{
    Ia64ApplicationProcessorTimerDeliveryRecord& record = m_timerDeliveryRecords[index];
    PortableFailure& failure = m_portableSchedulerFailures[index];
    const Kernel::Time::DeferredTimerDrainOutcome outcome = m_deferredTimerWorkers[index]->DrainDueRetained();
    if (outcome != Kernel::Time::DeferredTimerDrainOutcome::Drained)
    {
        failure.DeferredTimerWorker = m_deferredTimerWorkers[index]->GetRetainedDrainFailure();
        return false;
    }
    record.Drain = m_deferredTimerWorkers[index]->GetRetainedDrainReceipt();
    record.Delivery = m_deferredTimerWorkers[index]->GetLastDeliveryReceipt();
    record.FinalWorker = m_deferredTimerWorkers[index]->GetSnapshot();
    record.FinalTimer = m_timerDeliveryTimers[index]->GetSnapshot();

    return record.Drain.FirstTimerIdentity.IsEqualTo(m_timerDeliveryTimerIdentities[index]) &&
           record.Drain.LastTimerIdentity.IsEqualTo(m_timerDeliveryTimerIdentities[index]) &&
           (record.Drain.FirstClaimSequence == 1U) && (record.Drain.LastClaimSequence == 1U) &&
           (record.Drain.Stop == Kernel::Time::DeferredTimerDrainStop::Empty) && (record.Drain.DeliveredCount == 1U) &&
           (record.Drain.State == Kernel::Time::DeferredTimerWorkerState::Ready) &&
           (record.Drain.QueueState.NextDeadlineTicks == 0U) && (record.Drain.QueueState.NextClaimSequence == 2U) &&
           (record.Drain.QueueState.ScheduledEntries == 0U) && (record.Drain.QueueState.PendingEntries == 0U) &&
           (record.Drain.QueueState.ClaimedEntries == 0U) &&
           record.Delivery.TimerIdentity.IsEqualTo(m_timerDeliveryTimerIdentities[index]) &&
           record.Delivery.NotificationIdentity.IsEqualTo(m_timerDeliveryNotificationIdentities[index]) &&
           record.Delivery.Expiration.Token.IsEqualTo(record.Arm.Timer.Token) &&
           (record.Delivery.Expiration.DeadlineTicks == record.Arm.Timer.DeadlineTicks) &&
           (record.Delivery.Expiration.ObservedTicks >= record.Delivery.Expiration.DeadlineTicks) &&
           (record.Delivery.Expiration.ObservedTicks >= record.InterruptEvidence.ObservedIntervalTimeCounter) &&
           (record.Delivery.ClaimSequence == 1U) && (record.Delivery.NotificationPublished == 1U) &&
           (record.Delivery.QueueCommitted == 1U) && (record.Delivery.QueueState.NextDeadlineTicks == 0U) &&
           (record.Delivery.QueueState.NextClaimSequence == 2U) &&
           (record.Delivery.QueueState.ScheduledEntries == 0U) && (record.Delivery.QueueState.PendingEntries == 0U) &&
           (record.Delivery.QueueState.ClaimedEntries == 0U) &&
           (record.FinalTimer.State == Kernel::Time::TimerState::Disarmed) && !record.FinalTimer.Token.IsValid() &&
           (record.FinalTimer.DeadlineTicks == 0U) &&
           (record.FinalWorker.State == Kernel::Time::DeferredTimerWorkerState::Ready) &&
           (record.FinalWorker.RegisteredBindings == 1U) && (record.FinalWorker.LastRegistrationSequence == 1U) &&
           (record.FinalWorker.QueueState.NextDeadlineTicks == 0U) &&
           (record.FinalWorker.QueueState.NextClaimSequence == 2U) &&
           (record.FinalWorker.QueueState.ScheduledEntries == 0U) &&
           (record.FinalWorker.QueueState.PendingEntries == 0U) && (record.FinalWorker.QueueState.ClaimedEntries == 0U);
}

namespace Detail
{
Error ConsumeTimerDeliveryNotification(Kernel::Ipc::Notification& notification,
                                       Ia64ApplicationProcessorTimerDeliveryRecord& record,
                                       Kernel::Ipc::NotificationFailure& failure) noexcept
{
    const auto claimed = notification.Claim();
    if (!claimed.HasValue())
    {
        failure = claimed.GetErrorReference();
        return Error::TimerDeliveryNotificationClaimRejected;
    }
    record.NotificationClaim = claimed.GetValue();
    record.ClaimedNotification = notification.GetSnapshot();
    if (!record.NotificationClaim.IsValid() || (record.NotificationClaim.GetSequence() != 1U) ||
        (record.ClaimedNotification.State != Kernel::Ipc::NotificationState::Claimed) ||
        (record.ClaimedNotification.Pending != 0U) || (record.ClaimedNotification.LastClaimSequence != 1U))
    {
        return Error::TimerDeliveryPostconditionRejected;
    }

    const auto committed = notification.CommitClaim(record.NotificationClaim);
    if (!committed.HasValue())
    {
        failure = committed.GetErrorReference();
        return Error::TimerDeliveryNotificationCommitRejected;
    }
    record.FinalNotification = committed.GetValue();
    if ((record.FinalNotification.State != Kernel::Ipc::NotificationState::Ready) ||
        (record.FinalNotification.Pending != 0U) || (record.FinalNotification.LastClaimSequence != 1U))
    {
        return Error::TimerDeliveryPostconditionRejected;
    }
    return Error::None;
}
} // Namespace Detail

bool Runtime::ReconcileTimerDeliveryDeadline(const Foundation::Size index) noexcept
{
    PortableFailure& failure = m_portableSchedulerFailures[index];
    const auto reconciled = m_schedulerControllers[index]->RefreshTimerQueueDeadline(
        m_timerDeliveryRecords[index].Delivery.Expiration.ObservedTicks);
    if (!reconciled.HasValue())
    {
        failure.Controller = reconciled.GetErrorReference();
        return false;
    }
    m_timerDeliveryFinalDeadlineReceipts[index] = reconciled.GetValue();
    Ia64ApplicationProcessorTimerDeliveryRecord& record = m_timerDeliveryRecords[index];
    record.FinalDeadline = m_timerDeliveryFinalDeadlineReceipts[index];
    record.FinalLocalTimer = m_localTimers[index].GetSnapshot();
    return (record.FinalDeadline.SelectedDeadlineTicks == 0U) &&
           (record.FinalDeadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (record.FinalDeadline.SelectedSources == 0U) && (record.FinalDeadline.Reprogrammed == 1U) &&
           (record.FinalDeadline.ObservedTicks >= record.Delivery.Expiration.ObservedTicks) &&
           (record.FinalLocalTimer.State == TimerState::Ready) &&
           (record.FinalLocalTimer.Vector == Interrupt::Ia64ExternalInterruptTimerVector) &&
           (record.FinalLocalTimer.Armed == 0U) && (record.FinalLocalTimer.ProgrammedDeadlineTicks == 0U);
}

bool Runtime::CaptureTimerDeliveryFinalSchedulerState(const Foundation::Size index) noexcept
{
    Ia64ApplicationProcessorTimerDeliveryRecord& record = m_timerDeliveryRecords[index];
    record.FinalController = m_schedulerControllers[index]->GetSnapshot();
    record.FinalRunLoop = m_schedulerRunLoops[index]->GetSnapshot();
    record.FinalThread = m_schedulerThreads[index].GetSnapshot();
    record.FinalSchedulingContext = m_schedulerContexts[index]->GetSnapshot();
    record.FinalService = Ia64ApplicationProcessorPortableSchedulerServiceSnapshot{
        .RequestSequence = m_serviceRequestSequences[index].LoadAcquire(),
        .CompletionSequence = m_serviceCompletionSequences[index].LoadAcquire(),
        .RequestCookie = m_serviceRequestCookies[index].LoadAcquire(),
        .CompletionCookie = m_serviceCompletionCookies[index].LoadAcquire(),
        .CompletionEvidenceValue = m_serviceCompletionEvidenceValues[index].LoadAcquire(),
        .CompletionRunSequence = m_serviceCompletionRunSequences[index].LoadAcquire(),
        .DispatchCount = m_serviceDispatchCounts[index].LoadAcquire(),
        .WakeCount = m_serviceWakeCounts[index].LoadAcquire(),
        .ServiceCount = m_serviceCounts[index].LoadAcquire(),
        .State = DecodePortableSchedulerServiceState(m_serviceMailboxStates[index].LoadAcquire()),
        .Result =
            m_serviceCompletionResults[index].LoadAcquire() == static_cast<Foundation::UInt64>(ServiceResult::Completed)
                ? ServiceResult::Completed
                : ServiceResult::None,
        .Reserved = {},
    };
    const Interrupt::Ia64LocalTimerInterruptEvidence finalEvidence =
        Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence(*m_localTimerEvidence[index], 2U);
    return ValidateTimerDeliverySchedulerState(index, 2U) &&
           (record.Delivery.NotificationState.State == Kernel::Ipc::NotificationState::Ready) &&
           (record.Delivery.NotificationState.Pending == 1U) &&
           (record.Delivery.NotificationState.LastClaimSequence == 0U) &&
           (record.FinalController.TimerQueue.NextDeadlineTicks == 0U) &&
           (record.FinalController.TimerQueue.NextClaimSequence == 2U) &&
           (record.FinalController.TimerQueue.ScheduledEntries == 0U) &&
           (record.FinalController.TimerQueue.PendingEntries == 0U) &&
           (record.FinalController.TimerQueue.ClaimedEntries == 0U) &&
           (record.FinalController.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Idle) &&
           (record.FinalController.Scheduler.Queue.NextSequence == 3U) &&
           (record.FinalController.Scheduler.Queue.ReadyEntries == 0U) &&
           (record.FinalController.Scheduler.Queue.ClaimedEntries == 0U) &&
           (record.FinalController.Scheduler.Queue.ReservedEntries == 0U) &&
           !record.FinalController.Scheduler.ActiveThread.IsValid() &&
           !record.FinalController.Scheduler.ActiveContext.IsValid() &&
           (record.FinalController.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (record.FinalController.Deadline.SelectedDeadlineTicks == 0U) &&
           (record.FinalController.Deadline.SelectedSources == 0U) &&
           (record.FinalRunLoop.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) &&
           (record.FinalRunLoop.BindingCount == 1U) &&
           (record.FinalThread.State == Kernel::Scheduler::ThreadState::Blocked) &&
           (record.FinalThread.RunSequence == 2U) &&
           record.FinalThread.BlockToken.IsEqualTo(m_schedulerBlockTokens[index]) &&
           (record.FinalSchedulingContext.State == Kernel::Scheduler::SchedulingContextState::Bound) &&
           record.FinalSchedulingContext.BaseThread.IsEqualTo(m_schedulerThreadIdentities[index]) &&
           record.FinalSchedulingContext.CurrentThread.IsEqualTo(m_schedulerThreadIdentities[index]) &&
           !record.FinalSchedulingContext.RunningProcessor.IsValid() &&
           (record.FinalSchedulingContext.RunStartTicks == 0U) &&
           (record.FinalSchedulingContext.RemainingBudgetTicks != 0U) &&
           (record.FinalSchedulingContext.NextReplenishmentTicks ==
            record.FinalSchedulingContext.AbsoluteDeadlineTicks) &&
           (record.FinalSchedulingContext.DonationDepth == 0U) &&
           (record.FinalSchedulingContext.IsBudgetExhausted == 0U) && (record.FinalService.RequestSequence == 1U) &&
           (record.FinalService.CompletionSequence == 1U) && (record.FinalService.CompletionRunSequence == 2U) &&
           (record.FinalService.DispatchCount == 2U) && (record.FinalService.WakeCount == 1U) &&
           (record.FinalService.ServiceCount == 1U) && (record.FinalService.State == ServiceState::Idle) &&
           (record.FinalService.Result == ServiceResult::Completed) &&
           (finalEvidence.Sequence == record.InterruptEvidence.Sequence) &&
           (finalEvidence.AcknowledgedSequence == 1U) &&
           (finalEvidence.ProgrammedDeadlineTicks == record.InterruptEvidence.ProgrammedDeadlineTicks) &&
           (finalEvidence.ObservedIntervalTimeCounter == record.InterruptEvidence.ObservedIntervalTimeCounter) &&
           (finalEvidence.InterruptedInstructionPointer == record.InterruptEvidence.InterruptedInstructionPointer) &&
           (finalEvidence.InterruptedProcessorStatus == record.InterruptEvidence.InterruptedProcessorStatus) &&
           (finalEvidence.InterruptedIfsValid == 0U) &&
           (finalEvidence.ProcessorIndex == record.InterruptEvidence.ProcessorIndex) &&
           (finalEvidence.Vector == record.InterruptEvidence.Vector) &&
           ReservedBytesAreZero(finalEvidence.Reserved, sizeof(finalEvidence.Reserved));
}

Error Runtime::PublishTimerDeliveryCompletion(const Foundation::Size index,
                                              const Kernel::Scheduler::CpuId processor) noexcept
{
    if ((m_idleWorkOwners[index].LoadAcquire() != IdleWorkTimerDelivery) ||
        (m_timerDeliveryStates[index].LoadAcquire() != TimerDeliveryExecuting) ||
        (m_timerDeliveryRequestSequences[index].LoadAcquire() != 1U) ||
        (m_timerDeliveryCompletionSequences[index].LoadAcquire() != 0U))
    {
        return FailArmedTimerDelivery(index, Error::TimerDeliveryPostconditionRejected,
                                      Operation::CompleteTimerDelivery,
                                      m_timerDeliveryCompletionSequences[index].LoadAcquire());
    }
    Ia64ApplicationProcessorTimerDeliveryRecord& record = m_timerDeliveryRecords[index];
    record.Processor = processor;
    record.Result = TimerDeliveryResult::Delivered;
    m_timerDeliveryResults[index].StoreRelease(static_cast<Foundation::UInt64>(TimerDeliveryResult::Delivered));
    m_timerDeliveryStates[index].StoreRelease(TimerDeliveryCompleted);
    // This release is authoritative for the complete plain record and every
    // preceding result/state word. Sequence two remains unacknowledged until
    // the BSP validates this record.
    m_timerDeliveryCompletionSequences[index].StoreRelease(1U);
    m_idleWorkOwners[index].StoreRelease(IdleWorkAvailable);
    return Error::None;
}

Error Runtime::CompleteTimerDeliveryFromIdle(const Kernel::Scheduler::CpuId processor,
                                             const Foundation::UInt64 generation, const PollStatus pollStatus,
                                             const PollRange pollCodeRange) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::CompleteTimerDelivery);
    if (validation != Error::None)
    {
        return validation;
    }
    if ((m_timerDeliveryStates[index].LoadAcquire() != TimerDeliveryExecuting) ||
        (m_idleWorkOwners[index].LoadAcquire() != IdleWorkTimerDelivery) ||
        (m_serviceMailboxStates[index].LoadAcquire() != ServiceMailboxIdle))
    {
        return FailArmedTimerDelivery(index, Error::TimerDeliveryUnavailable, Operation::CompleteTimerDelivery,
                                      m_timerDeliveryStates[index].LoadAcquire());
    }

    if (!CaptureTimerDeliveryInterruptEvidence(index, pollStatus, pollCodeRange))
    {
        return FailArmedTimerDelivery(index,
                                      pollStatus == PollStatus::Completed ? Error::TimerDeliveryEvidenceRejected
                                                                          : Error::TimerDeliveryPollRejected,
                                      Operation::CompleteTimerDelivery, static_cast<Foundation::UInt64>(pollStatus));
    }
    if (!DrainTimerDelivery(index))
    {
        return FailArmedTimerDelivery(
            index, Error::TimerDeliveryDrainRejected, Operation::CompleteTimerDelivery,
            static_cast<Foundation::UInt64>(m_portableSchedulerFailures[index].DeferredTimerWorker.Error));
    }
    const Error consumed =
        Detail::ConsumeTimerDeliveryNotification(*m_timerDeliveryNotifications[index], m_timerDeliveryRecords[index],
                                                 m_portableSchedulerFailures[index].Notification);
    if (consumed != Error::None)
    {
        return FailArmedTimerDelivery(
            index, consumed, Operation::CompleteTimerDelivery,
            static_cast<Foundation::UInt64>(m_portableSchedulerFailures[index].Notification.Error));
    }
    if (!ReconcileTimerDeliveryDeadline(index))
    {
        return FailArmedTimerDelivery(
            index, Error::TimerDeliveryDeadlineReconcileRejected, Operation::CompleteTimerDelivery,
            static_cast<Foundation::UInt64>(m_portableSchedulerFailures[index].Controller.Error));
    }
    if (!CaptureTimerDeliveryFinalSchedulerState(index))
    {
        return FailArmedTimerDelivery(index, Error::TimerDeliveryPostconditionRejected,
                                      Operation::CompleteTimerDelivery, 1U);
    }
    return PublishTimerDeliveryCompletion(index, processor);
}

Error Runtime::FinishPortableScheduler(const Kernel::Scheduler::CpuId processor,
                                       const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation =
        ValidateProcessorGeneration(processor, generation, index, Operation::FinishPortableScheduler);
    if (validation != Error::None)
    {
        return validation;
    }

    const Foundation::UInt64 requestState = m_serviceMailboxStates[index].LoadAcquire();
    const Foundation::UInt64 requestSequence = m_serviceRequestSequences[index].LoadAcquire();
    const Foundation::UInt64 expectedRunSequence = requestState == ServiceMailboxExecuting ? requestSequence + 1U : 1U;
    const Error active = ValidatePortableSchedulerActiveBinding(index, processor, expectedRunSequence,
                                                                Operation::FinishPortableScheduler);
    if (active != Error::None)
    {
        return active;
    }

    m_schedulerWorkerReceipts[index] = {};
    m_schedulerWorkerFailures[index] = {};
    const bool finished = m_schedulerRunLoops[index]->FinishAndDispatch(
        m_schedulerThreads[index], m_workerContexts[index], Kernel::Scheduler::ThreadRunDisposition::Blocked, 0U,
        m_schedulerWorkerReceipts[index], m_schedulerWorkerFailures[index]);
    m_portableSchedulerFailures[index].RunLoop = m_schedulerWorkerFailures[index];
    if (!finished ||
        (m_schedulerWorkerReceipts[index].State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active) ||
        (m_schedulerWorkerReceipts[index].ThreadStorage != &m_schedulerThreads[index]) ||
        (m_serviceMailboxStates[index].LoadAcquire() != ServiceMailboxExecuting))
    {
        return FailPortableScheduler(index, Error::PortableSchedulerFinishRejected, Operation::FinishPortableScheduler,
                                     static_cast<Foundation::UInt64>(finished ? 1U : 0U));
    }
    return ValidatePortableSchedulerActiveBinding(index, processor, m_serviceRequestSequences[index].LoadAcquire() + 1U,
                                                  Operation::FinishPortableScheduler);
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void
Runtime::RunPortableSchedulerIdleLoop(const Kernel::Scheduler::CpuId processor,
                                      const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    if (!TryGetProcessorIndex(processor, index))
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::IdleProcessor);
    }
    Platform::Synchronization::BindProcessorDrain(m_tlbShootdownDrains[index], nullptr);
    for (;;)
    {
        Platform::Synchronization::ProcessorWaitProgress();
        if ((m_processorStates[index].LoadAcquire() == static_cast<Foundation::UInt64>(Lifecycle::IdleServing)) &&
            (ServiceTlbShootdownFromIdle(processor, generation) != Error::None))
        {
            Park(Diagnostics::Ia64ApplicationProcessorStage::ShootdownDrain);
        }
        const Foundation::UInt64 serviceState = m_serviceMailboxStates[index].LoadAcquire();
        const Foundation::UInt64 timerState = m_timerDeliveryStates[index].LoadAcquire();
        if ((serviceState == ServiceMailboxPending) && (timerState == TimerDeliveryPending))
        {
            static_cast<void>(FailPortableScheduler(index, Error::TimerDeliveryGateConflict,
                                                    Operation::BeginTimerDelivery, serviceState));
            Park(Diagnostics::Ia64ApplicationProcessorStage::TimerServiceConflict);
        }
        if (timerState == TimerDeliveryPending)
        {
            if (BeginTimerDeliveryFromIdle(processor, generation) != Error::None)
            {
                Park(Diagnostics::Ia64ApplicationProcessorStage::TimerServiceBegin);
            }
            const PollRange pollCodeRange = Interrupt::GetIa64ExternalInterruptPollCodeRange();
            const PollStatus pollStatus = AsteriaIa64PollForExternalInterruptSequence(
                AddressOf(m_localTimerEvidence[index]), 2U, m_timerDeliveryPollDeadlines[index]);
            if (CompleteTimerDeliveryFromIdle(processor, generation, pollStatus, pollCodeRange) != Error::None)
            {
                Park(Diagnostics::Ia64ApplicationProcessorStage::TimerServiceComplete);
            }
        }
        else if (ServicePortableSchedulerFromIdle(processor, generation) != Error::None)
        {
            Park(Diagnostics::Ia64ApplicationProcessorStage::SchedulerService);
        }
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        __asm__ volatile("hint @pause" ::: "memory");
#else
        __asm__ volatile("" ::: "memory");
#endif
    }
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void
Runtime::RunTranslatedLanding(const Kernel::Scheduler::CpuId processor) noexcept
{
    Foundation::Size index{0U};
    if ((GetState() != Ia64ApplicationProcessorKernelRuntimeState::Ready) || !TryGetProcessorIndex(processor, index))
    {
        const Failure failure =
            MakeFailure(Error::InvalidProcessor, Operation::EnterTranslatedIdle, processor.GetValue(), 0U,
                        Lifecycle::Unused, ReleaseError::None, Ia64KernelContextError::None, 0U);
        PublishFirstFailure(failure);
        Park(Diagnostics::Ia64ApplicationProcessorStage::LandingProcessor);
    }
    Diagnostics::Ia64StopProcessorOnline();
    const Foundation::UInt64 generation = m_generations[index];
    AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence(&m_runtimeEntryEvidence[index]);
    if (EnterTranslatedIdle(processor, generation, m_runtimeEntryEvidence[index]) != Error::None)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::LandingEntry);
    }
    if (BeginLocalTimerInterruptProof(processor, generation) != Error::None)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::TimerProofBegin);
    }
    const PollRange pollCodeRange = Interrupt::GetIa64ExternalInterruptPollCodeRange();
    const PollStatus pollStatus = AsteriaIa64PollForExternalInterruptSequence(AddressOf(m_localTimerEvidence[index]),
                                                                              1U, m_localTimerPollDeadlines[index]);
    if (CompleteLocalTimerInterruptProof(processor, generation, pollStatus, pollCodeRange) != Error::None)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::TimerProofComplete);
    }
    // Cancel the completed proof deadline, write ar.itc + srlz.d, then build every subsequent
    // deadline from the new ITC.
    if (!Platform::Synchronization::SynchronizeOnlineProcessorClock(m_localTimers[index], 0U,
                                                                    Diagnostics::GetIa64EvidenceRecord().Clocks))
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::TimerProofComplete);
    }
    Platform::Synchronization::ProcessorWatchdogOnline();
    if (PreparePortableScheduler(processor, generation) != Error::None)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::SchedulerPrepare);
    }
    if (DispatchPortableSchedulerFromIdle(processor, generation) != Error::None)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::SchedulerDispatch);
    }
    RunPortableSchedulerIdleLoop(processor, generation);
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void Runtime::RunWorkerAdapter(void* const argument) noexcept
{
    if (argument == nullptr)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::WorkerArgument);
    }
    auto* const worker = static_cast<WorkerArgument*>(argument);
    Runtime* const owner = worker->Owner;
    if (owner == nullptr)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::WorkerOwner);
    }
    if (!ReservedBytesAreZero(worker->Reserved, sizeof(worker->Reserved)))
    {
        Foundation::Size index{0U};
        if (owner->TryGetProcessorIndex(worker->Processor, index))
        {
            static_cast<void>(owner->FailProcessor(index, Error::InvalidWorkerArgument, Operation::WorkerAdapter,
                                                   ReleaseError::None, Ia64KernelContextError::None, 0U));
        }
        Park(Diagnostics::Ia64ApplicationProcessorStage::WorkerReserved);
    }
    owner->RunWorker(worker->Processor, worker->Generation);
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void Runtime::RunWorker(const Kernel::Scheduler::CpuId processor,
                                                            const Foundation::UInt64 generation) noexcept
{
    Foundation::Size index{0U};
    const Error validation = ValidateProcessorGeneration(processor, generation, index, Operation::WorkerAdapter);
    if (validation != Error::None)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::WorkerGeneration);
    }
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence(&m_firstWorkerEntryEvidence[index]);
#else
    const Memory::Ia64ApplicationProcessorRuntimeProcessorLayout& layout = m_plan.Processors[index];
    m_firstWorkerEntryEvidence[index] = Ia64ApplicationProcessorKernelRuntimeRegisterEvidence{
        .StackPointer =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerAliasVirtualStart(layout) + 0x100U,
        .RseBackingStorePointer =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout) + 0x100U,
        .RseBackingStoreBase =
            Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout),
        .RseNatCollection = 0U,
        .RseConfiguration = 0U,
        .ProcessorStatus = TranslationStateMask | InstructionCollectionMask,
        .CaptureComplete = 1U,
        .Reserved = {},
    };
#endif
    if (PublishWorkerEntry(processor, generation, m_firstWorkerEntryEvidence[index]) != Error::None)
    {
        Park(Diagnostics::Ia64ApplicationProcessorStage::WorkerPublication);
    }
    for (;;)
    {
        Platform::Synchronization::ProcessorWaitProgress();
        if (FinishPortableScheduler(processor, generation) != Error::None)
        {
            Park(Diagnostics::Ia64ApplicationProcessorStage::WorkerFinish);
        }
        if (RecordPortableSchedulerServiceCompletion(index, processor) != Error::None)
        {
            Park(Diagnostics::Ia64ApplicationProcessorStage::ServiceCompletion);
        }
    }
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void Runtime::Park(const Diagnostics::Ia64ApplicationProcessorStage stage) noexcept
{
    Diagnostics::StopIa64(Kernel::Diagnostics::StopCode::ApplicationProcessorStage,
                          static_cast<Foundation::UInt64>(stage));
}

} // Namespace Asteria::Platform::Ia64::Scheduler

extern "C" [[noreturn]] void AsteriaIa64ApplicationProcessorKernelRuntimeWorkerEntry(void* const argument) noexcept
{
    Asteria::Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntime::RunWorkerAdapter(argument);
}
