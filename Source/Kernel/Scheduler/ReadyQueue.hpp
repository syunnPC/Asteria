// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_READY_QUEUE_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_READY_QUEUE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Scheduler
{
constexpr Foundation::Size MaximumReadyQueueEntries{32U};
static_assert(MaximumReadyQueueEntries <= 32U, "ClaimNext tracks skipped entries in a 32-bit mask");

enum class ReadyQueueError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidContext,
    ContextRegistryMismatch,
    InvalidThread,
    ThreadRegistryMismatch,
    QueueFull,
    DuplicateContext,
    DuplicateThread,
    ContextAlreadyQueued,
    ThreadAlreadyQueued,
    ContextPinRejected,
    ThreadPinRejected,
    ContextInstanceMismatch,
    ThreadInstanceMismatch,
    ContextNotRunnable,
    ThreadNotReady,
    ContextThreadMismatch,
    CpuNotAllowed,
    ContextMembershipMismatch,
    ThreadMembershipMismatch,
    QueueCorrupt,
    SequenceExhausted,
    Empty,
    InvalidClaim,
    StaleClaim,
    EntryClaimed,
    EntryReserved,
    NotFound,
    InvalidReservation,
    StaleReservation,
    InvalidBlockToken,
    ThreadNotBlocked,
};

struct ReadyQueueFailure
{
    ReadyQueueError Error;
    Object::ObjectPinError ContextPin;
    Object::ObjectPinError ThreadPin;
    Foundation::UInt8 Reserved;
};

struct ReadyQueueEnqueueReceipt
{
    Foundation::UInt64 Sequence;
};

struct ReadyQueueState
{
    /// Next FIFO position. Temporary claims use an independent generation and
    /// therefore do not consume or reorder this sequence.
    Foundation::UInt64 NextSequence;
    Foundation::UInt8 ReadyEntries;
    Foundation::UInt8 ClaimedEntries;
    Foundation::UInt8 ReservedEntries;
    CpuId Processor;
    Foundation::UInt8 Reserved[4];
};

class ReadyQueue;

/// Names one fixed queue slot retained for an exact blocked Thread.
/// The originating queue outlives every copy and use of this token.
class ReadyQueueEnqueueReservation final
{
public:
    constexpr ReadyQueueEnqueueReservation() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_queue != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const ReadyQueueEnqueueReservation other) const noexcept
    {
        return (m_queue == other.m_queue) && (m_sequence == other.m_sequence);
    }

    [[nodiscard]] constexpr bool IsFor(const ReadyQueue& queue) const noexcept
    {
        return m_queue == &queue;
    }

