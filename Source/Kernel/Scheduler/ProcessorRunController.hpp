// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_PROCESSOR_RUN_CONTROLLER_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_PROCESSOR_RUN_CONTROLLER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Time/ILocalTimer.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"
#include "Kernel/Time/ProcessorDeadlineController.hpp"
#include "Kernel/Time/TimerQueue.hpp"

namespace Asteria::Kernel::Scheduler
{
enum class ProcessorRunState : Foundation::UInt8
{
    Idle,
    Active,
    Faulted,
};

enum class ProcessorRunDispatchMode : Foundation::UInt8
{
    HardwareDeadline,
    CooperativeBootstrap,
};

enum class ProcessorRunExternalStopHardwareState : Foundation::UInt8
{
    Unverified,
    TimerMaskedReadBackAndInterruptAcknowledged,
    SynchronousPageFaultCaptured,
};

enum class ProcessorRunExternalStopError : Foundation::UInt8
{
    None,
    InvalidObservation,
    ModeMismatch,
    AdmissionMismatch,
    TimerQueueNotEmpty,
    DeadlineMismatch,
    CapturedTimeBeforeDeadline,
    HardwareEvidenceMissing,
    DeadlineConsumptionRejected,
    SchedulerCompletionRejected,
};

enum class ProcessorRunError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyActive,
    NotActive,
    Faulted,
    TimerWorkPending,
    NoRunnableThread,
    SchedulerRejected,
    DeadlineRefreshRequired,
    DeadlineReached,
    DeadlineRejected,
    CompletionRejected,
    ActiveAdmissionMismatch,
    DeferredWorkPending,
    ExternalStopRejected,
};

struct ProcessorRunFailure
{
    ProcessorSchedulerFailure Scheduler;
    Time::TimerQueueState TimerQueue;
    Time::ProcessorDeadlineFailure Deadline;
    Time::ProcessorDeadlineReceipt DeadlineReceipt;
    ProcessorRunError Error;
    /// One when the scheduler still owns the exact active claim, including
    /// after a terminal fault; zero when no run lifetime is retained.
    Foundation::UInt8 ActiveRunRetained;
    ProcessorRunExternalStopError ExternalStop;
    Foundation::UInt8 Reserved[5];
};

struct ProcessorRunExternalStopObservation
{
    ThreadId Thread;
    SchedulingContextId SchedulingContext;
    Foundation::UInt64 RunSequence;
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    /// Opaque nonzero epoch owned by the target's retained frame slot. It is
    /// transported unchanged through common scheduling and consumed exactly
    /// once by the architecture resume continuation.
    Foundation::UInt64 ExternalFrameToken;
    CpuId Processor;
    ProcessorRunExternalStopHardwareState HardwareState;
    Foundation::UInt8 Reserved[6];
};

/// Compact diagnostic for the final read-only admission validation. Scheduler
/// and TimerQueue state are unchanged by this check, so their larger nested
/// snapshots are intentionally omitted from this stack-sensitive boundary.
struct ProcessorRunEntryFailure
{
    Time::ProcessorDeadlineFailure Deadline;
    Time::ProcessorDeadlineReceipt DeadlineReceipt;
    ProcessorRunError Error;
    Foundation::UInt8 ActiveRunRetained;
    Foundation::UInt8 Reserved[6];
};

/// One scheduler admission tagged with the controller's immutable dispatch
/// mode. HardwareDeadline carries the installed processor-local deadline;
/// CooperativeBootstrap carries an explicitly Disarmed receipt.
struct ProcessorRunAdmission
{
    ProcessorSchedulerAdmission Scheduler;
    Time::ProcessorDeadlineReceipt Deadline;
    ProcessorRunDispatchMode Mode;
    Foundation::UInt8 Reserved[7];
};

struct ProcessorRunSnapshot
{
    ProcessorSchedulerSnapshot Scheduler;
    Time::TimerQueueState TimerQueue;
    Time::ProcessorDeadlineSnapshot Deadline;
    ProcessorRunState State;
    ProcessorRunDispatchMode Mode;
    Foundation::UInt8 Reserved[6];
};

