// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_TIME_DEFERRED_TIMER_WORKER_HPP_INCLUDED
#define ASTERIA_KERNEL_TIME_DEFERRED_TIMER_WORKER_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Time/TimerNotificationDelivery.hpp"
#include "Kernel/Time/TimerQueue.hpp"

namespace Asteria::Kernel::Time
{
constexpr Foundation::Size MaximumDeferredTimerBindings{MaximumTimerQueueEntries};

enum class DeferredTimerWorkerState : Foundation::UInt8
{
    Ready,
    Draining,
    Faulted,
};

enum class DeferredTimerDrainStop : Foundation::UInt8
{
    Empty,
    NotDue,
    WorkLimit,
};

enum class DeferredTimerDrainOutcome : Foundation::UInt8
{
    Drained,
    Rejected,
};

enum class DeferredTimerWorkerError : Foundation::UInt8
{
    None,
    InvalidTimer,
    TimerRegistryMismatch,
    TimerNotInitialized,
    TimerPinRejected,
    TimerInstanceMismatch,
    InvalidNotification,
    NotificationRegistryMismatch,
    NotificationNotInitialized,
    NotificationPinRejected,
    NotificationInstanceMismatch,
    DuplicateTimer,
    BindingCapacityExceeded,
    RegistrationSequenceExhausted,
    DrainActive,
    Faulted,
    InvalidRegistration,
    StaleRegistration,
    TimerNotDisarmed,
    CorruptState,
    QueueRejected,
    BindingNotFound,
    ClaimCancellationRejected,
    DeliveryRejected,
    DeliveryTerminal,
    ClaimRetirementRejected,
};

class DeferredTimerWorkerSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    DeferredTimerWorkerSequencePolicy() = delete;

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

class DeferredTimerWorker;

/// Names one exact Timer-to-Notification binding generation.
/// Lifetime:
/// - The originating DeferredTimerWorker outlives every copy and use.
class DeferredTimerRegistrationToken final
{
public:
    constexpr DeferredTimerRegistrationToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_worker != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const DeferredTimerRegistrationToken other) const noexcept
    {
        return (m_worker == other.m_worker) && (m_sequence == other.m_sequence);
    }

private:
    friend class DeferredTimerWorker;

    constexpr DeferredTimerRegistrationToken(const DeferredTimerWorker& worker,
                                             const Foundation::UInt64 sequence) noexcept
        : m_worker{&worker}, m_sequence{sequence}
    {
    }

