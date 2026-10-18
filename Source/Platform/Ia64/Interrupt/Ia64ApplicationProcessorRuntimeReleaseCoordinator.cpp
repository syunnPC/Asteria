// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorRuntimeReleaseCoordinator.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
using Coordinator = Ia64ApplicationProcessorRuntimeReleaseCoordinator;
using CoordinatorState = Ia64ApplicationProcessorRuntimeReleaseCoordinatorState;
using Evidence = Ia64ApplicationProcessorRuntimeServingEvidence;
using Fault = Ia64ApplicationProcessorRuntimeFault;
using FaultOperation = Ia64ApplicationProcessorRuntimeFaultOperation;
using InitializationError = Ia64ApplicationProcessorRuntimeInitializationError;
using Release = Ia64ApplicationProcessorRuntimeRelease;
using ReleaseResult = Foundation::Result<Release, Ia64ApplicationProcessorRuntimeTransitionError>;
using ServingObservation = Ia64ApplicationProcessorRuntimeServingObservation;
using ServingResult = Foundation::Result<ServingObservation, Ia64ApplicationProcessorRuntimeTransitionError>;
using Stage = Ia64ApplicationProcessorRuntimeStage;
using TransitionError = Ia64ApplicationProcessorRuntimeTransitionError;

constexpr Foundation::UInt64 PublishingFaultState{0x8000'0000'0000'0001ULL};
constexpr Foundation::UInt64 PublishingReleaseStage{0x8000'0000'0000'0002ULL};
constexpr Foundation::UInt64 PublishingServingEvidenceStage{0x8000'0000'0000'0003ULL};
constexpr Foundation::UInt64 PublishingServingObservationState{0x8000'0000'0000'0004ULL};
constexpr Foundation::UInt64 PublishingAllServingState{0x8000'0000'0000'0005ULL};
constexpr Foundation::UInt8 InvalidProcessorValue{0xFFU};
constexpr Foundation::UInt64 ActiveProcessorMask{0xFFU};
constexpr Foundation::UInt64 ActiveGenerationShift{8U};

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
}

[[nodiscard]] constexpr Foundation::UInt64 EncodeActiveRelease(const Foundation::UInt8 processor,
                                                               const Foundation::UInt64 generation) noexcept
{
    return (generation << ActiveGenerationShift) | processor;
}

[[nodiscard]] constexpr Foundation::UInt8 DecodeActiveProcessor(const Foundation::UInt64 activeRelease) noexcept
{
    return static_cast<Foundation::UInt8>(activeRelease & ActiveProcessorMask);
}

[[nodiscard]] constexpr Foundation::UInt64 DecodeActiveGeneration(const Foundation::UInt64 activeRelease) noexcept
{
    return activeRelease >> ActiveGenerationShift;
}

[[nodiscard]] constexpr bool ActiveReleaseEncodingIsValid(const Foundation::UInt64 activeRelease,
                                                          const Foundation::UInt8 processorCount,
                                                          const Foundation::UInt8 enabledProcessorMask) noexcept
{
    if (activeRelease == 0U)
    {
        return true;
    }
    const Foundation::UInt8 processor = DecodeActiveProcessor(activeRelease);
    const Foundation::UInt64 generation = DecodeActiveGeneration(activeRelease);
    return (processor != 0U) && (processor < Kernel::Scheduler::MaximumProcessorCount) &&
           (processor < processorCount) && ((enabledProcessorMask & ProcessorBit(processor)) != 0U) &&
           (generation == processor);
}

[[nodiscard]] constexpr Foundation::UInt8 ActiveProcessorForFault(const Foundation::UInt64 activeRelease) noexcept
{
    const Foundation::UInt8 processor = DecodeActiveProcessor(activeRelease);
    return processor < Kernel::Scheduler::MaximumProcessorCount ? processor : InvalidProcessorValue;
}

[[nodiscard]] constexpr Stage DecodeStage(const Foundation::UInt64 stage) noexcept
{
    switch (stage)
    {
    case static_cast<Foundation::UInt64>(Stage::Unused):
        return Stage::Unused;

    case static_cast<Foundation::UInt64>(Stage::AwaitingRelease):
    case PublishingReleaseStage:
        return Stage::AwaitingRelease;

    case static_cast<Foundation::UInt64>(Stage::Released):
        return Stage::Released;

    case static_cast<Foundation::UInt64>(Stage::Claimed):
        return Stage::Claimed;

    case static_cast<Foundation::UInt64>(Stage::RuntimeEntered):
        return Stage::RuntimeEntered;

    case static_cast<Foundation::UInt64>(Stage::SchedulerWaiting):
    case PublishingServingEvidenceStage:
        return Stage::SchedulerWaiting;

    case static_cast<Foundation::UInt64>(Stage::IdleServing):
        return Stage::IdleServing;
    }
    return Stage::Invalid;
}

[[nodiscard]] constexpr CoordinatorState DecodeCoordinatorState(const Foundation::UInt64 state) noexcept
{
    switch (state)
    {
    case static_cast<Foundation::UInt64>(CoordinatorState::Uninitialized):
        return CoordinatorState::Uninitialized;

    case static_cast<Foundation::UInt64>(CoordinatorState::Ready):
    case PublishingFaultState:
    case PublishingServingObservationState:
    case PublishingAllServingState:
        return CoordinatorState::Ready;

    case static_cast<Foundation::UInt64>(CoordinatorState::AllServing):
        return CoordinatorState::AllServing;

    case static_cast<Foundation::UInt64>(CoordinatorState::Faulted):
        return CoordinatorState::Faulted;
    }
    return CoordinatorState::Faulted;
}

[[nodiscard]] constexpr bool CoordinatorStateEncodingIsKnown(const Foundation::UInt64 state) noexcept
{
    switch (state)
    {
    case static_cast<Foundation::UInt64>(CoordinatorState::Uninitialized):
    case static_cast<Foundation::UInt64>(CoordinatorState::Ready):
    case static_cast<Foundation::UInt64>(CoordinatorState::AllServing):
    case static_cast<Foundation::UInt64>(CoordinatorState::Faulted):
    case PublishingFaultState:
    case PublishingServingObservationState:
    case PublishingAllServingState:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr TransitionError StateConflictError(const Foundation::UInt64 state) noexcept
{
    if ((state == static_cast<Foundation::UInt64>(CoordinatorState::AllServing)) ||
        (state == PublishingAllServingState))
    {
        return TransitionError::CoordinatorAllServing;
    }
    if ((state == static_cast<Foundation::UInt64>(CoordinatorState::Faulted)) || (state == PublishingFaultState))
    {
        return TransitionError::Faulted;
    }
    if ((state == static_cast<Foundation::UInt64>(CoordinatorState::Uninitialized)) ||
        (state == static_cast<Foundation::UInt64>(CoordinatorState::Ready)) ||
        (state == PublishingServingObservationState))
    {
        return TransitionError::CoordinatorNotReady;
    }
    return TransitionError::InvalidCoordinatorStateEncoding;
}

[[nodiscard]] constexpr bool ReservedEvidenceIsZero(const Evidence& evidence) noexcept
{
    for (const Foundation::UInt8 reserved : evidence.Reserved)
    {
        if (reserved != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool
TakeoverReservedIsZero(const Ia64ApplicationProcessorTakeoverProgressSnapshot& takeover) noexcept
{
    for (const Foundation::UInt8 reserved : takeover.Reserved)
    {
        if (reserved != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr TransitionError ActiveReleaseMismatchError(const Foundation::UInt8 processor,
                                                                   const Foundation::UInt64 generation,
                                                                   const Foundation::UInt64 activeRelease) noexcept
{
    if (activeRelease == 0U)
    {
        return TransitionError::ProcessorNotReleased;
    }

    const Foundation::UInt64 activeGeneration = DecodeActiveGeneration(activeRelease);
    if (generation < activeGeneration)
    {
        return TransitionError::StaleGeneration;
    }
    if (generation > activeGeneration)
    {
        return TransitionError::ForeignGeneration;
    }
    if (processor != DecodeActiveProcessor(activeRelease))
    {
        return TransitionError::OutOfOrderProcessor;
    }
    return TransitionError::None;
}

[[nodiscard]] constexpr Release MakeRelease(const Foundation::UInt8 processor,
                                            const Foundation::UInt64 generation) noexcept
{
    return Release{
        .Generation = generation,
        .Processor = Kernel::Scheduler::CpuId{processor},
        .Reserved = {},
    };
}

[[nodiscard]] constexpr Fault MakeEncodingFault(const TransitionError error, const Foundation::UInt8 processorValue,
                                                const Stage observedStage) noexcept
{
    return Fault{
        .ExpectedGeneration = 0U,
        .ObservedGeneration = 0U,
        .Error = error,
        .Operation = FaultOperation::None,
        .ExpectedStage = Stage::Unused,
        .ObservedStage = observedStage,
        .ProcessorValue = processorValue,
        .Reserved = {},
    };
}
} // namespace

namespace Detail
{
InitializationError ValidateInitializationState(const Foundation::UInt64 state) noexcept
{
    if ((state == static_cast<Foundation::UInt64>(CoordinatorState::Faulted)) || (state == PublishingFaultState))
    {
        return InitializationError::Faulted;
    }
    if ((state == static_cast<Foundation::UInt64>(CoordinatorState::Ready)) ||
        (state == static_cast<Foundation::UInt64>(CoordinatorState::AllServing)) ||
        (state == PublishingServingObservationState) || (state == PublishingAllServingState))
    {
        return InitializationError::AlreadyInitialized;
    }
    if (state != static_cast<Foundation::UInt64>(CoordinatorState::Uninitialized))
    {
        return InitializationError::InvalidCoordinatorStateEncoding;
    }
    return InitializationError::None;
}

TransitionError ValidateTransitionState(const Foundation::UInt64 state) noexcept
{
    if (state == static_cast<Foundation::UInt64>(CoordinatorState::Uninitialized))
    {
        return TransitionError::CoordinatorNotReady;
    }
    if (state == static_cast<Foundation::UInt64>(CoordinatorState::AllServing))
    {
        return TransitionError::CoordinatorAllServing;
    }
    if ((state == static_cast<Foundation::UInt64>(CoordinatorState::Faulted)) || (state == PublishingFaultState))
    {
        return TransitionError::Faulted;
    }
    if (state == PublishingAllServingState)
    {
        return TransitionError::CoordinatorAllServing;
    }
    if (state == PublishingServingObservationState)
    {
        return TransitionError::CoordinatorNotReady;
    }
    if (state != static_cast<Foundation::UInt64>(CoordinatorState::Ready))
    {
        return TransitionError::InvalidCoordinatorStateEncoding;
    }
    return TransitionError::None;
}

Ia64ApplicationProcessorRuntimeMissingReleaseResolution
ResolveMissingActiveRelease(const Foundation::UInt8 expectedProcessor, const Foundation::UInt8 processorValue,
                            const Foundation::UInt64 generation, const Foundation::UInt64 stage,
                            const Stage duplicateStage, const TransitionError duplicateError) noexcept
{
    const Foundation::UInt64 expectedGeneration = expectedProcessor;
    const Stage observedStage = DecodeStage(stage);
    if (observedStage == Stage::Invalid)
    {
        return Ia64ApplicationProcessorRuntimeMissingReleaseResolution{
            .ExpectedGeneration = expectedGeneration,
            .Error = TransitionError::InvalidStageEncoding,
            .ObservedStage = observedStage,
            .CommitsFault = true,
        };
    }
    const bool releaseMayStillPublishActive =
        (processorValue == expectedProcessor) && (generation == expectedGeneration) &&
        ((stage == static_cast<Foundation::UInt64>(Stage::AwaitingRelease)) || (stage == PublishingReleaseStage) ||
         (stage == static_cast<Foundation::UInt64>(Stage::Released)));
    if (releaseMayStillPublishActive)
    {
        return Ia64ApplicationProcessorRuntimeMissingReleaseResolution{
            .ExpectedGeneration = expectedGeneration,
            .Error = TransitionError::ProcessorNotReleased,
            .ObservedStage = observedStage,
            .CommitsFault = false,
        };
    }

    TransitionError error{TransitionError::InvalidPrecedingStage};
    if (generation < expectedGeneration)
    {
        error = TransitionError::StaleGeneration;
    }
    else if (generation > expectedGeneration)
    {
        error = TransitionError::ForeignGeneration;
    }
    else if (processorValue != expectedProcessor)
    {
        error = TransitionError::OutOfOrderProcessor;
    }
    else if (observedStage == duplicateStage)
    {
        error = duplicateError;
    }
    return Ia64ApplicationProcessorRuntimeMissingReleaseResolution{
        .ExpectedGeneration = expectedGeneration,
        .Error = error,
        .ObservedStage = observedStage,
        .CommitsFault = true,
    };
}

bool RecordCoordinatorState(Ia64ApplicationProcessorRuntimeReleaseSnapshot& snapshot,
                            const Foundation::UInt64 state) noexcept
{
    if (!CoordinatorStateEncodingIsKnown(state))
    {
        snapshot = Ia64ApplicationProcessorRuntimeReleaseSnapshot{};
        snapshot.State = CoordinatorState::Faulted;
        snapshot.Fault =
            MakeEncodingFault(TransitionError::InvalidCoordinatorStateEncoding, InvalidProcessorValue, Stage::Invalid);
        return false;
    }
    snapshot.State = DecodeCoordinatorState(state);
    return true;
}

Stage RecordProcessorStage(Ia64ApplicationProcessorRuntimeReleaseSnapshot& snapshot, const Foundation::UInt8 processor,
                           const Foundation::UInt64 stage) noexcept
{
    const Stage decoded = DecodeStage(stage);
    snapshot.Processors[processor].Stage = decoded;
    if (decoded == Stage::Invalid)
    {
        snapshot.State = CoordinatorState::Faulted;
        if (snapshot.Fault.Error == TransitionError::None)
        {
            snapshot.Fault = MakeEncodingFault(TransitionError::InvalidStageEncoding, processor, decoded);
        }
        return decoded;
    }
    if ((decoded != Stage::Unused) && (decoded != Stage::AwaitingRelease))
    {
        snapshot.ReleasedProcessorMask =
            static_cast<Foundation::UInt8>(snapshot.ReleasedProcessorMask | ProcessorBit(processor));
    }
    if (decoded == Stage::IdleServing)
    {
        snapshot.OnlineProcessorMask =
            static_cast<Foundation::UInt8>(snapshot.OnlineProcessorMask | ProcessorBit(processor));
    }
    return decoded;
}

void RecordActiveRelease(Ia64ApplicationProcessorRuntimeReleaseSnapshot& snapshot,
                         const Foundation::UInt64 activeRelease, const Foundation::UInt8 processorCount,
                         const Foundation::UInt8 enabledProcessorMask) noexcept
{
    if (!ActiveReleaseEncodingIsValid(activeRelease, processorCount, enabledProcessorMask))
    {
        snapshot.State = CoordinatorState::Faulted;
        if (snapshot.Fault.Error == TransitionError::None)
        {
            snapshot.Fault = MakeEncodingFault(TransitionError::InvalidActiveReleaseEncoding,
                                               ActiveProcessorForFault(activeRelease), Stage::Invalid);
        }
        return;
    }
    if (activeRelease != 0U)
    {
        snapshot.ActiveGeneration = DecodeActiveGeneration(activeRelease);
        snapshot.ActiveProcessor = Kernel::Scheduler::CpuId{DecodeActiveProcessor(activeRelease)};
    }
}
} // namespace Detail

InitializationError Coordinator::Initialize(const Ia64ApplicationProcessorTakeoverProgressSnapshot& takeover,
                                            const Foundation::UInt8 enabledProcessorMask) noexcept
{
    const InitializationError stateError = Detail::ValidateInitializationState(m_state.LoadAcquire());
    if (stateError != InitializationError::None)
    {
        return stateError;
    }
    if (takeover.State != Ia64ApplicationProcessorTakeoverCoordinatorState::Ready)
    {
        return InitializationError::TakeoverNotReady;
    }
    if ((takeover.ProcessorCount == 0U) || (takeover.ProcessorCount > Kernel::Scheduler::MaximumProcessorCount))
    {
        return InitializationError::InvalidProcessorCount;
    }

    const Foundation::UInt8 exactMask = static_cast<Foundation::UInt8>(ProcessorBit(takeover.ProcessorCount) - 1U);
    if (enabledProcessorMask != exactMask)
    {
        return InitializationError::InvalidEnabledProcessorMask;
    }
    if (takeover.EnabledProcessorMask != enabledProcessorMask)
    {
        return InitializationError::EnabledProcessorMaskMismatch;
    }

    const Foundation::UInt8 exactApMask =
        static_cast<Foundation::UInt8>(exactMask & static_cast<Foundation::UInt8>(~Foundation::UInt8{1U}));
    if ((takeover.TerminalMask != exactApMask) || (takeover.FailedMask != 0U))
    {
        return InitializationError::InvalidTakeoverMasks;
    }
    if (!TakeoverReservedIsZero(takeover))
    {
        return InitializationError::InvalidTakeoverReserved;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const Stage expectedRuntimeStage =
            ((processor != 0U) && (processor < takeover.ProcessorCount)) ? Stage::AwaitingRelease : Stage::Unused;
        const Ia64ApplicationProcessorTakeoverLifecycle expectedTakeoverStage =
            expectedRuntimeStage == Stage::AwaitingRelease
                ? Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled
                : Ia64ApplicationProcessorTakeoverLifecycle::Unused;
        if (takeover.Processors[processor] != expectedTakeoverStage)
        {
            return InitializationError::InvalidTakeoverLifecycle;
        }
    }

    m_processorCount = takeover.ProcessorCount;
    m_enabledProcessorMask = enabledProcessorMask;
    for (Foundation::UInt8 processor{1U}; processor < takeover.ProcessorCount; ++processor)
    {
        m_stages[processor].StoreRelease(static_cast<Foundation::UInt64>(Stage::AwaitingRelease));
    }

    const CoordinatorState committedState =
        takeover.ProcessorCount == 1U ? CoordinatorState::AllServing : CoordinatorState::Ready;
    m_state.StoreRelease(static_cast<Foundation::UInt64>(committedState));
    return InitializationError::None;
}

TransitionError Coordinator::ValidateProcessorAndState(const Kernel::Scheduler::CpuId processor,
                                                       const Foundation::UInt64 generation,
                                                       Foundation::UInt8& processorValue) const noexcept
{
    processorValue = InvalidProcessorValue;
    const TransitionError stateError = Detail::ValidateTransitionState(m_state.LoadAcquire());
    if (stateError != TransitionError::None)
    {
        return stateError;
    }
    if (!processor.IsValid())
    {
        return TransitionError::InvalidProcessor;
    }

    processorValue = processor.GetValue();
    if (processorValue == 0U)
    {
        return TransitionError::BootstrapProcessor;
    }
    if ((processorValue >= m_processorCount) || ((m_enabledProcessorMask & ProcessorBit(processorValue)) == 0U))
    {
        return TransitionError::ProcessorNotEnabled;
    }
    if (generation == 0U)
    {
        return TransitionError::InvalidGeneration;
    }
    return TransitionError::None;
}

// Release publishes Released before its active word, so the exact next token
// remains retryable across that bounded window. Any later stage with no active
// word belongs to a closed or contradictory generation and must fail-stop.
TransitionError Coordinator::ResolveMissingActiveRelease(const FaultOperation operation,
                                                         const Foundation::UInt8 processorValue,
                                                         const Foundation::UInt64 generation, const Stage expectedStage,
                                                         const Stage duplicateStage,
                                                         const TransitionError duplicateError) noexcept
{
    const Foundation::UInt64 state = m_state.LoadAcquire();
    if (state != static_cast<Foundation::UInt64>(CoordinatorState::Ready))
    {
        return StateConflictError(state);
    }

    Foundation::UInt8 expectedProcessor{1U};
    while ((expectedProcessor < m_processorCount) && (m_servingObserved[expectedProcessor].LoadAcquire() != 0U))
    {
        ++expectedProcessor;
    }
    const Detail::Ia64ApplicationProcessorRuntimeMissingReleaseResolution resolution =
        Detail::ResolveMissingActiveRelease(expectedProcessor, processorValue, generation,
                                            m_stages[processorValue].LoadAcquire(), duplicateStage, duplicateError);
    if (!resolution.CommitsFault)
    {
        return resolution.Error;
    }
    return CommitFault(resolution.Error, operation, processorValue, resolution.ExpectedGeneration, generation,
                       expectedStage, resolution.ObservedStage);
}

TransitionError Coordinator::CommitFault(const TransitionError error, const FaultOperation operation,
                                         const Foundation::UInt8 processorValue,
                                         const Foundation::UInt64 expectedGeneration,
                                         const Foundation::UInt64 observedGeneration, const Stage expectedStage,
                                         const Stage observedStage) noexcept
{
    Foundation::UInt64 expectedState = static_cast<Foundation::UInt64>(CoordinatorState::Ready);
    if (!m_state.CompareExchangeStrongAcquireRelease(expectedState, PublishingFaultState))
    {
        return StateConflictError(expectedState);
    }

    m_fault = Ia64ApplicationProcessorRuntimeFault{
        .ExpectedGeneration = expectedGeneration,
        .ObservedGeneration = observedGeneration,
        .Error = error,
        .Operation = operation,
        .ExpectedStage = expectedStage,
        .ObservedStage = observedStage,
        .ProcessorValue = processorValue,
        .Reserved = {},
    };
    m_state.StoreRelease(static_cast<Foundation::UInt64>(CoordinatorState::Faulted));
    return error;
}

ReleaseResult Coordinator::Release(const Kernel::Scheduler::CpuId processor,
                                   const Foundation::UInt64 generation) noexcept
{
    Foundation::UInt8 processorValue{InvalidProcessorValue};
    const TransitionError validation = ValidateProcessorAndState(processor, generation, processorValue);
    if (validation != TransitionError::None)
    {
        return ReleaseResult::Failure(validation);
    }

    const Foundation::UInt64 activeRelease = m_activeRelease.LoadAcquire();
    if (activeRelease != 0U)
    {
        if (!ActiveReleaseEncodingIsValid(activeRelease, m_processorCount, m_enabledProcessorMask))
        {
            const TransitionError error =
                CommitFault(TransitionError::InvalidActiveReleaseEncoding, FaultOperation::Release,
                            ActiveProcessorForFault(activeRelease), generation, DecodeActiveGeneration(activeRelease),
                            Stage::AwaitingRelease, Stage::Invalid);
            return ReleaseResult::Failure(error);
        }
        const Foundation::UInt8 activeProcessor = DecodeActiveProcessor(activeRelease);
        const Foundation::UInt64 activeGeneration = DecodeActiveGeneration(activeRelease);
        const Stage activeStage = DecodeStage(m_stages[activeProcessor].LoadAcquire());
        if (activeStage == Stage::Invalid)
        {
            const TransitionError error =
                CommitFault(TransitionError::InvalidStageEncoding, FaultOperation::Release, activeProcessor,
                            activeGeneration, generation, Stage::Released, activeStage);
            return ReleaseResult::Failure(error);
        }
        const Stage requestedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
        if (requestedStage == Stage::Invalid)
        {
            const TransitionError error =
                CommitFault(TransitionError::InvalidStageEncoding, FaultOperation::Release, processorValue,
                            activeGeneration, generation, Stage::AwaitingRelease, requestedStage);
            return ReleaseResult::Failure(error);
        }
        if ((processorValue == activeProcessor) && (generation == activeGeneration))
        {
            const TransitionError error =
                CommitFault(TransitionError::DuplicateRelease, FaultOperation::Release, processorValue,
                            activeGeneration, generation, Stage::AwaitingRelease, activeStage);
            return ReleaseResult::Failure(error);
        }
        if ((processorValue == static_cast<Foundation::UInt8>(activeProcessor + 1U)) &&
            (generation == (activeGeneration + 1U)))
        {
            return ReleaseResult::Failure(TransitionError::ServingNotObserved);
        }

        const TransitionError mismatch = ActiveReleaseMismatchError(processorValue, generation, activeRelease);
        TransitionError error = mismatch == TransitionError::None ? TransitionError::OutOfOrderProcessor : mismatch;
        const TransitionError committedError =
            CommitFault(error, FaultOperation::Release, processorValue, activeGeneration, generation,
                        Stage::AwaitingRelease, requestedStage);
        return ReleaseResult::Failure(committedError);
    }

    Foundation::UInt8 expectedProcessor{1U};
    while ((expectedProcessor < m_processorCount) && (m_servingObserved[expectedProcessor].LoadAcquire() != 0U))
    {
        ++expectedProcessor;
    }
    if (expectedProcessor >= m_processorCount)
    {
        return ReleaseResult::Failure(TransitionError::CoordinatorAllServing);
    }

    const Foundation::UInt64 expectedGeneration = expectedProcessor;
    const Stage requestedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
    if (requestedStage == Stage::Invalid)
    {
        const TransitionError error =
            CommitFault(TransitionError::InvalidStageEncoding, FaultOperation::Release, processorValue,
                        expectedGeneration, generation, Stage::AwaitingRelease, requestedStage);
        return ReleaseResult::Failure(error);
    }
    if (generation != expectedGeneration)
    {
        const TransitionError error =
            generation < expectedGeneration ? TransitionError::StaleGeneration : TransitionError::ForeignGeneration;
        const TransitionError committedError =
            CommitFault(error, FaultOperation::Release, processorValue, expectedGeneration, generation,
                        Stage::AwaitingRelease, requestedStage);
        return ReleaseResult::Failure(committedError);
    }
    if (processorValue != expectedProcessor)
    {
        const TransitionError error =
            CommitFault(TransitionError::OutOfOrderProcessor, FaultOperation::Release, processorValue,
                        expectedGeneration, generation, Stage::AwaitingRelease, requestedStage);
        return ReleaseResult::Failure(error);
    }

    Foundation::UInt64 expectedStage = static_cast<Foundation::UInt64>(Stage::AwaitingRelease);
    if (!m_stages[processorValue].CompareExchangeStrongAcquireRelease(expectedStage, PublishingReleaseStage))
    {
        const Stage observedStage = DecodeStage(expectedStage);
        const TransitionError error =
            CommitFault(observedStage == Stage::Invalid ? TransitionError::InvalidStageEncoding
                                                        : TransitionError::InvalidPrecedingStage,
                        FaultOperation::Release, processorValue, expectedGeneration, generation, Stage::AwaitingRelease,
                        observedStage);
        return ReleaseResult::Failure(error);
    }

    m_generations[processorValue] = generation;
    m_stages[processorValue].StoreRelease(static_cast<Foundation::UInt64>(Stage::Released));
    m_activeRelease.StoreRelease(EncodeActiveRelease(processorValue, generation));
    return ReleaseResult::Success(MakeRelease(processorValue, generation));
}

ReleaseResult Coordinator::Claim(const Kernel::Scheduler::CpuId processor, const Foundation::UInt64 generation) noexcept
{
    Foundation::UInt8 processorValue{InvalidProcessorValue};
    const TransitionError validation = ValidateProcessorAndState(processor, generation, processorValue);
    if (validation != TransitionError::None)
    {
        return ReleaseResult::Failure(validation);
    }

    const Foundation::UInt64 activeRelease = m_activeRelease.LoadAcquire();
    if (!ActiveReleaseEncodingIsValid(activeRelease, m_processorCount, m_enabledProcessorMask))
    {
        const TransitionError error =
            CommitFault(TransitionError::InvalidActiveReleaseEncoding, FaultOperation::Claim,
                        ActiveProcessorForFault(activeRelease), generation, DecodeActiveGeneration(activeRelease),
                        Stage::Released, Stage::Invalid);
        return ReleaseResult::Failure(error);
    }
    const TransitionError mismatch = ActiveReleaseMismatchError(processorValue, generation, activeRelease);
    if (mismatch != TransitionError::None)
    {
        if (mismatch != TransitionError::ProcessorNotReleased)
        {
            const Stage observedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
            const TransitionError error =
                CommitFault(observedStage == Stage::Invalid ? TransitionError::InvalidStageEncoding : mismatch,
                            FaultOperation::Claim, processorValue, DecodeActiveGeneration(activeRelease), generation,
                            Stage::Released, observedStage);
            return ReleaseResult::Failure(error);
        }
        return ReleaseResult::Failure(ResolveMissingActiveRelease(FaultOperation::Claim, processorValue, generation,
                                                                  Stage::Released, Stage::Claimed,
                                                                  TransitionError::DuplicateClaim));
    }

    Foundation::UInt64 expectedStage = static_cast<Foundation::UInt64>(Stage::Released);
    if (!m_stages[processorValue].CompareExchangeStrongAcquireRelease(expectedStage,
                                                                      static_cast<Foundation::UInt64>(Stage::Claimed)))
    {
        const Stage observedStage = DecodeStage(expectedStage);
        TransitionError error =
            observedStage == Stage::Claimed ? TransitionError::DuplicateClaim : TransitionError::InvalidPrecedingStage;
        if (observedStage == Stage::Invalid)
        {
            error = TransitionError::InvalidStageEncoding;
        }
        const TransitionError committedError = CommitFault(error, FaultOperation::Claim, processorValue, generation,
                                                           generation, Stage::Released, observedStage);
        return ReleaseResult::Failure(committedError);
    }
    return ReleaseResult::Success(MakeRelease(processorValue, generation));
}

ReleaseResult Coordinator::TryObserveRelease(const Kernel::Scheduler::CpuId processor) const noexcept
{
    Foundation::UInt8 processorValue{InvalidProcessorValue};
    const TransitionError validation = ValidateProcessorAndState(processor, 1U, processorValue);
    if (validation != TransitionError::None)
    {
        return ReleaseResult::Failure(validation);
    }

    const Foundation::UInt64 activeRelease = m_activeRelease.LoadAcquire();
    if (!ActiveReleaseEncodingIsValid(activeRelease, m_processorCount, m_enabledProcessorMask))
    {
        return ReleaseResult::Failure(TransitionError::InvalidActiveReleaseEncoding);
    }
    const Stage stage = DecodeStage(m_stages[processorValue].LoadAcquire());
    if (stage == Stage::Invalid)
    {
        return ReleaseResult::Failure(TransitionError::InvalidStageEncoding);
    }
    if ((activeRelease == 0U) || (DecodeActiveProcessor(activeRelease) != processorValue))
    {
        return ReleaseResult::Failure(TransitionError::ProcessorNotReleased);
    }

    const Foundation::UInt64 generation = DecodeActiveGeneration(activeRelease);
    if ((generation == 0U) || (m_generations[processorValue] != generation) || (stage != Stage::Released))
    {
        return ReleaseResult::Failure(TransitionError::ProcessorNotReleased);
    }
    return ReleaseResult::Success(MakeRelease(processorValue, generation));
}

TransitionError Coordinator::PublishRuntimeEntered(const Kernel::Scheduler::CpuId processor,
                                                   const Foundation::UInt64 generation) noexcept
{
    Foundation::UInt8 processorValue{InvalidProcessorValue};
    const TransitionError validation = ValidateProcessorAndState(processor, generation, processorValue);
    if (validation != TransitionError::None)
    {
        return validation;
    }

    const Foundation::UInt64 activeRelease = m_activeRelease.LoadAcquire();
    if (!ActiveReleaseEncodingIsValid(activeRelease, m_processorCount, m_enabledProcessorMask))
    {
        return CommitFault(TransitionError::InvalidActiveReleaseEncoding, FaultOperation::PublishRuntimeEntered,
                           ActiveProcessorForFault(activeRelease), generation, DecodeActiveGeneration(activeRelease),
                           Stage::Claimed, Stage::Invalid);
    }
    const TransitionError mismatch = ActiveReleaseMismatchError(processorValue, generation, activeRelease);
    if (mismatch != TransitionError::None)
    {
        if (mismatch != TransitionError::ProcessorNotReleased)
        {
            const Stage observedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
            return CommitFault(observedStage == Stage::Invalid ? TransitionError::InvalidStageEncoding : mismatch,
                               FaultOperation::PublishRuntimeEntered, processorValue,
                               DecodeActiveGeneration(activeRelease), generation, Stage::Claimed, observedStage);
        }
        return ResolveMissingActiveRelease(FaultOperation::PublishRuntimeEntered, processorValue, generation,
                                           Stage::Claimed, Stage::RuntimeEntered, TransitionError::DuplicateStage);
    }

    Foundation::UInt64 expectedStage = static_cast<Foundation::UInt64>(Stage::Claimed);
    if (!m_stages[processorValue].CompareExchangeStrongAcquireRelease(
            expectedStage, static_cast<Foundation::UInt64>(Stage::RuntimeEntered)))
    {
        const Stage observedStage = DecodeStage(expectedStage);
        TransitionError error = observedStage == Stage::RuntimeEntered ? TransitionError::DuplicateStage
                                                                       : TransitionError::InvalidPrecedingStage;
        if (observedStage == Stage::Invalid)
        {
            error = TransitionError::InvalidStageEncoding;
        }
        return CommitFault(error, FaultOperation::PublishRuntimeEntered, processorValue, generation, generation,
                           Stage::Claimed, observedStage);
    }
    return TransitionError::None;
}

TransitionError Coordinator::PublishSchedulerWaiting(const Kernel::Scheduler::CpuId processor,
                                                     const Foundation::UInt64 generation) noexcept
{
    Foundation::UInt8 processorValue{InvalidProcessorValue};
    const TransitionError validation = ValidateProcessorAndState(processor, generation, processorValue);
    if (validation != TransitionError::None)
    {
        return validation;
    }

    const Foundation::UInt64 activeRelease = m_activeRelease.LoadAcquire();
    if (!ActiveReleaseEncodingIsValid(activeRelease, m_processorCount, m_enabledProcessorMask))
    {
        return CommitFault(TransitionError::InvalidActiveReleaseEncoding, FaultOperation::PublishSchedulerWaiting,
                           ActiveProcessorForFault(activeRelease), generation, DecodeActiveGeneration(activeRelease),
                           Stage::RuntimeEntered, Stage::Invalid);
    }
    const TransitionError mismatch = ActiveReleaseMismatchError(processorValue, generation, activeRelease);
    if (mismatch != TransitionError::None)
    {
        if (mismatch != TransitionError::ProcessorNotReleased)
        {
            const Stage observedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
            return CommitFault(observedStage == Stage::Invalid ? TransitionError::InvalidStageEncoding : mismatch,
                               FaultOperation::PublishSchedulerWaiting, processorValue,
                               DecodeActiveGeneration(activeRelease), generation, Stage::RuntimeEntered, observedStage);
        }
        return ResolveMissingActiveRelease(FaultOperation::PublishSchedulerWaiting, processorValue, generation,
                                           Stage::RuntimeEntered, Stage::SchedulerWaiting,
                                           TransitionError::DuplicateStage);
    }

    Foundation::UInt64 expectedStage = static_cast<Foundation::UInt64>(Stage::RuntimeEntered);
    if (!m_stages[processorValue].CompareExchangeStrongAcquireRelease(
            expectedStage, static_cast<Foundation::UInt64>(Stage::SchedulerWaiting)))
    {
        const Stage observedStage = DecodeStage(expectedStage);
        TransitionError error = observedStage == Stage::SchedulerWaiting ? TransitionError::DuplicateStage
                                                                         : TransitionError::InvalidPrecedingStage;
        if (observedStage == Stage::Invalid)
        {
            error = TransitionError::InvalidStageEncoding;
        }
        return CommitFault(error, FaultOperation::PublishSchedulerWaiting, processorValue, generation, generation,
                           Stage::RuntimeEntered, observedStage);
    }
    return TransitionError::None;
}

TransitionError Coordinator::PublishIdleServing(const Kernel::Scheduler::CpuId processor,
                                                const Foundation::UInt64 generation, const Evidence& evidence) noexcept
{
    Foundation::UInt8 processorValue{InvalidProcessorValue};
    const TransitionError validation = ValidateProcessorAndState(processor, generation, processorValue);
    if (validation != TransitionError::None)
    {
        return validation;
    }
    if (!ReservedEvidenceIsZero(evidence))
    {
        return TransitionError::InvalidEvidence;
    }

    const Foundation::UInt64 activeRelease = m_activeRelease.LoadAcquire();
    if (!ActiveReleaseEncodingIsValid(activeRelease, m_processorCount, m_enabledProcessorMask))
    {
        return CommitFault(TransitionError::InvalidActiveReleaseEncoding, FaultOperation::PublishIdleServing,
                           ActiveProcessorForFault(activeRelease), generation, DecodeActiveGeneration(activeRelease),
                           Stage::SchedulerWaiting, Stage::Invalid);
    }
    const TransitionError mismatch = ActiveReleaseMismatchError(processorValue, generation, activeRelease);
    if (mismatch != TransitionError::None)
    {
        if (mismatch != TransitionError::ProcessorNotReleased)
        {
            const Stage observedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
            return CommitFault(observedStage == Stage::Invalid ? TransitionError::InvalidStageEncoding : mismatch,
                               FaultOperation::PublishIdleServing, processorValue,
                               DecodeActiveGeneration(activeRelease), generation, Stage::SchedulerWaiting,
                               observedStage);
        }
        return ResolveMissingActiveRelease(FaultOperation::PublishIdleServing, processorValue, generation,
                                           Stage::SchedulerWaiting, Stage::IdleServing,
                                           TransitionError::DuplicateStage);
    }

    Foundation::UInt64 expectedStage = static_cast<Foundation::UInt64>(Stage::SchedulerWaiting);
    if (!m_stages[processorValue].CompareExchangeStrongAcquireRelease(expectedStage, PublishingServingEvidenceStage))
    {
        const Stage observedStage = DecodeStage(expectedStage);
        TransitionError error = observedStage == Stage::IdleServing ? TransitionError::DuplicateStage
                                                                    : TransitionError::InvalidPrecedingStage;
        if (observedStage == Stage::Invalid)
        {
            error = TransitionError::InvalidStageEncoding;
        }
        return CommitFault(error, FaultOperation::PublishIdleServing, processorValue, generation, generation,
                           Stage::SchedulerWaiting, observedStage);
    }

    m_evidence[processorValue] = evidence;
    m_stages[processorValue].StoreRelease(static_cast<Foundation::UInt64>(Stage::IdleServing));
    return TransitionError::None;
}

ServingResult Coordinator::ObserveServing(const Kernel::Scheduler::CpuId processor,
                                          const Foundation::UInt64 generation) noexcept
{
    Foundation::UInt8 processorValue{InvalidProcessorValue};
    const TransitionError validation = ValidateProcessorAndState(processor, generation, processorValue);
    if (validation != TransitionError::None)
    {
        return ServingResult::Failure(validation);
    }

    const Foundation::UInt64 activeRelease = m_activeRelease.LoadAcquire();
    if (!ActiveReleaseEncodingIsValid(activeRelease, m_processorCount, m_enabledProcessorMask))
    {
        const TransitionError error =
            CommitFault(TransitionError::InvalidActiveReleaseEncoding, FaultOperation::ObserveServing,
                        ActiveProcessorForFault(activeRelease), generation, DecodeActiveGeneration(activeRelease),
                        Stage::IdleServing, Stage::Invalid);
        return ServingResult::Failure(error);
    }
    const TransitionError mismatch = ActiveReleaseMismatchError(processorValue, generation, activeRelease);
    if (mismatch != TransitionError::None)
    {
        if (mismatch != TransitionError::ProcessorNotReleased)
        {
            const Stage observedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
            const TransitionError error =
                CommitFault(observedStage == Stage::Invalid ? TransitionError::InvalidStageEncoding : mismatch,
                            FaultOperation::ObserveServing, processorValue, DecodeActiveGeneration(activeRelease),
                            generation, Stage::IdleServing, observedStage);
            return ServingResult::Failure(error);
        }
        return ServingResult::Failure(ResolveMissingActiveRelease(FaultOperation::ObserveServing, processorValue,
                                                                  generation, Stage::IdleServing, Stage::IdleServing,
                                                                  TransitionError::DuplicateStage));
    }

    const Stage observedStage = DecodeStage(m_stages[processorValue].LoadAcquire());
    if (observedStage == Stage::Invalid)
    {
        const TransitionError error =
            CommitFault(TransitionError::InvalidStageEncoding, FaultOperation::ObserveServing, processorValue,
                        generation, generation, Stage::IdleServing, observedStage);
        return ServingResult::Failure(error);
    }
    if (observedStage != Stage::IdleServing)
    {
        return ServingResult::Failure(TransitionError::ServingPending);
    }

    const bool isFinalProcessor = processorValue == static_cast<Foundation::UInt8>(m_processorCount - 1U);
    Foundation::UInt64 expectedState = static_cast<Foundation::UInt64>(CoordinatorState::Ready);
    const Foundation::UInt64 publishingState =
        isFinalProcessor ? PublishingAllServingState : PublishingServingObservationState;
    if (!m_state.CompareExchangeStrongAcquireRelease(expectedState, publishingState))
    {
        return ServingResult::Failure(StateConflictError(expectedState));
    }

    const Evidence evidence = m_evidence[processorValue];
    m_servingObserved[processorValue].StoreRelease(1U);
    m_activeRelease.StoreRelease(0U);
    const CoordinatorState publishedState = isFinalProcessor ? CoordinatorState::AllServing : CoordinatorState::Ready;
    m_state.StoreRelease(static_cast<Foundation::UInt64>(publishedState));

    return ServingResult::Success(ServingObservation{
        .Evidence = evidence,
        .Generation = generation,
        .Processor = processor,
        .Reserved = {},
    });
}

Ia64ApplicationProcessorRuntimeReleaseSnapshot Coordinator::GetSnapshot() const noexcept
{
    Ia64ApplicationProcessorRuntimeReleaseSnapshot snapshot{};
    const Foundation::UInt64 rawState = m_state.LoadAcquire();
    if (!Detail::RecordCoordinatorState(snapshot, rawState) || (snapshot.State == CoordinatorState::Uninitialized))
    {
        return snapshot;
    }

    snapshot.ProcessorCount = m_processorCount;
    snapshot.EnabledProcessorMask = m_enabledProcessorMask;
    if (rawState == static_cast<Foundation::UInt64>(CoordinatorState::Faulted))
    {
        snapshot.Fault = m_fault;
    }
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const Stage stage = Detail::RecordProcessorStage(snapshot, processor, m_stages[processor].LoadAcquire());
        Ia64ApplicationProcessorRuntimeProcessorSnapshot& processorSnapshot = snapshot.Processors[processor];
        if ((snapshot.ReleasedProcessorMask & ProcessorBit(processor)) != 0U)
        {
            processorSnapshot.Generation = m_generations[processor];
        }
        if (stage == Stage::IdleServing)
        {
            processorSnapshot.Evidence = m_evidence[processor];
        }
        processorSnapshot.ServingObserved = m_servingObserved[processor].LoadAcquire() != 0U;
        if (processorSnapshot.ServingObserved)
        {
            snapshot.ObservedProcessorMask =
                static_cast<Foundation::UInt8>(snapshot.ObservedProcessorMask | ProcessorBit(processor));
        }
    }

    Detail::RecordActiveRelease(snapshot, m_activeRelease.LoadAcquire(), m_processorCount, m_enabledProcessorMask);
    return snapshot;
}
} // namespace Asteria::Platform::Ia64::Interrupt
