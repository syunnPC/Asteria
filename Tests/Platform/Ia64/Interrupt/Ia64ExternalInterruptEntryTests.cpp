// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Interrupt::AcquireIa64LocalTimerInterruptEvidence;
using Platform::Ia64::Interrupt::AcquireIa64LocalTimerInterruptEvidenceForSequence;
using Platform::Ia64::Interrupt::AcquireIa64LocalTimerInterruptEvidenceSequence;
using Platform::Ia64::Interrupt::AcknowledgeIa64LocalTimerInterruptEvidence;
using Platform::Ia64::Interrupt::GetIa64ExternalInterruptPollCodeRange;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptDisposition;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptIfsValidMask;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptInstructionSlotMask;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptModelError;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptModelInput;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptSequencePollModelInput;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptInterruptedProcessorStatusValue;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence;
using Platform::Ia64::Interrupt::Ia64LocalTimerInterruptAcknowledgementError;
using Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidenceValidationError;
using Platform::Ia64::Interrupt::ModelIa64ExternalInterrupt;
using Platform::Ia64::Interrupt::ModelIa64ExternalInterruptSequencePoll;
using Platform::Ia64::Interrupt::ValidateIa64LocalTimerInterruptEvidence;
using Platform::Ia64::Interrupt::ValidateIa64LocalTimerInterruptEvidenceForSequence;

constexpr Foundation::UInt64 ContextAddress{0x1000U};
constexpr Foundation::UInt64 EvidenceAddress{0x2000U};
constexpr Foundation::UInt64 PollStart{0x4000U};
constexpr Foundation::UInt64 PollEnd{0x4400U};
constexpr Foundation::UInt64 Deadline{0x1000U};
constexpr Foundation::UInt64 ObservedTicks{0x1010U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr Ia64ExternalInterruptEntryContext ValidContext() noexcept
{
    return Ia64ExternalInterruptEntryContext{
        .EvidenceAddress = EvidenceAddress,
        .ProcessorIndex = 2U,
        .TimerVector = Ia64ExternalInterruptTimerVector,
        .ExternalPreemptionControlAddress = 0U,
    };
}

[[nodiscard]] constexpr Ia64ExternalInterruptModelInput
ValidInput(const Foundation::UInt64 instructionSlot = 0U) noexcept
{
    return Ia64ExternalInterruptModelInput{
        .CurrentProcessorStatus = 0x0000'0010'0802'0000ULL,
        .InterruptedInstructionPointer = PollStart + 0x20U,
        .InterruptedProcessorStatus = Ia64ExternalInterruptInterruptedProcessorStatusValue | (instructionSlot << 41U),
        .InterruptedFunctionState = 0x1234U,
        .InterruptionStatus = 0U,
        .ClaimedVector = Ia64ExternalInterruptTimerVector,
        .IntervalTimerMatch = Deadline,
        .IntervalTimeCounter = ObservedTicks,
    };
}

[[nodiscard]] constexpr Ia64LocalTimerInterruptEvidence
ValidEvidence(const Foundation::UInt64 instructionSlot = 0U) noexcept
{
    return Ia64LocalTimerInterruptEvidence{
        .Sequence = 1U,
        .ProgrammedDeadlineTicks = Deadline,
        .ObservedIntervalTimeCounter = ObservedTicks,
        .InterruptedInstructionPointer = PollStart + 0x20U,
        .InterruptedProcessorStatus = Ia64ExternalInterruptInterruptedProcessorStatusValue | (instructionSlot << 41U),
        .InterruptedIfsValid = 0U,
        .ProcessorIndex = 2U,
        .Vector = Ia64ExternalInterruptTimerVector,
        .Reserved = {},
        .AcknowledgedSequence = 0U,
    };
}

[[nodiscard]] Ia64ExternalInterruptModelError
ModelError(const Ia64ExternalInterruptEntryContext& context, const Ia64LocalTimerInterruptEvidence& evidence,
           const Ia64ExternalInterruptModelInput input, const Foundation::UInt64 contextAddress = ContextAddress,
           const Foundation::UInt64 evidenceAddress = EvidenceAddress) noexcept
{
    const auto result = ModelIa64ExternalInterrupt(contextAddress, context, evidenceAddress, evidence, input);
    return result.HasValue() ? Ia64ExternalInterruptModelError::None : result.GetError();
}

void TestHostedAdaptersAndAcquire(TestContext& runner) noexcept
{
    const auto codeRange = GetIa64ExternalInterruptPollCodeRange();
    ASTERIA_EXPECT(runner, "host adapters expose no fabricated legacy or sequence-aware poll execution",
                   codeRange.Start == 0U, codeRange.End == 0U,
                   AsteriaIa64PollForExternalInterrupt(EvidenceAddress, Deadline) ==
                       Ia64ExternalInterruptPollStatus::NativeUnavailable,
                   AsteriaIa64PollForExternalInterruptSequence(EvidenceAddress, 2U, Deadline) ==
                       Ia64ExternalInterruptPollStatus::NativeUnavailable);

    Ia64LocalTimerInterruptEvidence unpublished = ValidEvidence();
    unpublished.Sequence = 0U;
    const Ia64LocalTimerInterruptEvidence acquiredUnpublished = AcquireIa64LocalTimerInterruptEvidence(unpublished);
    unpublished.Sequence = 2U;
    const Ia64LocalTimerInterruptEvidence acquiredInvalid = AcquireIa64LocalTimerInterruptEvidence(unpublished);
    Ia64LocalTimerInterruptEvidence published = ValidEvidence(2U);
    published.InterruptedIfsValid = 0x1234'5678'9ABC'DEF0ULL;
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        published.Reserved[index] = static_cast<Foundation::UInt8>(0xA0U + index);
    }
    const Ia64LocalTimerInterruptEvidence acquiredPublished = AcquireIa64LocalTimerInterruptEvidence(published);
    Ia64LocalTimerInterruptEvidence second = published;
    second.Sequence = 2U;
    second.AcknowledgedSequence = 1U;
    const Ia64LocalTimerInterruptEvidence acquiredSecond =
        AcquireIa64LocalTimerInterruptEvidenceForSequence(second, 2U);
    const Foundation::UInt64 observedSecondSequence = AcquireIa64LocalTimerInterruptEvidenceSequence(second);
    const Ia64LocalTimerInterruptEvidence wrongSecond = AcquireIa64LocalTimerInterruptEvidenceForSequence(second, 3U);
    ASTERIA_EXPECT(
        runner, "legacy and exact-sequence acquire snapshots expose only one outstanding generation",
        acquiredUnpublished.Sequence == 0U, acquiredUnpublished.ProgrammedDeadlineTicks == 0U,
        acquiredInvalid.Sequence == 2U, acquiredInvalid.ProgrammedDeadlineTicks == 0U,
        acquiredInvalid.AcknowledgedSequence == 0U, acquiredPublished.Sequence == 1U,
        acquiredPublished.ProgrammedDeadlineTicks == Deadline,
        acquiredPublished.ObservedIntervalTimeCounter == ObservedTicks,
        acquiredPublished.InterruptedInstructionPointer == PollStart + 0x20U,
        acquiredPublished.InterruptedProcessorStatus == published.InterruptedProcessorStatus,
        acquiredPublished.InterruptedIfsValid == published.InterruptedIfsValid,
        acquiredPublished.ProcessorIndex == published.ProcessorIndex, acquiredPublished.Vector == published.Vector,
        acquiredPublished.Reserved[0U] == 0xA0U, acquiredPublished.Reserved[1U] == 0xA1U,
        acquiredPublished.Reserved[2U] == 0xA2U, acquiredPublished.Reserved[3U] == 0xA3U,
        acquiredPublished.Reserved[4U] == 0xA4U, acquiredPublished.Reserved[5U] == 0xA5U,
        acquiredPublished.AcknowledgedSequence == 0U, acquiredSecond.Sequence == 2U, observedSecondSequence == 2U,
        acquiredSecond.AcknowledgedSequence == 1U, acquiredSecond.ProgrammedDeadlineTicks == Deadline,
        wrongSecond.Sequence == 2U, wrongSecond.AcknowledgedSequence == 1U, wrongSecond.ProgrammedDeadlineTicks == 0U);
}

