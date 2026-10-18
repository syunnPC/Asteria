// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Scheduler/ProcessorScheduler.hpp"

namespace Asteria::Kernel::Scheduler
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr bool IsDeferredAdmission(const DispatchCoordinatorFailure& failure) noexcept
{
    return (failure.Error == DispatchCoordinatorError::SchedulingContextRejected) &&
           ((failure.Context == SchedulingContextError::BudgetUnavailable) ||
            (failure.Context == SchedulingContextError::DeadlineExpired));
}

/// A failure caused by the selected Thread or SchedulingContext rather than by
/// this processor's coordinator or clock. The search skips that candidate.
[[nodiscard]] constexpr bool IsCandidateRejection(const DispatchCoordinatorFailure& failure) noexcept
{
    switch (failure.Error)
    {
    case DispatchCoordinatorError::InvalidThread:
    case DispatchCoordinatorError::ThreadRegistryMismatch:
    case DispatchCoordinatorError::ThreadInstanceMismatch:
    case DispatchCoordinatorError::ThreadPinRejected:
    case DispatchCoordinatorError::ThreadRejected:
    case DispatchCoordinatorError::SchedulingContextRejected:
        return true;

    case DispatchCoordinatorError::None:
    case DispatchCoordinatorError::AlreadyActive:
    case DispatchCoordinatorError::NotActive:
    case DispatchCoordinatorError::Faulted:
    case DispatchCoordinatorError::InvalidCpu:
    case DispatchCoordinatorError::InvalidDisposition:
    case DispatchCoordinatorError::ClockExhausted:
    case DispatchCoordinatorError::InvariantViolation:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr bool ThreadStateMatchesDisposition(const ThreadState state,
                                                           const ThreadRunDisposition disposition) noexcept
{
    switch (disposition)
    {
    case ThreadRunDisposition::Ready:
        return state == ThreadState::Ready;

    case ThreadRunDisposition::Blocked:
        return state == ThreadState::Blocked;

    case ThreadRunDisposition::Exited:
        return state == ThreadState::Exited;
    }
    return false;
}

[[nodiscard]] constexpr ProcessorActiveExecutionBindingFailure MakeActiveExecutionBindingFailure(
    const ProcessorActiveExecutionBindingError error, const ThreadState thread = ThreadState::Uninitialized,
    const SchedulingContextState context = SchedulingContextState::Uninitialized, const CpuId processor = {}) noexcept
{
    return ProcessorActiveExecutionBindingFailure{
        .Error = error,
        .Thread = thread,
        .SchedulingContext = context,
        .Processor = processor,
        .Reserved = {0U, 0U, 0U, 0U},
    };
}
} // namespace

ProcessorSchedulerFailure ProcessorScheduler::MakeFailure(const ProcessorSchedulerError error,
                                                          const Foundation::UInt64 retryAtTicks,
                                                          const ReadyQueueFailure& queue,
                                                          const DispatchCoordinatorFailure& dispatch,
                                                          const ReadyQueueError queueTransition) noexcept
{
    return ProcessorSchedulerFailure{
        .RetryAtTicks = retryAtTicks,
        .Dispatch = dispatch,
        .Queue = queue,
        .Error = error,
        .QueueTransition = queueTransition,
        .Reserved = {0U, 0U},
    };
}

Foundation::Result<ReadyQueueEnqueueReceipt, ProcessorSchedulerFailure>
ProcessorScheduler::Enqueue(const SchedulingContextId contextId, SchedulingContext& context, const ThreadId threadId,
                            Thread& thread) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueEnqueueReceipt, ProcessorSchedulerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProcessorSchedulerState::Faulted)
    {
        return ResultType::Failure(MakeFailure(m_processor.IsValid() ? ProcessorSchedulerError::Faulted
                                                                     : ProcessorSchedulerError::InvalidConfiguration));
    }
    const auto result = m_readyQueue.Enqueue(contextId, context, threadId, thread);
    if (!result.HasValue())
    {
        return ResultType::Failure(MakeFailure(ProcessorSchedulerError::QueueRejected, 0U, result.GetError()));
    }
    return ResultType::Success(result.GetValue());
}

