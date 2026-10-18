// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Ipc
{
WaitSetThreadDispatcherFailure WaitSetThreadDispatcher::MakeFailure(
    const WaitSetThreadDispatcherError error, const WaitSetFailure& waitSet, const Scheduler::ReadyQueueFailure& queue,
    const Object::ObjectPinError waitSetPin, const Object::ObjectPinError threadPin,
    const Object::ObjectPinError schedulingContextPin, const Scheduler::ThreadError thread,
    const Scheduler::ReadyQueueError queueTransition) noexcept
{
    return WaitSetThreadDispatcherFailure{
        .WaitSet = waitSet,
        .Queue = queue,
        .WaitSetPin = waitSetPin,
        .ThreadPin = threadPin,
        .SchedulingContextPin = schedulingContextPin,
        .Thread = thread,
        .QueueTransition = queueTransition,
        .Error = error,
    };
}

bool WaitSetThreadDispatcher::ContextCanResume(const Scheduler::SchedulingContextState state) noexcept
{
    switch (state)
    {
    case Scheduler::SchedulingContextState::Bound:
    case Scheduler::SchedulingContextState::Donated:
    case Scheduler::SchedulingContextState::BudgetExhausted:
        return true;

    case Scheduler::SchedulingContextState::Uninitialized:
    case Scheduler::SchedulingContextState::Unbound:
    case Scheduler::SchedulingContextState::Running:
        return false;
    }
    return false;
}

bool WaitSetThreadDispatcher::QueueFailureIsRetryable(const Scheduler::ReadyQueueFailure& failure) noexcept
{
    if (failure.Error == Scheduler::ReadyQueueError::QueueFull)
    {
        return true;
    }
    if ((failure.Error == Scheduler::ReadyQueueError::ContextPinRejected) &&
        (failure.ContextPin == Object::ObjectPinError::ReferenceExhausted))
    {
        return true;
    }
    return (failure.Error == Scheduler::ReadyQueueError::ThreadPinRejected) &&
           (failure.ThreadPin == Object::ObjectPinError::ReferenceExhausted);
}

bool WaitSetThreadDispatcher::WaitSetClaimIsDeferred(const WaitSetError error) noexcept
{
    return (error == WaitSetError::Empty) || (error == WaitSetError::WouldBlock) ||
           (error == WaitSetError::ClaimActive);
}

bool WaitSetThreadDispatcher::CaptureRunningCorrelationLocked() noexcept
{
    if ((m_waitSet == nullptr) || (m_thread == nullptr) || (m_context == nullptr) || !m_waitSetPin.IsValid() ||
        !m_threadPin.IsValid() || !m_contextPin.IsValid() || !m_waitSetIdentity.IsValid() ||
        !m_threadIdentity.IsValid() || !m_contextIdentity.IsValid())
    {
        return false;
    }

    const WaitSetSnapshot waitSet = m_waitSet->GetSnapshot();
    const Scheduler::ThreadSnapshot thread = m_thread->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    const Scheduler::CpuId wakeProcessor = m_wakeFacade.GetHomeProcessor(*m_context);
    const bool correlated = ((waitSet.State == WaitSetState::Ready) || (waitSet.State == WaitSetState::Claimed)) &&
                            (thread.State == Scheduler::ThreadState::Running) && thread.RunningProcessor.IsValid() &&
                            !thread.BlockToken.IsValid() && (thread.RunSequence != 0U) && (thread.ExitCode == 0U) &&
                            (context.State == Scheduler::SchedulingContextState::Running) &&
                            context.RunningProcessor.IsValid() &&
                            (context.RunningProcessor.GetValue() == thread.RunningProcessor.GetValue()) &&
                            context.CurrentThread.IsEqualTo(m_threadIdentity) && wakeProcessor.IsValid() &&
                            (wakeProcessor.GetValue() == thread.RunningProcessor.GetValue());
    if (correlated)
    {
        m_runSequence = thread.RunSequence;
    }
    return correlated;
}

bool WaitSetThreadDispatcher::BlockedStateIsCorrelatedLocked() const noexcept
{
    if ((m_waitSet == nullptr) || (m_thread == nullptr) || (m_context == nullptr) || !m_waitSetPin.IsValid() ||
        !m_threadPin.IsValid() || !m_contextPin.IsValid() || !m_blockToken.IsValid())
    {
        return false;
    }

    const Scheduler::ThreadSnapshot thread = m_thread->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    return (thread.State == Scheduler::ThreadState::Blocked) && thread.BlockToken.IsEqualTo(m_blockToken) &&
           (thread.RunSequence == m_runSequence) && !thread.RunningProcessor.IsValid() && (thread.ExitCode == 0U) &&
           ContextCanResume(context.State) && !context.RunningProcessor.IsValid() &&
           context.CurrentThread.IsEqualTo(m_threadIdentity);
}

bool WaitSetThreadDispatcher::CandidateBlockIsCorrelatedLocked(const Scheduler::ThreadBlockToken blockToken,
                                                               bool& tokenMismatch) const noexcept
{
    tokenMismatch = false;
    if ((m_thread == nullptr) || (m_context == nullptr) || !m_threadPin.IsValid() || !m_contextPin.IsValid())
    {
        return false;
    }

    const Scheduler::ThreadSnapshot thread = m_thread->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    if (!blockToken.IsValid() || (thread.RunSequence != m_runSequence) ||
        ((thread.State == Scheduler::ThreadState::Blocked) && thread.BlockToken.IsValid() &&
         !thread.BlockToken.IsEqualTo(blockToken)))
    {
        tokenMismatch = true;
        return false;
    }
    return (thread.State == Scheduler::ThreadState::Blocked) && thread.BlockToken.IsEqualTo(blockToken) &&
           !thread.RunningProcessor.IsValid() && (thread.ExitCode == 0U) && ContextCanResume(context.State) &&
           !context.RunningProcessor.IsValid() && context.CurrentThread.IsEqualTo(m_threadIdentity);
}

WaitSetThreadDispatcher::OperationResult
WaitSetThreadDispatcher::Prepare(const WaitSetThreadPrepareRequest& request) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    WaitSetThreadDispatcherFailure failure{};
    if (!ValidatePrepareRequestLocked(request, failure) || !PinPreparationLocked(request, failure))
    {
        return OperationResult::Failure(failure);
    }
    const WaitSetThreadDispatcherGenerationCounter previousGenerations = m_transactionGenerations;
    if (!AdvanceTransactionGenerationLocked())
    {
        const WaitSetThreadDispatcherFailure exhausted =
            MakeFailure(WaitSetThreadDispatcherError::TransactionGenerationExhausted);
        ResetPreparationLocked();
        m_diagnostic = exhausted;
        return OperationResult::Failure(m_diagnostic);
    }

    const OperationResult result = ClaimPreparedLocked();
    if (!result.HasValue() && (m_state == WaitSetThreadDispatcherState::Idle))
    {
        m_transactionGenerations = previousGenerations;
    }
    return result;
}