void TestModelSuccessAndArchitectedVectors(TestContext& runner) noexcept
{
    bool everyInstructionSlotAccepted{true};
    for (Foundation::UInt64 slot{0U}; slot < 3U; ++slot)
    {
        const auto result = ModelIa64ExternalInterrupt(ContextAddress, ValidContext(), EvidenceAddress,
                                                       Ia64LocalTimerInterruptEvidence{}, ValidInput(slot));
        everyInstructionSlotAccepted =
            everyInstructionSlotAccepted && result.HasValue() &&
            (result.GetValue().Disposition == Ia64ExternalInterruptDisposition::ResumeTimer) &&
            (result.GetValue().TimerVectorRegister == Ia64ExternalInterruptMaskedTimerVector) &&
            (result.GetValue().EoiCount == 1U) && (result.GetValue().Evidence.Sequence == 1U) &&
            (result.GetValue().Evidence.ProgrammedDeadlineTicks == Deadline) &&
            (result.GetValue().Evidence.ObservedIntervalTimeCounter == ObservedTicks) &&
            (result.GetValue().Evidence.InterruptedIfsValid == 0U) &&
            (result.GetValue().Evidence.ProcessorIndex == 2U) &&
            (result.GetValue().Evidence.Vector == Ia64ExternalInterruptTimerVector);
    }
    ASTERIA_EXPECT(runner, "timer model accepts RI zero through two and publishes exact mask, EOI, and evidence",
                   everyInstructionSlotAccepted);

    bool everyRestoredUserStatusAccepted{true};
    for (Foundation::UInt64 userMask{0U}; userMask < 16U; ++userMask)
    {
        Ia64ExternalInterruptModelInput input = ValidInput();
        input.InterruptedProcessorStatus |= userMask << 2U;
        const auto restoredStatusResult = ModelIa64ExternalInterrupt(ContextAddress, ValidContext(), EvidenceAddress,
                                                                     Ia64LocalTimerInterruptEvidence{}, input);
        everyRestoredUserStatusAccepted =
            everyRestoredUserStatusAccepted && restoredStatusResult.HasValue() &&
            (restoredStatusResult.GetValue().Evidence.InterruptedProcessorStatus == input.InterruptedProcessorStatus);
    }
    ASTERIA_EXPECT(runner, "every interrupted UP/AC/MFL/MFH state is accepted and preserved",
                   everyRestoredUserStatusAccepted);

    Ia64ExternalInterruptModelInput spurious = ValidInput();
    spurious.ClaimedVector = 15U;
    const auto result = ModelIa64ExternalInterrupt(ContextAddress, ValidContext(), EvidenceAddress,
                                                   Ia64LocalTimerInterruptEvidence{}, spurious);
    ASTERIA_EXPECT(runner, "architected spurious vector resumes without EOI or evidence", result.HasValue(),
                   result.GetValue().Disposition == Ia64ExternalInterruptDisposition::ResumeSpurious,
                   result.GetValue().EoiCount == 0U, result.GetValue().Evidence.Sequence == 0U);
}

