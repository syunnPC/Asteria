// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Scheduler/ProcessorRunController.hpp"

namespace Asteria::Kernel::Scheduler
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt8 TimerQueueDeadlineSource =
    static_cast<Foundation::UInt8>(Time::ProcessorDeadlineSource::TimerQueue);
constexpr Foundation::UInt8 RunStopDeadlineSource =
    static_cast<Foundation::UInt8>(Time::ProcessorDeadlineSource::RunStop);
constexpr Foundation::UInt8 SchedulerRetryDeadlineSource =
    static_cast<Foundation::UInt8>(Time::ProcessorDeadlineSource::SchedulerRetry);

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size size) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsRecoverableCooperativeDeadline(const Time::ProcessorDeadlineState state,
                                                              const Foundation::UInt64 selectedDeadlineTicks,
                                                              const Foundation::UInt8 selectedSources) noexcept
{
    return (state == Time::ProcessorDeadlineState::Armed) && (selectedDeadlineTicks != 0U) &&
           (selectedDeadlineTicks != MaximumUInt64) && (selectedSources == TimerQueueDeadlineSource);
}

static_assert(IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, 1U, TimerQueueDeadlineSource));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, 1U, RunStopDeadlineSource));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, 1U, SchedulerRetryDeadlineSource));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, 1U,
                                                static_cast<Foundation::UInt8>(TimerQueueDeadlineSource |
                                                                               RunStopDeadlineSource)));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, 1U, 0U));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, 1U, 0x80U));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, 0U, TimerQueueDeadlineSource));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Armed, MaximumUInt64,
                                                TimerQueueDeadlineSource));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Disarmed, 1U, TimerQueueDeadlineSource));
static_assert(!IsRecoverableCooperativeDeadline(Time::ProcessorDeadlineState::Faulted, 1U, TimerQueueDeadlineSource));
}

ProcessorRunFailure ProcessorRunController::MakeFailure(
    const ProcessorRunError error, const ProcessorSchedulerFailure& scheduler, const Time::TimerQueueState& timerQueue,
    const Time::ProcessorDeadlineFailure& deadline, const Time::ProcessorDeadlineReceipt& deadlineReceipt,
    const bool activeRunRetained, const ProcessorRunExternalStopError externalStop) noexcept
{
    return ProcessorRunFailure{
        .Scheduler = scheduler,
        .TimerQueue = timerQueue,
        .Deadline = deadline,
        .DeadlineReceipt = deadlineReceipt,
        .Error = error,
        .ActiveRunRetained = static_cast<Foundation::UInt8>(activeRunRetained ? 1U : 0U),
        .ExternalStop = externalStop,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    };
}

Foundation::Result<Time::ProcessorDeadlineReceipt, Time::ProcessorDeadlineFailure>
ProcessorRunController::ProgramDeadline(const Foundation::UInt64 runStopTicks,
                                        const Foundation::UInt64 schedulerRetryTicks,
                                        const Time::TimerQueueState& timerQueue,
                                        const Foundation::UInt64 minimumObservedTicks) noexcept
{
    return m_deadline.Program(
        Time::ProcessorDeadlineSources{
            .RunStopTicks = runStopTicks,
            .TimerQueueTicks = timerQueue.NextDeadlineTicks,
            .SchedulerRetryTicks = schedulerRetryTicks,
        },
        minimumObservedTicks);
}

ProcessorRunFailure ProcessorRunController::MakeStateFailure(const ProcessorRunError error) const noexcept
{
    const ProcessorSchedulerSnapshot scheduler = m_scheduler.GetSnapshot();
    const ProcessorRunError reportedError =
        !m_processor.IsValid() || !IsValidMode(m_mode) ? ProcessorRunError::InvalidConfiguration : error;
    return MakeFailure(reportedError, {}, m_timerQueue.GetState(), {}, {}, scheduler.ActiveThread.IsValid());
}

ProcessorRunFailure
ProcessorRunController::MakeSchedulerOperationFailure(const ProcessorSchedulerFailure& schedulerFailure) noexcept
{
    const ProcessorSchedulerSnapshot scheduler = m_scheduler.GetSnapshot();
    if (scheduler.State == ProcessorSchedulerState::Faulted)
    {
        EnterFaulted();
    }
    return MakeFailure(ProcessorRunError::SchedulerRejected, schedulerFailure, m_timerQueue.GetState(), {}, {},
                       scheduler.ActiveThread.IsValid());
}

void ProcessorRunController::EnterFaulted() noexcept
{
    m_state = ProcessorRunState::Faulted;
}

void ProcessorRunController::ClearActiveAdmission() noexcept
{
    m_activeAdmission = ProcessorRunAdmission{};
}

void ProcessorRunController::FinishDeferredDispatch(const ProcessorSchedulerFailure& schedulerFailure,
                                                    const Time::TimerQueueState& timerQueue,
                                                    ProcessorRunFailure& failure) noexcept
{
    if (schedulerFailure.Error != ProcessorSchedulerError::NoRunnableThread)
    {
        const ProcessorSchedulerSnapshot scheduler = m_scheduler.GetSnapshot();
        if (scheduler.State == ProcessorSchedulerState::Faulted)
        {
            EnterFaulted();
        }
        failure = MakeFailure(ProcessorRunError::SchedulerRejected, schedulerFailure, timerQueue, {}, {},
                              scheduler.ActiveThread.IsValid());
        return;
    }

    const auto deadlineResult = ProgramDeadline(0U, schedulerFailure.RetryAtTicks, timerQueue);
    if (deadlineResult.HasValue())
    {
        failure = MakeFailure(ProcessorRunError::NoRunnableThread, schedulerFailure, timerQueue, {},
                              deadlineResult.GetValue());
        return;
    }

    const Time::ProcessorDeadlineFailure deadlineFailure = deadlineResult.GetError();
    const bool reached = deadlineFailure.Error == Time::ProcessorDeadlineError::DeadlineReached;
    if (!reached)
    {
        EnterFaulted();
    }
    failure = MakeFailure(reached ? ProcessorRunError::DeadlineReached : ProcessorRunError::DeadlineRejected,
                          schedulerFailure, timerQueue, deadlineFailure);
}