WaitSetThreadDispatcherFailure
WaitSetThreadDispatcher::RejectReceiptInvariant(const WaitSetThreadDispatcherReceipt& reported) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetThreadDispatcherState::Faulted)
    {
        return m_diagnostic;
    }

    const WaitSetThreadDispatcherReceipt current = MakeReceiptLocked();
    const WaitSetThreadDispatcherFailure failure = MakeFailure(WaitSetThreadDispatcherError::ReceiptInvariantRejected);
    if ((m_state == WaitSetThreadDispatcherState::Prepared) && ReceiptsAreEqual(reported, current))
    {
        ResetPreparationLocked();
        m_diagnostic = failure;
        return m_diagnostic;
    }

    EnterFaultedLocked(failure);
    return m_diagnostic;
}

bool WaitSetThreadDispatcher::ValidatePrepareRequestLocked(const WaitSetThreadPrepareRequest& request,
                                                           WaitSetThreadDispatcherFailure& failure) noexcept
{
    if (m_state == WaitSetThreadDispatcherState::Faulted)
    {
        failure = m_diagnostic;
        return false;
    }
    if (m_state == WaitSetThreadDispatcherState::Completed)
    {
        failure = MakeFailure(WaitSetThreadDispatcherError::AlreadyCompleted);
        return false;
    }
    if (m_state != WaitSetThreadDispatcherState::Idle)
    {
        failure = MakeFailure(WaitSetThreadDispatcherError::AlreadyPrepared);
        return false;
    }
    if (!m_wakeFacade.GetProcessor().IsValid())
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::InvalidConfiguration);
        failure = m_diagnostic;
        return false;
    }
    if (!request.WaitSet.IsValid())
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::InvalidWaitSet);
        failure = m_diagnostic;
        return false;
    }
    if (!request.WaitSet.IsFromRegistry(m_objectRegistry))
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::WaitSetRegistryMismatch);
        failure = m_diagnostic;
        return false;
    }
    if (!request.Thread.IsValid())
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::InvalidThread);
        failure = m_diagnostic;
        return false;
    }
    if (!request.Thread.IsFromRegistry(m_objectRegistry))
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::ThreadRegistryMismatch);
        failure = m_diagnostic;
        return false;
    }
    if (!request.SchedulingContext.IsValid())
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::InvalidSchedulingContext);
        failure = m_diagnostic;
        return false;
    }
    if (!request.SchedulingContext.IsFromRegistry(m_objectRegistry))
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::SchedulingContextRegistryMismatch);
        failure = m_diagnostic;
        return false;
    }
    if (!m_transactionGenerations.CanIssue())
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::TransactionGenerationExhausted);
        failure = m_diagnostic;
        return false;
    }
    return true;
}

