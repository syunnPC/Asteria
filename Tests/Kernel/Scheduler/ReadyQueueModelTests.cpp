// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/ReadyQueue.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include <cstdio>
#include <cstring>
#include <new>

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Model-based test of per-CPU ready placement: ReadyQueue with the SchedulingContext
// budget/donation state machine and the Thread lifecycle, over the real ObjectRegistry,
// RootCapabilityIssuer, and SpinLock. Each seeded step applies one random operation (valid, stale,
// foreign, mistyped, or mismatched operands; rarely an in-place queue rebuild) to the real objects
// and a reference model, then compares every result, receipt, claim view, queue counter, and
// touched snapshot (all every 16 steps). Bursts and drains move queue 0 between empty and full; a
// quarter of the seeds end near the UInt64 time horizon; rare policy violations exercise the
// rejection of lifecycle changes to queued objects and ClaimNext revalidation. A mismatch prints
// seed, step, and operation; ASTERIA_TEST_SEED replays it. The model follows the implementation
// where the architecture text is broader. The example at the end pins EDF by the current window;
// the characterization documents the bounded periodic budget's back-to-back burst.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::MaximumProcessorCount;
using Kernel::Scheduler::ReadyQueue;
using Kernel::Scheduler::ReadyQueueClaimToken;
using Kernel::Scheduler::ReadyQueueEnqueueReservation;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingContextState;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadBlockToken;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadState;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;
using TestSupport::Random;
using Configuration = Kernel::Scheduler::SchedulingContextConfiguration;
using ContextError = Kernel::Scheduler::SchedulingContextError;
using PinError = Kernel::Object::ObjectPinError;
using QueueError = Kernel::Scheduler::ReadyQueueError;

// Queues 0 and 1 serve the lowest and highest CPU of the four-CPU profile;
// queue 2 names CPU 4, which the profile rejects.
constexpr Size QueueCount{3U};
constexpr Size ValidQueueCount{2U};
constexpr UInt8 QueueProcessors[QueueCount]{0U, 3U, 4U};
// The last context and thread are unregistered scratch objects, rebuilt in
// place to exercise initialization.
constexpr Size ContextCount{61U};
constexpr Size ThreadCount{65U};
constexpr Size ScratchContext{ContextCount - 1U};
constexpr Size ScratchThread{ThreadCount - 1U};
// Unbound server threads; donations prefer them, so their pin counts reach the
// registry's per-object reference limit.
constexpr Size ServerThreadFirst{ScratchContext};
constexpr Size EntryCount{Kernel::Scheduler::MaximumReadyQueueEntries};
constexpr Size ChainCapacity{static_cast<Size>(Kernel::Scheduler::MaximumSchedulingContextDonationDepth) + 1U};
constexpr Size RegistrySlotCount{Asteria::TestSupport::TestObjectRegistry::EntryCapacity};
constexpr UInt32 ReferenceLimit{5U};
constexpr Size StepsPerSeed{2500U};
constexpr Size PhasePeriod{500U};
constexpr Size BurstLength{150U};
constexpr Size SweepInterval{16U};
constexpr Size RetiredCapacity{16U};
constexpr Size TouchCapacity{8U};
constexpr UInt64 InitialTicks{1000U};
constexpr UInt64 MaximumTicks{~UInt64{0U}};
constexpr UInt64 ObjectSlotMask{0xFFFFU};
constexpr UInt64 GenerationShift{16U};
constexpr Size NoIndex{~Size{0U}};
constexpr UInt8 NoProcessor{0xFFU};
constexpr UInt8 ReadyDisposition{static_cast<UInt8>(ThreadRunDisposition::Ready)};
constexpr UInt8 BlockedDisposition{static_cast<UInt8>(ThreadRunDisposition::Blocked)};
constexpr UInt8 ExitedDisposition{static_cast<UInt8>(ThreadRunDisposition::Exited)};

enum class ObjectKind : UInt8
{
    Thread,
    Context,
};

enum class Origin : UInt8
{
    None,
    Main,
    Foreign,
};

enum class EntryPhase : UInt8
{
    Empty,
    Reserved,
    Ready,
    Claimed,
};

enum class ClaimResolution : UInt8
{
    Commit,
    Cancel,
    Requeue,
};

enum class Phase : UInt8
{
    Mixed,
    Burst,
    Drain,
};

struct Identity
{
    Origin Registry;
    UInt64 Value;
};

// A held ThreadBlockToken with its issuing thread storage and block generation
// for the model; Value zero is the default, invalid token.
struct HeldBlockToken
{
    ThreadBlockToken Token;
    Size Thread;
    UInt64 Value;
};

struct RegistrySlot
{
    bool Occupied;
    UInt32 References;
    ObjectKind Kind;
    Size Owner;
};

struct ThreadRecord
{
    bool Initialized;
    ThreadState State{ThreadState::Uninitialized};
    UInt64 RunSequence;
    UInt64 BlockSequence;
    UInt64 ExitCode;
    UInt64 ChargeOwner;
    UInt8 Processor{NoProcessor};
    Size Queue{NoIndex};
    UInt64 ObjectValue;
};

struct ContextRecord
{
    bool Initialized;
    Configuration Settings;
    UInt64 ChargeOwner;
    UInt64 Chain[ChainCapacity];
    Size ChainLength;
    UInt64 Remaining;
    UInt64 NextReplenishment;
    UInt64 Deadline;
    UInt64 LastObserved;
    UInt64 RunStart;
    bool Running;
    UInt8 Processor{NoProcessor};
    Size Queue{NoIndex};
    UInt64 ObjectValue;
};

// A queued pair; its configuration is its context's immutable one.
struct EntryRecord
{
    EntryPhase Phase;
    Size Context;
    Size Thread;
    UInt64 ContextValue;
    UInt64 ThreadValue;
    UInt64 Sequence;
    UInt64 ClaimSequence;
};

struct QueueRecord
{
    UInt8 Processor;
    EntryRecord Entries[EntryCount]{};
    UInt64 NextSequence{1U};
    UInt64 NextClaimSequence{1U};
};

// Predicted queue result: the affected entry and the reported sequence.
struct QueueOutcome
{
    QueueError Error;
    PinError ContextPin;
    PinError ThreadPin;
    Size Slot;
    Size Context;
    Size Thread;
    UInt64 Sequence;
};

struct RunOutcome
{
    ContextError Error;
    UInt64 MaximumRun;
    UInt64 Thread;
    UInt64 Elapsed;
    UInt64 Charged;
    UInt64 Remaining;
    bool DeadlineMissed;
};

// The first failed precondition of an ordered list; every condition is
// evaluated, so each must be safe after an earlier one failed. Then applies a
// state change only when all held.
template <typename TError, TError Success = TError::None> class FirstFailure final
{
public:
    [[nodiscard]] FirstFailure& Require(const bool holds, const TError error) noexcept
    {
        m_error = ((m_error == Success) && !holds) ? error : m_error;
        return *this;
    }

    template <typename TChange> [[nodiscard]] TError Then(const TChange& change) const noexcept
    {
        if (m_error == Success)
        {
            change();
        }
        return m_error;
    }

    [[nodiscard]] TError Get() const noexcept
    {
        return m_error;
    }

private:
    TError m_error{Success};
};

[[nodiscard]] constexpr bool IsValidIdentity(const Identity identity) noexcept
{
    return (identity.Registry != Origin::None) && (identity.Value != 0U);
}

[[nodiscard]] constexpr bool IsValidProcessor(const UInt8 processor) noexcept
{
    return processor < MaximumProcessorCount;
}

[[nodiscard]] constexpr UInt8 ProcessorBit(const UInt8 processor) noexcept
{
    return IsValidProcessor(processor) ? static_cast<UInt8>(1U << processor) : UInt8{0U};
}

[[nodiscard]] constexpr UInt64 Smaller(const UInt64 first, const UInt64 second) noexcept
{
    return (first < second) ? first : second;
}

// Sums must be representable and differ from MonotonicTime::Infinite.
[[nodiscard]] constexpr bool TryAddTicks(const UInt64 left, const UInt64 right, UInt64& sum) noexcept
{
    const bool fits = left <= (MaximumTicks - right);
    sum = fits ? (left + right) : sum;
    return fits && (sum != MaximumTicks);
}

// Moves a passed boundary to the fixed-phase window containing now (host
// division checks the fixed-step remainder); false if unrepresentable.
[[nodiscard]] bool RollWindow(const Configuration& settings, const UInt64 now, UInt64& start, UInt64& boundary,
                              UInt64& deadline) noexcept
{
    start = now - ((now - boundary) % settings.PeriodTicks);
    return TryAddTicks(start, settings.PeriodTicks, boundary) &&
           TryAddTicks(start, settings.RelativeDeadlineTicks, deadline);
}

[[nodiscard]] constexpr bool IsRunnable(const SchedulingContextState state) noexcept
{
    return (state == SchedulingContextState::Bound) || (state == SchedulingContextState::Donated) ||
           (state == SchedulingContextState::BudgetExhausted);
}

[[nodiscard]] constexpr UInt8 ClassRank(const SchedulingClass value) noexcept
{
    constexpr UInt8 Ranks[]{5U, 2U, 3U, 4U, 1U, 0U};
    const UInt8 raw = static_cast<UInt8>(value);
    return (raw < 6U) ? Ranks[raw] : UInt8{6U};
}

// Documented order: class, criticality, priority, earlier effective deadline
// (at the selection sample) within DeadlineSporadic, FIFO.
[[nodiscard]] bool Outranks(const EntryRecord& candidate, const Configuration& left, const UInt64 leftDeadline,
                            const EntryRecord& current, const Configuration& right, const UInt64 rightDeadline) noexcept
{
    if (ClassRank(left.Class) != ClassRank(right.Class))
    {
        return ClassRank(left.Class) < ClassRank(right.Class);
    }
    if (left.Criticality != right.Criticality)
    {
        return static_cast<UInt8>(left.Criticality) > static_cast<UInt8>(right.Criticality);
    }
    if (left.Priority != right.Priority)
    {
        return left.Priority > right.Priority;
    }
    const bool edf = (left.Class == SchedulingClass::DeadlineSporadic) && (leftDeadline != rightDeadline);
    return edf ? (leftDeadline < rightDeadline) : (candidate.Sequence < current.Sequence);
}

[[nodiscard]] bool PlacementAllowed(const Configuration& settings, const UInt8 processor) noexcept
{
    const UInt8 bit = ProcessorBit(processor);
    return ((settings.CpuAffinityMask & bit) != 0U) &&
           ((settings.Class != SchedulingClass::Isolated) || (settings.CpuAffinityMask == bit));
}

[[nodiscard]] bool ConfigurationValid(const Configuration& settings) noexcept
{
    const UInt8 schedulingClass = static_cast<UInt8>(settings.Class);
    const UInt8 criticality = static_cast<UInt8>(settings.Criticality);
    return (settings.BudgetTicks != 0U) && (settings.BudgetTicks <= settings.RelativeDeadlineTicks) &&
           (settings.RelativeDeadlineTicks <= settings.PeriodTicks) && (settings.Weight != 0U) &&
           (settings.CpuAffinityMask != 0U) && ((settings.CpuAffinityMask & 0xF0U) == 0U) && (schedulingClass >= 1U) &&
           (schedulingClass <= 5U) && (criticality >= 1U) && (criticality <= 4U) && (settings.Reserved == 0U);
}

[[nodiscard]] constexpr ContextError MapPinError(const PinError error) noexcept
{
    constexpr ContextError Mapped[]{
        ContextError::None,           ContextError::InvalidThread,      ContextError::ThreadPinUnavailable,
        ContextError::ThreadNotFound, ContextError::ThreadTypeMismatch, ContextError::ThreadPinUnavailable};
    const UInt8 raw = static_cast<UInt8>(error);
    return (raw < 6U) ? Mapped[raw] : ContextError::ThreadPinUnavailable;
}

[[nodiscard]] QueueOutcome Outcome(const QueueError error, const PinError contextPin = PinError::Ok,
                                   const PinError threadPin = PinError::Ok) noexcept
{
    return QueueOutcome{error, contextPin, threadPin, NoIndex, NoIndex, NoIndex, 0U};
}

// Raw storage lets scratch objects be rebuilt in place and orders destruction.
struct System
{
    System() noexcept
    {
        for (Size index{0U}; index < ThreadCount; ++index)
        {
            Threads[index] = new (ThreadStorage[index]) Thread{};
        }
        for (Size index{0U}; index < ContextCount; ++index)
        {
            Contexts[index] = new (ContextStorage[index]) SchedulingContext{Registry};
        }
        for (Size index{0U}; index < QueueCount; ++index)
        {
            Queues[index] = new (QueueStorage[index]) ReadyQueue{Registry, CpuId{QueueProcessors[index]}};
        }
    }

    ~System() noexcept
    {
        for (ReadyQueue* const queue : Queues)
        {
            queue->~ReadyQueue();
        }
        for (SchedulingContext* const context : Contexts)
        {
            context->~SchedulingContext();
        }
        for (Thread* const thread : Threads)
        {
            thread->~Thread();
        }
    }

    SpinLock Lock;
    Asteria::TestSupport::TestObjectRegistry Registry{Lock, ReferenceLimit};
    SpinLock ForeignLock;
    Asteria::TestSupport::TestObjectRegistry ForeignRegistry{ForeignLock};
    RootCapabilityIssuer Issuer{Registry};
    ManualClock Clock{InitialTicks};
    alignas(Thread) unsigned char ThreadStorage[ThreadCount][sizeof(Thread)]{};
    alignas(SchedulingContext) unsigned char ContextStorage[ContextCount][sizeof(SchedulingContext)]{};
    alignas(ReadyQueue) unsigned char QueueStorage[QueueCount][sizeof(ReadyQueue)]{};
    Thread* Threads[ThreadCount]{};
    SchedulingContext* Contexts[ContextCount]{};
    ReadyQueue* Queues[QueueCount]{};
};

// Each model operation checks preconditions in the documented (else the
// implemented) order and changes state only on success.
class Model final
{
public:
    UInt64 Now{InitialTicks};
    Size Registered{0U};
    RegistrySlot Slots[RegistrySlotCount]{};
    ThreadRecord Threads[ThreadCount]{};
    ContextRecord Contexts[ContextCount]{};
    QueueRecord Queues[QueueCount]{{QueueProcessors[0U]}, {QueueProcessors[1U]}, {QueueProcessors[2U]}};

    // Values carry slot + 1 in the low 16 bits and the generation above them.
    [[nodiscard]] UInt64 Register(const ObjectKind kind, const Size owner) noexcept
    {
        Slots[Registered % RegistrySlotCount] = RegistrySlot{true, 0U, kind, owner};
        return static_cast<UInt64>(++Registered) | (UInt64{1U} << GenerationShift);
    }

    [[nodiscard]] Size SlotOf(const UInt64 value) const noexcept
    {
        const UInt64 slot = value & ObjectSlotMask;
        const Size index = ((slot != 0U) && (slot <= RegistrySlotCount)) ? static_cast<Size>(slot - 1U) : NoIndex;
        const bool named = (index != NoIndex) && Slots[index].Occupied && ((value >> GenerationShift) == 1U);
        return named ? index : NoIndex;
    }