void ProcessorRunController::FinishRejectedAdmissionCompletion(const ProcessorSchedulerFailure& schedulerFailure,
                                                               const Time::TimerQueueState& timerQueue,
                                                               const Time::ProcessorDeadlineFailure& deadlineFailure,
                                                               ProcessorRunFailure& failure) noexcept
{
    const bool retained = m_scheduler.GetSnapshot().ActiveThread.IsValid();
    EnterFaulted();
    failure =
        MakeFailure(ProcessorRunError::CompletionRejected, schedulerFailure, timerQueue, deadlineFailure, {}, retained);
}

void ProcessorRunController::FinishRejectedAdmission(const Time::TimerQueueState& timerQueue,
                                                     const Time::ProcessorDeadlineFailure& deadlineFailure,
                                                     ProcessorRunFailure& failure) noexcept
{
    if (deadlineFailure.Error == Time::ProcessorDeadlineError::DeadlineReached)
    {
        const auto completion = m_scheduler.Finish(ThreadRunDisposition::Ready);
        if (completion.HasValue())
        {
            failure = MakeFailure(ProcessorRunError::DeadlineReached, {}, timerQueue, deadlineFailure);
            return;
        }

        FinishRejectedAdmissionCompletion(completion.GetError(), timerQueue, deadlineFailure, failure);
        return;
    }

    EnterFaulted();
    failure = MakeFailure(ProcessorRunError::DeadlineRejected, {}, timerQueue, deadlineFailure, {}, true);
}

bool ProcessorRunController::FinishAdmittedDispatch(const ProcessorSchedulerAdmission& schedulerAdmission,
                                                    const Time::TimerQueueState& timerQueue,
                                                    ProcessorRunAdmission& admission,
                                                    ProcessorRunFailure& failure) noexcept
{
    const auto deadlineResult =
        ProgramDeadline(schedulerAdmission.StopAtTicks, schedulerAdmission.SchedulerRetryAtTicks, timerQueue,
                        schedulerAdmission.Dispatch.StartTicks);
    if (!deadlineResult.HasValue())
    {
        FinishRejectedAdmission(timerQueue, deadlineResult.GetError(), failure);
        return false;
    }

    admission = ProcessorRunAdmission{
        .Scheduler = schedulerAdmission,
        .Deadline = deadlineResult.GetValue(),
        .Mode = ProcessorRunDispatchMode::HardwareDeadline,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
    m_activeAdmission = admission;
    m_state = ProcessorRunState::Active;
    return true;
}

bool ProcessorRunController::DispatchNextCooperatively(const Time::TimerQueueState& timerQueue,
                                                       ProcessorRunAdmission& admission,
                                                       ProcessorRunFailure& failure) noexcept
{
    const auto schedulerResult = m_scheduler.DispatchNextWithoutDeferredWork();
    if (!schedulerResult.HasValue())
    {
        FinishRejectedCooperativeDispatch(schedulerResult.GetErrorReference(), timerQueue, failure);
        return false;
    }

    FinishCooperativeDispatch(schedulerResult.GetValue(), admission);
    return true;
}

void ProcessorRunController::FinishRejectedCooperativeDispatch(const ProcessorSchedulerFailure& schedulerFailure,
                                                               const Time::TimerQueueState& timerQueue,
                                                               ProcessorRunFailure& failure) noexcept
{
    if (schedulerFailure.Error != ProcessorSchedulerError::NoRunnableThread)
    {
        const ProcessorSchedulerSnapshot scheduler = m_scheduler.GetSnapshot();
        if (scheduler.State == ProcessorSchedulerState::Faulted)
        {
            EnterFaulted();
        }
        failure = MakeFailure(ProcessorRunError::SchedulerRejected, schedulerFailure, timerQueue, {}, {},
                              scheduler.ActiveThread.IsValid());
        return;
    }

    failure = MakeFailure(schedulerFailure.RetryAtTicks == 0U ? ProcessorRunError::NoRunnableThread
                                                              : ProcessorRunError::DeferredWorkPending,
                          schedulerFailure, timerQueue);
}

void ProcessorRunController::FinishCooperativeDispatch(const ProcessorSchedulerAdmission& schedulerAdmission,
                                                       ProcessorRunAdmission& admission) noexcept
{
    admission = ProcessorRunAdmission{
        .Scheduler = schedulerAdmission,
        .Deadline =
            Time::ProcessorDeadlineReceipt{
                .SelectedDeadlineTicks = 0U,
                .ObservedTicks = schedulerAdmission.Dispatch.StartTicks,
                .State = Time::ProcessorDeadlineState::Disarmed,
                .SelectedSources = 0U,
                .Reprogrammed = 0U,
                .Reserved = {0U, 0U, 0U, 0U, 0U},
            },
        .Mode = ProcessorRunDispatchMode::CooperativeBootstrap,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
    m_activeAdmission = admission;
    m_state = ProcessorRunState::Active;
}

Foundation::Result<DispatchCompletion, ProcessorRunFailure>
ProcessorRunController::FinishRejectedRun(const ProcessorSchedulerFailure& schedulerFailure) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, ProcessorRunFailure>;

    const ProcessorSchedulerSnapshot scheduler = m_scheduler.GetSnapshot();
    if (scheduler.State == ProcessorSchedulerState::Faulted)
    {
        EnterFaulted();
    }
    return ResultType::Failure(MakeFailure(ProcessorRunError::CompletionRejected, schedulerFailure,
                                           m_timerQueue.GetState(), {}, {}, scheduler.ActiveThread.IsValid()));
}

Foundation::Result<ReadyQueueEnqueueReceipt, ProcessorRunFailure>
ProcessorRunController::Enqueue(const SchedulingContextId contextId, SchedulingContext& context,
                                const ThreadId threadId, Thread& thread) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueEnqueueReceipt, ProcessorRunFailure>;

    if (m_state == ProcessorRunState::Faulted)
    {
        return ResultType::Failure(MakeStateFailure(ProcessorRunError::Faulted));
    }

    const auto result = m_scheduler.Enqueue(contextId, context, threadId, thread);
    if (!result.HasValue())
    {
        return ResultType::Failure(MakeSchedulerOperationFailure(result.GetError()));
    }
    return ResultType::Success(result.GetValue());
}

Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>
ProcessorRunController::GetActiveExecutionBinding() noexcept
{
    using ResultType = Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>;

    if (!m_processor.IsValid() || !IsValidMode(m_mode))
    {
        return ResultType::Failure(ProcessorActiveExecutionBindingFailure{
            .Error = ProcessorActiveExecutionBindingError::InvalidConfiguration,
            .Thread = ThreadState::Uninitialized,
            .SchedulingContext = SchedulingContextState::Uninitialized,
            .Processor = m_processor,
            .Reserved = {0U, 0U, 0U, 0U},
        });
    }
    if (m_state == ProcessorRunState::Faulted)
    {
        return ResultType::Failure(ProcessorActiveExecutionBindingFailure{
            .Error = ProcessorActiveExecutionBindingError::Faulted,
            .Thread = ThreadState::Uninitialized,
            .SchedulingContext = SchedulingContextState::Uninitialized,
            .Processor = m_processor,
            .Reserved = {0U, 0U, 0U, 0U},
        });
    }
    if (m_state != ProcessorRunState::Active)
    {
        return ResultType::Failure(ProcessorActiveExecutionBindingFailure{
            .Error = ProcessorActiveExecutionBindingError::NotActive,
            .Thread = ThreadState::Uninitialized,
            .SchedulingContext = SchedulingContextState::Uninitialized,
            .Processor = m_processor,
            .Reserved = {0U, 0U, 0U, 0U},
        });
    }

    const auto result = m_scheduler.GetActiveExecutionBinding(m_activeAdmission.Scheduler);
    if (!result.HasValue())
    {
        EnterFaulted();
        return ResultType::Failure(result.GetErrorReference());
    }

    const ProcessorActiveExecutionBinding& binding = result.GetValue();
    if (!binding.ThreadIdentity.IsEqualTo(m_activeAdmission.Scheduler.Dispatch.Context.Thread) ||
        (binding.ThreadStorage != m_activeAdmission.Scheduler.ThreadStorage) ||
        !binding.SchedulingContextIdentity.IsEqualTo(m_activeAdmission.Scheduler.Context) ||
        (binding.SchedulingContextStorage != m_activeAdmission.Scheduler.ContextStorage) ||
        !binding.Processor.IsValid() || (binding.Processor.GetValue() != m_processor.GetValue()) ||
        (binding.RunSequence == 0U) || (binding.RunSequence != m_activeAdmission.Scheduler.Dispatch.Thread.RunSequence))
    {
        EnterFaulted();
        return ResultType::Failure(ProcessorActiveExecutionBindingFailure{
            .Error = ProcessorActiveExecutionBindingError::AdmissionMismatch,
            .Thread = binding.ThreadStorage->GetSnapshot().State,
            .SchedulingContext = binding.SchedulingContextStorage->GetSnapshot().State,
            .Processor = m_processor,
            .Reserved = {0U, 0U, 0U, 0U},
        });
    }
    return ResultType::Success(binding);
}