bool WaitSetThreadDispatcher::AdvanceTransactionGenerationLocked() noexcept
{
    return m_transactionGenerations.TryIssue(m_transactionGeneration);
}

bool WaitSetThreadDispatcher::PinPreparationLocked(const WaitSetThreadPrepareRequest& request,
                                                   WaitSetThreadDispatcherFailure& failure) noexcept
{
    if (m_waitSetPin.IsValid() || m_threadPin.IsValid() || m_contextPin.IsValid())
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::Faulted));
        failure = m_diagnostic;
        return false;
    }

    Object::ObjectPinError pin =
        m_objectRegistry.PinObject(request.WaitSet.GetObjectId(), Object::ObjectType::WaitSet, m_waitSetPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::WaitSetPinRejected, {}, {}, pin);
        failure = m_diagnostic;
        return false;
    }
    pin = m_objectRegistry.PinObject(request.Thread.GetObjectId(), Object::ObjectType::Thread, m_threadPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        m_waitSetPin.Reset();
        m_diagnostic =
            MakeFailure(WaitSetThreadDispatcherError::ThreadPinRejected, {}, {}, Object::ObjectPinError::Ok, pin);
        failure = m_diagnostic;
        return false;
    }
    pin = m_objectRegistry.PinObject(request.SchedulingContext.GetObjectId(), Object::ObjectType::SchedulingContext,
                                     m_contextPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        m_threadPin.Reset();
        m_waitSetPin.Reset();
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::SchedulingContextPinRejected, {}, {},
                                   Object::ObjectPinError::Ok, Object::ObjectPinError::Ok, pin);
        failure = m_diagnostic;
        return false;
    }

    m_waitSet = static_cast<WaitSet*>(&m_waitSetPin.GetObject());
    m_thread = static_cast<Scheduler::Thread*>(&m_threadPin.GetObject());
    m_context = static_cast<Scheduler::SchedulingContext*>(&m_contextPin.GetObject());
    m_waitSetIdentity = request.WaitSet;
    m_threadIdentity = request.Thread;
    m_contextIdentity = request.SchedulingContext;
    m_deadline = request.DeadlineTicks;
    m_timedOut = false;
    m_aborted = false;
    m_state = WaitSetThreadDispatcherState::Prepared;
    ClearFailureLocked();
    if (!CaptureRunningCorrelationLocked())
    {
        const WaitSetThreadDispatcherFailure diagnostic =
            MakeFailure(WaitSetThreadDispatcherError::CorrelatedStateChanged);
        ResetPreparationLocked();
        m_diagnostic = diagnostic;
        failure = diagnostic;
        return false;
    }
    return true;
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::ClaimPreparedLocked() noexcept
{
    const auto claimed = m_waitSet->ClaimReady();
    if (!claimed.HasValue())
    {
        if (WaitSetClaimIsDeferred(claimed.GetError().Error))
        {
            return OperationResult::Success(MakeReceiptLocked());
        }
        const WaitSetThreadDispatcherFailure failure =
            MakeFailure(WaitSetThreadDispatcherError::WaitSetClaimRejected, claimed.GetError());
        ResetPreparationLocked();
        m_diagnostic = failure;
        return OperationResult::Failure(m_diagnostic);
    }
    return CompleteImmediateClaimLocked(claimed.GetValue());
}

