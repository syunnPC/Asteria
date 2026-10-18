// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_PROCESSOR_SCHEDULER_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_PROCESSOR_SCHEDULER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/DispatchCoordinator.hpp"
#include "Kernel/Scheduler/ReadyQueue.hpp"
#include "Kernel/Time/IMonotonicClock.hpp"
#include "Platform/Synchronization/SpinLock.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

namespace Asteria::Kernel::Scheduler
{
enum class BlockedRunCommitOutcome : Foundation::UInt8
{
    Accepted,
    RetryableRejected,
    TerminalRejected,
};

enum class ProcessorSchedulerState : Foundation::UInt8
{
    Idle,
    Active,
    Faulted,
};

enum class ProcessorSchedulerError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyActive,
    NotActive,
    Faulted,
    NoRunnableThread,
    QueueRejected,
    DispatchRejected,
    RetryCalculationFailed,
    QueueTransitionFailed,
    InvariantViolation,
};

enum class ProcessorActiveExecutionBindingError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    NotActive,
    Faulted,
    AdmissionMismatch,
    ThreadStateMismatch,
    SchedulingContextStateMismatch,
    NativeContextMismatch,
    ProtectionDomainMismatch,
    AddressSpaceMismatch,
};

struct ProcessorSchedulerFailure
{
    Foundation::UInt64 RetryAtTicks;
    DispatchCoordinatorFailure Dispatch;
    ReadyQueueFailure Queue;
    ProcessorSchedulerError Error;
    ReadyQueueError QueueTransition;
    Foundation::UInt8 Reserved[2];
};

/// One immediately consumable run admission. ContextStorage and ThreadStorage
/// remain valid only while the originating scheduler retains this exact Active
/// transaction; callers do not retain or dereference them after Finish or
/// terminal fault handling.
struct ProcessorSchedulerAdmission
{
    DispatchAdmission Dispatch;
    SchedulingContextId Context;
    SchedulingContext* ContextStorage;
    Thread* ThreadStorage;
    Foundation::UInt64 StopAtTicks;
    /// Earliest replenishment of a higher-ranked candidate skipped during
    /// this selection. Zero means that no candidate was deferred.
    Foundation::UInt64 SchedulerRetryAtTicks;
};

struct ProcessorSchedulerSnapshot
{
    ReadyQueueState Queue;
    ThreadId ActiveThread;
    SchedulingContextId ActiveContext;
    ProcessorSchedulerState State;
    CpuId Processor;
    Foundation::UInt8 Reserved[6];
};

struct ProcessorActiveExecutionBinding
{
    ThreadId ThreadIdentity;
    Thread* ThreadStorage;
    SchedulingContextId SchedulingContextIdentity;
    SchedulingContext* SchedulingContextStorage;
    CpuId Processor;
    Foundation::UInt64 RunSequence;
};

struct ProcessorActiveExecutionBindingFailure
{
    ProcessorActiveExecutionBindingError Error;
    ThreadState Thread;
    SchedulingContextState SchedulingContext;
    CpuId Processor;
    Foundation::UInt8 Reserved[4];
};

class ProcessorScheduler;

/// Narrow wake authority for the exact ReadyQueue owned by one scheduler.
/// It exposes neither selection nor raw queue storage. The scheduler lock
/// serializes every reservation transition with dispatch and completion.
class ProcessorSchedulerWakeFacade final
{
public:
    ProcessorSchedulerWakeFacade(const ProcessorSchedulerWakeFacade&) = delete;
    ProcessorSchedulerWakeFacade& operator=(const ProcessorSchedulerWakeFacade&) = delete;
    ProcessorSchedulerWakeFacade(ProcessorSchedulerWakeFacade&&) = delete;
    ProcessorSchedulerWakeFacade& operator=(ProcessorSchedulerWakeFacade&&) = delete;

    // Immutable table of home facades.
    void BindHomes(ProcessorSchedulerWakeFacade* const* homes) noexcept
    {
        m_homes = homes;
    }
    [[nodiscard]] CpuId GetProcessor() const noexcept;
    // Correlations use the thread's exact home, even when this
    // facade was supplied by a boot-domain runtime.
    [[nodiscard]] CpuId GetHomeProcessor(const SchedulingContext& context) const noexcept
    {
        // A standalone facade also supports the common scheduler's broad
        // affinity contexts. Installed home tables require an exact home bit.
        if (m_homes == nullptr)
        {
            return GetProcessor();
        }
        const auto affinity = context.GetConfiguration().CpuAffinityMask;
        for (Foundation::Size home{0U}; home < MaximumProcessorCount; ++home)
        {
            if (affinity == (Foundation::ProcessorMask{1U} << home) && m_homes[home] != nullptr)
            {
                return CpuId{static_cast<Foundation::UInt8>(home)};
            }
        }
        return CpuId{};
    }

    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>
    ReserveBlockedEnqueue(SchedulingContextId contextId, SchedulingContext& context, ThreadId threadId, Thread& thread,
                          ThreadBlockToken blockToken) noexcept;

    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>
    CommitReservedEnqueue(ReadyQueueEnqueueReservation reservation) noexcept;