Foundation::Result<ProcessorSchedulerAdmission, ProcessorSchedulerFailure> ProcessorScheduler::DispatchNext() noexcept
{
    using ResultType = Foundation::Result<ProcessorSchedulerAdmission, ProcessorSchedulerFailure>;

    ProcessorSchedulerAdmission admission{};
    ProcessorSchedulerFailure failure{};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!DispatchNextLocked(admission, failure, DeferredCandidatePolicy::ContinueSearch))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(admission);
}

Foundation::Result<ProcessorSchedulerAdmission, ProcessorSchedulerFailure>
ProcessorScheduler::DispatchNextWithoutDeferredWork() noexcept
{
    using ResultType = Foundation::Result<ProcessorSchedulerAdmission, ProcessorSchedulerFailure>;

    ProcessorSchedulerAdmission admission{};
    ProcessorSchedulerFailure failure{};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!DispatchNextLocked(admission, failure, DeferredCandidatePolicy::RejectDispatch))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(admission);
}

bool ProcessorScheduler::DispatchNextLocked(ProcessorSchedulerAdmission& admission, ProcessorSchedulerFailure& failure,
                                            const DeferredCandidatePolicy deferredCandidatePolicy) noexcept
{
    if (m_state == ProcessorSchedulerState::Faulted)
    {
        failure = MakeFailure(m_processor.IsValid() ? ProcessorSchedulerError::Faulted
                                                    : ProcessorSchedulerError::InvalidConfiguration);
        return false;
    }
    if (m_state == ProcessorSchedulerState::Active)
    {
        failure = MakeFailure(ProcessorSchedulerError::AlreadyActive);
        return false;
    }
    if (m_activeClaim.GetToken().IsValid() || (m_skippedClaimCount != 0U))
    {
        EnterFaultedLocked();
        failure = MakeFailure(ProcessorSchedulerError::InvariantViolation);
        return false;
    }

    // Nothing to rank: an empty queue is reported without a clock sample.
    if (m_readyQueue.GetState().ReadyEntries == 0U)
    {
        failure = MakeFailure(ProcessorSchedulerError::NoRunnableThread, 0U,
                              ReadyQueueFailure{
                                  .Error = ReadyQueueError::Empty,
                                  .ContextPin = Object::ObjectPinError::Ok,
                                  .ThreadPin = Object::ObjectPinError::Ok,
                                  .Reserved = 0U,
                              });
        return false;
    }

    // One selection sample ranks every candidate of this search; each
    // admission attempt still samples the clock in DispatchCoordinator::Start.
    const Object::MonotonicTime selectionTime{m_clock.GetTicks()};
    Foundation::UInt64 retryAtTicks{0U};
    DispatchCoordinatorFailure rejection{};
    for (Foundation::Size attempt{0U}; attempt < MaximumReadyQueueEntries; ++attempt)
    {
        const auto claimResult = m_readyQueue.ClaimNext(selectionTime);
        if (!claimResult.HasValue())
        {
            const ReadyQueueFailure queueFailure = claimResult.GetError();
            const ReadyQueueError restoreError = RestoreSkippedClaimsLocked();
            if (restoreError != ReadyQueueError::None)
            {
                EnterFaultedLocked();
                failure = MakeFailure(ProcessorSchedulerError::QueueTransitionFailed, retryAtTicks, queueFailure, {},
                                      restoreError);
                return false;
            }
            if (queueFailure.Error == ReadyQueueError::Empty)
            {
                failure = MakeFailure(ProcessorSchedulerError::NoRunnableThread, retryAtTicks, queueFailure, rejection);
                return false;
            }
            EnterFaultedLocked();
            failure = MakeFailure(ProcessorSchedulerError::QueueRejected, retryAtTicks, queueFailure);
            return false;
        }

        const CandidateOutcome outcome =
            DispatchClaimLocked(claimResult.GetValue(), retryAtTicks, rejection, admission, failure);
        if (outcome == CandidateOutcome::Admitted)
        {
            return true;
        }
        if (outcome == CandidateOutcome::Rejected)
        {
            return false;
        }
        if ((outcome == CandidateOutcome::Deferred) &&
            (deferredCandidatePolicy == DeferredCandidatePolicy::RejectDispatch))
        {
            const ReadyQueueError restoreError = RestoreSkippedClaimsLocked();
            if (restoreError != ReadyQueueError::None)
            {
                EnterFaultedLocked();
                failure =
                    MakeFailure(ProcessorSchedulerError::QueueTransitionFailed, retryAtTicks, {}, {}, restoreError);
                return false;
            }
            failure = MakeFailure(ProcessorSchedulerError::NoRunnableThread, retryAtTicks,
                                  ReadyQueueFailure{
                                      .Error = ReadyQueueError::Empty,
                                      .ContextPin = Object::ObjectPinError::Ok,
                                      .ThreadPin = Object::ObjectPinError::Ok,
                                      .Reserved = 0U,
                                  },
                                  rejection);
            return false;
        }
    }

    const ReadyQueueError restoreError = RestoreSkippedClaimsLocked();
    if (restoreError != ReadyQueueError::None)
    {
        EnterFaultedLocked();
        failure = MakeFailure(ProcessorSchedulerError::QueueTransitionFailed, retryAtTicks, {}, {}, restoreError);
        return false;
    }
    failure = MakeFailure(ProcessorSchedulerError::NoRunnableThread, retryAtTicks,
                          ReadyQueueFailure{
                              .Error = ReadyQueueError::Empty,
                              .ContextPin = Object::ObjectPinError::Ok,
                              .ThreadPin = Object::ObjectPinError::Ok,
                              .Reserved = 0U,
                          },
                          rejection);
    return false;
}