private:
    friend class ReadyQueue;

    constexpr ReadyQueueEnqueueReservation(const ReadyQueue& queue, const Foundation::UInt64 sequence) noexcept
        : m_queue{&queue}, m_sequence{sequence}
    {
    }

    const ReadyQueue* m_queue{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

/// Names one outstanding claim from one ReadyQueue.
/// The originating queue outlives every copy and use of this token.
class ReadyQueueClaimToken final
{
public:
    constexpr ReadyQueueClaimToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_queue != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const ReadyQueueClaimToken other) const noexcept
    {
        return (m_queue == other.m_queue) && (m_sequence == other.m_sequence);
    }

private:
    friend class ReadyQueue;

    constexpr ReadyQueueClaimToken(const ReadyQueue& queue, const Foundation::UInt64 sequence) noexcept
        : m_queue{&queue}, m_sequence{sequence}
    {
    }

    const ReadyQueue* m_queue{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

/// Copyable view of one claimed queue entry. The queue retains both generation
/// pins until CommitClaim or CancelClaim consumes Token.
class ReadyQueueClaim final
{
public:
    constexpr ReadyQueueClaim() noexcept = default;

    [[nodiscard]] constexpr ReadyQueueClaimToken GetToken() const noexcept
    {
        return m_token;
    }

    [[nodiscard]] constexpr SchedulingContextId GetContextId() const noexcept
    {
        return m_contextId;
    }

    [[nodiscard]] constexpr ThreadId GetThreadId() const noexcept
    {
        return m_threadId;
    }

    /// Preconditions:
    /// - Token still names an unresolved claim in the originating queue.
    [[nodiscard]] SchedulingContext& GetContext() const noexcept
    {
        return *m_context;
    }

    /// Preconditions:
    /// - Token still names an unresolved claim in the originating queue.
    [[nodiscard]] Thread& GetThread() const noexcept
    {
        return *m_thread;
    }

    [[nodiscard]] constexpr SchedulingContextConfiguration GetConfiguration() const noexcept
    {
        return m_configuration;
    }

    /// SchedulingContext::GetEffectiveDeadline at the selection sample that produced this claim; it
    /// ranks DeadlineSporadic entries.
    [[nodiscard]] constexpr Foundation::UInt64 GetEffectiveDeadlineTicks() const noexcept
    {
        return m_effectiveDeadlineTicks;
    }

private:
    friend class ReadyQueue;

    constexpr ReadyQueueClaim(const ReadyQueueClaimToken token, const SchedulingContextId contextId,
                              const ThreadId threadId, SchedulingContext& context, Thread& thread,
                              const SchedulingContextConfiguration configuration,
                              const Foundation::UInt64 effectiveDeadlineTicks) noexcept
        : m_token{token}, m_contextId{contextId}, m_threadId{threadId}, m_context{&context}, m_thread{&thread},
          m_configuration{configuration}, m_effectiveDeadlineTicks{effectiveDeadlineTicks}
    {
    }

    ReadyQueueClaimToken m_token{};
    SchedulingContextId m_contextId{};
    ThreadId m_threadId{};
    SchedulingContext* m_context{nullptr};
    Thread* m_thread{nullptr};
    SchedulingContextConfiguration m_configuration{};
    Foundation::UInt64 m_effectiveDeadlineTicks{0U};
};

/// Fixed-capacity ready set for one processor. Enqueue retains the exact SchedulingContext and
/// Thread generations. ClaimNext selects one entry but keeps both pins until the caller either
/// commits the removal after successful dispatch admission or cancels the claim. Operations may
/// spin and are forbidden from interrupt context and same-CPU reentry. Selection orders scheduling
/// class, criticality, priority, the effective deadline at the caller's sample for the deadline
/// class, then FIFO enqueue sequence. Weight is retained for the later weighted-fair accounting
/// policy and does not affect this bounded selector.
class ReadyQueue final
{
public:
    /// Preconditions:
    /// - objectRegistry outlives this queue and every returned claim.
    /// - Each SchedulingContext and Thread may belong to at most one ready
    /// queue globally. Enqueue enforces both memberships.
    /// - While queued or claimed, direct Thread and SchedulingContext state
    /// transitions and registry destruction of either generation are
    /// excluded by the scheduler policy lock. Removal or successful claim
    /// handoff precedes any destruction request.
    ReadyQueue(Object::ObjectRegistry& objectRegistry, CpuId processor) noexcept
        : m_objectRegistry{objectRegistry}, m_processor{processor}
    {
    }

    /// Releases all retained memberships and pins without acquiring the queue
    /// lock.
    /// Preconditions:
    /// - No claim or concurrent operation remains.
    ~ReadyQueue() noexcept;

    ReadyQueue(const ReadyQueue&) = delete;
    ReadyQueue& operator=(const ReadyQueue&) = delete;
    ReadyQueue(ReadyQueue&&) = delete;
    ReadyQueue& operator=(ReadyQueue&&) = delete;

    /// Preconditions:
    /// - The supplied object storage remains alive and immovable throughout
    /// this call.
    /// - Direct state transitions on both objects are excluded throughout this
    /// call and remain excluded after success until removal or claim handoff.
    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>
    Enqueue(SchedulingContextId contextId, SchedulingContext& context, ThreadId threadId, Thread& thread) noexcept;

    /// Retains one exact blocked caller, its SchedulingContext, global queue
    /// memberships, and a FIFO sequence before a ReplyToken commits. The
    /// caller must belong to the context's donation chain. The reservation
    /// is invisible to ClaimNext and must be resolved exactly once.
    /// The caller excludes direct lifecycle and membership transitions from
    /// this call through CommitReservedEnqueue or CancelReservedEnqueue.
    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>
    ReserveBlockedEnqueue(SchedulingContextId contextId, SchedulingContext& context, ThreadId threadId, Thread& thread,
                          ThreadBlockToken blockToken) noexcept;

    /// Publishes a reserved caller after its exact block token was woken and
    /// its donated SchedulingContext was returned. No registry pin, capacity,
    /// membership, or sequence allocation remains after reservation success.
    [[nodiscard]] Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>
    CommitReservedEnqueue(ReadyQueueEnqueueReservation reservation) noexcept;

    /// Releases the reserved memberships, pins, and slot without waking or
    /// otherwise changing the retained Thread or SchedulingContext.
    [[nodiscard]] ReadyQueueError CancelReservedEnqueue(ReadyQueueEnqueueReservation reservation) noexcept;

    /// Selects the best ready entry and marks it claimed without releasing pins. DeadlineSporadic
    /// entries rank by SchedulingContext::GetEffectiveDeadline at now, the caller's sample of the
    /// processor clock; the queue itself reads no clock. Candidates are revalidated in rank order:
    /// an entry whose Thread is no longer Ready or whose context is no longer runnable with that
    /// current Thread is skipped and stays Ready. Empty means that no ready entry revalidated.
    [[nodiscard]] Foundation::Result<ReadyQueueClaim, ReadyQueueFailure> ClaimNext(Object::MonotonicTime now) noexcept;

    /// Removes a claimed entry after another owner retained everything needed
    /// for dispatch. The claim becomes stale and both queue pins are released.
    [[nodiscard]] ReadyQueueError CommitClaim(ReadyQueueClaimToken token) noexcept;

    /// Restores a claimed entry to its original ready position.
    /// Preconditions:
    /// - Work attempted under this claim did not change either retained state
    /// machine, or it completely rolled those changes back.
    [[nodiscard]] ReadyQueueError CancelClaim(ReadyQueueClaimToken token) noexcept;

    /// Restores a successfully completed Ready dispatch as a new FIFO entry
    /// without releasing either generation pin or its fixed-capacity slot.
    /// The claim becomes stale. Failure leaves it claimed and unchanged.
    [[nodiscard]] ReadyQueueError RequeueClaim(ReadyQueueClaimToken token) noexcept;

    /// Removes one unclaimed Thread generation and releases both pins.
    [[nodiscard]] ReadyQueueError Remove(ThreadId thread) noexcept;

    [[nodiscard]] ReadyQueueState GetState() const noexcept;

private:
    enum class EntryState : Foundation::UInt8
    {
        Empty,
        Reserved,
        Ready,
        Claimed,
    };

    struct Entry
    {
        Object::ObjectPin ContextPin;
        Object::ObjectPin ThreadPin;
        SchedulingContext* Context;
        Thread* ThreadStorage;
        SchedulingContextId ContextIdentity;
        ThreadId ThreadIdentity;
        SchedulingContextConfiguration Configuration;
        /// Cached effective deadline, valid for samples from
        /// RankSampleTicks up to, but excluding, RankDeadlineTicks while the
        /// entry stays published; zero forces a recomputation.
        Foundation::UInt64 RankDeadlineTicks;
        Foundation::UInt64 RankSampleTicks;
        Foundation::UInt64 Sequence;
        Foundation::UInt64 ClaimSequence;
        EntryState State;
        Foundation::UInt8 Reserved[7];
    };

    [[nodiscard]] static ReadyQueueFailure
    MakeFailure(ReadyQueueError error, Object::ObjectPinError contextPin = Object::ObjectPinError::Ok,
                Object::ObjectPinError threadPin = Object::ObjectPinError::Ok) noexcept;
    [[nodiscard]] static Foundation::UInt8 ClassRank(SchedulingClass schedulingClass) noexcept;
    [[nodiscard]] static Foundation::UInt64 RankDeadlineLocked(Entry& entry, Object::MonotonicTime now) noexcept;
    [[nodiscard]] static bool IsBetterCandidateLocked(Entry& candidate, Entry& current,
                                                      Object::MonotonicTime now) noexcept;
    static void ResetRankLocked(Entry& entry) noexcept;
    /// Returns the index of the best Ready entry outside excludedEntries,
    /// or MaximumReadyQueueEntries when none remains.
    [[nodiscard]] Foundation::Size SelectBestLocked(Object::MonotonicTime now,
                                                    Foundation::UInt32 excludedEntries) noexcept;
    [[nodiscard]] static bool IsRunnablePairLocked(const Entry& entry) noexcept;
    [[nodiscard]] Entry* FindClaimLocked(ReadyQueueClaimToken token) noexcept;
    [[nodiscard]] Entry* FindReservationLocked(ReadyQueueEnqueueReservation reservation) noexcept;
    [[nodiscard]] ReadyQueueError ClearEntryLocked(Entry& entry) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    CpuId m_processor;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Entry m_entries[MaximumReadyQueueEntries]{};
    Foundation::UInt64 m_nextSequence{1U};
    Foundation::UInt64 m_nextClaimSequence{1U};
    Foundation::Size m_readyEntries{0U};
    Foundation::Size m_claimedEntries{0U};
    Foundation::Size m_reservedEntries{0U};
};

static_assert(sizeof(ReadyQueueError) == 1U);
static_assert(sizeof(ReadyQueueFailure) == 4U);
static_assert(sizeof(ReadyQueueEnqueueReceipt) == 8U);
static_assert(sizeof(ReadyQueueState) == 16U);
static_assert(alignof(ReadyQueueState) == 8U);
static_assert(sizeof(ReadyQueueEnqueueReservation) == 16U);
static_assert(alignof(ReadyQueueEnqueueReservation) == 8U);
static_assert(sizeof(ReadyQueueClaimToken) == 16U);
static_assert(alignof(ReadyQueueClaimToken) == 8U);
static_assert(sizeof(ReadyQueueClaim) == 120U);
static_assert(alignof(ReadyQueueClaim) == 8U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_READY_QUEUE_HPP_INCLUDED