    [[nodiscard]] PinError Pin(const UInt64 value, const ObjectKind kind) noexcept
    {
        const Size index = SlotOf(value);
        RegistrySlot& slot = Slots[(index == NoIndex) ? 0U : index];
        return FirstFailure<PinError, PinError::Ok>{}
            .Require(index != NoIndex, PinError::ObjectNotFound)
            .Require(slot.Kind == kind, PinError::TypeMismatch)
            .Require(slot.References < ReferenceLimit, PinError::ReferenceExhausted)
            .Then(
                [&slot]() noexcept
                {
                    ++slot.References;
                });
    }

    void Unpin(const UInt64 value) noexcept
    {
        const Size index = SlotOf(value);
        Slots[(index == NoIndex) ? 0U : index].References -= (index == NoIndex) ? 0U : 1U;
    }

    [[nodiscard]] Size OwnerOf(const UInt64 value, const ObjectKind kind) const noexcept
    {
        const Size index = SlotOf(value);
        return ((index != NoIndex) && (Slots[index].Kind == kind)) ? Slots[index].Owner : NoIndex;
    }

    [[nodiscard]] ThreadError InitializeThread(const Size index, const UInt64 owner) noexcept
    {
        ThreadRecord& thread = Threads[index];
        return FirstFailure<ThreadError>{}
            .Require(!thread.Initialized, ThreadError::AlreadyInitialized)
            .Require(owner != 0U, ThreadError::InvalidConfiguration)
            .Then(
                [&thread, owner]() noexcept
                {
                    thread.Initialized = true;
                    thread.State = ThreadState::Dormant;
                    thread.ChargeOwner = owner;
                });
    }

    [[nodiscard]] ThreadError MakeReady(const Size index) noexcept
    {
        ThreadRecord& thread = Threads[index];
        return FirstFailure<ThreadError>{}
            .Require(thread.Initialized, ThreadError::NotInitialized)
            .Require(thread.State != ThreadState::Exited, ThreadError::AlreadyExited)
            .Require(thread.State != ThreadState::Running, ThreadError::AlreadyRunning)
            .Require(thread.State == ThreadState::Dormant, ThreadError::InvalidState)
            .Then(
                [&thread]() noexcept
                {
                    thread.State = ThreadState::Ready;
                });
    }

    [[nodiscard]] ThreadError BeginRun(const Size index, const UInt8 processor) noexcept
    {
        ThreadRecord& thread = Threads[index];
        return FirstFailure<ThreadError>{}
            .Require(thread.Initialized, ThreadError::NotInitialized)
            .Require(IsValidProcessor(processor), ThreadError::InvalidCpu)
            .Require(thread.State != ThreadState::Exited, ThreadError::AlreadyExited)
            .Require(thread.State != ThreadState::Running, ThreadError::AlreadyRunning)
            .Require(thread.State == ThreadState::Ready, ThreadError::InvalidState)
            .Then(
                [&thread, processor]() noexcept
                {
                    ++thread.RunSequence;
                    thread.Processor = processor;
                    thread.State = ThreadState::Running;
                });
    }

    [[nodiscard]] ThreadError FinishRun(const Size index, const UInt8 processor, const UInt8 disposition,
                                        const UInt64 exitCode) noexcept
    {
        constexpr ThreadState Completed[]{ThreadState::Ready, ThreadState::Ready, ThreadState::Blocked,
                                          ThreadState::Exited};
        ThreadRecord& thread = Threads[index];
        const bool exits = disposition == ExitedDisposition;
        return FirstFailure<ThreadError>{}
            .Require(thread.Initialized, ThreadError::NotInitialized)
            .Require(IsValidProcessor(processor), ThreadError::InvalidCpu)
            .Require((disposition >= 1U) && (disposition <= 3U) && (exits || (exitCode == 0U)),
                     ThreadError::InvalidDisposition)
            .Require(thread.State == ThreadState::Running, ThreadError::NotRunning)
            .Require(thread.Processor == processor, ThreadError::WrongCpu)
            .Then(
                [&]() noexcept
                {
                    thread.BlockSequence += (disposition == BlockedDisposition) ? 1U : 0U;
                    thread.ExitCode = exits ? exitCode : thread.ExitCode;
                    thread.Processor = NoProcessor;
                    thread.State = Completed[disposition & 3U];
                });
    }

    [[nodiscard]] ThreadError Wake(const Size index, const HeldBlockToken& token) noexcept
    {
        ThreadRecord& thread = Threads[index];
        return FirstFailure<ThreadError>{}
            .Require(thread.Initialized, ThreadError::NotInitialized)
            .Require((token.Value != 0U) && (token.Thread == index), ThreadError::InvalidBlockToken)
            .Require(thread.State != ThreadState::Exited, ThreadError::AlreadyExited)
            .Require(thread.State == ThreadState::Blocked, ThreadError::InvalidState)
            .Require(token.Value == thread.BlockSequence, ThreadError::StaleBlockToken)
            .Then(
                [&thread]() noexcept
                {
                    thread.State = ThreadState::Ready;
                });
    }

    [[nodiscard]] ThreadError Terminate(const Size index, const UInt64 exitCode) noexcept
    {
        ThreadRecord& thread = Threads[index];
        return FirstFailure<ThreadError>{}
            .Require(thread.Initialized, ThreadError::NotInitialized)
            .Require(thread.State != ThreadState::Exited, ThreadError::AlreadyExited)
            .Require(thread.State != ThreadState::Running, ThreadError::AlreadyRunning)
            .Require(thread.Queue == NoIndex, ThreadError::InReadyQueue)
            .Then(
                [&thread, exitCode]() noexcept
                {
                    thread.Processor = NoProcessor;
                    thread.ExitCode = exitCode;
                    thread.State = ThreadState::Exited;
                });
    }

    [[nodiscard]] bool ThreadIsReady(const Size index) const noexcept
    {
        const ThreadRecord& thread = Threads[index];
        return (thread.State == ThreadState::Ready) && (thread.ExitCode == 0U) && (thread.Processor == NoProcessor);
    }

    [[nodiscard]] SchedulingContextState StateOf(const Size index) const noexcept
    {
        const ContextRecord& context = Contexts[index];
        SchedulingContextState state =
            (context.ChainLength > 1U) ? SchedulingContextState::Donated : SchedulingContextState::Bound;
        state = (context.Remaining == 0U) ? SchedulingContextState::BudgetExhausted : state;
        state = context.Running ? SchedulingContextState::Running : state;
        state = (context.ChainLength == 0U) ? SchedulingContextState::Unbound : state;
        return context.Initialized ? state : SchedulingContextState::Uninitialized;
    }

    [[nodiscard]] UInt64 Tip(const Size index) const noexcept
    {
        const ContextRecord& context = Contexts[index];
        return (context.ChainLength == 0U) ? 0U : context.Chain[context.ChainLength - 1U];
    }

    [[nodiscard]] bool ChainContains(const Size index, const UInt64 value) const noexcept
    {
        bool found{false};
        for (Size position{0U}; position < Contexts[index].ChainLength; ++position)
        {
            found = found || (Contexts[index].Chain[position] == value);
        }
        return found;
    }

    [[nodiscard]] ContextError InitializeContext(const Size index, const Configuration& settings, const UInt64 owner,
                                                 const UInt64 time) noexcept
    {
        ContextRecord& context = Contexts[index];
        UInt64 boundary{0U};
        UInt64 deadline{0U};
        return FirstFailure<ContextError>{}
            .Require(!context.Initialized, ContextError::AlreadyInitialized)
            .Require((owner != 0U) && ConfigurationValid(settings) &&
                         TryAddTicks(time, settings.PeriodTicks, boundary) &&
                         TryAddTicks(time, settings.RelativeDeadlineTicks, deadline),
                     ContextError::InvalidConfiguration)
            .Then(
                [&]() noexcept
                {
                    context.Initialized = true;
                    context.Settings = settings;
                    context.ChargeOwner = owner;
                    context.Remaining = settings.BudgetTicks;
                    context.NextReplenishment = boundary;
                    context.Deadline = deadline;
                    context.LastObserved = time;
                });
    }

    [[nodiscard]] FirstFailure<ContextError> ChainChecks(const Size index, const Identity thread) const noexcept
    {
        return FirstFailure<ContextError>{}
            .Require(Contexts[index].Initialized, ContextError::NotInitialized)
            .Require(IsValidIdentity(thread), ContextError::InvalidThread)
            .Require(thread.Registry == Origin::Main, ContextError::ThreadRegistryMismatch);
    }

    [[nodiscard]] ContextError PinEdge(const ContextError checked, const UInt64 thread) noexcept
    {
        return (checked == ContextError::None) ? MapPinError(Pin(thread, ObjectKind::Thread)) : checked;
    }

    [[nodiscard]] ContextError Bind(const Size index, const Identity thread) noexcept
    {
        ContextRecord& context = Contexts[index];
        const ContextError error =
            PinEdge(ChainChecks(index, thread).Require(context.ChainLength == 0U, ContextError::AlreadyBound).Get(),
                    thread.Value);
        return FirstFailure<ContextError>{}
            .Require(error == ContextError::None, error)
            .Then(
                [&context, thread]() noexcept
                {
                    context.Chain[0U] = thread.Value;
                    context.ChainLength = 1U;
                });
    }

    [[nodiscard]] ContextError Unbind(const Size index, const Identity thread) noexcept
    {
        ContextRecord& context = Contexts[index];
        return ChainChecks(index, thread)
            .Require(!context.Running, ContextError::AlreadyRunning)
            .Require(context.Queue == NoIndex, ContextError::InReadyQueue)
            .Require(context.ChainLength != 0U, ContextError::NotBound)
            .Require(context.ChainLength <= 1U, ContextError::DonationActive)
            .Require(context.Chain[0U] == thread.Value, ContextError::NotCurrentThread)
            .Then(
                [this, &context]() noexcept
                {
                    Unpin(context.Chain[0U]);
                    context.Chain[0U] = 0U;
                    context.ChainLength = 0U;
                });
    }

    [[nodiscard]] ContextError Donate(const Size index, const Identity caller, const Identity server) noexcept
    {
        ContextRecord& context = Contexts[index];
        const ContextError error =
            PinEdge(FirstFailure<ContextError>{}
                        .Require(context.Initialized, ContextError::NotInitialized)
                        .Require(IsValidIdentity(caller) && IsValidIdentity(server), ContextError::InvalidThread)
                        .Require((caller.Registry == Origin::Main) && (server.Registry == Origin::Main),
                                 ContextError::ThreadRegistryMismatch)
                        .Require(!context.Running, ContextError::AlreadyRunning)
                        .Require(context.Queue == NoIndex, ContextError::InReadyQueue)
                        .Require(context.ChainLength != 0U, ContextError::NotBound)
                        .Require(Tip(index) == caller.Value, ContextError::NotCurrentThread)
                        .Require(!ChainContains(index, server.Value), ContextError::DonationCycle)
                        .Require(context.ChainLength < ChainCapacity, ContextError::DonationDepthExceeded)
                        .Get(),
                    server.Value);
        return FirstFailure<ContextError>{}
            .Require(error == ContextError::None, error)
            .Then(
                [&context, server]() noexcept
                {
                    context.Chain[context.ChainLength++] = server.Value;
                });
    }

    [[nodiscard]] ContextError ReturnDonation(const Size index, const Identity server) noexcept
    {
        ContextRecord& context = Contexts[index];
        return ChainChecks(index, server)
            .Require(!context.Running, ContextError::AlreadyRunning)
            .Require(context.ChainLength != 0U, ContextError::NotBound)
            .Require(context.ChainLength != 1U, ContextError::NoDonation)
            .Require(Tip(index) == server.Value, ContextError::NotCurrentThread)
            .Then(
                [this, &context]() noexcept
                {
                    --context.ChainLength;
                    Unpin(context.Chain[context.ChainLength]);
                    context.Chain[context.ChainLength] = 0U;
                });
    }

    // A sample at or after the boundary refreshes the window lazily; every
    // failure leaves the window and the watermark unchanged.
    [[nodiscard]] RunOutcome StartRun(const Size index, const Identity thread, const UInt8 processor,
                                      const UInt64 now) noexcept
    {
        ContextRecord& context = Contexts[index];
        UInt64 boundary = context.NextReplenishment;
        UInt64 deadline = context.Deadline;
        UInt64 start{0U};
        const bool rolls = context.Initialized && (now >= boundary);
        const bool representable = !rolls || RollWindow(context.Settings, now, start, boundary, deadline);
        const UInt64 remaining = rolls ? context.Settings.BudgetTicks : context.Remaining;
        RunOutcome outcome{};
        outcome.Error = FirstFailure<ContextError>{}
                            .Require(context.Initialized, ContextError::NotInitialized)
                            .Require(IsValidIdentity(thread), ContextError::InvalidThread)
                            .Require(thread.Registry == Origin::Main, ContextError::ThreadRegistryMismatch)
                            .Require(IsValidProcessor(processor), ContextError::InvalidCpu)
                            .Require(!context.Running, ContextError::AlreadyRunning)
                            .Require(context.ChainLength != 0U, ContextError::NotBound)
                            .Require(Tip(index) == thread.Value, ContextError::NotCurrentThread)
                            .Require((context.Settings.CpuAffinityMask & ProcessorBit(processor)) != 0U,
                                     ContextError::CpuNotAllowed)
                            .Require(now >= context.LastObserved, ContextError::TimeReversed)
                            .Require(representable, ContextError::TimeRangeExhausted)
                            .Require(remaining != 0U, ContextError::BudgetUnavailable)
                            .Require(now < deadline, ContextError::DeadlineExpired)
                            .Then(
                                [&]() noexcept
                                {
                                    outcome.MaximumRun = Smaller(remaining, Smaller(deadline - now, boundary - now));
                                    context.Remaining = remaining;
                                    context.NextReplenishment = boundary;
                                    context.Deadline = deadline;
                                    context.LastObserved = now;
                                    context.RunStart = now;
                                    context.Running = true;
                                    context.Processor = processor;
                                });
        return outcome;
    }

    // A stop after the boundary charges the current window the time since its
    // start, unless the run covered a whole window after the boundary, which
    // leaves it empty; an unrepresentable window saturates.
    [[nodiscard]] RunOutcome EndRun(const Size index, const UInt8 processor, const UInt64 now) noexcept
    {
        ContextRecord& context = Contexts[index];
        RunOutcome outcome{};
        outcome.Error =
            FirstFailure<ContextError>{}
                .Require(context.Initialized, ContextError::NotInitialized)
                .Require(IsValidProcessor(processor), ContextError::InvalidCpu)
                .Require(context.Running, ContextError::NotRunning)
                .Require(context.Processor == processor, ContextError::WrongCpu)
                .Require((now >= context.LastObserved) && (now >= context.RunStart), ContextError::TimeReversed)
                .Then(
                    [&]() noexcept
                    {
                        outcome.Thread = Tip(index);
                        outcome.Elapsed = now - context.RunStart;
                        outcome.Charged = Smaller(outcome.Elapsed, context.Remaining);
                        outcome.DeadlineMissed = now > context.Deadline;
                        const bool crossed = now >= context.NextReplenishment;
                        const bool coveredWindow =
                            crossed && ((now - context.NextReplenishment) >= context.Settings.PeriodTicks);
                        UInt64 start{0U};
                        const bool rolled = crossed && RollWindow(context.Settings, now, start,
                                                                  context.NextReplenishment, context.Deadline);
                        const UInt64 crossedBudget =
                            (rolled && !coveredWindow)
                                ? context.Settings.BudgetTicks - Smaller(context.Settings.BudgetTicks, now - start)
                                : 0U;
                        outcome.Remaining = crossed ? crossedBudget : (context.Remaining - outcome.Charged);
                        context.NextReplenishment = (crossed && !rolled) ? MaximumTicks : context.NextReplenishment;
                        context.Deadline = (crossed && !rolled) ? MaximumTicks : context.Deadline;
                        context.Remaining = outcome.Remaining;
                        context.LastObserved = now;
                        context.RunStart = 0U;
                        context.Processor = NoProcessor;
                        context.Running = false;
                    });
        return outcome;
    }