/// Joins one processor's scheduler, TimerQueue boundary, and local one-shot deadline into the final
/// architecture-neutral run-admission transaction. The owning CPU serializes every operation with
/// local interrupts disabled. The TimerQueue uses the exact localTimer clock instance and all
/// mutations of that queue are in the same external serialization domain. Operations may spin in
/// the underlying registry, queue, and scheduler locks and are not callable from interrupt context.
class ProcessorRunController final
{
public:
    /// Preconditions:
    /// - objectRegistry, timerQueue, and localTimer outlive this controller;
    /// - timerQueue was constructed with localTimer as its clock;
    /// - processor identifies the CPU that owns localTimer and this policy
    /// path;
    /// - CooperativeBootstrap is used only for a voluntarily finishing BSP
    /// bootstrap worker while timer and deferred work remain empty.
    ProcessorRunController(Object::ObjectRegistry& objectRegistry, Time::TimerQueue& timerQueue,
                           Hal::Time::ILocalTimer& localTimer, CpuId processor,
                           ProcessorRunDispatchMode mode = ProcessorRunDispatchMode::HardwareDeadline) noexcept
        : m_scheduler{objectRegistry, localTimer, processor}, m_timerQueue{timerQueue}, m_deadline{localTimer},
          m_localTimer{localTimer}, m_processor{processor}, m_mode{mode},
          m_state{processor.IsValid() && IsValidMode(mode) ? ProcessorRunState::Idle : ProcessorRunState::Faulted}
    {
    }

    /// Preconditions:
    /// - State is Idle, or this is the initial configuration-fault state;
    /// - the deadline controller is Disarmed;
    /// - no operation or retained admission remains.
    ~ProcessorRunController() noexcept = default;

    ProcessorRunController(const ProcessorRunController&) = delete;
    ProcessorRunController& operator=(const ProcessorRunController&) = delete;
    ProcessorRunController(ProcessorRunController&&) = delete;
    ProcessorRunController& operator=(ProcessorRunController&&) = delete;

    /// Standalone placement operation. An owner that composes this controller
    /// into ProcessorKernelRunLoop must instead use EnqueueBoundThread so the
    /// exact identity is retained for frame-safe removal.
    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReceipt, ProcessorRunFailure>
    Enqueue(SchedulingContextId contextId, SchedulingContext& context, ThreadId threadId, Thread& thread) noexcept;

    /// Standalone removal operation. A composed ProcessorKernelRunLoop owner
    /// must use RemoveBoundThread; calling this directly could bypass retained
    /// external-frame ownership.
    [[nodiscard]] Foundation::Result<ReadyQueueState, ProcessorRunFailure> Remove(ThreadId thread) noexcept;

    /// In HardwareDeadline mode, selects one thread and installs the minimum of its run-stop
    /// boundary, TimerQueue deadline, and skipped-candidate retry before publishing the admission.
    /// NoRunnableThread is a nonterminal failure whose DeadlineReceipt describes the installed idle
    /// wakeup. In CooperativeBootstrap mode, admits only work with no TimerQueue or deferred
    /// scheduler boundary. It publishes an explicitly Disarmed deadline receipt and performs no
    /// local-timer program or cancellation.
    [[nodiscard]] Foundation::Result<ProcessorRunAdmission, ProcessorRunFailure> DispatchNext() noexcept;

    /// Reconciles the processor-local hardware boundary with its current sources after TimerQueue
    /// work changed or a local-timer interrupt was acknowledged. While Idle, only TimerQueueTicks
    /// is supplied; run-stop and scheduler-retry sources are zero. Pending or Claimed delivery work
    /// is rejected without reading or changing the local timer. While a HardwareDeadline run is
    /// Active, the minimum of its run stop, the TimerQueue deadline, and its scheduler retry is
    /// installed, so a TimerQueue or retry boundary that fires before the run stop does not leave
    /// the run unbounded. A retry that has already fired is omitted; its candidate is considered at
    /// the next dispatch. A reached run stop or TimerQueue deadline is the nonterminal
    /// DeadlineReached, reported without a hardware write so the caller completes the run or drains
    /// the queue first. Pending or Claimed delivery work does not prevent the refresh. The
    /// installed receipt replaces the active admission's deadline, which a later external stop
    /// validates. A CooperativeBootstrap run owns no hardware boundary and is rejected as
    /// AlreadyActive. Preconditions: - The owning CPU provides the controller's usual serialization
    /// domain; - an Active admission has already been consumed by the architecture: its context was
    /// entered, or its external frame resume committed; - this is normal context, not an ISR.
    [[nodiscard]] Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>
    RefreshTimerQueueDeadline(Foundation::UInt64 minimumObservedTicks = 0U) noexcept;

