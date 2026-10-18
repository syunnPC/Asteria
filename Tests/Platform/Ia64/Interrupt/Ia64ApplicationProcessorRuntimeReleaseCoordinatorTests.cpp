// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorRuntimeReleaseCoordinator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Interrupt::Detail::RecordActiveRelease;
using Platform::Ia64::Interrupt::Detail::RecordCoordinatorState;
using Platform::Ia64::Interrupt::Detail::RecordProcessorStage;
using Platform::Ia64::Interrupt::Detail::ResolveMissingActiveRelease;
using Platform::Ia64::Interrupt::Detail::ValidateInitializationState;
using Platform::Ia64::Interrupt::Detail::ValidateTransitionState;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeServingObservation;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeServingEvidence;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeFaultOperation;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeInitializationError;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinator;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinatorState;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeReleaseSnapshot;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeStage;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeTransitionError;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinatorState;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverLifecycle;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverProgressSnapshot;

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
}

[[nodiscard]] constexpr Ia64ApplicationProcessorTakeoverProgressSnapshot
MakeTakeoverSnapshot(const Foundation::UInt8 processorCount) noexcept
{
    Ia64ApplicationProcessorTakeoverProgressSnapshot snapshot{};
    snapshot.ProcessorCount = processorCount;
    if ((processorCount != 0U) && (processorCount <= Kernel::Scheduler::MaximumProcessorCount))
    {
        snapshot.EnabledProcessorMask = static_cast<Foundation::UInt8>(ProcessorBit(processorCount) - 1U);
        snapshot.TerminalMask = static_cast<Foundation::UInt8>(snapshot.EnabledProcessorMask &
                                                               static_cast<Foundation::UInt8>(~Foundation::UInt8{1U}));
        for (Foundation::UInt8 processor{1U}; processor < processorCount; ++processor)
        {
            snapshot.Processors[processor] = Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled;
        }
    }
    snapshot.State = Ia64ApplicationProcessorTakeoverCoordinatorState::Ready;
    return snapshot;
}

