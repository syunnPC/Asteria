// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_WAIT_SET_THREAD_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_WAIT_SET_THREAD_DISPATCHER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Ipc
{
/// Identifies one WaitSet generation within one ObjectRegistry.
/// Lifetime:
/// - The originating registry outlives every copy and use of this value.
class WaitSetId final
{
public:
    constexpr WaitSetId() noexcept = default;

    constexpr WaitSetId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
        : m_registry{&registry}, m_object{object}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_object.GetValue();
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_registry != nullptr) && m_object.IsValid();
    }

    [[nodiscard]] constexpr Object::ObjectId GetObjectId() const noexcept
    {
        return m_object;
    }

    [[nodiscard]] constexpr bool IsFromRegistry(const Object::ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const WaitSetId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class WaitSetThreadDispatcherState : Foundation::UInt8
{
    Idle,
    Prepared,
    Blocked,
    QueuePressure,
    Completed,
    Faulted,
};

enum class WaitSetThreadDispatcherError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidWaitSet,
    WaitSetRegistryMismatch,
    InvalidThread,
    ThreadRegistryMismatch,
    InvalidSchedulingContext,
    SchedulingContextRegistryMismatch,
    AlreadyPrepared,
    TransactionGenerationExhausted,
    WaitSetPinRejected,
    ThreadPinRejected,
    SchedulingContextPinRejected,
    CorrelatedStateChanged,
    WaitSetClaimRejected,
    WaitSetClaimCancellationRejected,
    InvalidBlockToken,
    InvalidState,
    QueueReservationRejected,
    QueueReservationRollbackRejected,
    WakeRejected,
    QueueCommitRejected,
    WaitSetClaimCommitRejected,
    AlreadyCompleted,
    CompletionUnavailable,
    ReceiptInvariantRejected,
    CompletionReceiptMismatch,
    Faulted,
};

class WaitSetThreadDispatcherGenerationPolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    WaitSetThreadDispatcherGenerationPolicy() = delete;

    /// Advances current without wrapping. Failure leaves next unchanged.
    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current == Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

/// Nonwrapping transaction-generation allocator of one dispatcher. Its value
/// is the last generation issued; zero means that none has been issued. A
/// caller that must withdraw an issued generation restores a copy taken
/// before TryIssue, so the next transaction observes the same generation.
class WaitSetThreadDispatcherGenerationCounter final
{
public:
    constexpr WaitSetThreadDispatcherGenerationCounter() noexcept = default;

    /// Continues the allocation sequence after lastIssued.
    explicit constexpr WaitSetThreadDispatcherGenerationCounter(const Foundation::UInt64 lastIssued) noexcept
        : m_lastIssued{lastIssued}
    {
    }

    /// False once Maximum has been issued; no later generation exists.
    [[nodiscard]] constexpr bool CanIssue() const noexcept
    {
        return m_lastIssued != WaitSetThreadDispatcherGenerationPolicy::Maximum;
    }

