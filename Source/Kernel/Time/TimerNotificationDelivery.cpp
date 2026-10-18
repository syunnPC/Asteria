// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Time/TimerNotificationDelivery.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Time
{
void TimerNotificationDelivery::ResetDiagnostics(const TimerId timerId,
                                                 const Ipc::NotificationId notificationId) noexcept
{
    m_failure.Queue.Timer.ObservedTicks = 0U;
    m_failure.Queue.Timer.Error = TimerError::None;
    for (Foundation::Size index{0U}; index < sizeof(m_failure.Queue.Timer.Reserved); ++index)
    {
        m_failure.Queue.Timer.Reserved[index] = 0U;
    }
    m_failure.Queue.NextDeadlineTicks = 0U;
    m_failure.Queue.Error = TimerQueueError::None;
    m_failure.Queue.Pin = Object::ObjectPinError::Ok;
    for (Foundation::Size index{0U}; index < sizeof(m_failure.Queue.Reserved); ++index)
    {
        m_failure.Queue.Reserved[index] = 0U;
    }
    m_failure.Timer.ObservedTicks = 0U;
    m_failure.Timer.Error = TimerError::None;
    for (Foundation::Size index{0U}; index < sizeof(m_failure.Timer.Reserved); ++index)
    {
        m_failure.Timer.Reserved[index] = 0U;
    }
    m_failure.TimerIdentity = timerId;
    m_failure.NotificationIdentity = notificationId;
    m_failure.Expiration.Token = TimerToken{};
    m_failure.Expiration.DeadlineTicks = 0U;
    m_failure.Expiration.ObservedTicks = 0U;
    m_failure.QueueState = m_timerQueue.GetState();
    m_failure.NotificationState.LastClaimSequence = 0U;
    m_failure.NotificationState.State = Ipc::NotificationState::Uninitialized;
    m_failure.NotificationState.Pending = 0U;
    for (Foundation::Size index{0U}; index < sizeof(m_failure.NotificationState.Reserved); ++index)
    {
        m_failure.NotificationState.Reserved[index] = 0U;
    }
    m_failure.ClaimSequence = 0U;
    m_failure.Error = TimerNotificationDeliveryError::None;
    m_failure.Cause = TimerNotificationDeliveryError::None;
    m_failure.TimerPin = Object::ObjectPinError::Ok;
    m_failure.NotificationPin = Object::ObjectPinError::Ok;
    m_failure.Notification = Ipc::NotificationError::None;
    m_failure.NotificationPublished = 0U;
    m_failure.QueueCommitted = 0U;
    m_failure.ClaimRetired = 0U;
    m_requestedTimerIdentity = timerId;
    m_claimToken = TimerQueueClaimToken{};
    m_claimedTimer = nullptr;
}

void TimerNotificationDelivery::RestoreClaimPending(const TimerNotificationDeliveryError cause) noexcept
{
    if (m_timerQueue.CancelClaim(m_claimToken, m_failure.QueueState, m_failure.Queue))
    {
        return;
    }

    m_failure.QueueState = m_timerQueue.GetState();
    m_failure.Cause = cause;
    m_failure.Error = TimerNotificationDeliveryError::ClaimCancellationRejected;
}

void TimerNotificationDelivery::RetireUndeliverableClaim(const TimerNotificationDeliveryError cause) noexcept
{
    if (m_timerQueue.RetireClaim(m_claimToken, m_failure.QueueState, m_failure.Queue))
    {
        m_failure.ClaimRetired = 1U;
        return;
    }

    m_failure.QueueState = m_timerQueue.GetState();
    m_failure.Cause = cause;
    m_failure.Error = TimerNotificationDeliveryError::ClaimRetirementRejected;
}

bool TimerNotificationDelivery::ProvesUndeliverable(const TimerNotificationDeliveryError error) const noexcept
{
    // The queue pin proves that the claimed Timer generation existed and has
    // not been collected, so ObjectNotFound means it is being destroyed.
    if (error == TimerNotificationDeliveryError::TimerPinRejected)
    {
        return m_failure.TimerPin == Object::ObjectPinError::ObjectNotFound;
    }
    // Only a Pinned destination's own pin proves the same for the
    // Notification; a Transient one may be retried with another destination.
    return (error == TimerNotificationDeliveryError::NotificationPinRejected) &&
           (m_destination == TimerNotificationDestination::Pinned) &&
           (m_failure.NotificationPin == Object::ObjectPinError::ObjectNotFound);
}

void TimerNotificationDelivery::RejectBeforePublication(const TimerNotificationDeliveryError error) noexcept
{
    m_failure.Error = error;
    if (ProvesUndeliverable(error))
    {
        RetireUndeliverableClaim(error);
        return;
    }
    RestoreClaimPending(error);
}

bool TimerNotificationDelivery::ClaimDue(const TimerId timerId, const Ipc::NotificationId notificationId) noexcept
{
    ResetDiagnostics(timerId, notificationId);
    const auto claimed = m_timerQueue.ClaimDue();
    if (!claimed.HasValue())
    {
        m_failure.Error = TimerNotificationDeliveryError::QueueClaimRejected;
        m_failure.Queue = claimed.GetError();
        m_failure.QueueState = m_timerQueue.GetState();
        return false;
    }

    const TimerQueueExpirationClaim& claim = claimed.GetValue();
    m_claimToken = claim.GetClaimToken();
    m_claimedTimer = &claim.GetTimer();
    m_failure.TimerIdentity = claim.GetTimerId();
    m_failure.Expiration = claim.GetExpiration();
    m_failure.QueueState = m_timerQueue.GetState();
    m_failure.ClaimSequence = m_claimToken.GetSequence();
    return true;
}

bool TimerNotificationDelivery::AdoptClaim(const TimerQueueExpirationClaim& claim,
                                           const Ipc::NotificationId notificationId) noexcept
{
    ResetDiagnostics(claim.GetTimerId(), notificationId);

    if (!m_timerQueue.ValidateClaimForDelivery(claim, m_canonicalClaim, m_failure.Queue))
    {
        m_failure.QueueState = m_timerQueue.GetState();
        m_failure.Error = TimerNotificationDeliveryError::InvalidClaim;
        return false;
    }

    const TimerQueueExpirationClaim& canonical = m_canonicalClaim;
    m_claimToken = canonical.GetClaimToken();
    m_claimedTimer = &canonical.GetTimer();
    m_failure.TimerIdentity = canonical.GetTimerId();
    m_failure.Expiration = canonical.GetExpiration();
    m_failure.QueueState = m_timerQueue.GetState();
    m_failure.ClaimSequence = m_claimToken.GetSequence();
    return true;
}

bool TimerNotificationDelivery::ValidateAndPin(Ipc::Notification& notification) noexcept
{
    const TimerId timerId = m_requestedTimerIdentity;
    const Ipc::NotificationId notificationId = m_failure.NotificationIdentity;
    if (m_claimedTimer == nullptr)
    {
        m_failure.Error = TimerNotificationDeliveryError::InvalidClaim;
        return false;
    }
    Timer& timer = *m_claimedTimer;
    if (!timerId.IsValid())
    {
        m_failure.Error = TimerNotificationDeliveryError::InvalidTimer;
        return false;
    }
    if (!timerId.IsFromRegistry(m_objectRegistry))
    {
        m_failure.Error = TimerNotificationDeliveryError::TimerRegistryMismatch;
        return false;
    }
    if (!m_failure.TimerIdentity.IsEqualTo(timerId) || (m_claimedTimer != &timer))
    {
        m_failure.Error = TimerNotificationDeliveryError::ClaimedTimerMismatch;
        return false;
    }

    const Object::ObjectPinError timerPinError =
        m_objectRegistry.PinObject(timerId.GetObjectId(), Object::ObjectType::Timer, m_timerPin);
    m_failure.TimerPin = timerPinError;
    if (timerPinError != Object::ObjectPinError::Ok)
    {
        m_failure.Error = TimerNotificationDeliveryError::TimerPinRejected;
        return false;
    }
    if (&m_timerPin.GetObject() != static_cast<Object::IKernelObject*>(&timer))
    {
        m_failure.Error = TimerNotificationDeliveryError::TimerInstanceMismatch;
        return false;
    }

    const TimerSnapshot claimedTimerState = timer.GetSnapshot();
    if ((claimedTimerState.State != TimerState::Expired) ||
        !claimedTimerState.Token.IsEqualTo(m_failure.Expiration.Token) ||
        (claimedTimerState.DeadlineTicks != m_failure.Expiration.DeadlineTicks))
    {
        m_failure.Error = TimerNotificationDeliveryError::ClaimedTimerStateMismatch;
        return false;
    }

    if (!notificationId.IsValid())
    {
        m_failure.Error = TimerNotificationDeliveryError::InvalidNotification;
        return false;
    }
    if (!notificationId.IsFromRegistry(m_objectRegistry))
    {
        m_failure.Error = TimerNotificationDeliveryError::NotificationRegistryMismatch;
        return false;
    }
    const Object::ObjectPinError notificationPinError =
        m_objectRegistry.PinObject(notificationId.GetObjectId(), Object::ObjectType::Notification, m_notificationPin);
    m_failure.NotificationPin = notificationPinError;
    if (notificationPinError != Object::ObjectPinError::Ok)
    {
        m_failure.Error = TimerNotificationDeliveryError::NotificationPinRejected;
        return false;
    }
    if (&m_notificationPin.GetObject() != static_cast<Object::IKernelObject*>(&notification))
    {
        m_failure.Error = TimerNotificationDeliveryError::NotificationInstanceMismatch;
        return false;
    }

    const bool notificationIsInitialized = notification.IsInitialized();
    m_failure.NotificationState = notification.GetSnapshot();
    if (!notificationIsInitialized)
    {
        m_failure.Notification = Ipc::NotificationError::NotInitialized;
        m_failure.Error = TimerNotificationDeliveryError::NotificationRejected;
        return false;
    }
    return true;
}

bool TimerNotificationDelivery::Publish(Ipc::Notification& notification) noexcept
{
    const Ipc::NotificationError signalError = notification.Signal();
    m_failure.Notification = signalError;
    m_failure.NotificationState = notification.GetSnapshot();
    if (signalError != Ipc::NotificationError::None)
    {
        m_failure.Error = TimerNotificationDeliveryError::NotificationRejected;
        return false;
    }
    m_failure.NotificationPublished = 1U;
    return true;
}

bool TimerNotificationDelivery::CommitClaim() noexcept
{
    if (!m_timerQueue.CommitClaim(m_claimToken, m_failure.QueueState, m_failure.Queue))
    {
        m_failure.QueueState = m_timerQueue.GetState();
        m_failure.Error = TimerNotificationDeliveryError::ClaimCommitRejected;
        return false;
    }
    m_failure.QueueCommitted = 1U;
    return true;
}

bool TimerNotificationDelivery::AcknowledgeTimer() noexcept
{
    const auto acknowledged = m_claimedTimer->Acknowledge(m_failure.Expiration.Token);
    if (!acknowledged.HasValue())
    {
        m_failure.Timer = acknowledged.GetError();
        m_failure.Error = TimerNotificationDeliveryError::TimerAcknowledgeRejected;
        return false;
    }
    return true;
}

void TimerNotificationDelivery::MakeReceipt() noexcept
{
    m_receipt.TimerIdentity = m_failure.TimerIdentity;
    m_receipt.NotificationIdentity = m_failure.NotificationIdentity;
    m_receipt.Expiration = m_failure.Expiration;
    m_receipt.QueueState = m_failure.QueueState;
    m_receipt.NotificationState = m_failure.NotificationState;
    m_receipt.ClaimSequence = m_failure.ClaimSequence;
    m_receipt.NotificationPublished = m_failure.NotificationPublished;
    m_receipt.QueueCommitted = m_failure.QueueCommitted;
}

void TimerNotificationDelivery::ReleaseTransientState() noexcept
{
    m_notificationPin.Reset();
    m_timerPin.Reset();
    m_claimToken = TimerQueueClaimToken{};
    m_claimedTimer = nullptr;
}

TimerNotificationDeliveryOutcome
TimerNotificationDelivery::CompleteDeliveryRetained(Ipc::Notification& notification) noexcept
{
    if (!ValidateAndPin(notification))
    {
        RejectBeforePublication(m_failure.Error);
        ReleaseTransientState();
        return TimerNotificationDeliveryOutcome::Rejected;
    }
    if (!Publish(notification))
    {
        RejectBeforePublication(m_failure.Error);
        ReleaseTransientState();
        return TimerNotificationDeliveryOutcome::Rejected;
    }
    if (!CommitClaim() || !AcknowledgeTimer())
    {
        ReleaseTransientState();
        return TimerNotificationDeliveryOutcome::Rejected;
    }

    MakeReceipt();
    ReleaseTransientState();
    return TimerNotificationDeliveryOutcome::Delivered;
}

TimerNotificationDeliveryOutcome TimerNotificationDelivery::DeliverRetained(const TimerId timerId, Timer& timer,
                                                                            const Ipc::NotificationId notificationId,
                                                                            Ipc::Notification& notification) noexcept
{
    m_destination = TimerNotificationDestination::Transient;
    if (!ClaimDue(timerId, notificationId))
    {
        ReleaseTransientState();
        return TimerNotificationDeliveryOutcome::Rejected;
    }
    if (m_claimedTimer != &timer)
    {
        m_failure.Error = TimerNotificationDeliveryError::ClaimedTimerMismatch;
        RejectBeforePublication(m_failure.Error);
        ReleaseTransientState();
        return TimerNotificationDeliveryOutcome::Rejected;
    }
    return CompleteDeliveryRetained(notification);
}

TimerNotificationDeliveryOutcome TimerNotificationDelivery::DeliverClaimedRetained(
    const TimerQueueExpirationClaim& claim, const Ipc::NotificationId notificationId, Ipc::Notification& notification,
    const TimerNotificationDestination destination) noexcept
{
    m_destination = destination;
    if (!AdoptClaim(claim, notificationId))
    {
        ReleaseTransientState();
        return TimerNotificationDeliveryOutcome::Rejected;
    }
    return CompleteDeliveryRetained(notification);
}

Foundation::Result<TimerNotificationDeliveryReceipt, TimerNotificationDeliveryFailure>
TimerNotificationDelivery::Deliver(const TimerId timerId, Timer& timer, const Ipc::NotificationId notificationId,
                                   Ipc::Notification& notification) noexcept
{
    using ResultType = Foundation::Result<TimerNotificationDeliveryReceipt, TimerNotificationDeliveryFailure>;

    if (DeliverRetained(timerId, timer, notificationId, notification) == TimerNotificationDeliveryOutcome::Rejected)
    {
        return ResultType::Failure(m_failure);
    }
    return ResultType::Success(m_receipt);
}

Foundation::Result<TimerNotificationDeliveryReceipt, TimerNotificationDeliveryFailure>
TimerNotificationDelivery::DeliverClaimed(const TimerQueueExpirationClaim& claim,
                                          const Ipc::NotificationId notificationId,
                                          Ipc::Notification& notification) noexcept
{
    using ResultType = Foundation::Result<TimerNotificationDeliveryReceipt, TimerNotificationDeliveryFailure>;

    if (DeliverClaimedRetained(claim, notificationId, notification) == TimerNotificationDeliveryOutcome::Rejected)
    {
        return ResultType::Failure(m_failure);
    }
    return ResultType::Success(m_receipt);
}
} // namespace Asteria::Kernel::Time
