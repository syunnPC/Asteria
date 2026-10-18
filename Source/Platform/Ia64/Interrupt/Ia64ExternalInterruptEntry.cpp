// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64GetExternalInterruptPollCodeRangeWords(Asteria::Foundation::UInt64 outputAddress) noexcept;

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

using ModelResult = Foundation::Result<Ia64ExternalInterruptModelAction, Ia64ExternalInterruptModelError>;

[[nodiscard]] constexpr bool IsRangeInRegionZero(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                                 const Foundation::UInt64 alignment) noexcept
{
    return (start != 0U) && (sizeBytes != 0U) && (alignment != 0U) && ((alignment & (alignment - 1U)) == 0U) &&
           ((start & (alignment - 1U)) == 0U) && (start < Ia64ExternalInterruptRegionZeroLimit) &&
           (sizeBytes <= (Ia64ExternalInterruptRegionZeroLimit - start));
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstSize,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondSize) noexcept
{
    return (firstStart < (secondStart + secondSize)) && (secondStart < (firstStart + firstSize));
}

[[nodiscard]] constexpr bool ReservedBytesAreZero(const Foundation::UInt8 (&reserved)[6U]) noexcept
{
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        if (reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsInterruptedProcessorStateCompatible(const Foundation::UInt64 processorStatus) noexcept
{
    const Foundation::UInt64 instructionSlot = (processorStatus & Ia64ExternalInterruptInstructionSlotMask) >> 41U;
    return (processorStatus & Ia64ExternalInterruptInterruptedProcessorStatusMask) ==
               Ia64ExternalInterruptInterruptedProcessorStatusValue &&
           (instructionSlot < 3U);
}

[[nodiscard]] constexpr ModelResult FailModel(const Ia64ExternalInterruptModelError error) noexcept
{
    return ModelResult::Failure(error);
}
} // namespace

Foundation::Result<Ia64ExternalInterruptModelAction, Ia64ExternalInterruptModelError>
ModelIa64ExternalInterrupt(const Foundation::UInt64 contextAddress, const Ia64ExternalInterruptEntryContext& context,
                           const Foundation::UInt64 evidenceAddress, const Ia64LocalTimerInterruptEvidence& evidence,
                           const Ia64ExternalInterruptModelInput input) noexcept
{
    if ((input.CurrentProcessorStatus & Ia64ExternalInterruptCurrentProcessorStatusMask) !=
        Ia64ExternalInterruptCurrentProcessorStatusValue)
    {
        return FailModel(Ia64ExternalInterruptModelError::IncompatibleCurrentState);
    }
    if (!IsInterruptedProcessorStateCompatible(input.InterruptedProcessorStatus))
    {
        return FailModel(Ia64ExternalInterruptModelError::IncompatibleInterruptedState);
    }
    if ((input.InterruptedInstructionPointer & 15U) != 0U)
    {
        return FailModel(Ia64ExternalInterruptModelError::MisalignedInterruptedInstructionPointer);
    }
    if ((input.InterruptedFunctionState & Ia64ExternalInterruptIfsValidMask) != 0U)
    {
        return FailModel(Ia64ExternalInterruptModelError::IfsValid);
    }
    if ((input.InterruptionStatus & Ia64ExternalInterruptNestedMask) != 0U)
    {
        return FailModel(Ia64ExternalInterruptModelError::NestedInterruption);
    }
    if (!IsRangeInRegionZero(contextAddress, sizeof(Ia64ExternalInterruptEntryContext),
                             alignof(Ia64ExternalInterruptEntryContext)))
    {
        return FailModel(Ia64ExternalInterruptModelError::InvalidContextAddress);
    }
    if ((context.ProcessorIndex >= Ia64ExternalInterruptMaximumProcessorCount) ||
        (context.TimerVector != Ia64ExternalInterruptTimerVector))
    {
        return FailModel(Ia64ExternalInterruptModelError::InvalidContext);
    }
    if ((context.EvidenceAddress != evidenceAddress) ||
        !IsRangeInRegionZero(evidenceAddress, sizeof(Ia64LocalTimerInterruptEvidence),
                             alignof(Ia64LocalTimerInterruptEvidence)) ||
        RangesOverlap(contextAddress, sizeof(Ia64ExternalInterruptEntryContext), evidenceAddress,
                      sizeof(Ia64LocalTimerInterruptEvidence)))
    {
        return FailModel(Ia64ExternalInterruptModelError::InvalidEvidenceAddress);
    }
    if (evidence.Sequence != evidence.AcknowledgedSequence)
    {
        return FailModel(Ia64ExternalInterruptModelError::EvidenceOutstanding);
    }
    if (evidence.Sequence == MaximumUInt64)
    {
        return FailModel(Ia64ExternalInterruptModelError::EvidenceSequenceExhausted);
    }

    if (input.ClaimedVector == Ia64ExternalInterruptSpuriousVector)
    {
        return ModelResult::Success(Ia64ExternalInterruptModelAction{
            .Evidence = evidence,
            .TimerVectorRegister = 0U,
            .Disposition = Ia64ExternalInterruptDisposition::ResumeSpurious,
            .EoiCount = 0U,
            .Reserved = {},
        });
    }
    if (input.ClaimedVector != Ia64ExternalInterruptTimerVector)
    {
        return FailModel(Ia64ExternalInterruptModelError::UnexpectedClaimedVector);
    }
    if ((input.IntervalTimerMatch == 0U) || (input.IntervalTimerMatch == MaximumUInt64) ||
        (input.IntervalTimeCounter < input.IntervalTimerMatch))
    {
        return FailModel(Ia64ExternalInterruptModelError::InvalidTimerState);
    }

    return ModelResult::Success(Ia64ExternalInterruptModelAction{
        .Evidence =
            Ia64LocalTimerInterruptEvidence{
                .Sequence = evidence.Sequence + 1U,
                .ProgrammedDeadlineTicks = input.IntervalTimerMatch,
                .ObservedIntervalTimeCounter = input.IntervalTimeCounter,
                .InterruptedInstructionPointer = input.InterruptedInstructionPointer,
                .InterruptedProcessorStatus = input.InterruptedProcessorStatus,
                .InterruptedIfsValid = 0U,
                .ProcessorIndex = static_cast<Foundation::UInt8>(context.ProcessorIndex),
                .Vector = static_cast<Foundation::UInt8>(input.ClaimedVector),
                .Reserved = {},
                .AcknowledgedSequence = evidence.AcknowledgedSequence,
            },
        .TimerVectorRegister = Ia64ExternalInterruptMaskedTimerVector,
        .Disposition = Ia64ExternalInterruptDisposition::ResumeTimer,
        .EoiCount = 1U,
        .Reserved = {},
    });
}

Ia64LocalTimerInterruptEvidenceValidationError ValidateIa64LocalTimerInterruptEvidence(
    const Ia64LocalTimerInterruptEvidence& evidence, const Foundation::UInt8 expectedProcessorIndex,
    const Foundation::UInt64 expectedDeadlineTicks, const Foundation::UInt64 pollingStart,
    const Foundation::UInt64 pollingEnd) noexcept
{
    return ValidateIa64LocalTimerInterruptEvidenceForSequence(evidence, 1U, expectedProcessorIndex,
                                                              expectedDeadlineTicks, pollingStart, pollingEnd);
}

Ia64LocalTimerInterruptEvidenceValidationError ValidateIa64LocalTimerInterruptEvidenceForSequence(
    const Ia64LocalTimerInterruptEvidence& evidence, const Foundation::UInt64 expectedSequence,
    const Foundation::UInt8 expectedProcessorIndex, const Foundation::UInt64 expectedDeadlineTicks,
    const Foundation::UInt64 pollingStart, const Foundation::UInt64 pollingEnd) noexcept
{
    if (expectedProcessorIndex >= Ia64ExternalInterruptMaximumProcessorCount)
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::InvalidExpectedProcessor;
    }
    if ((expectedDeadlineTicks == 0U) || (expectedDeadlineTicks == MaximumUInt64))
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::InvalidExpectedDeadline;
    }
    if ((pollingStart & 15U) != 0U || (pollingEnd & 15U) != 0U || (pollingStart >= pollingEnd) ||
        (pollingEnd > Ia64ExternalInterruptRegionZeroLimit))
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::InvalidPollingRange;
    }
    if ((expectedSequence == 0U) || (evidence.Sequence != expectedSequence) ||
        (evidence.AcknowledgedSequence != (expectedSequence - 1U)))
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::Incomplete;
    }
    if (evidence.ProgrammedDeadlineTicks != expectedDeadlineTicks)
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::DeadlineMismatch;
    }
    if (evidence.ObservedIntervalTimeCounter < evidence.ProgrammedDeadlineTicks)
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::CounterBeforeDeadline;
    }
    if ((evidence.InterruptedInstructionPointer & 15U) != 0U ||
        (evidence.InterruptedInstructionPointer < pollingStart) ||
        (evidence.InterruptedInstructionPointer >= pollingEnd))
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::InstructionPointerOutsidePollingRange;
    }
    if (!IsInterruptedProcessorStateCompatible(evidence.InterruptedProcessorStatus))
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::InvalidInterruptedProcessorStatus;
    }
    if (evidence.InterruptedIfsValid != 0U)
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::IfsValid;
    }
    if (evidence.ProcessorIndex != expectedProcessorIndex)
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::ProcessorMismatch;
    }
    if (evidence.Vector != Ia64ExternalInterruptTimerVector)
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::VectorMismatch;
    }
    if (!ReservedBytesAreZero(evidence.Reserved))
    {
        return Ia64LocalTimerInterruptEvidenceValidationError::ReservedNotZero;
    }
    return Ia64LocalTimerInterruptEvidenceValidationError::None;
}