void TestModelStateRejections(TestContext& runner) noexcept
{
    Ia64ExternalInterruptModelInput input = ValidInput();
    input.CurrentProcessorStatus ^= Foundation::UInt64{1U} << 14U;
    ASTERIA_EXPECT(runner, "current delivery state is exact before IVR",
                   ModelError(ValidContext(), {}, input) == Ia64ExternalInterruptModelError::IncompatibleCurrentState);

    input = ValidInput();
    input.CurrentProcessorStatus |= Foundation::UInt64{1U} << 1U;
    ASTERIA_EXPECT(runner, "current big-endian state is rejected before packed evidence stores",
                   ModelError(ValidContext(), {}, input) == Ia64ExternalInterruptModelError::IncompatibleCurrentState);

    constexpr Foundation::UInt64 interruptedStateMutationBits[]{
        Foundation::UInt64{1U} << 0U,  Foundation::UInt64{1U} << 1U,  Foundation::UInt64{1U} << 6U,
        Foundation::UInt64{1U} << 13U, Foundation::UInt64{1U} << 14U, Foundation::UInt64{1U} << 17U,
        Foundation::UInt64{1U} << 32U, Foundation::UInt64{1U} << 34U, Foundation::UInt64{1U} << 44U,
        Foundation::UInt64{1U} << 46U,
    };
    bool exactInterruptedStateRejected{true};
    for (const Foundation::UInt64 bit : interruptedStateMutationBits)
    {
        input = ValidInput();
        input.InterruptedProcessorStatus ^= bit;
        exactInterruptedStateRejected =
            exactInterruptedStateRejected &&
            (ModelError(ValidContext(), {}, input) == Ia64ExternalInterruptModelError::IncompatibleInterruptedState);
    }
    input = ValidInput(3U);
    exactInterruptedStateRejected =
        exactInterruptedStateRejected &&
        (ModelError(ValidContext(), {}, input) == Ia64ExternalInterruptModelError::IncompatibleInterruptedState);
    ASTERIA_EXPECT(runner, "BE, reserved, CPL0, native translated bank-one i/ic state, and legal RI remain exact",
                   exactInterruptedStateRejected);

    input = ValidInput();
    input.InterruptedInstructionPointer += 1U;
    const Ia64ExternalInterruptModelError misalignedIip = ModelError(ValidContext(), {}, input);
    input = ValidInput();
    input.InterruptedFunctionState |= Ia64ExternalInterruptIfsValidMask;
    const Ia64ExternalInterruptModelError validIfs = ModelError(ValidContext(), {}, input);
    input = ValidInput();
    input.InterruptionStatus |= Foundation::UInt64{1U} << 39U;
    const Ia64ExternalInterruptModelError nested = ModelError(ValidContext(), {}, input);
    ASTERIA_EXPECT(runner, "IIP alignment, only IFS.v, and ISR.ni are rejected before destructive IVR",
                   misalignedIip == Ia64ExternalInterruptModelError::MisalignedInterruptedInstructionPointer,
                   validIfs == Ia64ExternalInterruptModelError::IfsValid,
                   nested == Ia64ExternalInterruptModelError::NestedInterruption);
}