    /// Validates the exact controller-issued admission and retained scheduler
    /// storage identities without reading or changing the local timer. Run-loop
    /// code calls this before using either storage pointer for binding or
    /// native context selection.
    [[nodiscard]] bool ValidateActiveAdmissionIdentity(const ProcessorRunAdmission& admission,
                                                       ProcessorRunEntryFailure& failure) noexcept;

    /// Revalidates the exact active admission after architecture preparation.
    /// HardwareDeadline also validates the installed deadline; its successful
    /// clock sample is the architecture-neutral admission commit point. Exact
    /// later instruction entry is owned by the target timer and interrupt
    /// handoff. DeadlineReached leaves the active claim retained for an
    /// ordered root restore followed by Finish(Ready). Every other failure is
    /// terminal. CooperativeBootstrap validates the mode-tagged scheduler
    /// claim and exact Disarmed receipt, then reads the timer once to reject
    /// entry at or after StopAtTicks. It does not program or cancel the timer.
    [[nodiscard]] bool ValidateActiveAdmission(const ProcessorRunAdmission& admission,
                                               ProcessorRunEntryFailure& failure) noexcept;

    /// Releases an admission that could not enter its native context. Hardware
    /// mode removes the rejected run-stop source and returns the deadline that
    /// remains installed for TimerQueue or scheduler-retry work. Cooperative
    /// mode returns its retained Disarmed receipt without touching the timer.
    [[nodiscard]] Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>
    RollbackActiveAdmission() noexcept;

    /// Accounts the active run. After HardwareDeadline success, local
    /// interrupts remain disabled and the caller immediately calls
    /// DispatchNext or otherwise reprograms the processor deadline before
    /// returning to ordinary work. A CooperativeBootstrap worker must finish
    /// voluntarily because this controller provides no interrupt boundary.
    [[nodiscard]] Foundation::Result<DispatchCompletion, ProcessorRunFailure>
    Finish(ThreadRunDisposition disposition, Foundation::UInt64 exitCode = 0U) noexcept;

    /// Consumes an exact one-shot HardwareDeadline stop and completes its
    /// active scheduler run as Ready at the captured architectural tick. This
    /// external-frame-only API accepts no voluntary disposition and performs
    /// no clock read. The TimerQueue and scheduler-retry sources must be empty,
    /// and the installed deadline source must be exactly RunStop. Every
    /// protocol mismatch is terminal while retaining the active scheduler
    /// admission for diagnosis; no rollback or retry path is entered.
    [[nodiscard]] Foundation::Result<DispatchCompletion, ProcessorRunFailure>
    FinishExternallyStoppedRunAsReady(const ProcessorRunExternalStopObservation& observation) noexcept;

    [[nodiscard]] ProcessorRunSnapshot GetSnapshot() const noexcept;

    /// Returns the exact active Thread/SchedulingContext/CPU/RunSequence
    /// binding retained by this controller. The scheduler revalidates its
    /// claim and both live objects before either storage pointer is published.
    [[nodiscard]] Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>
    GetActiveExecutionBinding() noexcept;

    /// Narrow wake authority for this controller's scheduler-owned queue.
    [[nodiscard]] constexpr ProcessorSchedulerWakeFacade& GetWakeFacade() noexcept
    {
        return m_scheduler.GetWakeFacade();
    }

    [[nodiscard]] constexpr CpuId GetProcessor() const noexcept
    {
        return m_processor;
    }

private:
    [[nodiscard]] static constexpr bool IsValidMode(const ProcessorRunDispatchMode mode) noexcept
    {
        return (mode == ProcessorRunDispatchMode::HardwareDeadline) ||
               (mode == ProcessorRunDispatchMode::CooperativeBootstrap);
    }