    /// Issues the next generation. Failure leaves this counter and generation
    /// unchanged.
    [[nodiscard]] constexpr bool TryIssue(Foundation::UInt64& generation) noexcept
    {
        Foundation::UInt64 next{m_lastIssued};
        if (!WaitSetThreadDispatcherGenerationPolicy::TryAdvance(m_lastIssued, next))
        {
            return false;
        }
        m_lastIssued = next;
        generation = next;
        return true;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetLastIssued() const noexcept
    {
        return m_lastIssued;
    }

private:
    Foundation::UInt64 m_lastIssued{0U};
};

struct WaitSetThreadPrepareRequest
{
    WaitSetId WaitSet;
    Scheduler::ThreadId Thread;
    Scheduler::SchedulingContextId SchedulingContext;
    /// The local tick at which a blocked wait may expire (Expire), or zero
    /// for none.
    Foundation::UInt64 DeadlineTicks;
};

struct WaitSetThreadDispatcherFailure
{
    WaitSetFailure WaitSet;
    Scheduler::ReadyQueueFailure Queue;
    Object::ObjectPinError WaitSetPin;
    Object::ObjectPinError ThreadPin;
    Object::ObjectPinError SchedulingContextPin;
    Scheduler::ThreadError Thread;
    Scheduler::ReadyQueueError QueueTransition;
    WaitSetThreadDispatcherError Error;
};

class WaitSetThreadDispatcher;

/// Identifies one exact transaction of one immovable dispatcher.
/// Lifetime:
/// - Dispatcher outlives every copy and use of this receipt.
struct WaitSetThreadDispatcherReceipt
{
    const WaitSetThreadDispatcher* Dispatcher;
    WaitSetId WaitSet;
    Scheduler::ThreadId Thread;
    Scheduler::SchedulingContextId SchedulingContext;
    NotificationId Notification;
    Foundation::UInt64 RegistrationSequence;
    Foundation::UInt64 RunSequence;
    Foundation::UInt64 BlockSequence;
    Foundation::UInt64 TransactionGeneration;
    Foundation::UInt64 Tag;
    Foundation::UInt64 QueueSequence;
    WaitSetThreadDispatcherState State;
    /// Completed by Expire rather than by readiness: no Notification, tag or registration.
    bool TimedOut;
    /// Completed by Abort, as the thread is killed; TimedOut as well.
    bool Aborted;
    Foundation::UInt8 Reserved[5];
};

/// Observes the exact active transaction. Dispatcher is this object's stable
/// instance identity. TransactionGeneration is nonzero whenever Prepare has
/// allocated the transaction visible in this snapshot and is zero otherwise.
struct WaitSetThreadDispatcherSnapshot
{
    const WaitSetThreadDispatcher* Dispatcher;
    WaitSetId WaitSet;
    Scheduler::ThreadId Thread;
    Scheduler::SchedulingContextId SchedulingContext;
    NotificationId Notification;
    Foundation::UInt64 RegistrationSequence;
    Foundation::UInt64 RunSequence;
    Foundation::UInt64 BlockSequence;
    Foundation::UInt64 TransactionGeneration;
    Foundation::UInt64 RetainedClaimSequence;
    Foundation::UInt64 RetainedQueueReservationSequence;
    Foundation::UInt64 Tag;
    Foundation::UInt64 QueueSequence;
    WaitSetThreadDispatcherFailure Diagnostic;
    WaitSetThreadDispatcherState State;
    bool TimedOut;
    bool Aborted;
    Foundation::UInt8 Reserved[3];
};

/// Owns one bounded normal-context WaitSet wait and exact Thread wake. Capability authorization
/// precedes this component. In particular, the caller has already authorized Wait on
/// request.WaitSet and has authorized the Thread/SchedulingContext relationship. ObjectPin retains
/// lifetime and grants no authority. Prepare pins and correlates the exact WaitSet, Running Thread,
/// and Running SchedulingContext generations and captures the Thread's nonwrapping run sequence. It
/// consumes immediately available readiness without changing Thread state. Empty, WouldBlock, and a
/// competing ClaimActive all preserve the wait: registrations may be added later, and removing the
/// last one does not fault a prepared or blocked transaction. CommitBlocked accepts only a block
/// token produced by the captured run after both Thread and SchedulingContext have stopped. It then
/// rechecks the WaitSet before another Poll is needed, closing signal-before-block races. Every
/// successful Prepare has a nonzero generation that advances only after pinning and correlation
/// succeed and never wraps. A failed Prepare that restores Idle also restores the generation
/// allocator. Once Maximum has been issued and its transaction returns to Idle, later valid Prepare
/// calls fail with TransactionGenerationExhausted before pinning or WaitSet mutation. A wake first
/// reserves capacity in the exact scheduler-owned ReadyQueue, pins both generations, and reserves
/// both memberships. Thread Wake and queue publication each commit exactly once. Only after both
/// succeed is the WaitSet claim consumed. Queue pressure cancels the claim, republishes readiness,
/// and retains the blocked transaction for Poll retry. Any contradiction after Thread::Wake is
/// terminal Faulted and remains visible through GetSnapshot. Operations may spin, are forbidden in
/// interrupt context, and require same-CPU reentry plus direct Thread/SchedulingContext lifecycle
/// changes to be excluded. Notification::Signal may run concurrently; it never calls this
/// component.
class WaitSetThreadDispatcher final
{
public:
    /// Preconditions:
    /// - objectRegistry is the registry used by wakeFacade and every request;
    /// - objectRegistry and wakeFacade's scheduler outlive this object;
    /// - wakeFacade remains alive and immovable for this lifetime.
    constexpr WaitSetThreadDispatcher(Object::ObjectRegistry& objectRegistry,
                                      Scheduler::ProcessorSchedulerWakeFacade& wakeFacade) noexcept
        : m_objectRegistry{objectRegistry}, m_wakeFacade{wakeFacade}
    {
    }