void TestModelContextAndEvidenceRejections(TestContext& runner) noexcept
{
    const Ia64ExternalInterruptModelInput input = ValidInput();
    ASTERIA_EXPECT(runner, "context null, alignment, and complete Region-0 range are validated",
                   ModelError(ValidContext(), {}, input, 0U) == Ia64ExternalInterruptModelError::InvalidContextAddress,
                   ModelError(ValidContext(), {}, input, ContextAddress + 1U) ==
                       Ia64ExternalInterruptModelError::InvalidContextAddress,
                   ModelError(ValidContext(), {}, input, (Foundation::UInt64{1U} << 61U) - 16U) ==
                       Ia64ExternalInterruptModelError::InvalidContextAddress);

    Ia64ExternalInterruptEntryContext context = ValidContext();
    context.ProcessorIndex = 4U;
    const Ia64ExternalInterruptModelError processor = ModelError(context, {}, input);
    context = ValidContext();
    context.TimerVector = 0xE7U;
    const Ia64ExternalInterruptModelError vector = ModelError(context, {}, input);
    context = ValidContext();
    context.ExternalPreemptionControlAddress = 0x4000U;
    const Ia64ExternalInterruptModelError ignoredNativeControl = ModelError(context, {}, input);
    ASTERIA_EXPECT(runner, "legacy evidence validates its fields and ignores dispatcher control",
                   processor == Ia64ExternalInterruptModelError::InvalidContext,
                   vector == Ia64ExternalInterruptModelError::InvalidContext,
                   ignoredNativeControl == Ia64ExternalInterruptModelError::None);

    context = ValidContext();
    context.EvidenceAddress = 0U;
    const Ia64ExternalInterruptModelError addressMismatch = ModelError(context, {}, input);
    context = ValidContext();
    const Ia64ExternalInterruptModelError misalignedEvidence =
        ModelError(context, {}, input, ContextAddress, EvidenceAddress + 1U);
    context.EvidenceAddress = ContextAddress;
    const Ia64ExternalInterruptModelError overlappingEvidence =
        ModelError(context, {}, input, ContextAddress, ContextAddress);
    ASTERIA_EXPECT(runner, "evidence identity, alignment, and non-overlap precede native use",
                   addressMismatch == Ia64ExternalInterruptModelError::InvalidEvidenceAddress,
                   misalignedEvidence == Ia64ExternalInterruptModelError::InvalidEvidenceAddress,
                   overlappingEvidence == Ia64ExternalInterruptModelError::InvalidEvidenceAddress);

    Ia64LocalTimerInterruptEvidence previous = ValidEvidence();
    previous.AcknowledgedSequence = previous.Sequence;
    const auto reusable = ModelIa64ExternalInterrupt(ContextAddress, ValidContext(), EvidenceAddress, previous, input);
    Ia64LocalTimerInterruptEvidence outstanding = previous;
    outstanding.Sequence = 2U;
    Ia64LocalTimerInterruptEvidence exhausted = previous;
    exhausted.Sequence = MaximumUInt64;
    exhausted.AcknowledgedSequence = MaximumUInt64;
    ASTERIA_EXPECT(
        runner, "acknowledged legacy payload is reusable while outstanding and exhausted generations are rejected",
        reusable.HasValue(), reusable.GetValue().Evidence.Sequence == 2U,
        reusable.GetValue().Evidence.AcknowledgedSequence == 1U,
        ModelError(ValidContext(), outstanding, input) == Ia64ExternalInterruptModelError::EvidenceOutstanding,
        ModelError(ValidContext(), exhausted, input) == Ia64ExternalInterruptModelError::EvidenceSequenceExhausted);
}

void TestModelClaimAndTimerRejections(TestContext& runner) noexcept
{
    Ia64ExternalInterruptModelInput input = ValidInput();
    input.ClaimedVector = 0xE7U;
    const Ia64ExternalInterruptModelError unknown = ModelError(ValidContext(), {}, input);
    input = ValidInput();
    input.IntervalTimerMatch = 0U;
    const Ia64ExternalInterruptModelError zeroDeadline = ModelError(ValidContext(), {}, input);
    input = ValidInput();
    input.IntervalTimerMatch = MaximumUInt64;
    const Ia64ExternalInterruptModelError maximumDeadline = ModelError(ValidContext(), {}, input);
    input = ValidInput();
    input.IntervalTimeCounter = Deadline - 1U;
    const Ia64ExternalInterruptModelError earlyCounter = ModelError(ValidContext(), {}, input);
    ASTERIA_EXPECT(runner, "unknown claims and invalid ITM/ITC relationships cannot resume",
                   unknown == Ia64ExternalInterruptModelError::UnexpectedClaimedVector,
                   zeroDeadline == Ia64ExternalInterruptModelError::InvalidTimerState,
                   maximumDeadline == Ia64ExternalInterruptModelError::InvalidTimerState,
                   earlyCounter == Ia64ExternalInterruptModelError::InvalidTimerState);
}