WaitSetThreadDispatcher::OperationResult
WaitSetThreadDispatcher::CompleteImmediateClaimLocked(const WaitSetNotificationClaim& claim) noexcept
{
    StoreClaimLocked(claim);
    const auto committed = m_waitSet->CommitClaim(m_claimToken);
    if (!committed.HasValue())
    {
        WaitSetThreadDispatcherFailure failure =
            MakeFailure(WaitSetThreadDispatcherError::WaitSetClaimCommitRejected, committed.GetError());
        if (!CancelClaimLocked(m_claimToken, failure))
        {
            EnterFaultedLocked(failure);
            return OperationResult::Failure(m_diagnostic);
        }
        ResetPreparationLocked();
        m_diagnostic = failure;
        return OperationResult::Failure(m_diagnostic);
    }

    m_claimToken = WaitSetClaimToken{};
    m_state = WaitSetThreadDispatcherState::Completed;
    ClearFailureLocked();
    const WaitSetThreadDispatcherReceipt receipt = MakeReceiptLocked();
    ReleaseTerminalPinsLocked();
    return OperationResult::Success(receipt);
}

WaitSetThreadDispatcher::OperationResult
WaitSetThreadDispatcher::CommitBlocked(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return CommitBlockedLocked(blockToken);
}

WaitSetThreadDispatcher::OperationResult
WaitSetThreadDispatcher::CommitBlockedLocked(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    if (m_state == WaitSetThreadDispatcherState::Faulted)
    {
        return OperationResult::Failure(m_diagnostic);
    }
    if (m_state == WaitSetThreadDispatcherState::Completed)
    {
        return OperationResult::Failure(MakeFailure(WaitSetThreadDispatcherError::AlreadyCompleted));
    }
    if (m_state != WaitSetThreadDispatcherState::Prepared)
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::InvalidState);
        return OperationResult::Failure(m_diagnostic);
    }

    bool tokenMismatch{false};
    if (!CandidateBlockIsCorrelatedLocked(blockToken, tokenMismatch))
    {
        m_diagnostic = MakeFailure(tokenMismatch ? WaitSetThreadDispatcherError::InvalidBlockToken
                                                 : WaitSetThreadDispatcherError::CorrelatedStateChanged);
        return OperationResult::Failure(m_diagnostic);
    }

    m_blockToken = blockToken;
    m_blockSequence = blockToken.GetValue();
    m_state = WaitSetThreadDispatcherState::Blocked;
    ClearFailureLocked();
    return ProgressLocked();
}

Scheduler::BlockedRunCommitOutcome
WaitSetThreadDispatcher::CommitBlockedFromFinishedRun(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const WaitSetThreadDispatcherState startingState = m_state;
    const OperationResult result = CommitBlockedLocked(blockToken);
    if (result.HasValue())
    {
        return Scheduler::BlockedRunCommitOutcome::Accepted;
    }
    if ((startingState == WaitSetThreadDispatcherState::Prepared) &&
        (m_state == WaitSetThreadDispatcherState::QueuePressure))
    {
        return Scheduler::BlockedRunCommitOutcome::RetryableRejected;
    }
    if (m_state != WaitSetThreadDispatcherState::Faulted)
    {
        EnterFaultedLocked(result.GetErrorReference());
    }
    return Scheduler::BlockedRunCommitOutcome::TerminalRejected;
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::ConsumeCompletion() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ConsumeCompletionLocked(nullptr);
}

WaitSetThreadDispatcher::OperationResult
WaitSetThreadDispatcher::ConsumeCompletion(const WaitSetThreadDispatcherReceipt& expected) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ConsumeCompletionLocked(&expected);
}