    /// Preconditions:
    /// - no active method or unresolved pre-wake rollback remains;
    /// - a caller abandoning Prepared, Blocked, or QueuePressure has assumed
    /// responsibility for the retained Thread lifecycle;
    /// - a terminal Faulted transaction retaining an internal claim or queue
    /// reservation remains alive until fatal-domain recovery ends its use;
    /// - no concurrent operation accesses this object.
    ~WaitSetThreadDispatcher() noexcept = default;

    WaitSetThreadDispatcher(const WaitSetThreadDispatcher&) = delete;
    WaitSetThreadDispatcher& operator=(const WaitSetThreadDispatcher&) = delete;
    WaitSetThreadDispatcher(WaitSetThreadDispatcher&&) = delete;
    WaitSetThreadDispatcher& operator=(WaitSetThreadDispatcher&&) = delete;

    /// Correlates one currently Running Thread/SchedulingContext and attempts
    /// immediate readiness. Every pre-claim failure releases all new pins.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure>
    Prepare(const WaitSetThreadPrepareRequest& request) noexcept;

    /// Rejects an observed receipt that failed an invariant check in the
    /// composing trusted layer. reported must identify the exact current
    /// transaction. An exact Prepared transaction has no retained claim and
    /// is rolled back to Idle. Completed has already consumed readiness,
    /// while a foreign report cannot identify the current transaction; either
    /// contradiction is terminal Faulted and preserves all receipt fields.
    [[nodiscard]] WaitSetThreadDispatcherFailure
    RejectReceiptInvariant(const WaitSetThreadDispatcherReceipt& reported) noexcept;

    /// Accepts the exact ThreadBlockToken returned by the scheduler after the
    /// prepared run completed as Blocked, then rechecks readiness immediately.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure>
    CommitBlocked(Scheduler::ThreadBlockToken blockToken) noexcept;

    /// Wait-specific post-Finish commitment used by the processor run loop.
    /// Accepted permits the same serialized transition to dispatch idle work.
    /// RetryableRejected retains the exact blocked transaction and its pins;
    /// the run loop must switch to idle without dispatch, and its owner may
    /// call Poll later. TerminalRejected forbids ordinary retry or dispatch;
    /// any retained claim/reservation remains actionable only until
    /// fatal-domain recovery resolves it.
    [[nodiscard]] Scheduler::BlockedRunCommitOutcome
    CommitBlockedFromFinishedRun(Scheduler::ThreadBlockToken blockToken) noexcept;

    /// Rechecks a retained Blocked or QueuePressure transaction. Empty,
    /// WouldBlock, and ClaimActive produce a successful Blocked receipt;
    /// QueuePressure remains retryable.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure> Poll() noexcept;

    /// Wakes a Blocked transaction whose deadline is at or before now with no readiness claimed:
    /// the thread's ready-queue placement is reserved, the thread woken and the placement published
    /// as for a readiness wake, and the Completed receipt is TimedOut, with no Notification, tag or
    /// registration. Callers Poll first, so readiness wins when both are due. A transaction that is
    /// not Blocked, has no deadline or is not due yet is returned unchanged; queue pressure leaves
    /// it Blocked for a later call.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure>
    Expire(Foundation::UInt64 now) noexcept;

    /// Wakes a Blocked transaction because its thread is being killed, whatever its deadline: the
    /// same steps as Expire, and the Completed receipt is TimedOut and Aborted, with no
    /// Notification, tag or registration. The woken thread must not return to user mode. A
    /// transaction that is not Blocked (none, an immediate completion, queue pressure, an earlier
    /// wake) is returned unchanged and not Aborted; queue pressure while waking leaves it Blocked
    /// for a later call.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure> Abort() noexcept;

    /// Returns a dispatcher whose thread will never resume to Idle: a
    /// Completed transaction is consumed as ConsumeCompletion would, Idle is
    /// left alone. Any other state is refused unchanged, since its thread
    /// still waits or its claim is retained.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure> Reset() noexcept;

    /// The deadline of the current transaction, or zero.
    [[nodiscard]] Foundation::UInt64 GetDeadline() const noexcept;