    [[nodiscard]] ReadyQueueError CancelReservedEnqueue(ReadyQueueEnqueueReservation reservation) noexcept;

private:
    friend class ProcessorScheduler;

    explicit constexpr ProcessorSchedulerWakeFacade(ProcessorScheduler& scheduler) noexcept : m_scheduler{scheduler}
    {
    }

    // Route once, then use a non-recursive local authority.
    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>
    ReserveLocalBlockedEnqueue(SchedulingContextId contextId, SchedulingContext& context, ThreadId threadId,
                               Thread& thread, ThreadBlockToken blockToken) noexcept;
    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>
    CommitLocalReservedEnqueue(ReadyQueueEnqueueReservation reservation) noexcept;
    [[nodiscard]] ReadyQueueError CancelLocalReservedEnqueue(ReadyQueueEnqueueReservation reservation) noexcept;

    ProcessorScheduler& m_scheduler;
    ProcessorSchedulerWakeFacade* const* m_homes{nullptr};
};

/// Owns one processor's ready selection and dispatch transaction. The active ReadyQueue claim
/// remains retained until Finish, so a Ready completion can take a fresh FIFO position without a
/// capacity or lifetime gap. DispatchNext ranks candidates at one clock sample, temporarily claims
/// budget-ineligible entries and entries whose Thread or SchedulingContext rejects admission while
/// it searches the remaining bounded queue, and returns the earliest replenishment of the
/// ineligible ones. Operations may spin and are forbidden from interrupt context and same-CPU
/// reentry. Architecture code uses StopAtTicks as an absolute preemption boundary and must not
/// enter the selected context after that instant.
class ProcessorScheduler final
{
public:
    /// Preconditions:
    /// - objectRegistry and clock outlive this scheduler.
    /// - All placement, lifecycle, donation, and dispatch operations for this
    /// processor go through this object's policy domain.
    ProcessorScheduler(Object::ObjectRegistry& objectRegistry, Time::IMonotonicClock& clock, CpuId processor) noexcept
        : m_readyQueue{objectRegistry, processor}, m_coordinator{objectRegistry, clock}, m_wakeFacade{*this},
          m_clock{clock}, m_processor{processor},
          m_state{processor.IsValid() ? ProcessorSchedulerState::Idle : ProcessorSchedulerState::Faulted}
    {
    }

    /// Preconditions:
    /// - State is Idle, or this is the initial configuration-fault state with
    /// no retained transaction, and no concurrent operation accesses this
    /// object.
    ~ProcessorScheduler() noexcept = default;

    ProcessorScheduler(const ProcessorScheduler&) = delete;
    ProcessorScheduler& operator=(const ProcessorScheduler&) = delete;
    ProcessorScheduler(ProcessorScheduler&&) = delete;
    ProcessorScheduler& operator=(ProcessorScheduler&&) = delete;

    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReceipt, ProcessorSchedulerFailure>
    Enqueue(SchedulingContextId contextId, SchedulingContext& context, ThreadId threadId, Thread& thread) noexcept;

    /// Selects and starts the best budget-eligible entry. A candidate whose
    /// Thread or SchedulingContext rejects admission is skipped and keeps its
    /// FIFO position. NoRunnableThread may carry a nonzero RetryAtTicks for
    /// the earliest skipped replenishment and, in Dispatch, the first such
    /// rejection.
    [[nodiscard]] Foundation::Result<ProcessorSchedulerAdmission, ProcessorSchedulerFailure> DispatchNext() noexcept;

    /// Selects the highest-ranked candidate but stops at the first deferred
    /// candidate instead of admitting lower-ranked work. Every temporary queue
    /// claim is restored at its original FIFO position before the deferred
    /// failure is returned.
    [[nodiscard]] Foundation::Result<ProcessorSchedulerAdmission, ProcessorSchedulerFailure>
    DispatchNextWithoutDeferredWork() noexcept;

    /// Accounts the active run. Ready takes a new FIFO position while Blocked
    /// and Exited release the retained queue claim.
    [[nodiscard]] Foundation::Result<DispatchCompletion, ProcessorSchedulerFailure>
    Finish(ThreadRunDisposition disposition, Foundation::UInt64 exitCode = 0U) noexcept;

