// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Scheduler/ReadyQueue.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Scheduler
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr bool IsRunnableContextState(const SchedulingContextState state) noexcept
{
    switch (state)
    {
    case SchedulingContextState::Bound:
    case SchedulingContextState::Donated:
    case SchedulingContextState::BudgetExhausted:
        return true;

    case SchedulingContextState::Uninitialized:
    case SchedulingContextState::Unbound:
    case SchedulingContextState::Running:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr bool ThreadIsReady(const ThreadSnapshot& snapshot) noexcept
{
    return (snapshot.State == ThreadState::Ready) && !snapshot.BlockToken.IsValid() && (snapshot.ExitCode == 0U) &&
           !snapshot.RunningProcessor.IsValid();
}

[[nodiscard]] constexpr bool ThreadIsBlockedAt(const ThreadSnapshot& snapshot, const ThreadBlockToken token) noexcept
{
    return (snapshot.State == ThreadState::Blocked) && snapshot.BlockToken.IsEqualTo(token) &&
           (snapshot.ExitCode == 0U) && !snapshot.RunningProcessor.IsValid();
}

[[nodiscard]] constexpr bool IsReservableContextState(const SchedulingContextState state) noexcept
{
    switch (state)
    {
    case SchedulingContextState::Bound:
    case SchedulingContextState::Donated:
    case SchedulingContextState::BudgetExhausted:
        return true;

    case SchedulingContextState::Uninitialized:
    case SchedulingContextState::Unbound:
    case SchedulingContextState::Running:
        return false;
    }
    return false;
}
} // namespace

ReadyQueue::~ReadyQueue() noexcept
{
    for (Entry& entry : m_entries)
    {
        if (entry.State != EntryState::Empty)
        {
            static_cast<void>(ClearEntryLocked(entry));
        }
    }
}

ReadyQueueFailure ReadyQueue::MakeFailure(const ReadyQueueError error, const Object::ObjectPinError contextPin,
                                          const Object::ObjectPinError threadPin) noexcept
{
    return ReadyQueueFailure{
        .Error = error,
        .ContextPin = contextPin,
        .ThreadPin = threadPin,
        .Reserved = 0U,
    };
}

Foundation::UInt8 ReadyQueue::ClassRank(const SchedulingClass schedulingClass) noexcept
{
    switch (schedulingClass)
    {
    case SchedulingClass::EmergencyRecovery:
        return 0U;

    case SchedulingClass::Isolated:
        return 1U;

    case SchedulingClass::DeadlineSporadic:
        return 2U;

    case SchedulingClass::WeightedFair:
        return 3U;

    case SchedulingClass::Background:
        return 4U;
    }
    return 0xFFU;
}

Foundation::UInt64 ReadyQueue::RankDeadlineLocked(Entry& entry, const Object::MonotonicTime now) noexcept
{
    // The effective deadline changes only when now reaches it, or when the
    // context itself changes, which republishes and therefore resets the entry.
    if ((entry.RankSampleTicks <= now.GetTicks()) && (now.GetTicks() < entry.RankDeadlineTicks))
    {
        return entry.RankDeadlineTicks;
    }
    entry.RankDeadlineTicks = entry.Context->GetEffectiveDeadline(now).GetTicks();
    entry.RankSampleTicks = now.GetTicks();
    return entry.RankDeadlineTicks;
}

void ReadyQueue::ResetRankLocked(Entry& entry) noexcept
{
    entry.RankDeadlineTicks = 0U;
    entry.RankSampleTicks = 0U;
}

bool ReadyQueue::IsBetterCandidateLocked(Entry& candidate, Entry& current, const Object::MonotonicTime now) noexcept
{
    const Foundation::UInt8 candidateRank = ClassRank(candidate.Configuration.Class);
    const Foundation::UInt8 currentRank = ClassRank(current.Configuration.Class);
    if (candidateRank != currentRank)
    {
        return candidateRank < currentRank;
    }

    const Foundation::UInt8 candidateCriticality = static_cast<Foundation::UInt8>(candidate.Configuration.Criticality);
    const Foundation::UInt8 currentCriticality = static_cast<Foundation::UInt8>(current.Configuration.Criticality);
    if (candidateCriticality != currentCriticality)
    {
        return candidateCriticality > currentCriticality;
    }
    if (candidate.Configuration.Priority != current.Configuration.Priority)
    {
        return candidate.Configuration.Priority > current.Configuration.Priority;
    }
    if (candidate.Configuration.Class == SchedulingClass::DeadlineSporadic)
    {
        const Foundation::UInt64 candidateDeadline = RankDeadlineLocked(candidate, now);
        const Foundation::UInt64 currentDeadline = RankDeadlineLocked(current, now);
        if (candidateDeadline != currentDeadline)
        {
            return candidateDeadline < currentDeadline;
        }
    }
    return candidate.Sequence < current.Sequence;
}

Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure> ReadyQueue::Enqueue(const SchedulingContextId contextId,
                                                                                    SchedulingContext& context,
                                                                                    const ThreadId threadId,
                                                                                    Thread& thread) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_processor.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidConfiguration));
    }
    if (!contextId.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidContext));
    }
    if (!contextId.IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextRegistryMismatch));
    }
    if (!threadId.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidThread));
    }
    if (!threadId.IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadRegistryMismatch));
    }

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
        if (entry.ContextIdentity.IsEqualTo(contextId))
        {
            return ResultType::Failure(MakeFailure(ReadyQueueError::DuplicateContext));
        }
        if (entry.ThreadIdentity.IsEqualTo(threadId))
        {
            return ResultType::Failure(MakeFailure(ReadyQueueError::DuplicateThread));
        }
    }
    if (emptyEntry == nullptr)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::QueueFull));
    }
    if (m_nextSequence == MaximumUInt64)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::SequenceExhausted));
    }
    if (emptyEntry->ContextPin.IsValid() || emptyEntry->ThreadPin.IsValid() || (emptyEntry->Context != nullptr) ||
        (emptyEntry->ThreadStorage != nullptr))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::QueueCorrupt));
    }

    const Object::ObjectPinError contextPinError = m_objectRegistry.PinObject(
        contextId.GetObjectId(), Object::ObjectType::SchedulingContext, emptyEntry->ContextPin);
    if (contextPinError != Object::ObjectPinError::Ok)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextPinRejected, contextPinError));
    }
    if (&emptyEntry->ContextPin.GetObject() != static_cast<Object::IKernelObject*>(&context))
    {
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextInstanceMismatch));
    }

    const Object::ObjectPinError threadPinError =
        m_objectRegistry.PinObject(threadId.GetObjectId(), Object::ObjectType::Thread, emptyEntry->ThreadPin);
    if (threadPinError != Object::ObjectPinError::Ok)
    {
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(
            MakeFailure(ReadyQueueError::ThreadPinRejected, Object::ObjectPinError::Ok, threadPinError));
    }
    if (&emptyEntry->ThreadPin.GetObject() != static_cast<Object::IKernelObject*>(&thread))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadInstanceMismatch));
    }

    const ThreadSnapshot threadSnapshot = thread.GetSnapshot();
    const SchedulingContextSnapshot contextSnapshot = context.GetSnapshot();
    const SchedulingContextConfiguration configuration = context.GetConfiguration();
    if (!ThreadIsReady(threadSnapshot))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadNotReady));
    }
    if (!IsRunnableContextState(contextSnapshot.State))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextNotRunnable));
    }
    if (!contextSnapshot.CurrentThread.IsEqualTo(threadId))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextThreadMismatch));
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << m_processor.GetValue());
    if (((configuration.CpuAffinityMask & processorBit) == 0U) ||
        ((configuration.Class == SchedulingClass::Isolated) && (configuration.CpuAffinityMask != processorBit)))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::CpuNotAllowed));
    }
    if (!context.TryEnterReadyQueue(*this))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextAlreadyQueued));
    }
    if (!thread.TryEnterReadyQueue(*this))
    {
        static_cast<void>(context.LeaveReadyQueue(*this));
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadAlreadyQueued));
    }

    const Foundation::UInt64 sequence = m_nextSequence;
    emptyEntry->Context = &context;
    emptyEntry->ThreadStorage = &thread;
    emptyEntry->ContextIdentity = contextId;
    emptyEntry->ThreadIdentity = threadId;
    emptyEntry->Configuration = configuration;
    ResetRankLocked(*emptyEntry);
    emptyEntry->Sequence = sequence;
    emptyEntry->ClaimSequence = 0U;
    emptyEntry->Reserved[0U] = 0U;
    emptyEntry->Reserved[1U] = 0U;
    emptyEntry->Reserved[2U] = 0U;
    emptyEntry->Reserved[3U] = 0U;
    emptyEntry->Reserved[4U] = 0U;
    emptyEntry->Reserved[5U] = 0U;
    emptyEntry->Reserved[6U] = 0U;
    emptyEntry->State = EntryState::Ready;
    ++m_readyEntries;
    ++m_nextSequence;
    return ResultType::Success(ReadyQueueEnqueueReceipt{
        .Sequence = sequence,
    });
}

Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>
ReadyQueue::ReserveBlockedEnqueue(const SchedulingContextId contextId, SchedulingContext& context,
                                  const ThreadId threadId, Thread& thread, const ThreadBlockToken blockToken) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueEnqueueReservation, ReadyQueueFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_processor.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidConfiguration));
    }
    if (!contextId.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidContext));
    }
    if (!contextId.IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextRegistryMismatch));
    }
    if (!threadId.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidThread));
    }
    if (!threadId.IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadRegistryMismatch));
    }
    if (!blockToken.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidBlockToken));
    }

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
        if (entry.ContextIdentity.IsEqualTo(contextId))
        {
            return ResultType::Failure(MakeFailure(ReadyQueueError::DuplicateContext));
        }
        if (entry.ThreadIdentity.IsEqualTo(threadId))
        {
            return ResultType::Failure(MakeFailure(ReadyQueueError::DuplicateThread));
        }
    }
    if (emptyEntry == nullptr)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::QueueFull));
    }
    if (m_nextSequence == MaximumUInt64)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::SequenceExhausted));
    }
    if (emptyEntry->ContextPin.IsValid() || emptyEntry->ThreadPin.IsValid() || (emptyEntry->Context != nullptr) ||
        (emptyEntry->ThreadStorage != nullptr))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::QueueCorrupt));
    }

    const Object::ObjectPinError contextPinError = m_objectRegistry.PinObject(
        contextId.GetObjectId(), Object::ObjectType::SchedulingContext, emptyEntry->ContextPin);
    if (contextPinError != Object::ObjectPinError::Ok)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextPinRejected, contextPinError));
    }
    if (&emptyEntry->ContextPin.GetObject() != static_cast<Object::IKernelObject*>(&context))
    {
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextInstanceMismatch));
    }

    const Object::ObjectPinError threadPinError =
        m_objectRegistry.PinObject(threadId.GetObjectId(), Object::ObjectType::Thread, emptyEntry->ThreadPin);
    if (threadPinError != Object::ObjectPinError::Ok)
    {
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(
            MakeFailure(ReadyQueueError::ThreadPinRejected, Object::ObjectPinError::Ok, threadPinError));
    }
    if (&emptyEntry->ThreadPin.GetObject() != static_cast<Object::IKernelObject*>(&thread))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadInstanceMismatch));
    }

    const ThreadSnapshot threadSnapshot = thread.GetSnapshot();
    const SchedulingContextSnapshot contextSnapshot = context.GetSnapshot();
    const SchedulingContextConfiguration configuration = context.GetConfiguration();
    if (threadSnapshot.State != ThreadState::Blocked)
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadNotBlocked));
    }
    if (!ThreadIsBlockedAt(threadSnapshot, blockToken))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidBlockToken));
    }
    if (!IsReservableContextState(contextSnapshot.State))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextNotRunnable));
    }
    // The caller need not be current yet: returning the donations above it
    // makes it current before CommitReservedEnqueue revalidates the pair.
    if (!context.ContainsThread(threadId))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextThreadMismatch));
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << m_processor.GetValue());
    if (((configuration.CpuAffinityMask & processorBit) == 0U) ||
        ((configuration.Class == SchedulingClass::Isolated) && (configuration.CpuAffinityMask != processorBit)))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::CpuNotAllowed));
    }
    if (!context.TryEnterReadyQueue(*this))
    {
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextAlreadyQueued));
    }
    if (!thread.TryReserveReadyQueue(*this, blockToken))
    {
        static_cast<void>(context.LeaveReadyQueue(*this));
        emptyEntry->ThreadPin.Reset();
        emptyEntry->ContextPin.Reset();
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadAlreadyQueued));
    }

    const Foundation::UInt64 sequence = m_nextSequence;
    emptyEntry->Context = &context;
    emptyEntry->ThreadStorage = &thread;
    emptyEntry->ContextIdentity = contextId;
    emptyEntry->ThreadIdentity = threadId;
    emptyEntry->Configuration = configuration;
    ResetRankLocked(*emptyEntry);
    emptyEntry->Sequence = sequence;
    emptyEntry->ClaimSequence = 0U;
    for (Foundation::Size index{0U}; index < sizeof(emptyEntry->Reserved); ++index)
    {
        emptyEntry->Reserved[index] = 0U;
    }
    emptyEntry->State = EntryState::Reserved;
    ++m_reservedEntries;
    ++m_nextSequence;
    return ResultType::Success(ReadyQueueEnqueueReservation{*this, sequence});
}

Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>
ReadyQueue::CommitReservedEnqueue(const ReadyQueueEnqueueReservation reservation) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueEnqueueReceipt, ReadyQueueFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!reservation.IsValid() || (reservation.m_queue != this))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidReservation));
    }
    Entry* const entry = FindReservationLocked(reservation);
    if (entry == nullptr)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::StaleReservation));
    }
    if ((m_reservedEntries == 0U) || !entry->ContextPin.IsValid() || !entry->ThreadPin.IsValid() ||
        (entry->Context == nullptr) || (entry->ThreadStorage == nullptr) || !entry->ContextIdentity.IsValid() ||
        !entry->ThreadIdentity.IsValid() || (entry->Sequence == 0U))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::QueueCorrupt));
    }
    if (!entry->Context->IsInReadyQueue(*this))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextMembershipMismatch));
    }
    if (!entry->ThreadStorage->IsInReadyQueue(*this))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadMembershipMismatch));
    }

    const ThreadSnapshot threadSnapshot = entry->ThreadStorage->GetSnapshot();
    const SchedulingContextSnapshot contextSnapshot = entry->Context->GetSnapshot();
    const SchedulingContextConfiguration configuration = entry->Context->GetConfiguration();
    if (!ThreadIsReady(threadSnapshot))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ThreadNotReady));
    }
    if (!IsRunnableContextState(contextSnapshot.State))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextNotRunnable));
    }
    if (!contextSnapshot.CurrentThread.IsEqualTo(entry->ThreadIdentity))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::ContextThreadMismatch));
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << m_processor.GetValue());
    if (((configuration.CpuAffinityMask & processorBit) == 0U) ||
        ((configuration.Class == SchedulingClass::Isolated) && (configuration.CpuAffinityMask != processorBit)))
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::CpuNotAllowed));
    }

    entry->Configuration = configuration;
    ResetRankLocked(*entry);
    entry->State = EntryState::Ready;
    --m_reservedEntries;
    ++m_readyEntries;
    return ResultType::Success(ReadyQueueEnqueueReceipt{
        .Sequence = entry->Sequence,
    });
}