ProcessorScheduler::CandidateOutcome
ProcessorScheduler::DispatchClaimLocked(const ReadyQueueClaim& claim, Foundation::UInt64& retryAtTicks,
                                        DispatchCoordinatorFailure& rejection, ProcessorSchedulerAdmission& admission,
                                        ProcessorSchedulerFailure& failure) noexcept
{
    const auto dispatchResult =
        m_coordinator.Start(claim.GetContext(), claim.GetThread(), claim.GetThreadId(), m_processor);
    if (dispatchResult.HasValue())
    {
        m_activeClaim = claim;
        const ReadyQueueError restoreError = RestoreSkippedClaimsLocked();
        if (restoreError != ReadyQueueError::None)
        {
            EnterFaultedLocked();
            failure = MakeFailure(ProcessorSchedulerError::QueueTransitionFailed, retryAtTicks, {}, {}, restoreError);
            return CandidateOutcome::Rejected;
        }

        const DispatchAdmission& dispatch = dispatchResult.GetValue();
        if (dispatch.StartTicks >= (MaximumUInt64 - dispatch.Context.MaximumRunTicks))
        {
            EnterFaultedLocked();
            failure = MakeFailure(ProcessorSchedulerError::InvariantViolation);
            return CandidateOutcome::Rejected;
        }
        admission = ProcessorSchedulerAdmission{
            .Dispatch = dispatch,
            .Context = claim.GetContextId(),
            .ContextStorage = &claim.GetContext(),
            .ThreadStorage = &claim.GetThread(),
            .StopAtTicks = dispatch.StartTicks + dispatch.Context.MaximumRunTicks,
            .SchedulerRetryAtTicks = retryAtTicks,
        };
        m_activeAdmission = admission;
        m_state = ProcessorSchedulerState::Active;
        return CandidateOutcome::Admitted;
    }

    DispatchCoordinatorFailure dispatchFailure = dispatchResult.GetError();
    if (IsDeferredAdmission(dispatchFailure))
    {
        const auto retryResult =
            claim.GetContext().GetNextReplenishmentAfter(Object::MonotonicTime{dispatchFailure.ObservedTicks});
        if (!retryResult.HasValue())
        {
            dispatchFailure.Context = retryResult.GetError();
            const ReadyQueueError cancelError = m_readyQueue.CancelClaim(claim.GetToken());
            const ReadyQueueError restoreError =
                cancelError == ReadyQueueError::None ? RestoreSkippedClaimsLocked() : cancelError;
            if (restoreError != ReadyQueueError::None)
            {
                EnterFaultedLocked();
            }
            failure =
                MakeFailure(restoreError == ReadyQueueError::None ? ProcessorSchedulerError::RetryCalculationFailed
                                                                  : ProcessorSchedulerError::QueueTransitionFailed,
                            retryAtTicks, {}, dispatchFailure, restoreError);
            return CandidateOutcome::Rejected;
        }

        const Foundation::UInt64 candidateRetry = retryResult.GetValue().GetTicks();
        if ((retryAtTicks == 0U) || (candidateRetry < retryAtTicks))
        {
            retryAtTicks = candidateRetry;
        }
        m_skippedClaims[m_skippedClaimCount] = claim.GetToken();
        ++m_skippedClaimCount;
        return CandidateOutcome::Deferred;
    }
    if (IsCandidateRejection(dispatchFailure))
    {
        // The coordinator rolled this candidate back completely. It keeps its
        // claim, and so its FIFO position, until the search ends.
        if (rejection.Error == DispatchCoordinatorError::None)
        {
            rejection = dispatchFailure;
        }
        m_skippedClaims[m_skippedClaimCount] = claim.GetToken();
        ++m_skippedClaimCount;
        return CandidateOutcome::Skipped;
    }

    const ReadyQueueError cancelError = m_readyQueue.CancelClaim(claim.GetToken());
    const ReadyQueueError restoreError =
        cancelError == ReadyQueueError::None ? RestoreSkippedClaimsLocked() : cancelError;
    if (restoreError != ReadyQueueError::None)
    {
        EnterFaultedLocked();
        failure = MakeFailure(ProcessorSchedulerError::QueueTransitionFailed, retryAtTicks, {}, dispatchFailure,
                              restoreError);
        return CandidateOutcome::Rejected;
    }
    failure = MakeFailure(ProcessorSchedulerError::DispatchRejected, retryAtTicks, {}, dispatchFailure);
    return CandidateOutcome::Rejected;
}