    /// Completes the exact active run as Ready at an architectural clock
    /// sample captured by external interruption entry. This is callable only
    /// from the owning CPU's non-ISR scheduler continuation.
    [[nodiscard]] Foundation::Result<DispatchCompletion, ProcessorSchedulerFailure>
    FinishExternallyStoppedAsReady(Foundation::UInt64 observedTicks) noexcept;

    [[nodiscard]] Foundation::Result<ReadyQueueState, ProcessorSchedulerFailure> Remove(ThreadId thread) noexcept;

    [[nodiscard]] ProcessorSchedulerSnapshot GetSnapshot() const noexcept;

    /// Revalidates one exact active admission against the retained queue
    /// claim, coordinator, Thread, SchedulingContext, CPU, and RunSequence.
    /// Any live-state contradiction faults the scheduler while retaining the
    /// active claim and its pins for terminal diagnosis.
    [[nodiscard]] Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>
    GetActiveExecutionBinding(const ProcessorSchedulerAdmission& admission) noexcept;

    [[nodiscard]] constexpr ProcessorSchedulerWakeFacade& GetWakeFacade() noexcept
    {
        return m_wakeFacade;
    }

private:
    friend class ProcessorSchedulerWakeFacade;

    enum class CandidateOutcome : Foundation::UInt8
    {
        Admitted,
        Deferred,
        Skipped,
        Rejected,
    };

    enum class DeferredCandidatePolicy : Foundation::UInt8
    {
        ContinueSearch,
        RejectDispatch,
    };

    [[nodiscard]] static ProcessorSchedulerFailure
    MakeFailure(ProcessorSchedulerError error, Foundation::UInt64 retryAtTicks = 0U,
                const ReadyQueueFailure& queue = {}, const DispatchCoordinatorFailure& dispatch = {},
                ReadyQueueError queueTransition = ReadyQueueError::None) noexcept;
    [[nodiscard]] bool DispatchNextLocked(ProcessorSchedulerAdmission& admission, ProcessorSchedulerFailure& failure,
                                          DeferredCandidatePolicy deferredCandidatePolicy) noexcept;
    [[nodiscard]] CandidateOutcome DispatchClaimLocked(const ReadyQueueClaim& claim, Foundation::UInt64& retryAtTicks,
                                                       DispatchCoordinatorFailure& rejection,
                                                       ProcessorSchedulerAdmission& admission,
                                                       ProcessorSchedulerFailure& failure) noexcept;
    [[nodiscard]] bool FinishLocked(ThreadRunDisposition disposition, Foundation::UInt64 exitCode,
                                    DispatchCompletion& completion, ProcessorSchedulerFailure& failure) noexcept;
    [[nodiscard]] bool
    CommitCompletionLocked(ThreadRunDisposition disposition,
                           const Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>& completionResult,
                           DispatchCompletion& completion, ProcessorSchedulerFailure& failure) noexcept;
    [[nodiscard]] ReadyQueueError RestoreSkippedClaimsLocked() noexcept;
    [[nodiscard]] static bool AdmissionsMatch(const ProcessorSchedulerAdmission& expected,
                                              const ProcessorSchedulerAdmission& candidate) noexcept;
    [[nodiscard]] bool GetActiveExecutionBindingLocked(const ProcessorSchedulerAdmission& admission,
                                                       ProcessorActiveExecutionBinding& binding,
                                                       ProcessorActiveExecutionBindingFailure& failure) noexcept;
    void EnterFaultedLocked() noexcept;

    ReadyQueue m_readyQueue;
    DispatchCoordinator m_coordinator;
    ProcessorSchedulerWakeFacade m_wakeFacade;
    Time::IMonotonicClock& m_clock;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    ReadyQueueClaim m_activeClaim{};
    ProcessorSchedulerAdmission m_activeAdmission{};
    ReadyQueueClaimToken m_skippedClaims[MaximumReadyQueueEntries]{};
    Foundation::Size m_skippedClaimCount{0U};
    CpuId m_processor{};
    ProcessorSchedulerState m_state{ProcessorSchedulerState::Faulted};
};

inline CpuId ProcessorSchedulerWakeFacade::GetProcessor() const noexcept
{
    return m_scheduler.m_processor;
}

inline Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>
ProcessorSchedulerWakeFacade::ReserveBlockedEnqueue(const SchedulingContextId contextId, SchedulingContext& context,
                                                    const ThreadId threadId, Thread& thread,
                                                    const ThreadBlockToken blockToken) noexcept
{
    if (m_homes != nullptr)
    {
        const auto affinity = context.GetConfiguration().CpuAffinityMask;
        for (Foundation::Size home{0U}; home < MaximumProcessorCount; ++home)
        {
            if (affinity == (Foundation::ProcessorMask{1U} << home) && m_homes[home] != nullptr &&
                m_homes[home] != this)
            {
                return m_homes[home]->ReserveLocalBlockedEnqueue(contextId, context, threadId, thread, blockToken);
            }
        }
    }
    return ReserveLocalBlockedEnqueue(contextId, context, threadId, thread, blockToken);
}