    [[nodiscard]] static ProcessorRunFailure
    MakeFailure(ProcessorRunError error, const ProcessorSchedulerFailure& scheduler = {},
                const Time::TimerQueueState& timerQueue = {}, const Time::ProcessorDeadlineFailure& deadline = {},
                const Time::ProcessorDeadlineReceipt& deadlineReceipt = {}, bool activeRunRetained = false,
                ProcessorRunExternalStopError externalStop = ProcessorRunExternalStopError::None) noexcept;
    [[nodiscard]] Foundation::Result<Time::ProcessorDeadlineReceipt, Time::ProcessorDeadlineFailure>
    ProgramDeadline(Foundation::UInt64 runStopTicks, Foundation::UInt64 schedulerRetryTicks,
                    const Time::TimerQueueState& timerQueue, Foundation::UInt64 minimumObservedTicks = 0U) noexcept;
    [[nodiscard]] ProcessorRunFailure MakeStateFailure(ProcessorRunError error) const noexcept;
    [[nodiscard]] ProcessorRunFailure
    MakeSchedulerOperationFailure(const ProcessorSchedulerFailure& schedulerFailure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool RefreshTimerQueueDeadlineTransaction(Foundation::UInt64 minimumObservedTicks,
                                                                              Time::ProcessorDeadlineReceipt& receipt,
                                                                              ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool RefreshActiveDeadlineTransaction(Foundation::UInt64 minimumObservedTicks,
                                                                          Time::ProcessorDeadlineReceipt& receipt,
                                                                          ProcessorRunFailure& failure) noexcept;
    [[nodiscard]] bool DispatchNextTransaction(ProcessorRunAdmission& admission, ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool DispatchNextWithHardwareDeadline(const Time::TimerQueueState& timerQueue,
                                                                          ProcessorRunAdmission& admission,
                                                                          ProcessorRunFailure& failure) noexcept;
    [[nodiscard]] bool ValidateCooperativeDispatchDeadline(const Time::TimerQueueState& timerQueue,
                                                           ProcessorRunFailure& failure) noexcept;
    [[nodiscard]] bool DispatchNextCooperatively(const Time::TimerQueueState& timerQueue,
                                                 ProcessorRunAdmission& admission,
                                                 ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] void FinishRejectedCooperativeDispatch(const ProcessorSchedulerFailure& schedulerFailure,
                                                             const Time::TimerQueueState& timerQueue,
                                                             ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] void FinishCooperativeDispatch(const ProcessorSchedulerAdmission& schedulerAdmission,
                                                     ProcessorRunAdmission& admission) noexcept;
    void FinishDeferredDispatch(const ProcessorSchedulerFailure& schedulerFailure,
                                const Time::TimerQueueState& timerQueue, ProcessorRunFailure& failure) noexcept;
    [[nodiscard]] bool FinishAdmittedDispatch(const ProcessorSchedulerAdmission& schedulerAdmission,
                                              const Time::TimerQueueState& timerQueue, ProcessorRunAdmission& admission,
                                              ProcessorRunFailure& failure) noexcept;
    [[nodiscard]] bool ActiveAdmissionMatches(const ProcessorRunAdmission& admission, bool& retained) const noexcept;
    [[gnu::noinline]] [[nodiscard]] bool
    FinishExternalStopTransaction(const ProcessorRunExternalStopObservation& observation,
                                  DispatchCompletion& completion, ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool ConsumeExternalStopDeadline(
        const ProcessorRunExternalStopObservation& observation, const Time::TimerQueueState& timerQueue,
        Time::ProcessorDeadlineReceipt& consumedDeadline, ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool
    CompleteExternalStopScheduler(const ProcessorRunExternalStopObservation& observation,
                                  const Time::TimerQueueState& timerQueue,
                                  const Time::ProcessorDeadlineReceipt& consumedDeadline,
                                  DispatchCompletion& completion, ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool
    ValidateExternalStopObservation(const ProcessorRunExternalStopObservation& observation,
                                    Time::TimerQueueState& timerQueue, ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] void RejectExternalStopProtocol(const ProcessorRunExternalStopObservation& observation,
                                                      const Time::TimerQueueState& timerQueue,
                                                      const Time::ProcessorDeadlineSnapshot& deadlineSnapshot,
                                                      ProcessorRunExternalStopError externalStop,
                                                      ProcessorRunFailure& failure) noexcept;
    [[nodiscard]] static bool AdmissionsMatch(const ProcessorRunAdmission& expected,
                                              const ProcessorRunAdmission& candidate) noexcept;
    [[nodiscard]] static Time::ProcessorDeadlineReceipt
    MakeCurrentDeadlineReceipt(const Time::ProcessorDeadlineSnapshot& snapshot,
                               Foundation::UInt64 observedTicks) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool
    ValidateCooperativeAdmissionDeadline(const ProcessorRunAdmission& admission,
                                         ProcessorRunEntryFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool FinishActiveAdmissionRollback(Time::ProcessorDeadlineReceipt& retainedReceipt,
                                                                       Foundation::UInt64& schedulerRetryTicks,
                                                                       ProcessorRunFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] Foundation::Result<Time::ProcessorDeadlineReceipt, ProcessorRunFailure>
    FinishHardwareAdmissionRollback(Foundation::UInt64 schedulerRetryTicks) noexcept;
    [[gnu::noinline]] [[nodiscard]] ProcessorRunFailure
    MakeRollbackDeadlineFailure(const Time::TimerQueueState& timerQueue,
                                const Time::ProcessorDeadlineFailure& deadlineFailure) noexcept;
    void ClearActiveAdmission() noexcept;
    void FinishRejectedAdmission(const Time::TimerQueueState& timerQueue,
                                 const Time::ProcessorDeadlineFailure& deadlineFailure,
                                 ProcessorRunFailure& failure) noexcept;
    void FinishRejectedAdmissionCompletion(const ProcessorSchedulerFailure& schedulerFailure,
                                           const Time::TimerQueueState& timerQueue,
                                           const Time::ProcessorDeadlineFailure& deadlineFailure,
                                           ProcessorRunFailure& failure) noexcept;
    [[nodiscard]] Foundation::Result<DispatchCompletion, ProcessorRunFailure>
    FinishRejectedRun(const ProcessorSchedulerFailure& schedulerFailure) noexcept;
    void EnterFaulted() noexcept;

    ProcessorScheduler m_scheduler;
    Time::TimerQueue& m_timerQueue;
    Time::ProcessorDeadlineController m_deadline;
    Hal::Time::ILocalTimer& m_localTimer;
    ProcessorRunAdmission m_activeAdmission{};
    CpuId m_processor{};
    ProcessorRunDispatchMode m_mode{ProcessorRunDispatchMode::HardwareDeadline};
    ProcessorRunState m_state{ProcessorRunState::Faulted};
};

static_assert(sizeof(ProcessorRunState) == 1U);
static_assert(sizeof(ProcessorRunDispatchMode) == 1U);
static_assert(sizeof(ProcessorRunExternalStopHardwareState) == 1U);
static_assert(sizeof(ProcessorRunExternalStopError) == 1U);
static_assert(sizeof(ProcessorRunError) == 1U);
static_assert(sizeof(ProcessorRunExternalStopObservation) == 72U);
static_assert(alignof(ProcessorRunExternalStopObservation) == 8U);
static_assert(sizeof(ProcessorRunFailure) == 112U);
static_assert(alignof(ProcessorRunFailure) == 8U);
static_assert(sizeof(ProcessorRunEntryFailure) == 56U);
static_assert(alignof(ProcessorRunEntryFailure) == 8U);
static_assert(sizeof(ProcessorRunAdmission) == 160U);
static_assert(alignof(ProcessorRunAdmission) == 8U);
static_assert(sizeof(ProcessorRunSnapshot) == 104U);
static_assert(alignof(ProcessorRunSnapshot) == 8U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_PROCESSOR_RUN_CONTROLLER_HPP_INCLUDED