Foundation::Result<DispatchCompletion, ProcessorSchedulerFailure>
ProcessorScheduler::Finish(const ThreadRunDisposition disposition, const Foundation::UInt64 exitCode) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, ProcessorSchedulerFailure>;

    DispatchCompletion completion{};
    ProcessorSchedulerFailure failure{};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!FinishLocked(disposition, exitCode, completion, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(completion);
}

Foundation::Result<DispatchCompletion, ProcessorSchedulerFailure>
ProcessorScheduler::FinishExternallyStoppedAsReady(const Foundation::UInt64 observedTicks) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, ProcessorSchedulerFailure>;

    DispatchCompletion completion{};
    ProcessorSchedulerFailure failure{};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProcessorSchedulerState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ProcessorSchedulerError::Faulted));
    }
    if (m_state != ProcessorSchedulerState::Active)
    {
        return ResultType::Failure(MakeFailure(ProcessorSchedulerError::NotActive));
    }
    if (!m_activeClaim.GetToken().IsValid())
    {
        EnterFaultedLocked();
        return ResultType::Failure(MakeFailure(ProcessorSchedulerError::InvariantViolation));
    }

    const auto completionResult = m_coordinator.FinishExternallyStoppedAsReady(observedTicks);
    if (!CommitCompletionLocked(ThreadRunDisposition::Ready, completionResult, completion, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(completion);
}

bool ProcessorScheduler::FinishLocked(const ThreadRunDisposition disposition, const Foundation::UInt64 exitCode,
                                      DispatchCompletion& completion, ProcessorSchedulerFailure& failure) noexcept
{
    if (m_state == ProcessorSchedulerState::Faulted)
    {
        failure = MakeFailure(ProcessorSchedulerError::Faulted);
        return false;
    }
    if (m_state != ProcessorSchedulerState::Active)
    {
        failure = MakeFailure(ProcessorSchedulerError::NotActive);
        return false;
    }
    if (!m_activeClaim.GetToken().IsValid())
    {
        EnterFaultedLocked();
        failure = MakeFailure(ProcessorSchedulerError::InvariantViolation);
        return false;
    }

    const auto completionResult = m_coordinator.Finish(disposition, exitCode);
    return CommitCompletionLocked(disposition, completionResult, completion, failure);
}

bool ProcessorScheduler::CommitCompletionLocked(
    const ThreadRunDisposition disposition,
    const Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>& completionResult,
    DispatchCompletion& completion, ProcessorSchedulerFailure& failure) noexcept
{
    if (!completionResult.HasValue())
    {
        const DispatchCoordinatorFailure dispatchFailure = completionResult.GetError();
        if ((dispatchFailure.Error == DispatchCoordinatorError::Faulted) ||
            (dispatchFailure.Error == DispatchCoordinatorError::InvariantViolation))
        {
            EnterFaultedLocked();
        }
        failure = MakeFailure(ProcessorSchedulerError::DispatchRejected, 0U, {}, dispatchFailure);
        return false;
    }

    const DispatchCompletion& completedRun = completionResult.GetValue();
    if (!ThreadStateMatchesDisposition(completedRun.Thread.State, disposition))
    {
        EnterFaultedLocked();
        failure = MakeFailure(ProcessorSchedulerError::InvariantViolation);
        return false;
    }
    const ReadyQueueError transitionError = disposition == ThreadRunDisposition::Ready
                                                ? m_readyQueue.RequeueClaim(m_activeClaim.GetToken())
                                                : m_readyQueue.CommitClaim(m_activeClaim.GetToken());
    if (transitionError != ReadyQueueError::None)
    {
        EnterFaultedLocked();
        failure = MakeFailure(ProcessorSchedulerError::QueueTransitionFailed, 0U, {}, {}, transitionError);
        return false;
    }

    completion = completedRun;
    m_activeClaim = ReadyQueueClaim{};
    m_activeAdmission = ProcessorSchedulerAdmission{};
    m_state = ProcessorSchedulerState::Idle;
    return true;
}

Foundation::Result<ReadyQueueState, ProcessorSchedulerFailure>
ProcessorScheduler::Remove(const ThreadId thread) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueState, ProcessorSchedulerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProcessorSchedulerState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ProcessorSchedulerError::Faulted));
    }
    const ReadyQueueError error = m_readyQueue.Remove(thread);
    if (error != ReadyQueueError::None)
    {
        return ResultType::Failure(MakeFailure(ProcessorSchedulerError::QueueRejected, 0U,
                                               ReadyQueueFailure{
                                                   .Error = error,
                                                   .ContextPin = Object::ObjectPinError::Ok,
                                                   .ThreadPin = Object::ObjectPinError::Ok,
                                                   .Reserved = 0U,
                                               }));
    }
    return ResultType::Success(m_readyQueue.GetState());
}