Foundation::Result<ReadyQueueState, ProcessorRunFailure> ProcessorRunController::Remove(const ThreadId thread) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueState, ProcessorRunFailure>;

    if (m_state == ProcessorRunState::Faulted)
    {
        return ResultType::Failure(MakeStateFailure(ProcessorRunError::Faulted));
    }

    const auto result = m_scheduler.Remove(thread);
    if (!result.HasValue())
    {
        return ResultType::Failure(MakeSchedulerOperationFailure(result.GetError()));
    }
    return ResultType::Success(result.GetValue());
}

bool ProcessorRunController::DispatchNextTransaction(ProcessorRunAdmission& admission,
                                                     ProcessorRunFailure& failure) noexcept
{
    if (m_state == ProcessorRunState::Faulted)
    {
        failure = MakeStateFailure(ProcessorRunError::Faulted);
        return false;
    }
    if (m_state == ProcessorRunState::Active)
    {
        failure = MakeStateFailure(ProcessorRunError::AlreadyActive);
        return false;
    }

    const Time::TimerQueueState timerQueue = m_timerQueue.GetState();
    const bool cooperative = m_mode == ProcessorRunDispatchMode::CooperativeBootstrap;
    if ((timerQueue.PendingEntries != 0U) || (timerQueue.ClaimedEntries != 0U) ||
        (cooperative && ((timerQueue.ScheduledEntries != 0U) || (timerQueue.NextDeadlineTicks != 0U))))
    {
        failure = MakeFailure(ProcessorRunError::TimerWorkPending, {}, timerQueue);
        return false;
    }

    if (cooperative)
    {
        if (!ValidateCooperativeDispatchDeadline(timerQueue, failure))
        {
            return false;
        }
        return DispatchNextCooperatively(timerQueue, admission, failure);
    }

    return DispatchNextWithHardwareDeadline(timerQueue, admission, failure);
}

bool ProcessorRunController::DispatchNextWithHardwareDeadline(const Time::TimerQueueState& timerQueue,
                                                              ProcessorRunAdmission& admission,
                                                              ProcessorRunFailure& failure) noexcept
{
    const auto schedulerResult = m_scheduler.DispatchNext();
    if (!schedulerResult.HasValue())
    {
        FinishDeferredDispatch(schedulerResult.GetError(), timerQueue, failure);
        return false;
    }
    return FinishAdmittedDispatch(schedulerResult.GetValue(), timerQueue, admission, failure);
}