    [[nodiscard]] ContextError NextReplenishmentAfter(const Size index, const UInt64 now, UInt64& result) const noexcept
    {
        const ContextRecord& context = Contexts[index];
        result = context.NextReplenishment;
        const bool rolls = context.Initialized && (result != MaximumTicks) && (now >= result);
        const UInt64 period = context.Settings.PeriodTicks;
        const bool representable =
            (result != MaximumTicks) && (!rolls || TryAddTicks(now - ((now - result) % period), period, result));
        return FirstFailure<ContextError>{}
            .Require(context.Initialized, ContextError::NotInitialized)
            .Require(!context.Running, ContextError::AlreadyRunning)
            .Require(now >= context.LastObserved, ContextError::TimeReversed)
            .Require(representable, ContextError::TimeRangeExhausted)
            .Get();
    }

    // The deadline of the earliest window at or after now in which StartRun
    // could admit the context: the window containing now while it has budget
    // and now precedes its deadline, else the following window's deadline.
    [[nodiscard]] UInt64 EffectiveDeadline(const Size index, const UInt64 now) const noexcept
    {
        const ContextRecord& context = Contexts[index];
        UInt64 boundary = context.NextReplenishment;
        UInt64 deadline = context.Deadline;
        UInt64 start{0U};
        const bool rolls = now >= boundary;
        const bool representable =
            context.Initialized && (!rolls || RollWindow(context.Settings, now, start, boundary, deadline));
        const bool eligible = ((rolls ? context.Settings.BudgetTicks : context.Remaining) != 0U) && (now < deadline);
        UInt64 next{0U};
        const bool following = TryAddTicks(boundary, context.Settings.RelativeDeadlineTicks, next);
        const UInt64 later = following ? next : MaximumTicks;
        return !representable ? MaximumTicks : (eligible ? deadline : later);
    }

    // A reservation needs a Blocked caller at its exact token that belongs to
    // the context's chain, but not yet that it is the current thread.
    [[nodiscard]] QueueOutcome Place(const Size queueIndex, const Identity context, const Size contextIndex,
                                     const Identity thread, const Size threadIndex, const bool reserve,
                                     const HeldBlockToken& token) noexcept
    {
        QueueRecord& queue = Queues[queueIndex];
        Size slot{NoIndex};
        QueueError duplicate{QueueError::None};
        for (Size index{0U}; (index < EntryCount) && (duplicate == QueueError::None); ++index)
        {
            const EntryRecord& entry = queue.Entries[index];
            const bool used = entry.Phase != EntryPhase::Empty;
            slot = ((slot == NoIndex) && !used) ? index : slot;
            duplicate = (used && (entry.ThreadValue == thread.Value)) ? QueueError::DuplicateThread : duplicate;
            duplicate = (used && (entry.ContextValue == context.Value)) ? QueueError::DuplicateContext : duplicate;
        }
        QueueOutcome outcome =
            Outcome(FirstFailure<QueueError>{}
                        .Require(IsValidProcessor(queue.Processor), QueueError::InvalidConfiguration)
                        .Require(IsValidIdentity(context), QueueError::InvalidContext)
                        .Require(context.Registry == Origin::Main, QueueError::ContextRegistryMismatch)
                        .Require(IsValidIdentity(thread), QueueError::InvalidThread)
                        .Require(thread.Registry == Origin::Main, QueueError::ThreadRegistryMismatch)
                        .Require(!reserve || (token.Value != 0U), QueueError::InvalidBlockToken)
                        .Require(duplicate == QueueError::None, duplicate)
                        .Require(slot != NoIndex, QueueError::QueueFull)
                        .Get());
        outcome = (outcome.Error == QueueError::None) ? PinPair(context, contextIndex, thread, threadIndex) : outcome;
        if ((outcome.Error != QueueError::None) || (slot == NoIndex))
        {
            return outcome;
        }
        const ThreadRecord& caller = Threads[threadIndex];
        outcome.Error =
            FirstFailure<QueueError>{}
                .Require(reserve || ThreadIsReady(threadIndex), QueueError::ThreadNotReady)
                .Require(!reserve || (caller.State == ThreadState::Blocked), QueueError::ThreadNotBlocked)
                .Require(!reserve || ((token.Thread == threadIndex) && (token.Value == caller.BlockSequence) &&
                                      (caller.ExitCode == 0U) && (caller.Processor == NoProcessor)),
                         QueueError::InvalidBlockToken)
                .Require(IsRunnable(StateOf(contextIndex)), QueueError::ContextNotRunnable)
                .Require(reserve ? ChainContains(contextIndex, thread.Value) : (Tip(contextIndex) == thread.Value),
                         QueueError::ContextThreadMismatch)
                .Require(PlacementAllowed(Contexts[contextIndex].Settings, queue.Processor), QueueError::CpuNotAllowed)
                .Require(Contexts[contextIndex].Queue == NoIndex, QueueError::ContextAlreadyQueued)
                .Require(caller.Queue == NoIndex, QueueError::ThreadAlreadyQueued)
                .Then(
                    [&]() noexcept
                    {
                        queue.Entries[slot] = EntryRecord{reserve ? EntryPhase::Reserved : EntryPhase::Ready,
                                                          contextIndex,
                                                          threadIndex,
                                                          context.Value,
                                                          thread.Value,
                                                          queue.NextSequence,
                                                          0U};
                        outcome.Slot = slot;
                        outcome.Sequence = queue.NextSequence++;
                        Contexts[contextIndex].Queue = queueIndex;
                        Threads[threadIndex].Queue = queueIndex;
                    });
        if (outcome.Error != QueueError::None)
        {
            Unpin(thread.Value);
            Unpin(context.Value);
        }
        return outcome;
    }

    [[nodiscard]] QueueOutcome PinPair(const Identity context, const Size contextIndex, const Identity thread,
                                       const Size threadIndex) noexcept
    {
        const PinError contextPin = Pin(context.Value, ObjectKind::Context);
        const PinError threadPin = (contextPin == PinError::Ok) ? Pin(thread.Value, ObjectKind::Thread) : PinError::Ok;
        const QueueError error =
            FirstFailure<QueueError>{}
                .Require(contextPin == PinError::Ok, QueueError::ContextPinRejected)
                .Require(OwnerOf(context.Value, ObjectKind::Context) == contextIndex,
                         QueueError::ContextInstanceMismatch)
                .Require(threadPin == PinError::Ok, QueueError::ThreadPinRejected)
                .Require(OwnerOf(thread.Value, ObjectKind::Thread) == threadIndex, QueueError::ThreadInstanceMismatch)
                .Get();
        if (error != QueueError::None)
        {
            Unpin((contextPin == PinError::Ok) ? context.Value : 0U);
            Unpin(((threadPin == PinError::Ok) && (contextPin == PinError::Ok)) ? thread.Value : 0U);
        }
        return (error == QueueError::ContextInstanceMismatch) ? Outcome(error) : Outcome(error, contextPin, threadPin);
    }

    void ClearEntry(EntryRecord& entry) noexcept
    {
        Threads[entry.Thread].Queue = NoIndex;
        Contexts[entry.Context].Queue = NoIndex;
        Unpin(entry.ThreadValue);
        Unpin(entry.ContextValue);
        entry = EntryRecord{};
    }

    [[nodiscard]] bool PairValid(const EntryRecord& entry) const noexcept
    {
        return ThreadIsReady(entry.Thread) && IsRunnable(StateOf(entry.Context)) &&
               (Tip(entry.Context) == entry.ThreadValue);
    }

    // TokenQueue NoIndex or a zero sequence describes the default token.
    [[nodiscard]] QueueOutcome FindToken(const Size queueIndex, const Size tokenQueue, const UInt64 sequence,
                                         const EntryPhase phase, const QueueError invalid,
                                         const QueueError stale) const noexcept
    {
        QueueOutcome outcome = Outcome(((tokenQueue != queueIndex) || (sequence == 0U)) ? invalid : stale);
        for (Size index{0U}; (outcome.Error == stale) && (index < EntryCount); ++index)
        {
            const EntryRecord& entry = Queues[queueIndex].Entries[index];
            const UInt64 value = (phase == EntryPhase::Claimed) ? entry.ClaimSequence : entry.Sequence;
            outcome = ((entry.Phase == phase) && (value == sequence))
                          ? QueueOutcome{QueueError::None, PinError::Ok, PinError::Ok,  index,
                                         entry.Context,    entry.Thread, entry.Sequence}
                          : outcome;
        }
        return outcome;
    }

    [[nodiscard]] QueueError Republish(QueueRecord& queue, EntryRecord& entry, const bool newSequence) noexcept
    {
        const ContextRecord& context = Contexts[entry.Context];
        return FirstFailure<QueueError>{}
            .Require(ThreadIsReady(entry.Thread), QueueError::ThreadNotReady)
            .Require(IsRunnable(StateOf(entry.Context)), QueueError::ContextNotRunnable)
            .Require(Tip(entry.Context) == entry.ThreadValue, QueueError::ContextThreadMismatch)
            .Require(PlacementAllowed(context.Settings, queue.Processor), QueueError::CpuNotAllowed)
            .Then(
                [&]() noexcept
                {
                    entry.Sequence = newSequence ? queue.NextSequence++ : entry.Sequence;
                    entry.ClaimSequence = 0U;
                    entry.Phase = EntryPhase::Ready;
                });
    }

    // Commit keeps the reservation-time sequence.
    [[nodiscard]] QueueOutcome ResolveReservation(const Size queueIndex, const Size tokenQueue, const UInt64 sequence,
                                                  const bool commit) noexcept
    {
        QueueOutcome outcome = FindToken(queueIndex, tokenQueue, sequence, EntryPhase::Reserved,
                                         QueueError::InvalidReservation, QueueError::StaleReservation);
        EntryRecord& entry = Queues[queueIndex].Entries[outcome.Slot % EntryCount];
        const bool found = outcome.Error == QueueError::None;
        outcome.Error = (found && commit) ? Republish(Queues[queueIndex], entry, false) : outcome.Error;
        if (found && !commit)
        {
            ClearEntry(entry);
        }
        return outcome;
    }

    // The best Ready entry whose pair still revalidates; one changed behind the
    // queue is skipped and stays Ready. Sequence reports the claim sequence.
    [[nodiscard]] QueueOutcome ClaimNext(const Size queueIndex, const UInt64 now) noexcept
    {
        QueueRecord& queue = Queues[queueIndex];
        Size selected{NoIndex};
        for (Size index{0U}; index < EntryCount; ++index)
        {
            const EntryRecord& entry = queue.Entries[index];
            const EntryRecord& best = queue.Entries[(selected == NoIndex) ? 0U : selected];
            const bool candidate = (entry.Phase == EntryPhase::Ready) && PairValid(entry);
            const bool better =
                candidate && ((selected == NoIndex) ||
                              Outranks(entry, Contexts[entry.Context].Settings, EffectiveDeadline(entry.Context, now),
                                       best, Contexts[best.Context].Settings, EffectiveDeadline(best.Context, now)));
            selected = better ? index : selected;
        }
        EntryRecord& entry = queue.Entries[(selected == NoIndex) ? 0U : selected];
        QueueOutcome outcome = Outcome(FirstFailure<QueueError>{}
                                           .Require(IsValidProcessor(queue.Processor), QueueError::InvalidConfiguration)
                                           .Require(selected != NoIndex, QueueError::Empty)
                                           .Then(
                                               [&]() noexcept
                                               {
                                                   entry.ClaimSequence = queue.NextClaimSequence++;
                                                   entry.Phase = EntryPhase::Claimed;
                                               }));
        return (outcome.Error != QueueError::None)
                   ? outcome
                   : QueueOutcome{QueueError::None, PinError::Ok, PinError::Ok,       selected,
                                  entry.Context,    entry.Thread, entry.ClaimSequence};
    }

    // Cancel restores the FIFO rank.
    [[nodiscard]] QueueOutcome ResolveClaim(const Size queueIndex, const Size tokenQueue, const UInt64 sequence,
                                            const ClaimResolution resolution) noexcept
    {
        QueueOutcome outcome = FindToken(queueIndex, tokenQueue, sequence, EntryPhase::Claimed,
                                         QueueError::InvalidClaim, QueueError::StaleClaim);
        EntryRecord& entry = Queues[queueIndex].Entries[(outcome.Slot == NoIndex) ? 0U : outcome.Slot];
        const bool found = outcome.Error == QueueError::None;
        const bool cancel = found && (resolution == ClaimResolution::Cancel);
        outcome.Error = (found && (resolution == ClaimResolution::Requeue)) ? Republish(Queues[queueIndex], entry, true)
                                                                            : outcome.Error;
        entry.ClaimSequence = cancel ? 0U : entry.ClaimSequence;
        entry.Phase = cancel ? EntryPhase::Ready : entry.Phase;
        if (found && (resolution == ClaimResolution::Commit))
        {
            ClearEntry(entry);
        }
        return outcome;
    }

    [[nodiscard]] QueueOutcome Remove(const Size queueIndex, const Identity thread) noexcept
    {
        QueueOutcome outcome = Outcome(FirstFailure<QueueError>{}
                                           .Require(IsValidIdentity(thread), QueueError::InvalidThread)
                                           .Require(thread.Registry == Origin::Main, QueueError::ThreadRegistryMismatch)
                                           .Require(false, QueueError::NotFound)
                                           .Get());
        for (EntryRecord& entry : Queues[queueIndex].Entries)
        {
            if ((outcome.Error != QueueError::NotFound) || (entry.Phase == EntryPhase::Empty) ||
                (entry.ThreadValue != thread.Value))
            {
                continue;
            }
            outcome =
                Outcome((entry.Phase == EntryPhase::Claimed) ? QueueError::EntryClaimed : QueueError::EntryReserved);
            if (entry.Phase == EntryPhase::Ready)
            {
                outcome = QueueOutcome{
                    QueueError::None, PinError::Ok, PinError::Ok, NoIndex, entry.Context, entry.Thread, 0U};
                ClearEntry(entry);
            }
        }
        return outcome;
    }

    [[nodiscard]] Size CountEntries(const Size queueIndex, const EntryPhase phase) const noexcept
    {
        Size count{0U};
        for (const EntryRecord& entry : Queues[queueIndex].Entries)
        {
            count += (entry.Phase == phase) ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] EntryPhase PhaseOf(const Size index, const bool context) const noexcept
    {
        const Size queue = context ? Contexts[index].Queue : Threads[index].Queue;
        EntryPhase phase{EntryPhase::Empty};
        for (Size slot{0U}; (queue < QueueCount) && (slot < EntryCount); ++slot)
        {
            const EntryRecord& entry = Queues[queue].Entries[slot];
            const bool held = (entry.Phase != EntryPhase::Empty) && ((context ? entry.Context : entry.Thread) == index);
            phase = held ? entry.Phase : phase;
        }
        return phase;
    }
};

template <typename TToken> struct RetiredToken
{
    Size Queue;
    TToken Token;
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
        m_lateEpochStep = m_random.Chance(1U, 4U) ? ((StepsPerSeed * 3U) / 4U) : NoIndex;
        bool matched{true};
        for (Size index{0U}; matched && (index < ScratchThread); ++index)
        {
            matched = CallInitializeThread(index, 1U + (index % 7U)) && Register(ObjectKind::Thread, index) &&
                      CallMakeReady(index);
        }
        for (Size index{0U}; matched && (index < ScratchContext); ++index)
        {
            const Configuration settings = PickConfiguration();
            const UInt64 start = m_model.Now - m_random.Below(Smaller(settings.PeriodTicks, 900U));
            matched = CallInitializeContext(index, settings, 20U + index, start) &&
                      Register(ObjectKind::Context, index) && CallBind(index, ThreadIdentity(index));
        }
        return matched && CheckObservables(true);
    }

