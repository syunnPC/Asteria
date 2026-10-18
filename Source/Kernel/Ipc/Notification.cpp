// SPDX-License-Identifier: Apache-2.0

#include "Notification.hpp"

namespace Asteria::Kernel::Ipc
{
NotificationFailure Notification::MakeFailure(const NotificationError error) noexcept
{
    return NotificationFailure{
        .Error = error,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

NotificationError Notification::Initialize(const Object::ResourceDomainId chargeOwner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_state != NotificationState::Uninitialized)
    {
        return NotificationError::AlreadyInitialized;
    }
    if (!chargeOwner.IsValid())
    {
        return NotificationError::InvalidConfiguration;
    }

    m_lastClaimSequence = 0U;
    m_activeClaimSequence = 0U;
    m_waitSet = nullptr;
    m_pending.StoreRelease(0U);
    m_state = NotificationState::Ready;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return NotificationError::None;
}

NotificationError Notification::Signal() noexcept
{
    if (m_chargeOwnerValue.LoadAcquire() == 0U)
    {
        return NotificationError::NotInitialized;
    }

    // The acquire half carries every earlier coalesced producer publication
    // into this release, so the eventual Claim observes all producer writes.
    // Modification order places this one always-lock-free exchange either
    // before a concurrent Claim exchange or after it.
    static_cast<void>(m_pending.ExchangeAcquireRelease(1U));
    return NotificationError::None;
}

Foundation::Result<NotificationClaimToken, NotificationFailure> Notification::Claim() noexcept
{
    using ResultType = Foundation::Result<NotificationClaimToken, NotificationFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_waitSet != nullptr)
    {
        return ResultType::Failure(MakeFailure(NotificationError::ManagedByWaitSet));
    }
    return ClaimLocked();
}

Foundation::Result<NotificationClaimToken, NotificationFailure> Notification::ClaimLocked() noexcept
{
    using ResultType = Foundation::Result<NotificationClaimToken, NotificationFailure>;

    if (m_state == NotificationState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(NotificationError::NotInitialized));
    }
    if (m_state == NotificationState::Claimed)
    {
        return ResultType::Failure(MakeFailure(NotificationError::ClaimActive));
    }

    Foundation::UInt64 nextSequence{m_lastClaimSequence};
    if (!NotificationClaimSequencePolicy::TryAdvance(m_lastClaimSequence, nextSequence))
    {
        return ResultType::Failure(MakeFailure(NotificationError::ClaimSequenceExhausted));
    }

    if (m_pending.ExchangeAcquireRelease(0U) == 0U)
    {
        return ResultType::Failure(MakeFailure(NotificationError::WouldBlock));
    }

    m_lastClaimSequence = nextSequence;
    m_activeClaimSequence = nextSequence;
    m_state = NotificationState::Claimed;
    return ResultType::Success(NotificationClaimToken{
        *this,
        m_activeClaimSequence,
    });
}

Foundation::Result<NotificationSnapshot, NotificationFailure>
Notification::CommitClaim(const NotificationClaimToken token) noexcept
{
    using ResultType = Foundation::Result<NotificationSnapshot, NotificationFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_waitSet != nullptr)
    {
        return ResultType::Failure(MakeFailure(NotificationError::ManagedByWaitSet));
    }
    return CommitClaimLocked(token);
}

Foundation::Result<NotificationSnapshot, NotificationFailure>
Notification::CommitClaimLocked(const NotificationClaimToken token) noexcept
{
    using ResultType = Foundation::Result<NotificationSnapshot, NotificationFailure>;

    const NotificationError validation = ValidateClaimLocked(token);
    if (validation != NotificationError::None)
    {
        return ResultType::Failure(MakeFailure(validation));
    }

    m_activeClaimSequence = 0U;
    m_state = NotificationState::Ready;
    return ResultType::Success(GetSnapshotLocked());
}

Foundation::Result<NotificationSnapshot, NotificationFailure>
Notification::CancelClaim(const NotificationClaimToken token) noexcept
{
    using ResultType = Foundation::Result<NotificationSnapshot, NotificationFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_waitSet != nullptr)
    {
        return ResultType::Failure(MakeFailure(NotificationError::ManagedByWaitSet));
    }
    return CancelClaimLocked(token);
}

