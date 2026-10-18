// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Time/Timer.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Model-based test of TimerQueue and Timer with the real ObjectRegistry,
// RootCapabilityIssuer, and SpinLock. A reference model of Timer.md and
// TimerQueue.md applies the same seeded random operations, with valid, stale,
// foreign, wrong-type, and wrong-clock inputs, and every result, receipt,
// failure field, clock sample count, queue state, Timer snapshot, and registry
// population is compared. A mismatch reports its seed; set ASTERIA_TEST_SEED
// to replay it.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Time::IMonotonicClock;
using Kernel::Time::MaximumTimerQueueEntries;
using Kernel::Time::Timer;
using Kernel::Time::TimerArmReceipt;
using Kernel::Time::TimerError;
using Kernel::Time::TimerExpiration;
using Kernel::Time::TimerFailure;
using Kernel::Time::TimerId;
using Kernel::Time::TimerQueue;
using Kernel::Time::TimerQueueError;
using Kernel::Time::TimerQueueExpirationClaim;
using Kernel::Time::TimerQueueFailure;
using Kernel::Time::TimerQueueState;
using Kernel::Time::TimerSnapshot;
using Kernel::Time::TimerState;
using Kernel::Time::TimerToken;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;
using TestSupport::Random;

constexpr Size QueueCount{2U};
constexpr Size EntryCount{MaximumTimerQueueEntries};
// Local Timers outnumber the slots of one queue, so QueueFull is reachable.
constexpr Size LocalTimerCount{40U};
constexpr Size ForeignTimer{LocalTimerCount};
constexpr Size SkewedTimer{LocalTimerCount + 1U};
constexpr Size UnpublishedTimer{LocalTimerCount + 2U};
constexpr Size TimerCount{LocalTimerCount + 3U};
constexpr Size DecoyInstance{TimerCount};
constexpr Size StepsPerSeed{4000U};
// Initialization registers every instance once; each step registers at most one.
constexpr Size RecordCapacity{TimerCount + StepsPerSeed};
constexpr Size PhaseLength{250U};
constexpr Size PhaseCount{3U};
constexpr Size FillPhase{0U};
constexpr Size MixedPhase{2U};
constexpr Size TerminalStep{StepsPerSeed - (StepsPerSeed / 10U)};
constexpr Size ExhaustionStep{TerminalStep + 20U};
constexpr UInt64 InitialTicks{1000U};
constexpr UInt64 SkewedTicks{500U};
constexpr UInt64 InfiniteTicks{~UInt64{0U}};
constexpr UInt64 ClockCeiling{UInt64{1U} << 62U};
constexpr Size NoIndex{~Size{0U}};
// Identities that name no registry record.
constexpr Size InvalidIdentity{NoIndex - 1U};
constexpr Size NullIdentity{NoIndex - 2U};
constexpr Size BogusIdentity{NoIndex - 3U};
constexpr UInt64 BogusObject{(UInt64{0x7FFF'FFFFU} << 16U) | 7U};

template <typename TEnum> [[nodiscard]] constexpr UInt64 Code(const TEnum value) noexcept
{
    return static_cast<UInt64>(value);
}

template <Size Count> [[nodiscard]] UInt64 Nonzero(const UInt8 (&bytes)[Count]) noexcept
{
    UInt64 any{0U};
    for (const UInt8 byte : bytes)
    {
        any |= UInt64{byte};
    }
    return any;
}

// The first failed check, in contract order, names the error; None otherwise.
template <typename TError, Size Count>
[[nodiscard]] constexpr TError FirstError(const bool (&failed)[Count], const TError (&errors)[Count]) noexcept
{
    for (Size index{0U}; index < Count; ++index)
    {
        if (failed[index])
        {
            return errors[index];
        }
    }
    return TError{};
}

template <typename TValue, typename TError>
[[nodiscard]] bool Unwrap(const Foundation::Result<TValue, TError>& result, TValue& value, TError& failure) noexcept
{
    value = result.GetValue();
    failure = result.GetError();
    return result.HasValue();
}

// Registered non-Timer object: pinning it as a Timer reports TypeMismatch.
class DecoyObject final : public IKernelObject
{
public:
    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::Notification;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{9U};
    }
};

enum class EntryState : UInt8
{
    Empty,
    Scheduled,
    Pending,
    Claimed,
};

enum class RecordState : UInt8
{
    Alive,
    Quiescing,
    Collected,
};

enum class Transition : UInt8
{
    Arm,
    Cancel,
    Expire,
    Acknowledge,
};

enum class ClaimUse : UInt8
{
    Commit,
    Restore,
    Validate,
    Retire,
};

// Slots are modelled because equal deadlines use stable slot order and a new
// entry takes the lowest free slot.
struct EntryModel
{
    EntryState State{EntryState::Empty};
    Size Record{NoIndex};
    Size TimerIndex{NoIndex};
    UInt64 Sequence{0U};
    UInt64 Deadline{0U};
    UInt64 Observed{0U};
    UInt64 ClaimSequence{0U};
};

struct TimerModel
{
    TimerState State{TimerState::Uninitialized};
    UInt64 Sequence{0U};
    UInt64 Deadline{0U};
    Size Queue{NoIndex};
    Size Record{NoIndex};
    Size LastRecord{NoIndex};
};

struct RecordModel
{
    ObjectId Identity{};
    Size Instance{NoIndex};
    RecordState State{RecordState::Alive};
    UInt32 Pins{0U};
    bool Foreign{false};
};

struct TokenRecord
{
    TimerToken Token{};
    Size TimerIndex{NoIndex};
};

// A claim copy and the queue that issued it (NoIndex for the default claim).
struct ClaimRecord
{
    TimerQueueExpirationClaim Claim{};
    Size Queue{NoIndex};
};

// Predicted outcome of one call; Queue and TimerCode are None on success.
struct Outcome
{
    TimerQueueError Queue{TimerQueueError::None};
    TimerError TimerCode{TimerError::None};
    UInt64 Observed{0U};
    ObjectPinError Pin{ObjectPinError::Ok};
    UInt64 Next{0U};
    UInt64 Samples{0U};
};

// A rejected Timer transition surfaces from the queue as TimerRejected.
[[nodiscard]] constexpr Outcome Rejected(Outcome transition, const UInt64 next) noexcept
{
    const bool rejected = transition.TimerCode != TimerError::None;
    transition.Queue = rejected ? TimerQueueError::TimerRejected : TimerQueueError::None;
    transition.Next = next;
    return transition;
}

// Timers and queues need constructor arguments, and a queue is rebuilt in place.
struct System
{
    System() noexcept
    {
        for (Size index{0U}; index < TimerCount; ++index)
        {
            IMonotonicClock& clock = (index == SkewedTimer) ? SkewedClock : Clock;
            Timers[index] = new (TimerStorage[index]) Timer{clock};
        }
        for (Size index{0U}; index < QueueCount; ++index)
        {
            Queues[index] = new (QueueStorage[index]) TimerQueue{Registry, Clock};
        }
    }

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    ~System() noexcept
    {
        for (TimerQueue* const queue : Queues)
        {
            queue->~TimerQueue();
        }
        for (Timer* const timer : Timers)
        {
            timer->~Timer();
        }
    }

    SpinLock RegistryLock;
    Asteria::TestSupport::TestObjectRegistry Registry{RegistryLock};
    RootCapabilityIssuer Issuer{Registry};
    SpinLock ForeignLock;
    Asteria::TestSupport::TestObjectRegistry ForeignRegistry{ForeignLock};
    RootCapabilityIssuer ForeignIssuer{ForeignRegistry};
    ManualClock Clock{InitialTicks};
    ManualClock SkewedClock{SkewedTicks};
    DecoyObject Decoy;
    alignas(Timer) unsigned char TimerStorage[TimerCount][sizeof(Timer)]{};
    Timer* Timers[TimerCount]{};
    alignas(TimerQueue) unsigned char QueueStorage[QueueCount][sizeof(TimerQueue)]{};
    TimerQueue* Queues[QueueCount]{};
};

class Model final
{
public:
    UInt64 Now{InitialTicks};
    EntryModel Entries[QueueCount][EntryCount]{};
    UInt64 NextClaim[QueueCount]{1U, 1U};
    TimerModel Timers[TimerCount]{};
    RecordModel Records[RecordCapacity]{};
    Size RecordCount{0U};
    // Alive generations in the local and the foreign registry.
    Size Alive[2]{};

    template <typename TPredicate>
    [[nodiscard]] Size FindEntry(const Size queue, const TPredicate& predicate) const noexcept
    {
        for (Size slot{0U}; slot < EntryCount; ++slot)
        {
            if (predicate(Entries[queue][slot]))
            {
                return slot;
            }
        }
        return NoIndex;
    }

    [[nodiscard]] UInt64 Count(const Size queue, const EntryState state) const noexcept
    {
        UInt64 count{0U};
        for (const EntryModel& entry : Entries[queue])
        {
            count += (entry.State == state) ? 1U : 0U;
        }
        return count;
    }

    // Earliest entry in one state; equal deadlines keep stable slot order, so
    // the earliest Empty entry is the lowest free slot.
    [[nodiscard]] Size Earliest(const Size queue, const EntryState state) const noexcept
    {
        Size selected{NoIndex};
        for (Size slot{0U}; slot < EntryCount; ++slot)
        {
            const EntryModel& entry = Entries[queue][slot];
            if ((entry.State == state) &&
                ((selected == NoIndex) || (entry.Deadline < Entries[queue][selected].Deadline)))
            {
                selected = slot;
            }
        }
        return selected;
    }

    // Smallest Scheduled deadline, or zero; Pending and Claimed never count.
    [[nodiscard]] UInt64 NextDeadline(const Size queue) const noexcept
    {
        const Size earliest = Earliest(queue, EntryState::Scheduled);
        return (earliest == NoIndex) ? 0U : Entries[queue][earliest].Deadline;
    }

    // Restored (Pending) deliveries take priority over every Scheduled entry.
    [[nodiscard]] Size SelectDue(const Size queue) const noexcept
    {
        const Size pending = Earliest(queue, EntryState::Pending);
        return (pending != NoIndex) ? pending : Earliest(queue, EntryState::Scheduled);
    }

    [[nodiscard]] ObjectPinError PredictPin(const Size identity) const noexcept
    {
        if ((identity >= RecordCount) || (Records[identity].State != RecordState::Alive))
        {
            return ObjectPinError::ObjectNotFound;
        }
        return (Records[identity].Instance == DecoyInstance) ? ObjectPinError::TypeMismatch : ObjectPinError::Ok;
    }

    // Collection returns the unpinned quiescing generation in the lowest slot.
    [[nodiscard]] Size NextCollectable() const noexcept
    {
        Size selected{NoIndex};
        for (Size index{0U}; index < RecordCount; ++index)
        {
            const RecordModel& record = Records[index];
            if (!record.Foreign && (record.State == RecordState::Quiescing) && (record.Pins == 0U) &&
                ((selected == NoIndex) ||
                 ((record.Identity.GetValue() & 0xFFFFU) < (Records[selected].Identity.GetValue() & 0xFFFFU))))
            {
                selected = index;
            }
        }
        return selected;
    }

    // One Timer transition by its owning queue, or directly with owner NoIndex.
    // Arm takes a finite deadline after its one clock sample, tokens name one
    // Timer generation, expiry samples once, and Expired blocks re-arm and
    // cancellation until acknowledged.
    [[nodiscard]] Outcome PredictTimer(const Transition kind, const Size timer, const Size owner,
                                       const TokenRecord& token, const UInt64 deadline) const noexcept
    {
        const TimerModel& model = Timers[timer];
        const UInt64 now = (timer == SkewedTimer) ? SkewedTicks : Now;
        const bool armed = model.State == TimerState::Armed;
        const bool expired = model.State == TimerState::Expired;
        const bool foreignToken = token.TimerIndex != timer;
        const bool staleToken = token.Token.GetSequence() != model.Sequence;
        const TimerError common =
            FirstError({(model.Queue != NoIndex) && (model.Queue != owner), model.State == TimerState::Uninitialized},
                       {TimerError::ManagedByQueue, TimerError::NotInitialized});
        TimerError specific{TimerError::None};
        TimerError late{TimerError::None};
        switch (kind)
        {
        case Transition::Arm:
            specific =
                FirstError({deadline == InfiniteTicks, expired, model.Sequence == InfiniteTicks},
                           {TimerError::InvalidDeadline, TimerError::ExpirationPending, TimerError::SequenceExhausted});
            late = FirstError({now == InfiniteTicks, deadline <= now},
                              {TimerError::ClockExhausted, TimerError::DeadlineReached});
            break;
        case Transition::Cancel:
            specific = FirstError({foreignToken, staleToken, expired, !armed},
                                  {TimerError::InvalidToken, TimerError::StaleToken, TimerError::ExpirationPending,
                                   TimerError::NotArmed});
            break;
        case Transition::Expire:
            specific = FirstError(
                {foreignToken, staleToken, expired, !armed},
                {TimerError::InvalidToken, TimerError::StaleToken, TimerError::AlreadyExpired, TimerError::NotArmed});
            late = FirstError({now == InfiniteTicks, now < model.Deadline},
                              {TimerError::ClockExhausted, TimerError::NotExpired});
            break;
        case Transition::Acknowledge:
            specific = FirstError(
                {foreignToken, staleToken, armed, !expired},
                {TimerError::InvalidToken, TimerError::StaleToken, TimerError::NotExpired, TimerError::NotArmed});
            break;
        }
        const TimerError early = (common != TimerError::None) ? common : specific;
        const bool sampled = (early == TimerError::None) && ((kind == Transition::Arm) || (kind == Transition::Expire));
        return sampled ? Outcome{.TimerCode = late, .Observed = now, .Samples = 1U} : Outcome{.TimerCode = early};
    }

    void ArmTimer(const Size timer, const UInt64 deadline, const Size queue) noexcept
    {
        TimerModel& model = Timers[timer];
        ++model.Sequence;
        model.Deadline = deadline;
        model.State = TimerState::Armed;
        model.Queue = queue;
    }

    void DisarmTimer(const Size timer) noexcept
    {
        Timers[timer].Deadline = 0U;
        Timers[timer].State = TimerState::Disarmed;
        Timers[timer].Queue = NoIndex;
    }

    void ClearEntry(const Size queue, const Size slot) noexcept
    {
        --Records[Entries[queue][slot].Record].Pins;
        Entries[queue][slot] = EntryModel{};
    }
};

class Driver final
{
public:
    Driver(System& system, Model& model, const UInt64 seed) noexcept
        : m_system{system}, m_model{model}, m_random{seed}, m_seed{seed}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        m_operation = "initialize";
        m_terminal = m_random.Chance(1U, 2U);
        bool matched = Check("invalid charge owner", Code(TimerError::InvalidConfiguration),
                             Code(m_system.Timers[0U]->Initialize(ResourceDomainId{})));
        for (Size timer{0U}; matched && (timer < UnpublishedTimer); ++timer)
        {
            Timer& real = *m_system.Timers[timer];
            const ResourceDomainId owner{100U + timer};
            matched = Check("initialize", Code(TimerError::None), Code(real.Initialize(owner))) &&
                      Check("initialize again", Code(TimerError::AlreadyInitialized), Code(real.Initialize(owner))) &&
                      Register(timer, timer == ForeignTimer);
            m_model.Timers[timer].State = TimerState::Disarmed;
        }
        m_foreignRecord = m_model.Timers[ForeignTimer].Record;
        m_decoyRecord = m_model.RecordCount;
        return matched && Register(DecoyInstance, false) && CheckAll();
    }

    [[nodiscard]] bool Step(const Size step) noexcept
    {
        using Operation = bool (Driver::*)();
        static constexpr const char* Names[]{"arm",    "cancel",   "claim",        "claim copy",
                                             "direct", "registry", "advance clock"};
        static constexpr Operation Operations[]{&Driver::OpArm,       &Driver::OpCancel, &Driver::OpClaim,
                                                &Driver::OpClaimCopy, &Driver::OpDirect, &Driver::OpRegistry,
                                                &Driver::OpAdvance};
        // Fill loads one queue and drains the other, Drain resolves claims, Mixed spreads.
        static constexpr UInt32 Weights[PhaseCount][7U]{
            {380U, 50U, 60U, 60U, 110U, 62U, 40U},
            {100U, 65U, 240U, 165U, 115U, 69U, 150U},
            {200U, 60U, 150U, 120U, 110U, 68U, 90U},
        };
        m_step = step;
        if ((step % PhaseLength) == 0U)
        {
            m_phase = static_cast<Size>(m_random.Below(PhaseCount));
            m_preferredQueue = static_cast<Size>(m_random.Below(QueueCount));
        }
        if (m_terminal && ((step == TerminalStep) || (step == ExhaustionStep)))
        {
            // The last finite tick makes every deadline due; exhaustion is terminal.
            m_operation = "terminal clock";
            return SetClock((step == TerminalStep) ? InfiniteTicks - 1U : InfiniteTicks);
        }
        const Size operation = Weighted(Weights[m_phase]);
        m_operation = Names[operation];
        return (this->*Operations[operation])() && CheckAll();
    }

    // Every membership, generation, and pin must be releasable at the end.
    [[nodiscard]] bool Teardown() noexcept
    {
        m_step = StepsPerSeed;
        m_operation = "teardown";
        bool matched = RebuildQueue(0U) && RebuildQueue(1U);
        for (Size timer{0U}; matched && (timer < TimerCount); ++timer)
        {
            const TimerState state = m_model.Timers[timer].State;
            const bool live = (state == TimerState::Armed) || (state == TimerState::Expired);
            const Transition settle = (state == TimerState::Armed) ? Transition::Cancel : Transition::Acknowledge;
            matched = (!live || Direct(settle, timer, m_latest[timer], 0U)) &&
                      Require("settled", m_model.Timers[timer].State == (live ? TimerState::Disarmed : state), timer) &&
                      Require("initialized",
                              m_system.Timers[timer]->IsInitialized() == (state != TimerState::Uninitialized), timer) &&
                      Check("charge owner", (state == TimerState::Uninitialized) ? 0U : 100U + timer,
                            m_system.Timers[timer]->GetChargeOwner().GetValue(), timer);
        }
        for (Size index{0U}; matched && (index < m_model.RecordCount); ++index)
        {
            RecordModel& record = m_model.Records[index];
            const bool destroy = (record.State == RecordState::Alive) && !record.Foreign;
            matched = !destroy || Check("destroy", 0U, Code(m_system.Issuer.DestroyObject(record.Identity)), index);
            record.State = destroy ? RecordState::Quiescing : record.State;
            m_model.Alive[0U] -= destroy ? 1U : 0U;
        }
        while (matched && (m_model.NextCollectable() != NoIndex))
        {
            matched = Collect();
        }
        return matched && Collect() && CheckAll();
    }

private:
    [[nodiscard]] bool Check(const char* const detail, const UInt64 expected, const UInt64 actual,
                             const Size index = NoIndex) const noexcept
    {
        if (expected == actual)
        {
            return true;
        }
        static_cast<void>(
            std::fprintf(stderr, "MODEL MISMATCH seed=%llu step=%zu operation=%s: %s[%lld] expected=%llu actual=%llu\n",
                         static_cast<unsigned long long>(m_seed), m_step, m_operation, detail,
                         (index == NoIndex) ? -1LL : static_cast<long long>(index),
                         static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual)));
        return false;
    }

    [[nodiscard]] bool Require(const char* const detail, const bool holds, const Size index = NoIndex) const noexcept
    {
        return Check(detail, 1U, holds ? 1U : 0U, index);
    }

    [[nodiscard]] UInt64 Samples() const noexcept
    {
        return m_system.Clock.GetReadCount() + m_system.SkewedClock.GetReadCount();
    }

    [[nodiscard]] bool SameTimer(const Outcome& expected, const bool succeeded, const TimerFailure& failure,
                                 const UInt64 before) const noexcept
    {
        return Check("clock samples", expected.Samples, Samples() - before) &&
               Check("timer result", Code(expected.TimerCode), succeeded ? 0U : Code(failure.Error)) &&
               (succeeded || (Check("failure observed ticks", expected.Observed, failure.ObservedTicks) &&
                              Check("timer failure reserved", 0U, Nonzero(failure.Reserved))));
    }

    [[nodiscard]] bool SameQueue(const Outcome& expected, const bool succeeded, const TimerQueueFailure& failure,
                                 const UInt64 before) const noexcept
    {
        return Check("queue result", Code(expected.Queue), succeeded ? 0U : Code(failure.Error)) &&
               (succeeded ? Check("clock samples", expected.Samples, Samples() - before)
                          : (Check("failure next deadline", expected.Next, failure.NextDeadlineTicks) &&
                             Check("failure pin", Code(expected.Pin), Code(failure.Pin)) &&
                             Check("queue failure reserved", 0U, Nonzero(failure.Reserved)) &&
                             SameTimer(expected, false, failure.Timer, before)));
    }

    // A snapshot names the current generation only while Armed or Expired.
    [[nodiscard]] bool SameSnapshot(const Size timer, const TimerSnapshot& snapshot) const noexcept
    {
        const TimerModel& model = m_model.Timers[timer];
        const bool generation = (model.State == TimerState::Armed) || (model.State == TimerState::Expired);
        return Check("timer state", Code(model.State), Code(snapshot.State), timer) &&
               Check("timer token", generation ? model.Sequence : 0U,
                     snapshot.Token.IsValid() ? snapshot.Token.GetSequence() : 0U, timer) &&
               Check("timer deadline", generation ? model.Deadline : 0U, snapshot.DeadlineTicks, timer) &&
               Check("timer snapshot reserved", 0U, Nonzero(snapshot.Reserved), timer);
    }

    [[nodiscard]] bool SameState(const Size queue, const TimerQueueState& state) const noexcept
    {
        return Check("queue next deadline", m_model.NextDeadline(queue), state.NextDeadlineTicks, queue) &&
               Check("queue next claim sequence", m_model.NextClaim[queue], state.NextClaimSequence, queue) &&
               Check("queue scheduled", m_model.Count(queue, EntryState::Scheduled), state.ScheduledEntries, queue) &&
               Check("queue pending", m_model.Count(queue, EntryState::Pending), state.PendingEntries, queue) &&
               Check("queue claimed", m_model.Count(queue, EntryState::Claimed), state.ClaimedEntries, queue) &&
               Check("queue state reserved", 0U, Nonzero(state.Reserved), queue);
    }

    [[nodiscard]] bool SameArmReceipt(const Size timer, const TimerArmReceipt& receipt, const UInt64 deadline,
                                      const UInt64 observed, const bool replaced) const noexcept
    {
        return Check("receipt token", m_model.Timers[timer].Sequence,
                     receipt.Token.IsValid() ? receipt.Token.GetSequence() : 0U) &&
               Require("receipt token binding", receipt.Token.IsEqualTo(m_system.Timers[timer]->GetSnapshot().Token)) &&
               Check("receipt deadline", deadline, receipt.DeadlineTicks) &&
               Check("receipt observed ticks", observed, receipt.ObservedTicks) &&
               Check("receipt replaced", replaced ? 1U : 0U, receipt.Replaced ? 1U : 0U) &&
               Check("receipt reserved", 0U, Nonzero(receipt.Reserved));
    }

    // A claim names the exact identity, storage, generation, and expiry.
    [[nodiscard]] bool SameClaim(const Size queue, const Size slot,
                                 const TimerQueueExpirationClaim& claim) const noexcept
    {
        const EntryModel& entry = m_model.Entries[queue][slot];
        const TimerExpiration expiration = claim.GetExpiration();
        return Check("claim sequence", entry.ClaimSequence,
                     claim.GetClaimToken().IsValid() ? claim.GetClaimToken().GetSequence() : 0U) &&
               Require("claimed identity", claim.GetTimerId().IsEqualTo(MakeTimerId(entry.Record))) &&
               Require("claimed storage", &claim.GetTimer() == m_system.Timers[entry.TimerIndex]) &&
               Check("expiration token", entry.Sequence, expiration.Token.GetSequence()) &&
               Require("expiration binding",
                       expiration.Token.IsEqualTo(m_system.Timers[entry.TimerIndex]->GetSnapshot().Token)) &&
               Check("expiration deadline", entry.Deadline, expiration.DeadlineTicks) &&
               Check("expiration observed ticks", entry.Observed, expiration.ObservedTicks) &&
               Check("claim next deadline", m_model.NextDeadline(queue), claim.GetNextDeadlineTicks());
    }

    [[nodiscard]] bool CheckAll() const noexcept
    {
        bool same = Check("local registry population", m_model.Alive[0U], m_system.Registry.GetActiveCount()) &&
                    Check("foreign registry population", m_model.Alive[1U], m_system.ForeignRegistry.GetActiveCount());
        for (Size queue{0U}; same && (queue < QueueCount); ++queue)
        {
            same = SameState(queue, m_system.Queues[queue]->GetState());
        }
        for (Size timer{0U}; same && (timer < TimerCount); ++timer)
        {
            same = SameSnapshot(timer, m_system.Timers[timer]->GetSnapshot());
        }
        return same;
    }

    [[nodiscard]] bool SetClock(const UInt64 ticks) noexcept
    {
        m_system.Clock.Set(ticks);
        m_model.Now = ticks;
        return CheckAll();
    }

    template <Size Count> [[nodiscard]] Size Weighted(const UInt32 (&weights)[Count]) noexcept
    {
        UInt64 total{0U};
        for (const UInt32 weight : weights)
        {
            total += weight;
        }
        UInt64 choice = m_random.Below(total);
        Size index{0U};
        while (choice >= weights[index])
        {
            choice -= weights[index];
            ++index;
        }
        return index;
    }

    // A random slot in the given state, or any occupied slot for Empty.
    [[nodiscard]] Size RandomSlot(const Size queue, const EntryState state) noexcept
    {
        const Size start = static_cast<Size>(m_random.Below(EntryCount));
        for (Size offset{0U}; offset < EntryCount; ++offset)
        {
            const EntryState candidate = m_model.Entries[queue][(start + offset) % EntryCount].State;
            if ((state == EntryState::Empty) ? (candidate != EntryState::Empty) : (candidate == state))
            {
                return (start + offset) % EntryCount;
            }
        }
        return NoIndex;
    }

    // Fill admits to the preferred queue and drains the other; Drain drains it.
    [[nodiscard]] Size PickQueue(const bool admission) noexcept
    {
        const bool other = !admission && (m_phase == FillPhase);
        const Size focus = other ? (m_preferredQueue + 1U) % QueueCount : m_preferredQueue;
        const bool focused = (m_phase != MixedPhase) && m_random.Chance(admission ? 9U : 4U, admission ? 10U : 5U);
        return focused ? focus : static_cast<Size>(m_random.Below(QueueCount));
    }

    // Reserved, zero, reached, far, first admissible, tied, grid-aligned (more
    // ties), and near deadlines; near ones wrap only at the end of time.
    [[nodiscard]] UInt64 PickDeadline() noexcept
    {
        static constexpr UInt32 Weights[]{5U, 1U, 7U, 4U, 8U, 15U, 24U, 36U};
        const UInt64 now = m_model.Now;
        const UInt64 back = m_random.Below(40U);
        const UInt64 near = now + 1U + m_random.Below(150U);
        const UInt64 tied = m_model.NextDeadline(static_cast<Size>(m_random.Below(QueueCount)));
        const UInt64 choices[]{InfiniteTicks,
                               0U,
                               (back > now) ? 0U : now - back,
                               InfiniteTicks - 1U - (back % 4U),
                               now + 1U,
                               (tied != 0U) ? tied : near,
                               (near + 15U) & ~UInt64{15U},
                               near};
        return choices[Weighted(Weights)];
    }

    [[nodiscard]] Size CurrentIdentity(const Size timer) const noexcept
    {
        const TimerModel& model = m_model.Timers[timer];
        const Size record = (model.Record != NoIndex) ? model.Record : model.LastRecord;
        return (record != NoIndex) ? record : InvalidIdentity;
    }

    // Any recorded generation (other, stale, quiescing, decoy, foreign), invalid
    // identities, and an unregistered object identifier.
    [[nodiscard]] Size PickIdentity() noexcept
    {
        static constexpr UInt32 Weights[]{7U, 6U, 2U, 1U, 1U, 1U, 2U};
        const Size choices[]{static_cast<Size>(m_random.Below(m_model.RecordCount)),
                             CurrentIdentity(static_cast<Size>(m_random.Below(TimerCount))),
                             m_decoyRecord,
                             m_foreignRecord,
                             InvalidIdentity,
                             NullIdentity,
                             BogusIdentity};
        return choices[Weighted(Weights)];
    }

    [[nodiscard]] Size PickTimerFor(const Size identity) noexcept
    {
        const bool named = (identity < m_model.RecordCount) && (m_model.Records[identity].Instance < TimerCount);
        return (named && m_random.Chance(1U, 2U)) ? m_model.Records[identity].Instance
                                                  : static_cast<Size>(m_random.Below(TimerCount));
    }

    // A TimerId must be valid and come from the queue's registry.
    [[nodiscard]] TimerQueueError IdentityError(const Size identity) const noexcept
    {
        const bool recorded = identity < m_model.RecordCount;
        return FirstError({!recorded && (identity != BogusIdentity), recorded && m_model.Records[identity].Foreign},
                          {TimerQueueError::InvalidTimer, TimerQueueError::TimerRegistryMismatch});
    }

    [[nodiscard]] TimerId MakeTimerId(const Size identity) const noexcept
    {
        if (identity < m_model.RecordCount)
        {
            const RecordModel& record = m_model.Records[identity];
            return TimerId{record.Foreign ? m_system.ForeignRegistry : m_system.Registry, record.Identity};
        }
        const ObjectId object{(identity == BogusIdentity) ? BogusObject : 0U};
        return (identity == InvalidIdentity) ? TimerId{} : TimerId{m_system.Registry, object};
    }

    void RememberToken(const TimerToken token, const Size timer) noexcept
    {
        m_previous[timer] = m_latest[timer];
        m_latest[timer] = TokenRecord{token, timer};
    }

    // Usually the current token; otherwise the previous generation of the same
    // Timer, the current token of any Timer, or the default token.
    [[nodiscard]] TokenRecord PickToken(const Size timer) noexcept
    {
        static constexpr UInt32 Weights[]{14U, 3U, 2U, 1U};
        const TokenRecord choices[]{m_latest[timer], m_previous[timer],
                                    m_latest[static_cast<Size>(m_random.Below(TimerCount))], TokenRecord{}};
        return choices[Weighted(Weights)];
    }

    // The live claim of a random Claimed entry, the latest or previous claim
    // copy of any slot (usually resolved, so stale), or the default claim.
    [[nodiscard]] ClaimRecord PickClaim() noexcept
    {
        static constexpr UInt32 Weights[]{12U, 4U, 3U, 1U};
        const Size queue = static_cast<Size>(m_random.Below(QueueCount));
        const Size any = static_cast<Size>(m_random.Below(EntryCount));
        const Size claimed = RandomSlot(queue, EntryState::Claimed);
        const TimerQueueExpirationClaim choices[]{m_live[queue][(claimed != NoIndex) ? claimed : any],
                                                  m_live[queue][any], m_stale[queue][any], TimerQueueExpirationClaim{}};
        const TimerQueueExpirationClaim& claim = choices[Weighted(Weights)];
        return ClaimRecord{claim, claim.GetClaimToken().IsValid() ? queue : NoIndex};
    }

    // CreateObject requires an unregistered instance: a Timer is registered
    // again only after its previous generation was collected.
    [[nodiscard]] bool Register(const Size instance, const bool foreign) noexcept
    {
        IKernelObject& object = (instance == DecoyInstance) ? static_cast<IKernelObject&>(m_system.Decoy)
                                                            : static_cast<IKernelObject&>(*m_system.Timers[instance]);
        const auto created =
            foreign ? m_system.ForeignIssuer.CreateObject(object) : m_system.Issuer.CreateObject(object);
        const Size index = m_model.RecordCount++;
        m_model.Records[index] = RecordModel{created.GetValue(), instance, RecordState::Alive, 0U, foreign};
        ++m_model.Alive[foreign ? 1U : 0U];
        if (instance < TimerCount)
        {
            m_model.Timers[instance].Record = index;
            m_model.Timers[instance].LastRecord = index;
        }
        return Check("registration", 0U, created.HasValue() ? 0U : Code(created.GetError()));
    }

    [[nodiscard]] bool Collect() noexcept
    {
        const Size expected = m_model.NextCollectable();
        const auto collected = m_system.Issuer.CollectDestroyedObject();
        if (expected == NoIndex)
        {
            return Check("collection", Code(ObjectRegistryError::NoDestructionReady),
                         collected.HasValue() ? 0U : Code(collected.GetError()));
        }
        RecordModel& record = m_model.Records[expected];
        record.State = RecordState::Collected;
        const IKernelObject* instance = &m_system.Decoy;
        if (record.Instance < TimerCount)
        {
            m_model.Timers[record.Instance].Record = NoIndex;
            instance = m_system.Timers[record.Instance];
        }
        return Require("collection", collected.HasValue()) &&
               Check("collected generation", record.Identity.GetValue(), collected.GetValue().Object.GetValue()) &&
               Require("collected instance", collected.GetValue().Instance == instance);
    }

    // Identity, registry, clock, duplicate storage, instance, capacity, and pin
    // checks precede the Timer transition; a failure changes nothing. A re-arm
    // pins the generation again, so a destroyed Timer cannot be re-armed.
    [[nodiscard]] bool OpArm() noexcept
    {
        const Size queue = PickQueue(true);
        Size timer = static_cast<Size>(m_random.Below(LocalTimerCount));
        Size identity = CurrentIdentity(timer);
        if (m_random.Chance(1U, 6U))
        {
            identity = PickIdentity();
            timer = m_random.Chance(1U, 8U) ? SkewedTimer : PickTimerFor(identity);
        }
        const UInt64 deadline = PickDeadline();
        const auto sameIdentity = [identity](const EntryModel& entry) noexcept
        {
            return entry.Record == identity;
        };
        const auto sameStorage = [identity, timer](const EntryModel& entry) noexcept
        {
            return (entry.TimerIndex == timer) && (entry.Record != identity);
        };
        const Size existing = m_model.FindEntry(queue, sameIdentity);
        const Size slot = (existing != NoIndex) ? existing : m_model.Earliest(queue, EntryState::Empty);
        const ObjectPinError pin = m_model.PredictPin(identity);
        const bool pinnedOther =
            (pin == ObjectPinError::Ok) && (existing == NoIndex) && (m_model.Records[identity].Instance != timer);
        Outcome expected{.Queue = FirstError(
                             {IdentityError(identity) != TimerQueueError::None, timer == SkewedTimer,
                              m_model.FindEntry(queue, sameStorage) != NoIndex,
                              (existing != NoIndex) && (m_model.Entries[queue][existing].TimerIndex != timer),
                              slot == NoIndex, pin != ObjectPinError::Ok, pinnedOther},
                             {IdentityError(identity), TimerQueueError::ClockMismatch, TimerQueueError::DuplicateTimer,
                              TimerQueueError::TimerInstanceMismatch, TimerQueueError::QueueFull,
                              TimerQueueError::TimerPinRejected, TimerQueueError::TimerInstanceMismatch}),
                         .Next = m_model.NextDeadline(queue)};
        expected.Pin = (expected.Queue == TimerQueueError::TimerPinRejected) ? pin : ObjectPinError::Ok;
        if (expected.Queue == TimerQueueError::None)
        {
            expected =
                Rejected(m_model.PredictTimer(Transition::Arm, timer, queue, TokenRecord{}, deadline), expected.Next);
        }

        const bool replaced = m_model.Timers[timer].State == TimerState::Armed;
        const UInt64 before = Samples();
        const auto armed =
            m_system.Queues[queue]->Arm(MakeTimerId(identity), *m_system.Timers[timer], MonotonicTime{deadline});
        const bool same = SameQueue(expected, armed.HasValue(), armed.GetError(), before);
        if (!same || !armed.HasValue())
        {
            return same;
        }
        m_model.ArmTimer(timer, deadline, queue);
        EntryModel& entry = m_model.Entries[queue][slot];
        if (existing == NoIndex)
        {
            entry = EntryModel{EntryState::Scheduled, identity, timer, 0U, 0U, 0U, 0U};
            ++m_model.Records[identity].Pins;
        }
        entry.Sequence = m_model.Timers[timer].Sequence;
        entry.Deadline = deadline;
        RememberToken(armed.GetValue().Timer.Token, timer);
        return SameArmReceipt(timer, armed.GetValue().Timer, deadline, expected.Observed, replaced) &&
               Check("receipt next deadline", m_model.NextDeadline(queue), armed.GetValue().NextDeadlineTicks);
    }

    // TimerQueue::Cancel removes one exact Scheduled generation; a Claimed
    // entry is reported, and the Timer rejects a Pending expiration.
    [[nodiscard]] bool OpCancel() noexcept
    {
        const Size queue = PickQueue(false);
        const Size occupied = RandomSlot(queue, EntryState::Empty);
        Size identity = PickIdentity();
        Size timer = PickTimerFor(identity);
        if ((occupied != NoIndex) && !m_random.Chance(1U, 4U))
        {
            identity = m_model.Entries[queue][occupied].Record;
            timer = m_model.Entries[queue][occupied].TimerIndex;
        }
        const TokenRecord token = PickToken(timer);
        const auto sameIdentity = [identity](const EntryModel& entry) noexcept
        {
            return entry.Record == identity;
        };
        const Size slot = m_model.FindEntry(queue, sameIdentity);
        const EntryModel entry = (slot == NoIndex) ? EntryModel{} : m_model.Entries[queue][slot];
        Outcome expected{.Queue = FirstError({IdentityError(identity) != TimerQueueError::None, slot == NoIndex,
                                              entry.TimerIndex != timer, entry.State == EntryState::Claimed},
                                             {IdentityError(identity), TimerQueueError::TimerNotFound,
                                              TimerQueueError::TimerInstanceMismatch, TimerQueueError::EntryClaimed}),
                         .Next = m_model.NextDeadline(queue)};
        if (expected.Queue == TimerQueueError::None)
        {
            expected = Rejected(m_model.PredictTimer(Transition::Cancel, timer, queue, token, 0U), expected.Next);
        }

        const UInt64 before = Samples();
        const auto cancelled =
            m_system.Queues[queue]->Cancel(MakeTimerId(identity), *m_system.Timers[timer], token.Token);
        const bool same = SameQueue(expected, cancelled.HasValue(), cancelled.GetError(), before);
        if (!same || !cancelled.HasValue())
        {
            return same;
        }
        m_model.DisarmTimer(timer);
        m_model.ClearEntry(queue, slot);
        return SameSnapshot(timer, cancelled.GetValue().Timer) &&
               Check("receipt next deadline", m_model.NextDeadline(queue), cancelled.GetValue().NextDeadlineTicks);
    }

    // Commit, restore (CancelClaim), validate, or retire one claim copy; only the exact
    // unresolved claim sequence of this queue is accepted.
    [[nodiscard]] bool UseClaim(const Size queue, const ClaimRecord& candidate, const ClaimUse use) noexcept
    {
        const UInt64 sequence = candidate.Claim.GetClaimToken().GetSequence();
        const auto live = [sequence](const EntryModel& entry) noexcept
        {
            return (entry.State == EntryState::Claimed) && (entry.ClaimSequence == sequence);
        };
        const Size slot = (candidate.Queue == queue) ? m_model.FindEntry(queue, live) : NoIndex;
        const Outcome expected{.Queue = FirstError({candidate.Queue != queue, slot == NoIndex},
                                                   {TimerQueueError::InvalidClaim, TimerQueueError::StaleClaim}),
                               .Next = m_model.NextDeadline(queue)};

        TimerQueue& real = *m_system.Queues[queue];
        TimerQueueState state{};
        TimerQueueExpirationClaim canonical{};
        TimerQueueFailure failure{};
        const UInt64 before = Samples();
        bool succeeded{false};
        switch (use)
        {
        case ClaimUse::Commit:
            succeeded = Unwrap(real.CommitClaim(candidate.Claim.GetClaimToken()), state, failure);
            break;
        case ClaimUse::Restore:
            succeeded = Unwrap(real.CancelClaim(candidate.Claim.GetClaimToken()), state, failure);
            break;
        case ClaimUse::Validate:
            succeeded = Unwrap(real.ValidateClaimForDelivery(candidate.Claim), canonical, failure);
            break;
        case ClaimUse::Retire:
            succeeded = Unwrap(real.RetireClaim(candidate.Claim.GetClaimToken()), state, failure);
            break;
        }
        const bool same = SameQueue(expected, succeeded, failure, before);
        if (!same || !succeeded)
        {
            return same;
        }
        if (use == ClaimUse::Validate)
        {
            return Require("canonical claim", canonical.GetClaimToken().IsEqualTo(candidate.Claim.GetClaimToken())) &&
                   SameClaim(queue, slot, canonical);
        }
        EntryModel& entry = m_model.Entries[queue][slot];
        if (use == ClaimUse::Commit)
        {
            // Commit drops membership and the pin; the Timer stays Expired.
            m_model.Timers[entry.TimerIndex].Queue = NoIndex;
            m_model.ClearEntry(queue, slot);
        }
        else if (use == ClaimUse::Retire)
        {
            // Retirement consumes the expiration: Disarmed, unmanaged, unpinned.
            m_model.DisarmTimer(entry.TimerIndex);
            m_model.ClearEntry(queue, slot);
        }
        else
        {
            // The restored expiration is Pending and this sequence is dead.
            entry.State = EntryState::Pending;
            entry.ClaimSequence = 0U;
        }
        return SameState(queue, state);
    }

    [[nodiscard]] bool OpClaimCopy() noexcept
    {
        const ClaimRecord candidate = PickClaim();
        const bool elsewhere = (candidate.Queue == NoIndex) || m_random.Chance(1U, 6U);
        const Size queue = elsewhere ? static_cast<Size>(m_random.Below(QueueCount)) : candidate.Queue;
        return UseClaim(queue, candidate, static_cast<ClaimUse>(m_random.Below(4U)));
    }

    // Pending entries are claimed without a clock sample, otherwise only the
    // earliest Scheduled Timer is sampled once. Three in five claims are then
    // delivered in TimerNotificationDelivery order: an independent Timer pin,
    // then commit and acknowledgement, restoration to Pending on a retryable
    // rejection, or retirement when the pin proves the generation destroyed.
    [[nodiscard]] bool OpClaim() noexcept
    {
        const Size queue = PickQueue(false);
        const Size selected = m_model.SelectDue(queue);
        const UInt64 now = m_model.Now;
        const bool scheduled =
            (selected != NoIndex) && (m_model.Entries[queue][selected].State == EntryState::Scheduled);
        const bool exhausted = scheduled && (now == InfiniteTicks);
        const bool early = scheduled && (now < m_model.Entries[queue][selected].Deadline);
        const Outcome expected{
            .Queue = FirstError({selected == NoIndex, exhausted, early},
                                {TimerQueueError::Empty, TimerQueueError::TimerRejected, TimerQueueError::NotDue}),
            .TimerCode = FirstError({exhausted, early}, {TimerError::ClockExhausted, TimerError::NotExpired}),
            .Observed = (exhausted || early) ? now : 0U,
            .Next = m_model.NextDeadline(queue),
            .Samples = scheduled ? 1U : 0U};

        TimerQueueExpirationClaim claim{};
        TimerQueueFailure failure{};
        failure.Reserved[0U] = 0xA5U; // A reused output must come back with clear reserved bytes.
        const UInt64 before = Samples();
        const bool succeeded = m_system.Queues[queue]->ClaimDue(claim, failure);
        const bool same = SameQueue(expected, succeeded, failure, before);
        if (!same || !succeeded)
        {
            return same;
        }
        EntryModel& entry = m_model.Entries[queue][selected];
        m_model.Timers[entry.TimerIndex].State = TimerState::Expired;
        entry.Observed = scheduled ? now : entry.Observed;
        entry.State = EntryState::Claimed;
        entry.ClaimSequence = m_model.NextClaim[queue]++;
        m_stale[queue][selected] = m_live[queue][selected];
        m_live[queue][selected] = claim;
        const bool claimed = SameClaim(queue, selected, claim);
        if (!claimed || m_random.Chance(2U, 5U))
        {
            return claimed;
        }
        const EntryModel delivered = entry;
        ObjectPin delivery{};
        const ObjectPinError pin =
            m_system.Registry.PinObject(claim.GetTimerId().GetObjectId(), ObjectType::Timer, delivery);
        const bool pinned = pin == ObjectPinError::Ok;
        const bool published = pinned && !m_random.Chance(1U, 5U);
        m_model.Records[delivered.Record].Pins += pinned ? 1U : 0U;
        const ClaimUse resolution = published                                 ? ClaimUse::Commit
                                    : (pin == ObjectPinError::ObjectNotFound) ? ClaimUse::Retire
                                                                              : ClaimUse::Restore;
        const bool matched = Check("delivery pin", Code(m_model.PredictPin(delivered.Record)), Code(pin)) &&
                             UseClaim(queue, ClaimRecord{claim, queue}, resolution) &&
                             (!published || Direct(Transition::Acknowledge, delivered.TimerIndex,
                                                   TokenRecord{claim.GetExpiration().Token, delivered.TimerIndex}, 0U));
        delivery.Reset();
        m_model.Records[delivered.Record].Pins -= pinned ? 1U : 0U;
        return matched;
    }

    [[nodiscard]] bool Direct(const Transition kind, const Size timer, const TokenRecord& token,
                              const UInt64 deadline) noexcept
    {
        const Outcome expected = m_model.PredictTimer(kind, timer, NoIndex, token, deadline);
        const TimerModel previous = m_model.Timers[timer];
        Timer& real = *m_system.Timers[timer];
        TimerArmReceipt receipt{};
        TimerSnapshot snapshot{};
        TimerExpiration expiration{};
        TimerFailure failure{};
        const UInt64 before = Samples();
        bool succeeded{false};
        switch (kind)
        {
        case Transition::Arm:
            succeeded = Unwrap(real.ArmAt(MonotonicTime{deadline}), receipt, failure);
            break;
        case Transition::Cancel:
            succeeded = Unwrap(real.Cancel(token.Token), snapshot, failure);
            break;
        case Transition::Expire:
            succeeded = Unwrap(real.TryExpire(token.Token), expiration, failure);
            break;
        case Transition::Acknowledge:
            succeeded = Unwrap(real.Acknowledge(token.Token), snapshot, failure);
            break;
        }
        const bool same = SameTimer(expected, succeeded, failure, before);
        if (!same || !succeeded)
        {
            return same;
        }
        if (kind == Transition::Arm)
        {
            m_model.ArmTimer(timer, deadline, NoIndex);
            RememberToken(receipt.Token, timer);
            return SameArmReceipt(timer, receipt, deadline, expected.Observed, previous.State == TimerState::Armed);
        }
        if (kind == Transition::Expire)
        {
            m_model.Timers[timer].State = TimerState::Expired;
            return Require("expiration token", expiration.Token.IsEqualTo(token.Token)) &&
                   Check("expiration deadline", previous.Deadline, expiration.DeadlineTicks) &&
                   Check("expiration observed ticks", expected.Observed, expiration.ObservedTicks);
        }
        m_model.DisarmTimer(timer);
        return SameSnapshot(timer, snapshot);
    }

    // Half of the steps acknowledge an expiration released by commit or teardown.
    [[nodiscard]] bool OpDirect() noexcept
    {
        const Size start = static_cast<Size>(m_random.Below(TimerCount));
        const bool acknowledge = m_random.Chance(1U, 2U);
        for (Size offset{0U}; acknowledge && (offset < TimerCount); ++offset)
        {
            const Size timer = (start + offset) % TimerCount;
            if ((m_model.Timers[timer].Queue == NoIndex) && (m_model.Timers[timer].State == TimerState::Expired))
            {
                return Direct(Transition::Acknowledge, timer, m_latest[timer], 0U);
            }
        }
        const Transition kind = static_cast<Transition>(m_random.Below(4U));
        // Call arguments are evaluated in an unspecified order, so both draws are
        // named: the deadline first, which keeps the traces recorded with GCC.
        const UInt64 deadline = PickDeadline();
        const TokenRecord token = PickToken(start);
        return Direct(kind, start, token, deadline);
    }

    // Destruction refuses new pins while a queue pin keeps the generation
    // uncollectable; collected Timers are registered again.
    [[nodiscard]] bool OpRegistry() noexcept
    {
        const UInt64 action = m_random.Below(64U);
        const Size start = static_cast<Size>(m_random.Below(UnpublishedTimer));
        const Size index = m_random.Chance(4U, 5U) ? m_model.Timers[start].LastRecord
                                                   : static_cast<Size>(m_random.Below(m_model.RecordCount));
        RecordModel& record = m_model.Records[index];
        if (action < 2U)
        {
            return RebuildQueue(action);
        }
        if (action < 23U)
        {
            return Collect();
        }
        for (Size offset{0U}; (action < 44U) && (offset < UnpublishedTimer); ++offset)
        {
            const Size timer = (start + offset) % UnpublishedTimer;
            if ((timer != ForeignTimer) && (m_model.Timers[timer].Record == NoIndex))
            {
                return Register(timer, false);
            }
        }
        if ((action < 44U) || record.Foreign || (record.Instance == DecoyInstance))
        {
            return true;
        }
        const bool alive = record.State == RecordState::Alive;
        const ObjectRegistryError expected = alive ? ObjectRegistryError::Ok : ObjectRegistryError::InvalidObject;
        record.State = alive ? RecordState::Quiescing : record.State;
        m_model.Alive[0U] -= alive ? 1U : 0U;
        return Check("destroy result", Code(expected), Code(m_system.Issuer.DestroyObject(record.Identity)));
    }

    // Advances are nondecreasing and often land exactly on, or one tick
    // before, the earliest queued deadline.
    [[nodiscard]] bool OpAdvance() noexcept
    {
        const UInt64 now = m_model.Now;
        const UInt64 mode = m_random.Below(10U);
        const UInt64 deadline = m_model.NextDeadline(static_cast<Size>(m_random.Below(QueueCount)));
        UInt64 target = now + ((mode < 8U) ? m_random.Below(30U) : 100U + m_random.Below(700U));
        if ((mode >= 5U) && (mode < 8U) && (deadline > now))
        {
            target = (mode == 5U) ? deadline - 1U : deadline;
        }
        m_model.Now = ((now >= ClockCeiling) || (target >= ClockCeiling)) ? now : target;
        m_system.Clock.Set(m_model.Now);
        return true;
    }

    // Destruction cancels Scheduled entries and releases Pending or Claimed ones,
    // still Expired, with every pin; the successor reuses address and sequences.
    [[nodiscard]] bool RebuildQueue(const Size queue) noexcept
    {
        const UInt64 before = Samples();
        m_system.Queues[queue]->~TimerQueue();
        m_system.Queues[queue] = new (m_system.QueueStorage[queue]) TimerQueue{m_system.Registry, m_system.Clock};
        for (Size slot{0U}; slot < EntryCount; ++slot)
        {
            const EntryModel entry = m_model.Entries[queue][slot];
            if (entry.State == EntryState::Scheduled)
            {
                m_model.DisarmTimer(entry.TimerIndex);
            }
            if (entry.State != EntryState::Empty)
            {
                m_model.Timers[entry.TimerIndex].Queue = NoIndex;
                m_model.ClearEntry(queue, slot);
            }
            m_live[queue][slot] = m_stale[queue][slot] = TimerQueueExpirationClaim{};
        }
        m_model.NextClaim[queue] = 1U;
        return Check("clock samples", 0U, Samples() - before) && CheckAll();
    }

    System& m_system;
    Model& m_model;
    Random m_random;
    UInt64 m_seed;
    Size m_step{0U};
    const char* m_operation{""};
    Size m_phase{MixedPhase};
    Size m_preferredQueue{0U};
    bool m_terminal{false};
    Size m_decoyRecord{NoIndex};
    Size m_foreignRecord{NoIndex};
    TokenRecord m_latest[TimerCount]{};
    TokenRecord m_previous[TimerCount]{};
    TimerQueueExpirationClaim m_live[QueueCount][EntryCount]{};
    TimerQueueExpirationClaim m_stale[QueueCount][EntryCount]{};
};