WaitSetThreadDispatcher::OperationResult
WaitSetThreadDispatcher::ConsumeCompletionLocked(const WaitSetThreadDispatcherReceipt* const expected) noexcept
{
    if (m_state == WaitSetThreadDispatcherState::Faulted)
    {
        return OperationResult::Failure(m_diagnostic);
    }
    if (m_state != WaitSetThreadDispatcherState::Completed)
    {
        return OperationResult::Failure(MakeFailure(WaitSetThreadDispatcherError::CompletionUnavailable));
    }

    const WaitSetThreadDispatcherReceipt receipt = MakeReceiptLocked();
    if ((expected != nullptr) && !ReceiptsAreEqual(*expected, receipt))
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::CompletionReceiptMismatch));
        return OperationResult::Failure(m_diagnostic);
    }
    ResetPreparationLocked();
    ClearFailureLocked();
    return OperationResult::Success(receipt);
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::Poll() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetThreadDispatcherState::Faulted)
    {
        return OperationResult::Failure(m_diagnostic);
    }
    if (m_state == WaitSetThreadDispatcherState::Completed)
    {
        return OperationResult::Failure(MakeFailure(WaitSetThreadDispatcherError::AlreadyCompleted));
    }
    if ((m_state != WaitSetThreadDispatcherState::Blocked) && (m_state != WaitSetThreadDispatcherState::QueuePressure))
    {
        m_diagnostic = MakeFailure(WaitSetThreadDispatcherError::InvalidState);
        return OperationResult::Failure(m_diagnostic);
    }
    return ProgressLocked();
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::Expire(const Foundation::UInt64 now) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetThreadDispatcherState::Faulted)
    {
        return OperationResult::Failure(m_diagnostic);
    }
    if ((m_state != WaitSetThreadDispatcherState::Blocked) || (m_deadline == 0U) || (now < m_deadline))
    {
        return OperationResult::Success(MakeReceiptLocked());
    }
    if (!BlockedStateIsCorrelatedLocked())
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::CorrelatedStateChanged));
        return OperationResult::Failure(m_diagnostic);
    }
    return ReserveExpiryLocked();
}

// A Blocked transaction retains no claim, so the expiry's wake commits none.
WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::ReserveExpiryLocked() noexcept
{
    const auto reserved =
        m_wakeFacade.ReserveBlockedEnqueue(m_contextIdentity, *m_context, m_threadIdentity, *m_thread, m_blockToken);
    if (!reserved.HasValue())
    {
        const WaitSetThreadDispatcherFailure failure =
            MakeFailure(WaitSetThreadDispatcherError::QueueReservationRejected, {}, reserved.GetError());
        if (QueueFailureIsRetryable(reserved.GetError()))
        {
            return OperationResult::Failure(failure);
        }
        EnterFaultedLocked(failure);
        return OperationResult::Failure(m_diagnostic);
    }
    m_queueReservation = reserved.GetValue();
    return WakeExpiredLocked();
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::WakeExpiredLocked() noexcept
{
    const Scheduler::ThreadError wake = m_thread->Wake(m_blockToken);
    if (wake != Scheduler::ThreadError::None)
    {
        WaitSetThreadDispatcherFailure failure =
            MakeFailure(WaitSetThreadDispatcherError::WakeRejected, {}, {}, Object::ObjectPinError::Ok,
                        Object::ObjectPinError::Ok, Object::ObjectPinError::Ok, wake);
        const Scheduler::ReadyQueueError rollback = m_wakeFacade.CancelReservedEnqueue(m_queueReservation);
        if (rollback == Scheduler::ReadyQueueError::None)
        {
            m_queueReservation = Scheduler::ReadyQueueEnqueueReservation{};
        }
        else
        {
            failure.QueueTransition = rollback;
            failure.Error = WaitSetThreadDispatcherError::QueueReservationRollbackRejected;
        }
        EnterFaultedLocked(failure);
        return OperationResult::Failure(m_diagnostic);
    }
    return PublishExpiredLocked();
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::PublishExpiredLocked() noexcept
{
    const auto queued = m_wakeFacade.CommitReservedEnqueue(m_queueReservation);
    if (!queued.HasValue())
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::QueueCommitRejected, {}, queued.GetError()));
        return OperationResult::Failure(m_diagnostic);
    }
    m_queueReservation = Scheduler::ReadyQueueEnqueueReservation{};
    m_queueSequence = queued.GetValue().Sequence;
    ClearClaimLocked();
    m_timedOut = true;
    m_state = WaitSetThreadDispatcherState::Completed;
    ClearFailureLocked();
    const WaitSetThreadDispatcherReceipt receipt = MakeReceiptLocked();
    ReleaseTerminalPinsLocked();
    return OperationResult::Success(receipt);
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::Abort() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetThreadDispatcherState::Faulted)
    {
        return OperationResult::Failure(m_diagnostic);
    }
    if (m_state != WaitSetThreadDispatcherState::Blocked)
    {
        return OperationResult::Success(MakeReceiptLocked());
    }
    if (!BlockedStateIsCorrelatedLocked())
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::CorrelatedStateChanged));
        return OperationResult::Failure(m_diagnostic);
    }
    m_aborted = true;
    const OperationResult aborted = ReserveExpiryLocked();
    // Queue pressure leaves the transaction Blocked and not aborted.
    if (m_state == WaitSetThreadDispatcherState::Blocked)
    {
        m_aborted = false;
    }
    return aborted;
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::Reset() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetThreadDispatcherState::Idle)
    {
        return OperationResult::Success(MakeReceiptLocked());
    }
    if (m_state != WaitSetThreadDispatcherState::Completed)
    {
        return OperationResult::Failure(MakeFailure(WaitSetThreadDispatcherError::InvalidState));
    }
    return ConsumeCompletionLocked(nullptr);
}