Foundation::Result<ProcessorRunAdmission, ProcessorRunFailure> ProcessorRunController::DispatchNext() noexcept
{
    using ResultType = Foundation::Result<ProcessorRunAdmission, ProcessorRunFailure>;

    ProcessorRunAdmission admission{};
    ProcessorRunFailure failure{};
    if (!DispatchNextTransaction(admission, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(admission);
}

bool ProcessorRunController::ValidateCooperativeDispatchDeadline(const Time::TimerQueueState& timerQueue,
                                                                 ProcessorRunFailure& failure) noexcept
{
    const Time::ProcessorDeadlineSnapshot snapshot = m_deadline.GetSnapshot();
    if ((snapshot.State == Time::ProcessorDeadlineState::Disarmed) && (snapshot.SelectedDeadlineTicks == 0U) &&
        (snapshot.SelectedSources == 0U))
    {
        return true;
    }

    const Time::ProcessorDeadlineReceipt receipt = MakeCurrentDeadlineReceipt(snapshot, 0U);
    if (IsRecoverableCooperativeDeadline(snapshot.State, snapshot.SelectedDeadlineTicks, snapshot.SelectedSources))
    {
        failure = MakeFailure(ProcessorRunError::DeadlineRefreshRequired, {}, timerQueue, {}, receipt);
        return false;
    }

    const Time::ProcessorDeadlineError deadlineError = snapshot.State == Time::ProcessorDeadlineState::Faulted
                                                           ? Time::ProcessorDeadlineError::Faulted
                                                           : Time::ProcessorDeadlineError::InvalidDeadline;
    EnterFaulted();
    failure = MakeFailure(ProcessorRunError::DeadlineRejected, {}, timerQueue,
                          Time::ProcessorDeadlineFailure{
                              .SelectedDeadlineTicks = snapshot.SelectedDeadlineTicks,
                              .ObservedTicks = 0U,
                              .Error = deadlineError,
                              .HardwareError = Hal::Time::LocalTimerError::None,
                              .AffectedSources = snapshot.SelectedSources,
                              .Reserved = {0U, 0U, 0U, 0U, 0U},
                          },
                          receipt);
    return false;
}

bool ProcessorRunController::RefreshTimerQueueDeadlineTransaction(const Foundation::UInt64 minimumObservedTicks,
                                                                  Time::ProcessorDeadlineReceipt& receipt,
                                                                  ProcessorRunFailure& failure) noexcept
{
    if (m_state == ProcessorRunState::Faulted)
    {
        failure = MakeStateFailure(ProcessorRunError::Faulted);
        return false;
    }
    if (m_state != ProcessorRunState::Idle)
    {
        if (m_mode == ProcessorRunDispatchMode::HardwareDeadline)
        {
            return RefreshActiveDeadlineTransaction(minimumObservedTicks, receipt, failure);
        }
        failure = MakeStateFailure(ProcessorRunError::AlreadyActive);
        return false;
    }

    const Time::TimerQueueState timerQueue = m_timerQueue.GetState();
    if ((timerQueue.PendingEntries != 0U) || (timerQueue.ClaimedEntries != 0U))
    {
        failure = MakeFailure(ProcessorRunError::TimerWorkPending, {}, timerQueue);
        return false;
    }

    const auto deadline = ProgramDeadline(0U, 0U, timerQueue, minimumObservedTicks);
    if (!deadline.HasValue())
    {
        failure = MakeRollbackDeadlineFailure(timerQueue, deadline.GetErrorReference());
        return false;
    }

    receipt = deadline.GetValue();
    return true;
}

bool ProcessorRunController::RefreshActiveDeadlineTransaction(const Foundation::UInt64 minimumObservedTicks,
                                                              Time::ProcessorDeadlineReceipt& receipt,
                                                              ProcessorRunFailure& failure) noexcept
{
    const Time::TimerQueueState timerQueue = m_timerQueue.GetState();
    const Foundation::UInt64 runStopTicks = m_activeAdmission.Scheduler.StopAtTicks;
    auto deadline = ProgramDeadline(runStopTicks, m_activeAdmission.Scheduler.SchedulerRetryAtTicks, timerQueue,
                                    minimumObservedTicks);
    if (!deadline.HasValue() && (deadline.GetErrorReference().Error == Time::ProcessorDeadlineError::DeadlineReached) &&
        (deadline.GetErrorReference().AffectedSources == SchedulerRetryDeadlineSource))
    {
        // The earliest due source is the retry alone: its replenished
        // candidate cannot preempt this run here and is considered at the next
        // dispatch, so only the remaining sources are installed.
        deadline = ProgramDeadline(runStopTicks, 0U, timerQueue, minimumObservedTicks);
    }
    if (!deadline.HasValue())
    {
        const Time::ProcessorDeadlineFailure& deadlineFailure = deadline.GetErrorReference();
        const bool reached = deadlineFailure.Error == Time::ProcessorDeadlineError::DeadlineReached;
        if (!reached)
        {
            EnterFaulted();
        }
        failure = MakeFailure(
            reached ? ProcessorRunError::DeadlineReached : ProcessorRunError::DeadlineRejected, {}, timerQueue,
            deadlineFailure, MakeCurrentDeadlineReceipt(m_deadline.GetSnapshot(), deadlineFailure.ObservedTicks), true);
        return false;
    }

    receipt = deadline.GetValue();
    m_activeAdmission.Deadline = receipt;
    return true;
}

Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>
ProcessorRunController::RefreshTimerQueueDeadline(const Foundation::UInt64 minimumObservedTicks) noexcept
{
    using ResultType = Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>;

    Time::ProcessorDeadlineReceipt receipt{};
    ProcessorRunFailure failure{};
    if (!RefreshTimerQueueDeadlineTransaction(minimumObservedTicks, receipt, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(receipt);
}

bool ProcessorRunController::ValidateActiveAdmissionIdentity(const ProcessorRunAdmission& admission,
                                                             ProcessorRunEntryFailure& failure) noexcept
{
    if (m_state == ProcessorRunState::Faulted)
    {
        failure = ProcessorRunEntryFailure{
            .Deadline = {},
            .DeadlineReceipt = m_activeAdmission.Deadline,
            .Error = ProcessorRunError::Faulted,
            .ActiveRunRetained =
                static_cast<Foundation::UInt8>(m_scheduler.GetSnapshot().ActiveThread.IsValid() ? 1U : 0U),
            .Reserved = {},
        };
        return false;
    }
    if (m_state != ProcessorRunState::Active)
    {
        const bool retained = m_scheduler.GetSnapshot().ActiveThread.IsValid();
        EnterFaulted();
        failure = ProcessorRunEntryFailure{
            .Deadline = {},
            .DeadlineReceipt = m_activeAdmission.Deadline,
            .Error = ProcessorRunError::ActiveAdmissionMismatch,
            .ActiveRunRetained = static_cast<Foundation::UInt8>(retained ? 1U : 0U),
            .Reserved = {},
        };
        return false;
    }

    bool retained{false};
    if (!ActiveAdmissionMatches(admission, retained))
    {
        EnterFaulted();
        failure = ProcessorRunEntryFailure{
            .Deadline = {},
            .DeadlineReceipt = m_activeAdmission.Deadline,
            .Error = ProcessorRunError::ActiveAdmissionMismatch,
            .ActiveRunRetained = static_cast<Foundation::UInt8>(retained ? 1U : 0U),
            .Reserved = {},
        };
        return false;
    }

    return true;
}

bool ProcessorRunController::ValidateActiveAdmission(const ProcessorRunAdmission& admission,
                                                     ProcessorRunEntryFailure& failure) noexcept
{
    if (!ValidateActiveAdmissionIdentity(admission, failure))
    {
        return false;
    }
    if (m_mode == ProcessorRunDispatchMode::CooperativeBootstrap)
    {
        return ValidateCooperativeAdmissionDeadline(admission, failure);
    }

    const auto validation = m_deadline.ValidateCurrent(admission.Deadline, admission.Scheduler.Dispatch.StartTicks);
    if (validation.HasValue())
    {
        return true;
    }

    const Time::ProcessorDeadlineFailure deadlineFailure = validation.GetError();
    const bool reached = deadlineFailure.Error == Time::ProcessorDeadlineError::DeadlineReached;
    if (!reached)
    {
        EnterFaulted();
    }
    failure = ProcessorRunEntryFailure{
        .Deadline = deadlineFailure,
        .DeadlineReceipt = admission.Deadline,
        .Error = reached ? ProcessorRunError::DeadlineReached : ProcessorRunError::DeadlineRejected,
        .ActiveRunRetained = 1U,
        .Reserved = {},
    };
    return false;
}

bool ProcessorRunController::ValidateCooperativeAdmissionDeadline(const ProcessorRunAdmission& admission,
                                                                  ProcessorRunEntryFailure& failure) noexcept
{
    const Foundation::UInt64 observedTicks = m_localTimer.GetTicks();
    Time::ProcessorDeadlineError deadlineError{Time::ProcessorDeadlineError::None};
    if (observedTicks == MaximumUInt64)
    {
        deadlineError = Time::ProcessorDeadlineError::ClockExhausted;
    }
    else if ((observedTicks < admission.Deadline.ObservedTicks) ||
             (observedTicks < admission.Scheduler.Dispatch.StartTicks))
    {
        deadlineError = Time::ProcessorDeadlineError::ClockReversed;
    }
    else if (admission.Scheduler.StopAtTicks <= observedTicks)
    {
        deadlineError = Time::ProcessorDeadlineError::DeadlineReached;
    }
    else
    {
        return true;
    }

    const bool reached = deadlineError == Time::ProcessorDeadlineError::DeadlineReached;
    if (!reached)
    {
        EnterFaulted();
    }
    failure = ProcessorRunEntryFailure{
        .Deadline =
            Time::ProcessorDeadlineFailure{
                .SelectedDeadlineTicks = admission.Scheduler.StopAtTicks,
                .ObservedTicks = observedTicks,
                .Error = deadlineError,
                .HardwareError = Hal::Time::LocalTimerError::None,
                .AffectedSources = static_cast<Foundation::UInt8>(Time::ProcessorDeadlineSource::RunStop),
                .Reserved = {},
            },
        .DeadlineReceipt = admission.Deadline,
        .Error = reached ? ProcessorRunError::DeadlineReached : ProcessorRunError::DeadlineRejected,
        .ActiveRunRetained = 1U,
        .Reserved = {},
    };
    return false;
}

bool ProcessorRunController::ActiveAdmissionMatches(const ProcessorRunAdmission& admission,
                                                    bool& retained) const noexcept
{
    const ProcessorSchedulerSnapshot scheduler = m_scheduler.GetSnapshot();
    retained = scheduler.ActiveThread.IsValid();
    return (scheduler.State == ProcessorSchedulerState::Active) && scheduler.Processor.IsValid() &&
           (scheduler.Processor.GetValue() == m_processor.GetValue()) && retained &&
           scheduler.ActiveThread.IsEqualTo(m_activeAdmission.Scheduler.Dispatch.Context.Thread) &&
           scheduler.ActiveContext.IsValid() &&
           scheduler.ActiveContext.IsEqualTo(m_activeAdmission.Scheduler.Context) &&
           AdmissionsMatch(m_activeAdmission, admission);
}

bool ProcessorRunController::AdmissionsMatch(const ProcessorRunAdmission& expected,
                                             const ProcessorRunAdmission& candidate) noexcept
{
    bool reservedMatches{true};
    for (Foundation::Size index{0U}; index < sizeof(expected.Scheduler.Dispatch.Thread.Reserved); ++index)
    {
        reservedMatches = reservedMatches && (expected.Scheduler.Dispatch.Thread.Reserved[index] ==
                                              candidate.Scheduler.Dispatch.Thread.Reserved[index]);
    }
    for (Foundation::Size index{0U}; index < sizeof(expected.Deadline.Reserved); ++index)
    {
        reservedMatches = reservedMatches && (expected.Deadline.Reserved[index] == candidate.Deadline.Reserved[index]);
    }
    for (Foundation::Size index{0U}; index < sizeof(expected.Reserved); ++index)
    {
        reservedMatches = reservedMatches && (expected.Reserved[index] == candidate.Reserved[index]);
    }

    return expected.Scheduler.Dispatch.Context.Thread.IsEqualTo(candidate.Scheduler.Dispatch.Context.Thread) &&
           (expected.Scheduler.Dispatch.Context.MaximumRunTicks ==
            candidate.Scheduler.Dispatch.Context.MaximumRunTicks) &&
           (expected.Scheduler.Dispatch.Context.Weight == candidate.Scheduler.Dispatch.Context.Weight) &&
           (expected.Scheduler.Dispatch.Context.Priority == candidate.Scheduler.Dispatch.Context.Priority) &&
           (expected.Scheduler.Dispatch.Context.Criticality == candidate.Scheduler.Dispatch.Context.Criticality) &&
           (expected.Scheduler.Dispatch.Context.Class == candidate.Scheduler.Dispatch.Context.Class) &&
           (expected.Scheduler.Dispatch.Context.Processor.GetValue() ==
            candidate.Scheduler.Dispatch.Context.Processor.GetValue()) &&
           (expected.Scheduler.Dispatch.Context.Reserved == candidate.Scheduler.Dispatch.Context.Reserved) &&
           expected.Scheduler.Dispatch.Thread.BlockToken.IsEqualTo(candidate.Scheduler.Dispatch.Thread.BlockToken) &&
           (expected.Scheduler.Dispatch.Thread.RunSequence == candidate.Scheduler.Dispatch.Thread.RunSequence) &&
           (expected.Scheduler.Dispatch.Thread.ExitCode == candidate.Scheduler.Dispatch.Thread.ExitCode) &&
           (expected.Scheduler.Dispatch.Thread.State == candidate.Scheduler.Dispatch.Thread.State) &&
           (expected.Scheduler.Dispatch.Thread.RunningProcessor.GetValue() ==
            candidate.Scheduler.Dispatch.Thread.RunningProcessor.GetValue()) &&
           (expected.Scheduler.Dispatch.StartTicks == candidate.Scheduler.Dispatch.StartTicks) &&
           expected.Scheduler.Context.IsEqualTo(candidate.Scheduler.Context) &&
           (expected.Scheduler.ContextStorage == candidate.Scheduler.ContextStorage) &&
           (expected.Scheduler.ThreadStorage == candidate.Scheduler.ThreadStorage) &&
           (expected.Scheduler.StopAtTicks == candidate.Scheduler.StopAtTicks) &&
           (expected.Scheduler.SchedulerRetryAtTicks == candidate.Scheduler.SchedulerRetryAtTicks) &&
           (expected.Deadline.SelectedDeadlineTicks == candidate.Deadline.SelectedDeadlineTicks) &&
           (expected.Deadline.ObservedTicks == candidate.Deadline.ObservedTicks) &&
           (expected.Deadline.State == candidate.Deadline.State) &&
           (expected.Deadline.SelectedSources == candidate.Deadline.SelectedSources) &&
           (expected.Deadline.Reprogrammed == candidate.Deadline.Reprogrammed) && (expected.Mode == candidate.Mode) &&
           reservedMatches;
}

Time::ProcessorDeadlineReceipt
ProcessorRunController::MakeCurrentDeadlineReceipt(const Time::ProcessorDeadlineSnapshot& snapshot,
                                                   const Foundation::UInt64 observedTicks) noexcept
{
    return Time::ProcessorDeadlineReceipt{
        .SelectedDeadlineTicks = snapshot.SelectedDeadlineTicks,
        .ObservedTicks = observedTicks,
        .State = snapshot.State,
        .SelectedSources = snapshot.SelectedSources,
        .Reprogrammed = 0U,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    };
}

bool ProcessorRunController::FinishActiveAdmissionRollback(Time::ProcessorDeadlineReceipt& retainedReceipt,
                                                           Foundation::UInt64& schedulerRetryTicks,
                                                           ProcessorRunFailure& failure) noexcept
{
    retainedReceipt = m_activeAdmission.Deadline;
    schedulerRetryTicks = m_activeAdmission.Scheduler.SchedulerRetryAtTicks;
    const auto completion = m_scheduler.Finish(ThreadRunDisposition::Ready);
    if (!completion.HasValue())
    {
        failure =
            MakeFailure(ProcessorRunError::CompletionRejected, completion.GetErrorReference(), m_timerQueue.GetState(),
                        {}, retainedReceipt, m_scheduler.GetSnapshot().ActiveThread.IsValid());
        return false;
    }

    ClearActiveAdmission();
    m_state = ProcessorRunState::Idle;
    return true;
}

ProcessorRunFailure
ProcessorRunController::MakeRollbackDeadlineFailure(const Time::TimerQueueState& timerQueue,
                                                    const Time::ProcessorDeadlineFailure& deadlineFailure) noexcept
{
    const bool reached = deadlineFailure.Error == Time::ProcessorDeadlineError::DeadlineReached;
    if (!reached)
    {
        EnterFaulted();
    }
    return MakeFailure(reached ? ProcessorRunError::DeadlineReached : ProcessorRunError::DeadlineRejected, {},
                       timerQueue, deadlineFailure,
                       MakeCurrentDeadlineReceipt(m_deadline.GetSnapshot(), deadlineFailure.ObservedTicks), false);
}

Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>
ProcessorRunController::FinishHardwareAdmissionRollback(const Foundation::UInt64 schedulerRetryTicks) noexcept
{
    using ResultType = Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>;

    const Time::TimerQueueState timerQueue = m_timerQueue.GetState();
    const auto deadline = ProgramDeadline(0U, schedulerRetryTicks, timerQueue);
    if (deadline.HasValue())
    {
        return ResultType::Success(deadline.GetValue());
    }
    return ResultType::Failure(MakeRollbackDeadlineFailure(timerQueue, deadline.GetErrorReference()));
}

Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>
ProcessorRunController::RollbackActiveAdmission() noexcept
{
    using ResultType = Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>;

    if (m_state == ProcessorRunState::Faulted)
    {
        return ResultType::Failure(MakeStateFailure(ProcessorRunError::Faulted));
    }
    if (m_state != ProcessorRunState::Active)
    {
        return ResultType::Failure(MakeStateFailure(ProcessorRunError::NotActive));
    }

    Time::ProcessorDeadlineReceipt retainedReceipt{};
    Foundation::UInt64 schedulerRetryTicks{0U};
    ProcessorRunFailure failure{};
    if (!FinishActiveAdmissionRollback(retainedReceipt, schedulerRetryTicks, failure))
    {
        return ResultType::Failure(failure);
    }

    if (m_mode == ProcessorRunDispatchMode::CooperativeBootstrap)
    {
        return ResultType::Success(retainedReceipt);
    }
    return FinishHardwareAdmissionRollback(schedulerRetryTicks);
}

Foundation::Result<DispatchCompletion, ProcessorRunFailure>
ProcessorRunController::Finish(const ThreadRunDisposition disposition, const Foundation::UInt64 exitCode) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, ProcessorRunFailure>;

    if (m_state == ProcessorRunState::Faulted)
    {
        return ResultType::Failure(MakeStateFailure(ProcessorRunError::Faulted));
    }
    if (m_state != ProcessorRunState::Active)
    {
        return ResultType::Failure(MakeStateFailure(ProcessorRunError::NotActive));
    }

    const auto result = m_scheduler.Finish(disposition, exitCode);
    if (!result.HasValue())
    {
        return FinishRejectedRun(result.GetError());
    }

    ClearActiveAdmission();
    m_state = ProcessorRunState::Idle;
    return ResultType::Success(result.GetValue());
}

Foundation::Result<DispatchCompletion, ProcessorRunFailure> ProcessorRunController::FinishExternallyStoppedRunAsReady(
    const ProcessorRunExternalStopObservation& observation) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, ProcessorRunFailure>;

    DispatchCompletion completion{};
    ProcessorRunFailure failure{};
    if (!FinishExternalStopTransaction(observation, completion, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(completion);
}

bool ProcessorRunController::FinishExternalStopTransaction(const ProcessorRunExternalStopObservation& observation,
                                                           DispatchCompletion& completion,
                                                           ProcessorRunFailure& failure) noexcept
{
    Time::TimerQueueState timerQueue{};
    if (!ValidateExternalStopObservation(observation, timerQueue, failure))
    {
        return false;
    }

    Time::ProcessorDeadlineReceipt consumedDeadline{};
    if (!ConsumeExternalStopDeadline(observation, timerQueue, consumedDeadline, failure))
    {
        return false;
    }
    if (!CompleteExternalStopScheduler(observation, timerQueue, consumedDeadline, completion, failure))
    {
        return false;
    }

    ClearActiveAdmission();
    m_state = ProcessorRunState::Idle;
    return true;
}

bool ProcessorRunController::ConsumeExternalStopDeadline(const ProcessorRunExternalStopObservation& observation,
                                                         const Time::TimerQueueState& timerQueue,
                                                         Time::ProcessorDeadlineReceipt& consumedDeadline,
                                                         ProcessorRunFailure& failure) noexcept
{
    const Time::ProcessorDeadlineReceipt deadline = m_activeAdmission.Deadline;
    const auto consumed = m_deadline.ConsumeExternallyMaskedExpiration(deadline, observation.ObservedTicks);
    if (!consumed.HasValue())
    {
        EnterFaulted();
        failure = MakeFailure(ProcessorRunError::ExternalStopRejected, {}, timerQueue, consumed.GetErrorReference(),
                              deadline, true, ProcessorRunExternalStopError::DeadlineConsumptionRejected);
        return false;
    }

    consumedDeadline = consumed.GetValue();
    return true;
}

bool ProcessorRunController::CompleteExternalStopScheduler(const ProcessorRunExternalStopObservation& observation,
                                                           const Time::TimerQueueState& timerQueue,
                                                           const Time::ProcessorDeadlineReceipt& consumedDeadline,
                                                           DispatchCompletion& completion,
                                                           ProcessorRunFailure& failure) noexcept
{
    const auto completed = m_scheduler.FinishExternallyStoppedAsReady(observation.ObservedTicks);
    if (!completed.HasValue())
    {
        const bool retained = m_scheduler.GetSnapshot().ActiveThread.IsValid();
        EnterFaulted();
        failure = MakeFailure(ProcessorRunError::ExternalStopRejected, completed.GetErrorReference(), timerQueue, {},
                              consumedDeadline, retained, ProcessorRunExternalStopError::SchedulerCompletionRejected);
        return false;
    }

    completion = completed.GetValue();
    return true;
}

bool ProcessorRunController::ValidateExternalStopObservation(const ProcessorRunExternalStopObservation& observation,
                                                             Time::TimerQueueState& timerQueue,
                                                             ProcessorRunFailure& failure) noexcept
{
    if (m_state == ProcessorRunState::Faulted)
    {
        failure = MakeStateFailure(ProcessorRunError::Faulted);
        return false;
    }
    if (m_state != ProcessorRunState::Active)
    {
        failure = MakeStateFailure(ProcessorRunError::NotActive);
        return false;
    }

    timerQueue = m_timerQueue.GetState();
    const Time::ProcessorDeadlineSnapshot deadlineSnapshot = m_deadline.GetSnapshot();
    const ProcessorRunExternalStopError configurationError =
        ((m_mode != ProcessorRunDispatchMode::HardwareDeadline) ||
         (m_activeAdmission.Mode != ProcessorRunDispatchMode::HardwareDeadline))
            ? ProcessorRunExternalStopError::ModeMismatch
        : (observation.ExternalFrameToken == 0U) || !BytesAreZero(observation.Reserved, sizeof(observation.Reserved))
            ? ProcessorRunExternalStopError::InvalidObservation
            : ProcessorRunExternalStopError::None;
    if (configurationError != ProcessorRunExternalStopError::None)
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot, configurationError, failure);
        return false;
    }

    const auto binding = GetActiveExecutionBinding();
    if (!binding.HasValue())
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot,
                                   ProcessorRunExternalStopError::AdmissionMismatch, failure);
        return false;
    }
    const ProcessorActiveExecutionBinding& active = binding.GetValue();
    if (!observation.Thread.IsValid() || !observation.Thread.IsEqualTo(active.ThreadIdentity) ||
        !observation.SchedulingContext.IsValid() ||
        !observation.SchedulingContext.IsEqualTo(active.SchedulingContextIdentity) || (observation.RunSequence == 0U) ||
        (observation.RunSequence != active.RunSequence) || !observation.Processor.IsValid() ||
        (observation.Processor.GetValue() != active.Processor.GetValue()) ||
        (observation.Processor.GetValue() != m_processor.GetValue()))
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot,
                                   ProcessorRunExternalStopError::AdmissionMismatch, failure);
        return false;
    }

    if ((timerQueue.NextDeadlineTicks != 0U) || (timerQueue.ScheduledEntries != 0U) ||
        (timerQueue.PendingEntries != 0U) || (timerQueue.ClaimedEntries != 0U) ||
        !BytesAreZero(timerQueue.Reserved, sizeof(timerQueue.Reserved)))
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot,
                                   ProcessorRunExternalStopError::TimerQueueNotEmpty, failure);
        return false;
    }

    const Time::ProcessorDeadlineReceipt& deadline = m_activeAdmission.Deadline;
    if ((m_activeAdmission.Scheduler.SchedulerRetryAtTicks != 0U) ||
        (deadline.State != Time::ProcessorDeadlineState::Armed) || (deadline.SelectedDeadlineTicks == 0U) ||
        (deadline.SelectedDeadlineTicks == MaximumUInt64) || (deadline.SelectedSources != RunStopDeadlineSource) ||
        (deadline.SelectedDeadlineTicks != m_activeAdmission.Scheduler.StopAtTicks) ||
        (deadline.SelectedDeadlineTicks != observation.ProgrammedDeadlineTicks) ||
        (deadline.ObservedTicks >= deadline.SelectedDeadlineTicks) || (deadline.Reprogrammed > 1U) ||
        !BytesAreZero(deadline.Reserved, sizeof(deadline.Reserved)) ||
        (deadlineSnapshot.State != Time::ProcessorDeadlineState::Armed) ||
        (deadlineSnapshot.SelectedDeadlineTicks != deadline.SelectedDeadlineTicks) ||
        (deadlineSnapshot.SelectedSources != RunStopDeadlineSource))
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot,
                                   ProcessorRunExternalStopError::DeadlineMismatch, failure);
        return false;
    }
    if (observation.ObservedTicks == MaximumUInt64)
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot,
                                   ProcessorRunExternalStopError::InvalidObservation, failure);
        return false;
    }
    if ((observation.ObservedTicks < deadline.SelectedDeadlineTicks) ||
        (observation.ObservedTicks < deadline.ObservedTicks) ||
        (observation.ObservedTicks < m_activeAdmission.Scheduler.Dispatch.StartTicks))
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot,
                                   ProcessorRunExternalStopError::CapturedTimeBeforeDeadline, failure);
        return false;
    }
    if (observation.HardwareState != ProcessorRunExternalStopHardwareState::TimerMaskedReadBackAndInterruptAcknowledged)
    {
        RejectExternalStopProtocol(observation, timerQueue, deadlineSnapshot,
                                   ProcessorRunExternalStopError::HardwareEvidenceMissing, failure);
        return false;
    }
    return true;
}