// The fixtures are tens of kilobytes; they live in static storage and are
// placement-constructed per seed because tests may not allocate.
alignas(System) unsigned char g_systemStorage[sizeof(System)];
alignas(Model) unsigned char g_modelStorage[sizeof(Model)];
alignas(Driver) unsigned char g_driverStorage[sizeof(Driver)];

// An expiration whose delivery can never succeed (its Timer generation was
// destroyed, so the delivery pin is refused) is retired: its Timer is
// Disarmed, the overdue Scheduled entry behind it is claimed next, and the
// released queue pin lets the destroyed generation be collected.
void CheckUndeliverableExpirationRetires(TestContext& runner) noexcept
{
    System* const system = new (g_systemStorage) System{};
    Timer& destroyed = *system->Timers[0U];
    Timer& behind = *system->Timers[1U];
    TimerQueue& queue = *system->Queues[0U];
    static_cast<void>(destroyed.Initialize(ResourceDomainId{1U}));
    static_cast<void>(behind.Initialize(ResourceDomainId{1U}));
    const ObjectId destroyedObject = system->Issuer.CreateObject(destroyed).GetValue();
    const bool armed =
        queue.Arm(TimerId{system->Registry, destroyedObject}, destroyed, MonotonicTime{InitialTicks + 10U})
            .HasValue() &&
        queue
            .Arm(TimerId{system->Registry, system->Issuer.CreateObject(behind).GetValue()}, behind,
                 MonotonicTime{InitialTicks + 20U})
            .HasValue() &&
        (system->Issuer.DestroyObject(destroyedObject) == ObjectRegistryError::Ok);
    system->Clock.Set(InitialTicks + 30U);
    const auto claim = queue.ClaimDue();
    ObjectPin delivery{};
    const bool undeliverable =
        claim.HasValue() && (&claim.GetValue().GetTimer() == &destroyed) &&
        (system->Registry.PinObject(destroyedObject, ObjectType::Timer, delivery) == ObjectPinError::ObjectNotFound);
    const auto retired = queue.RetireClaim(claim.GetValue().GetClaimToken());
    const auto next = queue.ClaimDue();
    const auto collected = system->Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner,
                   "an undeliverable expiration is retired, so its queue drains and its generation is collected", armed,
                   undeliverable, retired.HasValue(),
                   (retired.GetValue().PendingEntries == 0U) && (retired.GetValue().ClaimedEntries == 0U),
                   destroyed.GetSnapshot().State == TimerState::Disarmed,
                   next.HasValue() && (&next.GetValue().GetTimer() == &behind),
                   collected.HasValue() && (collected.GetValue().Instance == &destroyed));
    static_cast<void>(queue.CommitClaim(next.GetValue().GetClaimToken()));
    static_cast<void>(behind.Acknowledge(next.GetValue().GetExpiration().Token));
    system->~System();
}

void RunSeed(TestContext& runner, const UInt64 seed) noexcept
{
    System* const system = new (g_systemStorage) System{};
    Model* const model = new (g_modelStorage) Model{};
    Driver* const driver = new (g_driverStorage) Driver{*system, *model, seed};
    bool matched = driver->Initialize();
    for (Size step{0U}; matched && (step < StepsPerSeed); ++step)
    {
        matched = driver->Step(step);
    }
    matched = matched && driver->Teardown();
    if (!matched)
    {
        static_cast<void>(
            std::fprintf(stderr, "  reproduce with ASTERIA_TEST_SEED=%llu\n", static_cast<unsigned long long>(seed)));
    }
    ASTERIA_EXPECT(runner, "timer queue matches its reference model for one seed", matched);
    driver->~Driver();
    model->~Model();
    system->~System();
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TimerQueueModelTests"};
    Asteria::Test::CheckUndeliverableExpirationRetires(runner);
    const Asteria::TestSupport::SeedRange seeds = Asteria::TestSupport::GetSeedRange(1U, 64U);
    for (Asteria::Foundation::UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::Test::RunSeed(runner, seed);
    }
    return runner.Finish();
}
