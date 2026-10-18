// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_TIME_TIMER_NOTIFICATION_DELIVERY_HPP_INCLUDED
#define ASTERIA_KERNEL_TIME_TIMER_NOTIFICATION_DELIVERY_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Time/TimerQueue.hpp"

namespace Asteria::Kernel::Time
{
enum class TimerNotificationDeliveryError : Foundation::UInt8
{
    None,
    QueueClaimRejected,
    InvalidClaim,
    InvalidTimer,
    TimerRegistryMismatch,
    ClaimedTimerMismatch,
    ClaimedTimerStateMismatch,
    TimerPinRejected,
    TimerInstanceMismatch,
    InvalidNotification,
    NotificationRegistryMismatch,
    NotificationPinRejected,
    NotificationInstanceMismatch,
    NotificationRejected,
    ClaimCancellationRejected,
    ClaimCommitRejected,
    TimerAcknowledgeRejected,
    ClaimRetirementRejected,
};

/// Says what a Notification pin rejection proves about the destination.
enum class TimerNotificationDestination : Foundation::UInt8
{
    /// The caller names the Notification for this attempt only; it may retry
    /// the restored claim with another destination.
    Transient,
    /// The caller holds its own pin on the exact Notification generation for
    /// as long as it delivers this Timer to it, as a DeferredTimerWorker
    /// binding does. A pin rejected as ObjectNotFound therefore proves that
    /// the generation is being destroyed and can never receive the expiration.
    Pinned,
};

enum class TimerNotificationDeliveryOutcome : Foundation::UInt8
{
    Delivered,
    Rejected,
};

struct TimerNotificationDeliveryReceipt
{
    TimerId TimerIdentity;
    Ipc::NotificationId NotificationIdentity;
    TimerExpiration Expiration;
    TimerQueueState QueueState;
    Ipc::NotificationSnapshot NotificationState;
    Foundation::UInt64 ClaimSequence;
    Foundation::UInt8 NotificationPublished;
    Foundation::UInt8 QueueCommitted;
    Foundation::UInt8 Reserved[6];
};

/// Complete diagnostic for pre-publication failures and the terminal-looking
/// contradictions that can follow Notification publication. ClaimRetired is
/// one when the failure proved the expiration undeliverable and the claim was
/// retired instead of restored to Pending. Cause is non-None only when that
/// restoration or retirement itself failed and Error is
/// ClaimCancellationRejected or ClaimRetirementRejected.
struct TimerNotificationDeliveryFailure
{
    TimerQueueFailure Queue;
    TimerFailure Timer;
    TimerId TimerIdentity;
    Ipc::NotificationId NotificationIdentity;
    TimerExpiration Expiration;
    TimerQueueState QueueState;
    Ipc::NotificationSnapshot NotificationState;
    Foundation::UInt64 ClaimSequence;
    TimerNotificationDeliveryError Error;
    TimerNotificationDeliveryError Cause;
    Object::ObjectPinError TimerPin;
    Object::ObjectPinError NotificationPin;
    Ipc::NotificationError Notification;
    Foundation::UInt8 NotificationPublished;
    Foundation::UInt8 QueueCommitted;
    Foundation::UInt8 ClaimRetired;
};

/// Publishes one due Timer expiration to one exact Notification generation. Deliver claims from
/// timerQueue. DeliverClaimed adopts one exact claim that the externally serialized caller obtained
/// from that same queue. Both paths take a second independent Timer pin before releasing the queue
/// pin, pin the Notification, publish its coalescing bit, commit the queue claim, and acknowledge
/// the exact Timer token. After successful adoption, a failure before publication restores the
/// claim to Pending for a retry, unless it proves the expiration can never be delivered: the
/// claimed Timer generation, or a Pinned destination's Notification generation, can no longer be
/// pinned because it is being destroyed. Such a claim is retired (TimerQueue: RetireClaim), which
/// consumes the expiration and releases the queue pin. Invalid claim copies are rejected without
/// mutating either queue. A contradiction after publication is reported without attempting to undo
/// the Notification. Blocking: - May spin on registry, TimerQueue, Timer, and Notification locks.
/// Interrupt safety: - Must not be called from an ISR or a same-CPU reentrant path. Thread safety:
/// - Not thread-safe. The caller exclusively serializes the complete duration of every delivery
/// call on this exact transaction object across all CPUs. Allocation: - Does not allocate.
class TimerNotificationDelivery final
{
public:
    /// Preconditions:
    /// - objectRegistry and timerQueue outlive this transaction object;
    /// - timerQueue uses objectRegistry for every admitted Timer;
    /// - direct claim completion and Timer acknowledgement are externally
    /// serialized with Deliver.
    constexpr TimerNotificationDelivery(Object::ObjectRegistry& objectRegistry, TimerQueue& timerQueue) noexcept
        : m_objectRegistry{objectRegistry}, m_timerQueue{timerQueue}
    {
    }

    ~TimerNotificationDelivery() noexcept = default;

    TimerNotificationDelivery(const TimerNotificationDelivery&) = delete;
    TimerNotificationDelivery& operator=(const TimerNotificationDelivery&) = delete;
    TimerNotificationDelivery(TimerNotificationDelivery&&) = delete;
    TimerNotificationDelivery& operator=(TimerNotificationDelivery&&) = delete;

