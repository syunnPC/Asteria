// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionFrame.hpp"

#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr bool IsValidCaptureRequest(const Ia64ExternalPreemptionCaptureRequest& request) noexcept
{
    return (request.ProcessorIndex < Ia64ExternalInterruptMaximumProcessorCount) &&
           (request.TimerVector == Ia64ExternalInterruptTimerVector) && (request.ProgrammedDeadlineTicks != 0U) &&
           (request.ProgrammedDeadlineTicks != MaximumUInt64) && (request.Reserved == 0U);
}

[[nodiscard]] constexpr bool IsValidTimerEvidence(const Ia64ExternalPreemptionFrame& frame,
                                                  const Ia64ExternalPreemptionTimerEvidence& evidence) noexcept
{
    return (evidence.ClaimedVector == frame.TimerVector) && (evidence.ObservedTicks >= frame.ProgrammedDeadlineTicks) &&
           (evidence.ObservedTicks != MaximumUInt64) &&
           (evidence.MaskedTimerVector == Ia64ExternalInterruptMaskedTimerVector) && (evidence.EoiCount == 1U);
}

[[nodiscard]] constexpr Ia64ExternalPreemptionCaptureStage
NextCaptureStage(const Ia64ExternalPreemptionCaptureStage stage) noexcept
{
    switch (stage)
    {
    case Ia64ExternalPreemptionCaptureStage::None:
        return Ia64ExternalPreemptionCaptureStage::InterruptionState;
    case Ia64ExternalPreemptionCaptureStage::InterruptionState:
        return Ia64ExternalPreemptionCaptureStage::IntegerState;
    case Ia64ExternalPreemptionCaptureStage::IntegerState:
        return Ia64ExternalPreemptionCaptureStage::ApplicationState;
    case Ia64ExternalPreemptionCaptureStage::ApplicationState:
        return Ia64ExternalPreemptionCaptureStage::FloatingState;
    case Ia64ExternalPreemptionCaptureStage::FloatingState:
        return Ia64ExternalPreemptionCaptureStage::RseTransition;
    case Ia64ExternalPreemptionCaptureStage::RseTransition:
    case Ia64ExternalPreemptionCaptureStage::TimerAcknowledged:
        return Ia64ExternalPreemptionCaptureStage::None;
    }
    return Ia64ExternalPreemptionCaptureStage::None;
}
} // namespace

Ia64ExternalPreemptionFrameError BeginIa64ExternalPreemptionCapture(Ia64ExternalPreemptionFrame& frame,
                                                                    const Ia64ExternalPreemptionCaptureRequest& request,
                                                                    Foundation::UInt64& captureToken) noexcept
{
    if (!IsValidCaptureRequest(request))
    {
        return Ia64ExternalPreemptionFrameError::InvalidRequest;
    }
    Synchronization::AtomicUInt64Reference publication{frame.PublishedToken};
    if ((publication.LoadAcquire() != 0U) || (frame.State != Ia64ExternalPreemptionFrameState::Inactive))
    {
        return Ia64ExternalPreemptionFrameError::AlreadyOwned;
    }
    if (frame.NextToken == 0U)
    {
        return Ia64ExternalPreemptionFrameError::TokenExhausted;
    }

    const Foundation::UInt64 selectedToken = frame.NextToken;
    frame.CaptureToken = selectedToken;
    frame.NextToken = selectedToken == MaximumUInt64 ? 0U : selectedToken + 1U;
    frame.State = Ia64ExternalPreemptionFrameState::Capturing;
    frame.CaptureStage = Ia64ExternalPreemptionCaptureStage::None;
    frame.ProcessorIndex = request.ProcessorIndex;
    frame.TimerVector = request.TimerVector;
    frame.ClaimedVector = 0U;
    frame.ProgrammedDeadlineTicks = request.ProgrammedDeadlineTicks;
    frame.ObservedTicks = 0U;
    frame.MaskedTimerVector = 0U;
    frame.EoiCount = 0U;
    captureToken = selectedToken;
    return Ia64ExternalPreemptionFrameError::None;
}

Ia64ExternalPreemptionFrameError
AdvanceIa64ExternalPreemptionCapture(Ia64ExternalPreemptionFrame& frame, const Foundation::UInt64 captureToken,
                                     const Ia64ExternalPreemptionCaptureStage completedStage) noexcept
{
    if (frame.State != Ia64ExternalPreemptionFrameState::Capturing)
    {
        return Ia64ExternalPreemptionFrameError::InvalidState;
    }
    if ((captureToken == 0U) || (captureToken != frame.CaptureToken))
    {
        return Ia64ExternalPreemptionFrameError::TokenMismatch;
    }
    const Ia64ExternalPreemptionCaptureStage nextStage = NextCaptureStage(frame.CaptureStage);
    if ((nextStage == Ia64ExternalPreemptionCaptureStage::None) || (completedStage != nextStage))
    {
        return Ia64ExternalPreemptionFrameError::InvalidCaptureStage;
    }

    frame.CaptureStage = completedStage;
    return Ia64ExternalPreemptionFrameError::None;
}