inline Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>
ProcessorSchedulerWakeFacade::ReserveLocalBlockedEnqueue(const SchedulingContextId contextId,
                                                         SchedulingContext& context, const ThreadId threadId,
                                                         Thread& thread, const ThreadBlockToken blockToken) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>;
    if (Platform::Synchronization::ProcessorPowerStopping())
    {
        return ResultType::Failure(ReadyQueueFailure{.Error = ReadyQueueError::InvalidConfiguration,
                                                     .ContextPin = Object::ObjectPinError::Ok,
                                                     .ThreadPin = Object::ObjectPinError::Ok,
                                                     .Reserved = 0U});
    }
    Platform::Synchronization::SpinLockGuard guard{m_scheduler.m_synchronization};
    if (m_scheduler.m_state == ProcessorSchedulerState::Faulted)
    {
        return ResultType::Failure(ReadyQueueFailure{
            .Error = m_scheduler.m_processor.IsValid() ? ReadyQueueError::QueueCorrupt
                                                       : ReadyQueueError::InvalidConfiguration,
            .ContextPin = Object::ObjectPinError::Ok,
            .ThreadPin = Object::ObjectPinError::Ok,
            .Reserved = 0U,
        });
    }
    return m_scheduler.m_readyQueue.ReserveBlockedEnqueue(contextId, context, threadId, thread, blockToken);
}

inline Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>
ProcessorSchedulerWakeFacade::CommitReservedEnqueue(const ReadyQueueEnqueueReservation reservation) noexcept
{
    if (m_homes != nullptr)
    {
        for (Foundation::Size home{0U}; home < MaximumProcessorCount; ++home)
        {
            if (m_homes[home] != nullptr && m_homes[home] != this &&
                reservation.IsFor(m_homes[home]->m_scheduler.m_readyQueue))
            {
                return m_homes[home]->CommitLocalReservedEnqueue(reservation);
            }
        }
    }
    return CommitLocalReservedEnqueue(reservation);
}

inline Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>
ProcessorSchedulerWakeFacade::CommitLocalReservedEnqueue(const ReadyQueueEnqueueReservation reservation) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_scheduler.m_synchronization};
    const auto result = m_scheduler.m_readyQueue.CommitReservedEnqueue(reservation);
    if (result.HasValue())
    {
        // The surrounding wake transaction still holds locks; queue its kick for the poster's next
        // unlocked service/return point.
        Platform::Synchronization::QueueProcessorKick(m_scheduler.m_processor);
    }
    return result;
}

inline ReadyQueueError
ProcessorSchedulerWakeFacade::CancelReservedEnqueue(const ReadyQueueEnqueueReservation reservation) noexcept
{
    if (m_homes != nullptr)
    {
        for (Foundation::Size home{0U}; home < MaximumProcessorCount; ++home)
        {
            if (m_homes[home] != nullptr && m_homes[home] != this &&
                reservation.IsFor(m_homes[home]->m_scheduler.m_readyQueue))
            {
                return m_homes[home]->CancelLocalReservedEnqueue(reservation);
            }
        }
    }
    return CancelLocalReservedEnqueue(reservation);
}

inline ReadyQueueError
ProcessorSchedulerWakeFacade::CancelLocalReservedEnqueue(const ReadyQueueEnqueueReservation reservation) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_scheduler.m_synchronization};
    return m_scheduler.m_readyQueue.CancelReservedEnqueue(reservation);
}

static_assert(sizeof(ProcessorSchedulerState) == 1U);
static_assert(sizeof(ProcessorSchedulerError) == 1U);
static_assert(sizeof(BlockedRunCommitOutcome) == 1U);
static_assert(sizeof(ProcessorActiveExecutionBindingError) == 1U);
static_assert(sizeof(ProcessorSchedulerFailure) == 32U);
static_assert(alignof(ProcessorSchedulerFailure) == 8U);
static_assert(sizeof(ProcessorSchedulerAdmission) == 128U);
static_assert(alignof(ProcessorSchedulerAdmission) == 8U);
static_assert(sizeof(ProcessorSchedulerSnapshot) == 56U);
static_assert(alignof(ProcessorSchedulerSnapshot) == 8U);
static_assert(sizeof(ProcessorActiveExecutionBinding) == 64U);
static_assert(alignof(ProcessorActiveExecutionBinding) == 8U);
static_assert(sizeof(ProcessorActiveExecutionBindingFailure) == 8U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_PROCESSOR_SCHEDULER_HPP_INCLUDED