void TestReusableMailboxCycles(TestContext& runner) noexcept
{
    const auto first = ModelIa64ExternalInterrupt(ContextAddress, ValidContext(), EvidenceAddress,
                                                  Ia64LocalTimerInterruptEvidence{}, ValidInput());
    Ia64LocalTimerInterruptEvidence mailbox =
        first.HasValue() ? first.GetValue().Evidence : Ia64LocalTimerInterruptEvidence{};
    const Ia64ExternalInterruptModelError outstanding = ModelError(ValidContext(), mailbox, ValidInput());
    const Ia64LocalTimerInterruptAcknowledgementError invalid = AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 0U);
    const Ia64LocalTimerInterruptAcknowledgementError foreign = AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 2U);
    const Ia64LocalTimerInterruptAcknowledgementError firstAcknowledged =
        AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 1U);
    const Ia64LocalTimerInterruptAcknowledgementError firstDouble =
        AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 1U);
    const auto second =
        ModelIa64ExternalInterrupt(ContextAddress, ValidContext(), EvidenceAddress, mailbox, ValidInput(2U));
    if (second.HasValue())
    {
        mailbox = second.GetValue().Evidence;
    }
    const Ia64LocalTimerInterruptAcknowledgementError oldDouble =
        AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 1U);
    const Ia64LocalTimerInterruptAcknowledgementError secondForeign =
        AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 3U);
    const Ia64LocalTimerInterruptAcknowledgementError secondAcknowledged =
        AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 2U);
    const Ia64LocalTimerInterruptAcknowledgementError stale = AcknowledgeIa64LocalTimerInterruptEvidence(mailbox, 1U);

    Ia64LocalTimerInterruptEvidence malformed = mailbox;
    malformed.Sequence = 5U;
    malformed.AcknowledgedSequence = 2U;
    const Ia64LocalTimerInterruptAcknowledgementError malformedState =
        AcknowledgeIa64LocalTimerInterruptEvidence(malformed, 5U);
    Ia64LocalTimerInterruptEvidence exhausted{};
    exhausted.Sequence = MaximumUInt64;
    exhausted.AcknowledgedSequence = MaximumUInt64;
    const Ia64LocalTimerInterruptAcknowledgementError exhaustion =
        AcknowledgeIa64LocalTimerInterruptEvidence(exhausted, MaximumUInt64);

    ASTERIA_EXPECT(runner,
                   "two publish/ack cycles retain exact double, stale, foreign, malformed, and overflow diagnostics",
                   first.HasValue(), outstanding == Ia64ExternalInterruptModelError::EvidenceOutstanding,
                   invalid == Ia64LocalTimerInterruptAcknowledgementError::InvalidSequence,
                   foreign == Ia64LocalTimerInterruptAcknowledgementError::ForeignSequence,
                   firstAcknowledged == Ia64LocalTimerInterruptAcknowledgementError::None,
                   firstDouble == Ia64LocalTimerInterruptAcknowledgementError::AlreadyAcknowledged, second.HasValue(),
                   oldDouble == Ia64LocalTimerInterruptAcknowledgementError::AlreadyAcknowledged,
                   secondForeign == Ia64LocalTimerInterruptAcknowledgementError::ForeignSequence,
                   secondAcknowledged == Ia64LocalTimerInterruptAcknowledgementError::None,
                   stale == Ia64LocalTimerInterruptAcknowledgementError::StaleSequence,
                   malformedState == Ia64LocalTimerInterruptAcknowledgementError::InvalidMailboxState,
                   exhaustion == Ia64LocalTimerInterruptAcknowledgementError::SequenceExhausted);
    ASTERIA_EXPECT(
        runner, "each cycle rewrites every preserved legacy payload field before publishing the next sequence",
        second.HasValue(), mailbox.Sequence == 2U, mailbox.AcknowledgedSequence == 2U,
        second.GetValue().Evidence.ProgrammedDeadlineTicks == Deadline,
        second.GetValue().Evidence.ObservedIntervalTimeCounter == ObservedTicks,
        second.GetValue().Evidence.InterruptedInstructionPointer == PollStart + 0x20U,
        second.GetValue().Evidence.InterruptedProcessorStatus == ValidInput(2U).InterruptedProcessorStatus,
        second.GetValue().Evidence.InterruptedIfsValid == 0U, second.GetValue().Evidence.ProcessorIndex == 2U,
        second.GetValue().Evidence.Vector == Ia64ExternalInterruptTimerVector,
        second.GetValue().Evidence.Reserved[0U] == 0U, second.GetValue().Evidence.Reserved[5U] == 0U);
}

