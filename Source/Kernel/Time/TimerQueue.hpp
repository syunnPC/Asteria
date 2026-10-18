// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_TIME_TIMER_QUEUE_HPP_INCLUDED
#define ASTERIA_KERNEL_TIME_TIMER_QUEUE_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Time/Timer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Time
{
constexpr Foundation::Size MaximumTimerQueueEntries{32U};

enum class TimerQueueError : Foundation::UInt8
{
    None,
    InvalidTimer,
    TimerRegistryMismatch,
    ClockMismatch,
    QueueFull,
    DuplicateTimer,
    TimerPinRejected,
    TimerInstanceMismatch,
    TimerRejected,
    TimerNotFound,
    EntryClaimed,
    QueueCorrupt,
    ClaimSequenceExhausted,
    Empty,
    NotDue,
    InvalidClaim,
    StaleClaim,
};

struct TimerQueueFailure
{
    TimerFailure Timer;
    Foundation::UInt64 NextDeadlineTicks;
    TimerQueueError Error;
    Object::ObjectPinError Pin;
    Foundation::UInt8 Reserved[6];
};

struct TimerQueueArmReceipt
{
    TimerArmReceipt Timer;
    Foundation::UInt64 NextDeadlineTicks;
};

struct TimerQueueCancelReceipt
{
    TimerSnapshot Timer;
    Foundation::UInt64 NextDeadlineTicks;
};

struct TimerQueueState
{
    Foundation::UInt64 NextDeadlineTicks;
    Foundation::UInt64 NextClaimSequence;
    Foundation::UInt8 ScheduledEntries;
    Foundation::UInt8 PendingEntries;
    Foundation::UInt8 ClaimedEntries;
    Foundation::UInt8 Reserved[5];
};

class TimerQueue;

/// Names one outstanding delivery claim from one TimerQueue. Cancelling a
/// claim makes every copy stale even if the same expiration is claimed again.
/// The originating queue outlives every copy and use of this token.
class TimerQueueClaimToken final
{
public:
    constexpr TimerQueueClaimToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_queue != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const TimerQueueClaimToken other) const noexcept
    {
        return (m_queue == other.m_queue) && (m_sequence == other.m_sequence);
    }

private:
    friend class TimerQueue;

    constexpr TimerQueueClaimToken(const TimerQueue& queue, const Foundation::UInt64 sequence) noexcept
        : m_queue{&queue}, m_sequence{sequence}
    {
    }

    const TimerQueue* m_queue{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

/// Copyable view of one claimed expiration. The queue retains the exact Timer
/// generation and storage until CommitClaim or RetireClaim consumes
/// ClaimToken.
class TimerQueueExpirationClaim final
{
public:
    constexpr TimerQueueExpirationClaim() noexcept = default;

    [[nodiscard]] constexpr TimerQueueClaimToken GetClaimToken() const noexcept
    {
        return m_claimToken;
    }

    [[nodiscard]] constexpr TimerId GetTimerId() const noexcept
    {
        return m_timerId;
    }

    [[nodiscard]] constexpr TimerExpiration GetExpiration() const noexcept
    {
        return m_expiration;
    }

    /// Preconditions:
    /// - ClaimToken still names an unresolved claim in the originating queue.
    [[nodiscard]] Timer& GetTimer() const noexcept
    {
        return *m_timer;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetNextDeadlineTicks() const noexcept
    {
        return m_nextDeadlineTicks;
    }

private:
    friend class TimerQueue;

    constexpr TimerQueueExpirationClaim(const TimerQueueClaimToken claimToken, const TimerId timerId,
                                        const TimerExpiration expiration, Timer& timer,
                                        const Foundation::UInt64 nextDeadlineTicks) noexcept
        : m_claimToken{claimToken}, m_timerId{timerId}, m_expiration{expiration}, m_timer{&timer},
          m_nextDeadlineTicks{nextDeadlineTicks}
    {
    }

    TimerQueueClaimToken m_claimToken{};
    TimerId m_timerId{};
    TimerExpiration m_expiration{};
    Timer* m_timer{nullptr};
    Foundation::UInt64 m_nextDeadlineTicks{0U};
};

/// Fixed-capacity ordered set of kernel Timer generations. Arm owns the whole Timer
/// arm-and-placement transaction and retains an ObjectRegistry pin; every arm, including the re-arm
/// of a Scheduled entry, requires the exact Timer generation to be pinnable. ClaimDue moves the
/// earliest due entry into a delivery claim without releasing that pin. A retryable delivery
/// failure returns it to Pending, a permanently undeliverable expiration is retired, and a
/// successful delivery commits the claim; the last two release queue ownership. Equal deadlines are
/// ordered by slot index, not by arrival. Operations are bounded linear scans, may spin, and are
/// forbidden from interrupt context and same-CPU reentry. A deferred interrupt worker invokes
/// ClaimDue; the hardware ISR itself does not acquire this lock.
class TimerQueue final
{
public:
    /// Preconditions:
    /// - objectRegistry and clock outlive this queue and every returned claim.
    /// - Every Timer admitted to this queue uses this exact clock instance.
    TimerQueue(Object::ObjectRegistry& objectRegistry, IMonotonicClock& clock) noexcept
        : m_objectRegistry{objectRegistry}, m_clock{clock}
    {
    }

    /// Cancels scheduled entries and releases all retained pins. Expirations
    /// already pending delivery remain Expired but cease to be queue-managed.
    /// Preconditions:
    /// - No external claim or concurrent operation remains.
    ~TimerQueue() noexcept;

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;
    TimerQueue(TimerQueue&&) = delete;
    TimerQueue& operator=(TimerQueue&&) = delete;

    /// Atomically arms and places timer. Re-arming an entry already scheduled
    /// in this queue replaces its generation without a lifetime gap; like a
    /// new arm, it is rejected with TimerPinRejected once the Timer's registry
    /// generation is being destroyed.
    [[nodiscard]] Foundation::Result<TimerQueueArmReceipt, TimerQueueFailure>
    Arm(TimerId timerId, Timer& timer, Object::MonotonicTime deadline) noexcept;

    /// Cancels and removes one exact scheduled generation.
    [[nodiscard]] Foundation::Result<TimerQueueCancelReceipt, TimerQueueFailure> Cancel(TimerId timerId, Timer& timer,
                                                                                        TimerToken token) noexcept;

    /// Claims the earliest pending expiration, or samples the earliest
    /// scheduled Timer once and claims it if due. NotDue reports the deadline
    /// that a hardware adapter should program next.
    [[nodiscard]] Foundation::Result<TimerQueueExpirationClaim, TimerQueueFailure> ClaimDue() noexcept;

    /// Stack-bounded form of ClaimDue for live deferred-worker call chains.
    /// Exactly one output is meaningful: claim on true, failure on false.
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool ClaimDue(TimerQueueExpirationClaim& claim,
                                                       TimerQueueFailure& failure) noexcept;

    /// Validates and canonicalizes one claimed-expiration copy while holding this queue's lock. The
    /// candidate's Timer pointer is compared as an address only and is never dereferenced. Success
    /// proves that the exact claim sequence, Timer identity and storage, and every expiration field
    /// still match one live Claimed entry in this queue. Preconditions: - The caller owns the
    /// external serialization domain for this queue and retains it through the later CommitClaim or
    /// CancelClaim operation; - candidate and every returned copy remain unresolved during that
    /// interval.
    [[nodiscard]] Foundation::Result<TimerQueueExpirationClaim, TimerQueueFailure>
    ValidateClaimForDelivery(const TimerQueueExpirationClaim& candidate) noexcept;

    /// Stack-bounded form of ValidateClaimForDelivery. The candidate Timer
    /// pointer is compared as an address and is never dereferenced.
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool ValidateClaimForDelivery(const TimerQueueExpirationClaim& candidate,
                                                                       TimerQueueExpirationClaim& canonical,
                                                                       TimerQueueFailure& failure) noexcept;

    /// Completes delivery, removes the entry, and releases the queue pin.
    /// Any later acknowledgement obtains its own registry pin or otherwise
    /// proves the Timer remains alive.
    [[nodiscard]] Foundation::Result<TimerQueueState, TimerQueueFailure>
    CommitClaim(TimerQueueClaimToken token) noexcept;

    /// Stack-bounded form of CommitClaim for an externally serialized caller.
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool CommitClaim(TimerQueueClaimToken token, TimerQueueState& state,
                                                          TimerQueueFailure& failure) noexcept;

    /// Restores an unsuccessful delivery claim to Pending. The Timer remains
    /// Expired and will be selected again before newly due scheduled entries.
    /// Every copy of the claim becomes stale; the next ClaimDue of the entry
    /// issues a new claim sequence.
    [[nodiscard]] Foundation::Result<TimerQueueState, TimerQueueFailure>
    CancelClaim(TimerQueueClaimToken token) noexcept;

    /// Stack-bounded form of CancelClaim for an externally serialized caller.
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool CancelClaim(TimerQueueClaimToken token, TimerQueueState& state,
                                                          TimerQueueFailure& failure) noexcept;

    /// Resolves a claim whose expiration can never be delivered, for example
    /// because its Timer or Notification generation was destroyed. While the
    /// queue pin still proves the Timer's lifetime, the Timer consumes the
    /// expiration and returns to Disarmed, as a committed delivery followed
    /// by acknowledgement would leave it; the entry is then removed and the
    /// pin released. The caller establishes that delivery is impossible.
    [[nodiscard]] Foundation::Result<TimerQueueState, TimerQueueFailure>
    RetireClaim(TimerQueueClaimToken token) noexcept;

    /// Stack-bounded form of RetireClaim for an externally serialized caller.
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool RetireClaim(TimerQueueClaimToken token, TimerQueueState& state,
                                                          TimerQueueFailure& failure) noexcept;

    [[nodiscard]] TimerQueueState GetState() const noexcept;

private:
    enum class EntryState : Foundation::UInt8
    {
        Empty,
        Scheduled,
        Pending,
        Claimed,
    };

    struct Entry
    {
        Object::ObjectPin Pin;
        TimerId Identity;
        Timer* Storage;
        TimerToken Token;
        Foundation::UInt64 DeadlineTicks;
        Foundation::UInt64 ObservedTicks;
        Foundation::UInt64 ClaimSequence;
        EntryState State;
    };

    [[nodiscard]] static TimerQueueFailure
    MakeFailure(TimerQueueError error, Foundation::UInt64 nextDeadlineTicks, const TimerFailure& timerFailure = {},
                Object::ObjectPinError pinError = Object::ObjectPinError::Ok) noexcept;
    static void SetFailure(TimerQueueFailure& failure, TimerQueueError error, Foundation::UInt64 nextDeadlineTicks,
                           const TimerFailure& timerFailure = {},
                           Object::ObjectPinError pinError = Object::ObjectPinError::Ok) noexcept;
    [[nodiscard]] bool ArmLocked(TimerId timerId, Timer& timer, Object::MonotonicTime deadline,
                                 TimerQueueArmReceipt& receipt, TimerQueueFailure& failure) noexcept;
    [[nodiscard]] bool CancelLocked(TimerId timerId, Timer& timer, TimerToken token, TimerQueueCancelReceipt& receipt,
                                    TimerQueueFailure& failure) noexcept;
    [[nodiscard]] bool ClaimDueLocked(TimerQueueExpirationClaim& claim, TimerQueueFailure& failure) noexcept;
    [[nodiscard]] Entry* SelectDueEntryLocked(TimerQueueFailure& failure) noexcept;
    [[nodiscard]] bool ExpireSelectedEntryLocked(Entry& entry, TimerQueueFailure& failure) noexcept;
    void PublishSelectedClaimLocked(Entry& entry, TimerQueueExpirationClaim& claim) noexcept;
    [[nodiscard]] bool ValidateClaimForDeliveryLocked(const TimerQueueExpirationClaim& candidate,
                                                      TimerQueueExpirationClaim& canonical,
                                                      TimerQueueFailure& failure) noexcept;
    [[nodiscard]] bool CommitClaimLocked(TimerQueueClaimToken token, TimerQueueState& state,
                                         TimerQueueFailure& failure) noexcept;
    [[nodiscard]] bool CancelClaimLocked(TimerQueueClaimToken token, TimerQueueState& state,
                                         TimerQueueFailure& failure) noexcept;
    [[nodiscard]] bool RetireClaimLocked(TimerQueueClaimToken token, TimerQueueState& state,
                                         TimerQueueFailure& failure) noexcept;
    [[nodiscard]] Entry* FindUnresolvedClaimLocked(TimerQueueClaimToken token, TimerQueueFailure& failure) noexcept;
    /// Rejects the re-arm of an existing entry, exactly as a new arm would be
    /// rejected, once timerId's generation can no longer be pinned. Out of
    /// line so that ArmLocked's frame stays within the target stack budget.
    [[gnu::noinline]] [[nodiscard]] bool ValidateRearmedGenerationLocked(TimerId timerId,
                                                                         Foundation::UInt64 previousNextDeadline,
                                                                         TimerQueueFailure& failure) noexcept;
    [[nodiscard]] Entry* FindTimerLocked(TimerId timerId) noexcept;
    [[nodiscard]] Entry* FindClaimLocked(TimerQueueClaimToken token) noexcept;
    [[nodiscard]] Entry* FindEarliestLocked(EntryState state) noexcept;
    [[nodiscard]] Foundation::UInt64 GetNextDeadlineLocked() const noexcept;
    [[nodiscard]] TimerQueueState GetStateLocked() const noexcept;
    void ClearEntryLocked(Entry& entry) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    IMonotonicClock& m_clock;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Entry m_entries[MaximumTimerQueueEntries]{};
    TimerExpiration m_expirationScratch{};
    Foundation::UInt64 m_nextClaimSequence{1U};
    Foundation::Size m_scheduledEntries{0U};
    Foundation::Size m_pendingEntries{0U};
    Foundation::Size m_claimedEntries{0U};
};

static_assert(sizeof(TimerQueueError) == 1U);
static_assert(sizeof(TimerQueueFailure) == 32U);
static_assert(alignof(TimerQueueFailure) == 8U);
static_assert(sizeof(TimerQueueArmReceipt) == 48U);
static_assert(alignof(TimerQueueArmReceipt) == 8U);
static_assert(sizeof(TimerQueueCancelReceipt) == 40U);
static_assert(alignof(TimerQueueCancelReceipt) == 8U);
static_assert(sizeof(TimerQueueState) == 24U);
static_assert(alignof(TimerQueueState) == 8U);
static_assert(sizeof(TimerQueueClaimToken) == 16U);
static_assert(alignof(TimerQueueClaimToken) == 8U);
static_assert(sizeof(TimerQueueExpirationClaim) == 80U);
static_assert(alignof(TimerQueueExpirationClaim) == 8U);
} // Namespace Asteria::Kernel::Time

#endif // ASTERIA_KERNEL_TIME_TIMER_QUEUE_HPP_INCLUDED