ReadyQueueError ReadyQueue::CancelReservedEnqueue(const ReadyQueueEnqueueReservation reservation) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!reservation.IsValid() || (reservation.m_queue != this))
    {
        return ReadyQueueError::InvalidReservation;
    }
    Entry* const entry = FindReservationLocked(reservation);
    if (entry == nullptr)
    {
        return ReadyQueueError::StaleReservation;
    }
    if (m_reservedEntries == 0U)
    {
        return ReadyQueueError::QueueCorrupt;
    }

    const ReadyQueueError clearError = ClearEntryLocked(*entry);
    if (clearError != ReadyQueueError::None)
    {
        return clearError;
    }
    --m_reservedEntries;
    return ReadyQueueError::None;
}

Foundation::Result<ReadyQueueClaim, ReadyQueueFailure> ReadyQueue::ClaimNext(const Object::MonotonicTime now) noexcept
{
    using ResultType = Foundation::Result<ReadyQueueClaim, ReadyQueueFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_processor.IsValid())
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::InvalidConfiguration));
    }

    // Candidates are revalidated in rank order. A pair changed behind the
    // queue is skipped for this selection and stays Ready until Remove.
    Foundation::UInt32 skippedEntries{0U};
    Entry* selected{nullptr};
    for (Foundation::Size attempt{0U}; (selected == nullptr) && (attempt < MaximumReadyQueueEntries); ++attempt)
    {
        const Foundation::Size index = SelectBestLocked(now, skippedEntries);
        if (index == MaximumReadyQueueEntries)
        {
            return ResultType::Failure(MakeFailure(ReadyQueueError::Empty));
        }
        Entry* const candidate = &m_entries[index];
        if (m_nextClaimSequence == MaximumUInt64)
        {
            return ResultType::Failure(MakeFailure(ReadyQueueError::SequenceExhausted));
        }
        if ((m_readyEntries == 0U) || !candidate->ContextPin.IsValid() || !candidate->ThreadPin.IsValid() ||
            (candidate->Context == nullptr) || (candidate->ThreadStorage == nullptr) ||
            !candidate->ContextIdentity.IsValid() || !candidate->ThreadIdentity.IsValid() ||
            (candidate->Sequence == 0U) || !candidate->Context->IsInReadyQueue(*this) ||
            !candidate->ThreadStorage->IsInReadyQueue(*this))
        {
            return ResultType::Failure(MakeFailure(ReadyQueueError::QueueCorrupt));
        }
        if (IsRunnablePairLocked(*candidate))
        {
            selected = candidate;
        }
        else
        {
            skippedEntries |= Foundation::UInt32{1U} << index;
        }
    }
    if (selected == nullptr)
    {
        return ResultType::Failure(MakeFailure(ReadyQueueError::Empty));
    }

    const Foundation::UInt64 claimSequence = m_nextClaimSequence;
    ++m_nextClaimSequence;
    selected->ClaimSequence = claimSequence;
    selected->State = EntryState::Claimed;
    --m_readyEntries;
    ++m_claimedEntries;
    const ReadyQueueClaimToken token{*this, claimSequence};
    return ResultType::Success(ReadyQueueClaim{token, selected->ContextIdentity, selected->ThreadIdentity,
                                               *selected->Context, *selected->ThreadStorage, selected->Configuration,
                                               RankDeadlineLocked(*selected, now)});
}