Ia64ExternalPreemptionFrameError
CompleteIa64ExternalPreemptionTimerAcknowledgement(Ia64ExternalPreemptionFrame& frame,
                                                   const Foundation::UInt64 captureToken,
                                                   const Ia64ExternalPreemptionTimerEvidence& evidence) noexcept
{
    if (frame.State != Ia64ExternalPreemptionFrameState::Capturing)
    {
        return Ia64ExternalPreemptionFrameError::InvalidState;
    }
    if ((captureToken == 0U) || (captureToken != frame.CaptureToken))
    {
        return Ia64ExternalPreemptionFrameError::TokenMismatch;
    }
    if (frame.CaptureStage != Ia64ExternalPreemptionCaptureStage::RseTransition)
    {
        return Ia64ExternalPreemptionFrameError::InvalidCaptureStage;
    }
    if (!IsValidTimerEvidence(frame, evidence))
    {
        return Ia64ExternalPreemptionFrameError::InvalidTimerEvidence;
    }

    frame.ClaimedVector = evidence.ClaimedVector;
    frame.ObservedTicks = evidence.ObservedTicks;
    frame.MaskedTimerVector = evidence.MaskedTimerVector;
    frame.EoiCount = evidence.EoiCount;
    frame.CaptureStage = Ia64ExternalPreemptionCaptureStage::TimerAcknowledged;
    return Ia64ExternalPreemptionFrameError::None;
}

Ia64ExternalPreemptionFrameError PublishIa64ExternalPreemptionCapture(Ia64ExternalPreemptionFrame& frame,
                                                                      const Foundation::UInt64 captureToken) noexcept
{
    if (frame.State != Ia64ExternalPreemptionFrameState::Capturing)
    {
        return Ia64ExternalPreemptionFrameError::InvalidState;
    }
    if ((captureToken == 0U) || (captureToken != frame.CaptureToken))
    {
        return Ia64ExternalPreemptionFrameError::TokenMismatch;
    }
    if (frame.CaptureStage != Ia64ExternalPreemptionCaptureStage::TimerAcknowledged)
    {
        return Ia64ExternalPreemptionFrameError::InvalidCaptureStage;
    }
    if (!IsValidCaptureRequest(Ia64ExternalPreemptionCaptureRequest{
            .ProcessorIndex = frame.ProcessorIndex,
            .TimerVector = frame.TimerVector,
            .ProgrammedDeadlineTicks = frame.ProgrammedDeadlineTicks,
            .Reserved = 0U,
        }) ||
        !IsValidTimerEvidence(frame, Ia64ExternalPreemptionTimerEvidence{
                                         .ClaimedVector = frame.ClaimedVector,
                                         .ObservedTicks = frame.ObservedTicks,
                                         .MaskedTimerVector = frame.MaskedTimerVector,
                                         .EoiCount = frame.EoiCount,
                                     }))
    {
        return Ia64ExternalPreemptionFrameError::InvalidTimerEvidence;
    }

    frame.State = Ia64ExternalPreemptionFrameState::Retained;
    Synchronization::AtomicUInt64Reference publication{frame.PublishedToken};
    publication.StoreRelease(captureToken);
    return Ia64ExternalPreemptionFrameError::None;
}

Foundation::UInt64 AcquireIa64ExternalPreemptionToken(const Ia64ExternalPreemptionFrame& frame) noexcept
{
    return Synchronization::AtomicUInt64Reference::LoadAcquire(frame.PublishedToken);
}

Ia64ExternalPreemptionFrameError CommitIa64ExternalPreemptionResume(Ia64ExternalPreemptionFrame& frame,
                                                                    const Foundation::UInt64 publishedToken) noexcept
{
    if ((publishedToken == 0U) || (AcquireIa64ExternalPreemptionToken(frame) != publishedToken) ||
        (frame.CaptureToken != publishedToken))
    {
        return Ia64ExternalPreemptionFrameError::TokenMismatch;
    }
    if (frame.State != Ia64ExternalPreemptionFrameState::Retained)
    {
        return Ia64ExternalPreemptionFrameError::NotRetained;
    }

    frame.State = Ia64ExternalPreemptionFrameState::ResumeCommitted;
    return Ia64ExternalPreemptionFrameError::None;
}

Ia64ExternalPreemptionFrameError
ModelReleaseIa64ExternalPreemptionFrameForRfi(Ia64ExternalPreemptionFrame& frame,
                                              const Foundation::UInt64 publishedToken) noexcept
{
    if ((publishedToken == 0U) || (AcquireIa64ExternalPreemptionToken(frame) != publishedToken) ||
        (frame.CaptureToken != publishedToken))
    {
        return Ia64ExternalPreemptionFrameError::TokenMismatch;
    }
    if (frame.State != Ia64ExternalPreemptionFrameState::ResumeCommitted)
    {
        return Ia64ExternalPreemptionFrameError::NotCommitted;
    }

    frame.CaptureStage = Ia64ExternalPreemptionCaptureStage::None;
    frame.State = Ia64ExternalPreemptionFrameState::Inactive;
    Synchronization::AtomicUInt64Reference publication{frame.PublishedToken};
    publication.StoreRelease(0U);
    return Ia64ExternalPreemptionFrameError::None;
}
} // namespace Asteria::Platform::Ia64::Interrupt
