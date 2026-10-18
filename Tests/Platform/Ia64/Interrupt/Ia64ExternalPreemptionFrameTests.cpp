// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionFrame.hpp"

#include "Support/TestContext.hpp"
#include <cstdio>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Platform::Ia64::Interrupt::AcquireIa64ExternalPreemptionToken;
using Platform::Ia64::Interrupt::AdvanceIa64ExternalPreemptionCapture;
using Platform::Ia64::Interrupt::BeginIa64ExternalPreemptionCapture;
using Platform::Ia64::Interrupt::CommitIa64ExternalPreemptionResume;
using Platform::Ia64::Interrupt::CompleteIa64ExternalPreemptionTimerAcknowledgement;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCaptureRequest;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCaptureStage;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrame;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrameError;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrameIsOwned;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrameState;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionTimerEvidence;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Platform::Ia64::Interrupt::ModelReleaseIa64ExternalPreemptionFrameForRfi;
using Platform::Ia64::Interrupt::PublishIa64ExternalPreemptionCapture;

namespace
{
constexpr UInt64 MaximumUInt64{~UInt64{0U}};
constexpr UInt64 Deadline{0x1020'3040U};
constexpr UInt64 ObservedTicks{Deadline + 9U};

void CopyBytes(const void* const source, UInt8* const destination, const Size size) noexcept
{
    const auto* const bytes = static_cast<const UInt8*>(source);
    for (Size index{0U}; index < size; ++index)
    {
        destination[index] = bytes[index];
    }
}

[[nodiscard]] bool BytesAreEqual(const void* const value, const UInt8* const expected, const Size size) noexcept
{
    const auto* const bytes = static_cast<const UInt8*>(value);
    for (Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr Ia64ExternalPreemptionCaptureRequest ValidRequest() noexcept
{
    return Ia64ExternalPreemptionCaptureRequest{
        .ProcessorIndex = 0U,
        .TimerVector = Ia64ExternalInterruptTimerVector,
        .ProgrammedDeadlineTicks = Deadline,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr Ia64ExternalPreemptionTimerEvidence ValidEvidence() noexcept
{
    return Ia64ExternalPreemptionTimerEvidence{
        .ClaimedVector = Ia64ExternalInterruptTimerVector,
        .ObservedTicks = ObservedTicks,
        .MaskedTimerVector = Ia64ExternalInterruptMaskedTimerVector,
        .EoiCount = 1U,
    };
}

[[nodiscard]] bool AdvanceThroughRse(Ia64ExternalPreemptionFrame& frame, const UInt64 token) noexcept
{
    constexpr Ia64ExternalPreemptionCaptureStage stages[]{
        Ia64ExternalPreemptionCaptureStage::InterruptionState, Ia64ExternalPreemptionCaptureStage::IntegerState,
        Ia64ExternalPreemptionCaptureStage::ApplicationState,  Ia64ExternalPreemptionCaptureStage::FloatingState,
        Ia64ExternalPreemptionCaptureStage::RseTransition,
    };
    for (const auto stage : stages)
    {
        if (AdvanceIa64ExternalPreemptionCapture(frame, token, stage) != Ia64ExternalPreemptionFrameError::None)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool RetainFrame(Ia64ExternalPreemptionFrame& frame, UInt64& token) noexcept
{
    return (BeginIa64ExternalPreemptionCapture(frame, ValidRequest(), token) ==
            Ia64ExternalPreemptionFrameError::None) &&
           AdvanceThroughRse(frame, token) &&
           (CompleteIa64ExternalPreemptionTimerAcknowledgement(frame, token, ValidEvidence()) ==
            Ia64ExternalPreemptionFrameError::None) &&
           (PublishIa64ExternalPreemptionCapture(frame, token) == Ia64ExternalPreemptionFrameError::None);
}

void TestDefaultAndBeginFailureAtomicity(TestContext& runner) noexcept
{
    Ia64ExternalPreemptionFrame frame{};
    ASTERIA_EXPECT(runner, "a fresh persistent frame has no published or target-owned capture",
                   AcquireIa64ExternalPreemptionToken(frame) == 0U, !Ia64ExternalPreemptionFrameIsOwned(frame),
                   frame.CaptureToken == 0U, frame.NextToken == 1U,
                   frame.State == Ia64ExternalPreemptionFrameState::Inactive,
                   frame.CaptureStage == Ia64ExternalPreemptionCaptureStage::None);

    frame.PublishedToken = 7U;
    ASTERIA_EXPECT(runner, "a stray published token conservatively retains frame ownership",
                   Ia64ExternalPreemptionFrameIsOwned(frame));
    frame.PublishedToken = 0U;

    UInt8 expected[sizeof(frame)]{};
    CopyBytes(&frame, expected, sizeof(frame));
    UInt64 token{0xA5A5U};
    auto request = ValidRequest();
    request.TimerVector = 0xEFU;
    ASTERIA_EXPECT(runner, "a foreign vector is rejected without reserving state or a token",
                   BeginIa64ExternalPreemptionCapture(frame, request, token) ==
                       Ia64ExternalPreemptionFrameError::InvalidRequest,
                   token == 0xA5A5U, BytesAreEqual(&frame, expected, sizeof(frame)));

    request = ValidRequest();
    request.Reserved = 1U;
    ASTERIA_EXPECT(runner, "reserved capture input is rejected without changing the frame",
                   BeginIa64ExternalPreemptionCapture(frame, request, token) ==
                       Ia64ExternalPreemptionFrameError::InvalidRequest,
                   token == 0xA5A5U, BytesAreEqual(&frame, expected, sizeof(frame)));

    request = ValidRequest();
    request.ProcessorIndex = Platform::Ia64::Interrupt::Ia64ExternalInterruptMaximumProcessorCount;
    ASTERIA_EXPECT(runner, "an out-of-range processor cannot own a native capture",
                   BeginIa64ExternalPreemptionCapture(frame, request, token) ==
                       Ia64ExternalPreemptionFrameError::InvalidRequest,
                   token == 0xA5A5U, BytesAreEqual(&frame, expected, sizeof(frame)));

    request = ValidRequest();
    request.ProgrammedDeadlineTicks = 0U;
    ASTERIA_EXPECT(runner, "zero cannot identify an armed hardware deadline",
                   BeginIa64ExternalPreemptionCapture(frame, request, token) ==
                       Ia64ExternalPreemptionFrameError::InvalidRequest,
                   token == 0xA5A5U, BytesAreEqual(&frame, expected, sizeof(frame)));
    request.ProgrammedDeadlineTicks = MaximumUInt64;
    ASTERIA_EXPECT(runner, "the reserved maximum tick cannot identify an armed deadline",
                   BeginIa64ExternalPreemptionCapture(frame, request, token) ==
                       Ia64ExternalPreemptionFrameError::InvalidRequest,
                   token == 0xA5A5U, BytesAreEqual(&frame, expected, sizeof(frame)));

    frame.NextToken = 0U;
    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "token exhaustion rejects capture without an ABA publication",
                   BeginIa64ExternalPreemptionCapture(frame, ValidRequest(), token) ==
                       Ia64ExternalPreemptionFrameError::TokenExhausted,
                   token == 0xA5A5U, BytesAreEqual(&frame, expected, sizeof(frame)));
}

void TestOrderedCaptureAndTimerEvidence(TestContext& runner) noexcept
{
    Ia64ExternalPreemptionFrame frame{};
    UInt64 token{0U};
    ASTERIA_EXPECT(
        runner, "begin reserves a nonzero generation without publishing a frame",
        BeginIa64ExternalPreemptionCapture(frame, ValidRequest(), token) == Ia64ExternalPreemptionFrameError::None,
        token == 1U, frame.CaptureToken == 1U, frame.NextToken == 2U, Ia64ExternalPreemptionFrameIsOwned(frame),
        frame.State == Ia64ExternalPreemptionFrameState::Capturing, AcquireIa64ExternalPreemptionToken(frame) == 0U);

    UInt8 expected[sizeof(frame)]{};
    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "a second capture cannot overwrite an in-progress native frame",
                   BeginIa64ExternalPreemptionCapture(frame, ValidRequest(), token) ==
                       Ia64ExternalPreemptionFrameError::AlreadyOwned,
                   BytesAreEqual(&frame, expected, sizeof(frame)));
    ASTERIA_EXPECT(runner, "a stale capture token cannot advance architectural ownership",
                   AdvanceIa64ExternalPreemptionCapture(frame, token + 1U,
                                                        Ia64ExternalPreemptionCaptureStage::InterruptionState) ==
                       Ia64ExternalPreemptionFrameError::TokenMismatch,
                   BytesAreEqual(&frame, expected, sizeof(frame)));
    ASTERIA_EXPECT(
        runner, "capture stages cannot skip the interruption-state image",
        AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::IntegerState) ==
            Ia64ExternalPreemptionFrameError::InvalidCaptureStage,
        BytesAreEqual(&frame, expected, sizeof(frame)));

    ASTERIA_EXPECT(
        runner, "interruption coordinates form the first completed capture stage",
        AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::InterruptionState) ==
            Ia64ExternalPreemptionFrameError::None);
    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(
        runner, "a completed capture stage cannot be repeated",
        AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::InterruptionState) ==
            Ia64ExternalPreemptionFrameError::InvalidCaptureStage,
        BytesAreEqual(&frame, expected, sizeof(frame)));

    ASTERIA_EXPECT(runner, "timer acknowledgement cannot precede the full architectural image",
                   CompleteIa64ExternalPreemptionTimerAcknowledgement(frame, token, ValidEvidence()) ==
                       Ia64ExternalPreemptionFrameError::InvalidCaptureStage,
                   BytesAreEqual(&frame, expected, sizeof(frame)));
    ASTERIA_EXPECT(
        runner, "integer, application, floating, and RSE capture advance in order",
        AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::IntegerState) ==
            Ia64ExternalPreemptionFrameError::None,
        AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::ApplicationState) ==
            Ia64ExternalPreemptionFrameError::None,
        AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::FloatingState) ==
            Ia64ExternalPreemptionFrameError::None,
        AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::RseTransition) ==
            Ia64ExternalPreemptionFrameError::None);

    const auto invalidEvidenceIsAtomic =
        [&frame, &expected, token](const Ia64ExternalPreemptionTimerEvidence& evidence) noexcept
    {
        CopyBytes(&frame, expected, sizeof(frame));
        return (CompleteIa64ExternalPreemptionTimerAcknowledgement(frame, token, evidence) ==
                Ia64ExternalPreemptionFrameError::InvalidTimerEvidence) &&
               BytesAreEqual(&frame, expected, sizeof(frame));
    };

    auto evidence = ValidEvidence();
    evidence.ClaimedVector = 0xE9U;
    ASTERIA_EXPECT(runner, "a foreign claimed vector cannot complete hardware ownership",
                   invalidEvidenceIsAtomic(evidence));
    evidence = ValidEvidence();
    evidence.ObservedTicks = Deadline - 1U;
    ASTERIA_EXPECT(runner, "an early timer observation cannot publish a stopped run",
                   invalidEvidenceIsAtomic(evidence));
    evidence.ObservedTicks = MaximumUInt64;
    ASTERIA_EXPECT(runner, "the reserved maximum observation cannot publish a stopped run",
                   invalidEvidenceIsAtomic(evidence));
    evidence = ValidEvidence();
    evidence.MaskedTimerVector = Ia64ExternalInterruptTimerVector;
    ASTERIA_EXPECT(runner, "timer acknowledgement requires exact masked-register readback",
                   invalidEvidenceIsAtomic(evidence));
    evidence = ValidEvidence();
    evidence.EoiCount = 0U;
    ASTERIA_EXPECT(runner, "timer acknowledgement requires one EOI", invalidEvidenceIsAtomic(evidence));
    evidence.EoiCount = 2U;
    ASTERIA_EXPECT(runner, "timer acknowledgement rejects duplicate EOI evidence", invalidEvidenceIsAtomic(evidence));

    ASTERIA_EXPECT(runner, "exact mask, readback, and one EOI complete capture before publication",
                   CompleteIa64ExternalPreemptionTimerAcknowledgement(frame, token, ValidEvidence()) ==
                       Ia64ExternalPreemptionFrameError::None,
                   frame.CaptureStage == Ia64ExternalPreemptionCaptureStage::TimerAcknowledged,
                   frame.ClaimedVector == Ia64ExternalInterruptTimerVector, frame.ObservedTicks == ObservedTicks,
                   frame.MaskedTimerVector == Ia64ExternalInterruptMaskedTimerVector, frame.EoiCount == 1U,
                   AcquireIa64ExternalPreemptionToken(frame) == 0U);

    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "capture cannot advance after hardware acknowledgement",
                   AdvanceIa64ExternalPreemptionCapture(frame, token, Ia64ExternalPreemptionCaptureStage::None) ==
                       Ia64ExternalPreemptionFrameError::InvalidCaptureStage,
                   BytesAreEqual(&frame, expected, sizeof(frame)));
}