Foundation::Size ReadyQueue::SelectBestLocked(const Object::MonotonicTime now,
                                              const Foundation::UInt32 excludedEntries) noexcept
{
    Foundation::Size selected{MaximumReadyQueueEntries};
    for (Foundation::Size index{0U}; index < MaximumReadyQueueEntries; ++index)
    {
        Entry& entry = m_entries[index];
        const bool excluded = ((excludedEntries >> index) & Foundation::UInt32{1U}) != 0U;
        if ((entry.State == EntryState::Ready) && !excluded &&
            ((selected == MaximumReadyQueueEntries) || IsBetterCandidateLocked(entry, m_entries[selected], now)))
        {
            selected = index;
        }
    }
    return selected;
}

bool ReadyQueue::IsRunnablePairLocked(const Entry& entry) noexcept
{
    const ThreadSnapshot threadSnapshot = entry.ThreadStorage->GetSnapshot();
    const SchedulingContextSnapshot contextSnapshot = entry.Context->GetSnapshot();
    return ThreadIsReady(threadSnapshot) && IsRunnableContextState(contextSnapshot.State) &&
           contextSnapshot.CurrentThread.IsEqualTo(entry.ThreadIdentity);
}

ReadyQueue::Entry* ReadyQueue::FindClaimLocked(const ReadyQueueClaimToken token) noexcept
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

