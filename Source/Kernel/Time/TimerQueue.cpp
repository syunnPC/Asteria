// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Time/TimerQueue.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Time
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
}

TimerQueue::~TimerQueue() noexcept
{
    for (Entry& entry : m_entries)
    {
        if (entry.State == EntryState::Empty)
        {
            continue;
        }
        if (entry.Storage != nullptr)
        {
            if (entry.State == EntryState::Scheduled)
            {
                static_cast<void>(entry.Storage->CancelFromQueue(*this, entry.Token));
            }
            else
            {
                static_cast<void>(entry.Storage->ReleaseFromQueue(*this, entry.Token));
            }
        }
        ClearEntryLocked(entry);
    }
}

TimerQueueFailure TimerQueue::MakeFailure(const TimerQueueError error, const Foundation::UInt64 nextDeadlineTicks,
                                          const TimerFailure& timerFailure,
                                          const Object::ObjectPinError pinError) noexcept
{
    return TimerQueueFailure{
        .Timer = timerFailure,
        .NextDeadlineTicks = nextDeadlineTicks,
        .Error = error,
        .Pin = pinError,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

void TimerQueue::SetFailure(TimerQueueFailure& failure, const TimerQueueError error,
                            const Foundation::UInt64 nextDeadlineTicks, const TimerFailure& timerFailure,
                            const Object::ObjectPinError pinError) noexcept
{
    failure.Timer = timerFailure;
    failure.NextDeadlineTicks = nextDeadlineTicks;
    failure.Error = error;
    failure.Pin = pinError;
    for (Foundation::Size index{0U}; index < sizeof(failure.Reserved); ++index)
    {
        failure.Reserved[index] = 0U;
    }
}

Foundation::Result<TimerQueueArmReceipt, TimerQueueFailure>
TimerQueue::Arm(const TimerId timerId, Timer& timer, const Object::MonotonicTime deadline) noexcept
{
    using ResultType = Foundation::Result<TimerQueueArmReceipt, TimerQueueFailure>;

    TimerQueueArmReceipt receipt{};
    TimerQueueFailure failure{};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!ArmLocked(timerId, timer, deadline, receipt, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(receipt);
}

bool TimerQueue::ArmLocked(const TimerId timerId, Timer& timer, const Object::MonotonicTime deadline,
                           TimerQueueArmReceipt& receipt, TimerQueueFailure& failure) noexcept
{
    const Foundation::UInt64 previousNextDeadline = GetNextDeadlineLocked();
    if (!timerId.IsValid())
    {
        failure = MakeFailure(TimerQueueError::InvalidTimer, previousNextDeadline);
        return false;
    }
    if (!timerId.IsFromRegistry(m_objectRegistry))
    {
        failure = MakeFailure(TimerQueueError::TimerRegistryMismatch, previousNextDeadline);
        return false;
    }
    if (&timer.m_clock != &m_clock)
    {
        failure = MakeFailure(TimerQueueError::ClockMismatch, previousNextDeadline);
        return false;
    }

    Entry* existingEntry{nullptr};
    Entry* emptyEntry{nullptr};
    for (Entry& entry : m_entries)
    {
        if (entry.State == EntryState::Empty)
        {
            if (emptyEntry == nullptr)
            {
                emptyEntry = &entry;
            }
            continue;
        }
        if (entry.Identity.IsEqualTo(timerId))
        {
            existingEntry = &entry;
            continue;
        }
        if (entry.Storage == &timer)
        {
            failure = MakeFailure(TimerQueueError::DuplicateTimer, previousNextDeadline);
            return false;
        }
    }

    if ((existingEntry != nullptr) && (existingEntry->Storage != &timer))
    {
        failure = MakeFailure(TimerQueueError::TimerInstanceMismatch, previousNextDeadline);
        return false;
    }
    if ((existingEntry == nullptr) && (emptyEntry == nullptr))
    {
        failure = MakeFailure(TimerQueueError::QueueFull, previousNextDeadline);
        return false;
    }

    Entry* const target = existingEntry != nullptr ? existingEntry : emptyEntry;
    const bool isNewEntry = existingEntry == nullptr;
    if (isNewEntry)
    {
        if (target->Pin.IsValid() || (target->Storage != nullptr) || target->Identity.IsValid() ||
            target->Token.IsValid() || (target->DeadlineTicks != 0U) || (target->ObservedTicks != 0U) ||
            (target->ClaimSequence != 0U))
        {
            failure = MakeFailure(TimerQueueError::QueueCorrupt, previousNextDeadline);
            return false;
        }

        const Object::ObjectPinError pinError =
            m_objectRegistry.PinObject(timerId.GetObjectId(), Object::ObjectType::Timer, target->Pin);
        if (pinError != Object::ObjectPinError::Ok)
        {
            failure = MakeFailure(TimerQueueError::TimerPinRejected, previousNextDeadline, {}, pinError);
            return false;
        }
        if (&target->Pin.GetObject() != static_cast<Object::IKernelObject*>(&timer))
        {
            target->Pin.Reset();
            failure = MakeFailure(TimerQueueError::TimerInstanceMismatch, previousNextDeadline);
            return false;
        }
    }
    else
    {
        if (!target->Pin.IsValid() || !target->Identity.IsValid() || !target->Token.IsValid() ||
            (target->DeadlineTicks == 0U) || (target->DeadlineTicks == Object::MonotonicTime::Infinite().GetTicks()))
        {
            failure = MakeFailure(TimerQueueError::QueueCorrupt, previousNextDeadline);
            return false;
        }

        if (!ValidateRearmedGenerationLocked(timerId, previousNextDeadline, failure))
        {
            return false;
        }
    }

    const auto armResult = timer.ArmAtFromQueue(*this, deadline);
    if (!armResult.HasValue())
    {
        if (isNewEntry)
        {
            target->Pin.Reset();
        }
        failure = MakeFailure(TimerQueueError::TimerRejected, previousNextDeadline, armResult.GetError());
        return false;
    }

    const TimerArmReceipt& armReceipt = armResult.GetValue();
    if (isNewEntry)
    {
        target->Identity = timerId;
        target->Storage = &timer;
        target->ObservedTicks = 0U;
        target->ClaimSequence = 0U;
        target->State = EntryState::Scheduled;
        ++m_scheduledEntries;
    }
    target->Token = armReceipt.Token;
    target->DeadlineTicks = armReceipt.DeadlineTicks;

    const Foundation::UInt64 nextDeadline = GetNextDeadlineLocked();
    receipt = TimerQueueArmReceipt{
        .Timer = armReceipt,
        .NextDeadlineTicks = nextDeadline,
    };
    return true;
}

Foundation::Result<TimerQueueCancelReceipt, TimerQueueFailure> TimerQueue::Cancel(const TimerId timerId, Timer& timer,
                                                                                  const TimerToken token) noexcept
{
    using ResultType = Foundation::Result<TimerQueueCancelReceipt, TimerQueueFailure>;

    TimerQueueCancelReceipt receipt{};
    TimerQueueFailure failure{};
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!CancelLocked(timerId, timer, token, receipt, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(receipt);
}

bool TimerQueue::CancelLocked(const TimerId timerId, Timer& timer, const TimerToken token,
                              TimerQueueCancelReceipt& receipt, TimerQueueFailure& failure) noexcept
{
    const Foundation::UInt64 previousNextDeadline = GetNextDeadlineLocked();
    if (!timerId.IsValid())
    {
        failure = MakeFailure(TimerQueueError::InvalidTimer, previousNextDeadline);
        return false;
    }
    if (!timerId.IsFromRegistry(m_objectRegistry))
    {
        failure = MakeFailure(TimerQueueError::TimerRegistryMismatch, previousNextDeadline);
        return false;
    }

    Entry* const entry = FindTimerLocked(timerId);
    if (entry == nullptr)
    {
        failure = MakeFailure(TimerQueueError::TimerNotFound, previousNextDeadline);
        return false;
    }
    if (entry->Storage != &timer)
    {
        failure = MakeFailure(TimerQueueError::TimerInstanceMismatch, previousNextDeadline);
        return false;
    }
    if (entry->State == EntryState::Claimed)
    {
        failure = MakeFailure(TimerQueueError::EntryClaimed, previousNextDeadline);
        return false;
    }
    if ((entry->State != EntryState::Scheduled) && (entry->State != EntryState::Pending))
    {
        failure = MakeFailure(TimerQueueError::QueueCorrupt, previousNextDeadline);
        return false;
    }
    // A Pending entry's Timer is Expired, which the Timer refuses to cancel;
    // the count still follows the entry's own state instead of that rule.
    Foundation::Size& stateEntries = (entry->State == EntryState::Scheduled) ? m_scheduledEntries : m_pendingEntries;
    if (stateEntries == 0U)
    {
        failure = MakeFailure(TimerQueueError::QueueCorrupt, previousNextDeadline);
        return false;
    }

    const auto cancelResult = timer.CancelFromQueue(*this, token);
    if (!cancelResult.HasValue())
    {
        failure = MakeFailure(TimerQueueError::TimerRejected, previousNextDeadline, cancelResult.GetError());
        return false;
    }

    const TimerSnapshot timerSnapshot = cancelResult.GetValue();
    ClearEntryLocked(*entry);
    --stateEntries;
    const Foundation::UInt64 nextDeadline = GetNextDeadlineLocked();
    receipt = TimerQueueCancelReceipt{
        .Timer = timerSnapshot,
        .NextDeadlineTicks = nextDeadline,
    };
    return true;
}

Foundation::Result<TimerQueueExpirationClaim, TimerQueueFailure> TimerQueue::ClaimDue() noexcept
{
    using ResultType = Foundation::Result<TimerQueueExpirationClaim, TimerQueueFailure>;

    TimerQueueExpirationClaim claim{};
    TimerQueueFailure failure{};
    if (!ClaimDue(claim, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(claim);
}

bool TimerQueue::ClaimDue(TimerQueueExpirationClaim& claim, TimerQueueFailure& failure) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ClaimDueLocked(claim, failure);
}

bool TimerQueue::ClaimDueLocked(TimerQueueExpirationClaim& claim, TimerQueueFailure& failure) noexcept
{
    Entry* const selected = SelectDueEntryLocked(failure);
    if (selected == nullptr)
    {
        return false;
    }
    if (!ExpireSelectedEntryLocked(*selected, failure))
    {
        return false;
    }
    PublishSelectedClaimLocked(*selected, claim);
    return true;
}

TimerQueue::Entry* TimerQueue::SelectDueEntryLocked(TimerQueueFailure& failure) noexcept
{
    Entry* selected = FindEarliestLocked(EntryState::Pending);
    if (selected == nullptr)
    {
        selected = FindEarliestLocked(EntryState::Scheduled);
    }
    const Foundation::UInt64 previousNextDeadline = GetNextDeadlineLocked();
    if (selected == nullptr)
    {
        SetFailure(failure, TimerQueueError::Empty, 0U);
        return nullptr;
    }
    if (m_nextClaimSequence == MaximumUInt64)
    {
        SetFailure(failure, TimerQueueError::ClaimSequenceExhausted, previousNextDeadline);
        return nullptr;
    }
    if (!selected->Pin.IsValid() || !selected->Identity.IsValid() ||
        !selected->Identity.IsFromRegistry(m_objectRegistry) || (selected->Storage == nullptr) ||
        (&selected->Storage->m_clock != &m_clock) || !selected->Token.IsValid() || (selected->DeadlineTicks == 0U) ||
        (selected->DeadlineTicks == Object::MonotonicTime::Infinite().GetTicks()))
    {
        SetFailure(failure, TimerQueueError::QueueCorrupt, previousNextDeadline);
        return nullptr;
    }

    return selected;
}

bool TimerQueue::ExpireSelectedEntryLocked(Entry& entry, TimerQueueFailure& failure) noexcept
{
    const Foundation::UInt64 previousNextDeadline = GetNextDeadlineLocked();
    if (entry.State == EntryState::Scheduled)
    {
        if (m_scheduledEntries == 0U)
        {
            SetFailure(failure, TimerQueueError::QueueCorrupt, previousNextDeadline);
            return false;
        }
        const auto expirationResult = entry.Storage->TryExpireFromQueue(*this, entry.Token);
        if (!expirationResult.HasValue())
        {
            const TimerFailure& timerFailure = expirationResult.GetErrorReference();
            SetFailure(failure,
                       timerFailure.Error == TimerError::NotExpired ? TimerQueueError::NotDue
                                                                    : TimerQueueError::TimerRejected,
                       previousNextDeadline, timerFailure);
            return false;
        }
        m_expirationScratch = expirationResult.GetValue();
        entry.ObservedTicks = m_expirationScratch.ObservedTicks;
        --m_scheduledEntries;
    }
    else if (entry.State == EntryState::Pending)
    {
        if (m_pendingEntries == 0U)
        {
            SetFailure(failure, TimerQueueError::QueueCorrupt, previousNextDeadline);
            return false;
        }
        m_expirationScratch.Token = entry.Token;
        m_expirationScratch.DeadlineTicks = entry.DeadlineTicks;
        m_expirationScratch.ObservedTicks = entry.ObservedTicks;
        --m_pendingEntries;
    }
    else
    {
        SetFailure(failure, TimerQueueError::QueueCorrupt, previousNextDeadline);
        return false;
    }

    return true;
}

void TimerQueue::PublishSelectedClaimLocked(Entry& entry, TimerQueueExpirationClaim& claim) noexcept
{
    const Foundation::UInt64 claimSequence = m_nextClaimSequence;
    ++m_nextClaimSequence;
    entry.ClaimSequence = claimSequence;
    entry.State = EntryState::Claimed;
    ++m_claimedEntries;
    claim.m_claimToken.m_queue = this;
    claim.m_claimToken.m_sequence = claimSequence;
    claim.m_timerId = entry.Identity;
    claim.m_expiration = m_expirationScratch;
    claim.m_timer = entry.Storage;
    claim.m_nextDeadlineTicks = GetNextDeadlineLocked();
}

Foundation::Result<TimerQueueExpirationClaim, TimerQueueFailure>
TimerQueue::ValidateClaimForDelivery(const TimerQueueExpirationClaim& candidate) noexcept
{
    using ResultType = Foundation::Result<TimerQueueExpirationClaim, TimerQueueFailure>;

    TimerQueueExpirationClaim canonical{};
    TimerQueueFailure failure{};
    if (!ValidateClaimForDelivery(candidate, canonical, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(canonical);
}

bool TimerQueue::ValidateClaimForDelivery(const TimerQueueExpirationClaim& candidate,
                                          TimerQueueExpirationClaim& canonical, TimerQueueFailure& failure) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ValidateClaimForDeliveryLocked(candidate, canonical, failure);
}

bool TimerQueue::ValidateClaimForDeliveryLocked(const TimerQueueExpirationClaim& candidate,
                                                TimerQueueExpirationClaim& canonical,
                                                TimerQueueFailure& failure) noexcept
{
    const Foundation::UInt64 nextDeadline = GetNextDeadlineLocked();
    const TimerQueueClaimToken claimToken = candidate.m_claimToken;
    if (!claimToken.IsValid() || (claimToken.m_queue != this))
    {
        failure = MakeFailure(TimerQueueError::InvalidClaim, nextDeadline);
        return false;
    }

    Entry* const entry = FindClaimLocked(claimToken);
    if (entry == nullptr)
    {
        failure = MakeFailure(TimerQueueError::StaleClaim, nextDeadline);
        return false;
    }
    if ((entry->State != EntryState::Claimed) || (m_claimedEntries == 0U) || !entry->Pin.IsValid() ||
        !entry->Identity.IsValid() || (entry->Storage == nullptr) || !entry->Token.IsValid() ||
        (entry->ClaimSequence != claimToken.GetSequence()))
    {
        failure = MakeFailure(TimerQueueError::QueueCorrupt, nextDeadline);
        return false;
    }

    if (!candidate.m_timerId.IsEqualTo(entry->Identity) || (candidate.m_timer != entry->Storage) ||
        !candidate.m_expiration.Token.IsEqualTo(entry->Token) ||
        (candidate.m_expiration.DeadlineTicks != entry->DeadlineTicks) ||
        (candidate.m_expiration.ObservedTicks != entry->ObservedTicks))
    {
        failure = MakeFailure(TimerQueueError::StaleClaim, nextDeadline);
        return false;
    }

    canonical.m_claimToken = claimToken;
    canonical.m_timerId = entry->Identity;
    canonical.m_expiration.Token = entry->Token;
    canonical.m_expiration.DeadlineTicks = entry->DeadlineTicks;
    canonical.m_expiration.ObservedTicks = entry->ObservedTicks;
    canonical.m_timer = entry->Storage;
    canonical.m_nextDeadlineTicks = nextDeadline;
    return true;
}

Foundation::Result<TimerQueueState, TimerQueueFailure>
TimerQueue::CommitClaim(const TimerQueueClaimToken token) noexcept
{
    using ResultType = Foundation::Result<TimerQueueState, TimerQueueFailure>;

    TimerQueueState state{};
    TimerQueueFailure failure{};
    if (!CommitClaim(token, state, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(state);
}

bool TimerQueue::CommitClaim(const TimerQueueClaimToken token, TimerQueueState& state,
                             TimerQueueFailure& failure) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return CommitClaimLocked(token, state, failure);
}

bool TimerQueue::CommitClaimLocked(const TimerQueueClaimToken token, TimerQueueState& state,
                                   TimerQueueFailure& failure) noexcept
{
    Entry* const entry = FindUnresolvedClaimLocked(token, failure);
    if (entry == nullptr)
    {
        return false;
    }

    const TimerError releaseError = entry->Storage->ReleaseFromQueue(*this, entry->Token);
    if (releaseError != TimerError::None)
    {
        failure =
            MakeFailure(TimerQueueError::TimerRejected, GetNextDeadlineLocked(), Timer::MakeFailure(releaseError));
        return false;
    }

    ClearEntryLocked(*entry);
    --m_claimedEntries;
    state = GetStateLocked();
    return true;
}

Foundation::Result<TimerQueueState, TimerQueueFailure>
TimerQueue::CancelClaim(const TimerQueueClaimToken token) noexcept
{
    using ResultType = Foundation::Result<TimerQueueState, TimerQueueFailure>;

    TimerQueueState state{};
    TimerQueueFailure failure{};
    if (!CancelClaim(token, state, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(state);
}

bool TimerQueue::CancelClaim(const TimerQueueClaimToken token, TimerQueueState& state,
                             TimerQueueFailure& failure) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return CancelClaimLocked(token, state, failure);
}

bool TimerQueue::CancelClaimLocked(const TimerQueueClaimToken token, TimerQueueState& state,
                                   TimerQueueFailure& failure) noexcept
{
    Entry* const entry = FindUnresolvedClaimLocked(token, failure);
    if (entry == nullptr)
    {
        return false;
    }

    entry->ClaimSequence = 0U;
    entry->State = EntryState::Pending;
    --m_claimedEntries;
    ++m_pendingEntries;
    state = GetStateLocked();
    return true;
}

Foundation::Result<TimerQueueState, TimerQueueFailure>
TimerQueue::RetireClaim(const TimerQueueClaimToken token) noexcept
{
    using ResultType = Foundation::Result<TimerQueueState, TimerQueueFailure>;

    TimerQueueState state{};
    TimerQueueFailure failure{};
    if (!RetireClaim(token, state, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(state);
}

bool TimerQueue::RetireClaim(const TimerQueueClaimToken token, TimerQueueState& state,
                             TimerQueueFailure& failure) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return RetireClaimLocked(token, state, failure);
}

bool TimerQueue::RetireClaimLocked(const TimerQueueClaimToken token, TimerQueueState& state,
                                   TimerQueueFailure& failure) noexcept
{
    Entry* const entry = FindUnresolvedClaimLocked(token, failure);
    if (entry == nullptr)
    {
        return false;
    }

    // The entry's pin still proves the Timer's lifetime here, even when its
    // registry generation is being destroyed; it is released only below.
    const TimerError retireError = entry->Storage->RetireFromQueue(*this, entry->Token);
    if (retireError != TimerError::None)
    {
        failure = MakeFailure(TimerQueueError::TimerRejected, GetNextDeadlineLocked(), Timer::MakeFailure(retireError));
        return false;
    }

    ClearEntryLocked(*entry);
    --m_claimedEntries;
    state = GetStateLocked();
    return true;
}

TimerQueue::Entry* TimerQueue::FindUnresolvedClaimLocked(const TimerQueueClaimToken token,
                                                         TimerQueueFailure& failure) noexcept
{
    const Foundation::UInt64 nextDeadline = GetNextDeadlineLocked();
    if (!token.IsValid() || (token.m_queue != this))
    {
        failure = MakeFailure(TimerQueueError::InvalidClaim, nextDeadline);
        return nullptr;
    }
    Entry* const entry = FindClaimLocked(token);
    if (entry == nullptr)
    {
        failure = MakeFailure(TimerQueueError::StaleClaim, nextDeadline);
        return nullptr;
    }
    if ((m_claimedEntries == 0U) || !entry->Pin.IsValid() || (entry->Storage == nullptr) || !entry->Token.IsValid())
    {
        failure = MakeFailure(TimerQueueError::QueueCorrupt, nextDeadline);
        return nullptr;
    }
    return entry;
}

bool TimerQueue::ValidateRearmedGenerationLocked(const TimerId timerId, const Foundation::UInt64 previousNextDeadline,
                                                 TimerQueueFailure& failure) noexcept
{
    // The entry's retained pin keeps a destroyed generation alive but does not
    // authorize new work, so a re-arm needs the same live generation as a new
    // arm. That pin already proved that this identity names exactly the
    // entry's Timer, so only liveness is checked, with a pin released at once.
    Object::ObjectPin generation{};
    const Object::ObjectPinError pinError =
        m_objectRegistry.PinObject(timerId.GetObjectId(), Object::ObjectType::Timer, generation);
    if (pinError == Object::ObjectPinError::Ok)
    {
        return true;
    }
    failure = MakeFailure(TimerQueueError::TimerPinRejected, previousNextDeadline, {}, pinError);
    return false;
}

TimerQueue::Entry* TimerQueue::FindTimerLocked(const TimerId timerId) noexcept
{
    for (Entry& entry : m_entries)
    {
        if ((entry.State != EntryState::Empty) && entry.Identity.IsEqualTo(timerId))
        {
            return &entry;
        }
    }
    return nullptr;
}

TimerQueue::Entry* TimerQueue::FindClaimLocked(const TimerQueueClaimToken token) noexcept
{
    for (Entry& entry : m_entries)
    {
        if ((entry.State == EntryState::Claimed) && (entry.ClaimSequence == token.GetSequence()))
        {
            return &entry;
        }
    }
    return nullptr;
}

TimerQueue::Entry* TimerQueue::FindEarliestLocked(const EntryState state) noexcept
{
    Entry* selected{nullptr};
    for (Entry& entry : m_entries)
    {
        if ((entry.State == state) && ((selected == nullptr) || (entry.DeadlineTicks < selected->DeadlineTicks)))
        {
            selected = &entry;
        }
    }
    return selected;
}

Foundation::UInt64 TimerQueue::GetNextDeadlineLocked() const noexcept
{
    Foundation::UInt64 nextDeadline{0U};
    for (const Entry& entry : m_entries)
    {
        if ((entry.State == EntryState::Scheduled) && ((nextDeadline == 0U) || (entry.DeadlineTicks < nextDeadline)))
        {
            nextDeadline = entry.DeadlineTicks;
        }
    }
    return nextDeadline;
}

TimerQueueState TimerQueue::GetStateLocked() const noexcept
{
    return TimerQueueState{
        .NextDeadlineTicks = GetNextDeadlineLocked(),
        .NextClaimSequence = m_nextClaimSequence,
        .ScheduledEntries = static_cast<Foundation::UInt8>(m_scheduledEntries),
        .PendingEntries = static_cast<Foundation::UInt8>(m_pendingEntries),
        .ClaimedEntries = static_cast<Foundation::UInt8>(m_claimedEntries),
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    };
}

void TimerQueue::ClearEntryLocked(Entry& entry) noexcept
{
    entry.Identity = TimerId{};
    entry.Storage = nullptr;
    entry.Token = TimerToken{};
    entry.DeadlineTicks = 0U;
    entry.ObservedTicks = 0U;
    entry.ClaimSequence = 0U;
    entry.State = EntryState::Empty;
    entry.Pin.Reset();
}

TimerQueueState TimerQueue::GetState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetStateLocked();
}
} // namespace Asteria::Kernel::Time