void TestPublicationCommitAndRelease(TestContext& runner) noexcept
{
    Ia64ExternalPreemptionFrame incomplete{};
    UInt64 incompleteToken{0U};
    static_cast<void>(BeginIa64ExternalPreemptionCapture(incomplete, ValidRequest(), incompleteToken));
    UInt8 expected[sizeof(incomplete)]{};
    CopyBytes(&incomplete, expected, sizeof(incomplete));
    ASTERIA_EXPECT(runner, "an incomplete architectural image cannot publish a usable token",
                   PublishIa64ExternalPreemptionCapture(incomplete, incompleteToken) ==
                       Ia64ExternalPreemptionFrameError::InvalidCaptureStage,
                   AcquireIa64ExternalPreemptionToken(incomplete) == 0U,
                   BytesAreEqual(&incomplete, expected, sizeof(incomplete)));
    ASTERIA_EXPECT(runner, "an unpublished capture cannot enter scheduler-committed state",
                   CommitIa64ExternalPreemptionResume(incomplete, incompleteToken) ==
                       Ia64ExternalPreemptionFrameError::TokenMismatch,
                   BytesAreEqual(&incomplete, expected, sizeof(incomplete)));

    Ia64ExternalPreemptionFrame corrupted{};
    UInt64 corruptedToken{0U};
    static_cast<void>(BeginIa64ExternalPreemptionCapture(corrupted, ValidRequest(), corruptedToken));
    static_cast<void>(AdvanceThroughRse(corrupted, corruptedToken));
    static_cast<void>(CompleteIa64ExternalPreemptionTimerAcknowledgement(corrupted, corruptedToken, ValidEvidence()));
    corrupted.EoiCount = 2U;
    CopyBytes(&corrupted, expected, sizeof(corrupted));
    ASTERIA_EXPECT(runner, "publication revalidates target-written timer acknowledgement fields",
                   PublishIa64ExternalPreemptionCapture(corrupted, corruptedToken) ==
                       Ia64ExternalPreemptionFrameError::InvalidTimerEvidence,
                   AcquireIa64ExternalPreemptionToken(corrupted) == 0U,
                   BytesAreEqual(&corrupted, expected, sizeof(corrupted)));

    Ia64ExternalPreemptionFrame frame{};
    UInt64 token{0U};
    ASTERIA_EXPECT(runner, "full capture release-publishes one exact retained generation", RetainFrame(frame, token),
                   token == 1U, frame.State == Ia64ExternalPreemptionFrameState::Retained,
                   AcquireIa64ExternalPreemptionToken(frame) == token);

    for (Size index{0U}; index < Platform::Ia64::Interrupt::Ia64ExternalPreemptionGeneralRegisterCount; ++index)
    {
        frame.GeneralRegisters[index] = 0x1000U + index;
    }
    frame.CompareAndStoreData = 0x1020'3040'5060'7080ULL;
    frame.FloatingRegisters[0].Low = 0x0123'4567'89AB'CDEFULL;
    frame.FloatingRegisters[125].High = 0xFEDC'BA98'7654'3210ULL;

    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "the restore tail cannot release a frame before scheduler commit",
                   ModelReleaseIa64ExternalPreemptionFrameForRfi(frame, token) ==
                       Ia64ExternalPreemptionFrameError::NotCommitted,
                   BytesAreEqual(&frame, expected, sizeof(frame)));
    ASTERIA_EXPECT(runner, "a stale scheduler token cannot commit retained architecture state",
                   CommitIa64ExternalPreemptionResume(frame, token + 1U) ==
                       Ia64ExternalPreemptionFrameError::TokenMismatch,
                   BytesAreEqual(&frame, expected, sizeof(frame)));
    ASTERIA_EXPECT(runner, "scheduler commit retains the frame until the no-return restore tail",
                   CommitIa64ExternalPreemptionResume(frame, token) == Ia64ExternalPreemptionFrameError::None,
                   frame.State == Ia64ExternalPreemptionFrameState::ResumeCommitted,
                   AcquireIa64ExternalPreemptionToken(frame) == token);

    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "scheduler commit cannot consume one retained frame twice",
                   CommitIa64ExternalPreemptionResume(frame, token) == Ia64ExternalPreemptionFrameError::NotRetained,
                   BytesAreEqual(&frame, expected, sizeof(frame)));

    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "a stale restore token cannot release persistent frame ownership",
                   ModelReleaseIa64ExternalPreemptionFrameForRfi(frame, token + 1U) ==
                       Ia64ExternalPreemptionFrameError::TokenMismatch,
                   BytesAreEqual(&frame, expected, sizeof(frame)));
    ASTERIA_EXPECT(runner, "release preserves loaded payload and advances to a nonreused token",
                   ModelReleaseIa64ExternalPreemptionFrameForRfi(frame, token) ==
                       Ia64ExternalPreemptionFrameError::None,
                   !Ia64ExternalPreemptionFrameIsOwned(frame), AcquireIa64ExternalPreemptionToken(frame) == 0U,
                   frame.NextToken == 2U, frame.GeneralRegisters[30] == 0x101EU,
                   frame.CompareAndStoreData == 0x1020'3040'5060'7080ULL,
                   frame.FloatingRegisters[0].Low == 0x0123'4567'89AB'CDEFULL,
                   frame.FloatingRegisters[125].High == 0xFEDC'BA98'7654'3210ULL);
    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "a completed restore token cannot replay its ownership clear",
                   ModelReleaseIa64ExternalPreemptionFrameForRfi(frame, token) ==
                       Ia64ExternalPreemptionFrameError::TokenMismatch,
                   BytesAreEqual(&frame, expected, sizeof(frame)));

    UInt64 secondToken{0U};
    ASTERIA_EXPECT(runner, "a reused frame publishes a strictly increasing lifetime token",
                   RetainFrame(frame, secondToken), secondToken == 2U,
                   AcquireIa64ExternalPreemptionToken(frame) == secondToken);
}