ReadyQueue::Entry* ReadyQueue::FindReservationLocked(const ReadyQueueEnqueueReservation reservation) noexcept
{
    for (Entry& entry : m_entries)
    {
        if ((entry.State == EntryState::Reserved) && (entry.Sequence == reservation.GetSequence()))
        {
            return &entry;
        }
    }
    return nullptr;
}

ReadyQueueError ReadyQueue::CommitClaim(const ReadyQueueClaimToken token) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!token.IsValid() || (token.m_queue != this))
    {
        return ReadyQueueError::InvalidClaim;
    }
    Entry* const entry = FindClaimLocked(token);
    if (entry == nullptr)
    {
        return ReadyQueueError::StaleClaim;
    }
    if (m_claimedEntries == 0U)
    {
        return ReadyQueueError::QueueCorrupt;
    }

    const ReadyQueueError clearError = ClearEntryLocked(*entry);
    if (clearError != ReadyQueueError::None)
    {
        return clearError;
    }
    --m_claimedEntries;
    return ReadyQueueError::None;
}

ReadyQueueError ReadyQueue::CancelClaim(const ReadyQueueClaimToken token) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!token.IsValid() || (token.m_queue != this))
    {
        return ReadyQueueError::InvalidClaim;
    }
    Entry* const entry = FindClaimLocked(token);
    if (entry == nullptr)
    {
        return ReadyQueueError::StaleClaim;
    }
    if (m_claimedEntries == 0U)
    {
        return ReadyQueueError::QueueCorrupt;
    }
    if ((entry->Context == nullptr) || !entry->Context->IsInReadyQueue(*this))
    {
        return ReadyQueueError::ContextMembershipMismatch;
    }
    if ((entry->ThreadStorage == nullptr) || !entry->ThreadStorage->IsInReadyQueue(*this))
    {
        return ReadyQueueError::ThreadMembershipMismatch;
    }

    entry->ClaimSequence = 0U;
    ResetRankLocked(*entry);
    entry->State = EntryState::Ready;
    --m_claimedEntries;
    ++m_readyEntries;
    return ReadyQueueError::None;
}

