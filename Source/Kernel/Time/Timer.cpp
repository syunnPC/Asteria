// SPDX-License-Identifier: Apache-2.0

#include "Timer.hpp"

namespace Asteria::Kernel::Time
{
TimerFailure Timer::MakeFailure(const TimerError error, const Foundation::UInt64 observedTicks) noexcept
{
    return TimerFailure{
        .ObservedTicks = observedTicks,
        .Error = error,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

TimerError Timer::Initialize(const Object::ResourceDomainId chargeOwner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_initialized)
    {
        return TimerError::AlreadyInitialized;
    }
    if (!chargeOwner.IsValid())
    {
        return TimerError::InvalidConfiguration;
    }

    m_state = TimerState::Disarmed;
    m_initialized = true;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return TimerError::None;
}

Foundation::Result<TimerArmReceipt, TimerFailure> Timer::ArmAt(const Object::MonotonicTime deadline) noexcept
{
    using ResultType = Foundation::Result<TimerArmReceipt, TimerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != nullptr)
    {
        return ResultType::Failure(MakeFailure(TimerError::ManagedByQueue));
    }
    return ArmAtLocked(deadline);
}

Foundation::Result<TimerArmReceipt, TimerFailure> Timer::ArmAtLocked(const Object::MonotonicTime deadline) noexcept
{
    using ResultType = Foundation::Result<TimerArmReceipt, TimerFailure>;

    if (!m_initialized)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotInitialized));
    }
    if (deadline.IsInfinite())
    {
        return ResultType::Failure(MakeFailure(TimerError::InvalidDeadline));
    }
    if (m_state == TimerState::Expired)
    {
        return ResultType::Failure(MakeFailure(TimerError::ExpirationPending));
    }

    Foundation::UInt64 nextSequence{m_armSequence};
    if (!TimerArmSequencePolicy::TryAdvance(m_armSequence, nextSequence))
    {
        return ResultType::Failure(MakeFailure(TimerError::SequenceExhausted));
    }

    const Foundation::UInt64 observedTicks = m_clock.GetTicks();
    if (observedTicks == Object::MonotonicTime::Infinite().GetTicks())
    {
        return ResultType::Failure(MakeFailure(TimerError::ClockExhausted, observedTicks));
    }
    if (deadline.GetTicks() <= observedTicks)
    {
        return ResultType::Failure(MakeFailure(TimerError::DeadlineReached, observedTicks));
    }

    const bool replaced = m_state == TimerState::Armed;
    m_armSequence = nextSequence;
    m_deadlineTicks = deadline.GetTicks();
    m_state = TimerState::Armed;
    return ResultType::Success(TimerArmReceipt{
        .Token = TimerToken{*this, m_armSequence},
        .DeadlineTicks = m_deadlineTicks,
        .ObservedTicks = observedTicks,
        .Replaced = replaced,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    });
}

Foundation::Result<TimerArmReceipt, TimerFailure> Timer::ArmAtFromQueue(const TimerQueue& queue,
                                                                        const Object::MonotonicTime deadline) noexcept
{
    using ResultType = Foundation::Result<TimerArmReceipt, TimerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_timerQueue != nullptr) && (m_timerQueue != &queue))
    {
        return ResultType::Failure(MakeFailure(TimerError::ManagedByQueue));
    }
    const auto result = ArmAtLocked(deadline);
    if (result.HasValue())
    {
        m_timerQueue = &queue;
    }
    return result;
}

Foundation::Result<TimerSnapshot, TimerFailure> Timer::Cancel(const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerSnapshot, TimerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != nullptr)
    {
        return ResultType::Failure(MakeFailure(TimerError::ManagedByQueue));
    }
    return CancelLocked(token);
}

Foundation::Result<TimerSnapshot, TimerFailure> Timer::CancelLocked(const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerSnapshot, TimerFailure>;

    if (!m_initialized)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotInitialized));
    }
    const TimerError tokenError = ValidateTokenLocked(token);
    if (tokenError != TimerError::None)
    {
        return ResultType::Failure(MakeFailure(tokenError));
    }
    if (m_state == TimerState::Expired)
    {
        return ResultType::Failure(MakeFailure(TimerError::ExpirationPending));
    }
    if (m_state != TimerState::Armed)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotArmed));
    }

    m_deadlineTicks = 0U;
    m_state = TimerState::Disarmed;
    return ResultType::Success(GetSnapshotLocked());
}

Foundation::Result<TimerSnapshot, TimerFailure> Timer::CancelFromQueue(const TimerQueue& queue,
                                                                       const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerSnapshot, TimerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != &queue)
    {
        return ResultType::Failure(MakeFailure(TimerError::ManagedByQueue));
    }
    const auto result = CancelLocked(token);
    if (result.HasValue())
    {
        m_timerQueue = nullptr;
    }
    return result;
}