Foundation::Result<NotificationSnapshot, NotificationFailure>
Notification::CancelClaimLocked(const NotificationClaimToken token) noexcept
{
    using ResultType = Foundation::Result<NotificationSnapshot, NotificationFailure>;

    const NotificationError validation = ValidateClaimLocked(token);
    if (validation != NotificationError::None)
    {
        return ResultType::Failure(MakeFailure(validation));
    }

    // Re-publish the claimed event before making a new claim possible.
    static_cast<void>(m_pending.ExchangeAcquireRelease(1U));
    m_activeClaimSequence = 0U;
    m_state = NotificationState::Ready;
    return ResultType::Success(GetSnapshotLocked());
}

NotificationError Notification::AttachWaitSet(const WaitSet& waitSet) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_state == NotificationState::Uninitialized)
    {
        return NotificationError::NotInitialized;
    }
    if (m_state == NotificationState::Claimed)
    {
        return NotificationError::ClaimActive;
    }
    if (m_waitSet != nullptr)
    {
        return NotificationError::ManagedByWaitSet;
    }
    m_waitSet = &waitSet;
    return NotificationError::None;
}

NotificationError Notification::DetachWaitSet(const WaitSet& waitSet) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_state == NotificationState::Uninitialized)
    {
        return NotificationError::NotInitialized;
    }
    if (m_waitSet != &waitSet)
    {
        return NotificationError::WaitSetMismatch;
    }
    if (m_state == NotificationState::Claimed)
    {
        return NotificationError::ClaimActive;
    }
    m_waitSet = nullptr;
    return NotificationError::None;
}

Foundation::Result<NotificationClaimToken, NotificationFailure>
Notification::ClaimFromWaitSet(const WaitSet& waitSet) noexcept
{
    using ResultType = Foundation::Result<NotificationClaimToken, NotificationFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_waitSet != &waitSet)
    {
        return ResultType::Failure(MakeFailure(NotificationError::WaitSetMismatch));
    }
    return ClaimLocked();
}

Foundation::Result<NotificationSnapshot, NotificationFailure>
Notification::CommitClaimFromWaitSet(const WaitSet& waitSet, const NotificationClaimToken token) noexcept
{
    using ResultType = Foundation::Result<NotificationSnapshot, NotificationFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_waitSet != &waitSet)
    {
        return ResultType::Failure(MakeFailure(NotificationError::WaitSetMismatch));
    }
    return CommitClaimLocked(token);
}

Foundation::Result<NotificationSnapshot, NotificationFailure>
Notification::CancelClaimFromWaitSet(const WaitSet& waitSet, const NotificationClaimToken token) noexcept
{
    using ResultType = Foundation::Result<NotificationSnapshot, NotificationFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    if (m_waitSet != &waitSet)
    {
        return ResultType::Failure(MakeFailure(NotificationError::WaitSetMismatch));
    }
    return CancelClaimLocked(token);
}

bool Notification::IsInitialized() const noexcept
{
    return m_chargeOwnerValue.LoadAcquire() != 0U;
}

NotificationSnapshot Notification::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_claimLock};
    return GetSnapshotLocked();
}

Object::ObjectType Notification::GetObjectType() const noexcept
{
    return Object::ObjectType::Notification;
}

Object::ResourceDomainId Notification::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

NotificationError Notification::ValidateClaimLocked(const NotificationClaimToken token) const noexcept
{
    if (m_state == NotificationState::Uninitialized)
    {
        return NotificationError::NotInitialized;
    }
    if (!token.IsValid() || (token.m_notification != this))
    {
        return NotificationError::InvalidClaim;
    }
    if ((m_state != NotificationState::Claimed) || (token.m_sequence != m_activeClaimSequence))
    {
        return NotificationError::StaleClaim;
    }
    return NotificationError::None;
}

NotificationSnapshot Notification::GetSnapshotLocked() const noexcept
{
    return NotificationSnapshot{
        .LastClaimSequence = m_lastClaimSequence,
        .State = m_state,
        .Pending = static_cast<Foundation::UInt8>(m_pending.LoadAcquire() != 0U),
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace Asteria::Kernel::Ipc