ReadyQueueError ReadyQueue::RequeueClaim(const ReadyQueueClaimToken token) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!token.IsValid() || (token.m_queue != this))
    {
        return ReadyQueueError::InvalidClaim;
    }
    Entry* const entry = FindClaimLocked(token);
    if (entry == nullptr)
    {
        return ReadyQueueError::StaleClaim;
    }
    if ((m_claimedEntries == 0U) || !entry->ContextPin.IsValid() || !entry->ThreadPin.IsValid() ||
        (entry->Context == nullptr) || (entry->ThreadStorage == nullptr) || !entry->ContextIdentity.IsValid() ||
        !entry->ThreadIdentity.IsValid() || (entry->Sequence == 0U))
    {
        return ReadyQueueError::QueueCorrupt;
    }
    if (!entry->Context->IsInReadyQueue(*this))
    {
        return ReadyQueueError::ContextMembershipMismatch;
    }
    if (!entry->ThreadStorage->IsInReadyQueue(*this))
    {
        return ReadyQueueError::ThreadMembershipMismatch;
    }
    if (m_nextSequence == MaximumUInt64)
    {
        return ReadyQueueError::SequenceExhausted;
    }

    const ThreadSnapshot threadSnapshot = entry->ThreadStorage->GetSnapshot();
    const SchedulingContextSnapshot contextSnapshot = entry->Context->GetSnapshot();
    const SchedulingContextConfiguration configuration = entry->Context->GetConfiguration();
    if (!ThreadIsReady(threadSnapshot))
    {
        return ReadyQueueError::ThreadNotReady;
    }
    if (!IsRunnableContextState(contextSnapshot.State))
    {
        return ReadyQueueError::ContextNotRunnable;
    }
    if (!contextSnapshot.CurrentThread.IsEqualTo(entry->ThreadIdentity))
    {
        return ReadyQueueError::ContextThreadMismatch;
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << m_processor.GetValue());
    if (((configuration.CpuAffinityMask & processorBit) == 0U) ||
        ((configuration.Class == SchedulingClass::Isolated) && (configuration.CpuAffinityMask != processorBit)))
    {
        return ReadyQueueError::CpuNotAllowed;
    }

    entry->Configuration = configuration;
    ResetRankLocked(*entry);
    entry->Sequence = m_nextSequence;
    entry->ClaimSequence = 0U;
    entry->State = EntryState::Ready;
    ++m_nextSequence;
    --m_claimedEntries;
    ++m_readyEntries;
    return ReadyQueueError::None;
}