void TestTwoMailboxCpuIsolation(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 processorZeroContextAddress{0x6000U};
    constexpr Foundation::UInt64 processorZeroEvidenceAddress{0x7000U};
    constexpr Foundation::UInt64 processorOneContextAddress{0x8000U};
    constexpr Foundation::UInt64 processorOneEvidenceAddress{0x9000U};
    const Ia64ExternalInterruptEntryContext processorZeroContext{
        .EvidenceAddress = processorZeroEvidenceAddress,
        .ProcessorIndex = 0U,
        .TimerVector = Ia64ExternalInterruptTimerVector,
        .ExternalPreemptionControlAddress = 0U,
    };
    const Ia64ExternalInterruptEntryContext processorOneContext{
        .EvidenceAddress = processorOneEvidenceAddress,
        .ProcessorIndex = 1U,
        .TimerVector = Ia64ExternalInterruptTimerVector,
        .ExternalPreemptionControlAddress = 0U,
    };
    const auto processorZeroFirst =
        ModelIa64ExternalInterrupt(processorZeroContextAddress, processorZeroContext, processorZeroEvidenceAddress,
                                   Ia64LocalTimerInterruptEvidence{}, ValidInput());
    const auto processorOneFirst =
        ModelIa64ExternalInterrupt(processorOneContextAddress, processorOneContext, processorOneEvidenceAddress,
                                   Ia64LocalTimerInterruptEvidence{}, ValidInput(1U));
    Ia64LocalTimerInterruptEvidence processorZeroMailbox =
        processorZeroFirst.HasValue() ? processorZeroFirst.GetValue().Evidence : Ia64LocalTimerInterruptEvidence{};
    Ia64LocalTimerInterruptEvidence processorOneMailbox =
        processorOneFirst.HasValue() ? processorOneFirst.GetValue().Evidence : Ia64LocalTimerInterruptEvidence{};
    const Ia64LocalTimerInterruptAcknowledgementError processorZeroAck =
        AcknowledgeIa64LocalTimerInterruptEvidence(processorZeroMailbox, 1U);
    const auto processorZeroSecond =
        ModelIa64ExternalInterrupt(processorZeroContextAddress, processorZeroContext, processorZeroEvidenceAddress,
                                   processorZeroMailbox, ValidInput(2U));
    const auto processorOneStillOutstanding =
        ModelIa64ExternalInterrupt(processorOneContextAddress, processorOneContext, processorOneEvidenceAddress,
                                   processorOneMailbox, ValidInput());
    const auto crossMailboxAddress =
        ModelIa64ExternalInterrupt(processorZeroContextAddress, processorZeroContext, processorOneEvidenceAddress,
                                   processorOneMailbox, ValidInput());
    const Ia64LocalTimerInterruptAcknowledgementError processorOneAck =
        AcknowledgeIa64LocalTimerInterruptEvidence(processorOneMailbox, 1U);
    const auto processorOneSecond =
        ModelIa64ExternalInterrupt(processorOneContextAddress, processorOneContext, processorOneEvidenceAddress,
                                   processorOneMailbox, ValidInput());
    ASTERIA_EXPECT(runner, "two CPU-local mailboxes advance and acknowledge independently without cross-address use",
                   processorZeroFirst.HasValue(), processorOneFirst.HasValue(),
                   processorZeroFirst.GetValue().Evidence.ProcessorIndex == 0U,
                   processorOneFirst.GetValue().Evidence.ProcessorIndex == 1U,
                   processorZeroAck == Ia64LocalTimerInterruptAcknowledgementError::None,
                   processorZeroSecond.HasValue(), processorZeroSecond.GetValue().Evidence.Sequence == 2U,
                   !processorOneStillOutstanding.HasValue(),
                   processorOneStillOutstanding.GetError() == Ia64ExternalInterruptModelError::EvidenceOutstanding,
                   processorOneMailbox.Sequence == 1U, processorOneMailbox.AcknowledgedSequence == 1U,
                   !crossMailboxAddress.HasValue(),
                   crossMailboxAddress.GetError() == Ia64ExternalInterruptModelError::InvalidEvidenceAddress,
                   processorOneAck == Ia64LocalTimerInterruptAcknowledgementError::None, processorOneSecond.HasValue(),
                   processorOneSecond.GetValue().Evidence.Sequence == 2U,
                   processorOneSecond.GetValue().Evidence.ProcessorIndex == 1U);
}