Ia64LocalTimerInterruptEvidence
AcquireIa64LocalTimerInterruptEvidence(const Ia64LocalTimerInterruptEvidence& evidence) noexcept
{
    return AcquireIa64LocalTimerInterruptEvidenceForSequence(evidence, 1U);
}

Foundation::UInt64
AcquireIa64LocalTimerInterruptEvidenceSequence(const Ia64LocalTimerInterruptEvidence& evidence) noexcept
{
    return __atomic_load_n(&evidence.Sequence, __ATOMIC_ACQUIRE);
}

Ia64LocalTimerInterruptEvidence
AcquireIa64LocalTimerInterruptEvidenceForSequence(const Ia64LocalTimerInterruptEvidence& evidence,
                                                  const Foundation::UInt64 expectedSequence) noexcept
{
    const Foundation::UInt64 sequence = __atomic_load_n(&evidence.Sequence, __ATOMIC_ACQUIRE);
    const Foundation::UInt64 acknowledgedSequence = __atomic_load_n(&evidence.AcknowledgedSequence, __ATOMIC_ACQUIRE);
    if ((expectedSequence == 0U) || (sequence != expectedSequence) || (acknowledgedSequence != (expectedSequence - 1U)))
    {
        Ia64LocalTimerInterruptEvidence snapshot{};
        snapshot.Sequence = sequence;
        snapshot.AcknowledgedSequence = acknowledgedSequence;
        return snapshot;
    }

    return Ia64LocalTimerInterruptEvidence{
        .Sequence = sequence,
        .ProgrammedDeadlineTicks = evidence.ProgrammedDeadlineTicks,
        .ObservedIntervalTimeCounter = evidence.ObservedIntervalTimeCounter,
        .InterruptedInstructionPointer = evidence.InterruptedInstructionPointer,
        .InterruptedProcessorStatus = evidence.InterruptedProcessorStatus,
        .InterruptedIfsValid = evidence.InterruptedIfsValid,
        .ProcessorIndex = evidence.ProcessorIndex,
        .Vector = evidence.Vector,
        .Reserved =
            {
                evidence.Reserved[0U],
                evidence.Reserved[1U],
                evidence.Reserved[2U],
                evidence.Reserved[3U],
                evidence.Reserved[4U],
                evidence.Reserved[5U],
            },
        .AcknowledgedSequence = acknowledgedSequence,
    };
}