Foundation::UInt64 WaitSetThreadDispatcher::GetDeadline() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_deadline;
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::ProgressLocked() noexcept
{
    if (!BlockedStateIsCorrelatedLocked())
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::CorrelatedStateChanged));
        return OperationResult::Failure(m_diagnostic);
    }

    const auto claimed = m_waitSet->ClaimReady();
    if (!claimed.HasValue())
    {
        if (WaitSetClaimIsDeferred(claimed.GetError().Error))
        {
            m_state = WaitSetThreadDispatcherState::Blocked;
            ClearFailureLocked();
            return OperationResult::Success(MakeReceiptLocked());
        }
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::WaitSetClaimRejected, claimed.GetError()));
        return OperationResult::Failure(m_diagnostic);
    }
    StoreClaimLocked(claimed.GetValue());
    return ReserveClaimedWakeLocked();
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::ReserveClaimedWakeLocked() noexcept
{
    const auto reserved =
        m_wakeFacade.ReserveBlockedEnqueue(m_contextIdentity, *m_context, m_threadIdentity, *m_thread, m_blockToken);
    if (!reserved.HasValue())
    {
        WaitSetThreadDispatcherFailure failure =
            MakeFailure(WaitSetThreadDispatcherError::QueueReservationRejected, {}, reserved.GetError());
        const bool retryable = QueueFailureIsRetryable(reserved.GetError());
        if (!CancelClaimLocked(m_claimToken, failure))
        {
            EnterFaultedLocked(failure);
            return OperationResult::Failure(m_diagnostic);
        }
        if (retryable)
        {
            m_state = WaitSetThreadDispatcherState::QueuePressure;
            m_diagnostic = failure;
            return OperationResult::Failure(m_diagnostic);
        }
        EnterFaultedLocked(failure);
        return OperationResult::Failure(m_diagnostic);
    }
    m_queueReservation = reserved.GetValue();
    return WakeReservedLocked();
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::WakeReservedLocked() noexcept
{
    const Scheduler::ThreadError wake = m_thread->Wake(m_blockToken);
    if (wake != Scheduler::ThreadError::None)
    {
        WaitSetThreadDispatcherFailure failure =
            MakeFailure(WaitSetThreadDispatcherError::WakeRejected, {}, {}, Object::ObjectPinError::Ok,
                        Object::ObjectPinError::Ok, Object::ObjectPinError::Ok, wake);
        static_cast<void>(CancelPreWakeLocked(m_queueReservation, m_claimToken, failure));
        EnterFaultedLocked(failure);
        return OperationResult::Failure(m_diagnostic);
    }
    return PublishReservedLocked();
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::PublishReservedLocked() noexcept
{
    const auto queued = m_wakeFacade.CommitReservedEnqueue(m_queueReservation);
    if (!queued.HasValue())
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::QueueCommitRejected, {}, queued.GetError()));
        return OperationResult::Failure(m_diagnostic);
    }
    m_queueReservation = Scheduler::ReadyQueueEnqueueReservation{};
    m_queueSequence = queued.GetValue().Sequence;
    return CommitWakeClaimLocked();
}