ProcessorSchedulerSnapshot ProcessorScheduler::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ProcessorSchedulerSnapshot{
        .Queue = m_readyQueue.GetState(),
        .ActiveThread = m_activeClaim.GetThreadId(),
        .ActiveContext = m_activeClaim.GetContextId(),
        .State = m_state,
        .Processor = m_processor,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

bool ProcessorScheduler::AdmissionsMatch(const ProcessorSchedulerAdmission& expected,
                                         const ProcessorSchedulerAdmission& candidate) noexcept
{
    bool reservedMatches{true};
    for (Foundation::Size index{0U}; index < sizeof(expected.Dispatch.Thread.Reserved); ++index)
    {
        reservedMatches =
            reservedMatches && (expected.Dispatch.Thread.Reserved[index] == candidate.Dispatch.Thread.Reserved[index]);
    }
    return expected.Dispatch.Context.Thread.IsEqualTo(candidate.Dispatch.Context.Thread) &&
           (expected.Dispatch.Context.MaximumRunTicks == candidate.Dispatch.Context.MaximumRunTicks) &&
           (expected.Dispatch.Context.Weight == candidate.Dispatch.Context.Weight) &&
           (expected.Dispatch.Context.Priority == candidate.Dispatch.Context.Priority) &&
           (expected.Dispatch.Context.Criticality == candidate.Dispatch.Context.Criticality) &&
           (expected.Dispatch.Context.Class == candidate.Dispatch.Context.Class) &&
           (expected.Dispatch.Context.Processor.GetValue() == candidate.Dispatch.Context.Processor.GetValue()) &&
           (expected.Dispatch.Context.Reserved == candidate.Dispatch.Context.Reserved) &&
           expected.Dispatch.Thread.BlockToken.IsEqualTo(candidate.Dispatch.Thread.BlockToken) &&
           (expected.Dispatch.Thread.RunSequence == candidate.Dispatch.Thread.RunSequence) &&
           (expected.Dispatch.Thread.ExitCode == candidate.Dispatch.Thread.ExitCode) &&
           (expected.Dispatch.Thread.State == candidate.Dispatch.Thread.State) &&
           (expected.Dispatch.Thread.RunningProcessor.GetValue() ==
            candidate.Dispatch.Thread.RunningProcessor.GetValue()) &&
           (expected.Dispatch.StartTicks == candidate.Dispatch.StartTicks) &&
           expected.Context.IsEqualTo(candidate.Context) && (expected.ContextStorage == candidate.ContextStorage) &&
           (expected.ThreadStorage == candidate.ThreadStorage) && (expected.StopAtTicks == candidate.StopAtTicks) &&
           (expected.SchedulerRetryAtTicks == candidate.SchedulerRetryAtTicks) && reservedMatches;
}

Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>
ProcessorScheduler::GetActiveExecutionBinding(const ProcessorSchedulerAdmission& admission) noexcept
{
    using ResultType = Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>;

    ProcessorActiveExecutionBinding binding{};
    ProcessorActiveExecutionBindingFailure failure{};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!GetActiveExecutionBindingLocked(admission, binding, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(binding);
}

bool ProcessorScheduler::GetActiveExecutionBindingLocked(const ProcessorSchedulerAdmission& admission,
                                                         ProcessorActiveExecutionBinding& binding,
                                                         ProcessorActiveExecutionBindingFailure& failure) noexcept
{
    if (!m_processor.IsValid())
    {
        failure = MakeActiveExecutionBindingFailure(ProcessorActiveExecutionBindingError::InvalidConfiguration);
        return false;
    }
    if (m_state == ProcessorSchedulerState::Faulted)
    {
        failure =
            MakeActiveExecutionBindingFailure(ProcessorActiveExecutionBindingError::Faulted, ThreadState::Uninitialized,
                                              SchedulingContextState::Uninitialized, m_processor);
        return false;
    }
    if (m_state != ProcessorSchedulerState::Active)
    {
        failure = MakeActiveExecutionBindingFailure(ProcessorActiveExecutionBindingError::NotActive,
                                                    ThreadState::Uninitialized, SchedulingContextState::Uninitialized,
                                                    m_processor);
        return false;
    }

    const DispatchCoordinatorSnapshot coordinator = m_coordinator.GetSnapshot();
    if (!m_activeClaim.GetToken().IsValid() || !AdmissionsMatch(m_activeAdmission, admission) ||
        !m_activeClaim.GetThreadId().IsEqualTo(m_activeAdmission.Dispatch.Context.Thread) ||
        !m_activeClaim.GetContextId().IsEqualTo(m_activeAdmission.Context) ||
        (&m_activeClaim.GetThread() != m_activeAdmission.ThreadStorage) ||
        (&m_activeClaim.GetContext() != m_activeAdmission.ContextStorage) ||
        (coordinator.State != DispatchCoordinatorState::Active) ||
        !coordinator.Thread.IsEqualTo(m_activeAdmission.Dispatch.Context.Thread) || !coordinator.Processor.IsValid() ||
        (coordinator.Processor.GetValue() != m_processor.GetValue()))
    {
        EnterFaultedLocked();
        failure = MakeActiveExecutionBindingFailure(ProcessorActiveExecutionBindingError::AdmissionMismatch,
                                                    ThreadState::Uninitialized, SchedulingContextState::Uninitialized,
                                                    m_processor);
        return false;
    }

    const ThreadSnapshot thread = m_activeAdmission.ThreadStorage->GetSnapshot();
    if ((thread.State != ThreadState::Running) || !thread.RunningProcessor.IsValid() ||
        (thread.RunningProcessor.GetValue() != m_processor.GetValue()) || thread.BlockToken.IsValid() ||
        (thread.RunSequence == 0U) || (thread.RunSequence != m_activeAdmission.Dispatch.Thread.RunSequence) ||
        (thread.ExitCode != 0U))
    {
        EnterFaultedLocked();
        failure = MakeActiveExecutionBindingFailure(ProcessorActiveExecutionBindingError::ThreadStateMismatch,
                                                    thread.State, SchedulingContextState::Uninitialized, m_processor);
        return false;
    }

    const SchedulingContextSnapshot context = m_activeAdmission.ContextStorage->GetSnapshot();
    if ((context.State != SchedulingContextState::Running) || !context.RunningProcessor.IsValid() ||
        (context.RunningProcessor.GetValue() != m_processor.GetValue()) ||
        !context.CurrentThread.IsEqualTo(m_activeAdmission.Dispatch.Context.Thread))
    {
        EnterFaultedLocked();
        failure =
            MakeActiveExecutionBindingFailure(ProcessorActiveExecutionBindingError::SchedulingContextStateMismatch,
                                              thread.State, context.State, m_processor);
        return false;
    }

    binding = ProcessorActiveExecutionBinding{
        .ThreadIdentity = m_activeAdmission.Dispatch.Context.Thread,
        .ThreadStorage = m_activeAdmission.ThreadStorage,
        .SchedulingContextIdentity = m_activeAdmission.Context,
        .SchedulingContextStorage = m_activeAdmission.ContextStorage,
        .Processor = m_processor,
        .RunSequence = thread.RunSequence,
    };
    return true;
}

ReadyQueueError ProcessorScheduler::RestoreSkippedClaimsLocked() noexcept
{
    while (m_skippedClaimCount != 0U)
    {
        const Foundation::Size index = m_skippedClaimCount - 1U;
        const ReadyQueueError error = m_readyQueue.CancelClaim(m_skippedClaims[index]);
        if (error != ReadyQueueError::None)
        {
            return error;
        }
        m_skippedClaims[index] = ReadyQueueClaimToken{};
        --m_skippedClaimCount;
    }
    return ReadyQueueError::None;
}

void ProcessorScheduler::EnterFaultedLocked() noexcept
{
    m_state = ProcessorSchedulerState::Faulted;
}
} // namespace Asteria::Kernel::Scheduler