void TestExactSequencePollModel(TestContext& runner) noexcept
{
    const Ia64ExternalInterruptPollStatus initiallyCompleted =
        ModelIa64ExternalInterruptSequencePoll(Ia64ExternalInterruptSequencePollModelInput{
            .ExpectedSequence = 2U,
            .InitialPublishedSequence = 2U,
            .InitialAcknowledgedSequence = 1U,
            .SequenceBeforeDisable = 0U,
            .SequenceAfterDisable = 0U,
            .DeadlineReached = false,
            .Reserved = {},
        });
    const Ia64ExternalInterruptPollStatus completed =
        ModelIa64ExternalInterruptSequencePoll(Ia64ExternalInterruptSequencePollModelInput{
            .ExpectedSequence = 2U,
            .InitialPublishedSequence = 1U,
            .InitialAcknowledgedSequence = 1U,
            .SequenceBeforeDisable = 2U,
            .SequenceAfterDisable = 2U,
            .DeadlineReached = false,
            .Reserved = {},
        });
    const Ia64ExternalInterruptPollStatus timeoutRace =
        ModelIa64ExternalInterruptSequencePoll(Ia64ExternalInterruptSequencePollModelInput{
            .ExpectedSequence = 2U,
            .InitialPublishedSequence = 1U,
            .InitialAcknowledgedSequence = 1U,
            .SequenceBeforeDisable = 1U,
            .SequenceAfterDisable = 2U,
            .DeadlineReached = true,
            .Reserved = {},
        });
    const Ia64ExternalInterruptPollStatus timedOut =
        ModelIa64ExternalInterruptSequencePoll(Ia64ExternalInterruptSequencePollModelInput{
            .ExpectedSequence = 2U,
            .InitialPublishedSequence = 1U,
            .InitialAcknowledgedSequence = 1U,
            .SequenceBeforeDisable = 1U,
            .SequenceAfterDisable = 1U,
            .DeadlineReached = true,
            .Reserved = {},
        });
    const Ia64ExternalInterruptPollStatus foreign =
        ModelIa64ExternalInterruptSequencePoll(Ia64ExternalInterruptSequencePollModelInput{
            .ExpectedSequence = 2U,
            .InitialPublishedSequence = 1U,
            .InitialAcknowledgedSequence = 1U,
            .SequenceBeforeDisable = 3U,
            .SequenceAfterDisable = 3U,
            .DeadlineReached = false,
            .Reserved = {},
        });
    const Ia64ExternalInterruptPollStatus outstanding =
        ModelIa64ExternalInterruptSequencePoll(Ia64ExternalInterruptSequencePollModelInput{
            .ExpectedSequence = 3U,
            .InitialPublishedSequence = 2U,
            .InitialAcknowledgedSequence = 1U,
            .SequenceBeforeDisable = 2U,
            .SequenceAfterDisable = 2U,
            .DeadlineReached = true,
            .Reserved = {},
        });
    ASTERIA_EXPECT(runner,
                   "exact polling accepts an immediate publication or exact next "
                   "generation and closes timeout with a disabled acquire recheck",
                   initiallyCompleted == Ia64ExternalInterruptPollStatus::Completed,
                   completed == Ia64ExternalInterruptPollStatus::Completed,
                   timeoutRace == Ia64ExternalInterruptPollStatus::Completed,
                   timedOut == Ia64ExternalInterruptPollStatus::TimedOut,
                   foreign == Ia64ExternalInterruptPollStatus::InvalidEvidence,
                   outstanding == Ia64ExternalInterruptPollStatus::InvalidEvidence);
}