    [[nodiscard]] bool Step(const Size step) noexcept
    {
        // Weights in the mixed, placement-burst, and drain phases.
        constexpr WeightedOperation Operations[]{
            {{110U, 10U, 0U}, "enqueue", &Driver::OpEnqueue},
            {{165U, 0U, 4U}, "dispatch cycle", &Driver::OpDispatchCycle},
            {{80U, 0U, 2U}, "resolve claim", &Driver::OpResolveClaim},
            {{35U, 0U, 4U}, "remove", &Driver::OpRemove},
            {{85U, 1U, 0U}, "reservation", &Driver::OpReservation},
            {{65U, 1U, 0U}, "thread run", &Driver::OpThreadRun},
            {{95U, 1U, 0U}, "context run", &Driver::OpContextRun},
            {{45U, 2U, 0U}, "binding", &Driver::OpBinding},
            {{75U, 1U, 0U}, "donation", &Driver::OpDonation},
            {{56U, 4U, 0U}, "lifecycle", &Driver::OpLifecycle},
            {{40U, 0U, 0U}, "initialize", &Driver::OpInitialize},
            {{147U, 0U, 0U}, "advance clock", &Driver::OpAdvance},
            {{2U, 0U, 0U}, "policy violation", &Driver::OpViolation},
            {{3U, 0U, 2U}, "rebuild queue", &Driver::OpRebuild},
        };
        m_step = step;
        // Late epoch: near the end of the range windows stop fitting.
        m_model.Now = (step == m_lateEpochStep) ? (MaximumTicks - 20'001U - m_random.Below(40'000U)) : m_model.Now;
        m_system.Clock.Set(m_model.Now);
        const Size offset = step % PhasePeriod;
        const bool burst = (offset < BurstLength) && m_random.Chance(3U, 5U);
        m_phase = burst ? Phase::Burst : Phase::Mixed;
        m_phase = (!burst && (offset >= BurstLength) && (offset < (2U * BurstLength)) && m_random.Chance(1U, 2U))
                      ? Phase::Drain
                      : m_phase;
        const Size column = static_cast<Size>(m_phase);
        UInt64 total{0U};
        for (const WeightedOperation& entry : Operations)
        {
            total += entry.Weights[column];
        }
        UInt64 choice = m_random.Below(total);
        for (const WeightedOperation& entry : Operations)
        {
            if (choice < entry.Weights[column])
            {
                m_operation = entry.Name;
                return (this->*entry.Run)() && CheckObservables((step % SweepInterval) == 0U);
            }
            choice -= entry.Weights[column];
        }
        return false;
    }

    // The destructors require no claims and no active runs.
    [[nodiscard]] bool Drain() noexcept
    {
        m_operation = "drain";
        bool matched{true};
        for (Size queue{0U}; queue < QueueCount; ++queue)
        {
            for (Size slot{0U}; matched && (slot < EntryCount); ++slot)
            {
                matched = (m_model.Queues[queue].Entries[slot].Phase != EntryPhase::Claimed) ||
                          CallResolveClaim(queue, queue, m_claims[queue][slot], ClaimResolution::Commit);
            }
        }
        for (Size index{0U}; matched && (index < ThreadCount); ++index)
        {
            const ContextRecord& context = m_model.Contexts[index % ContextCount];
            const UInt64 stop = (context.LastObserved > Now()) ? context.LastObserved : Now();
            matched = ((index >= ContextCount) || !context.Running || CallEndRun(index, context.Processor, stop)) &&
                      ((m_model.Threads[index].State != ThreadState::Running) ||
                       CallFinishRun(index, m_model.Threads[index].Processor, ReadyDisposition, 0U));
        }
        return matched && CheckObservables(true);
    }