Ia64LocalTimerInterruptAcknowledgementError
AcknowledgeIa64LocalTimerInterruptEvidence(Ia64LocalTimerInterruptEvidence& evidence,
                                           const Foundation::UInt64 sequence) noexcept
{
    if (sequence == 0U)
    {
        return Ia64LocalTimerInterruptAcknowledgementError::InvalidSequence;
    }

    const Foundation::UInt64 publishedSequence = __atomic_load_n(&evidence.Sequence, __ATOMIC_ACQUIRE);
    const Foundation::UInt64 acknowledgedSequence = __atomic_load_n(&evidence.AcknowledgedSequence, __ATOMIC_ACQUIRE);
    if ((acknowledgedSequence > publishedSequence) ||
        ((publishedSequence != acknowledgedSequence) && ((publishedSequence - acknowledgedSequence) != 1U)))
    {
        return Ia64LocalTimerInterruptAcknowledgementError::InvalidMailboxState;
    }
    if (acknowledgedSequence == MaximumUInt64)
    {
        return Ia64LocalTimerInterruptAcknowledgementError::SequenceExhausted;
    }
    if (sequence == acknowledgedSequence)
    {
        return Ia64LocalTimerInterruptAcknowledgementError::AlreadyAcknowledged;
    }
    if ((sequence < acknowledgedSequence) || (sequence < publishedSequence))
    {
        return Ia64LocalTimerInterruptAcknowledgementError::StaleSequence;
    }
    if (sequence > publishedSequence)
    {
        return Ia64LocalTimerInterruptAcknowledgementError::ForeignSequence;
    }

    __atomic_store_n(&evidence.AcknowledgedSequence, sequence, __ATOMIC_RELEASE);
    return Ia64LocalTimerInterruptAcknowledgementError::None;
}