void ProcessorRunController::RejectExternalStopProtocol(const ProcessorRunExternalStopObservation& observation,
                                                        const Time::TimerQueueState& timerQueue,
                                                        const Time::ProcessorDeadlineSnapshot& deadlineSnapshot,
                                                        const ProcessorRunExternalStopError externalStop,
                                                        ProcessorRunFailure& failure) noexcept
{
    const bool retained = m_scheduler.GetSnapshot().ActiveThread.IsValid();
    EnterFaulted();
    failure = MakeFailure(ProcessorRunError::ExternalStopRejected, {}, timerQueue,
                          Time::ProcessorDeadlineFailure{
                              .SelectedDeadlineTicks = deadlineSnapshot.SelectedDeadlineTicks,
                              .ObservedTicks = observation.ObservedTicks,
                              .Error = Time::ProcessorDeadlineError::InvalidDeadline,
                              .HardwareError = Hal::Time::LocalTimerError::None,
                              .AffectedSources = deadlineSnapshot.SelectedSources,
                              .Reserved = {0U, 0U, 0U, 0U, 0U},
                          },
                          m_activeAdmission.Deadline, retained, externalStop);
}

ProcessorRunSnapshot ProcessorRunController::GetSnapshot() const noexcept
{
    return ProcessorRunSnapshot{
        .Scheduler = m_scheduler.GetSnapshot(),
        .TimerQueue = m_timerQueue.GetState(),
        .Deadline = m_deadline.GetSnapshot(),
        .State = m_state,
        .Mode = m_mode,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace Asteria::Kernel::Scheduler