WaitSetThreadDispatcher::OperationResult WaitSetThreadDispatcher::CommitWakeClaimLocked() noexcept
{
    const auto committed = m_waitSet->CommitClaim(m_claimToken);
    if (!committed.HasValue())
    {
        EnterFaultedLocked(MakeFailure(WaitSetThreadDispatcherError::WaitSetClaimCommitRejected, committed.GetError()));
        return OperationResult::Failure(m_diagnostic);
    }

    m_claimToken = WaitSetClaimToken{};
    m_state = WaitSetThreadDispatcherState::Completed;
    ClearFailureLocked();
    const WaitSetThreadDispatcherReceipt receipt = MakeReceiptLocked();
    ReleaseTerminalPinsLocked();
    return OperationResult::Success(receipt);
}

bool WaitSetThreadDispatcher::CancelClaimLocked(const WaitSetClaimToken claim,
                                                WaitSetThreadDispatcherFailure& failure) noexcept
{
    const auto cancelled = m_waitSet->CancelClaim(claim);
    if (!cancelled.HasValue())
    {
        failure.WaitSet = cancelled.GetError();
        failure.Error = WaitSetThreadDispatcherError::WaitSetClaimCancellationRejected;
        return false;
    }
    ClearClaimLocked();
    return true;
}

bool WaitSetThreadDispatcher::CancelPreWakeLocked(const Scheduler::ReadyQueueEnqueueReservation reservation,
                                                  const WaitSetClaimToken claim,
                                                  WaitSetThreadDispatcherFailure& failure) noexcept
{
    bool restored{true};
    const Scheduler::ReadyQueueError queueRollback = m_wakeFacade.CancelReservedEnqueue(reservation);
    if (queueRollback != Scheduler::ReadyQueueError::None)
    {
        failure.QueueTransition = queueRollback;
        failure.Error = WaitSetThreadDispatcherError::QueueReservationRollbackRejected;
        restored = false;
    }
    else
    {
        m_queueReservation = Scheduler::ReadyQueueEnqueueReservation{};
    }
    if (!CancelClaimLocked(claim, failure))
    {
        restored = false;
    }
    return restored;
}

WaitSetThreadDispatcherReceipt WaitSetThreadDispatcher::MakeReceiptLocked() const noexcept
{
    return WaitSetThreadDispatcherReceipt{
        .Dispatcher = this,
        .WaitSet = m_waitSetIdentity,
        .Thread = m_threadIdentity,
        .SchedulingContext = m_contextIdentity,
        .Notification = m_notification,
        .RegistrationSequence = m_registrationSequence,
        .RunSequence = m_runSequence,
        .BlockSequence = m_blockSequence,
        .TransactionGeneration = m_transactionGeneration,
        .Tag = m_tag,
        .QueueSequence = m_queueSequence,
        .State = m_state,
        .TimedOut = m_timedOut,
        .Aborted = m_aborted,
        .Reserved = {},
    };
}