void TestMaximumTokenExhaustion(TestContext& runner) noexcept
{
    Ia64ExternalPreemptionFrame frame{};
    frame.NextToken = MaximumUInt64;
    UInt64 token{0U};
    ASTERIA_EXPECT(runner, "the final nonzero token is unique and marks future exhaustion", RetainFrame(frame, token),
                   token == MaximumUInt64, frame.NextToken == 0U);
    ASTERIA_EXPECT(runner, "the final retained generation can complete its exact restore lifecycle",
                   CommitIa64ExternalPreemptionResume(frame, token) == Ia64ExternalPreemptionFrameError::None,
                   ModelReleaseIa64ExternalPreemptionFrameForRfi(frame, token) ==
                       Ia64ExternalPreemptionFrameError::None);

    UInt8 expected[sizeof(frame)]{};
    CopyBytes(&frame, expected, sizeof(frame));
    UInt64 replay{17U};
    ASTERIA_EXPECT(runner, "exhaustion is fail-stop rather than wrapping to an ABA generation",
                   BeginIa64ExternalPreemptionCapture(frame, ValidRequest(), replay) ==
                       Ia64ExternalPreemptionFrameError::TokenExhausted,
                   replay == 17U, BytesAreEqual(&frame, expected, sizeof(frame)));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ExternalPreemptionFrameTests"};
    Asteria::Test::TestDefaultAndBeginFailureAtomicity(runner);
    Asteria::Test::TestOrderedCaptureAndTimerEvidence(runner);
    Asteria::Test::TestPublicationCommitAndRelease(runner);
    Asteria::Test::TestMaximumTokenExhaustion(runner);
    return runner.Finish();
}