    /// Consumes one Completed receipt exactly once and resets this dispatcher to Idle with no
    /// actionable token, object pointer, or retained pin. BSP sequence: Prepare against the trusted
    /// active binding, then pass this dispatcher to FinishWaitAndDispatch. Accepted may dispatch; a
    /// retryable rejection switches idle and is progressed with Poll before later idle dispatch; a
    /// terminal rejection leaves the run loop faulted. Accepted/Blocked is later progressed by Poll
    /// after a signal, while Accepted/Completed already published the wake. After an immediate
    /// completion, or after the woken thread is admitted again, call ConsumeCompletion before
    /// reusing this dispatcher for another Prepare.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure>
    ConsumeCompletion() noexcept;

    /// Atomically verifies and consumes one exact Completed receipt. A
    /// mismatch is terminal and preserves the completed correlation in the
    /// Faulted snapshot; the dispatcher is reset to Idle only after every
    /// receipt field matches expected while holding the dispatcher lock.
    [[nodiscard]] Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure>
    ConsumeCompletion(const WaitSetThreadDispatcherReceipt& expected) noexcept;

    [[nodiscard]] WaitSetThreadDispatcherSnapshot GetSnapshot() const noexcept;

private:
    using OperationResult = Foundation::Result<WaitSetThreadDispatcherReceipt, WaitSetThreadDispatcherFailure>;