ReadyQueueError ReadyQueue::Remove(const ThreadId thread) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!thread.IsValid())
    {
        return ReadyQueueError::InvalidThread;
    }
    if (!thread.IsFromRegistry(m_objectRegistry))
    {
        return ReadyQueueError::ThreadRegistryMismatch;
    }

    for (Entry& entry : m_entries)
    {
        if ((entry.State == EntryState::Empty) || !entry.ThreadIdentity.IsEqualTo(thread))
        {
            continue;
        }
        if (entry.State == EntryState::Claimed)
        {
            return ReadyQueueError::EntryClaimed;
        }
        if (entry.State == EntryState::Reserved)
        {
            return ReadyQueueError::EntryReserved;
        }
        if (m_readyEntries == 0U)
        {
            return ReadyQueueError::QueueCorrupt;
        }

        const ReadyQueueError clearError = ClearEntryLocked(entry);
        if (clearError != ReadyQueueError::None)
        {
            return clearError;
        }
        --m_readyEntries;
        return ReadyQueueError::None;
    }
    return ReadyQueueError::NotFound;
}

ReadyQueueError ReadyQueue::ClearEntryLocked(Entry& entry) noexcept
{
    if ((entry.Context == nullptr) || !entry.Context->IsInReadyQueue(*this))
    {
        return ReadyQueueError::ContextMembershipMismatch;
    }
    if ((entry.ThreadStorage == nullptr) || !entry.ThreadStorage->IsInReadyQueue(*this))
    {
        return ReadyQueueError::ThreadMembershipMismatch;
    }
    if (!entry.ThreadStorage->LeaveReadyQueue(*this))
    {
        return ReadyQueueError::ThreadMembershipMismatch;
    }
    if (!entry.Context->LeaveReadyQueue(*this))
    {
        return ReadyQueueError::ContextMembershipMismatch;
    }
    entry.Context = nullptr;
    entry.ThreadStorage = nullptr;
    entry.ContextIdentity = SchedulingContextId{};
    entry.ThreadIdentity = ThreadId{};
    entry.Configuration = SchedulingContextConfiguration{};
    ResetRankLocked(entry);
    entry.Sequence = 0U;
    entry.ClaimSequence = 0U;
    entry.State = EntryState::Empty;
    entry.Reserved[0U] = 0U;
    entry.Reserved[1U] = 0U;
    entry.Reserved[2U] = 0U;
    entry.Reserved[3U] = 0U;
    entry.Reserved[4U] = 0U;
    entry.Reserved[5U] = 0U;
    entry.Reserved[6U] = 0U;
    entry.ThreadPin.Reset();
    entry.ContextPin.Reset();
    return ReadyQueueError::None;
}

ReadyQueueState ReadyQueue::GetState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ReadyQueueState{
        .NextSequence = m_nextSequence,
        .ReadyEntries = static_cast<Foundation::UInt8>(m_readyEntries),
        .ClaimedEntries = static_cast<Foundation::UInt8>(m_claimedEntries),
        .ReservedEntries = static_cast<Foundation::UInt8>(m_reservedEntries),
        .Processor = m_processor,
        .Reserved = {0U, 0U, 0U, 0U},
    };
}
} // namespace Asteria::Kernel::Scheduler