    [[nodiscard]] Foundation::Result<TimerNotificationDeliveryReceipt, TimerNotificationDeliveryFailure>
    Deliver(TimerId timerId, Timer& timer, Ipc::NotificationId notificationId,
            Ipc::Notification& notification) noexcept;

    /// Delivers one exact claim already obtained from this transaction's TimerQueue. The queue
    /// first validates the claim sequence, identity, storage address, and expiration fields without
    /// dereferencing candidate storage. Only the canonical live claim is then independently pinned.
    /// Stale, foreign, and consumed copies are rejected before publication. Preconditions: - claim
    /// was returned by this transaction's timerQueue; - claim remains unresolved; - direct claim
    /// completion and Timer acknowledgement remain externally serialized with this call.
    [[nodiscard]] Foundation::Result<TimerNotificationDeliveryReceipt, TimerNotificationDeliveryFailure>
    DeliverClaimed(const TimerQueueExpirationClaim& claim, Ipc::NotificationId notificationId,
                   Ipc::Notification& notification) noexcept;

    /// Executes the same transaction as Deliver while retaining the receipt
    /// or failure in this object. This small-return path avoids materializing
    /// the large Result union in a bounded-stack caller.
    ASTERIA_NOINLINE_NOIPA [[nodiscard]]
    TimerNotificationDeliveryOutcome DeliverRetained(TimerId timerId, Timer& timer, Ipc::NotificationId notificationId,
                                                     Ipc::Notification& notification) noexcept;

    /// Executes the same transaction as DeliverClaimed while retaining its
    /// output. Before any Timer dereference or Notification publication, the
    /// originating queue must validate and canonicalize the exact live claim.
    /// destination says whether a destroyed Notification generation makes the
    /// expiration undeliverable (Pinned) or leaves the claim to the caller
    /// (Transient, the behaviour of DeliverClaimed).
    ASTERIA_NOINLINE_NOIPA [[nodiscard]]
    TimerNotificationDeliveryOutcome
    DeliverClaimedRetained(const TimerQueueExpirationClaim& claim, Ipc::NotificationId notificationId,
                           Ipc::Notification& notification,
                           TimerNotificationDestination destination = TimerNotificationDestination::Transient) noexcept;

    /// Valid after a retained call returns Delivered and until the next call.
    [[nodiscard]] constexpr const TimerNotificationDeliveryReceipt& GetRetainedReceipt() const noexcept
    {
        return m_receipt;
    }

    /// Valid after a retained call returns Rejected and until the next call.
    [[nodiscard]] constexpr const TimerNotificationDeliveryFailure& GetRetainedFailure() const noexcept
    {
        return m_failure;
    }

private:
    void ResetDiagnostics(TimerId timerId, Ipc::NotificationId notificationId) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool ClaimDue(TimerId timerId, Ipc::NotificationId notificationId) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool AdoptClaim(const TimerQueueExpirationClaim& claim,
                                                    Ipc::NotificationId notificationId) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool ValidateAndPin(Ipc::Notification& notification) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool Publish(Ipc::Notification& notification) noexcept;
    [[gnu::noinline]] [[nodiscard]] bool CommitClaim() noexcept;
    [[gnu::noinline]] [[nodiscard]] bool AcknowledgeTimer() noexcept;
    [[gnu::noinline]] void RestoreClaimPending(TimerNotificationDeliveryError cause) noexcept;
    [[gnu::noinline]] void RetireUndeliverableClaim(TimerNotificationDeliveryError cause) noexcept;
    [[nodiscard]] bool ProvesUndeliverable(TimerNotificationDeliveryError error) const noexcept;
    void RejectBeforePublication(TimerNotificationDeliveryError error) noexcept;
    void MakeReceipt() noexcept;
    void ReleaseTransientState() noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]]
    TimerNotificationDeliveryOutcome CompleteDeliveryRetained(Ipc::Notification& notification) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    TimerQueue& m_timerQueue;
    Object::ObjectPin m_timerPin;
    Object::ObjectPin m_notificationPin;
    TimerQueueClaimToken m_claimToken{};
    TimerQueueExpirationClaim m_canonicalClaim{};
    TimerId m_requestedTimerIdentity{};
    Timer* m_claimedTimer{nullptr};
    TimerNotificationDestination m_destination{TimerNotificationDestination::Transient};
    TimerNotificationDeliveryReceipt m_receipt{};
    TimerNotificationDeliveryFailure m_failure{};
};

static_assert(sizeof(TimerNotificationDeliveryError) == 1U);
static_assert(sizeof(TimerNotificationDeliveryOutcome) == 1U);
static_assert(sizeof(TimerNotificationDestination) == 1U);
static_assert(sizeof(TimerNotificationDeliveryReceipt) == 120U);
static_assert(alignof(TimerNotificationDeliveryReceipt) == 8U);
static_assert(sizeof(TimerNotificationDeliveryFailure) == 168U);
static_assert(alignof(TimerNotificationDeliveryFailure) == 8U);
} // Namespace Asteria::Kernel::Time

#endif // ASTERIA_KERNEL_TIME_TIMER_NOTIFICATION_DELIVERY_HPP_INCLUDED