    const DeferredTimerWorker* m_worker{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

struct DeferredTimerRegistrationReceipt
{
    DeferredTimerRegistrationToken Registration;
    TimerId TimerIdentity;
    Ipc::NotificationId NotificationIdentity;
};

struct DeferredTimerWorkerSnapshot
{
    TimerQueueState QueueState;
    Foundation::UInt64 LastRegistrationSequence;
    DeferredTimerWorkerState State;
    Foundation::UInt8 RegisteredBindings;
    Foundation::UInt8 Reserved[6];
};

struct DeferredTimerDrainReceipt
{
    TimerId FirstTimerIdentity;
    TimerId LastTimerIdentity;
    TimerQueueState QueueState;
    Foundation::UInt64 FirstClaimSequence;
    Foundation::UInt64 LastClaimSequence;
    DeferredTimerDrainStop Stop;
    Foundation::UInt8 DeliveredCount;
    DeferredTimerWorkerState State;
    /// Expirations retired because they can never be delivered.
    Foundation::UInt8 RetiredCount;
    Foundation::UInt8 Reserved[4];
};

/// Compact public diagnostic. The complete most-recent delivery diagnostic is
/// retained by DeferredTimerWorker so DrainDue does not copy two large nested
/// transaction records through a target's bounded kernel stack.
struct DeferredTimerWorkerFailure
{
    TimerQueueFailure Queue;
    TimerQueueState QueueState;
    TimerId TimerIdentity;
    Ipc::NotificationId NotificationIdentity;
    Foundation::UInt64 RegistrationSequence;
    TimerNotificationDeliveryError Delivery;
    Object::ObjectPinError TimerPin;
    Object::ObjectPinError NotificationPin;
    Ipc::NotificationError Notification;
    TimerState Timer;
    DeferredTimerWorkerError Error;
    DeferredTimerWorkerState State;
    Foundation::UInt8 DeliveredCount;
    Foundation::UInt8 ClaimRestored;
    Foundation::UInt8 NotificationPublished;
    Foundation::UInt8 QueueCommitted;
    Foundation::UInt8 Reserved[5];
};

/// Fixed-capacity normal-context Timer-to-Notification delivery worker. Each binding pins the exact
/// Timer and Notification generations. DrainDue claims at most MaximumTimerQueueEntries
/// expirations, resolves every claim against the exact Timer identity and storage, and delegates
/// publication to TimerNotificationDelivery with the binding as a Pinned destination. Empty and
/// NotDue are successful stop reasons. An expiration that can never be delivered, because its Timer
/// or its binding's Notification was destroyed while the Timer was queued, is retired and the drain
/// continues. A missing binding of a live Timer and every other pre-publication failure restore the
/// claim to Pending and stop, so a later drain retries it. A contradiction after Notification
/// publication, or a failed claim restoration or retirement, faults this worker permanently.
/// Blocking: - May spin on registry, TimerQueue, Timer, and Notification locks. Interrupt safety: -
/// Must not be called from an ISR or a same-CPU reentrant path. Thread safety: - Not thread-safe.
/// One owning CPU externally serializes every operation, all mutations of timerQueue, and direct
/// claim completion. Allocation: - Does not allocate. All scans and drain loops have fixed bounds.
class DeferredTimerWorker final
{
public:
    /// Preconditions:
    /// - objectRegistry and timerQueue outlive this worker;
    /// - timerQueue uses objectRegistry for every admitted Timer;
    /// - every Timer admitted while DrainDue is used either has an exact live
    /// binding or is intentionally recoverable through BindingNotFound; a
    /// Timer destroyed while queued needs neither, its expiration is retired.
    constexpr DeferredTimerWorker(Object::ObjectRegistry& objectRegistry, TimerQueue& timerQueue) noexcept
        : m_objectRegistry{objectRegistry}, m_timerQueue{timerQueue}, m_delivery{objectRegistry, timerQueue}
    {
    }

    /// Releases every retained binding pin.
    /// Preconditions:
    /// - no operation or token use is concurrent;
    /// - DrainDue is not active.
    ~DeferredTimerWorker() noexcept;

    DeferredTimerWorker(const DeferredTimerWorker&) = delete;
    DeferredTimerWorker& operator=(const DeferredTimerWorker&) = delete;
    DeferredTimerWorker(DeferredTimerWorker&&) = delete;
    DeferredTimerWorker& operator=(DeferredTimerWorker&&) = delete;

    [[nodiscard]] Foundation::Result<DeferredTimerRegistrationReceipt, DeferredTimerWorkerFailure>
    Register(TimerId timerId, Timer& timer, Ipc::NotificationId notificationId,
             Ipc::Notification& notification) noexcept;

    /// Removes one exact live binding. The Timer must be Disarmed, which also
    /// proves that it is not managed by a TimerQueue under Timer's state
    /// invariant. Faulted workers may still release safe bindings.
    [[nodiscard]] Foundation::Result<DeferredTimerWorkerSnapshot, DeferredTimerWorkerFailure>
    Unregister(DeferredTimerRegistrationToken registration) noexcept;

    /// Delivers due work until the queue is Empty, the earliest Timer is
    /// NotDue, one operation fails, or MaximumTimerQueueEntries claims have
    /// been delivered or retired.
    [[nodiscard]] Foundation::Result<DeferredTimerDrainReceipt, DeferredTimerWorkerFailure> DrainDue() noexcept;

    /// Executes DrainDue while retaining its receipt or failure in this worker.
    /// The small outcome keeps the complete target worker call chain bounded.
    ASTERIA_NOINLINE_NOIPA [[nodiscard]]
    DeferredTimerDrainOutcome DrainDueRetained() noexcept;

    /// Valid after DrainDueRetained returns Drained and until the next drain.
    [[nodiscard]] constexpr const DeferredTimerDrainReceipt& GetRetainedDrainReceipt() const noexcept
    {
        return m_drainReceipt;
    }

    /// Valid after DrainDueRetained returns Rejected and until the next drain.
    [[nodiscard]] constexpr const DeferredTimerWorkerFailure& GetRetainedDrainFailure() const noexcept
    {
        return m_retainedDrainFailure;
    }

    [[nodiscard]] DeferredTimerWorkerSnapshot GetSnapshot() const noexcept;

    /// Valid until the next DrainDue call. A successful zero-delivery drain
    /// returns a default record.
    [[nodiscard]] constexpr const TimerNotificationDeliveryReceipt& GetLastDeliveryReceipt() const noexcept
    {
        return m_lastDeliveryReceipt;
    }

    /// Valid until the next DrainDue call. A successful drain returns a
    /// default record unless the delivery transaction retired an expiration;
    /// the record then describes the last such retirement.
    [[nodiscard]] constexpr const TimerNotificationDeliveryFailure& GetLastDeliveryFailure() const noexcept
    {
        return m_lastDeliveryFailure;
    }

    [[nodiscard]] static constexpr bool
    IsTerminalDeliveryFailure(const TimerNotificationDeliveryFailure& failure) noexcept
    {
        return (failure.NotificationPublished != 0U) || (failure.QueueCommitted != 0U) ||
               (failure.Error == TimerNotificationDeliveryError::ClaimCancellationRejected) ||
               (failure.Error == TimerNotificationDeliveryError::ClaimRetirementRejected);
    }

private:
    enum class EntryState : Foundation::UInt8
    {
        Empty,
        Registered,
    };

    struct Entry
    {
        Object::ObjectPin m_timerPin;
        Object::ObjectPin m_notificationPin;
        TimerId m_timerIdentity;
        Ipc::NotificationId m_notificationIdentity;
        Timer* m_timerStorage;
        Ipc::Notification* m_notificationStorage;
        Foundation::UInt64 m_registrationSequence;
        EntryState m_state;
    };

    void ResetFailure() noexcept;
    void SetFailure(DeferredTimerWorkerError error) noexcept;
    [[nodiscard]] Entry* FindRegistration(DeferredTimerRegistrationToken registration) noexcept;
    [[nodiscard]] Entry* ResolveActiveBinding(bool& identityContradiction) noexcept;
    [[nodiscard]] static bool EntryIsClear(const Entry& entry) noexcept;
    [[nodiscard]] static bool EntryIsValid(const Entry& entry) noexcept;
    static void ClearEntry(Entry& entry) noexcept;
    [[nodiscard]] bool RestoreActiveClaim() noexcept;
    [[gnu::noinline]] [[nodiscard]] bool ActiveTimerGenerationEnded() noexcept;
    [[nodiscard]] bool RetireActiveClaim() noexcept;
    void RetainDeliveryFailure(const TimerNotificationDeliveryFailure& failure, TimerState timerState,
                               Foundation::UInt64 registrationSequence) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool DeliverActive(Entry& entry) noexcept;
    void FinishDrainRetained(DeferredTimerDrainStop stop) noexcept;
    [[nodiscard]] DeferredTimerDrainOutcome RejectDrainRetained() noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    TimerQueue& m_timerQueue;
    TimerNotificationDelivery m_delivery;
    Entry m_entries[MaximumDeferredTimerBindings]{};
    TimerQueueExpirationClaim m_activeClaim{};
    DeferredTimerDrainReceipt m_drainReceipt{};
    TimerNotificationDeliveryReceipt m_lastDeliveryReceipt{};
    TimerNotificationDeliveryFailure m_lastDeliveryFailure{};
    DeferredTimerWorkerFailure m_failure{};
    DeferredTimerWorkerFailure m_retainedDrainFailure{};
    TimerId m_firstTimerIdentity{};
    TimerId m_lastTimerIdentity{};
    Foundation::UInt64 m_firstClaimSequence{0U};
    Foundation::UInt64 m_lastClaimSequence{0U};
    Foundation::UInt64 m_lastRegistrationSequence{0U};
    Foundation::Size m_registeredBindings{0U};
    Foundation::Size m_deliveredCount{0U};
    Foundation::Size m_retiredCount{0U};
    DeferredTimerWorkerState m_state{DeferredTimerWorkerState::Ready};
};

/// Internal transition rule of DeferredTimerWorker after
/// TimerNotificationDelivery rejects the active claim without retiring it; a
/// retired claim is resolved and the drain continues before this rule applies.
namespace Detail
{
struct DeferredTimerDeliveryRejection
{
    DeferredTimerWorkerState State;
    DeferredTimerWorkerError Error;
    Foundation::UInt8 ClaimRestored;
};

/// A terminal failure, as classified by IsTerminalDeliveryFailure, faults the
/// worker permanently. Every other rejection is retryable: the worker reports
/// the claim restored to Pending and remains Ready.
[[nodiscard]] constexpr DeferredTimerDeliveryRejection
ClassifyDeliveryRejection(const TimerNotificationDeliveryFailure& failure) noexcept
{
    if (DeferredTimerWorker::IsTerminalDeliveryFailure(failure))
    {
        return DeferredTimerDeliveryRejection{
            .State = DeferredTimerWorkerState::Faulted,
            .Error = DeferredTimerWorkerError::DeliveryTerminal,
            .ClaimRestored = 0U,
        };
    }
    return DeferredTimerDeliveryRejection{
        .State = DeferredTimerWorkerState::Ready,
        .Error = DeferredTimerWorkerError::DeliveryRejected,
        .ClaimRestored = 1U,
    };
}
} // Namespace Detail

static_assert(MaximumDeferredTimerBindings == MaximumTimerQueueEntries);
static_assert(sizeof(DeferredTimerWorkerState) == 1U);
static_assert(sizeof(DeferredTimerDrainStop) == 1U);
static_assert(sizeof(DeferredTimerDrainOutcome) == 1U);
static_assert(sizeof(DeferredTimerWorkerError) == 1U);
static_assert(sizeof(DeferredTimerRegistrationToken) == 16U);
static_assert(alignof(DeferredTimerRegistrationToken) == 8U);
static_assert(sizeof(DeferredTimerRegistrationReceipt) == 48U);
static_assert(alignof(DeferredTimerRegistrationReceipt) == 8U);
static_assert(sizeof(DeferredTimerWorkerSnapshot) == 40U);
static_assert(alignof(DeferredTimerWorkerSnapshot) == 8U);
static_assert(sizeof(DeferredTimerDrainReceipt) == 80U);
static_assert(alignof(DeferredTimerDrainReceipt) == 8U);
static_assert(sizeof(DeferredTimerWorkerFailure) == 112U);
static_assert(alignof(DeferredTimerWorkerFailure) == 8U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return DeferredTimerWorkerSequencePolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{17U};
        return !DeferredTimerWorkerSequencePolicy::TryAdvance(DeferredTimerWorkerSequencePolicy::Maximum, next) &&
               (next == 17U);
    }());
} // Namespace Asteria::Kernel::Time

#endif // ASTERIA_KERNEL_TIME_DEFERRED_TIMER_WORKER_HPP_INCLUDED