void TestEvidenceValidation(TestContext& runner) noexcept
{
    bool everyInstructionSlotAccepted{true};
    for (Foundation::UInt64 slot{0U}; slot < 3U; ++slot)
    {
        everyInstructionSlotAccepted =
            everyInstructionSlotAccepted &&
            (ValidateIa64LocalTimerInterruptEvidence(ValidEvidence(slot), 2U, Deadline, PollStart, PollEnd) ==
             Ia64LocalTimerInterruptEvidenceValidationError::None);
    }
    ASTERIA_EXPECT(runner, "evidence validator accepts all three architectural instruction slots",
                   everyInstructionSlotAccepted);

    Ia64LocalTimerInterruptEvidence secondGeneration = ValidEvidence(2U);
    secondGeneration.Sequence = 2U;
    secondGeneration.AcknowledgedSequence = 1U;
    Ia64LocalTimerInterruptEvidence alreadyAcknowledged = secondGeneration;
    alreadyAcknowledged.AcknowledgedSequence = 2U;
    ASTERIA_EXPECT(
        runner,
        "sequence-aware validation accepts only one exact outstanding generation while legacy remains sequence one",
        ValidateIa64LocalTimerInterruptEvidenceForSequence(secondGeneration, 2U, 2U, Deadline, PollStart, PollEnd) ==
            Ia64LocalTimerInterruptEvidenceValidationError::None,
        ValidateIa64LocalTimerInterruptEvidence(secondGeneration, 2U, Deadline, PollStart, PollEnd) ==
            Ia64LocalTimerInterruptEvidenceValidationError::Incomplete,
        ValidateIa64LocalTimerInterruptEvidenceForSequence(alreadyAcknowledged, 2U, 2U, Deadline, PollStart, PollEnd) ==
            Ia64LocalTimerInterruptEvidenceValidationError::Incomplete);

    bool everyRestoredUserStatusAccepted{true};
    for (Foundation::UInt64 userMask{0U}; userMask < 16U; ++userMask)
    {
        Ia64LocalTimerInterruptEvidence evidence = ValidEvidence();
        evidence.InterruptedProcessorStatus |= userMask << 2U;
        everyRestoredUserStatusAccepted =
            everyRestoredUserStatusAccepted &&
            (ValidateIa64LocalTimerInterruptEvidence(evidence, 2U, Deadline, PollStart, PollEnd) ==
             Ia64LocalTimerInterruptEvidenceValidationError::None);
    }
    ASTERIA_EXPECT(runner, "evidence validator accepts every restored UP/AC/MFL/MFH state",
                   everyRestoredUserStatusAccepted);

    const Ia64LocalTimerInterruptEvidence valid = ValidEvidence();
    ASTERIA_EXPECT(runner, "expected processor, deadline, and raw polling interval are validated independently",
                   ValidateIa64LocalTimerInterruptEvidence(valid, 4U, Deadline, PollStart, PollEnd) ==
                       Ia64LocalTimerInterruptEvidenceValidationError::InvalidExpectedProcessor,
                   ValidateIa64LocalTimerInterruptEvidence(valid, 2U, 0U, PollStart, PollEnd) ==
                       Ia64LocalTimerInterruptEvidenceValidationError::InvalidExpectedDeadline,
                   ValidateIa64LocalTimerInterruptEvidence(valid, 2U, Deadline, PollStart + 1U, PollEnd) ==
                       Ia64LocalTimerInterruptEvidenceValidationError::InvalidPollingRange);

    Ia64LocalTimerInterruptEvidence alignedEndIip = valid;
    alignedEndIip.InterruptedInstructionPointer = PollEnd;
    ASTERIA_EXPECT(runner, "misaligned polling end is rejected before a newly included aligned IIP",
                   ValidateIa64LocalTimerInterruptEvidence(alignedEndIip, 2U, Deadline, PollStart, PollEnd + 1U) ==
                       Ia64LocalTimerInterruptEvidenceValidationError::InvalidPollingRange);

    constexpr Ia64LocalTimerInterruptEvidenceValidationError expectedErrors[]{
        Ia64LocalTimerInterruptEvidenceValidationError::Incomplete,
        Ia64LocalTimerInterruptEvidenceValidationError::DeadlineMismatch,
        Ia64LocalTimerInterruptEvidenceValidationError::CounterBeforeDeadline,
        Ia64LocalTimerInterruptEvidenceValidationError::InstructionPointerOutsidePollingRange,
        Ia64LocalTimerInterruptEvidenceValidationError::InvalidInterruptedProcessorStatus,
        Ia64LocalTimerInterruptEvidenceValidationError::IfsValid,
        Ia64LocalTimerInterruptEvidenceValidationError::ProcessorMismatch,
        Ia64LocalTimerInterruptEvidenceValidationError::VectorMismatch,
    };
    bool representativeFieldsRejected{true};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        Ia64LocalTimerInterruptEvidence mutated = ValidEvidence();
        switch (index)
        {
        case 0U:
            mutated.Sequence = 0U;
            break;
        case 1U:
            mutated.ProgrammedDeadlineTicks += 1U;
            break;
        case 2U:
            mutated.ObservedIntervalTimeCounter = Deadline - 1U;
            break;
        case 3U:
            mutated.InterruptedInstructionPointer = PollEnd;
            break;
        case 4U:
            mutated.InterruptedProcessorStatus ^= Foundation::UInt64{1U} << 44U;
            break;
        case 5U:
            mutated.InterruptedIfsValid = 1U;
            break;
        case 6U:
            mutated.ProcessorIndex = 1U;
            break;
        case 7U:
            mutated.Vector = 0xE7U;
            break;
        }
        representativeFieldsRejected =
            representativeFieldsRejected && (ValidateIa64LocalTimerInterruptEvidence(mutated, 2U, Deadline, PollStart,
                                                                                     PollEnd) == expectedErrors[index]);
    }
    ASTERIA_EXPECT(runner, "every semantic evidence field has an independent rejection outcome",
                   representativeFieldsRejected);

    bool everyReservedByteRejected{true};
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        Ia64LocalTimerInterruptEvidence mutated = ValidEvidence();
        mutated.Reserved[index] = 1U;
        everyReservedByteRejected =
            everyReservedByteRejected &&
            (ValidateIa64LocalTimerInterruptEvidence(mutated, 2U, Deadline, PollStart, PollEnd) ==
             Ia64LocalTimerInterruptEvidenceValidationError::ReservedNotZero);
    }
    Ia64LocalTimerInterruptEvidence reservedInstructionSlot = ValidEvidence();
    reservedInstructionSlot.InterruptedProcessorStatus |= Ia64ExternalInterruptInstructionSlotMask;
    ASTERIA_EXPECT(runner, "reserved evidence bytes and RI encoding three are rejected", everyReservedByteRejected,
                   ValidateIa64LocalTimerInterruptEvidence(reservedInstructionSlot, 2U, Deadline, PollStart, PollEnd) ==
                       Ia64LocalTimerInterruptEvidenceValidationError::InvalidInterruptedProcessorStatus);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ExternalInterruptEntryTests"};
    Asteria::Test::TestHostedAdaptersAndAcquire(runner);
    Asteria::Test::TestModelSuccessAndArchitectedVectors(runner);
    Asteria::Test::TestModelStateRejections(runner);
    Asteria::Test::TestModelContextAndEvidenceRejections(runner);
    Asteria::Test::TestModelClaimAndTimerRejections(runner);
    Asteria::Test::TestReusableMailboxCycles(runner);
    Asteria::Test::TestTwoMailboxCpuIsolation(runner);
    Asteria::Test::TestExactSequencePollModel(runner);
    Asteria::Test::TestEvidenceValidation(runner);
    return runner.Finish();
}