private:
    using Operation = bool (Driver::*)() noexcept;

    struct WeightedOperation
    {
        UInt32 Weights[3];
        const char* Name;
        Operation Run;
    };

    [[nodiscard]] bool Mismatch(const char* const detail, const UInt64 expected, const UInt64 actual) const noexcept
    {
        static_cast<void>(
            std::fprintf(stderr, "MODEL MISMATCH seed=%llu step=%zu operation=%s: %s expected=%llu actual=%llu\n",
                         static_cast<unsigned long long>(m_seed), m_step, m_operation, detail,
                         static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual)));
        return false;
    }

    [[nodiscard]] bool Same(const char* const detail, const UInt64 expected, const UInt64 actual) const noexcept
    {
        return (expected == actual) || Mismatch(detail, expected, actual);
    }

    template <typename TEnum>
    [[nodiscard]] bool SameCode(const char* const detail, const TEnum expected, const TEnum actual) const noexcept
    {
        return Same(detail, static_cast<UInt64>(expected), static_cast<UInt64>(actual));
    }

    template <typename TIdentity>
    [[nodiscard]] bool SameIdentity(const char* const detail, const UInt64 expected,
                                    const TIdentity actual) const noexcept
    {
        const TIdentity wanted = (expected == 0U) ? TIdentity{} : TIdentity{m_system.Registry, ObjectId{expected}};
        return actual.IsEqualTo(wanted) || Mismatch(detail, expected, actual.GetValue());
    }

    [[nodiscard]] bool SameQueueResult(const QueueOutcome& expected, const bool succeeded,
                                       const Kernel::Scheduler::ReadyQueueFailure& failure) const noexcept
    {
        return SameCode("result", expected.Error, succeeded ? QueueError::None : failure.Error) &&
               (succeeded || (SameCode("context pin error", expected.ContextPin, failure.ContextPin) &&
                              SameCode("thread pin error", expected.ThreadPin, failure.ThreadPin)));
    }

    // Processor scheduling widens affinity and introduces ABI alignment padding. Compare semantic
    // fields; padding bytes are not configuration state.
    [[nodiscard]] bool SameConfiguration(const Configuration& expected, const Configuration& actual) const noexcept
    {
        return Same("budget", expected.BudgetTicks, actual.BudgetTicks) &&
               Same("period", expected.PeriodTicks, actual.PeriodTicks) &&
               Same("deadline", expected.RelativeDeadlineTicks, actual.RelativeDeadlineTicks) &&
               Same("weight", expected.Weight, actual.Weight) && Same("priority", expected.Priority, actual.Priority) &&
               SameCode("criticality", expected.Criticality, actual.Criticality) &&
               Same("affinity", expected.CpuAffinityMask, actual.CpuAffinityMask) &&
               SameCode("class", expected.Class, actual.Class) && Same("reserved", expected.Reserved, actual.Reserved);
    }

    template <typename TIdentity> [[nodiscard]] TIdentity ToId(const Identity identity) const noexcept
    {
        const ObjectRegistry& origin =
            (identity.Registry == Origin::Main) ? m_system.Registry : m_system.ForeignRegistry;
        return (identity.Registry == Origin::None) ? TIdentity{} : TIdentity{origin, ObjectId{identity.Value}};
    }

    void Touch(const Size context, const Size thread) noexcept
    {
        m_touchedContexts[m_touchedContextCount] = context;
        m_touchedContextCount += ((context < ContextCount) && (m_touchedContextCount + 1U < TouchCapacity)) ? 1U : 0U;
        m_touchedThreads[m_touchedThreadCount] = thread;
        m_touchedThreadCount += ((thread < ThreadCount) && (m_touchedThreadCount + 1U < TouchCapacity)) ? 1U : 0U;
    }

    // IMonotonicClock never reports the all-ones value.
    void Advance(const UInt64 ticks) noexcept
    {
        m_model.Now = (m_model.Now > (MaximumTicks - 1U - ticks)) ? (MaximumTicks - 1U) : (m_model.Now + ticks);
        m_system.Clock.Set(m_model.Now);
    }

    [[nodiscard]] UInt64 Now() const noexcept
    {
        return m_system.Clock.GetTicks();
    }

    [[nodiscard]] bool CheckThread(const Size index) const noexcept
    {
        const ThreadRecord& expected = m_model.Threads[index];
        const Thread& thread = *m_system.Threads[index];
        const auto snapshot = thread.GetSnapshot();
        const bool blocked = expected.State == ThreadState::Blocked;
        return SameCode("thread state", expected.State, snapshot.State) &&
               Same("thread block token valid", blocked, snapshot.BlockToken.IsValid()) &&
               Same("thread block token", blocked ? expected.BlockSequence : 0U, snapshot.BlockToken.GetValue()) &&
               Same("thread run sequence", expected.RunSequence, snapshot.RunSequence) &&
               Same("thread exit code", expected.ExitCode, snapshot.ExitCode) &&
               Same("thread processor", expected.Processor, snapshot.RunningProcessor.GetValue()) &&
               Same("thread initialized", expected.Initialized, thread.IsInitialized()) &&
               Same("thread charge owner", expected.ChargeOwner, thread.GetChargeOwner().GetValue());
    }

    [[nodiscard]] bool CheckContext(const Size index) const noexcept
    {
        const ContextRecord& expected = m_model.Contexts[index];
        const SchedulingContext& context = *m_system.Contexts[index];
        const auto snapshot = context.GetSnapshot();
        const Size depth = (expected.ChainLength == 0U) ? 0U : (expected.ChainLength - 1U);
        return SameIdentity("context base thread", expected.Chain[0U], snapshot.BaseThread) &&
               SameIdentity("context current thread", m_model.Tip(index), snapshot.CurrentThread) &&
               SameCode("context state", m_model.StateOf(index), snapshot.State) &&
               Same("context remaining budget", expected.Remaining, snapshot.RemainingBudgetTicks) &&
               Same("context next replenishment", expected.NextReplenishment, snapshot.NextReplenishmentTicks) &&
               Same("context absolute deadline", expected.Deadline, snapshot.AbsoluteDeadlineTicks) &&
               Same("context effective deadline", m_model.EffectiveDeadline(index, Now()),
                    context.GetEffectiveDeadline(MonotonicTime{Now()}).GetTicks()) &&
               Same("context run start", expected.RunStart, snapshot.RunStartTicks) &&
               Same("context processor", expected.Processor, snapshot.RunningProcessor.GetValue()) &&
               Same("context donation depth", depth, snapshot.DonationDepth) &&
               Same("context budget exhausted", expected.Remaining == 0U, snapshot.IsBudgetExhausted) &&
               Same("context initialized", expected.Initialized, context.IsReady()) &&
               Same("context charge owner", expected.ChargeOwner, context.GetChargeOwner().GetValue()) &&
               SameConfiguration(expected.Settings, context.GetConfiguration());
    }

    // Queue counters after every step; the touched objects, or all objects.
    [[nodiscard]] bool CheckObservables(const bool everything) noexcept
    {
        bool matched = Same("clock", m_model.Now, Now());
        for (Size index{0U}; matched && (index < QueueCount); ++index)
        {
            const auto state = m_system.Queues[index]->GetState();
            matched =
                Same("queue next sequence", m_model.Queues[index].NextSequence, state.NextSequence) &&
                Same("queue ready", m_model.CountEntries(index, EntryPhase::Ready), state.ReadyEntries) &&
                Same("queue claimed", m_model.CountEntries(index, EntryPhase::Claimed), state.ClaimedEntries) &&
                Same("queue reserved", m_model.CountEntries(index, EntryPhase::Reserved), state.ReservedEntries) &&
                Same("queue processor", m_model.Queues[index].Processor, state.Processor.GetValue());
        }
        for (Size index{0U}; matched && (index < (everything ? ThreadCount : m_touchedThreadCount)); ++index)
        {
            matched = CheckThread(everything ? index : m_touchedThreads[index]);
        }
        for (Size index{0U}; matched && (index < (everything ? ContextCount : m_touchedContextCount)); ++index)
        {
            matched = CheckContext(everything ? index : m_touchedContexts[index]);
        }
        m_touchedContextCount = 0U;
        m_touchedThreadCount = 0U;
        return matched;
    }

    [[nodiscard]] Size Draw(const Size count) noexcept
    {
        return static_cast<Size>(m_random.Below(count));
    }

    [[nodiscard]] Size ServerThread() noexcept
    {
        return ServerThreadFirst + static_cast<Size>(m_random.Below(ScratchThread - ServerThreadFirst));
    }

    [[nodiscard]] Identity ThreadIdentity(const Size index) const noexcept
    {
        return Identity{Origin::Main, m_model.Threads[index].ObjectValue};
    }

    [[nodiscard]] Identity ContextIdentity(const Size index) const noexcept
    {
        return Identity{Origin::Main, m_model.Contexts[index].ObjectValue};
    }

    [[nodiscard]] Size TipThread(const Size context) const noexcept
    {
        return m_model.OwnerOf(m_model.Tip(context), ObjectKind::Thread);
    }

    // Occasionally a stale, mistyped, foreign, invalid, or unnamed identity.
    [[nodiscard]] Identity Vary(const UInt64 current, const ObjectKind kind) noexcept
    {
        constexpr UInt64 Unnamed[]{UInt64{1U} << GenerationShift, UInt64{300U} | (UInt64{1U} << GenerationShift), 7U};
        const UInt64 mode = m_random.Below(100U);
        const Identity other =
            (kind == ObjectKind::Thread) ? ContextIdentity(Draw(ContextCount)) : ThreadIdentity(Draw(ThreadCount));
        const Identity invalid = (mode < 94U) ? Identity{Origin::Foreign, current} : Identity{Origin::None, 0U};
        const Identity unusual = (mode < 97U) ? invalid : Identity{Origin::Main, Unnamed[m_random.Below(3U)]};
        const Identity stale = (mode < 86U) ? Identity{Origin::Main, current + (UInt64{1U} << GenerationShift)} : other;
        const Identity varied = (mode < 90U) ? stale : unusual;
        return (mode < 80U) ? Identity{Origin::Main, current} : varied;
    }

    [[nodiscard]] Identity VaryThread(const Size index) noexcept
    {
        return Vary(m_model.Threads[index].ObjectValue, ObjectKind::Thread);
    }

    [[nodiscard]] Size PickQueue() noexcept
    {
        const UInt64 choice = m_random.Below(20U);
        const Size other = (choice < 18U) ? 1U : 2U;
        return (choice < 12U) ? 0U : other;
    }

    [[nodiscard]] UInt8 PickProcessor(const UInt8 preferred) noexcept
    {
        const UInt64 choice = m_random.Below(20U);
        const UInt8 invalid = m_random.Chance(1U, 2U) ? MaximumProcessorCount : NoProcessor;
        const UInt8 other = (choice < 19U) ? static_cast<UInt8>(m_random.Below(MaximumProcessorCount)) : invalid;
        return (choice < 17U) ? preferred : other;
    }

    // The lowest CPU the affinity names; CPU 0 when it names none.
    [[nodiscard]] UInt8 AllowedProcessor(const Size context) const noexcept
    {
        UInt8 processor{0U};
        while ((processor < MaximumProcessorCount) &&
               ((m_model.Contexts[context].Settings.CpuAffinityMask & ProcessorBit(processor)) == 0U))
        {
            ++processor;
        }
        return (processor < MaximumProcessorCount) ? processor : UInt8{0U};
    }

    // Sometimes an exact edge of the context's own state; a context only
    // requires its own samples to be nondecreasing.
    [[nodiscard]] UInt64 PickSample(const Size context) noexcept
    {
        const ContextRecord& record = m_model.Contexts[context];
        const UInt64 watermark = record.Running ? record.RunStart : record.LastObserved;
        const UInt64 back = m_random.Below(64U);
        const UInt64 edges[]{watermark, watermark - ((watermark != 0U) ? 1U : 0U), record.NextReplenishment,
                             record.Deadline, (Now() > back) ? (Now() - back) : 0U};
        const UInt64 mode = m_random.Below(20U);
        return ((mode < 5U) && (edges[mode % 5U] != MaximumTicks)) ? edges[mode % 5U] : Now();
    }

    // Mostly exactly the admitted limit, as a boundary-armed timer stops a run.
    [[nodiscard]] UInt64 PickRunLength(const UInt64 maximumRun) noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const UInt64 late = maximumRun + 1U + m_random.Below((mode < 95U) ? 20U : 400U);
        const UInt64 other = ((mode >= 90U) && (mode < 95U)) ? 0U : late;
        const UInt64 early = (mode < 75U) ? m_random.Below(maximumRun + 1U) : other;
        return (mode < 45U) ? maximumRun : early;
    }

    [[nodiscard]] UInt8 PickDisposition(UInt64& exitCode) noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        exitCode = (mode >= 93U) ? m_random.Below(4U) : 0U;
        const UInt8 stopped = (mode < 93U) ? BlockedDisposition : ExitedDisposition;
        return (mode < 65U) ? ReadyDisposition : stopped;
    }

    // Few criticality and priority values, so deadline and FIFO often decide.
    [[nodiscard]] Configuration PickConfiguration() noexcept
    {
        using Class = SchedulingClass;
        using Level = SchedulingCriticality;
        constexpr Class Classes[]{Class::DeadlineSporadic, Class::DeadlineSporadic, Class::DeadlineSporadic,
                                  Class::DeadlineSporadic, Class::WeightedFair,     Class::WeightedFair,
                                  Class::Background,       Class::Isolated,         Class::Isolated,
                                  Class::EmergencyRecovery};
        constexpr Level Criticalities[]{Level::Normal, Level::Normal, Level::Normal,
                                        Level::High,   Level::Low,    Level::Emergency};
        constexpr UInt8 Priorities[]{8U, 8U, 8U, 9U, 7U, 0U, 255U};
        constexpr UInt64 Periods[]{1U, 10U, 50U, 100U};
        Configuration settings{};
        settings.Class = Classes[m_random.Below(10U)];
        settings.Criticality = Criticalities[m_random.Below(6U)];
        settings.Priority = Priorities[m_random.Below(7U)];
        const UInt64 periodMode = m_random.Below(100U);
        const UInt64 huge = UInt64{1U} << (32U + m_random.Below(9U));
        const UInt64 listed = (periodMode < 96U) ? Periods[m_random.Below(4U)] : huge;
        settings.PeriodTicks = (periodMode < 70U) ? (20U + m_random.Below(181U)) : listed;
        const UInt64 deadlineMode = m_random.Below(20U);
        const UInt64 early = (deadlineMode < 7U) ? 0U : m_random.Below((settings.PeriodTicks / 2U) + 1U);
        settings.RelativeDeadlineTicks =
            (deadlineMode < 15U) ? (settings.PeriodTicks - early) : (1U + m_random.Below(settings.PeriodTicks));
        const UInt64 budgetMode = m_random.Below(10U);
        settings.BudgetTicks = (budgetMode == 0U) ? 1U : (1U + m_random.Below(settings.RelativeDeadlineTicks));
        settings.BudgetTicks = (budgetMode == 1U) ? settings.RelativeDeadlineTicks : settings.BudgetTicks;
        settings.Weight = static_cast<UInt16>(1U + m_random.Below(1000U));
        const UInt8 single = ProcessorBit(m_random.Chance(1U, 2U) ? UInt8{0U} : static_cast<UInt8>(m_random.Below(4U)));
        const UInt8 broad = m_random.Chance(4U, 5U) ? static_cast<UInt8>(1U | m_random.Below(16U))
                                                    : static_cast<UInt8>(1U + m_random.Below(15U));
        settings.CpuAffinityMask =
            ((settings.Class == SchedulingClass::Isolated) && m_random.Chance(3U, 4U)) ? single : broad;
        return settings;
    }

    [[nodiscard]] Configuration BreakConfiguration(Configuration settings, const UInt64 time) noexcept
    {
        constexpr UInt8 Unknown[]{0U, 6U, 255U};
        const UInt64 mode = m_random.Below(12U);
        const UInt8 unknown = Unknown[m_random.Below(3U)];
        const UInt64 unrepresentable = MaximumTicks - time + m_random.Below(2U);
        settings.BudgetTicks = (mode == 0U) ? 0U : settings.BudgetTicks;
        settings.PeriodTicks = (mode == 1U) ? 0U : settings.PeriodTicks;
        settings.RelativeDeadlineTicks = (mode == 2U) ? 0U : settings.RelativeDeadlineTicks;
        settings.BudgetTicks = (mode == 3U) ? (settings.RelativeDeadlineTicks + 1U) : settings.BudgetTicks;
        settings.RelativeDeadlineTicks = (mode == 4U) ? (settings.PeriodTicks + 1U) : settings.RelativeDeadlineTicks;
        settings.Weight = (mode == 5U) ? UInt16{0U} : settings.Weight;
        settings.CpuAffinityMask = (mode == 6U) ? UInt8{0U} : settings.CpuAffinityMask;
        settings.CpuAffinityMask = (mode == 7U) ? static_cast<UInt8>(settings.CpuAffinityMask | (0x10U << (time % 4U)))
                                                : settings.CpuAffinityMask;
        settings.Class = (mode == 8U) ? static_cast<SchedulingClass>(unknown) : settings.Class;
        settings.Criticality = (mode == 9U) ? static_cast<SchedulingCriticality>(unknown) : settings.Criticality;
        settings.Reserved = (mode == 10U) ? static_cast<UInt16>(1U + (time % 0xFFFFU)) : settings.Reserved;
        settings.PeriodTicks = (mode == 11U) ? unrepresentable : settings.PeriodTicks;
        settings.RelativeDeadlineTicks = (mode == 11U) ? unrepresentable : settings.RelativeDeadlineTicks;
        settings.BudgetTicks = (mode == 11U) ? 1U : settings.BudgetTicks;
        return settings;
    }

    [[nodiscard]] HeldBlockToken PickBlockToken(const Size thread) noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const HeldBlockToken foreign = (mode < 95U) ? m_blockTokens[Draw(ThreadCount)][0U] : HeldBlockToken{};
        return (mode < 88U) ? m_blockTokens[thread][(mode < 80U) ? 0U : 1U] : foreign;
    }

    void RememberBlockToken(const Size thread, const ThreadBlockToken token) noexcept
    {
        m_blockTokens[thread][1U] = m_blockTokens[thread][0U];
        m_blockTokens[thread][0U] = HeldBlockToken{token, thread, token.GetValue()};
    }

    template <typename TPredicate> [[nodiscard]] Size FindIndex(const Size count, const TPredicate& predicate) noexcept
    {
        const Size first = static_cast<Size>(m_random.Below(count));
        Size found{NoIndex};
        for (Size offset{0U}; (found == NoIndex) && (offset < count); ++offset)
        {
            found = predicate((first + offset) % count) ? ((first + offset) % count) : NoIndex;
        }
        return found;
    }

    template <typename TPreferred, typename TFallback>
    [[nodiscard]] Size Prefer(const Size count, const UInt64 percent, const TPreferred& preferred,
                              const TFallback& fallback) noexcept
    {
        const Size found = (m_random.Below(100U) < percent) ? FindIndex(count,
                                                                        [&](const Size index) noexcept
                                                                        {
                                                                            return preferred(index) && fallback(index);
                                                                        })
                                                            : NoIndex;
        return (found != NoIndex) ? found : FindIndex(count, fallback);
    }

    [[nodiscard]] Size FindSlot(const Size queue, const EntryPhase phase) noexcept
    {
        return FindIndex(EntryCount,
                         [this, queue, phase](const Size slot) noexcept
                         {
                             return m_model.Queues[queue].Entries[slot].Phase == phase;
                         });
    }

    // A claimed entry of a valid queue satisfying predicate, as queue and slot.
    template <typename TPredicate>
    [[nodiscard]] bool FindClaim(const TPredicate& predicate, Size& queue, Size& slot) noexcept
    {
        const Size found = FindIndex(ValidQueueCount * EntryCount,
                                     [this, &predicate](const Size index) noexcept
                                     {
                                         const EntryRecord& entry =
                                             m_model.Queues[index / EntryCount].Entries[index % EntryCount];
                                         return (entry.Phase == EntryPhase::Claimed) && predicate(entry);
                                     });
        queue = found / EntryCount;
        slot = found % EntryCount;
        return found != NoIndex;
    }

    // A usable live token, else a retired, default, or other-queue token;
    // returns the issuing queue.
    template <typename TToken, typename TUsable>
    [[nodiscard]] Size PickToken(const Size queue, const EntryPhase phase, const TToken (&live)[QueueCount][EntryCount],
                                 const RetiredToken<TToken> (&retired)[RetiredCapacity], const Size retiredCount,
                                 const TUsable& usable, TToken& token) noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const Size slot =
            (queue >= ValidQueueCount)
                ? NoIndex
                : FindIndex(EntryCount,
                            [&](const Size index) noexcept
                            {
                                return (m_model.Queues[queue].Entries[index].Phase == phase) && usable(queue, index);
                            });
        const Size owner = (queue + 1U) % ValidQueueCount;
        const Size other = FindSlot(owner, phase);
        const RetiredToken<TToken>& old = retired[m_random.Below((retiredCount == 0U) ? 1U : retiredCount)];
        const bool useLive = (mode < 78U) && (slot != NoIndex);
        const bool useRetired = !useLive && (mode < 90U) && (retiredCount != 0U);
        const bool useOther = !useLive && !useRetired && (mode >= 95U) && (other != NoIndex);
        token = useOther ? live[owner][other % EntryCount] : TToken{};
        token = useRetired ? old.Token : token;
        token = useLive ? live[queue % QueueCount][slot % EntryCount] : token;
        Size issuer = useOther ? owner : NoIndex;
        issuer = useRetired ? old.Queue : issuer;
        return useLive ? queue : issuer;
    }

    // Policy: no direct transition of an object in a Ready or Reserved entry.
    [[nodiscard]] bool MayTransition(const Size index, const bool context) const noexcept
    {
        const EntryPhase phase = m_model.PhaseOf(index, context);
        return (phase == EntryPhase::Empty) || (phase == EntryPhase::Claimed);
    }

    [[nodiscard]] bool IsDeadThread(const Size index) const noexcept
    {
        return (m_model.Threads[index].State == ThreadState::Exited) || (m_model.Threads[index].ObjectValue == 0U);
    }

    [[nodiscard]] bool IsBaseThread(const Size index) const noexcept
    {
        bool base{false};
        for (const ContextRecord& context : m_model.Contexts)
        {
            base = base || ((context.ChainLength != 0U) && (context.Chain[0U] == m_model.Threads[index].ObjectValue));
        }
        return base;
    }

    // Paired calls predict with the model, call the real object, and compare.
    [[nodiscard]] bool Register(const ObjectKind kind, const Size index) noexcept
    {
        const bool thread = kind == ObjectKind::Thread;
        const UInt64 expected = m_model.Register(kind, index);
        const auto created = thread ? m_system.Issuer.CreateObject(*m_system.Threads[index])
                                    : m_system.Issuer.CreateObject(*m_system.Contexts[index]);
        (thread ? m_model.Threads[index].ObjectValue : m_model.Contexts[index].ObjectValue) = expected;
        return Same("registered", 1U, created.HasValue()) &&
               Same("registered object", expected, created.GetValue().GetValue());
    }

    [[nodiscard]] bool CallInitializeThread(const Size index, const UInt64 owner) noexcept
    {
        Touch(NoIndex, index);
        return SameCode("thread initialize", m_model.InitializeThread(index, owner),
                        m_system.Threads[index]->Initialize(ResourceDomainId{owner}));
    }

    [[nodiscard]] bool CallMakeReady(const Size index) noexcept
    {
        Touch(NoIndex, index);
        return SameCode("make ready", m_model.MakeReady(index), m_system.Threads[index]->MakeReady());
    }

    [[nodiscard]] bool CallBeginRun(const Size index, const UInt8 processor, ThreadError& result) noexcept
    {
        Touch(NoIndex, index);
        result = m_model.BeginRun(index, processor);
        return SameCode("begin run", result, m_system.Threads[index]->BeginRun(CpuId{processor}));
    }

    [[nodiscard]] bool CallFinishRun(const Size index, const UInt8 processor, const UInt8 disposition,
                                     const UInt64 exitCode) noexcept
    {
        Touch(NoIndex, index);
        const ThreadError expected = m_model.FinishRun(index, processor, disposition, exitCode);
        const auto finished = m_system.Threads[index]->FinishRun(
            CpuId{processor}, static_cast<ThreadRunDisposition>(disposition), exitCode);
        const bool matched =
            SameCode("finish run", expected, finished.HasValue() ? ThreadError::None : finished.GetError());
        if (matched && (expected == ThreadError::None) && (m_model.Threads[index].State == ThreadState::Blocked))
        {
            RememberBlockToken(index, finished.GetValue().BlockToken);
        }
        return matched;
    }

    [[nodiscard]] bool CallWake(const Size index, const HeldBlockToken& token) noexcept
    {
        Touch(NoIndex, index);
        return SameCode("wake", m_model.Wake(index, token), m_system.Threads[index]->Wake(token.Token));
    }

    [[nodiscard]] bool CallTerminate(const Size index, const UInt64 exitCode) noexcept
    {
        Touch(NoIndex, index);
        return SameCode("terminate", m_model.Terminate(index, exitCode), m_system.Threads[index]->Terminate(exitCode));
    }

    [[nodiscard]] bool CallInitializeContext(const Size index, const Configuration& settings, const UInt64 owner,
                                             const UInt64 time) noexcept
    {
        Touch(index, NoIndex);
        return SameCode("context initialize", m_model.InitializeContext(index, settings, owner, time),
                        m_system.Contexts[index]->Initialize(settings, ResourceDomainId{owner}, MonotonicTime{time}));
    }

    [[nodiscard]] bool CallBind(const Size index, const Identity thread) noexcept
    {
        Touch(index, NoIndex);
        return SameCode("bind", m_model.Bind(index, thread), m_system.Contexts[index]->Bind(ToId<ThreadId>(thread)));
    }

    [[nodiscard]] bool CallUnbind(const Size index, const Identity thread) noexcept
    {
        Touch(index, NoIndex);
        return SameCode("unbind", m_model.Unbind(index, thread),
                        m_system.Contexts[index]->Unbind(ToId<ThreadId>(thread)));
    }

    [[nodiscard]] bool CallDonate(const Size index, const Identity caller, const Identity server,
                                  ContextError& result) noexcept
    {
        Touch(index, NoIndex);
        result = m_model.Donate(index, caller, server);
        return SameCode("donate", result,
                        m_system.Contexts[index]->Donate(ToId<ThreadId>(caller), ToId<ThreadId>(server)));
    }

    [[nodiscard]] bool CallReturnDonation(const Size index, const Identity server) noexcept
    {
        Touch(index, NoIndex);
        return SameCode("return donation", m_model.ReturnDonation(index, server),
                        m_system.Contexts[index]->ReturnDonation(ToId<ThreadId>(server)));
    }

    [[nodiscard]] bool CallStartRun(const Size index, const Identity thread, const UInt8 processor, const UInt64 now,
                                    RunOutcome& result) noexcept
    {
        Touch(index, NoIndex);
        const Configuration& settings = m_model.Contexts[index].Settings;
        result = m_model.StartRun(index, thread, processor, now);
        m_runAdmitted[index] = m_runAdmitted[index] || (result.Error == ContextError::None);
        const auto started =
            m_system.Contexts[index]->StartRun(ToId<ThreadId>(thread), CpuId{processor}, MonotonicTime{now});
        if (!SameCode("start run", result.Error, started.HasValue() ? ContextError::None : started.GetError()))
        {
            return false;
        }
        const auto& dispatch = started.GetValue();
        return (result.Error != ContextError::None) ||
               (SameIdentity("dispatch thread", thread.Value, dispatch.Thread) &&
                Same("dispatch maximum run", result.MaximumRun, dispatch.MaximumRunTicks) &&
                Same("dispatch weight", settings.Weight, dispatch.Weight) &&
                Same("dispatch priority", settings.Priority, dispatch.Priority) &&
                SameCode("dispatch criticality", settings.Criticality, dispatch.Criticality) &&
                SameCode("dispatch class", settings.Class, dispatch.Class) &&
                Same("dispatch processor", processor, dispatch.Processor.GetValue()));
    }

    [[nodiscard]] bool CallEndRun(const Size index, const UInt8 processor, const UInt64 now) noexcept
    {
        Touch(index, NoIndex);
        const RunOutcome expected = m_model.EndRun(index, processor, now);
        const auto ended = m_system.Contexts[index]->EndRun(CpuId{processor}, MonotonicTime{now});
        if (!SameCode("end run", expected.Error, ended.HasValue() ? ContextError::None : ended.GetError()))
        {
            return false;
        }
        const auto& receipt = ended.GetValue();
        return (expected.Error != ContextError::None) ||
               (SameIdentity("receipt thread", expected.Thread, receipt.Thread) &&
                Same("receipt elapsed", expected.Elapsed, receipt.ElapsedTicks) &&
                Same("receipt charged", expected.Charged, receipt.ChargedTicks) &&
                Same("receipt overrun", expected.Elapsed - expected.Charged, receipt.OverrunTicks) &&
                Same("receipt remaining budget", expected.Remaining, receipt.RemainingBudgetTicks) &&
                Same("receipt deadline missed", expected.DeadlineMissed, receipt.DeadlineMissed) &&
                Same("receipt budget exhausted", expected.Remaining == 0U, receipt.BudgetExhausted));
    }

    [[nodiscard]] bool CallNextReplenishment(const Size index, const UInt64 now) noexcept
    {
        Touch(index, NoIndex);
        UInt64 boundary{0U};
        const ContextError expected = m_model.NextReplenishmentAfter(index, now, boundary);
        const auto actual = m_system.Contexts[index]->GetNextReplenishmentAfter(MonotonicTime{now});
        return SameCode("next replenishment", expected, actual.HasValue() ? ContextError::None : actual.GetError()) &&
               ((expected != ContextError::None) ||
                Same("next replenishment boundary", boundary, actual.GetValue().GetTicks()));
    }

    [[nodiscard]] bool CallPlace(const Size queue, const Identity context, const Size contextIndex,
                                 const Identity thread, const Size threadIndex, const bool reserve,
                                 const HeldBlockToken& token) noexcept
    {
        Touch(contextIndex, threadIndex);
        const QueueOutcome expected = m_model.Place(queue, context, contextIndex, thread, threadIndex, reserve, token);
        ReadyQueue& real = *m_system.Queues[queue];
        if (!reserve)
        {
            const auto placed = real.Enqueue(ToId<SchedulingContextId>(context), *m_system.Contexts[contextIndex],
                                             ToId<ThreadId>(thread), *m_system.Threads[threadIndex]);
            return SameQueueResult(expected, placed.HasValue(), placed.GetError()) &&
                   (!placed.HasValue() || Same("enqueue sequence", expected.Sequence, placed.GetValue().Sequence));
        }
        const auto reserved =
            real.ReserveBlockedEnqueue(ToId<SchedulingContextId>(context), *m_system.Contexts[contextIndex],
                                       ToId<ThreadId>(thread), *m_system.Threads[threadIndex], token.Token);
        if (reserved.HasValue() && (expected.Slot < EntryCount))
        {
            m_reservations[queue][expected.Slot] = reserved.GetValue();
        }
        return SameQueueResult(expected, reserved.HasValue(), reserved.GetError()) &&
               (!reserved.HasValue() ||
                (Same("reservation valid", 1U, reserved.GetValue().IsValid()) &&
                 Same("reservation sequence", expected.Sequence, reserved.GetValue().GetSequence())));
    }

    [[nodiscard]] bool CallResolveReservation(const Size queue, const Size tokenQueue,
                                              const ReadyQueueEnqueueReservation token, const bool commit) noexcept
    {
        const QueueOutcome expected = m_model.ResolveReservation(queue, tokenQueue, token.GetSequence(), commit);
        Touch(expected.Context, expected.Thread);
        ReadyQueue& real = *m_system.Queues[queue];
        bool matched{false};
        if (commit)
        {
            const auto committed = real.CommitReservedEnqueue(token);
            matched =
                SameQueueResult(expected, committed.HasValue(), committed.GetError()) &&
                (!committed.HasValue() || Same("committed sequence", expected.Sequence, committed.GetValue().Sequence));
        }
        else
        {
            matched = SameCode("cancel reservation", expected.Error, real.CancelReservedEnqueue(token));
        }
        if (matched && (expected.Error == QueueError::None) && (expected.Slot < EntryCount))
        {
            m_reservations[queue][expected.Slot] = ReadyQueueEnqueueReservation{};
            m_retiredReservations[m_retiredNext++ % RetiredCapacity] = RetiredToken{queue, token};
            m_retiredReservationCount = Smaller(m_retiredReservationCount + 1U, RetiredCapacity);
        }
        return matched;
    }

    // Usually the clock; sometimes an exact window edge of a Ready entry, which
    // decides whether its window still admits it at the selection sample.
    [[nodiscard]] UInt64 PickSelectionSample(const Size queue) noexcept
    {
        const Size slot = FindSlot(queue % QueueCount, EntryPhase::Ready);
        return ((slot == NoIndex) || m_random.Chance(3U, 4U))
                   ? Now()
                   : PickSample(m_model.Queues[queue % QueueCount].Entries[slot].Context);
    }

    [[nodiscard]] bool CallClaimNext(const Size queue, Size& slot) noexcept
    {
        const UInt64 sample = PickSelectionSample(queue);
        const QueueOutcome expected = m_model.ClaimNext(queue, sample);
        const auto actual = m_system.Queues[queue]->ClaimNext(MonotonicTime{sample});
        const bool matched = SameQueueResult(expected, actual.HasValue(), actual.GetError());
        slot = (matched && actual.HasValue() && (expected.Slot < EntryCount)) ? expected.Slot : NoIndex;
        if (slot == NoIndex)
        {
            return matched;
        }
        const auto& claim = actual.GetValue();
        const EntryRecord& entry = m_model.Queues[queue].Entries[slot];
        Touch(entry.Context, entry.Thread);
        m_claims[queue][slot] = claim.GetToken();
        m_runAdmitted[entry.Context] = false;
        return SameIdentity("claimed context", entry.ContextValue, claim.GetContextId()) &&
               SameIdentity("claimed thread", entry.ThreadValue, claim.GetThreadId()) &&
               Same("claimed context storage", 1U, &claim.GetContext() == m_system.Contexts[entry.Context]) &&
               Same("claimed thread storage", 1U, &claim.GetThread() == m_system.Threads[entry.Thread]) &&
               Same("claim token valid", 1U, claim.GetToken().IsValid()) &&
               Same("claim token sequence", expected.Sequence, claim.GetToken().GetSequence()) &&
               Same("claimed effective deadline", m_model.EffectiveDeadline(entry.Context, sample),
                    claim.GetEffectiveDeadlineTicks()) &&
               SameConfiguration(m_model.Contexts[entry.Context].Settings, claim.GetConfiguration());
    }

    [[nodiscard]] bool CallResolveClaim(const Size queue, const Size tokenQueue, const ReadyQueueClaimToken token,
                                        const ClaimResolution resolution) noexcept
    {
        const QueueOutcome expected = m_model.ResolveClaim(queue, tokenQueue, token.GetSequence(), resolution);
        Touch(expected.Context, expected.Thread);
        using Resolver = QueueError (ReadyQueue::*)(ReadyQueueClaimToken) noexcept;
        constexpr Resolver Resolvers[]{&ReadyQueue::CommitClaim, &ReadyQueue::CancelClaim, &ReadyQueue::RequeueClaim};
        const QueueError actual = (m_system.Queues[queue]->*Resolvers[static_cast<Size>(resolution) % 3U])(token);
        if ((actual == QueueError::None) && (expected.Slot < EntryCount))
        {
            m_claims[queue][expected.Slot] = ReadyQueueClaimToken{};
            m_retiredClaims[m_retiredNext++ % RetiredCapacity] = RetiredToken{queue, token};
            m_retiredClaimCount = Smaller(m_retiredClaimCount + 1U, RetiredCapacity);
        }
        return SameCode("claim resolution", expected.Error, actual);
    }

    [[nodiscard]] bool CallRemove(const Size queue, const Identity thread) noexcept
    {
        const QueueOutcome expected = m_model.Remove(queue, thread);
        Touch(expected.Context, expected.Thread);
        return SameCode("remove", expected.Error, m_system.Queues[queue]->Remove(ToId<ThreadId>(thread)));
    }

    [[nodiscard]] bool OpEnqueue() noexcept
    {
        const Size queue = ((m_phase == Phase::Burst) && m_random.Chance(9U, 10U)) ? 0U : PickQueue();
        const UInt8 processor = m_model.Queues[queue].Processor;
        // Usually a pair that satisfies every placement rule, so the queue fills.
        const bool smart = m_random.Chance(4U, 5U);
        const Size ready =
            !smart
                ? NoIndex
                : FindIndex(ContextCount,
                            [this, processor](const Size index) noexcept
                            {
                                const ContextRecord& record = m_model.Contexts[index];
                                const Size thread = TipThread(index);
                                return (record.ObjectValue != 0U) && (record.Queue == NoIndex) &&
                                       IsRunnable(m_model.StateOf(index)) && (thread < ThreadCount) &&
                                       m_model.ThreadIsReady(thread) && (m_model.Threads[thread].Queue == NoIndex) &&
                                       (!IsValidProcessor(processor) || PlacementAllowed(record.Settings, processor));
                            });
        if (ready != NoIndex)
        {
            return CallPlace(queue, ContextIdentity(ready), ready, ThreadIdentity(TipThread(ready)), TipThread(ready),
                             false, {});
        }
        Size context = Draw(ContextCount);
        const Size tip = TipThread(context);
        Size thread = ((tip == NoIndex) || m_random.Chance(1U, 5U)) ? Draw(ThreadCount) : tip;
        const Identity contextIdentity = Vary(m_model.Contexts[context].ObjectValue, ObjectKind::Context);
        const Identity threadIdentity = VaryThread(thread);
        context = m_random.Chance(1U, 12U) ? Draw(ContextCount) : context;
        thread = m_random.Chance(1U, 12U) ? Draw(ThreadCount) : thread;
        return CallPlace(queue, contextIdentity, context, threadIdentity, thread, false, {});
    }

    // Commits or cancels a reservation, or reserves a Blocked caller preferring
    // a context whose chain contains it, as a reply does (sometimes a caller that
    // already holds a membership).
    [[nodiscard]] bool OpReservation() noexcept
    {
        const Size queue = PickQueue();
        if ((m_phase == Phase::Burst) || m_random.Chance(9U, 17U))
        {
            const bool commit = (m_phase == Phase::Burst) || m_random.Chance(2U, 3U);
            ReadyQueueEnqueueReservation token{};
            const Size tokenQueue = PickToken(
                queue, EntryPhase::Reserved, m_reservations, m_retiredReservations, m_retiredReservationCount,
                [](const Size, const Size) noexcept
                {
                    return true;
                },
                token);
            return CallResolveReservation(queue, tokenQueue, token, commit);
        }
        const UInt64 mode = m_random.Below(20U);
        const bool member = mode >= 17U;
        const Size blocked = FindIndex(ThreadCount,
                                       [this, member](const Size index) noexcept
                                       {
                                           return (m_model.Threads[index].State == ThreadState::Blocked) &&
                                                  ((m_model.Threads[index].Queue != NoIndex) == member);
                                       });
        const Size thread = ((blocked == NoIndex) || (mode < 2U)) ? Draw(ThreadCount) : blocked;
        const UInt64 caller = m_model.Threads[thread].ObjectValue;
        const UInt8 processor = m_model.Queues[queue].Processor;
        const Size chained =
            FindIndex(ContextCount,
                      [this, caller](const Size index) noexcept
                      {
                          return (m_model.Contexts[index].Queue == NoIndex) && m_model.ChainContains(index, caller);
                      });
        const Size runnable =
            ((chained == NoIndex) && member)
                ? FindIndex(ContextCount,
                            [this, processor](const Size index) noexcept
                            {
                                return (m_model.Contexts[index].Queue == NoIndex) &&
                                       (m_model.Contexts[index].ObjectValue != 0U) &&
                                       IsRunnable(m_model.StateOf(index)) &&
                                       (!IsValidProcessor(processor) ||
                                        PlacementAllowed(m_model.Contexts[index].Settings, processor));
                            })
                : chained;
        const Size context = ((runnable == NoIndex) || (mode == 2U)) ? Draw(ContextCount) : runnable;
        const bool vary = m_random.Chance(1U, 10U);
        const Identity contextIdentity =
            vary ? Vary(m_model.Contexts[context].ObjectValue, ObjectKind::Context) : ContextIdentity(context);
        const Identity threadIdentity = vary ? VaryThread(thread) : ThreadIdentity(thread);
        const HeldBlockToken token = PickBlockToken(thread);
        return CallPlace(queue, contextIdentity, context, threadIdentity, thread, true, token);
    }

    [[nodiscard]] bool OpResolveClaim() noexcept
    {
        constexpr ClaimResolution Resolutions[]{ClaimResolution::Commit, ClaimResolution::Cancel,
                                                ClaimResolution::Requeue};
        const ClaimResolution drawn = Resolutions[m_random.Below(3U)];
        const ClaimResolution resolution = (m_phase == Phase::Drain) ? ClaimResolution::Commit : drawn;
        const Size queue = PickQueue();
        ReadyQueueClaimToken token{};
        const Size tokenQueue = PickToken(
            queue, EntryPhase::Claimed, m_claims, m_retiredClaims, m_retiredClaimCount,
            [this, resolution](const Size owner, const Size slot) noexcept
            {
                // Cancel requires the pair to be exactly as claimed.
                const EntryRecord& entry = m_model.Queues[owner].Entries[slot];
                return (resolution != ClaimResolution::Cancel) ||
                       (!m_runAdmitted[entry.Context] && m_model.PairValid(entry));
            },
            token);
        return CallResolveClaim(queue, tokenQueue, token, resolution);
    }

    // A ProcessorScheduler transaction: a rejected admission rolls back and
    // cancels; an admitted run is handed off or completed by requeue or commit.
    [[nodiscard]] bool OpDispatchCycle() noexcept
    {
        const bool claimOnly = m_random.Chance(1U, 3U);
        const Size valid = m_random.Chance(2U, 3U) ? 0U : 1U;
        const Size queue = claimOnly ? PickQueue() : valid;
        Size slot{NoIndex};
        const bool claimed = CallClaimNext(queue, slot);
        if (!claimed || (slot == NoIndex) || claimOnly)
        {
            return claimed;
        }
        const EntryRecord entry = m_model.Queues[queue].Entries[slot];
        const UInt8 processor = m_model.Queues[queue].Processor;
        const ReadyQueueClaimToken token = m_claims[queue][slot];
        ThreadError began{};
        RunOutcome started{};
        if (!CallBeginRun(entry.Thread, processor, began) ||
            ((began == ThreadError::None) &&
             !CallStartRun(entry.Context, Identity{Origin::Main, entry.ThreadValue}, processor, Now(), started)))
        {
            return false;
        }
        if ((began != ThreadError::None) || (started.Error != ContextError::None))
        {
            return ((began != ThreadError::None) || (CallFinishRun(entry.Thread, processor, ReadyDisposition, 0U) &&
                                                     CallNextReplenishment(entry.Context, Now()))) &&
                   CallResolveClaim(queue, queue, token, ClaimResolution::Cancel);
        }
        if (m_random.Chance(1U, 10U))
        {
            return CallResolveClaim(queue, queue, token, ClaimResolution::Commit);
        }
        Advance(PickRunLength(started.MaximumRun));
        UInt64 exitCode{0U};
        const UInt8 disposition = PickDisposition(exitCode);
        return CallEndRun(entry.Context, processor, Now()) &&
               CallFinishRun(entry.Thread, processor, disposition, exitCode) &&
               CallResolveClaim(queue, queue, token,
                                (disposition == ReadyDisposition) ? ClaimResolution::Requeue : ClaimResolution::Commit);
    }

    // Removal is the only exit for an entry changed behind the queue.
    [[nodiscard]] bool OpRemove() noexcept
    {
        const Size queue = PickQueue();
        const UInt64 mode = m_random.Below(100U);
        const Size poisoned =
            (mode < 25U) ? FindIndex(EntryCount,
                                     [this, queue](const Size index) noexcept
                                     {
                                         const EntryRecord& entry = m_model.Queues[queue].Entries[index];
                                         return (entry.Phase == EntryPhase::Ready) && !m_model.PairValid(entry);
                                     })
                         : NoIndex;
        const bool sameQueue = (poisoned != NoIndex) || m_random.Chance(4U, 5U);
        const Size owner = sameQueue ? queue : ((queue + 1U) % ValidQueueCount);
        const Size occupied = FindIndex(EntryCount,
                                        [this, owner](const Size index) noexcept
                                        {
                                            return m_model.Queues[owner].Entries[index].Phase != EntryPhase::Empty;
                                        });
        const Size slot = ((poisoned == NoIndex) && (mode < 60U)) ? occupied : poisoned;
        const Identity varied = (mode < 90U) ? VaryThread(Draw(ThreadCount)) : ContextIdentity(Draw(ContextCount));
        return CallRemove(queue, (slot != NoIndex)
                                     ? Identity{Origin::Main, m_model.Queues[owner].Entries[slot].ThreadValue}
                                     : varied);
    }

    [[nodiscard]] bool OpThreadRun() noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const auto mayTransition = [this](const Size index) noexcept
        {
            return MayTransition(index, false);
        };
        if ((m_phase == Phase::Burst) || (mode < 50U))
        {
            const Size thread = Prefer(
                ThreadCount, 80U,
                [this](const Size index) noexcept
                {
                    return m_model.Threads[index].State == ThreadState::Running;
                },
                mayTransition);
            UInt64 exitCode{0U};
            const UInt8 drawn = PickDisposition(exitCode);
            const UInt64 variant = m_random.Below(20U);
            const UInt8 unknown = m_random.Chance(1U, 2U) ? UInt8{0U} : UInt8{4U};
            const UInt8 running = (thread == NoIndex) ? UInt8{0U} : m_model.Threads[thread].Processor;
            const UInt8 fallback = static_cast<UInt8>(m_random.Below(MaximumProcessorCount));
            const UInt8 processor = PickProcessor(IsValidProcessor(running) ? running : fallback);
            const UInt64 code = (variant == 1U) ? (1U + m_random.Below(9U)) : exitCode;
            return (thread == NoIndex) || CallFinishRun(thread, processor, (variant == 0U) ? unknown : drawn, code);
        }
        Size queue{NoIndex};
        Size slot{NoIndex};
        const bool claimed = (mode < 70U) && FindClaim(
                                                 [this](const EntryRecord& entry) noexcept
                                                 {
                                                     return m_model.ThreadIsReady(entry.Thread);
                                                 },
                                                 queue, slot);
        const Size free = Prefer(
            ThreadCount, (mode < 90U) ? 100U : 0U,
            [this](const Size index) noexcept
            {
                return m_model.Threads[index].State == ThreadState::Ready;
            },
            mayTransition);
        const Size thread = claimed ? m_model.Queues[queue].Entries[slot].Thread : free;
        const UInt8 preferred = claimed ? m_model.Queues[queue].Processor : static_cast<UInt8>(m_random.Below(4U));
        ThreadError result{};
        return (thread == NoIndex) || CallBeginRun(thread, PickProcessor(preferred), result);
    }

    [[nodiscard]] bool OpContextRun() noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const auto mayTransition = [this](const Size index) noexcept
        {
            return MayTransition(index, true);
        };
        if (mode < 21U)
        {
            const Size context = Draw(ContextCount);
            return CallNextReplenishment(context, m_random.Chance(1U, 20U) ? (MaximumTicks - m_random.Below(2U))
                                                                           : PickSample(context));
        }
        if ((m_phase == Phase::Burst) || (mode < 58U))
        {
            const Size context = Prefer(
                ContextCount, 80U,
                [this](const Size index) noexcept
                {
                    return m_model.Contexts[index].Running;
                },
                mayTransition);
            const UInt8 running = (context == NoIndex) ? UInt8{0U} : m_model.Contexts[context].Processor;
            const UInt8 fallback = static_cast<UInt8>(m_random.Below(MaximumProcessorCount));
            const UInt8 processor = PickProcessor(IsValidProcessor(running) ? running : fallback);
            return (context == NoIndex) || CallEndRun(context, processor, PickSample(context));
        }
        Size queue{NoIndex};
        Size slot{NoIndex};
        const bool claimed = (mode < 75U) && FindClaim(
                                                 [this](const EntryRecord& entry) noexcept
                                                 {
                                                     return !m_model.Contexts[entry.Context].Running;
                                                 },
                                                 queue, slot);
        const Size free = Prefer(
            ContextCount, (mode < 92U) ? 100U : 0U,
            [this](const Size index) noexcept
            {
                return !m_model.Contexts[index].Running;
            },
            mayTransition);
        const Size context = claimed ? m_model.Queues[queue].Entries[slot].Context : free;
        if (context == NoIndex)
        {
            return true;
        }
        const UInt8 preferred = claimed ? m_model.Queues[queue].Processor : AllowedProcessor(context);
        const Identity tip{Origin::Main, m_model.Tip(context)};
        const Identity thread = ((tip.Value == 0U) || m_random.Chance(3U, 20U)) ? VaryThread(Draw(ThreadCount)) : tip;
        const UInt8 processor = PickProcessor(preferred);
        RunOutcome result{};
        return CallStartRun(context, thread, processor, PickSample(context), result);
    }

    [[nodiscard]] bool OpBinding() noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const auto mayTransition = [this](const Size index) noexcept
        {
            return MayTransition(index, true);
        };
        if (m_random.Chance(5U, 9U))
        {
            const Size context = Prefer(
                ContextCount, 70U,
                [this](const Size index) noexcept
                {
                    return m_model.Contexts[index].ChainLength == 0U;
                },
                mayTransition);
            const Size free = (mode < 50U) ? FindIndex(ThreadCount,
                                                       [this](const Size index) noexcept
                                                       {
                                                           return !IsDeadThread(index) && !IsBaseThread(index);
                                                       })
                                           : NoIndex;
            const Size chosen = ((mode >= 70U) && (mode < 85U)) ? ServerThread() : free;
            const Identity thread = (mode < 85U) ? ThreadIdentity((chosen == NoIndex) ? Draw(ThreadCount) : chosen)
                                                 : VaryThread(Draw(ThreadCount));
            return (context == NoIndex) || CallBind(context, thread);
        }
        const Size context = Prefer(
            ContextCount, (mode < 90U) ? 100U : 0U,
            [this, mode](const Size index) noexcept
            {
                const Size tip = TipThread(index);
                return (m_model.Contexts[index].ChainLength == 1U) &&
                       ((mode >= 60U) || (tip == NoIndex) || IsDeadThread(tip));
            },
            mayTransition);
        const ContextRecord& record = m_model.Contexts[(context == NoIndex) ? 0U : context];
        const bool vary = (record.ChainLength == 0U) || m_random.Chance(1U, 5U);
        return (context == NoIndex) ||
               CallUnbind(context, vary ? VaryThread(Draw(ThreadCount)) : Identity{Origin::Main, record.Chain[0U]});
    }

    // Deepening the longest chain with fresh threads reaches the depth limit.
    [[nodiscard]] bool OpDonation() noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const auto mayTransition = [this](const Size index) noexcept
        {
            return MayTransition(index, true);
        };
        if ((m_phase == Phase::Burst) || m_random.Chance(2U, 5U))
        {
            const Size reserved = (mode < 40U)
                                      ? FindIndex(ContextCount,
                                                  [this](const Size index) noexcept
                                                  {
                                                      return (m_model.PhaseOf(index, true) == EntryPhase::Reserved) &&
                                                             (m_model.Contexts[index].ChainLength > 1U);
                                                  })
                                      : NoIndex;
            const Size donated = Prefer(
                ContextCount, (mode < 80U) ? 100U : 0U,
                [this](const Size index) noexcept
                {
                    return m_model.Contexts[index].ChainLength > 1U;
                },
                mayTransition);
            const Size context = (reserved != NoIndex) ? reserved : donated;
            const Identity tip = (context == NoIndex) ? Identity{} : Identity{Origin::Main, m_model.Tip(context)};
            const bool vary = (tip.Value == 0U) || m_random.Chance(3U, 20U);
            return (context == NoIndex) || CallReturnDonation(context, vary ? VaryThread(Draw(ThreadCount)) : tip);
        }
        const bool deepen = (mode >= 35U) && (mode < 55U);
        Size context = (mode < 35U) ? FindIndex(ContextCount,
                                                [this](const Size index) noexcept
                                                {
                                                    const Size tip = TipThread(index);
                                                    return MayTransition(index, true) && (tip < ThreadCount) &&
                                                           (m_model.Threads[tip].State == ThreadState::Blocked);
                                                })
                                    : NoIndex;
        for (Size index{0U}; deepen && (index < ContextCount); ++index)
        {
            const Size longest = (context == NoIndex) ? 0U : m_model.Contexts[context].ChainLength;
            const bool longer = (context == NoIndex) || (m_model.Contexts[index].ChainLength > longest);
            context = (MayTransition(index, true) && longer) ? index : context;
        }
        context = (context != NoIndex) ? context : FindIndex(ContextCount, mayTransition);
        if (context == NoIndex)
        {
            return true;
        }
        const Identity tip{Origin::Main, m_model.Tip(context)};
        Identity caller = ((tip.Value == 0U) || m_random.Chance(3U, 20U)) ? VaryThread(Draw(ThreadCount)) : tip;
        const UInt64 serverMode = m_random.Below(100U);
        const ContextRecord& record = m_model.Contexts[context];
        Identity server = (serverMode < 85U) ? ThreadIdentity(Draw(ThreadCount)) : VaryThread(Draw(ThreadCount));
        server = (serverMode < 30U) ? ThreadIdentity(ServerThread()) : server;
        server = ((serverMode >= 30U) && (serverMode < 40U) && (record.ChainLength != 0U))
                     ? Identity{Origin::Main, record.Chain[m_random.Below(record.ChainLength) % ChainCapacity]}
                     : server;
        ContextError result{ContextError::None};
        const Size rounds = (deepen && m_random.Chance(1U, 10U)) ? ChainCapacity : 1U;
        for (Size round{0U}; (round < rounds) && (result == ContextError::None); ++round)
        {
            const Size fresh = FindIndex(ThreadCount,
                                         [this, context](const Size index) noexcept
                                         {
                                             return (m_model.Threads[index].ObjectValue != 0U) &&
                                                    !m_model.ChainContains(context, m_model.Threads[index].ObjectValue);
                                         });
            server = deepen ? ThreadIdentity((fresh == NoIndex) ? Draw(ThreadCount) : fresh) : server;
            if (!CallDonate(context, caller, server, result))
            {
                return false;
            }
            caller = Identity{Origin::Main, m_model.Tip(context)};
        }
        return true;
    }

    [[nodiscard]] bool OpLifecycle() noexcept
    {
        const UInt64 choice = m_random.Below((m_phase == Phase::Burst) ? 55U : 61U);
        const UInt64 mode = m_random.Below(100U);
        const auto mayTransition = [this](const Size index) noexcept
        {
            return MayTransition(index, false);
        };
        if (choice < 20U)
        {
            const Size thread = Prefer(
                ThreadCount, 70U,
                [this](const Size index) noexcept
                {
                    return m_model.Threads[index].State == ThreadState::Dormant;
                },
                mayTransition);
            return (thread == NoIndex) || CallMakeReady(thread);
        }
        if (choice < 55U)
        {
            const Size reserved = (mode < 35U)
                                      ? FindIndex(ThreadCount,
                                                  [this](const Size index) noexcept
                                                  {
                                                      return m_model.PhaseOf(index, false) == EntryPhase::Reserved;
                                                  })
                                      : NoIndex;
            const Size blocked = Prefer(
                ThreadCount, (mode < 80U) ? 100U : 0U,
                [this](const Size index) noexcept
                {
                    return m_model.Threads[index].State == ThreadState::Blocked;
                },
                mayTransition);
            const Size thread = (reserved != NoIndex) ? reserved : blocked;
            return (thread == NoIndex) || CallWake(thread, PickBlockToken(thread));
        }
        const Size thread = FindIndex(ThreadCount, mayTransition);
        return (thread == NoIndex) || CallTerminate(thread, m_random.Below(5U));
    }

    // Rebuilds and initializes a scratch object, or initializes another again.
    [[nodiscard]] bool OpInitialize() noexcept
    {
        const bool scratch = m_random.Chance(4U, 5U);
        const UInt64 owner = m_random.Chance(1U, 8U) ? 0U : (1U + m_random.Below(50U));
        if (m_random.Chance(1U, 3U))
        {
            const Size thread = scratch ? ScratchThread : Draw(ThreadCount);
            if (scratch && (m_model.Threads[thread].State != ThreadState::Running))
            {
                m_blockTokens[thread][0U] = HeldBlockToken{};
                m_blockTokens[thread][1U] = HeldBlockToken{};
                m_system.Threads[thread]->~Thread();
                m_system.Threads[thread] = new (m_system.ThreadStorage[thread]) Thread{};
                m_model.Threads[thread] = ThreadRecord{};
            }
            return CallInitializeThread(thread, owner);
        }
        const Size context = scratch ? ScratchContext : Draw(ContextCount);
        if (scratch && !m_model.Contexts[context].Running)
        {
            for (Size position{0U}; position < m_model.Contexts[context].ChainLength; ++position)
            {
                m_model.Unpin(m_model.Contexts[context].Chain[position]);
            }
            m_system.Contexts[context]->~SchedulingContext();
            m_system.Contexts[context] = new (m_system.ContextStorage[context]) SchedulingContext{m_system.Registry};
            m_model.Contexts[context] = ContextRecord{};
        }
        const UInt64 timeMode = m_random.Below(100U);
        const UInt64 earlier = Now() - m_random.Below(Smaller(Now(), 500U));
        const UInt64 later = Now() + m_random.Below(100U);
        UInt64 time = (timeMode < 15U) ? later : Now();
        time = ((timeMode >= 15U) && (timeMode < 30U)) ? earlier : time;
        const Configuration drawn = PickConfiguration();
        return CallInitializeContext(context, m_random.Chance(3U, 10U) ? BreakConfiguration(drawn, time) : drawn, owner,
                                     time);
    }

    [[nodiscard]] bool OpAdvance() noexcept
    {
        const UInt64 mode = m_random.Below(100U);
        const UInt64 small = m_random.Below((mode < 60U) ? 20U : 150U);
        const UInt64 large = (mode < 95U) ? (150U + m_random.Below(1000U)) : (1000U + m_random.Below(20'000U));
        Advance((mode < 85U) ? small : large);
        return true;
    }

    // A transition the policy forbids while an entry is Ready: Terminate,
    // Unbind, and Donate reject it; ReturnDonation and BeginRun, which the
    // reply and dispatch protocols use on reserved and claimed entries, leave
    // a pair that ClaimNext must skip.
    [[nodiscard]] bool OpViolation() noexcept
    {
        const Size queue = Draw(ValidQueueCount);
        const Size slot = FindSlot(queue, EntryPhase::Ready);
        if (slot == NoIndex)
        {
            return true;
        }
        const EntryRecord entry = m_model.Queues[queue].Entries[slot];
        const Identity thread{Origin::Main, entry.ThreadValue};
        const UInt64 kind = m_random.Below(5U);
        ContextError result{};
        ThreadError began{};
        switch (kind)
        {
        case 0U:
            return CallTerminate(entry.Thread, 1U + m_random.Below(9U));
        case 1U:
            return CallUnbind(entry.Context, thread);
        case 2U:
            return CallDonate(entry.Context, thread, ThreadIdentity(ServerThread()), result);
        case 3U:
            return CallReturnDonation(entry.Context, thread);
        default:
            return CallBeginRun(entry.Thread, m_model.Queues[queue].Processor, began);
        }
    }

    // Rebuilds a queue without claims; the destructor releases its entries.
    [[nodiscard]] bool OpRebuild() noexcept
    {
        const Size queue = Draw(ValidQueueCount);
        if (m_model.CountEntries(queue, EntryPhase::Claimed) != 0U)
        {
            return true;
        }
        for (EntryRecord& entry : m_model.Queues[queue].Entries)
        {
            if (entry.Phase != EntryPhase::Empty)
            {
                m_model.ClearEntry(entry);
            }
        }
        m_model.Queues[queue] = QueueRecord{QueueProcessors[queue]};
        m_system.Queues[queue]->~ReadyQueue();
        m_system.Queues[queue] =
            new (m_system.QueueStorage[queue]) ReadyQueue{m_system.Registry, CpuId{QueueProcessors[queue]}};
        return CheckObservables(true);
    }

    System& m_system;
    Model& m_model;
    Random m_random;
    UInt64 m_seed;
    Size m_step{0U};
    const char* m_operation{"initialize"};
    Phase m_phase{Phase::Mixed};
    Size m_lateEpochStep{NoIndex};
    ReadyQueueClaimToken m_claims[QueueCount][EntryCount]{};
    bool m_runAdmitted[ContextCount]{};
    ReadyQueueEnqueueReservation m_reservations[QueueCount][EntryCount]{};
    RetiredToken<ReadyQueueClaimToken> m_retiredClaims[RetiredCapacity]{};
    Size m_retiredClaimCount{0U};
    RetiredToken<ReadyQueueEnqueueReservation> m_retiredReservations[RetiredCapacity]{};
    Size m_retiredReservationCount{0U};
    Size m_retiredNext{0U};
    HeldBlockToken m_blockTokens[ThreadCount][2U]{};
    Size m_touchedContexts[TouchCapacity]{};
    Size m_touchedContextCount{0U};
    Size m_touchedThreads[TouchCapacity]{};
    Size m_touchedThreadCount{0U};
};

// Tests may not allocate; the large fixtures live in static storage.
alignas(System) unsigned char g_systemStorage[sizeof(System)];
alignas(Model) unsigned char g_modelStorage[sizeof(Model)];
alignas(Driver) unsigned char g_driverStorage[sizeof(Driver)];

void RunSeed(TestContext& context, const UInt64 seed) noexcept
{
    System* const system = new (g_systemStorage) System{};
    Model* const model = new (g_modelStorage) Model{};
    Driver* const driver = new (g_driverStorage) Driver{*system, *model, seed};
    bool matched = driver->Initialize();
    for (Size step{0U}; matched && (step < StepsPerSeed); ++step)
    {
        matched = driver->Step(step);
    }
    matched = matched && driver->Drain();
    if (!matched)
    {
        static_cast<void>(
            std::fprintf(stderr, "  reproduce with ASTERIA_TEST_SEED=%llu\n", static_cast<unsigned long long>(seed)));
    }
    ASTERIA_EXPECT(context, "ready placement, budgets, and lifecycles match the model for one seed", matched);
    driver->~Driver();
    model->~Model();
    system->~System();
}

// A registered, bound, Ready DeadlineSporadic pair for the characterizations.
class CharacterizedPair final
{
public:
    CharacterizedPair(RootCapabilityIssuer& issuer, ObjectRegistry& registry, const UInt64 period,
                      const UInt64 deadline, const UInt64 initialTicks) noexcept
        : Context{registry}
    {
        const Configuration settings{
            10U, period, deadline, 1U, 8U, SchedulingCriticality::Normal, 0x01U, SchedulingClass::DeadlineSporadic, 0U};
        static_cast<void>(ThreadStorage.Initialize(ResourceDomainId{1U}));
        static_cast<void>(Context.Initialize(settings, ResourceDomainId{1U}, MonotonicTime{initialTicks}));
        ThreadIdentity = ThreadId{registry, issuer.CreateObject(ThreadStorage).GetValue()};
        ContextIdentity = SchedulingContextId{registry, issuer.CreateObject(Context).GetValue()};
        Ready =
            (Context.Bind(ThreadIdentity) == ContextError::None) && (ThreadStorage.MakeReady() == ThreadError::None);
    }

    Thread ThreadStorage;
    SchedulingContext Context;
    ThreadId ThreadIdentity{};
    SchedulingContextId ContextIdentity{};
    bool Ready{false};
};

// EDF ranks by each context's window at the selection sample 950. The idle
// Context last refreshed its window at tick 0 (deadline 100) but is now in [900, 1000); the peer's
// current deadline is 960; the expired context's window [500, 1500) passed its deadline 600, so it
// can next run in [1500, 2500) with deadline 1600. Stale cached deadlines would order idle,
// expired, peer.
void CheckCurrentWindowDeadlineOrder(TestContext& runner) noexcept
{
    SpinLock lock;
    Asteria::TestSupport::TestObjectRegistry registry{lock};
    RootCapabilityIssuer issuer{registry};
    CharacterizedPair idle{issuer, registry, 100U, 100U, 0U};
    CharacterizedPair peer{issuer, registry, 1000U, 400U, 560U};
    CharacterizedPair expired{issuer, registry, 1000U, 100U, 500U};
    ReadyQueue queue{registry, CpuId{0U}};
    const bool placed =
        queue.Enqueue(peer.ContextIdentity, peer.Context, peer.ThreadIdentity, peer.ThreadStorage).HasValue() &&
        queue.Enqueue(expired.ContextIdentity, expired.Context, expired.ThreadIdentity, expired.ThreadStorage)
            .HasValue() &&
        queue.Enqueue(idle.ContextIdentity, idle.Context, idle.ThreadIdentity, idle.ThreadStorage).HasValue();
    const MonotonicTime sample{950U};
    const auto first = queue.ClaimNext(sample);
    const auto second = queue.ClaimNext(sample);
    const auto third = queue.ClaimNext(sample);
    ASTERIA_EXPECT(
        runner, "EDF ranks by the window at the selection sample, an expired one by its next window", idle.Ready,
        peer.Ready, expired.Ready, placed, first.HasValue(), second.HasValue(), third.HasValue(),
        &first.GetValue().GetContext() == &peer.Context, first.GetValue().GetEffectiveDeadlineTicks() == 960U,
        &second.GetValue().GetContext() == &idle.Context, second.GetValue().GetEffectiveDeadlineTicks() == 1000U,
        &third.GetValue().GetContext() == &expired.Context, third.GetValue().GetEffectiveDeadlineTicks() == 1600U,
        idle.Context.GetSnapshot().AbsoluteDeadlineTicks == 100U,
        expired.Context.GetSnapshot().AbsoluteDeadlineTicks == 600U);
    static_cast<void>(queue.CommitClaim(first.GetValue().GetToken()));
    static_cast<void>(queue.CommitClaim(second.GetValue().GetToken()));
    static_cast<void>(queue.CommitClaim(third.GetValue().GetToken()));
}

// Fixed-phase replenishment: budget 10 per period 100, deferred to the end of
// window [0, 100), stopped exactly on the boundary, and admitted again at once
// with a full budget, runs twenty ticks back to back in [90, 110).
void CheckBoundaryBurstCharacterization(TestContext& runner) noexcept
{
    SpinLock lock;
    Asteria::TestSupport::TestObjectRegistry registry{lock};
    RootCapabilityIssuer issuer{registry};
    CharacterizedPair pair{issuer, registry, 100U, 100U, 0U};
    const auto first = pair.Context.StartRun(pair.ThreadIdentity, CpuId{0U}, MonotonicTime{90U});
    const auto firstStop = pair.Context.EndRun(CpuId{0U}, MonotonicTime{100U});
    const auto second = pair.Context.StartRun(pair.ThreadIdentity, CpuId{0U}, MonotonicTime{100U});
    const auto secondStop = pair.Context.EndRun(CpuId{0U}, MonotonicTime{110U});
    ASTERIA_EXPECT(runner, "fixed-phase replenishment admits two full budgets back to back across one boundary",
                   pair.Ready, first.GetValue().MaximumRunTicks == 10U, firstStop.GetValue().ChargedTicks == 10U,
                   firstStop.GetValue().RemainingBudgetTicks == 10U, second.GetValue().MaximumRunTicks == 10U,
                   secondStop.GetValue().ChargedTicks == 10U);
}

// Budget 10 per period 100, admitted at 90 and stopped after the boundary at
// 100: a stop at 101 leaves the window [100, 200) its budget less the tick
// already spent in it, a stop at 150 has spent more than the budget there,
// and a run that covered all of [100, 200) leaves [200, 300) empty.
void CheckLateStopCharacterization(TestContext& runner) noexcept
{
    SpinLock lock;
    Asteria::TestSupport::TestObjectRegistry registry{lock};
    RootCapabilityIssuer issuer{registry};
    CharacterizedPair oneTick{issuer, registry, 100U, 100U, 0U};
    CharacterizedPair pastBudget{issuer, registry, 100U, 100U, 0U};
    CharacterizedPair wholeWindow{issuer, registry, 100U, 100U, 0U};
    const CpuId cpu{0U};

    static_cast<void>(oneTick.Context.StartRun(oneTick.ThreadIdentity, cpu, MonotonicTime{90U}));
    const auto oneTickStop = oneTick.Context.EndRun(cpu, MonotonicTime{101U});
    const auto oneTickNext = oneTick.Context.StartRun(oneTick.ThreadIdentity, cpu, MonotonicTime{101U});
    ASTERIA_EXPECT(runner, "a stop one tick after the boundary costs that window one tick", oneTick.Ready,
                   oneTickStop.GetValue().ChargedTicks == 10U, oneTickStop.GetValue().OverrunTicks == 1U,
                   oneTickStop.GetValue().RemainingBudgetTicks == 9U, oneTickStop.GetValue().BudgetExhausted == 0U,
                   oneTickNext.HasValue(), oneTickNext.GetValue().MaximumRunTicks == 9U);

    static_cast<void>(pastBudget.Context.StartRun(pastBudget.ThreadIdentity, cpu, MonotonicTime{90U}));
    const auto pastBudgetStop = pastBudget.Context.EndRun(cpu, MonotonicTime{150U});
    const auto pastBudgetEarly = pastBudget.Context.StartRun(pastBudget.ThreadIdentity, cpu, MonotonicTime{150U});
    const auto pastBudgetNext = pastBudget.Context.StartRun(pastBudget.ThreadIdentity, cpu, MonotonicTime{200U});
    ASTERIA_EXPECT(runner, "a stop past the budget of the new window empties it until the next boundary",
                   pastBudget.Ready, pastBudgetStop.GetValue().RemainingBudgetTicks == 0U,
                   pastBudgetStop.GetValue().BudgetExhausted == 1U,
                   !pastBudgetEarly.HasValue() && (pastBudgetEarly.GetError() == ContextError::BudgetUnavailable),
                   pastBudgetNext.HasValue(), pastBudgetNext.GetValue().MaximumRunTicks == 10U);

    static_cast<void>(wholeWindow.Context.StartRun(wholeWindow.ThreadIdentity, cpu, MonotonicTime{90U}));
    const auto wholeWindowStop = wholeWindow.Context.EndRun(cpu, MonotonicTime{203U});
    const auto wholeWindowEarly = wholeWindow.Context.StartRun(wholeWindow.ThreadIdentity, cpu, MonotonicTime{203U});
    const auto wholeWindowNext = wholeWindow.Context.StartRun(wholeWindow.ThreadIdentity, cpu, MonotonicTime{300U});
    ASTERIA_EXPECT(runner, "a run that covered a whole window leaves the current one empty", wholeWindow.Ready,
                   wholeWindowStop.GetValue().RemainingBudgetTicks == 0U,
                   !wholeWindowEarly.HasValue() && (wholeWindowEarly.GetError() == ContextError::BudgetUnavailable),
                   wholeWindowNext.HasValue(), wholeWindowNext.GetValue().MaximumRunTicks == 10U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ReadyQueueModelTests"};
    Asteria::Test::CheckCurrentWindowDeadlineOrder(runner);
    Asteria::Test::CheckBoundaryBurstCharacterization(runner);
    Asteria::Test::CheckLateStopCharacterization(runner);
    const Asteria::TestSupport::SeedRange seeds = Asteria::TestSupport::GetSeedRange(1U, 64U);
    for (Asteria::Foundation::UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::Test::RunSeed(runner, seed);
    }
    return runner.Finish();
}