Ia64ExternalInterruptPollStatus
ModelIa64ExternalInterruptSequencePoll(const Ia64ExternalInterruptSequencePollModelInput input) noexcept
{
    if (input.ExpectedSequence == 0U)
    {
        return Ia64ExternalInterruptPollStatus::InvalidEvidence;
    }
    const Foundation::UInt64 predecessor = input.ExpectedSequence - 1U;
    if ((input.InitialAcknowledgedSequence != predecessor) ||
        ((input.InitialPublishedSequence != predecessor) && (input.InitialPublishedSequence != input.ExpectedSequence)))
    {
        return Ia64ExternalInterruptPollStatus::InvalidEvidence;
    }
    if (input.InitialPublishedSequence == input.ExpectedSequence)
    {
        return Ia64ExternalInterruptPollStatus::Completed;
    }
    if (input.SequenceBeforeDisable == input.ExpectedSequence)
    {
        return input.SequenceAfterDisable == input.ExpectedSequence ? Ia64ExternalInterruptPollStatus::Completed
                                                                    : Ia64ExternalInterruptPollStatus::InvalidEvidence;
    }
    if ((input.SequenceBeforeDisable != predecessor) || !input.DeadlineReached)
    {
        return Ia64ExternalInterruptPollStatus::InvalidEvidence;
    }
    if (input.SequenceAfterDisable == input.ExpectedSequence)
    {
        return Ia64ExternalInterruptPollStatus::Completed;
    }
    return input.SequenceAfterDisable == predecessor ? Ia64ExternalInterruptPollStatus::TimedOut
                                                     : Ia64ExternalInterruptPollStatus::InvalidEvidence;
}

Ia64ExternalInterruptPollCodeRange GetIa64ExternalInterruptPollCodeRange() noexcept
{
    Ia64ExternalInterruptPollCodeRange range{};
    const Foundation::UInt64 outputAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&range));
    if (AsteriaIa64GetExternalInterruptPollCodeRangeWords(outputAddress) != 0U)
    {
        return Ia64ExternalInterruptPollCodeRange{};
    }
    return range;
}
} // namespace Asteria::Platform::Ia64::Interrupt

// The target routines are in Ia64ExternalInterruptEntry.S.
#if !defined(__ia64__)
extern "C" void AsteriaIa64ExternalInterruptEntry() noexcept
{
    __builtin_trap();
}

extern "C" ASTERIA_NOINLINE_NOIPA Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus
AsteriaIa64PollForExternalInterrupt(const Asteria::Foundation::UInt64 evidenceAddress,
                                    const Asteria::Foundation::UInt64 absoluteDeadlineTicks) noexcept
{
    static_cast<void>(evidenceAddress);
    static_cast<void>(absoluteDeadlineTicks);
    return Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus::NativeUnavailable;
}

extern "C" ASTERIA_NOINLINE_NOIPA Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus
AsteriaIa64PollForExternalInterruptSequence(const Asteria::Foundation::UInt64 evidenceAddress,
                                            const Asteria::Foundation::UInt64 expectedSequence,
                                            const Asteria::Foundation::UInt64 absoluteDeadlineTicks) noexcept
{
    static_cast<void>(evidenceAddress);
    static_cast<void>(expectedSequence);
    static_cast<void>(absoluteDeadlineTicks);
    return Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus::NativeUnavailable;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64GetExternalInterruptPollCodeRangeWords(const Asteria::Foundation::UInt64 outputAddress) noexcept
{
    static_cast<void>(outputAddress);
    return 1U;
}
#endif
