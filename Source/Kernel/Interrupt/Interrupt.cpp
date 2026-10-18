// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Interrupt/Interrupt.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Interrupt
{
InterruptFailure Interrupt::MakeFailure(const InterruptError error, const Hal::Interrupt::InterruptSourceError source,
                                        const Object::ObjectPinError pin,
                                        const Ipc::NotificationError notification) noexcept
{
    return InterruptFailure{
        .Notification =
            Ipc::NotificationFailure{
                .Error = notification,
                .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
            },
        .Source = source,
        .Pin = pin,
        .Error = error,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    };
}

InterruptFailure Interrupt::Initialize(const Object::ResourceDomainId chargeOwner,
                                       const Ipc::NotificationId notificationId,
                                       Ipc::Notification& notification) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (DecodeState(m_stateWord.LoadAcquire()) != InterruptState::Uninitialized)
    {
        return MakeFailure(InterruptError::AlreadyInitialized);
    }
    if (!chargeOwner.IsValid())
    {
        return MakeFailure(InterruptError::InvalidConfiguration);
    }
    if (!notificationId.IsValid())
    {
        return MakeFailure(InterruptError::InvalidNotification);
    }
    if (!notificationId.IsFromRegistry(m_registry))
    {
        return MakeFailure(InterruptError::NotificationRegistryMismatch);
    }

    const Object::ObjectPinError pinError =
        m_registry.PinObject(notificationId.GetObjectId(), Object::ObjectType::Notification, m_notificationPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(InterruptError::NotificationPinRejected, Hal::Interrupt::InterruptSourceError::None,
                           pinError);
    }
    if (&m_notificationPin.GetObject() != static_cast<Object::IKernelObject*>(&notification))
    {
        m_notificationPin.Reset();
        return MakeFailure(InterruptError::NotificationInstanceMismatch);
    }

    const Hal::Interrupt::InterruptSourceError sourceError = m_source.Claim();
    if (sourceError != Hal::Interrupt::InterruptSourceError::None)
    {
        m_notificationPin.Reset();
        return MakeFailure(InterruptError::SourceRejected, sourceError);
    }

    m_notificationIdentity = notificationId;
    m_notification = &notification;
    m_sourceClaimed = true;
    m_stateWord.StoreRelease(EncodeState(InterruptState::Masked, 0U));
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return MakeFailure(InterruptError::None);
}

InterruptFailure Interrupt::Arm() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const Foundation::UInt64 stateWord = m_stateWord.LoadAcquire();
    const InterruptState state = DecodeState(stateWord);
    const Foundation::UInt64 sequence = DecodeSequence(stateWord);
    if (state == InterruptState::Uninitialized)
    {
        return MakeFailure(InterruptError::NotInitialized);
    }
    if (state == InterruptState::Quiesced)
    {
        return MakeFailure(InterruptError::AlreadyQuiesced);
    }
    if (state == InterruptState::Faulted)
    {
        return MakeFailure(InterruptError::Faulted);
    }
    if (state == InterruptState::Armed)
    {
        return MakeFailure(InterruptError::AlreadyArmed);
    }
    if (state == InterruptState::Pending)
    {
        return MakeFailure(InterruptError::EventPending);
    }
    if (state != InterruptState::Masked)
    {
        EnterFaulted(sequence);
        return MakeFailure(InterruptError::CorruptState);
    }

    const Hal::Interrupt::InterruptSourceError sourceError = m_source.Unmask();
    if (sourceError != Hal::Interrupt::InterruptSourceError::None)
    {
        if (SourceFailureIsTerminal(sourceError))
        {
            EnterFaulted(sequence);
        }
        return MakeFailure(InterruptError::SourceRejected, sourceError);
    }
    m_stateWord.StoreRelease(EncodeState(InterruptState::Armed, sequence));
    DeliverMissedLocked(sequence);
    return MakeFailure(InterruptError::None);
}

void Interrupt::DeliverMissedLocked(const Foundation::UInt64 sequence) noexcept
{
    if (m_missed.ExchangeAcquireRelease(0U) == 0U)
    {
        return;
    }
    Foundation::UInt64 nextSequence{sequence};
    if (!InterruptEventSequencePolicy::TryAdvance(sequence, nextSequence))
    {
        EnterFaulted(sequence);
        return;
    }
    m_stateWord.StoreRelease(EncodeState(InterruptState::Pending, nextSequence));
    if (m_notification->Signal() != Ipc::NotificationError::None)
    {
        EnterFaulted(nextSequence);
    }
}