[[nodiscard]] constexpr Ia64ApplicationProcessorRuntimeServingEvidence
MakeEvidence(const Foundation::UInt64 tag) noexcept
{
    return Ia64ApplicationProcessorRuntimeServingEvidence{
        .RuntimeEntryValue = 0x1000U + tag,
        .FirstWorkerEntryValue = 0x2000U + tag,
        .ResumedWorkerEntryValue = 0x3000U + tag,
        .ServingIdleValue = 0x4000U + tag,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr bool EvidenceMatches(const Ia64ApplicationProcessorRuntimeServingEvidence& first,
                                             const Ia64ApplicationProcessorRuntimeServingEvidence& second) noexcept
{
    bool reservedMatches{true};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        reservedMatches = reservedMatches && (first.Reserved[index] == second.Reserved[index]);
    }
    return (first.RuntimeEntryValue == second.RuntimeEntryValue) &&
           (first.FirstWorkerEntryValue == second.FirstWorkerEntryValue) &&
           (first.ResumedWorkerEntryValue == second.ResumedWorkerEntryValue) &&
           (first.ServingIdleValue == second.ServingIdleValue) && reservedMatches;
}

[[nodiscard]] constexpr bool EvidenceIsZero(const Ia64ApplicationProcessorRuntimeServingEvidence& evidence) noexcept
{
    return EvidenceMatches(evidence, Ia64ApplicationProcessorRuntimeServingEvidence{});
}

[[nodiscard]] bool SnapshotIsFresh(const Ia64ApplicationProcessorRuntimeReleaseSnapshot& snapshot) noexcept
{
    bool processorsFresh{true};
    for (const auto& processor : snapshot.Processors)
    {
        processorsFresh = processorsFresh && EvidenceIsZero(processor.Evidence) && (processor.Generation == 0U) &&
                          (processor.Stage == Ia64ApplicationProcessorRuntimeStage::Unused) &&
                          !processor.ServingObserved;
    }
    return processorsFresh &&
           (snapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Uninitialized) &&
           (snapshot.ActiveGeneration == 0U) && !snapshot.ActiveProcessor.IsValid() &&
           (snapshot.ProcessorCount == 0U) && (snapshot.EnabledProcessorMask == 0U) &&
           (snapshot.ReleasedProcessorMask == 0U) && (snapshot.OnlineProcessorMask == 0U) &&
           (snapshot.ObservedProcessorMask == 0U) &&
           (snapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None);
}

[[nodiscard]] bool InitializeCoordinator(Ia64ApplicationProcessorRuntimeReleaseCoordinator& coordinator,
                                         const Foundation::UInt8 processorCount) noexcept
{
    const Ia64ApplicationProcessorTakeoverProgressSnapshot takeover = MakeTakeoverSnapshot(processorCount);
    return coordinator.Initialize(takeover, takeover.EnabledProcessorMask) ==
           Ia64ApplicationProcessorRuntimeInitializationError::None;
}

[[nodiscard]] bool PublishThroughServingIdle(Ia64ApplicationProcessorRuntimeReleaseCoordinator& coordinator,
                                             const Foundation::UInt8 processor, const Foundation::UInt64 generation,
                                             const Ia64ApplicationProcessorRuntimeServingEvidence& evidence) noexcept
{
    const auto claim = coordinator.Claim(CpuId{processor}, generation);
    return claim.HasValue() && (claim.GetValue().Processor.GetValue() == processor) &&
           (claim.GetValue().Generation == generation) &&
           (coordinator.PublishRuntimeEntered(CpuId{processor}, generation) ==
            Ia64ApplicationProcessorRuntimeTransitionError::None) &&
           (coordinator.PublishSchedulerWaiting(CpuId{processor}, generation) ==
            Ia64ApplicationProcessorRuntimeTransitionError::None) &&
           (coordinator.PublishIdleServing(CpuId{processor}, generation, evidence) ==
            Ia64ApplicationProcessorRuntimeTransitionError::None);
}

[[nodiscard]] bool ReleasePublishObserveServing(Ia64ApplicationProcessorRuntimeReleaseCoordinator& coordinator,
                                                const Foundation::UInt8 processor) noexcept
{
    const Foundation::UInt64 generation = processor;
    const auto release = coordinator.Release(CpuId{processor}, generation);
    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(generation);
    if (!release.HasValue() || !PublishThroughServingIdle(coordinator, processor, generation, evidence))
    {
        return false;
    }
    const auto observation = coordinator.ObserveServing(CpuId{processor}, generation);
    if (!observation.HasValue())
    {
        return false;
    }
    bool reservedZero{true};
    for (const Foundation::UInt8 reserved : observation.GetValue().Reserved)
    {
        reservedZero = reservedZero && (reserved == 0U);
    }
    return (observation.GetValue().Generation == generation) &&
           (observation.GetValue().Processor.GetValue() == processor) &&
           EvidenceMatches(observation.GetValue().Evidence, evidence) && reservedZero;
}

void TestInitializationRetryAndFailureAtomicity(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    ASTERIA_EXPECT(runner, "fresh coordinator snapshot is entirely zero and uninitialized",
                   SnapshotIsFresh(coordinator.GetSnapshot()));

    Ia64ApplicationProcessorTakeoverProgressSnapshot takeover = MakeTakeoverSnapshot(2U);
    takeover.State = Ia64ApplicationProcessorTakeoverCoordinatorState::Uninitialized;
    const bool notReady =
        coordinator.Initialize(takeover, 0x03U) == Ia64ApplicationProcessorRuntimeInitializationError::TakeoverNotReady;
    takeover = MakeTakeoverSnapshot(0U);
    const bool badCount = coordinator.Initialize(takeover, 0x01U) ==
                          Ia64ApplicationProcessorRuntimeInitializationError::InvalidProcessorCount;
    takeover = MakeTakeoverSnapshot(5U);
    const bool countAboveMaximum = coordinator.Initialize(takeover, 0x01U) ==
                                   Ia64ApplicationProcessorRuntimeInitializationError::InvalidProcessorCount;
    takeover = MakeTakeoverSnapshot(2U);
    const bool missingBsp = coordinator.Initialize(takeover, 0x02U) ==
                            Ia64ApplicationProcessorRuntimeInitializationError::InvalidEnabledProcessorMask;
    takeover.EnabledProcessorMask = 0x01U;
    const bool maskMismatch = coordinator.Initialize(takeover, 0x03U) ==
                              Ia64ApplicationProcessorRuntimeInitializationError::EnabledProcessorMaskMismatch;
    takeover = MakeTakeoverSnapshot(2U);
    takeover.TerminalMask = 0U;
    const bool takeoverMaskMismatch = coordinator.Initialize(takeover, 0x03U) ==
                                      Ia64ApplicationProcessorRuntimeInitializationError::InvalidTakeoverMasks;
    takeover = MakeTakeoverSnapshot(2U);
    takeover.FailedMask = 0x02U;
    const bool failedMask = coordinator.Initialize(takeover, 0x03U) ==
                            Ia64ApplicationProcessorRuntimeInitializationError::InvalidTakeoverMasks;
    takeover = MakeTakeoverSnapshot(2U);
    takeover.Processors[1U] = Ia64ApplicationProcessorTakeoverLifecycle::Claimed;
    const bool lifecycleMismatch = coordinator.Initialize(takeover, 0x03U) ==
                                   Ia64ApplicationProcessorRuntimeInitializationError::InvalidTakeoverLifecycle;
    takeover = MakeTakeoverSnapshot(2U);
    takeover.Reserved[6U] = 1U;
    const bool reservedMismatch = coordinator.Initialize(takeover, 0x03U) ==
                                  Ia64ApplicationProcessorRuntimeInitializationError::InvalidTakeoverReserved;

    ASTERIA_EXPECT(runner, "all initialization validation failures are atomic and retryable", notReady, badCount,
                   countAboveMaximum, missingBsp, maskMismatch, takeoverMaskMismatch, failedMask, lifecycleMismatch,
                   reservedMismatch, SnapshotIsFresh(coordinator.GetSnapshot()));

    takeover = MakeTakeoverSnapshot(2U);
    const bool retrySucceeded =
        coordinator.Initialize(takeover, 0x03U) == Ia64ApplicationProcessorRuntimeInitializationError::None;
    ASTERIA_EXPECT(runner, "a valid retry commits once and repeated initialization is rejected", retrySucceeded,
                   coordinator.Initialize(takeover, 0x03U) ==
                       Ia64ApplicationProcessorRuntimeInitializationError::AlreadyInitialized);
}

void TestSupportedMasksAndOrder(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator singleProcessor;
    const bool singleInitialized = InitializeCoordinator(singleProcessor, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot singleSnapshot = singleProcessor.GetSnapshot();
    ASTERIA_EXPECT(runner, "SMP1 is all-serving without creating an AP release generation", singleInitialized,
                   singleSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::AllServing,
                   singleSnapshot.EnabledProcessorMask == 0x01U,
                   singleProcessor.Release(CpuId{1U}, 1U).GetError() ==
                       Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorAllServing);

    for (Foundation::UInt8 processorCount{2U}; processorCount <= Kernel::Scheduler::MaximumProcessorCount;
         ++processorCount)
    {
        Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
        bool exactOrder = InitializeCoordinator(coordinator, processorCount);
        for (Foundation::UInt8 processor{1U}; processor < processorCount; ++processor)
        {
            const auto release = coordinator.Release(CpuId{processor}, processor);
            exactOrder = exactOrder && release.HasValue() && (release.GetValue().Processor.GetValue() == processor) &&
                         (release.GetValue().Generation == processor);

            const auto pending = coordinator.ObserveServing(CpuId{processor}, processor);
            exactOrder = exactOrder && !pending.HasValue() &&
                         (pending.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ServingPending);

            if (processor < static_cast<Foundation::UInt8>(processorCount - 1U))
            {
                const Foundation::UInt8 nextProcessor = static_cast<Foundation::UInt8>(processor + 1U);
                const auto activeGate = coordinator.Release(CpuId{nextProcessor}, nextProcessor);
                exactOrder =
                    exactOrder && !activeGate.HasValue() &&
                    (activeGate.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ServingNotObserved);
            }

            const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(processor);
            exactOrder = exactOrder && PublishThroughServingIdle(coordinator, processor, processor, evidence);
            const Ia64ApplicationProcessorRuntimeReleaseSnapshot serving = coordinator.GetSnapshot();
            exactOrder = exactOrder &&
                         (serving.Processors[processor].Stage == Ia64ApplicationProcessorRuntimeStage::IdleServing) &&
                         EvidenceMatches(serving.Processors[processor].Evidence, evidence);

            if (processor < static_cast<Foundation::UInt8>(processorCount - 1U))
            {
                const Foundation::UInt8 nextProcessor = static_cast<Foundation::UInt8>(processor + 1U);
                const auto gated = coordinator.Release(CpuId{nextProcessor}, nextProcessor);
                exactOrder = exactOrder && !gated.HasValue() &&
                             (gated.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ServingNotObserved);
            }

            const auto observation = coordinator.ObserveServing(CpuId{processor}, processor);
            exactOrder =
                exactOrder && observation.HasValue() && EvidenceMatches(observation.GetValue().Evidence, evidence);
        }

        const Foundation::UInt8 exactMask = static_cast<Foundation::UInt8>(ProcessorBit(processorCount) - 1U);
        const Foundation::UInt8 exactApMask =
            static_cast<Foundation::UInt8>(exactMask & static_cast<Foundation::UInt8>(~Foundation::UInt8{1U}));
        const Ia64ApplicationProcessorRuntimeReleaseSnapshot allServing = coordinator.GetSnapshot();
        exactOrder =
            exactOrder && (allServing.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::AllServing) &&
            (allServing.EnabledProcessorMask == exactMask) && (allServing.ReleasedProcessorMask == exactApMask) &&
            (allServing.OnlineProcessorMask == exactApMask) && (allServing.ObservedProcessorMask == exactApMask) &&
            (allServing.ActiveGeneration == 0U) && !allServing.ActiveProcessor.IsValid();
        ASTERIA_EXPECT(runner, "masks 0x3, 0x7, and 0xF reach serving in exact CPU/generation order", exactOrder);
    }
}

void TestExactServingLifecycle(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const bool initialized = InitializeCoordinator(coordinator, 2U);
    const auto released = coordinator.Release(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot releaseSnapshot = coordinator.GetSnapshot();
    const auto releasedPending = coordinator.ObserveServing(CpuId{1U}, 1U);

    const auto claimed = coordinator.Claim(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot claimSnapshot = coordinator.GetSnapshot();
    const auto claimedPending = coordinator.ObserveServing(CpuId{1U}, 1U);

    const Ia64ApplicationProcessorRuntimeTransitionError entered = coordinator.PublishRuntimeEntered(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot enteredSnapshot = coordinator.GetSnapshot();
    const auto enteredPending = coordinator.ObserveServing(CpuId{1U}, 1U);

    const Ia64ApplicationProcessorRuntimeTransitionError waiting = coordinator.PublishSchedulerWaiting(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot waitingSnapshot = coordinator.GetSnapshot();
    const auto waitingPending = coordinator.ObserveServing(CpuId{1U}, 1U);

    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(0x77U);
    const Ia64ApplicationProcessorRuntimeTransitionError serving =
        coordinator.PublishIdleServing(CpuId{1U}, 1U, evidence);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot servingSnapshot = coordinator.GetSnapshot();
    const auto observation = coordinator.ObserveServing(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot observedSnapshot = coordinator.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "the exact release-to-serving lifecycle publishes each boundary once", initialized, released.HasValue(),
        releaseSnapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::Released,
        releaseSnapshot.Processors[1U].Generation == 1U, releaseSnapshot.ReleasedProcessorMask == 0x02U,
        releaseSnapshot.OnlineProcessorMask == 0U, releaseSnapshot.ObservedProcessorMask == 0U,
        releaseSnapshot.ActiveGeneration == 1U, releaseSnapshot.ActiveProcessor.GetValue() == 1U,
        !releasedPending.HasValue(),
        releasedPending.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ServingPending,
        claimed.HasValue(), claimSnapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::Claimed,
        !claimedPending.HasValue(),
        claimedPending.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ServingPending,
        entered == Ia64ApplicationProcessorRuntimeTransitionError::None,
        enteredSnapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::RuntimeEntered,
        !enteredPending.HasValue(),
        enteredPending.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ServingPending,
        waiting == Ia64ApplicationProcessorRuntimeTransitionError::None,
        waitingSnapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::SchedulerWaiting,
        EvidenceIsZero(waitingSnapshot.Processors[1U].Evidence), !waitingSnapshot.Processors[1U].ServingObserved,
        !waitingPending.HasValue(),
        waitingPending.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ServingPending,
        serving == Ia64ApplicationProcessorRuntimeTransitionError::None,
        servingSnapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::IdleServing,
        EvidenceMatches(servingSnapshot.Processors[1U].Evidence, evidence),
        servingSnapshot.OnlineProcessorMask == 0x02U, servingSnapshot.ObservedProcessorMask == 0U,
        !servingSnapshot.Processors[1U].ServingObserved, observation.HasValue(),
        EvidenceMatches(observation.GetValue().Evidence, evidence),
        observedSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::AllServing,
        observedSnapshot.OnlineProcessorMask == 0x02U, observedSnapshot.ObservedProcessorMask == 0x02U,
        observedSnapshot.Processors[1U].ServingObserved, observedSnapshot.ActiveGeneration == 0U,
        !observedSnapshot.ActiveProcessor.IsValid(),
        observedSnapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None);
}

void TestClosedGenerationFailureAttribution(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const bool firstObserved = InitializeCoordinator(coordinator, 3U) && ReleasePublishObserveServing(coordinator, 1U);
    const auto nextBeforeRelease = coordinator.Claim(CpuId{2U}, 2U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot beforeFault = coordinator.GetSnapshot();
    const Ia64ApplicationProcessorRuntimeServingEvidence duplicateEvidence = MakeEvidence(0x91U);
    const Ia64ApplicationProcessorRuntimeTransitionError duplicate =
        coordinator.PublishIdleServing(CpuId{1U}, 1U, duplicateEvidence);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot faulted = coordinator.GetSnapshot();

    ASTERIA_EXPECT(
        runner,
        "a closed non-final generation faults while the exact next AP "
        "remains retryable",
        firstObserved, !nextBeforeRelease.HasValue(),
        nextBeforeRelease.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        beforeFault.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
        beforeFault.ActiveGeneration == 0U, beforeFault.ObservedProcessorMask == 0x02U,
        beforeFault.Processors[2U].Stage == Ia64ApplicationProcessorRuntimeStage::AwaitingRelease,
        duplicate == Ia64ApplicationProcessorRuntimeTransitionError::StaleGeneration,
        faulted.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
        faulted.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::StaleGeneration,
        faulted.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::PublishIdleServing,
        faulted.Fault.ProcessorValue == 1U, faulted.Fault.ExpectedGeneration == 2U,
        faulted.Fault.ObservedGeneration == 1U,
        faulted.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::SchedulerWaiting,
        faulted.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::IdleServing,
        faulted.ObservedProcessorMask == 0x02U, EvidenceMatches(faulted.Processors[1U].Evidence, MakeEvidence(1U)),
        coordinator.Release(CpuId{2U}, 2U).GetError() == Ia64ApplicationProcessorRuntimeTransitionError::Faulted);
}

void TestActiveReleaseMismatchFaultAttribution(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator releaseCoordinator;
    const bool releasePrepared =
        InitializeCoordinator(releaseCoordinator, 3U) && releaseCoordinator.Release(CpuId{1U}, 1U).HasValue();
    const auto wrongRelease = releaseCoordinator.Release(CpuId{2U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot releaseFault = releaseCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "an active release mismatch retains exact Release attribution", releasePrepared,
                   !wrongRelease.HasValue(),
                   wrongRelease.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::OutOfOrderProcessor,
                   releaseFault.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   releaseFault.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::OutOfOrderProcessor,
                   releaseFault.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::Release,
                   releaseFault.Fault.ProcessorValue == 2U, releaseFault.Fault.ExpectedGeneration == 1U,
                   releaseFault.Fault.ObservedGeneration == 1U,
                   releaseFault.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::AwaitingRelease,
                   releaseFault.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::AwaitingRelease);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator runtimeCoordinator;
    const bool runtimePrepared = InitializeCoordinator(runtimeCoordinator, 3U) &&
                                 runtimeCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                 runtimeCoordinator.Claim(CpuId{1U}, 1U).HasValue();
    const Ia64ApplicationProcessorRuntimeTransitionError wrongRuntime =
        runtimeCoordinator.PublishRuntimeEntered(CpuId{1U}, 2U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot runtimeFault = runtimeCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "an active generation mismatch retains RuntimeEntered attribution", runtimePrepared,
                   wrongRuntime == Ia64ApplicationProcessorRuntimeTransitionError::ForeignGeneration,
                   runtimeFault.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   runtimeFault.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::ForeignGeneration,
                   runtimeFault.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::PublishRuntimeEntered,
                   runtimeFault.Fault.ProcessorValue == 1U, runtimeFault.Fault.ExpectedGeneration == 1U,
                   runtimeFault.Fault.ObservedGeneration == 2U,
                   runtimeFault.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::Claimed,
                   runtimeFault.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Claimed);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator observationCoordinator;
    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(0x92U);
    const bool observationPrepared = InitializeCoordinator(observationCoordinator, 3U) &&
                                     observationCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                     PublishThroughServingIdle(observationCoordinator, 1U, 1U, evidence);
    const auto wrongObservation = observationCoordinator.ObserveServing(CpuId{2U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot observationFault = observationCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "an active serving mismatch retains exact ObserveServing attribution", observationPrepared,
                   !wrongObservation.HasValue(),
                   wrongObservation.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::OutOfOrderProcessor,
                   observationFault.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   observationFault.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::OutOfOrderProcessor,
                   observationFault.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::ObserveServing,
                   observationFault.Fault.ProcessorValue == 2U, observationFault.Fault.ExpectedGeneration == 1U,
                   observationFault.Fault.ObservedGeneration == 1U,
                   observationFault.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::IdleServing,
                   observationFault.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::AwaitingRelease);
}

// The coordinator never stores these words. They model corrupted storage,
// which the public operations cannot produce, so the fail-closed rules that
// every operation and snapshot applies are exercised directly.
void TestEncodedStorageCorruption(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 unknownEncoding{0x4000'0000'0000'00A5ULL};

    Ia64ApplicationProcessorRuntimeReleaseCoordinator stateCoordinator;
    const bool stateInitialized = InitializeCoordinator(stateCoordinator, 2U);
    Ia64ApplicationProcessorRuntimeReleaseSnapshot invalidStateSnapshot = stateCoordinator.GetSnapshot();
    const bool invalidStateUsable = RecordCoordinatorState(invalidStateSnapshot, unknownEncoding);
    const auto uninitializedAdmission = ValidateInitializationState(
        static_cast<Foundation::UInt64>(Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Uninitialized));
    const auto unknownInitialization = ValidateInitializationState(unknownEncoding);
    const auto readyTransition = ValidateTransitionState(
        static_cast<Foundation::UInt64>(Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready));
    const auto unknownTransition = ValidateTransitionState(unknownEncoding);
    ASTERIA_EXPECT(runner, "an unknown coordinator state encoding is never reported as usable", stateInitialized,
                   uninitializedAdmission == Ia64ApplicationProcessorRuntimeInitializationError::None,
                   unknownInitialization ==
                       Ia64ApplicationProcessorRuntimeInitializationError::InvalidCoordinatorStateEncoding,
                   readyTransition == Ia64ApplicationProcessorRuntimeTransitionError::None,
                   unknownTransition == Ia64ApplicationProcessorRuntimeTransitionError::InvalidCoordinatorStateEncoding,
                   !invalidStateUsable,
                   invalidStateSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   invalidStateSnapshot.Fault.Error ==
                       Ia64ApplicationProcessorRuntimeTransitionError::InvalidCoordinatorStateEncoding,
                   invalidStateSnapshot.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::None,
                   invalidStateSnapshot.Fault.ProcessorValue == 0xFFU,
                   invalidStateSnapshot.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Invalid,
                   invalidStateSnapshot.ProcessorCount == 0U, !invalidStateSnapshot.ActiveProcessor.IsValid());

    Ia64ApplicationProcessorRuntimeReleaseCoordinator stageCoordinator;
    const bool stageInitialized = InitializeCoordinator(stageCoordinator, 2U);
    Ia64ApplicationProcessorRuntimeReleaseSnapshot stageSnapshot = stageCoordinator.GetSnapshot();
    const Ia64ApplicationProcessorRuntimeStage recordedStage = RecordProcessorStage(stageSnapshot, 1U, unknownEncoding);
    const Ia64ApplicationProcessorRuntimeStage laterStage = RecordProcessorStage(stageSnapshot, 2U, unknownEncoding);
    const auto invalidStageClaim =
        ResolveMissingActiveRelease(1U, 1U, 1U, unknownEncoding, Ia64ApplicationProcessorRuntimeStage::Claimed,
                                    Ia64ApplicationProcessorRuntimeTransitionError::DuplicateClaim);
    ASTERIA_EXPECT(runner, "an unknown per-CPU stage is explicit and latches the detecting operation", stageInitialized,
                   recordedStage == Ia64ApplicationProcessorRuntimeStage::Invalid,
                   laterStage == Ia64ApplicationProcessorRuntimeStage::Invalid,
                   stageSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   stageSnapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::Invalid,
                   stageSnapshot.ReleasedProcessorMask == 0U, stageSnapshot.OnlineProcessorMask == 0U,
                   stageSnapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::InvalidStageEncoding,
                   stageSnapshot.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::None,
                   stageSnapshot.Fault.ProcessorValue == 1U, invalidStageClaim.CommitsFault,
                   invalidStageClaim.Error == Ia64ApplicationProcessorRuntimeTransitionError::InvalidStageEncoding,
                   invalidStageClaim.ExpectedGeneration == 1U,
                   invalidStageClaim.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Invalid);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator activeCoordinator;
    const bool activeInitialized = InitializeCoordinator(activeCoordinator, 2U);
    constexpr Foundation::UInt64 malformedActiveRelease{0x01FFU};
    constexpr Foundation::UInt64 exactActiveRelease{0x0101U};
    Ia64ApplicationProcessorRuntimeReleaseSnapshot exactActiveSnapshot = activeCoordinator.GetSnapshot();
    RecordActiveRelease(exactActiveSnapshot, exactActiveRelease, 2U, 0x03U);
    Ia64ApplicationProcessorRuntimeReleaseSnapshot malformedActiveSnapshot = activeCoordinator.GetSnapshot();
    RecordActiveRelease(malformedActiveSnapshot, malformedActiveRelease, 2U, 0x03U);
    ASTERIA_EXPECT(runner, "a malformed active processor encoding faults without exposing an index", activeInitialized,
                   exactActiveSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
                   exactActiveSnapshot.ActiveGeneration == 1U, exactActiveSnapshot.ActiveProcessor.GetValue() == 1U,
                   malformedActiveSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   malformedActiveSnapshot.Fault.Error ==
                       Ia64ApplicationProcessorRuntimeTransitionError::InvalidActiveReleaseEncoding,
                   malformedActiveSnapshot.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::None,
                   malformedActiveSnapshot.Fault.ProcessorValue == 0xFFU,
                   malformedActiveSnapshot.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Invalid,
                   !malformedActiveSnapshot.ActiveProcessor.IsValid(), malformedActiveSnapshot.ActiveGeneration == 0U);
}

void TestReleasedBeforeActivePublicationWindow(TestContext& runner) noexcept
{
    // Release stores Released before its active word. Only a concurrent
    // interleaving observes that window, so its resolution is exercised
    // directly and anchored to the public AwaitingRelease retry below.
    const auto releasedWindow = ResolveMissingActiveRelease(
        1U, 1U, 1U, static_cast<Foundation::UInt64>(Ia64ApplicationProcessorRuntimeStage::Released),
        Ia64ApplicationProcessorRuntimeStage::Claimed, Ia64ApplicationProcessorRuntimeTransitionError::DuplicateClaim);
    const auto closedGeneration = ResolveMissingActiveRelease(
        1U, 1U, 1U, static_cast<Foundation::UInt64>(Ia64ApplicationProcessorRuntimeStage::Claimed),
        Ia64ApplicationProcessorRuntimeStage::Claimed, Ia64ApplicationProcessorRuntimeTransitionError::DuplicateClaim);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const bool initialized = InitializeCoordinator(coordinator, 2U);
    const auto claimBeforeRelease = coordinator.Claim(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot afterRetry = coordinator.GetSnapshot();
    const auto released = coordinator.Release(CpuId{1U}, 1U);
    const auto observed = coordinator.TryObserveRelease(CpuId{1U});
    const auto claimed = coordinator.Claim(CpuId{1U}, 1U);

    ASTERIA_EXPECT(
        runner, "the exact Released-before-active publication window remains retryable", !releasedWindow.CommitsFault,
        releasedWindow.Error == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        releasedWindow.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Released, closedGeneration.CommitsFault,
        closedGeneration.Error == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateClaim, initialized,
        !claimBeforeRelease.HasValue(),
        claimBeforeRelease.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        afterRetry.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
        afterRetry.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None, released.HasValue(),
        observed.HasValue(), observed.GetValue().Generation == 1U, claimed.HasValue(),
        claimed.GetValue().Generation == 1U);
}

void TestIdentifierAndClaimRejections(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const auto beforeReadiness = coordinator.ObserveServing(CpuId{1U}, 1U);
    const bool initialized = InitializeCoordinator(coordinator, 2U);
    const auto beforeRelease = coordinator.ObserveServing(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot unreleased = coordinator.GetSnapshot();
    const auto release = coordinator.Release(CpuId{1U}, 1U);
    const auto invalid = coordinator.Claim(CpuId{4U}, 1U);
    const auto bsp = coordinator.Claim(CpuId{0U}, 1U);
    const auto absent = coordinator.Claim(CpuId{2U}, 1U);
    const auto zero = coordinator.Claim(CpuId{1U}, 0U);
    const auto claim = coordinator.Claim(CpuId{1U}, 1U);
    ASTERIA_EXPECT(
        runner, "pre-readiness, unreleased, identifier, and generation-zero requests are atomic", initialized,
        !beforeReadiness.HasValue(),
        beforeReadiness.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorNotReady,
        !beforeRelease.HasValue(),
        beforeRelease.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        unreleased.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
        unreleased.ActiveGeneration == 0U,
        unreleased.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::AwaitingRelease, release.HasValue(),
        !invalid.HasValue(), invalid.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::InvalidProcessor,
        !bsp.HasValue(), bsp.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::BootstrapProcessor,
        !absent.HasValue(), absent.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotEnabled,
        !zero.HasValue(), zero.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::InvalidGeneration,
        claim.HasValue());

    const auto duplicate = coordinator.Claim(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot faulted = coordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "duplicate claim latches exact diagnostics and blocks later mutation", !duplicate.HasValue(),
                   duplicate.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateClaim,
                   faulted.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   faulted.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateClaim,
                   faulted.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::Claim,
                   faulted.Fault.ProcessorValue == 1U,
                   faulted.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::Released,
                   faulted.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Claimed,
                   coordinator.PublishRuntimeEntered(CpuId{1U}, 1U) ==
                       Ia64ApplicationProcessorRuntimeTransitionError::Faulted);
}

void TestServingPublicationIdentityRejections(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator validationCoordinator;
    const Ia64ApplicationProcessorRuntimeTransitionError beforeReadiness =
        validationCoordinator.PublishSchedulerWaiting(CpuId{1U}, 1U);
    const bool validationPrepared = InitializeCoordinator(validationCoordinator, 3U) &&
                                    validationCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                    validationCoordinator.Claim(CpuId{1U}, 1U).HasValue();
    const Ia64ApplicationProcessorRuntimeTransitionError invalidProcessor =
        validationCoordinator.PublishRuntimeEntered(CpuId{4U}, 1U);
    const Ia64ApplicationProcessorRuntimeTransitionError bootstrapProcessor =
        validationCoordinator.PublishRuntimeEntered(CpuId{0U}, 1U);
    const Ia64ApplicationProcessorRuntimeTransitionError disabledProcessor =
        validationCoordinator.PublishRuntimeEntered(CpuId{3U}, 1U);
    const Ia64ApplicationProcessorRuntimeTransitionError zeroGeneration =
        validationCoordinator.PublishRuntimeEntered(CpuId{1U}, 0U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot validationSnapshot = validationCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "publication identifier validation rejects without partial mutation",
                   beforeReadiness == Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorNotReady,
                   validationPrepared,
                   invalidProcessor == Ia64ApplicationProcessorRuntimeTransitionError::InvalidProcessor,
                   bootstrapProcessor == Ia64ApplicationProcessorRuntimeTransitionError::BootstrapProcessor,
                   disabledProcessor == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotEnabled,
                   zeroGeneration == Ia64ApplicationProcessorRuntimeTransitionError::InvalidGeneration,
                   validationSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
                   validationSnapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::Claimed,
                   validationSnapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator processorCoordinator;
    const bool processorPrepared = InitializeCoordinator(processorCoordinator, 3U) &&
                                   processorCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                   processorCoordinator.Claim(CpuId{1U}, 1U).HasValue() &&
                                   (processorCoordinator.PublishRuntimeEntered(CpuId{1U}, 1U) ==
                                    Ia64ApplicationProcessorRuntimeTransitionError::None);
    const Ia64ApplicationProcessorRuntimeTransitionError wrongProcessor =
        processorCoordinator.PublishSchedulerWaiting(CpuId{2U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot processorFault = processorCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "SchedulerWaiting rejects and records a foreign processor", processorPrepared,
                   wrongProcessor == Ia64ApplicationProcessorRuntimeTransitionError::OutOfOrderProcessor,
                   processorFault.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   processorFault.Fault.Operation ==
                       Ia64ApplicationProcessorRuntimeFaultOperation::PublishSchedulerWaiting,
                   processorFault.Fault.ProcessorValue == 2U, processorFault.Fault.ExpectedGeneration == 1U,
                   processorFault.Fault.ObservedGeneration == 1U);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator generationCoordinator;
    const bool generationPrepared = InitializeCoordinator(generationCoordinator, 2U) &&
                                    generationCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                    generationCoordinator.Claim(CpuId{1U}, 1U).HasValue() &&
                                    (generationCoordinator.PublishRuntimeEntered(CpuId{1U}, 1U) ==
                                     Ia64ApplicationProcessorRuntimeTransitionError::None) &&
                                    (generationCoordinator.PublishSchedulerWaiting(CpuId{1U}, 1U) ==
                                     Ia64ApplicationProcessorRuntimeTransitionError::None);
    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(0xA5U);
    const Ia64ApplicationProcessorRuntimeTransitionError wrongGeneration =
        generationCoordinator.PublishIdleServing(CpuId{1U}, 2U, evidence);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot generationFault = generationCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "IdleServing rejects and records a foreign generation", generationPrepared,
                   wrongGeneration == Ia64ApplicationProcessorRuntimeTransitionError::ForeignGeneration,
                   generationFault.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   generationFault.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::PublishIdleServing,
                   generationFault.Fault.ExpectedGeneration == 1U, generationFault.Fault.ObservedGeneration == 2U,
                   generationFault.OnlineProcessorMask == 0U);
}

void TestNonMutatingReleaseObservation(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const auto beforeReadiness = coordinator.TryObserveRelease(CpuId{1U});
    const bool initialized = InitializeCoordinator(coordinator, 3U);
    const auto invalid = coordinator.TryObserveRelease(CpuId{4U});
    const auto bsp = coordinator.TryObserveRelease(CpuId{0U});
    const auto absent = coordinator.TryObserveRelease(CpuId{3U});
    const auto firstBeforeRelease = coordinator.TryObserveRelease(CpuId{1U});
    const auto secondBeforeRelease = coordinator.TryObserveRelease(CpuId{2U});
    const bool firstReleased = coordinator.Release(CpuId{1U}, 1U).HasValue();
    const auto firstObserved = coordinator.TryObserveRelease(CpuId{1U});
    const auto secondWhileFirstActive = coordinator.TryObserveRelease(CpuId{2U});
    const auto beforeClaim = coordinator.GetSnapshot();
    const bool firstClaimed = coordinator.Claim(CpuId{1U}, 1U).HasValue();
    const auto firstAfterClaim = coordinator.TryObserveRelease(CpuId{1U});
    const auto afterClaim = coordinator.GetSnapshot();

    const Ia64ApplicationProcessorRuntimeServingEvidence firstEvidence = MakeEvidence(1U);
    const bool firstServing =
        firstClaimed &&
        (coordinator.PublishRuntimeEntered(CpuId{1U}, 1U) == Ia64ApplicationProcessorRuntimeTransitionError::None) &&
        (coordinator.PublishSchedulerWaiting(CpuId{1U}, 1U) == Ia64ApplicationProcessorRuntimeTransitionError::None) &&
        (coordinator.PublishIdleServing(CpuId{1U}, 1U, firstEvidence) ==
         Ia64ApplicationProcessorRuntimeTransitionError::None) &&
        coordinator.ObserveServing(CpuId{1U}, 1U).HasValue();
    const bool secondReleased = coordinator.Release(CpuId{2U}, 2U).HasValue();
    const auto staleFirstObservation = coordinator.TryObserveRelease(CpuId{1U});
    const auto secondObserved = coordinator.TryObserveRelease(CpuId{2U});
    const auto finalSnapshot = coordinator.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "release observation lets every AP poll without claiming or faulting", !beforeReadiness.HasValue(),
        beforeReadiness.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorNotReady, initialized,
        !invalid.HasValue(), invalid.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::InvalidProcessor,
        !bsp.HasValue(), bsp.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::BootstrapProcessor,
        !absent.HasValue(), absent.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotEnabled,
        !firstBeforeRelease.HasValue(),
        firstBeforeRelease.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        !secondBeforeRelease.HasValue(),
        secondBeforeRelease.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        firstReleased, firstObserved.HasValue(), firstObserved.GetValue().Processor.GetValue() == 1U,
        firstObserved.GetValue().Generation == 1U, !secondWhileFirstActive.HasValue(),
        secondWhileFirstActive.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        beforeClaim.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
        beforeClaim.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None, !firstAfterClaim.HasValue(),
        firstAfterClaim.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        afterClaim.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
        afterClaim.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::Claimed,
        afterClaim.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None, firstServing, secondReleased,
        !staleFirstObservation.HasValue(),
        staleFirstObservation.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased,
        secondObserved.HasValue(), secondObserved.GetValue().Processor.GetValue() == 2U,
        secondObserved.GetValue().Generation == 2U,
        finalSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
        finalSnapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None);
}

void TestStaleForeignAndOutOfOrderClaims(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator foreignCoordinator;
    const bool foreignPrepared =
        InitializeCoordinator(foreignCoordinator, 2U) && foreignCoordinator.Release(CpuId{1U}, 1U).HasValue();
    const auto foreign = foreignCoordinator.Claim(CpuId{1U}, 2U);
    ASTERIA_EXPECT(runner, "a foreign future claim generation faults the coordinator", foreignPrepared,
                   !foreign.HasValue(),
                   foreign.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ForeignGeneration,
                   foreignCoordinator.GetSnapshot().Fault.ObservedGeneration == 2U);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator orderCoordinator;
    const bool orderPrepared =
        InitializeCoordinator(orderCoordinator, 3U) && orderCoordinator.Release(CpuId{1U}, 1U).HasValue();
    const auto outOfOrder = orderCoordinator.Claim(CpuId{2U}, 1U);
    ASTERIA_EXPECT(runner, "a claim from a nonreleased enabled AP faults the coordinator", orderPrepared,
                   !outOfOrder.HasValue(),
                   outOfOrder.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::OutOfOrderProcessor,
                   orderCoordinator.GetSnapshot().Fault.ProcessorValue == 2U);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator staleCoordinator;
    const bool firstServing =
        InitializeCoordinator(staleCoordinator, 3U) && ReleasePublishObserveServing(staleCoordinator, 1U);
    const bool secondReleased = staleCoordinator.Release(CpuId{2U}, 2U).HasValue();
    const auto stale = staleCoordinator.Claim(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot staleFault = staleCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "an observed serving generation cannot claim a later active release", firstServing,
                   secondReleased, !stale.HasValue(),
                   stale.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::StaleGeneration,
                   staleFault.Fault.ExpectedGeneration == 2U, staleFault.Fault.ObservedGeneration == 1U);
}

void TestReleaseRejections(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator orderCoordinator;
    const bool orderInitialized = InitializeCoordinator(orderCoordinator, 3U);
    const auto outOfOrder = orderCoordinator.Release(CpuId{2U}, 1U);
    ASTERIA_EXPECT(runner, "an out-of-order CPU release faults without publishing a slot", orderInitialized,
                   !outOfOrder.HasValue(),
                   outOfOrder.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::OutOfOrderProcessor,
                   orderCoordinator.GetSnapshot().ReleasedProcessorMask == 0U);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator duplicateCoordinator;
    const bool duplicatePrepared =
        InitializeCoordinator(duplicateCoordinator, 2U) && duplicateCoordinator.Release(CpuId{1U}, 1U).HasValue();
    const auto duplicate = duplicateCoordinator.Release(CpuId{1U}, 1U);
    ASTERIA_EXPECT(
        runner, "a duplicate active release faults the coordinator", duplicatePrepared, !duplicate.HasValue(),
        duplicate.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateRelease,
        duplicateCoordinator.GetSnapshot().Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::Release);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator staleCoordinator;
    const bool firstObserved =
        InitializeCoordinator(staleCoordinator, 3U) && ReleasePublishObserveServing(staleCoordinator, 1U);
    const auto stale = staleCoordinator.Release(CpuId{1U}, 1U);
    ASTERIA_EXPECT(runner, "a stale observed release generation faults the coordinator", firstObserved,
                   !stale.HasValue(),
                   stale.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::StaleGeneration,
                   staleCoordinator.GetSnapshot().Fault.ExpectedGeneration == 2U);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator wrapCoordinator;
    const bool wrapInitialized = InitializeCoordinator(wrapCoordinator, 2U);
    constexpr Foundation::UInt64 maximumGeneration{~Foundation::UInt64{0U}};
    const auto wrap = wrapCoordinator.Release(CpuId{1U}, maximumGeneration);
    ASTERIA_EXPECT(runner, "a wrap-edge generation is rejected before release publication", wrapInitialized,
                   !wrap.HasValue(),
                   wrap.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::ForeignGeneration,
                   wrapCoordinator.GetSnapshot().ReleasedProcessorMask == 0U);
}

void TestIllegalStagesAndFaultRetention(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const bool claimed = InitializeCoordinator(coordinator, 2U) && coordinator.Release(CpuId{1U}, 1U).HasValue() &&
                         coordinator.Claim(CpuId{1U}, 1U).HasValue();
    const Ia64ApplicationProcessorRuntimeTransitionError skipped = coordinator.PublishSchedulerWaiting(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot faulted = coordinator.GetSnapshot();
    const Ia64ApplicationProcessorRuntimeTransitionError later = coordinator.PublishRuntimeEntered(CpuId{1U}, 1U);
    ASTERIA_EXPECT(runner, "an illegal stage skip faults and retains the first diagnostic", claimed,
                   skipped == Ia64ApplicationProcessorRuntimeTransitionError::InvalidPrecedingStage,
                   faulted.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   faulted.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::Claimed,
                   faulted.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::PublishSchedulerWaiting,
                   faulted.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::RuntimeEntered,
                   faulted.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Claimed,
                   later == Ia64ApplicationProcessorRuntimeTransitionError::Faulted,
                   coordinator.GetSnapshot().Fault.Operation == faulted.Fault.Operation);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator duplicateCoordinator;
    const bool runtimeEntered = InitializeCoordinator(duplicateCoordinator, 2U) &&
                                duplicateCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                duplicateCoordinator.Claim(CpuId{1U}, 1U).HasValue() &&
                                (duplicateCoordinator.PublishRuntimeEntered(CpuId{1U}, 1U) ==
                                 Ia64ApplicationProcessorRuntimeTransitionError::None);
    const Ia64ApplicationProcessorRuntimeTransitionError duplicate =
        duplicateCoordinator.PublishRuntimeEntered(CpuId{1U}, 1U);
    ASTERIA_EXPECT(runner, "duplicate stage publication is distinguished and faulting", runtimeEntered,
                   duplicate == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateStage,
                   duplicateCoordinator.GetSnapshot().Fault.ObservedStage ==
                       Ia64ApplicationProcessorRuntimeStage::RuntimeEntered);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator servingCoordinator;
    const bool enteredForServing = InitializeCoordinator(servingCoordinator, 2U) &&
                                   servingCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                   servingCoordinator.Claim(CpuId{1U}, 1U).HasValue() &&
                                   (servingCoordinator.PublishRuntimeEntered(CpuId{1U}, 1U) ==
                                    Ia64ApplicationProcessorRuntimeTransitionError::None);
    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(0x45U);
    const Ia64ApplicationProcessorRuntimeTransitionError skippedWaiting =
        servingCoordinator.PublishIdleServing(CpuId{1U}, 1U, evidence);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot servingFault = servingCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "IdleServing cannot be published before SchedulerWaiting", enteredForServing,
                   skippedWaiting == Ia64ApplicationProcessorRuntimeTransitionError::InvalidPrecedingStage,
                   servingFault.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   servingFault.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::RuntimeEntered,
                   servingFault.OnlineProcessorMask == 0U, EvidenceIsZero(servingFault.Processors[1U].Evidence),
                   servingFault.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::PublishIdleServing,
                   servingFault.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::SchedulerWaiting,
                   servingFault.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::RuntimeEntered);
}

void TestEvidenceFailureAtomicityAndVisibility(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const bool schedulerWaiting =
        InitializeCoordinator(coordinator, 2U) && coordinator.Release(CpuId{1U}, 1U).HasValue() &&
        coordinator.Claim(CpuId{1U}, 1U).HasValue() &&
        (coordinator.PublishRuntimeEntered(CpuId{1U}, 1U) == Ia64ApplicationProcessorRuntimeTransitionError::None) &&
        (coordinator.PublishSchedulerWaiting(CpuId{1U}, 1U) == Ia64ApplicationProcessorRuntimeTransitionError::None);

    Ia64ApplicationProcessorRuntimeServingEvidence invalid = MakeEvidence(0x55U);
    invalid.Reserved[3U] = 1U;
    const Ia64ApplicationProcessorRuntimeTransitionError invalidResult =
        coordinator.PublishIdleServing(CpuId{1U}, 1U, invalid);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot unchanged = coordinator.GetSnapshot();
    const Ia64ApplicationProcessorRuntimeServingEvidence valid = MakeEvidence(0x66U);
    const Ia64ApplicationProcessorRuntimeTransitionError validResult =
        coordinator.PublishIdleServing(CpuId{1U}, 1U, valid);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot published = coordinator.GetSnapshot();
    const auto observation = coordinator.ObserveServing(CpuId{1U}, 1U);
    ASTERIA_EXPECT(runner, "reserved evidence is failure-atomic and serving evidence is exact", schedulerWaiting,
                   invalidResult == Ia64ApplicationProcessorRuntimeTransitionError::InvalidEvidence,
                   unchanged.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
                   unchanged.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::SchedulerWaiting,
                   EvidenceIsZero(unchanged.Processors[1U].Evidence),
                   validResult == Ia64ApplicationProcessorRuntimeTransitionError::None,
                   published.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::IdleServing,
                   EvidenceMatches(published.Processors[1U].Evidence, valid), observation.HasValue(),
                   EvidenceMatches(observation.GetValue().Evidence, valid));

    Ia64ApplicationProcessorRuntimeReleaseCoordinator zeroCoordinator;
    const Ia64ApplicationProcessorRuntimeServingEvidence zeroEvidence{};
    const bool zeroAccepted = InitializeCoordinator(zeroCoordinator, 2U) &&
                              zeroCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                              PublishThroughServingIdle(zeroCoordinator, 1U, 1U, zeroEvidence);
    const auto zeroObservation = zeroCoordinator.ObserveServing(CpuId{1U}, 1U);
    ASTERIA_EXPECT(runner, "the coordinator transports zero-valued runtime evidence without interpretation",
                   zeroAccepted, zeroObservation.HasValue(), EvidenceIsZero(zeroObservation.GetValue().Evidence));
}

void TestConcurrentEvidencePublication(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const bool released = InitializeCoordinator(coordinator, 2U) && coordinator.Release(CpuId{1U}, 1U).HasValue();
    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(0x1234U);
    std::atomic<bool> apSucceeded{false};
    std::thread applicationProcessor{[&coordinator, &evidence, &apSucceeded]() noexcept
                                     {
                                         apSucceeded.store(PublishThroughServingIdle(coordinator, 1U, 1U, evidence),
                                                           std::memory_order_release);
                                     }};

    bool observed{false};
    Ia64ApplicationProcessorRuntimeServingObservation observation{};
    for (Foundation::Size attempt{0U}; attempt < 100'000U; ++attempt)
    {
        const auto result = coordinator.ObserveServing(CpuId{1U}, 1U);
        if (result.HasValue())
        {
            observation = result.GetValue();
            observed = true;
            break;
        }
        if (result.GetError() != Ia64ApplicationProcessorRuntimeTransitionError::ServingPending)
        {
            break;
        }
        std::this_thread::yield();
    }
    applicationProcessor.join();
    if (!observed)
    {
        const auto result = coordinator.ObserveServing(CpuId{1U}, 1U);
        if (result.HasValue())
        {
            observation = result.GetValue();
            observed = true;
        }
    }

    ASTERIA_EXPECT(
        runner, "BSP acquire observation sees the complete AP serving payload", released,
        apSucceeded.load(std::memory_order_acquire), observed, EvidenceMatches(observation.Evidence, evidence),
        coordinator.GetSnapshot().State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::AllServing);
}

void TestConcurrentNonMutatingReleaseObservation(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
    const bool initialized = InitializeCoordinator(coordinator, 3U);
    std::atomic<bool> begin{false};
    std::atomic<bool> firstObserved{false};
    std::atomic<bool> observationValid{true};

    std::thread firstApplicationProcessor{
        [&coordinator, &begin, &firstObserved, &observationValid]() noexcept
        {
            while (!begin.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (Foundation::Size attempt{0U}; attempt < 100'000U; ++attempt)
            {
                const auto observed = coordinator.TryObserveRelease(CpuId{1U});
                if (observed.HasValue())
                {
                    const bool exact =
                        (observed.GetValue().Processor.GetValue() == 1U) && (observed.GetValue().Generation == 1U);
                    if (!exact)
                    {
                        observationValid.store(false, std::memory_order_release);
                    }
                    firstObserved.store(exact, std::memory_order_release);
                    return;
                }
                if (observed.GetError() != Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased)
                {
                    observationValid.store(false, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
            observationValid.store(false, std::memory_order_release);
        }};
    std::thread secondApplicationProcessor{
        [&coordinator, &begin, &firstObserved, &observationValid]() noexcept
        {
            while (!begin.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            while (!firstObserved.load(std::memory_order_acquire) && observationValid.load(std::memory_order_acquire))
            {
                const auto observed = coordinator.TryObserveRelease(CpuId{2U});
                if (observed.HasValue() ||
                    (observed.GetError() != Ia64ApplicationProcessorRuntimeTransitionError::ProcessorNotReleased))
                {
                    observationValid.store(false, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }};

    begin.store(true, std::memory_order_release);
    const auto release = coordinator.Release(CpuId{1U}, 1U);
    firstApplicationProcessor.join();
    secondApplicationProcessor.join();
    const auto snapshot = coordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "concurrent AP polling acquires only the selected release token", initialized,
                   release.HasValue(), firstObserved.load(std::memory_order_acquire),
                   observationValid.load(std::memory_order_acquire),
                   snapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Ready,
                   snapshot.Processors[1U].Stage == Ia64ApplicationProcessorRuntimeStage::Released,
                   snapshot.Processors[2U].Stage == Ia64ApplicationProcessorRuntimeStage::AwaitingRelease,
                   snapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None);
}

void TestConcurrentFirstFaultRetention(TestContext& runner) noexcept
{
    bool everyFaultRetained{true};
    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(0x5AU);
    for (Foundation::Size attempt{0U}; attempt < 64U; ++attempt)
    {
        Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
        const bool prepared = InitializeCoordinator(coordinator, 2U) && coordinator.Release(CpuId{1U}, 1U).HasValue() &&
                              coordinator.Claim(CpuId{1U}, 1U).HasValue();
        std::atomic<Foundation::UInt8> readyCount{0U};
        std::atomic<bool> begin{false};
        std::atomic<Ia64ApplicationProcessorRuntimeTransitionError> waitingError{
            Ia64ApplicationProcessorRuntimeTransitionError::None};
        std::atomic<Ia64ApplicationProcessorRuntimeTransitionError> servingError{
            Ia64ApplicationProcessorRuntimeTransitionError::None};

        std::thread waitingPublisher{[&coordinator, &readyCount, &begin, &waitingError]() noexcept
                                     {
                                         static_cast<void>(readyCount.fetch_add(1U, std::memory_order_acq_rel));
                                         while (!begin.load(std::memory_order_acquire))
                                         {
                                             std::this_thread::yield();
                                         }
                                         waitingError.store(coordinator.PublishSchedulerWaiting(CpuId{1U}, 1U),
                                                            std::memory_order_release);
                                     }};
        std::thread servingPublisher{[&coordinator, &evidence, &readyCount, &begin, &servingError]() noexcept
                                     {
                                         static_cast<void>(readyCount.fetch_add(1U, std::memory_order_acq_rel));
                                         while (!begin.load(std::memory_order_acquire))
                                         {
                                             std::this_thread::yield();
                                         }
                                         servingError.store(coordinator.PublishIdleServing(CpuId{1U}, 1U, evidence),
                                                            std::memory_order_release);
                                     }};

        while (readyCount.load(std::memory_order_acquire) != 2U)
        {
            std::this_thread::yield();
        }
        begin.store(true, std::memory_order_release);
        waitingPublisher.join();
        servingPublisher.join();

        const Ia64ApplicationProcessorRuntimeTransitionError waiting = waitingError.load(std::memory_order_acquire);
        const Ia64ApplicationProcessorRuntimeTransitionError serving = servingError.load(std::memory_order_acquire);
        const Ia64ApplicationProcessorRuntimeReleaseSnapshot retained = coordinator.GetSnapshot();
        const bool waitingWon =
            (waiting == Ia64ApplicationProcessorRuntimeTransitionError::InvalidPrecedingStage) &&
            (serving == Ia64ApplicationProcessorRuntimeTransitionError::Faulted) &&
            (retained.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::PublishSchedulerWaiting) &&
            (retained.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::RuntimeEntered);
        const bool servingWon =
            (serving == Ia64ApplicationProcessorRuntimeTransitionError::InvalidPrecedingStage) &&
            (waiting == Ia64ApplicationProcessorRuntimeTransitionError::Faulted) &&
            (retained.Fault.Operation == Ia64ApplicationProcessorRuntimeFaultOperation::PublishIdleServing) &&
            (retained.Fault.ExpectedStage == Ia64ApplicationProcessorRuntimeStage::SchedulerWaiting);
        const Ia64ApplicationProcessorRuntimeTransitionError later = coordinator.PublishRuntimeEntered(CpuId{1U}, 1U);
        const Ia64ApplicationProcessorRuntimeReleaseSnapshot afterLater = coordinator.GetSnapshot();
        everyFaultRetained =
            everyFaultRetained && prepared &&
            (retained.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted) &&
            (retained.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::InvalidPrecedingStage) &&
            (retained.Fault.ProcessorValue == 1U) &&
            (retained.Fault.ObservedStage == Ia64ApplicationProcessorRuntimeStage::Claimed) &&
            (retained.OnlineProcessorMask == 0U) && (waitingWon || servingWon) &&
            (later == Ia64ApplicationProcessorRuntimeTransitionError::Faulted) &&
            (afterLater.Fault.Operation == retained.Fault.Operation) &&
            (afterLater.Fault.ExpectedStage == retained.Fault.ExpectedStage);
    }
    ASTERIA_EXPECT(runner, "concurrent invalid publishers retain exactly one first fault", everyFaultRetained);
}

void TestAllServingAndFaultRace(TestContext& runner) noexcept
{
    const Ia64ApplicationProcessorRuntimeServingEvidence evidence = MakeEvidence(0xABCDU);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator faultFirst;
    const bool faultFirstPrepared = InitializeCoordinator(faultFirst, 2U) &&
                                    faultFirst.Release(CpuId{1U}, 1U).HasValue() &&
                                    PublishThroughServingIdle(faultFirst, 1U, 1U, evidence);
    const Ia64ApplicationProcessorRuntimeTransitionError duplicateFirst =
        faultFirst.PublishIdleServing(CpuId{1U}, 1U, evidence);
    const auto observationAfterFault = faultFirst.ObserveServing(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot faultFirstSnapshot = faultFirst.GetSnapshot();
    ASTERIA_EXPECT(runner, "a committed duplicate serving publication prevents observation", faultFirstPrepared,
                   duplicateFirst == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateStage,
                   !observationAfterFault.HasValue(),
                   observationAfterFault.GetError() == Ia64ApplicationProcessorRuntimeTransitionError::Faulted,
                   faultFirstSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted,
                   faultFirstSnapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateStage,
                   faultFirstSnapshot.ObservedProcessorMask == 0U);

    Ia64ApplicationProcessorRuntimeReleaseCoordinator observationFirstCoordinator;
    const bool observationFirstPrepared = InitializeCoordinator(observationFirstCoordinator, 2U) &&
                                          observationFirstCoordinator.Release(CpuId{1U}, 1U).HasValue() &&
                                          PublishThroughServingIdle(observationFirstCoordinator, 1U, 1U, evidence);
    const auto firstObservation = observationFirstCoordinator.ObserveServing(CpuId{1U}, 1U);
    const Ia64ApplicationProcessorRuntimeTransitionError duplicateAfter =
        observationFirstCoordinator.PublishIdleServing(CpuId{1U}, 1U, evidence);
    const Ia64ApplicationProcessorRuntimeReleaseSnapshot servingSnapshot = observationFirstCoordinator.GetSnapshot();
    ASTERIA_EXPECT(runner, "a reserved AllServing observation prevents a later fault", observationFirstPrepared,
                   firstObservation.HasValue(),
                   duplicateAfter == Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorAllServing,
                   servingSnapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::AllServing,
                   servingSnapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None,
                   servingSnapshot.ObservedProcessorMask == 0x02U);

    bool everyRaceLinearized{true};
    for (Foundation::Size attempt{0U}; attempt < 128U; ++attempt)
    {
        Ia64ApplicationProcessorRuntimeReleaseCoordinator coordinator;
        const bool prepared = InitializeCoordinator(coordinator, 2U) && coordinator.Release(CpuId{1U}, 1U).HasValue() &&
                              PublishThroughServingIdle(coordinator, 1U, 1U, evidence);
        std::atomic<Foundation::UInt8> readyCount{0U};
        std::atomic<bool> begin{false};
        std::atomic<Ia64ApplicationProcessorRuntimeTransitionError> duplicateError{
            Ia64ApplicationProcessorRuntimeTransitionError::None};
        std::atomic<Ia64ApplicationProcessorRuntimeTransitionError> observationError{
            Ia64ApplicationProcessorRuntimeTransitionError::None};
        std::atomic<bool> observationSucceeded{false};

        std::thread duplicatePublisher{[&coordinator, &evidence, &readyCount, &begin, &duplicateError]() noexcept
                                       {
                                           static_cast<void>(readyCount.fetch_add(1U, std::memory_order_acq_rel));
                                           while (!begin.load(std::memory_order_acquire))
                                           {
                                               std::this_thread::yield();
                                           }
                                           duplicateError.store(coordinator.PublishIdleServing(CpuId{1U}, 1U, evidence),
                                                                std::memory_order_release);
                                       }};
        std::thread servingObserver{
            [&coordinator, &readyCount, &begin, &observationError, &observationSucceeded]() noexcept
            {
                static_cast<void>(readyCount.fetch_add(1U, std::memory_order_acq_rel));
                while (!begin.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                const auto result = coordinator.ObserveServing(CpuId{1U}, 1U);
                observationSucceeded.store(result.HasValue(), std::memory_order_release);
                observationError.store(result.GetError(), std::memory_order_release);
            }};

        while (readyCount.load(std::memory_order_acquire) != 2U)
        {
            std::this_thread::yield();
        }
        begin.store(true, std::memory_order_release);
        duplicatePublisher.join();
        servingObserver.join();

        const Ia64ApplicationProcessorRuntimeTransitionError duplicate = duplicateError.load(std::memory_order_acquire);
        const Ia64ApplicationProcessorRuntimeTransitionError observation =
            observationError.load(std::memory_order_acquire);
        const bool observationHasValue = observationSucceeded.load(std::memory_order_acquire);
        const Ia64ApplicationProcessorRuntimeReleaseSnapshot snapshot = coordinator.GetSnapshot();
        const bool observationWon =
            (snapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::AllServing) &&
            observationHasValue && (observation == Ia64ApplicationProcessorRuntimeTransitionError::None) &&
            (duplicate == Ia64ApplicationProcessorRuntimeTransitionError::CoordinatorAllServing) &&
            (snapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::None) &&
            (snapshot.ObservedProcessorMask == 0x02U) && (snapshot.ActiveGeneration == 0U);
        const bool faultWon =
            (snapshot.State == Ia64ApplicationProcessorRuntimeReleaseCoordinatorState::Faulted) &&
            !observationHasValue && (observation == Ia64ApplicationProcessorRuntimeTransitionError::Faulted) &&
            (duplicate == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateStage) &&
            (snapshot.Fault.Error == Ia64ApplicationProcessorRuntimeTransitionError::DuplicateStage) &&
            (snapshot.ObservedProcessorMask == 0U) && (snapshot.ActiveGeneration == 1U);
        everyRaceLinearized = everyRaceLinearized && prepared && (observationWon || faultWon);
    }
    ASTERIA_EXPECT(runner, "duplicate serving publication and observation have one atomic winner", everyRaceLinearized);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ApplicationProcessorRuntimeReleaseCoordinatorTests"};
    Asteria::Test::TestInitializationRetryAndFailureAtomicity(runner);
    Asteria::Test::TestSupportedMasksAndOrder(runner);
    Asteria::Test::TestExactServingLifecycle(runner);
    Asteria::Test::TestClosedGenerationFailureAttribution(runner);
    Asteria::Test::TestActiveReleaseMismatchFaultAttribution(runner);
    Asteria::Test::TestEncodedStorageCorruption(runner);
    Asteria::Test::TestReleasedBeforeActivePublicationWindow(runner);
    Asteria::Test::TestIdentifierAndClaimRejections(runner);
    Asteria::Test::TestServingPublicationIdentityRejections(runner);
    Asteria::Test::TestNonMutatingReleaseObservation(runner);
    Asteria::Test::TestStaleForeignAndOutOfOrderClaims(runner);
    Asteria::Test::TestReleaseRejections(runner);
    Asteria::Test::TestIllegalStagesAndFaultRetention(runner);
    Asteria::Test::TestEvidenceFailureAtomicityAndVisibility(runner);
    Asteria::Test::TestConcurrentEvidencePublication(runner);
    Asteria::Test::TestConcurrentNonMutatingReleaseObservation(runner);
    Asteria::Test::TestConcurrentFirstFaultRetention(runner);
    Asteria::Test::TestAllServingAndFaultRace(runner);
    return runner.Finish();
}