Foundation::Result<TimerExpiration, TimerFailure> Timer::TryExpire(const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerExpiration, TimerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != nullptr)
    {
        return ResultType::Failure(MakeFailure(TimerError::ManagedByQueue));
    }
    return TryExpireLocked(token);
}

Foundation::Result<TimerExpiration, TimerFailure> Timer::TryExpireLocked(const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerExpiration, TimerFailure>;

    if (!m_initialized)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotInitialized));
    }
    const TimerError tokenError = ValidateTokenLocked(token);
    if (tokenError != TimerError::None)
    {
        return ResultType::Failure(MakeFailure(tokenError));
    }
    if (m_state == TimerState::Expired)
    {
        return ResultType::Failure(MakeFailure(TimerError::AlreadyExpired));
    }
    if (m_state != TimerState::Armed)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotArmed));
    }

    const Foundation::UInt64 observedTicks = m_clock.GetTicks();
    if (observedTicks == Object::MonotonicTime::Infinite().GetTicks())
    {
        return ResultType::Failure(MakeFailure(TimerError::ClockExhausted, observedTicks));
    }
    if (observedTicks < m_deadlineTicks)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotExpired, observedTicks));
    }

    m_state = TimerState::Expired;
    return ResultType::Success(TimerExpiration{
        .Token = TimerToken{*this, m_armSequence},
        .DeadlineTicks = m_deadlineTicks,
        .ObservedTicks = observedTicks,
    });
}

Foundation::Result<TimerExpiration, TimerFailure> Timer::TryExpireFromQueue(const TimerQueue& queue,
                                                                            const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerExpiration, TimerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != &queue)
    {
        return ResultType::Failure(MakeFailure(TimerError::ManagedByQueue));
    }
    return TryExpireLocked(token);
}

Foundation::Result<TimerSnapshot, TimerFailure> Timer::Acknowledge(const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerSnapshot, TimerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != nullptr)
    {
        return ResultType::Failure(MakeFailure(TimerError::ManagedByQueue));
    }
    return AcknowledgeLocked(token);
}

Foundation::Result<TimerSnapshot, TimerFailure> Timer::AcknowledgeLocked(const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerSnapshot, TimerFailure>;

    if (!m_initialized)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotInitialized));
    }
    const TimerError tokenError = ValidateTokenLocked(token);
    if (tokenError != TimerError::None)
    {
        return ResultType::Failure(MakeFailure(tokenError));
    }
    if (m_state == TimerState::Armed)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotExpired));
    }
    if (m_state != TimerState::Expired)
    {
        return ResultType::Failure(MakeFailure(TimerError::NotArmed));
    }

    m_deadlineTicks = 0U;
    m_state = TimerState::Disarmed;
    return ResultType::Success(GetSnapshotLocked());
}

TimerError Timer::ReleaseFromQueue(const TimerQueue& queue, const TimerToken token) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != &queue)
    {
        return TimerError::ManagedByQueue;
    }
    const TimerError tokenError = ValidateTokenLocked(token);
    if (tokenError != TimerError::None)
    {
        return tokenError;
    }
    if (m_state == TimerState::Armed)
    {
        return TimerError::NotExpired;
    }
    if (m_state != TimerState::Expired)
    {
        return TimerError::NotArmed;
    }
    m_timerQueue = nullptr;
    return TimerError::None;
}

TimerError Timer::RetireFromQueue(const TimerQueue& queue, const TimerToken token) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_timerQueue != &queue)
    {
        return TimerError::ManagedByQueue;
    }
    const auto acknowledged = AcknowledgeLocked(token);
    if (!acknowledged.HasValue())
    {
        return acknowledged.GetErrorReference().Error;
    }
    m_timerQueue = nullptr;
    return TimerError::None;
}

bool Timer::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_initialized;
}

TimerSnapshot Timer::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetSnapshotLocked();
}

Object::ObjectType Timer::GetObjectType() const noexcept
{
    return Object::ObjectType::Timer;
}

Object::ResourceDomainId Timer::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

TimerError Timer::ValidateTokenLocked(const TimerToken token) const noexcept
{
    if (!token.IsValid() || (token.m_timer != this))
    {
        return TimerError::InvalidToken;
    }
    if (token.GetSequence() != m_armSequence)
    {
        return TimerError::StaleToken;
    }
    return TimerError::None;
}

TimerSnapshot Timer::GetSnapshotLocked() const noexcept
{
    const bool hasGeneration = (m_state == TimerState::Armed) || (m_state == TimerState::Expired);
    return TimerSnapshot{
        .Token = hasGeneration ? TimerToken{*this, m_armSequence} : TimerToken{},
        .DeadlineTicks = hasGeneration ? m_deadlineTicks : 0U,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace Asteria::Kernel::Time