bool WaitSetThreadDispatcher::ReceiptsAreEqual(const WaitSetThreadDispatcherReceipt& left,
                                               const WaitSetThreadDispatcherReceipt& right) noexcept
{
    bool reservedMatches{true};
    for (Foundation::Size index{0U}; index < sizeof(left.Reserved); ++index)
    {
        reservedMatches = reservedMatches && (left.Reserved[index] == right.Reserved[index]);
    }

    return reservedMatches && (left.Dispatcher == right.Dispatcher) && left.WaitSet.IsEqualTo(right.WaitSet) &&
           left.Thread.IsEqualTo(right.Thread) && left.SchedulingContext.IsEqualTo(right.SchedulingContext) &&
           left.Notification.IsEqualTo(right.Notification) &&
           (left.RegistrationSequence == right.RegistrationSequence) && (left.RunSequence == right.RunSequence) &&
           (left.BlockSequence == right.BlockSequence) && (left.TransactionGeneration == right.TransactionGeneration) &&
           (left.Tag == right.Tag) && (left.QueueSequence == right.QueueSequence) && (left.State == right.State) &&
           (left.TimedOut == right.TimedOut) && (left.Aborted == right.Aborted);
}

WaitSetThreadDispatcherSnapshot WaitSetThreadDispatcher::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return WaitSetThreadDispatcherSnapshot{
        .Dispatcher = this,
        .WaitSet = m_waitSetIdentity,
        .Thread = m_threadIdentity,
        .SchedulingContext = m_contextIdentity,
        .Notification = m_notification,
        .RegistrationSequence = m_registrationSequence,
        .RunSequence = m_runSequence,
        .BlockSequence = m_blockSequence,
        .TransactionGeneration = m_transactionGeneration,
        .RetainedClaimSequence = m_claimToken.GetSequence(),
        .RetainedQueueReservationSequence = m_queueReservation.GetSequence(),
        .Tag = m_tag,
        .QueueSequence = m_queueSequence,
        .Diagnostic = m_diagnostic,
        .State = m_state,
        .TimedOut = m_timedOut,
        .Aborted = m_aborted,
        .Reserved = {},
    };
}

void WaitSetThreadDispatcher::StoreClaimLocked(const WaitSetNotificationClaim& claim) noexcept
{
    m_claimToken = claim.Claim;
    m_registrationSequence = claim.Registration.GetSequence();
    m_notification = claim.Notification;
    m_tag = claim.Tag;
}

void WaitSetThreadDispatcher::ClearClaimLocked() noexcept
{
    m_claimToken = WaitSetClaimToken{};
    m_registrationSequence = 0U;
    m_notification = NotificationId{};
    m_tag = 0U;
}

void WaitSetThreadDispatcher::ClearFailureLocked() noexcept
{
    m_diagnostic = WaitSetThreadDispatcherFailure{};
}

void WaitSetThreadDispatcher::ResetPreparationLocked() noexcept
{
    m_waitSet = nullptr;
    m_thread = nullptr;
    m_context = nullptr;
    m_waitSetIdentity = WaitSetId{};
    m_threadIdentity = Scheduler::ThreadId{};
    m_contextIdentity = Scheduler::SchedulingContextId{};
    m_blockToken = Scheduler::ThreadBlockToken{};
    m_runSequence = 0U;
    m_blockSequence = 0U;
    m_transactionGeneration = 0U;
    ClearClaimLocked();
    m_queueReservation = Scheduler::ReadyQueueEnqueueReservation{};
    m_queueSequence = 0U;
    m_deadline = 0U;
    m_timedOut = false;
    m_aborted = false;
    m_contextPin.Reset();
    m_threadPin.Reset();
    m_waitSetPin.Reset();
    m_state = WaitSetThreadDispatcherState::Idle;
}

void WaitSetThreadDispatcher::ReleaseTerminalPinsLocked() noexcept
{
    m_blockToken = Scheduler::ThreadBlockToken{};
    m_claimToken = WaitSetClaimToken{};
    m_queueReservation = Scheduler::ReadyQueueEnqueueReservation{};
    m_waitSet = nullptr;
    m_thread = nullptr;
    m_context = nullptr;
    m_contextPin.Reset();
    m_threadPin.Reset();
    m_waitSetPin.Reset();
}

void WaitSetThreadDispatcher::EnterFaultedLocked(const WaitSetThreadDispatcherFailure& failure) noexcept
{
    m_diagnostic = failure;
    m_state = WaitSetThreadDispatcherState::Faulted;
    if (!m_claimToken.IsValid() && !m_queueReservation.IsValid())
    {
        ReleaseTerminalPinsLocked();
    }
}
} // namespace Asteria::Kernel::Ipc