InterruptFailure Interrupt::Mask() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const Foundation::UInt64 stateWord = m_stateWord.LoadAcquire();
    const InterruptState state = DecodeState(stateWord);
    const Foundation::UInt64 sequence = DecodeSequence(stateWord);
    if (state == InterruptState::Uninitialized)
    {
        return MakeFailure(InterruptError::NotInitialized);
    }
    if (state == InterruptState::Quiesced)
    {
        return MakeFailure(InterruptError::AlreadyQuiesced);
    }
    if (state == InterruptState::Faulted)
    {
        return MakeFailure(InterruptError::Faulted);
    }
    if (state == InterruptState::Masked)
    {
        return MakeFailure(InterruptError::AlreadyMasked);
    }
    if (state == InterruptState::Pending)
    {
        return MakeFailure(InterruptError::EventPending);
    }
    if (state != InterruptState::Armed)
    {
        EnterFaulted(sequence);
        return MakeFailure(InterruptError::CorruptState);
    }

    const Hal::Interrupt::InterruptSourceError sourceError = m_source.Mask();
    if (sourceError != Hal::Interrupt::InterruptSourceError::None)
    {
        if (SourceFailureIsTerminal(sourceError))
        {
            EnterFaulted(sequence);
        }
        return MakeFailure(InterruptError::SourceRejected, sourceError);
    }
    m_stateWord.StoreRelease(EncodeState(InterruptState::Masked, sequence));
    return MakeFailure(InterruptError::None);
}

Foundation::Result<InterruptDelivery, InterruptFailure> Interrupt::HandleInterrupt() noexcept
{
    using ResultType = Foundation::Result<InterruptDelivery, InterruptFailure>;

    if (m_chargeOwnerValue.LoadAcquire() == 0U)
    {
        return ResultType::Failure(MakeFailure(InterruptError::NotInitialized));
    }

    Foundation::UInt64 stateWord = m_stateWord.LoadAcquire();
    const InterruptState initialState = DecodeState(stateWord);
    if ((initialState == InterruptState::Uninitialized) || (initialState == InterruptState::Quiesced))
    {
        return ResultType::Failure(MakeFailure(InterruptError::NotInitialized));
    }
    if (initialState == InterruptState::Faulted)
    {
        return ResultType::Failure(MakeFailure(InterruptError::Faulted));
    }
    if (!StateIsKnown(initialState))
    {
        EnterFaulted(DecodeSequence(stateWord));
        return ResultType::Failure(MakeFailure(InterruptError::CorruptState));
    }

    const Hal::Interrupt::InterruptSourceError sourceError = m_source.MaskAndAcknowledge();
    stateWord = m_stateWord.LoadAcquire();
    const Foundation::UInt64 sequence = DecodeSequence(stateWord);
    if (sourceError != Hal::Interrupt::InterruptSourceError::None)
    {
        EnterFaulted(sequence);
        return ResultType::Failure(MakeFailure(InterruptError::SourceRejected, sourceError));
    }

    const InterruptState state = DecodeState(stateWord);
    if (state == InterruptState::Pending)
    {
        return ResultType::Success(InterruptDelivery{
            .Event = InterruptEventToken{*this, sequence},
            .NewEvent = false,
            .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
        });
    }
    if (state == InterruptState::Masked)
    {
        m_missed.StoreRelease(1U);
        return ResultType::Failure(MakeFailure(InterruptError::SpuriousInterrupt));
    }
    if ((state == InterruptState::Uninitialized) || (state == InterruptState::Quiesced))
    {
        return ResultType::Failure(MakeFailure(InterruptError::NotInitialized));
    }
    if (state != InterruptState::Armed)
    {
        EnterFaulted(sequence);
        return ResultType::Failure(MakeFailure(InterruptError::CorruptState));
    }

    Foundation::UInt64 nextSequence{sequence};
    if (!InterruptEventSequencePolicy::TryAdvance(sequence, nextSequence))
    {
        EnterFaulted(sequence);
        return ResultType::Failure(MakeFailure(InterruptError::EventSequenceExhausted));
    }

    m_stateWord.StoreRelease(EncodeState(InterruptState::Pending, nextSequence));
    const Ipc::NotificationError notificationError = m_notification->Signal();
    if (notificationError != Ipc::NotificationError::None)
    {
        EnterFaulted(nextSequence);
        return ResultType::Failure(MakeFailure(InterruptError::NotificationRejected,
                                               Hal::Interrupt::InterruptSourceError::None, Object::ObjectPinError::Ok,
                                               notificationError));
    }

    return ResultType::Success(InterruptDelivery{
        .Event = InterruptEventToken{*this, nextSequence},
        .NewEvent = true,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    });
}

