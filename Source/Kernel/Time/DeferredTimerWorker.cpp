// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Time/DeferredTimerWorker.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Time
{
DeferredTimerWorker::~DeferredTimerWorker() noexcept
{
    for (Entry& entry : m_entries)
    {
        ClearEntry(entry);
    }
    m_registeredBindings = 0U;
}

void DeferredTimerWorker::ResetFailure() noexcept
{
    m_failure = DeferredTimerWorkerFailure{};
    m_failure.QueueState = m_timerQueue.GetState();
    m_failure.TimerPin = Object::ObjectPinError::Ok;
    m_failure.NotificationPin = Object::ObjectPinError::Ok;
    m_failure.Notification = Ipc::NotificationError::None;
    m_failure.State = m_state;
}

void DeferredTimerWorker::SetFailure(const DeferredTimerWorkerError error) noexcept
{
    m_failure.QueueState = m_timerQueue.GetState();
    m_failure.Error = error;
    m_failure.State = m_state;
    m_failure.DeliveredCount = static_cast<Foundation::UInt8>(m_deliveredCount);
}

bool DeferredTimerWorker::EntryIsClear(const Entry& entry) noexcept
{
    if (entry.m_timerPin.IsValid() || entry.m_notificationPin.IsValid() || entry.m_timerIdentity.IsValid() ||
        entry.m_notificationIdentity.IsValid() || (entry.m_timerStorage != nullptr) ||
        (entry.m_notificationStorage != nullptr) || (entry.m_registrationSequence != 0U) ||
        (entry.m_state != EntryState::Empty))
    {
        return false;
    }
    return true;
}

bool DeferredTimerWorker::EntryIsValid(const Entry& entry) noexcept
{
    if (!entry.m_timerPin.IsValid() || !entry.m_notificationPin.IsValid() || !entry.m_timerIdentity.IsValid() ||
        !entry.m_notificationIdentity.IsValid() || (entry.m_timerStorage == nullptr) ||
        (entry.m_notificationStorage == nullptr) || (entry.m_registrationSequence == 0U) ||
        (entry.m_state != EntryState::Registered) ||
        (entry.m_timerPin.GetObjectId().GetValue() != entry.m_timerIdentity.GetObjectId().GetValue()) ||
        (entry.m_notificationPin.GetObjectId().GetValue() != entry.m_notificationIdentity.GetObjectId().GetValue()) ||
        (entry.m_timerPin.GetObjectType() != Object::ObjectType::Timer) ||
        (entry.m_notificationPin.GetObjectType() != Object::ObjectType::Notification) ||
        (&entry.m_timerPin.GetObject() != static_cast<Object::IKernelObject*>(entry.m_timerStorage)) ||
        (&entry.m_notificationPin.GetObject() != static_cast<Object::IKernelObject*>(entry.m_notificationStorage)))
    {
        return false;
    }
    return true;
}

void DeferredTimerWorker::ClearEntry(Entry& entry) noexcept
{
    entry.m_notificationPin.Reset();
    entry.m_timerPin.Reset();
    entry.m_timerIdentity = TimerId{};
    entry.m_notificationIdentity = Ipc::NotificationId{};
    entry.m_timerStorage = nullptr;
    entry.m_notificationStorage = nullptr;
    entry.m_registrationSequence = 0U;
    entry.m_state = EntryState::Empty;
}

Foundation::Result<DeferredTimerRegistrationReceipt, DeferredTimerWorkerFailure>
DeferredTimerWorker::Register(const TimerId timerId, Timer& timer, const Ipc::NotificationId notificationId,
                              Ipc::Notification& notification) noexcept
{
    using ResultType = Foundation::Result<DeferredTimerRegistrationReceipt, DeferredTimerWorkerFailure>;

    ResetFailure();
    m_failure.TimerIdentity = timerId;
    m_failure.NotificationIdentity = notificationId;
    if (m_state == DeferredTimerWorkerState::Draining)
    {
        SetFailure(DeferredTimerWorkerError::DrainActive);
        return ResultType::Failure(m_failure);
    }
    if (m_state == DeferredTimerWorkerState::Faulted)
    {
        SetFailure(DeferredTimerWorkerError::Faulted);
        return ResultType::Failure(m_failure);
    }
    if (!timerId.IsValid())
    {
        SetFailure(DeferredTimerWorkerError::InvalidTimer);
        return ResultType::Failure(m_failure);
    }
    if (!timerId.IsFromRegistry(m_objectRegistry))
    {
        SetFailure(DeferredTimerWorkerError::TimerRegistryMismatch);
        return ResultType::Failure(m_failure);
    }
    if (!notificationId.IsValid())
    {
        SetFailure(DeferredTimerWorkerError::InvalidNotification);
        return ResultType::Failure(m_failure);
    }
    if (!notificationId.IsFromRegistry(m_objectRegistry))
    {
        SetFailure(DeferredTimerWorkerError::NotificationRegistryMismatch);
        return ResultType::Failure(m_failure);
    }

    Entry* emptyEntry{nullptr};
    Foundation::Size observedBindings{0U};
    for (Entry& entry : m_entries)
    {
        if (entry.m_state == EntryState::Empty)
        {
            if (!EntryIsClear(entry))
            {
                m_state = DeferredTimerWorkerState::Faulted;
                SetFailure(DeferredTimerWorkerError::CorruptState);
                return ResultType::Failure(m_failure);
            }
            if (emptyEntry == nullptr)
            {
                emptyEntry = &entry;
            }
            continue;
        }
        if (!EntryIsValid(entry))
        {
            m_state = DeferredTimerWorkerState::Faulted;
            SetFailure(DeferredTimerWorkerError::CorruptState);
            return ResultType::Failure(m_failure);
        }
        ++observedBindings;
        if (entry.m_timerIdentity.IsEqualTo(timerId) || (entry.m_timerStorage == &timer))
        {
            SetFailure(DeferredTimerWorkerError::DuplicateTimer);
            return ResultType::Failure(m_failure);
        }
    }
    if (observedBindings != m_registeredBindings)
    {
        m_state = DeferredTimerWorkerState::Faulted;
        SetFailure(DeferredTimerWorkerError::CorruptState);
        return ResultType::Failure(m_failure);
    }
    if ((emptyEntry == nullptr) || (m_registeredBindings == MaximumDeferredTimerBindings))
    {
        SetFailure(DeferredTimerWorkerError::BindingCapacityExceeded);
        return ResultType::Failure(m_failure);
    }

    Foundation::UInt64 nextSequence{m_lastRegistrationSequence};
    if (!DeferredTimerWorkerSequencePolicy::TryAdvance(m_lastRegistrationSequence, nextSequence))
    {
        SetFailure(DeferredTimerWorkerError::RegistrationSequenceExhausted);
        return ResultType::Failure(m_failure);
    }

    const Object::ObjectPinError timerPinError =
        m_objectRegistry.PinObject(timerId.GetObjectId(), Object::ObjectType::Timer, emptyEntry->m_timerPin);
    m_failure.TimerPin = timerPinError;
    if (timerPinError != Object::ObjectPinError::Ok)
    {
        SetFailure(DeferredTimerWorkerError::TimerPinRejected);
        return ResultType::Failure(m_failure);
    }
    if (&emptyEntry->m_timerPin.GetObject() != static_cast<Object::IKernelObject*>(&timer))
    {
        emptyEntry->m_timerPin.Reset();
        SetFailure(DeferredTimerWorkerError::TimerInstanceMismatch);
        return ResultType::Failure(m_failure);
    }

    const Object::ObjectPinError notificationPinError = m_objectRegistry.PinObject(
        notificationId.GetObjectId(), Object::ObjectType::Notification, emptyEntry->m_notificationPin);
    m_failure.NotificationPin = notificationPinError;
    if (notificationPinError != Object::ObjectPinError::Ok)
    {
        emptyEntry->m_timerPin.Reset();
        SetFailure(DeferredTimerWorkerError::NotificationPinRejected);
        return ResultType::Failure(m_failure);
    }
    if (&emptyEntry->m_notificationPin.GetObject() != static_cast<Object::IKernelObject*>(&notification))
    {
        emptyEntry->m_notificationPin.Reset();
        emptyEntry->m_timerPin.Reset();
        SetFailure(DeferredTimerWorkerError::NotificationInstanceMismatch);
        return ResultType::Failure(m_failure);
    }

    m_failure.Timer = timer.GetSnapshot().State;
    if (!timer.IsInitialized())
    {
        emptyEntry->m_notificationPin.Reset();
        emptyEntry->m_timerPin.Reset();
        SetFailure(DeferredTimerWorkerError::TimerNotInitialized);
        return ResultType::Failure(m_failure);
    }
    if (!notification.IsInitialized())
    {
        m_failure.Notification = Ipc::NotificationError::NotInitialized;
        emptyEntry->m_notificationPin.Reset();
        emptyEntry->m_timerPin.Reset();
        SetFailure(DeferredTimerWorkerError::NotificationNotInitialized);
        return ResultType::Failure(m_failure);
    }

    emptyEntry->m_timerIdentity = timerId;
    emptyEntry->m_notificationIdentity = notificationId;
    emptyEntry->m_timerStorage = &timer;
    emptyEntry->m_notificationStorage = &notification;
    emptyEntry->m_registrationSequence = nextSequence;
    emptyEntry->m_state = EntryState::Registered;
    m_lastRegistrationSequence = nextSequence;
    ++m_registeredBindings;

    return ResultType::Success(DeferredTimerRegistrationReceipt{
        .Registration =
            DeferredTimerRegistrationToken{
                *this,
                nextSequence,
            },
        .TimerIdentity = timerId,
        .NotificationIdentity = notificationId,
    });
}

DeferredTimerWorker::Entry*
DeferredTimerWorker::FindRegistration(const DeferredTimerRegistrationToken registration) noexcept
{
    for (Entry& entry : m_entries)
    {
        if ((entry.m_state == EntryState::Registered) && (entry.m_registrationSequence == registration.m_sequence))
        {
            return &entry;
        }
    }
    return nullptr;
}

Foundation::Result<DeferredTimerWorkerSnapshot, DeferredTimerWorkerFailure>
DeferredTimerWorker::Unregister(const DeferredTimerRegistrationToken registration) noexcept
{
    using ResultType = Foundation::Result<DeferredTimerWorkerSnapshot, DeferredTimerWorkerFailure>;

    ResetFailure();
    if (m_state == DeferredTimerWorkerState::Draining)
    {
        SetFailure(DeferredTimerWorkerError::DrainActive);
        return ResultType::Failure(m_failure);
    }
    if (!registration.IsValid() || (registration.m_worker != this))
    {
        SetFailure(DeferredTimerWorkerError::InvalidRegistration);
        return ResultType::Failure(m_failure);
    }

    Entry* const entry = FindRegistration(registration);
    if (entry == nullptr)
    {
        SetFailure(DeferredTimerWorkerError::StaleRegistration);
        return ResultType::Failure(m_failure);
    }
    m_failure.TimerIdentity = entry->m_timerIdentity;
    m_failure.NotificationIdentity = entry->m_notificationIdentity;
    m_failure.RegistrationSequence = entry->m_registrationSequence;
    if (!EntryIsValid(*entry))
    {
        m_state = DeferredTimerWorkerState::Faulted;
        SetFailure(DeferredTimerWorkerError::CorruptState);
        return ResultType::Failure(m_failure);
    }

    const TimerSnapshot timerSnapshot = entry->m_timerStorage->GetSnapshot();
    m_failure.Timer = timerSnapshot.State;
    if (timerSnapshot.State != TimerState::Disarmed)
    {
        SetFailure(DeferredTimerWorkerError::TimerNotDisarmed);
        return ResultType::Failure(m_failure);
    }

    ClearEntry(*entry);
    --m_registeredBindings;
    return ResultType::Success(GetSnapshot());
}

DeferredTimerWorker::Entry* DeferredTimerWorker::ResolveActiveBinding(bool& identityContradiction) noexcept
{
    identityContradiction = false;
    const TimerId timerId = m_activeClaim.GetTimerId();
    Timer* const timerStorage = &m_activeClaim.GetTimer();
    for (Entry& entry : m_entries)
    {
        if (entry.m_state == EntryState::Empty)
        {
            if (!EntryIsClear(entry))
            {
                identityContradiction = true;
                return nullptr;
            }
            continue;
        }
        if (!EntryIsValid(entry))
        {
            identityContradiction = true;
            return nullptr;
        }

        const bool identityMatches = entry.m_timerIdentity.IsEqualTo(timerId);
        const bool storageMatches = entry.m_timerStorage == timerStorage;
        if (identityMatches && storageMatches)
        {
            return &entry;
        }
        if (identityMatches || storageMatches)
        {
            identityContradiction = true;
            return nullptr;
        }
    }
    return nullptr;
}

bool DeferredTimerWorker::RestoreActiveClaim() noexcept
{
    const TimerQueueClaimToken claimToken = m_activeClaim.GetClaimToken();
    if (!claimToken.IsValid())
    {
        m_state = DeferredTimerWorkerState::Faulted;
        SetFailure(DeferredTimerWorkerError::CorruptState);
        return false;
    }

    if (!m_timerQueue.CancelClaim(claimToken, m_failure.QueueState, m_failure.Queue))
    {
        m_failure.QueueState = m_timerQueue.GetState();
        m_state = DeferredTimerWorkerState::Faulted;
        SetFailure(DeferredTimerWorkerError::ClaimCancellationRejected);
        return false;
    }
    m_failure.ClaimRestored = 1U;
    m_activeClaim = TimerQueueExpirationClaim{};
    return true;
}

bool DeferredTimerWorker::ActiveTimerGenerationEnded() noexcept
{
    // The queue pin proves that the claimed generation existed and has not
    // been collected, so a Timer that can no longer be pinned is being
    // destroyed and no binding can ever be registered for it.
    Object::ObjectPin generation{};
    return m_objectRegistry.PinObject(m_activeClaim.GetTimerId().GetObjectId(), Object::ObjectType::Timer,
                                      generation) == Object::ObjectPinError::ObjectNotFound;
}

bool DeferredTimerWorker::RetireActiveClaim() noexcept
{
    const TimerQueueClaimToken claimToken = m_activeClaim.GetClaimToken();
    if (!claimToken.IsValid())
    {
        m_state = DeferredTimerWorkerState::Faulted;
        SetFailure(DeferredTimerWorkerError::CorruptState);
        return false;
    }

    if (!m_timerQueue.RetireClaim(claimToken, m_failure.QueueState, m_failure.Queue))
    {
        m_failure.QueueState = m_timerQueue.GetState();
        m_state = DeferredTimerWorkerState::Faulted;
        SetFailure(DeferredTimerWorkerError::ClaimRetirementRejected);
        return false;
    }
    m_activeClaim = TimerQueueExpirationClaim{};
    ++m_retiredCount;
    return true;
}

void DeferredTimerWorker::RetainDeliveryFailure(const TimerNotificationDeliveryFailure& failure,
                                                const TimerState timerState,
                                                const Foundation::UInt64 registrationSequence) noexcept
{
    m_lastDeliveryFailure = failure;
    m_failure.Queue = failure.Queue;
    m_failure.QueueState = failure.QueueState;
    m_failure.TimerIdentity = failure.TimerIdentity;
    m_failure.NotificationIdentity = failure.NotificationIdentity;
    m_failure.RegistrationSequence = registrationSequence;
    m_failure.Delivery = failure.Error;
    m_failure.TimerPin = failure.TimerPin;
    m_failure.NotificationPin = failure.NotificationPin;
    m_failure.Notification = failure.Notification;
    m_failure.Timer = timerState;
    m_failure.NotificationPublished = failure.NotificationPublished;
    m_failure.QueueCommitted = failure.QueueCommitted;
    const Detail::DeferredTimerDeliveryRejection rejection = Detail::ClassifyDeliveryRejection(failure);
    if (rejection.ClaimRestored != 0U)
    {
        m_failure.ClaimRestored = 1U;
    }
    m_state = rejection.State;
    SetFailure(rejection.Error);
}

bool DeferredTimerWorker::DeliverActive(Entry& entry) noexcept
{
    m_failure.TimerIdentity = entry.m_timerIdentity;
    m_failure.NotificationIdentity = entry.m_notificationIdentity;
    m_failure.RegistrationSequence = entry.m_registrationSequence;
    const TimerNotificationDeliveryOutcome outcome =
        m_delivery.DeliverClaimedRetained(m_activeClaim, entry.m_notificationIdentity, *entry.m_notificationStorage,
                                          TimerNotificationDestination::Pinned);
    m_activeClaim = TimerQueueExpirationClaim{};
    if (outcome == TimerNotificationDeliveryOutcome::Rejected)
    {
        const TimerNotificationDeliveryFailure& failure = m_delivery.GetRetainedFailure();
        if (failure.ClaimRetired != 0U)
        {
            // The expiration can never be delivered and has been consumed;
            // the binding stays until its owner removes it.
            m_lastDeliveryFailure = failure;
            ++m_retiredCount;
            return true;
        }
        RetainDeliveryFailure(failure, entry.m_timerStorage->GetSnapshot().State, entry.m_registrationSequence);
        return false;
    }

    m_lastDeliveryReceipt = m_delivery.GetRetainedReceipt();
    if (m_deliveredCount == 0U)
    {
        m_firstTimerIdentity = m_lastDeliveryReceipt.TimerIdentity;
        m_firstClaimSequence = m_lastDeliveryReceipt.ClaimSequence;
    }
    m_lastTimerIdentity = m_lastDeliveryReceipt.TimerIdentity;
    m_lastClaimSequence = m_lastDeliveryReceipt.ClaimSequence;
    ++m_deliveredCount;
    return true;
}

void DeferredTimerWorker::FinishDrainRetained(const DeferredTimerDrainStop stop) noexcept
{
    m_state = DeferredTimerWorkerState::Ready;
    m_drainReceipt = DeferredTimerDrainReceipt{
        .FirstTimerIdentity = m_firstTimerIdentity,
        .LastTimerIdentity = m_lastTimerIdentity,
        .QueueState = m_timerQueue.GetState(),
        .FirstClaimSequence = m_firstClaimSequence,
        .LastClaimSequence = m_lastClaimSequence,
        .Stop = stop,
        .DeliveredCount = static_cast<Foundation::UInt8>(m_deliveredCount),
        .State = m_state,
        .RetiredCount = static_cast<Foundation::UInt8>(m_retiredCount),
        .Reserved = {0U, 0U, 0U, 0U},
    };
}

DeferredTimerDrainOutcome DeferredTimerWorker::RejectDrainRetained() noexcept
{
    m_retainedDrainFailure = m_failure;
    return DeferredTimerDrainOutcome::Rejected;
}

Foundation::Result<DeferredTimerDrainReceipt, DeferredTimerWorkerFailure> DeferredTimerWorker::DrainDue() noexcept
{
    using ResultType = Foundation::Result<DeferredTimerDrainReceipt, DeferredTimerWorkerFailure>;

    if (DrainDueRetained() == DeferredTimerDrainOutcome::Rejected)
    {
        return ResultType::Failure(m_failure);
    }
    return ResultType::Success(m_drainReceipt);
}

DeferredTimerDrainOutcome DeferredTimerWorker::DrainDueRetained() noexcept
{
    m_retainedDrainFailure = DeferredTimerWorkerFailure{};
    ResetFailure();
    if (m_state == DeferredTimerWorkerState::Draining)
    {
        SetFailure(DeferredTimerWorkerError::DrainActive);
        return RejectDrainRetained();
    }
    if (m_state == DeferredTimerWorkerState::Faulted)
    {
        SetFailure(DeferredTimerWorkerError::Faulted);
        return RejectDrainRetained();
    }

    m_state = DeferredTimerWorkerState::Draining;
    ResetFailure();
    m_activeClaim = TimerQueueExpirationClaim{};
    m_drainReceipt = DeferredTimerDrainReceipt{};
    m_lastDeliveryReceipt = TimerNotificationDeliveryReceipt{};
    m_lastDeliveryFailure = TimerNotificationDeliveryFailure{};
    m_firstTimerIdentity = TimerId{};
    m_lastTimerIdentity = TimerId{};
    m_firstClaimSequence = 0U;
    m_lastClaimSequence = 0U;
    m_deliveredCount = 0U;
    m_retiredCount = 0U;

    for (Foundation::Size attempt{0U}; attempt < MaximumTimerQueueEntries; ++attempt)
    {
        if (!m_timerQueue.ClaimDue(m_activeClaim, m_failure.Queue))
        {
            m_failure.QueueState = m_timerQueue.GetState();
            if (m_failure.Queue.Error == TimerQueueError::Empty)
            {
                FinishDrainRetained(DeferredTimerDrainStop::Empty);
                return DeferredTimerDrainOutcome::Drained;
            }
            if (m_failure.Queue.Error == TimerQueueError::NotDue)
            {
                FinishDrainRetained(DeferredTimerDrainStop::NotDue);
                return DeferredTimerDrainOutcome::Drained;
            }
            m_state = DeferredTimerWorkerState::Faulted;
            SetFailure(DeferredTimerWorkerError::QueueRejected);
            return RejectDrainRetained();
        }

        m_failure.TimerIdentity = m_activeClaim.GetTimerId();
        m_failure.Timer = m_activeClaim.GetTimer().GetSnapshot().State;
        bool identityContradiction{false};
        Entry* const entry = ResolveActiveBinding(identityContradiction);
        if ((entry == nullptr) && !identityContradiction && ActiveTimerGenerationEnded())
        {
            if (!RetireActiveClaim())
            {
                return RejectDrainRetained();
            }
            continue;
        }
        if (entry == nullptr)
        {
            if (!RestoreActiveClaim())
            {
                return RejectDrainRetained();
            }
            if (identityContradiction)
            {
                m_state = DeferredTimerWorkerState::Faulted;
                SetFailure(DeferredTimerWorkerError::CorruptState);
            }
            else
            {
                m_state = DeferredTimerWorkerState::Ready;
                SetFailure(DeferredTimerWorkerError::BindingNotFound);
            }
            return RejectDrainRetained();
        }

        if (!DeliverActive(*entry))
        {
            return RejectDrainRetained();
        }
    }

    FinishDrainRetained(DeferredTimerDrainStop::WorkLimit);
    return DeferredTimerDrainOutcome::Drained;
}

DeferredTimerWorkerSnapshot DeferredTimerWorker::GetSnapshot() const noexcept
{
    return DeferredTimerWorkerSnapshot{
        .QueueState = m_timerQueue.GetState(),
        .LastRegistrationSequence = m_lastRegistrationSequence,
        .State = m_state,
        .RegisteredBindings = static_cast<Foundation::UInt8>(m_registeredBindings),
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace Asteria::Kernel::Time