    [[gnu::noinline]] [[nodiscard]] static WaitSetThreadDispatcherFailure
    MakeFailure(WaitSetThreadDispatcherError error, const WaitSetFailure& waitSet = {},
                const Scheduler::ReadyQueueFailure& queue = {},
                Object::ObjectPinError waitSetPin = Object::ObjectPinError::Ok,
                Object::ObjectPinError threadPin = Object::ObjectPinError::Ok,
                Object::ObjectPinError schedulingContextPin = Object::ObjectPinError::Ok,
                Scheduler::ThreadError thread = Scheduler::ThreadError::None,
                Scheduler::ReadyQueueError queueTransition = Scheduler::ReadyQueueError::None) noexcept;
    [[nodiscard]] static bool ContextCanResume(Scheduler::SchedulingContextState state) noexcept;
    [[nodiscard]] static bool QueueFailureIsRetryable(const Scheduler::ReadyQueueFailure& failure) noexcept;
    [[nodiscard]] static bool WaitSetClaimIsDeferred(WaitSetError error) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool ValidatePrepareRequestLocked(const WaitSetThreadPrepareRequest& request,
                                                                      WaitSetThreadDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool AdvanceTransactionGenerationLocked() noexcept;
    [[gnu::noinline]] [[nodiscard]] bool PinPreparationLocked(const WaitSetThreadPrepareRequest& request,
                                                              WaitSetThreadDispatcherFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool CaptureRunningCorrelationLocked() noexcept;
    [[nodiscard]] bool BlockedStateIsCorrelatedLocked() const noexcept;
    [[nodiscard]] bool CandidateBlockIsCorrelatedLocked(Scheduler::ThreadBlockToken blockToken,
                                                        bool& tokenMismatch) const noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult ProgressLocked() noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult
    CommitBlockedLocked(Scheduler::ThreadBlockToken blockToken) noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult ClaimPreparedLocked() noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult ReserveClaimedWakeLocked() noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult WakeReservedLocked() noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult PublishReservedLocked() noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult CommitWakeClaimLocked() noexcept;
    [[gnu::noinline]] [[nodiscard]] OperationResult
    CompleteImmediateClaimLocked(const WaitSetNotificationClaim& claim) noexcept;
    [[nodiscard]] bool CancelClaimLocked(WaitSetClaimToken claim, WaitSetThreadDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool CancelPreWakeLocked(Scheduler::ReadyQueueEnqueueReservation reservation, WaitSetClaimToken claim,
                                           WaitSetThreadDispatcherFailure& failure) noexcept;
    [[gnu::noinline]] [[nodiscard]] WaitSetThreadDispatcherReceipt MakeReceiptLocked() const noexcept;
    [[nodiscard]] static bool ReceiptsAreEqual(const WaitSetThreadDispatcherReceipt& left,
                                               const WaitSetThreadDispatcherReceipt& right) noexcept;
    // Expire's steps after its checks: reserve the thread's placement, wake
    // it, publish it and complete as timed out.
    [[nodiscard]] OperationResult ReserveExpiryLocked() noexcept;
    [[nodiscard]] OperationResult WakeExpiredLocked() noexcept;
    [[nodiscard]] OperationResult PublishExpiredLocked() noexcept;
    [[nodiscard]] OperationResult ConsumeCompletionLocked(const WaitSetThreadDispatcherReceipt* expected) noexcept;
    void StoreClaimLocked(const WaitSetNotificationClaim& claim) noexcept;
    void ClearClaimLocked() noexcept;
    void ClearFailureLocked() noexcept;
    void ResetPreparationLocked() noexcept;
    void ReleaseTerminalPinsLocked() noexcept;
    void EnterFaultedLocked(const WaitSetThreadDispatcherFailure& failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Scheduler::ProcessorSchedulerWakeFacade& m_wakeFacade;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Object::ObjectPin m_waitSetPin;
    Object::ObjectPin m_threadPin;
    Object::ObjectPin m_contextPin;
    WaitSet* m_waitSet{nullptr};
    Scheduler::Thread* m_thread{nullptr};
    Scheduler::SchedulingContext* m_context{nullptr};
    WaitSetId m_waitSetIdentity{};
    Scheduler::ThreadId m_threadIdentity{};
    Scheduler::SchedulingContextId m_contextIdentity{};
    Scheduler::ThreadBlockToken m_blockToken{};
    WaitSetClaimToken m_claimToken{};
    Scheduler::ReadyQueueEnqueueReservation m_queueReservation{};
    NotificationId m_notification{};
    Foundation::UInt64 m_registrationSequence{0U};
    Foundation::UInt64 m_runSequence{0U};
    Foundation::UInt64 m_blockSequence{0U};
    WaitSetThreadDispatcherGenerationCounter m_transactionGenerations{};
    Foundation::UInt64 m_transactionGeneration{0U};
    Foundation::UInt64 m_tag{0U};
    Foundation::UInt64 m_queueSequence{0U};
    Foundation::UInt64 m_deadline{0U};
    WaitSetThreadDispatcherFailure m_diagnostic{};
    WaitSetThreadDispatcherState m_state{WaitSetThreadDispatcherState::Idle};
    bool m_timedOut{false};
    bool m_aborted{false};
};

static_assert(sizeof(WaitSetId) == 16U);
static_assert(alignof(WaitSetId) == 8U);
static_assert(sizeof(WaitSetThreadDispatcherState) == 1U);
static_assert(sizeof(WaitSetThreadDispatcherError) == 1U);
static_assert(sizeof(WaitSetThreadPrepareRequest) == 56U);
static_assert(alignof(WaitSetThreadPrepareRequest) == 8U);
static_assert(sizeof(WaitSetThreadDispatcherFailure) == 26U);
static_assert(alignof(WaitSetThreadDispatcherFailure) == 1U);
static_assert(sizeof(WaitSetThreadDispatcherReceipt) == 128U);
static_assert(alignof(WaitSetThreadDispatcherReceipt) == 8U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherReceipt, State) == 120U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherReceipt, TimedOut) == 121U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherReceipt, Aborted) == 122U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherReceipt, Reserved) == 123U);
static_assert(sizeof(WaitSetThreadDispatcherSnapshot) == 168U);
static_assert(alignof(WaitSetThreadDispatcherSnapshot) == 8U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherSnapshot, State) == 162U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherSnapshot, TimedOut) == 163U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherSnapshot, Aborted) == 164U);
static_assert(__builtin_offsetof(WaitSetThreadDispatcherSnapshot, Reserved) == 165U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return WaitSetThreadDispatcherGenerationPolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{17U};
        return !WaitSetThreadDispatcherGenerationPolicy::TryAdvance(WaitSetThreadDispatcherGenerationPolicy::Maximum,
                                                                    next) &&
               (next == 17U);
    }());
static_assert(sizeof(WaitSetThreadDispatcherGenerationCounter) == sizeof(Foundation::UInt64));
static_assert(
    []() constexpr
    {
        WaitSetThreadDispatcherGenerationCounter counter{};
        Foundation::UInt64 generation{0U};
        return counter.CanIssue() && counter.TryIssue(generation) && (generation == 1U) &&
               (counter.GetLastIssued() == 1U);
    }());
static_assert(
    []() constexpr
    {
        WaitSetThreadDispatcherGenerationCounter counter{WaitSetThreadDispatcherGenerationPolicy::Maximum};
        Foundation::UInt64 generation{17U};
        return !counter.CanIssue() && !counter.TryIssue(generation) && (generation == 17U) &&
               (counter.GetLastIssued() == WaitSetThreadDispatcherGenerationPolicy::Maximum);
    }());
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_WAIT_SET_THREAD_DISPATCHER_HPP_INCLUDED