Foundation::Result<InterruptSnapshot, InterruptFailure>
Interrupt::Acknowledge(const InterruptEventToken event, const InterruptCompletion completion) noexcept
{
    using ResultType = Foundation::Result<InterruptSnapshot, InterruptFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const Foundation::UInt64 stateWord = m_stateWord.LoadAcquire();
    const InterruptError validation = ValidateEventLocked(event, stateWord);
    if (validation != InterruptError::None)
    {
        return ResultType::Failure(MakeFailure(validation));
    }
    if ((completion != InterruptCompletion::KeepMasked) && (completion != InterruptCompletion::Rearm))
    {
        return ResultType::Failure(MakeFailure(InterruptError::InvalidCompletion));
    }

    const Foundation::UInt64 sequence = DecodeSequence(stateWord);
    if (completion == InterruptCompletion::Rearm)
    {
        const Hal::Interrupt::InterruptSourceError sourceError = m_source.Unmask();
        if (sourceError != Hal::Interrupt::InterruptSourceError::None)
        {
            if (SourceFailureIsTerminal(sourceError))
            {
                EnterFaulted(sequence);
            }
            return ResultType::Failure(MakeFailure(InterruptError::SourceRejected, sourceError));
        }
        m_stateWord.StoreRelease(EncodeState(InterruptState::Armed, sequence));
        DeliverMissedLocked(sequence);
    }
    else
    {
        m_stateWord.StoreRelease(EncodeState(InterruptState::Masked, sequence));
    }
    return ResultType::Success(GetSnapshotLocked());
}

InterruptFailure Interrupt::Quiesce() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const Foundation::UInt64 stateWord = m_stateWord.LoadAcquire();
    const InterruptState state = DecodeState(stateWord);
    const Foundation::UInt64 sequence = DecodeSequence(stateWord);
    if (state == InterruptState::Uninitialized)
    {
        return MakeFailure(InterruptError::NotInitialized);
    }
    if (state == InterruptState::Quiesced)
    {
        return MakeFailure(InterruptError::AlreadyQuiesced);
    }
    if (state == InterruptState::Armed)
    {
        return MakeFailure(InterruptError::AlreadyArmed);
    }
    if (state == InterruptState::Pending)
    {
        return MakeFailure(InterruptError::EventPending);
    }
    if ((state != InterruptState::Masked) && (state != InterruptState::Faulted))
    {
        EnterFaulted(sequence);
        return MakeFailure(InterruptError::CorruptState);
    }

    const Hal::Interrupt::InterruptSourceError sourceError = m_source.Release();
    if (sourceError != Hal::Interrupt::InterruptSourceError::None)
    {
        if (SourceFailureIsTerminal(sourceError))
        {
            EnterFaulted(sequence);
        }
        return MakeFailure(InterruptError::SourceRejected, sourceError);
    }

    m_sourceClaimed = false;
    m_missed.StoreRelease(0U);
    m_notification = nullptr;
    m_notificationIdentity = Ipc::NotificationId{};
    m_notificationPin.Reset();
    m_stateWord.StoreRelease(EncodeState(InterruptState::Quiesced, sequence));
    return MakeFailure(InterruptError::None);
}

bool Interrupt::IsInitialized() const noexcept
{
    return m_chargeOwnerValue.LoadAcquire() != 0U;
}

InterruptSnapshot Interrupt::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetSnapshotLocked();
}

Object::ObjectType Interrupt::GetObjectType() const noexcept
{
    return Object::ObjectType::Interrupt;
}

Object::ResourceDomainId Interrupt::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

InterruptError Interrupt::ValidateEventLocked(const InterruptEventToken event,
                                              const Foundation::UInt64 stateWord) const noexcept
{
    const InterruptState state = DecodeState(stateWord);
    if (state == InterruptState::Uninitialized)
    {
        return InterruptError::NotInitialized;
    }
    if (state == InterruptState::Quiesced)
    {
        return InterruptError::AlreadyQuiesced;
    }
    if (state == InterruptState::Faulted)
    {
        return InterruptError::Faulted;
    }
    if (!StateIsKnown(state))
    {
        return InterruptError::CorruptState;
    }
    if (!event.IsValid() || (event.m_interrupt != this))
    {
        return InterruptError::InvalidEvent;
    }
    if ((state != InterruptState::Pending) || (event.m_sequence != DecodeSequence(stateWord)))
    {
        return InterruptError::StaleEvent;
    }
    return InterruptError::None;
}

InterruptSnapshot Interrupt::GetSnapshotLocked() const noexcept
{
    const Foundation::UInt64 stateWord = m_stateWord.LoadAcquire();
    const InterruptState state = DecodeState(stateWord);
    const Foundation::UInt64 sequence = DecodeSequence(stateWord);
    return InterruptSnapshot{
        .Notification = m_notificationIdentity,
        .PendingEvent = state == InterruptState::Pending ? InterruptEventToken{*this, sequence} : InterruptEventToken{},
        .LastEventSequence = sequence,
        .State = state,
        .SourceClaimed = m_sourceClaimed,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

void Interrupt::EnterFaulted(const Foundation::UInt64 sequence) noexcept
{
    m_stateWord.StoreRelease(EncodeState(InterruptState::Faulted, sequence));
}
} // namespace Asteria::Kernel::Interrupt
