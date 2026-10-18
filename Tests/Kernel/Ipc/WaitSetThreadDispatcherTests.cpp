// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <optional>

#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationError;
using Kernel::Ipc::NotificationId;
using Kernel::Ipc::NotificationState;
using Kernel::Ipc::WaitSet;
using Kernel::Ipc::WaitSetError;
using Kernel::Ipc::WaitSetId;
using Kernel::Ipc::WaitSetRegistrationToken;
using Kernel::Ipc::WaitSetState;
using Kernel::Ipc::WaitSetThreadDispatcher;
using Kernel::Ipc::WaitSetThreadDispatcherError;
using Kernel::Ipc::WaitSetThreadDispatcherFailure;
using Kernel::Ipc::WaitSetThreadDispatcherGenerationCounter;
using Kernel::Ipc::WaitSetThreadDispatcherGenerationPolicy;
using Kernel::Ipc::WaitSetThreadDispatcherReceipt;
using Kernel::Ipc::WaitSetThreadDispatcherSnapshot;
using Kernel::Ipc::WaitSetThreadDispatcherState;
using Kernel::Ipc::WaitSetThreadPrepareRequest;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::BlockedRunCommitOutcome;
using Kernel::Scheduler::MaximumReadyQueueEntries;
using Kernel::Scheduler::ReadyQueueError;
using Kernel::Scheduler::ReadyQueueState;
using Kernel::Scheduler::ProcessorScheduler;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
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

constexpr Foundation::UInt64 WaitTag{0xA57E'1200'0000'0042ULL};

[[nodiscard]] constexpr SchedulingContextConfiguration Configuration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 200U,
        .PeriodTicks = 1000U,
        .RelativeDeadlineTicks = 800U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x0FU,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool FailureIs(const WaitSetThreadDispatcherFailure& failure,
                             const WaitSetThreadDispatcherError error) noexcept
{
    return failure.Error == error;
}

[[nodiscard]] bool ReceiptIs(const WaitSetThreadDispatcherReceipt& receipt, const WaitSetThreadDispatcherState state,
                             const WaitSetId waitSet, const ThreadId thread, const SchedulingContextId context,
                             const Foundation::UInt64 tag = 0U) noexcept
{
    return receipt.WaitSet.IsEqualTo(waitSet) && receipt.Thread.IsEqualTo(thread) &&
           receipt.SchedulingContext.IsEqualTo(context) && (receipt.State == state) && (receipt.Tag == tag);
}

template <Foundation::Size Capacity>
[[nodiscard]] bool ReservedIsZero(const Foundation::UInt8 (&reserved)[Capacity]) noexcept
{
    bool zero{true};
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        zero = zero && (reserved[index] == 0U);
    }
    return zero;
}

class WaitFixture final
{
private:
    SpinLock m_registryLock;
    Foundation::UInt64 m_nextTicks{1U};

public:
    WaitFixture() noexcept
        : Registry{m_registryLock}, Issuer{Registry}, Set{Registry}, Context{Registry},
          Scheduler{Registry, Clock, CpuId{0U}}
    {
    }

    ~WaitFixture() noexcept
    {
        static_cast<void>(Remove(ThreadIdentity));

        const auto contextSnapshot = Context.GetSnapshot();
        const auto threadSnapshot = ThreadStorage.GetSnapshot();
        if (contextSnapshot.State == SchedulingContextState::Running)
        {
            static_cast<void>(Context.EndRun(contextSnapshot.RunningProcessor, MonotonicTime{m_nextTicks++}));
        }
        if (threadSnapshot.State == ThreadState::Running)
        {
            static_cast<void>(
                ThreadStorage.FinishRun(threadSnapshot.RunningProcessor, ThreadRunDisposition::Exited, 1U));
        }
        else if ((threadSnapshot.State != ThreadState::Uninitialized) && (threadSnapshot.State != ThreadState::Exited))
        {
            static_cast<void>(ThreadStorage.Terminate(1U));
        }

        static_cast<void>(Context.Unbind(ThreadIdentity));
        if (Registration.IsValid())
        {
            static_cast<void>(Set.Unregister(Registration));
        }
        RetireObject(WaitSetObject);
        RetireObject(NotificationObject);
        RetireObject(ContextObject);
        RetireObject(ThreadObject);
        while (Issuer.CollectDestroyedObject().HasValue())
        {
        }
    }

    WaitFixture(const WaitFixture&) = delete;
    WaitFixture& operator=(const WaitFixture&) = delete;
    WaitFixture(WaitFixture&&) = delete;
    WaitFixture& operator=(WaitFixture&&) = delete;

    [[nodiscard]] bool Initialize() noexcept
    {
        if ((Event.Initialize(ResourceDomainId{11U}) != NotificationError::None) ||
            (Set.Initialize(ResourceDomainId{11U}) != WaitSetError::None) ||
            (ThreadStorage.Initialize(ResourceDomainId{11U}) != ThreadError::None) ||
            (Context.Initialize(Configuration(), ResourceDomainId{11U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }

        const auto notification = Issuer.CreateObject(Event);
        const auto waitSet = Issuer.CreateObject(Set);
        const auto thread = Issuer.CreateObject(ThreadStorage);
        const auto context = Issuer.CreateObject(Context);
        if (!notification.HasValue() || !waitSet.HasValue() || !thread.HasValue() || !context.HasValue())
        {
            return false;
        }
        NotificationObject = notification.GetValue();
        WaitSetObject = waitSet.GetValue();
        ThreadObject = thread.GetValue();
        ContextObject = context.GetValue();
        NotificationIdentity = NotificationId{Registry, NotificationObject};
        WaitSetIdentity = WaitSetId{Registry, WaitSetObject};
        ThreadIdentity = ThreadId{Registry, ThreadObject};
        ContextIdentity = SchedulingContextId{Registry, ContextObject};

        const auto registration = Set.RegisterNotification(NotificationIdentity, Event, WaitTag);
        if (!registration.HasValue())
        {
            return false;
        }
        Registration = registration.GetValue().Registration;
        return (Context.Bind(ThreadIdentity) == SchedulingContextError::None) &&
               (ThreadStorage.MakeReady() == ThreadError::None) && StartRun();
    }

    [[nodiscard]] bool StartRun() noexcept
    {
        if (ThreadStorage.BeginRun(CpuId{0U}) != ThreadError::None)
        {
            return false;
        }
        const auto started = Context.StartRun(ThreadIdentity, CpuId{0U}, MonotonicTime{m_nextTicks++});
        if (started.HasValue())
        {
            return true;
        }
        static_cast<void>(ThreadStorage.FinishRun(CpuId{0U}, ThreadRunDisposition::Ready));
        return false;
    }

    [[nodiscard]] ThreadBlockToken Block() noexcept
    {
        const auto context = Context.EndRun(CpuId{0U}, MonotonicTime{m_nextTicks++});
        const auto thread = ThreadStorage.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        if (!context.HasValue() || !thread.HasValue())
        {
            return ThreadBlockToken{};
        }
        return thread.GetValue().BlockToken;
    }

    [[nodiscard]] bool YieldAndStartRun() noexcept
    {
        const auto context = Context.EndRun(CpuId{0U}, MonotonicTime{m_nextTicks++});
        const auto thread = ThreadStorage.FinishRun(CpuId{0U}, ThreadRunDisposition::Ready);
        return context.HasValue() && thread.HasValue() && StartRun();
    }

    [[nodiscard]] WaitSetThreadPrepareRequest Request() const noexcept
    {
        return WaitSetThreadPrepareRequest{
            .WaitSet = WaitSetIdentity,
            .Thread = ThreadIdentity,
            .SchedulingContext = ContextIdentity,
            .DeadlineTicks = Deadline,
        };
    }

    [[nodiscard]] ReadyQueueState QueueState() const noexcept
    {
        return Scheduler.GetSnapshot().Queue;
    }

    [[nodiscard]] Foundation::Result<Kernel::Scheduler::ReadyQueueEnqueueReceipt,
                                     Kernel::Scheduler::ProcessorSchedulerFailure>
    Enqueue(const SchedulingContextId contextId, SchedulingContext& context, const ThreadId threadId,
            Thread& thread) noexcept
    {
        return Scheduler.Enqueue(contextId, context, threadId, thread);
    }

    [[nodiscard]] ReadyQueueError Remove(const ThreadId thread) noexcept
    {
        const auto removed = Scheduler.Remove(thread);
        return removed.HasValue() ? ReadyQueueError::None : removed.GetError().Queue.Error;
    }

    void RetireObject(ObjectId& object) noexcept
    {
        if (!object.IsValid())
        {
            return;
        }
        static_cast<void>(Issuer.DestroyObject(object));
        object = ObjectId{};
    }

    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    Notification Event;
    WaitSet Set;
    Thread ThreadStorage;
    SchedulingContext Context;
    ManualClock Clock{1U};
    ProcessorScheduler Scheduler;
    ObjectId NotificationObject{};
    ObjectId WaitSetObject{};
    ObjectId ThreadObject{};
    ObjectId ContextObject{};
    NotificationId NotificationIdentity{};
    WaitSetId WaitSetIdentity{};
    ThreadId ThreadIdentity{};
    SchedulingContextId ContextIdentity{};
    WaitSetRegistrationToken Registration{};
    // The deadline Request asks for, zero for none.
    Foundation::UInt64 Deadline{0U};
};

class WaitParticipant final
{
public:
    WaitParticipant(ObjectRegistry& registry, RootCapabilityIssuer& issuer, const CpuId processor) noexcept
        : Context{registry}, Scheduler{registry, Clock, processor}, m_issuer{issuer}, m_registry{registry},
          m_processor{processor}
    {
    }

    ~WaitParticipant() noexcept
    {
        static_cast<void>(Remove(ThreadIdentity));

        const auto context = Context.GetSnapshot();
        const auto thread = ThreadStorage.GetSnapshot();
        if (context.State == SchedulingContextState::Running)
        {
            static_cast<void>(Context.EndRun(context.RunningProcessor, MonotonicTime{m_nextTicks++}));
        }
        if (thread.State == ThreadState::Running)
        {
            static_cast<void>(ThreadStorage.FinishRun(thread.RunningProcessor, ThreadRunDisposition::Exited, 1U));
        }
        else if ((thread.State != ThreadState::Uninitialized) && (thread.State != ThreadState::Exited))
        {
            static_cast<void>(ThreadStorage.Terminate(1U));
        }
        static_cast<void>(Context.Unbind(ThreadIdentity));
        if (ContextObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ContextObject));
        }
        if (ThreadObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ThreadObject));
        }
        static_cast<void>(m_issuer.CollectDestroyedObject());
        static_cast<void>(m_issuer.CollectDestroyedObject());
    }

    WaitParticipant(const WaitParticipant&) = delete;
    WaitParticipant& operator=(const WaitParticipant&) = delete;
    WaitParticipant(WaitParticipant&&) = delete;
    WaitParticipant& operator=(WaitParticipant&&) = delete;

    [[nodiscard]] bool Initialize() noexcept
    {
        if ((ThreadStorage.Initialize(ResourceDomainId{41U}) != ThreadError::None) ||
            (Context.Initialize(Configuration(), ResourceDomainId{41U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }
        const auto thread = m_issuer.CreateObject(ThreadStorage);
        const auto context = m_issuer.CreateObject(Context);
        if (!thread.HasValue() || !context.HasValue())
        {
            return false;
        }
        ThreadObject = thread.GetValue();
        ContextObject = context.GetValue();
        ThreadIdentity = ThreadId{m_registry, ThreadObject};
        ContextIdentity = SchedulingContextId{m_registry, ContextObject};
        if ((Context.Bind(ThreadIdentity) != SchedulingContextError::None) ||
            (ThreadStorage.MakeReady() != ThreadError::None) ||
            (ThreadStorage.BeginRun(m_processor) != ThreadError::None))
        {
            return false;
        }
        return Context.StartRun(ThreadIdentity, m_processor, MonotonicTime{m_nextTicks++}).HasValue();
    }

    [[nodiscard]] ThreadBlockToken Block() noexcept
    {
        const auto context = Context.EndRun(m_processor, MonotonicTime{m_nextTicks++});
        const auto thread = ThreadStorage.FinishRun(m_processor, ThreadRunDisposition::Blocked);
        if (!context.HasValue() || !thread.HasValue())
        {
            return ThreadBlockToken{};
        }
        return thread.GetValue().BlockToken;
    }

    [[nodiscard]] ReadyQueueState QueueState() const noexcept
    {
        return Scheduler.GetSnapshot().Queue;
    }

    [[nodiscard]] ReadyQueueError Remove(const ThreadId thread) noexcept
    {
        const auto removed = Scheduler.Remove(thread);
        return removed.HasValue() ? ReadyQueueError::None : removed.GetError().Queue.Error;
    }

    Thread ThreadStorage;
    SchedulingContext Context;
    ManualClock Clock{1U};
    ProcessorScheduler Scheduler;
    ObjectId ThreadObject{};
    ObjectId ContextObject{};
    ThreadId ThreadIdentity{};
    SchedulingContextId ContextIdentity{};

private:
    RootCapabilityIssuer& m_issuer;
    ObjectRegistry& m_registry;
    CpuId m_processor{};
    Foundation::UInt64 m_nextTicks{1U};
};

void TestImmediateReadyAndDuplicateCompletion(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool ready = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const NotificationError signalled = fixture.Event.Signal();
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const auto duplicatePoll = dispatcher.Poll();
    const auto duplicateBlock = dispatcher.CommitBlocked({});
    const auto thread = fixture.ThreadStorage.GetSnapshot();
    const auto notification = fixture.Event.GetSnapshot();
    const auto waitSet = fixture.Set.GetSnapshot();

    ASTERIA_EXPECT(runner, "readiness present before Prepare is consumed without blocking", ready,
                   signalled == NotificationError::None, prepared.HasValue(),
                   ReceiptIs(prepared.GetValue(), WaitSetThreadDispatcherState::Completed, fixture.WaitSetIdentity,
                             fixture.ThreadIdentity, fixture.ContextIdentity, WaitTag),
                   prepared.GetValue().RegistrationSequence == fixture.Registration.GetSequence(),
                   prepared.GetValue().RunSequence == thread.RunSequence, prepared.GetValue().BlockSequence == 0U,
                   prepared.GetValue().Notification.IsEqualTo(fixture.NotificationIdentity),
                   prepared.GetValue().QueueSequence == 0U, ReservedIsZero(prepared.GetValue().Reserved),
                   thread.State == ThreadState::Running, fixture.QueueState().ReadyEntries == 0U,
                   notification.State == NotificationState::Ready, notification.Pending == 0U,
                   waitSet.State == WaitSetState::Ready, waitSet.ClaimedEntries == 0U);
    ASTERIA_EXPECT(runner, "completed immediate readiness cannot be polled or woken twice", !duplicatePoll.HasValue(),
                   FailureIs(duplicatePoll.GetError(), WaitSetThreadDispatcherError::AlreadyCompleted),
                   !duplicateBlock.HasValue(),
                   FailureIs(duplicateBlock.GetError(), WaitSetThreadDispatcherError::AlreadyCompleted),
                   dispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Completed,
                   fixture.QueueState().ReadyEntries == 0U);
}

void TestSignalBeforeBlockCommit(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool ready = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const NotificationError signalled = fixture.Event.Signal();
    const ThreadBlockToken blocked = fixture.Block();
    const BlockedRunCommitOutcome outcome = dispatcher.CommitBlockedFromFinishedRun(blocked);
    const auto completed = dispatcher.ConsumeCompletion();
    const auto thread = fixture.ThreadStorage.GetSnapshot();

    ASTERIA_EXPECT(runner, "post-Finish commitment closes the race and consumes completion", ready, prepared.HasValue(),
                   prepared.GetValue().State == WaitSetThreadDispatcherState::Prepared,
                   signalled == NotificationError::None, blocked.IsValid(),
                   outcome == BlockedRunCommitOutcome::Accepted, completed.HasValue(),
                   ReceiptIs(completed.GetValue(), WaitSetThreadDispatcherState::Completed, fixture.WaitSetIdentity,
                             fixture.ThreadIdentity, fixture.ContextIdentity, WaitTag),
                   completed.GetValue().QueueSequence != 0U, thread.State == ThreadState::Ready,
                   fixture.QueueState().ReadyEntries == 1U, fixture.QueueState().ReservedEntries == 0U,
                   fixture.Set.GetSnapshot().ClaimedEntries == 0U, fixture.Event.GetSnapshot().Pending == 0U);
}

void TestCompletionConsumeAndReuse(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const NotificationError firstSignal = fixture.Event.Signal();
    const auto firstPrepared = dispatcher.Prepare(fixture.Request());
    const auto firstConsumed = firstPrepared.HasValue() ? dispatcher.ConsumeCompletion(firstPrepared.GetValue())
                                                        : dispatcher.ConsumeCompletion();
    const auto reset = dispatcher.GetSnapshot();
    const auto duplicate = dispatcher.ConsumeCompletion();
    const NotificationError secondSignal = fixture.Event.Signal();
    const auto secondPrepared = dispatcher.Prepare(fixture.Request());
    const auto secondConsumed = secondPrepared.HasValue() ? dispatcher.ConsumeCompletion(secondPrepared.GetValue())
                                                          : dispatcher.ConsumeCompletion();

    ASTERIA_EXPECT(runner, "completion consumes exactly once and resets the dispatcher for reuse", initialized,
                   firstSignal == NotificationError::None, firstPrepared.HasValue(), firstConsumed.HasValue(),
                   ReceiptIs(firstConsumed.GetValue(), WaitSetThreadDispatcherState::Completed, fixture.WaitSetIdentity,
                             fixture.ThreadIdentity, fixture.ContextIdentity, WaitTag),
                   reset.State == WaitSetThreadDispatcherState::Idle, !reset.WaitSet.IsValid(), !reset.Thread.IsValid(),
                   !reset.SchedulingContext.IsValid(), reset.RetainedClaimSequence == 0U,
                   reset.RetainedQueueReservationSequence == 0U, ReservedIsZero(reset.Reserved), !duplicate.HasValue(),
                   FailureIs(duplicate.GetError(), WaitSetThreadDispatcherError::CompletionUnavailable),
                   secondSignal == NotificationError::None, secondPrepared.HasValue(), secondConsumed.HasValue(),
                   secondConsumed.GetValue().RegistrationSequence == fixture.Registration.GetSequence(),
                   ReservedIsZero(secondConsumed.GetValue().Reserved));
}

void TestPreparationReceiptInvariantRejection(TestContext& runner) noexcept
{
    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const WaitSetThreadDispatcherFailure rejected = prepared.HasValue()
                                                            ? dispatcher.RejectReceiptInvariant(prepared.GetValue())
                                                            : WaitSetThreadDispatcherFailure{};
        const WaitSetThreadDispatcherSnapshot snapshot = dispatcher.GetSnapshot();
        const auto repeated = dispatcher.ConsumeCompletion();
        const NotificationError signalled = fixture.Event.Signal();
        const auto reused = dispatcher.Prepare(fixture.Request());
        const auto consumed =
            reused.HasValue() ? dispatcher.ConsumeCompletion(reused.GetValue()) : dispatcher.ConsumeCompletion();

        ASTERIA_EXPECT(
            runner,
            "rejecting an exact Prepared receipt rolls back all pins and "
            "permits exact-once reuse",
            initialized, prepared.HasValue(), prepared.GetValue().State == WaitSetThreadDispatcherState::Prepared,
            FailureIs(rejected, WaitSetThreadDispatcherError::ReceiptInvariantRejected),
            snapshot.State == WaitSetThreadDispatcherState::Idle, !snapshot.WaitSet.IsValid(),
            !snapshot.Thread.IsValid(), !snapshot.SchedulingContext.IsValid(), snapshot.RetainedClaimSequence == 0U,
            snapshot.RetainedQueueReservationSequence == 0U, !repeated.HasValue(),
            FailureIs(repeated.GetError(), WaitSetThreadDispatcherError::CompletionUnavailable),
            signalled == NotificationError::None, reused.HasValue(), consumed.HasValue(),
            dispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
    }

    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const NotificationError signalled = fixture.Event.Signal();
        const auto completed = dispatcher.Prepare(fixture.Request());
        const WaitSetThreadDispatcherFailure rejected = completed.HasValue()
                                                            ? dispatcher.RejectReceiptInvariant(completed.GetValue())
                                                            : WaitSetThreadDispatcherFailure{};
        const WaitSetThreadDispatcherSnapshot snapshot = dispatcher.GetSnapshot();
        const auto repeated =
            completed.HasValue() ? dispatcher.ConsumeCompletion(completed.GetValue()) : dispatcher.ConsumeCompletion();

        ASTERIA_EXPECT(runner,
                       "rejecting a Completed Prepare receipt is terminal after "
                       "readiness consumption and keeps its evidence",
                       initialized, signalled == NotificationError::None, completed.HasValue(),
                       completed.GetValue().State == WaitSetThreadDispatcherState::Completed,
                       FailureIs(rejected, WaitSetThreadDispatcherError::ReceiptInvariantRejected),
                       snapshot.State == WaitSetThreadDispatcherState::Faulted,
                       snapshot.WaitSet.IsEqualTo(fixture.WaitSetIdentity),
                       snapshot.Notification.IsEqualTo(fixture.NotificationIdentity),
                       snapshot.RegistrationSequence == fixture.Registration.GetSequence(), snapshot.Tag == WaitTag,
                       !repeated.HasValue(),
                       FailureIs(repeated.GetError(), WaitSetThreadDispatcherError::ReceiptInvariantRejected));
    }

    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        WaitSetThreadDispatcherReceipt foreign{};
        if (prepared.HasValue())
        {
            foreign = prepared.GetValue();
            ++foreign.RunSequence;
        }
        const WaitSetThreadDispatcherFailure rejected = dispatcher.RejectReceiptInvariant(foreign);
        const WaitSetThreadDispatcherSnapshot snapshot = dispatcher.GetSnapshot();

        ASTERIA_EXPECT(runner,
                       "a foreign Prepared report cannot cancel the current generation "
                       "and faults with its evidence intact",
                       initialized, prepared.HasValue(),
                       FailureIs(rejected, WaitSetThreadDispatcherError::ReceiptInvariantRejected),
                       snapshot.State == WaitSetThreadDispatcherState::Faulted,
                       snapshot.WaitSet.IsEqualTo(fixture.WaitSetIdentity),
                       snapshot.RunSequence == prepared.GetValue().RunSequence, snapshot.RetainedClaimSequence == 0U,
                       snapshot.RetainedQueueReservationSequence == 0U);
    }
}

void TestAtomicCompletionReceiptMismatch(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const NotificationError signalled = fixture.Event.Signal();
    const auto completed = dispatcher.Prepare(fixture.Request());
    WaitSetThreadDispatcherReceipt foreign{};
    if (completed.HasValue())
    {
        foreign = completed.GetValue();
        ++foreign.RegistrationSequence;
    }
    const auto rejected = completed.HasValue() ? dispatcher.ConsumeCompletion(foreign) : dispatcher.ConsumeCompletion();
    const WaitSetThreadDispatcherSnapshot snapshot = dispatcher.GetSnapshot();
    const auto repeated =
        completed.HasValue() ? dispatcher.ConsumeCompletion(completed.GetValue()) : dispatcher.ConsumeCompletion();

    ASTERIA_EXPECT(
        runner,
        "an exact-consume receipt mismatch faults before reset and never "
        "loses completed evidence",
        initialized, signalled == NotificationError::None, completed.HasValue(), !rejected.HasValue(),
        FailureIs(rejected.GetError(), WaitSetThreadDispatcherError::CompletionReceiptMismatch),
        snapshot.State == WaitSetThreadDispatcherState::Faulted, snapshot.WaitSet.IsEqualTo(fixture.WaitSetIdentity),
        snapshot.Notification.IsEqualTo(fixture.NotificationIdentity),
        snapshot.RegistrationSequence == fixture.Registration.GetSequence(), snapshot.Tag == WaitTag,
        !repeated.HasValue(), FailureIs(repeated.GetError(), WaitSetThreadDispatcherError::CompletionReceiptMismatch));
}

void TestStaleTransactionReceiptsCannotReplay(TestContext& runner) noexcept
{
    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto first = dispatcher.Prepare(fixture.Request());
        const WaitSetThreadDispatcherFailure firstRejected =
            first.HasValue() ? dispatcher.RejectReceiptInvariant(first.GetValue()) : WaitSetThreadDispatcherFailure{};
        const auto current = dispatcher.Prepare(fixture.Request());
        const WaitSetThreadDispatcherFailure staleRejected =
            first.HasValue() ? dispatcher.RejectReceiptInvariant(first.GetValue()) : WaitSetThreadDispatcherFailure{};
        const WaitSetThreadDispatcherSnapshot snapshot = dispatcher.GetSnapshot();

        ASTERIA_EXPECT(runner, "a stale Prepared receipt cannot reject a later transaction", initialized,
                       first.HasValue(), current.HasValue(),
                       FailureIs(firstRejected, WaitSetThreadDispatcherError::ReceiptInvariantRejected),
                       FailureIs(staleRejected, WaitSetThreadDispatcherError::ReceiptInvariantRejected),
                       first.GetValue().Dispatcher == &dispatcher, current.GetValue().Dispatcher == &dispatcher,
                       first.GetValue().TransactionGeneration == 1U, current.GetValue().TransactionGeneration == 2U,
                       snapshot.State == WaitSetThreadDispatcherState::Faulted, snapshot.Dispatcher == &dispatcher,
                       snapshot.TransactionGeneration == current.GetValue().TransactionGeneration,
                       snapshot.WaitSet.IsEqualTo(fixture.WaitSetIdentity));
    }

    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const NotificationError firstSignal = fixture.Event.Signal();
        const auto first = dispatcher.Prepare(fixture.Request());
        const auto firstConsumed =
            first.HasValue() ? dispatcher.ConsumeCompletion(first.GetValue()) : dispatcher.ConsumeCompletion();
        const NotificationError secondSignal = fixture.Event.Signal();
        const auto current = dispatcher.Prepare(fixture.Request());
        const auto staleConsumed =
            first.HasValue() ? dispatcher.ConsumeCompletion(first.GetValue()) : dispatcher.ConsumeCompletion();
        const WaitSetThreadDispatcherSnapshot snapshot = dispatcher.GetSnapshot();

        ASTERIA_EXPECT(runner, "a stale Completed receipt cannot consume a later completion", initialized,
                       firstSignal == NotificationError::None, first.HasValue(), firstConsumed.HasValue(),
                       secondSignal == NotificationError::None, current.HasValue(), !staleConsumed.HasValue(),
                       FailureIs(staleConsumed.GetError(), WaitSetThreadDispatcherError::CompletionReceiptMismatch),
                       first.GetValue().TransactionGeneration == 1U, current.GetValue().TransactionGeneration == 2U,
                       snapshot.State == WaitSetThreadDispatcherState::Faulted,
                       snapshot.TransactionGeneration == current.GetValue().TransactionGeneration,
                       snapshot.Notification.IsEqualTo(fixture.NotificationIdentity));
    }
}

void TestCrossDispatcherReceiptsCannotSubstitute(TestContext& runner) noexcept
{
    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher primary{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        WaitSetThreadDispatcher secondary{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto primaryPrepared = primary.Prepare(fixture.Request());
        const auto secondaryPrepared = secondary.Prepare(fixture.Request());
        const WaitSetThreadDispatcherFailure substituted =
            primaryPrepared.HasValue() ? secondary.RejectReceiptInvariant(primaryPrepared.GetValue())
                                       : WaitSetThreadDispatcherFailure{};
        const WaitSetThreadDispatcherSnapshot primarySnapshot = primary.GetSnapshot();
        const WaitSetThreadDispatcherSnapshot secondarySnapshot = secondary.GetSnapshot();

        ASTERIA_EXPECT(runner, "an otherwise identical Prepared receipt cannot cross dispatchers", initialized,
                       primaryPrepared.HasValue(), secondaryPrepared.HasValue(),
                       primaryPrepared.GetValue().TransactionGeneration == 1U,
                       secondaryPrepared.GetValue().TransactionGeneration == 1U,
                       primaryPrepared.GetValue().Dispatcher == &primary,
                       secondaryPrepared.GetValue().Dispatcher == &secondary,
                       FailureIs(substituted, WaitSetThreadDispatcherError::ReceiptInvariantRejected),
                       primarySnapshot.State == WaitSetThreadDispatcherState::Prepared,
                       secondarySnapshot.State == WaitSetThreadDispatcherState::Faulted,
                       secondarySnapshot.TransactionGeneration == secondaryPrepared.GetValue().TransactionGeneration);
    }

    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher primary{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        WaitSetThreadDispatcher secondary{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const NotificationError primarySignal = fixture.Event.Signal();
        const auto primaryCompleted = primary.Prepare(fixture.Request());
        const NotificationError secondarySignal = fixture.Event.Signal();
        const auto secondaryCompleted = secondary.Prepare(fixture.Request());
        const auto substituted = primaryCompleted.HasValue() ? secondary.ConsumeCompletion(primaryCompleted.GetValue())
                                                             : secondary.ConsumeCompletion();
        const WaitSetThreadDispatcherSnapshot secondarySnapshot = secondary.GetSnapshot();
        const auto primaryConsumed = primaryCompleted.HasValue()
                                         ? primary.ConsumeCompletion(primaryCompleted.GetValue())
                                         : primary.ConsumeCompletion();

        ASTERIA_EXPECT(runner,
                       "an otherwise identical Completed receipt cannot cross "
                       "dispatchers",
                       initialized, primarySignal == NotificationError::None, primaryCompleted.HasValue(),
                       secondarySignal == NotificationError::None, secondaryCompleted.HasValue(),
                       primaryCompleted.GetValue().TransactionGeneration == 1U,
                       secondaryCompleted.GetValue().TransactionGeneration == 1U, !substituted.HasValue(),
                       FailureIs(substituted.GetError(), WaitSetThreadDispatcherError::CompletionReceiptMismatch),
                       secondarySnapshot.State == WaitSetThreadDispatcherState::Faulted,
                       secondarySnapshot.TransactionGeneration == 1U, primaryConsumed.HasValue());
    }
}

void TestTransactionGenerationExhaustion(TestContext& runner) noexcept
{
    Foundation::UInt64 next{0xA5A5U};
    const bool reachesMaximum = WaitSetThreadDispatcherGenerationPolicy::TryAdvance(
        WaitSetThreadDispatcherGenerationPolicy::Maximum - 1U, next);
    const bool rejectsWrap =
        !WaitSetThreadDispatcherGenerationPolicy::TryAdvance(WaitSetThreadDispatcherGenerationPolicy::Maximum, next);

    // The dispatcher checks CanIssue before pinning, issues after
    // correlation, and restores a copy when a failed Prepare returns to Idle.
    WaitSetThreadDispatcherGenerationCounter counter{WaitSetThreadDispatcherGenerationPolicy::Maximum - 1U};
    const WaitSetThreadDispatcherGenerationCounter beforeMaximum = counter;
    const bool admitsMaximum = counter.CanIssue();
    Foundation::UInt64 maximum{0U};
    const bool issuedMaximum = counter.TryIssue(maximum);
    WaitSetThreadDispatcherGenerationCounter withdrawn = beforeMaximum;
    Foundation::UInt64 reissued{0U};
    const bool reissuedMaximum = withdrawn.TryIssue(reissued);
    const bool admitsAfterMaximum = counter.CanIssue();
    Foundation::UInt64 exhausted{0xA5A5U};
    const bool issuedAfterMaximum = counter.TryIssue(exhausted);

    ASTERIA_EXPECT(runner,
                   "transaction-generation policy reaches UInt64 maximum and rejects "
                   "wrap",
                   reachesMaximum, rejectsWrap, next == WaitSetThreadDispatcherGenerationPolicy::Maximum);
    ASTERIA_EXPECT(runner,
                   "the generation counter issues maximum once, reissues a withdrawn "
                   "generation, and then rejects every later issue without mutation",
                   admitsMaximum, issuedMaximum, maximum == WaitSetThreadDispatcherGenerationPolicy::Maximum,
                   reissuedMaximum, reissued == WaitSetThreadDispatcherGenerationPolicy::Maximum, !admitsAfterMaximum,
                   !issuedAfterMaximum, exhausted == 0xA5A5U,
                   counter.GetLastIssued() == WaitSetThreadDispatcherGenerationPolicy::Maximum);
}

void TestTerminalPostFinishRejection(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const ThreadBlockToken blocked = fixture.Block();
    const BlockedRunCommitOutcome rejected = dispatcher.CommitBlockedFromFinishedRun({});
    const auto faulted = dispatcher.GetSnapshot();
    const BlockedRunCommitOutcome repeated = dispatcher.CommitBlockedFromFinishedRun(blocked);

    ASTERIA_EXPECT(runner, "a rejected post-Finish block token is terminal and never dispatchable", initialized,
                   prepared.HasValue(), blocked.IsValid(), rejected == BlockedRunCommitOutcome::TerminalRejected,
                   repeated == BlockedRunCommitOutcome::TerminalRejected,
                   faulted.State == WaitSetThreadDispatcherState::Faulted,
                   FailureIs(faulted.Diagnostic, WaitSetThreadDispatcherError::InvalidBlockToken),
                   faulted.RetainedClaimSequence == 0U, faulted.RetainedQueueReservationSequence == 0U,
                   fixture.ThreadStorage.GetSnapshot().State == ThreadState::Blocked);
}

void TestSignalAfterBlockAndNoSignal(TestContext& runner) noexcept
{
    {
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const NotificationError signalled = fixture.Event.Signal();
        const auto polled = dispatcher.Poll();

        ASTERIA_EXPECT(runner, "Poll wakes an exact blocked generation after a later signal", ready,
                       prepared.HasValue(), blocked.IsValid(), committed.HasValue(),
                       committed.GetValue().State == WaitSetThreadDispatcherState::Blocked,
                       signalled == NotificationError::None, polled.HasValue(),
                       ReceiptIs(polled.GetValue(), WaitSetThreadDispatcherState::Completed, fixture.WaitSetIdentity,
                                 fixture.ThreadIdentity, fixture.ContextIdentity, WaitTag),
                       fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                       fixture.QueueState().ReadyEntries == 1U);
    }

    {
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const auto firstPoll = dispatcher.Poll();
        const auto secondPoll = dispatcher.Poll();
        const auto thread = fixture.ThreadStorage.GetSnapshot();

        ASTERIA_EXPECT(runner, "repeated Poll without a signal preserves one blocked transaction", ready,
                       prepared.HasValue(), committed.HasValue(), firstPoll.HasValue(), secondPoll.HasValue(),
                       committed.GetValue().State == WaitSetThreadDispatcherState::Blocked,
                       firstPoll.GetValue().State == WaitSetThreadDispatcherState::Blocked,
                       secondPoll.GetValue().State == WaitSetThreadDispatcherState::Blocked,
                       thread.State == ThreadState::Blocked, thread.BlockToken.IsEqualTo(blocked),
                       fixture.QueueState().ReadyEntries == 0U, fixture.QueueState().ReservedEntries == 0U,
                       fixture.Set.GetSnapshot().State == WaitSetState::Ready,
                       fixture.Event.GetSnapshot().Pending == 0U);
    }
}

// Domain teardown: a blocked wait whose deadline passed with nothing ready is woken as timed out;
// readiness found first wins.
void TestExpiry(TestContext& runner) noexcept
{
    {
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        fixture.Deadline = 500U;
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const auto early = dispatcher.Expire(499U);
        const auto stillBlocked = fixture.ThreadStorage.GetSnapshot().State;
        const auto expired = dispatcher.Expire(500U);
        const auto snapshot = dispatcher.GetSnapshot();
        const auto again = dispatcher.Expire(600U);
        const auto consumed = dispatcher.ConsumeCompletion();
        ASTERIA_EXPECT(runner, "Expire wakes a blocked wait at its deadline as timed out, and not before", ready,
                       prepared.HasValue(), committed.HasValue(), dispatcher.GetDeadline() == 0U, early.HasValue(),
                       early.GetValue().State == WaitSetThreadDispatcherState::Blocked, !early.GetValue().TimedOut,
                       stillBlocked == ThreadState::Blocked, expired.HasValue(),
                       expired.GetValue().State == WaitSetThreadDispatcherState::Completed, expired.GetValue().TimedOut,
                       !expired.GetValue().Notification.IsValid(), expired.GetValue().Tag == 0U,
                       expired.GetValue().RegistrationSequence == 0U, expired.GetValue().QueueSequence != 0U,
                       snapshot.TimedOut, snapshot.State == WaitSetThreadDispatcherState::Completed, again.HasValue(),
                       again.GetValue().State == WaitSetThreadDispatcherState::Completed,
                       fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                       fixture.QueueState().ReadyEntries == 1U, consumed.HasValue(),
                       consumed.HasValue() && consumed.GetValue().TimedOut,
                       dispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle,
                       !dispatcher.GetSnapshot().TimedOut, fixture.Event.GetSnapshot().Pending == 0U);
    }

    {
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        fixture.Deadline = 500U;
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const Foundation::UInt64 deadline = dispatcher.GetDeadline();
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const NotificationError signalled = fixture.Event.Signal();
        const auto polled = dispatcher.Poll();
        const auto expired = dispatcher.Expire(1000U);
        ASTERIA_EXPECT(runner, "readiness found by Poll wins over a passed deadline", ready, prepared.HasValue(),
                       deadline == 500U, committed.HasValue(), signalled == NotificationError::None, polled.HasValue(),
                       ReceiptIs(polled.GetValue(), WaitSetThreadDispatcherState::Completed, fixture.WaitSetIdentity,
                                 fixture.ThreadIdentity, fixture.ContextIdentity, WaitTag),
                       !polled.GetValue().TimedOut, expired.HasValue(), !expired.GetValue().TimedOut,
                       fixture.QueueState().ReadyEntries == 1U);
    }

    {
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const auto expired = dispatcher.Expire(~Foundation::UInt64{0U} - 1U);
        ASTERIA_EXPECT(runner, "a wait without a deadline never expires", ready, prepared.HasValue(),
                       committed.HasValue(), expired.HasValue(),
                       expired.GetValue().State == WaitSetThreadDispatcherState::Blocked,
                       fixture.ThreadStorage.GetSnapshot().State == ThreadState::Blocked);
    }
}

// Service IPC: a kill wakes a blocked waiter as an expiry would, marked Aborted; anything but a
// Blocked transaction is left alone.
void TestAbortAndReset(TestContext& runner) noexcept
{
    {
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto idle = dispatcher.Abort();
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const auto refused = dispatcher.Reset();
        const auto stillBlocked = dispatcher.GetSnapshot().State;
        const auto aborted = dispatcher.Abort();
        const auto snapshot = dispatcher.GetSnapshot();
        const auto again = dispatcher.Abort();
        const auto consumed = dispatcher.ConsumeCompletion();
        ASTERIA_EXPECT(
            runner, "Abort wakes a blocked wait without a deadline as timed out and aborted", ready, idle.HasValue(),
            idle.GetValue().State == WaitSetThreadDispatcherState::Idle, !idle.GetValue().Aborted, prepared.HasValue(),
            committed.HasValue(), !refused.HasValue(),
            refused.GetError().Error == WaitSetThreadDispatcherError::InvalidState,
            stillBlocked == WaitSetThreadDispatcherState::Blocked, aborted.HasValue(),
            aborted.GetValue().State == WaitSetThreadDispatcherState::Completed, aborted.GetValue().TimedOut,
            aborted.GetValue().Aborted, !aborted.GetValue().Notification.IsValid(), aborted.GetValue().Tag == 0U,
            aborted.GetValue().RegistrationSequence == 0U, snapshot.Aborted, snapshot.TimedOut, again.HasValue(),
            again.GetValue().State == WaitSetThreadDispatcherState::Completed,
            fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready, fixture.QueueState().ReadyEntries == 1U,
            consumed.HasValue(), consumed.HasValue() && consumed.GetValue().Aborted,
            dispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle, !dispatcher.GetSnapshot().Aborted,
            !dispatcher.GetSnapshot().TimedOut);
    }

    {
        // Woken by readiness before the kill: Abort changes nothing, and a
        // reaper that never resumes the thread resets the dispatcher.
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const NotificationError signalled = fixture.Event.Signal();
        const auto polled = dispatcher.Poll();
        const auto aborted = dispatcher.Abort();
        const auto reset = dispatcher.Reset();
        const auto idle = dispatcher.Reset();
        ASTERIA_EXPECT(runner, "a wait woken before the kill is not aborted, and Reset returns it to Idle", ready,
                       prepared.HasValue(), committed.HasValue(), signalled == NotificationError::None,
                       polled.HasValue(), polled.GetValue().State == WaitSetThreadDispatcherState::Completed,
                       aborted.HasValue(), !aborted.GetValue().Aborted, !aborted.GetValue().TimedOut,
                       aborted.GetValue().Tag == WaitTag, reset.HasValue(), !reset.GetValue().Aborted,
                       dispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle, idle.HasValue(),
                       idle.GetValue().State == WaitSetThreadDispatcherState::Idle);
    }

    {
        // A timed wait (a driver's backstop) killed before its deadline is
        // Aborted, and its deadline passing afterwards wakes nothing again.
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        fixture.Deadline = 500U;
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const auto aborted = dispatcher.Abort();
        const auto expired = dispatcher.Expire(500U);
        const auto consumed = dispatcher.ConsumeCompletion();
        ASTERIA_EXPECT(runner, "Abort wakes a timed wait before its deadline, which then wakes nothing", ready,
                       prepared.HasValue(), committed.HasValue(), aborted.HasValue(),
                       aborted.GetValue().State == WaitSetThreadDispatcherState::Completed, aborted.GetValue().Aborted,
                       aborted.GetValue().TimedOut, expired.HasValue(),
                       expired.GetValue().State == WaitSetThreadDispatcherState::Completed, expired.GetValue().Aborted,
                       fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                       fixture.QueueState().ReadyEntries == 1U, consumed.HasValue(),
                       consumed.HasValue() && consumed.GetValue().Aborted);
    }

    {
        // Its deadline passed first: the wait completed as timed out and the
        // thread is ready to run, so a kill finds nothing to abort (the
        // thread table then takes the thread off the run queue instead).
        WaitFixture fixture;
        const bool ready = fixture.Initialize();
        fixture.Deadline = 500U;
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const ThreadBlockToken blocked = fixture.Block();
        const auto committed = dispatcher.CommitBlocked(blocked);
        const auto expired = dispatcher.Expire(500U);
        const auto aborted = dispatcher.Abort();
        const auto consumed = dispatcher.ConsumeCompletion();
        ASTERIA_EXPECT(runner, "a timed wait that expired before the kill is not aborted", ready, prepared.HasValue(),
                       committed.HasValue(), expired.HasValue(), expired.GetValue().TimedOut,
                       !expired.GetValue().Aborted, aborted.HasValue(), !aborted.GetValue().Aborted,
                       aborted.GetValue().TimedOut, fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                       fixture.QueueState().ReadyEntries == 1U, consumed.HasValue(),
                       consumed.HasValue() && !consumed.GetValue().Aborted);
    }
}

void TestCompetingClaimCancelAndRetry(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    const NotificationError signalled = fixture.Event.Signal();
    const auto held = fixture.Set.ClaimReady();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const ThreadBlockToken blocked = fixture.Block();
    const auto committed = dispatcher.CommitBlocked(blocked);
    const auto blockedSnapshot = dispatcher.GetSnapshot();
    const auto waitSetWhileHeld = fixture.Set.GetSnapshot();
    const bool cancelled = held.HasValue() && fixture.Set.CancelClaim(held.GetValue().Claim).HasValue();
    const auto retried = dispatcher.Poll();

    ASTERIA_EXPECT(runner, "a direct-held WaitSet claim defers Prepare and CommitBlocked", initialized,
                   signalled == NotificationError::None, held.HasValue(), prepared.HasValue(),
                   prepared.GetValue().State == WaitSetThreadDispatcherState::Prepared, blocked.IsValid(),
                   committed.HasValue(), committed.GetValue().State == WaitSetThreadDispatcherState::Blocked,
                   blockedSnapshot.State == WaitSetThreadDispatcherState::Blocked,
                   blockedSnapshot.Diagnostic.Error == WaitSetThreadDispatcherError::None,
                   waitSetWhileHeld.State == WaitSetState::Claimed);
    ASTERIA_EXPECT(
        runner, "cancelling the competing claim republishes readiness for Poll retry", cancelled, retried.HasValue(),
        ReceiptIs(retried.GetValue(), WaitSetThreadDispatcherState::Completed, fixture.WaitSetIdentity,
                  fixture.ThreadIdentity, fixture.ContextIdentity, WaitTag),
        retried.GetValue().BlockSequence == blocked.GetValue(),
        fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready, fixture.QueueState().ReadyEntries == 1U,
        fixture.Set.GetSnapshot().ClaimedEntries == 0U, fixture.Event.GetSnapshot().Pending == 0U);
}

void TestTwoDispatchersShareWaitSet(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool primaryReady = fixture.Initialize();
    WaitParticipant peer{fixture.Registry, fixture.Issuer, CpuId{1U}};
    const bool peerReady = peer.Initialize();
    WaitSetThreadDispatcher primary{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    WaitSetThreadDispatcher secondary{fixture.Registry, peer.Scheduler.GetWakeFacade()};
    const NotificationError heldSignal = fixture.Event.Signal();
    const auto held = fixture.Set.ClaimReady();
    const WaitSetThreadPrepareRequest peerRequest{
        .WaitSet = fixture.WaitSetIdentity,
        .Thread = peer.ThreadIdentity,
        .SchedulingContext = peer.ContextIdentity,
        .DeadlineTicks = 0U,
    };
    const auto primaryPrepared = primary.Prepare(fixture.Request());
    const auto secondaryPrepared = secondary.Prepare(peerRequest);
    const ThreadBlockToken primaryBlock = fixture.Block();
    const ThreadBlockToken secondaryBlock = peer.Block();
    const bool cancelled = held.HasValue() && fixture.Set.CancelClaim(held.GetValue().Claim).HasValue();
    const auto primaryCommitted = primary.CommitBlocked(primaryBlock);
    const auto secondaryCommitted = secondary.CommitBlocked(secondaryBlock);
    const auto primaryQueueAfterFirst = fixture.QueueState();
    const auto peerQueueAfterFirst = peer.QueueState();
    const NotificationError secondSignal = fixture.Event.Signal();
    const auto secondaryPolled = secondary.Poll();

    ASTERIA_EXPECT(
        runner, "two dispatchers may wait on one WaitSet without exclusive ownership", primaryReady, peerReady,
        heldSignal == NotificationError::None, held.HasValue(), primaryPrepared.HasValue(),
        primaryPrepared.GetValue().State == WaitSetThreadDispatcherState::Prepared, secondaryPrepared.HasValue(),
        secondaryPrepared.GetValue().State == WaitSetThreadDispatcherState::Prepared, primaryBlock.IsValid(),
        secondaryBlock.IsValid(), cancelled, primaryCommitted.HasValue(),
        primaryCommitted.GetValue().State == WaitSetThreadDispatcherState::Completed, secondaryCommitted.HasValue(),
        secondaryCommitted.GetValue().State == WaitSetThreadDispatcherState::Blocked,
        primaryQueueAfterFirst.ReadyEntries == 1U, peerQueueAfterFirst.ReadyEntries == 0U);
    ASTERIA_EXPECT(runner, "separate signals wake shared-WaitSet dispatchers exactly once each",
                   secondSignal == NotificationError::None, secondaryPolled.HasValue(),
                   secondaryPolled.GetValue().State == WaitSetThreadDispatcherState::Completed,
                   peer.QueueState().ReadyEntries == 1U, fixture.Set.GetSnapshot().ClaimedEntries == 0U);
}

void TestLastUnregisterRemainsRetryable(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const auto removed = fixture.Set.Unregister(fixture.Registration);
    fixture.Registration = WaitSetRegistrationToken{};
    const ThreadBlockToken blocked = fixture.Block();
    const auto committed = dispatcher.CommitBlocked(blocked);
    const auto replacement =
        fixture.Set.RegisterNotification(fixture.NotificationIdentity, fixture.Event, WaitTag + 1U);
    if (replacement.HasValue())
    {
        fixture.Registration = replacement.GetValue().Registration;
    }
    const NotificationError signalled = fixture.Event.Signal();
    const auto retried = dispatcher.Poll();

    ASTERIA_EXPECT(runner, "removing the last registration leaves an existing wait blocked", initialized,
                   prepared.HasValue(), removed.HasValue(), removed.GetValue().RegisteredEntries == 0U,
                   blocked.IsValid(), committed.HasValue(),
                   committed.GetValue().State == WaitSetThreadDispatcherState::Blocked,
                   dispatcher.GetSnapshot().Diagnostic.Error == WaitSetThreadDispatcherError::None);
    ASTERIA_EXPECT(runner, "a later registration and signal complete a wait that observed Empty",
                   replacement.HasValue(), signalled == NotificationError::None, retried.HasValue(),
                   retried.GetValue().State == WaitSetThreadDispatcherState::Completed,
                   retried.GetValue().RegistrationSequence == replacement.GetValue().Registration.GetSequence(),
                   retried.GetValue().Tag == (WaitTag + 1U),
                   fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready);
}

void TestIdentityAndPinRejection(TestContext& runner) noexcept
{
    WaitFixture fixture;
    WaitFixture foreign;
    const bool ready = fixture.Initialize() && foreign.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};

    WaitSetThreadPrepareRequest invalid = fixture.Request();
    invalid.WaitSet = WaitSetId{};
    const auto invalidWaitSet = dispatcher.Prepare(invalid);
    invalid = fixture.Request();
    invalid.WaitSet = foreign.WaitSetIdentity;
    const auto foreignWaitSet = dispatcher.Prepare(invalid);
    invalid = fixture.Request();
    invalid.Thread = foreign.ThreadIdentity;
    const auto foreignThread = dispatcher.Prepare(invalid);
    invalid = fixture.Request();
    invalid.SchedulingContext = foreign.ContextIdentity;
    const auto foreignContext = dispatcher.Prepare(invalid);

    invalid = fixture.Request();
    invalid.WaitSet = WaitSetId{fixture.Registry, fixture.NotificationObject};
    const auto wrongWaitSetType = dispatcher.Prepare(invalid);
    invalid = fixture.Request();
    invalid.Thread = ThreadId{fixture.Registry, fixture.NotificationObject};
    const auto wrongThreadType = dispatcher.Prepare(invalid);
    invalid = fixture.Request();
    invalid.SchedulingContext = SchedulingContextId{fixture.Registry, fixture.NotificationObject};
    const auto wrongContextType = dispatcher.Prepare(invalid);

    WaitSet staleSet{fixture.Registry};
    static_cast<void>(staleSet.Initialize(ResourceDomainId{21U}));
    const auto staleObject = fixture.Issuer.CreateObject(staleSet);
    WaitSetId staleIdentity{};
    if (staleObject.HasValue())
    {
        staleIdentity = WaitSetId{fixture.Registry, staleObject.GetValue()};
        static_cast<void>(fixture.Issuer.DestroyObject(staleObject.GetValue()));
        static_cast<void>(fixture.Issuer.CollectDestroyedObject());
    }
    invalid = fixture.Request();
    invalid.WaitSet = staleIdentity;
    const auto staleWaitSet = dispatcher.Prepare(invalid);
    const auto valid = dispatcher.Prepare(fixture.Request());

    ASTERIA_EXPECT(
        runner, "typed identities reject invalid and foreign registry generations", ready, !invalidWaitSet.HasValue(),
        FailureIs(invalidWaitSet.GetError(), WaitSetThreadDispatcherError::InvalidWaitSet), !foreignWaitSet.HasValue(),
        FailureIs(foreignWaitSet.GetError(), WaitSetThreadDispatcherError::WaitSetRegistryMismatch),
        !foreignThread.HasValue(),
        FailureIs(foreignThread.GetError(), WaitSetThreadDispatcherError::ThreadRegistryMismatch),
        !foreignContext.HasValue(),
        FailureIs(foreignContext.GetError(), WaitSetThreadDispatcherError::SchedulingContextRegistryMismatch));
    ASTERIA_EXPECT(
        runner, "type and stale-generation pin failures leave Prepare retryable", !wrongWaitSetType.HasValue(),
        FailureIs(wrongWaitSetType.GetError(), WaitSetThreadDispatcherError::WaitSetPinRejected),
        wrongWaitSetType.GetError().WaitSetPin == ObjectPinError::TypeMismatch, !wrongThreadType.HasValue(),
        FailureIs(wrongThreadType.GetError(), WaitSetThreadDispatcherError::ThreadPinRejected),
        wrongThreadType.GetError().ThreadPin == ObjectPinError::TypeMismatch, !wrongContextType.HasValue(),
        FailureIs(wrongContextType.GetError(), WaitSetThreadDispatcherError::SchedulingContextPinRejected),
        wrongContextType.GetError().SchedulingContextPin == ObjectPinError::TypeMismatch, !staleWaitSet.HasValue(),
        FailureIs(staleWaitSet.GetError(), WaitSetThreadDispatcherError::WaitSetPinRejected),
        staleWaitSet.GetError().WaitSetPin == ObjectPinError::ObjectNotFound, valid.HasValue(),
        valid.GetValue().State == WaitSetThreadDispatcherState::Prepared);
}

void TestStaleAndForeignBlockTokens(TestContext& runner) noexcept
{
    WaitFixture fixture;
    WaitFixture foreign;
    const bool initialized = fixture.Initialize() && foreign.Initialize();
    const ThreadBlockToken stale = fixture.Block();
    const bool restarted =
        stale.IsValid() && (fixture.ThreadStorage.Wake(stale) == ThreadError::None) && fixture.StartRun();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const ThreadBlockToken current = fixture.Block();
    const ThreadBlockToken foreignToken = foreign.Block();
    const auto invalid = dispatcher.CommitBlocked({});
    const auto staleResult = dispatcher.CommitBlocked(stale);
    const auto foreignResult = dispatcher.CommitBlocked(foreignToken);
    const WaitSetThreadDispatcherSnapshot beforeCorrect = dispatcher.GetSnapshot();
    const auto correct = dispatcher.CommitBlocked(current);

    ASTERIA_EXPECT(
        runner, "invalid, stale, and foreign block tokens change no dispatcher state", initialized, restarted,
        prepared.HasValue(), current.IsValid(), foreignToken.IsValid(), !invalid.HasValue(),
        FailureIs(invalid.GetError(), WaitSetThreadDispatcherError::InvalidBlockToken), !staleResult.HasValue(),
        FailureIs(staleResult.GetError(), WaitSetThreadDispatcherError::InvalidBlockToken), !foreignResult.HasValue(),
        FailureIs(foreignResult.GetError(), WaitSetThreadDispatcherError::InvalidBlockToken),
        beforeCorrect.State == WaitSetThreadDispatcherState::Prepared, beforeCorrect.BlockSequence == 0U,
        beforeCorrect.RetainedClaimSequence == 0U, beforeCorrect.RetainedQueueReservationSequence == 0U,
        fixture.QueueState().ReadyEntries == 0U, fixture.QueueState().ReservedEntries == 0U,
        fixture.Set.GetSnapshot().ClaimedEntries == 0U, correct.HasValue(),
        correct.GetValue().State == WaitSetThreadDispatcherState::Blocked);
}

void TestPreparedRunSequenceCannotReplay(TestContext& runner) noexcept
{
    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const Foundation::UInt64 preparedRun = prepared.HasValue() ? prepared.GetValue().RunSequence : 0U;
        const bool reran = fixture.YieldAndStartRun();
        const ThreadBlockToken laterBlock = fixture.Block();
        const auto replayed = dispatcher.CommitBlocked(laterBlock);
        const auto snapshot = dispatcher.GetSnapshot();

        ASTERIA_EXPECT(runner, "a later run's first block cannot satisfy an earlier Prepare", initialized,
                       prepared.HasValue(), preparedRun != 0U, reran, laterBlock.IsValid(),
                       fixture.ThreadStorage.GetSnapshot().RunSequence == (preparedRun + 1U), !replayed.HasValue(),
                       FailureIs(replayed.GetError(), WaitSetThreadDispatcherError::InvalidBlockToken),
                       snapshot.State == WaitSetThreadDispatcherState::Prepared, snapshot.RunSequence == preparedRun,
                       snapshot.BlockSequence == 0U);
    }

    {
        WaitFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = dispatcher.Prepare(fixture.Request());
        const Foundation::UInt64 preparedRun = prepared.HasValue() ? prepared.GetValue().RunSequence : 0U;
        const ThreadBlockToken firstBlock = fixture.Block();
        const bool reran =
            firstBlock.IsValid() && (fixture.ThreadStorage.Wake(firstBlock) == ThreadError::None) && fixture.StartRun();
        const ThreadBlockToken secondBlock = fixture.Block();
        const auto replayed = dispatcher.CommitBlocked(secondBlock);

        ASTERIA_EXPECT(runner, "wake, rerun, and reblock cannot replay a prepared transaction", initialized,
                       prepared.HasValue(), firstBlock.IsValid(), reran, secondBlock.IsValid(),
                       fixture.ThreadStorage.GetSnapshot().RunSequence == (preparedRun + 1U), !replayed.HasValue(),
                       FailureIs(replayed.GetError(), WaitSetThreadDispatcherError::InvalidBlockToken),
                       dispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Prepared,
                       dispatcher.GetSnapshot().BlockSequence == 0U);
    }
}

void TestCompletedSnapshotGenerationSafety(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const NotificationError signalled = fixture.Event.Signal();
    const auto completed = dispatcher.Prepare(fixture.Request());
    const auto terminal = dispatcher.GetSnapshot();
    const ObjectId oldWaitSetObject = fixture.WaitSetObject;
    const ObjectId oldThreadObject = fixture.ThreadObject;
    const auto removed = fixture.Set.Unregister(fixture.Registration);
    fixture.Registration = WaitSetRegistrationToken{};
    const ThreadBlockToken stopped = fixture.Block();
    const ThreadError terminated = fixture.ThreadStorage.Terminate(1U);
    const SchedulingContextError unbound = fixture.Context.Unbind(fixture.ThreadIdentity);

    const ObjectRegistryError waitSetRetired = fixture.Issuer.DestroyObject(oldWaitSetObject);
    const auto waitSetRecord = fixture.Issuer.CollectDestroyedObject();
    const auto replacementWaitSet = fixture.Issuer.CreateObject(fixture.Set);
    const ObjectRegistryError threadRetired = fixture.Issuer.DestroyObject(oldThreadObject);
    const auto threadRecord = fixture.Issuer.CollectDestroyedObject();
    const auto replacementThread = fixture.Issuer.CreateObject(fixture.ThreadStorage);

    if (replacementWaitSet.HasValue())
    {
        fixture.WaitSetObject = replacementWaitSet.GetValue();
        fixture.WaitSetIdentity = WaitSetId{fixture.Registry, fixture.WaitSetObject};
    }
    if (replacementThread.HasValue())
    {
        fixture.ThreadObject = replacementThread.GetValue();
        fixture.ThreadIdentity = ThreadId{fixture.Registry, fixture.ThreadObject};
    }
    const auto afterReuse = dispatcher.GetSnapshot();

    ASTERIA_EXPECT(runner, "Completed exposes only identities and inert scalar transaction data", initialized,
                   signalled == NotificationError::None, completed.HasValue(),
                   terminal.State == WaitSetThreadDispatcherState::Completed,
                   terminal.RegistrationSequence == completed.GetValue().RegistrationSequence,
                   terminal.RunSequence == completed.GetValue().RunSequence, terminal.BlockSequence == 0U,
                   terminal.RetainedClaimSequence == 0U, terminal.RetainedQueueReservationSequence == 0U,
                   ReservedIsZero(completed.GetValue().Reserved), ReservedIsZero(terminal.Reserved),
                   terminal.WaitSet.IsEqualTo(completed.GetValue().WaitSet),
                   terminal.Thread.IsEqualTo(completed.GetValue().Thread),
                   afterReuse.WaitSet.IsEqualTo(terminal.WaitSet), afterReuse.Thread.IsEqualTo(terminal.Thread),
                   afterReuse.RegistrationSequence == terminal.RegistrationSequence,
                   afterReuse.RunSequence == terminal.RunSequence, afterReuse.RetainedClaimSequence == 0U,
                   afterReuse.RetainedQueueReservationSequence == 0U, ReservedIsZero(afterReuse.Reserved));
    ASTERIA_EXPECT(runner, "same storage re-registration cannot alias completed generations", removed.HasValue(),
                   stopped.IsValid(), terminated == ThreadError::None, unbound == SchedulingContextError::None,
                   waitSetRetired == ObjectRegistryError::Ok, waitSetRecord.HasValue(),
                   waitSetRecord.GetValue().Instance == &fixture.Set, replacementWaitSet.HasValue(),
                   threadRetired == ObjectRegistryError::Ok, threadRecord.HasValue(),
                   threadRecord.GetValue().Instance == &fixture.ThreadStorage, replacementThread.HasValue(),
                   replacementWaitSet.GetValue().GetValue() != oldWaitSetObject.GetValue(),
                   replacementThread.GetValue().GetValue() != oldThreadObject.GetValue(),
                   !terminal.WaitSet.IsEqualTo(fixture.WaitSetIdentity),
                   !terminal.Thread.IsEqualTo(fixture.ThreadIdentity));
}

void TestQueuePressureAndClaimRollback(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const ThreadBlockToken blocked = fixture.Block();

    Thread fillerThreads[MaximumReadyQueueEntries]{};
    std::optional<SchedulingContext> fillerContexts[MaximumReadyQueueEntries]{};
    ObjectId fillerThreadObjects[MaximumReadyQueueEntries]{};
    ObjectId fillerContextObjects[MaximumReadyQueueEntries]{};
    ThreadId fillerThreadIds[MaximumReadyQueueEntries]{};
    SchedulingContextId fillerContextIds[MaximumReadyQueueEntries]{};
    bool filled{true};
    for (Foundation::Size index{0U}; index < MaximumReadyQueueEntries; ++index)
    {
        fillerContexts[index].emplace(fixture.Registry);
        filled = filled && (fillerThreads[index].Initialize(ResourceDomainId{31U}) == ThreadError::None) &&
                 (fillerContexts[index]->Initialize(Configuration(), ResourceDomainId{31U}, MonotonicTime{0U}) ==
                  SchedulingContextError::None);
        const auto thread = fixture.Issuer.CreateObject(fillerThreads[index]);
        const auto context = fixture.Issuer.CreateObject(*fillerContexts[index]);
        filled = filled && thread.HasValue() && context.HasValue();
        if (!thread.HasValue() || !context.HasValue())
        {
            break;
        }
        fillerThreadObjects[index] = thread.GetValue();
        fillerContextObjects[index] = context.GetValue();
        fillerThreadIds[index] = ThreadId{fixture.Registry, fillerThreadObjects[index]};
        fillerContextIds[index] = SchedulingContextId{fixture.Registry, fillerContextObjects[index]};
        filled =
            filled && (fillerContexts[index]->Bind(fillerThreadIds[index]) == SchedulingContextError::None) &&
            (fillerThreads[index].MakeReady() == ThreadError::None) &&
            fixture
                .Enqueue(fillerContextIds[index], *fillerContexts[index], fillerThreadIds[index], fillerThreads[index])
                .HasValue();
    }

    const NotificationError signalled = fixture.Event.Signal();
    const BlockedRunCommitOutcome pressured = dispatcher.CommitBlockedFromFinishedRun(blocked);
    const auto pressureSnapshot = dispatcher.GetSnapshot();
    const auto waitSetAfterPressure = fixture.Set.GetSnapshot();
    const auto notificationAfterPressure = fixture.Event.GetSnapshot();
    const auto threadAfterPressure = fixture.ThreadStorage.GetSnapshot();
    const auto queueAfterPressure = fixture.QueueState();
    const ReadyQueueError freed = fixture.Remove(fillerThreadIds[0U]);
    const auto retried = dispatcher.Poll();

    ASTERIA_EXPECT(
        runner, "queue pressure cancels the pre-wake claim and retains retry state", initialized, prepared.HasValue(),
        blocked.IsValid(), filled, signalled == NotificationError::None,
        pressured == BlockedRunCommitOutcome::RetryableRejected,
        FailureIs(pressureSnapshot.Diagnostic, WaitSetThreadDispatcherError::QueueReservationRejected),
        pressureSnapshot.Diagnostic.Queue.Error == ReadyQueueError::QueueFull,
        pressureSnapshot.State == WaitSetThreadDispatcherState::QueuePressure,
        pressureSnapshot.BlockSequence == blocked.GetValue(), pressureSnapshot.RetainedClaimSequence == 0U,
        pressureSnapshot.RetainedQueueReservationSequence == 0U, threadAfterPressure.State == ThreadState::Blocked,
        queueAfterPressure.ReadyEntries == MaximumReadyQueueEntries, queueAfterPressure.ReservedEntries == 0U,
        waitSetAfterPressure.State == WaitSetState::Ready, waitSetAfterPressure.ClaimedEntries == 0U,
        notificationAfterPressure.State == NotificationState::Ready, notificationAfterPressure.Pending == 1U);
    ASTERIA_EXPECT(runner, "freeing one queue slot lets Poll commit the retained event once",
                   freed == ReadyQueueError::None, retried.HasValue(),
                   ReceiptIs(retried.GetValue(), WaitSetThreadDispatcherState::Completed, fixture.WaitSetIdentity,
                             fixture.ThreadIdentity, fixture.ContextIdentity, WaitTag),
                   fixture.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   fixture.QueueState().ReadyEntries == MaximumReadyQueueEntries,
                   fixture.QueueState().ReservedEntries == 0U, fixture.Set.GetSnapshot().ClaimedEntries == 0U,
                   fixture.Event.GetSnapshot().Pending == 0U);

    for (Foundation::Size index{0U}; index < MaximumReadyQueueEntries; ++index)
    {
        if (index != 0U)
        {
            static_cast<void>(fixture.Remove(fillerThreadIds[index]));
        }
        if (fillerThreads[index].IsInitialized())
        {
            static_cast<void>(fillerThreads[index].Terminate(1U));
        }
        if (fillerContexts[index].has_value())
        {
            static_cast<void>(fillerContexts[index]->Unbind(fillerThreadIds[index]));
        }
        if (fillerContextObjects[index].IsValid())
        {
            static_cast<void>(fixture.Issuer.DestroyObject(fillerContextObjects[index]));
        }
        if (fillerThreadObjects[index].IsValid())
        {
            static_cast<void>(fixture.Issuer.DestroyObject(fillerThreadObjects[index]));
        }
    }
    for (Foundation::Size index{0U}; index < (MaximumReadyQueueEntries * 2U); ++index)
    {
        static_cast<void>(fixture.Issuer.CollectDestroyedObject());
    }
}

void TestTerminalCorrelationDiagnostic(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
    const auto prepared = dispatcher.Prepare(fixture.Request());
    const ThreadBlockToken blocked = fixture.Block();
    const auto committed = dispatcher.CommitBlocked(blocked);
    const ThreadError externalWake = fixture.ThreadStorage.Wake(blocked);
    const auto contradicted = dispatcher.Poll();
    const WaitSetThreadDispatcherSnapshot snapshot = dispatcher.GetSnapshot();
    const auto repeated = dispatcher.Poll();
    const auto removed = fixture.Set.Unregister(fixture.Registration);
    fixture.Registration = WaitSetRegistrationToken{};
    const ThreadError terminated = fixture.ThreadStorage.Terminate(1U);
    const SchedulingContextError unbound = fixture.Context.Unbind(fixture.ThreadIdentity);
    const ObjectRegistryError waitSetRetired = fixture.Issuer.DestroyObject(fixture.WaitSetObject);
    const ObjectRegistryError threadRetired = fixture.Issuer.DestroyObject(fixture.ThreadObject);
    const ObjectRegistryError contextRetired = fixture.Issuer.DestroyObject(fixture.ContextObject);
    Foundation::Size collectedCount{0U};
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        if (fixture.Issuer.CollectDestroyedObject().HasValue())
        {
            ++collectedCount;
        }
    }
    fixture.WaitSetObject = ObjectId{};
    fixture.ThreadObject = ObjectId{};
    fixture.ContextObject = ObjectId{};

    ASTERIA_EXPECT(runner, "a block-generation contradiction becomes a stable terminal diagnostic", initialized,
                   prepared.HasValue(), committed.HasValue(), externalWake == ThreadError::None,
                   !contradicted.HasValue(),
                   FailureIs(contradicted.GetError(), WaitSetThreadDispatcherError::CorrelatedStateChanged),
                   snapshot.State == WaitSetThreadDispatcherState::Faulted,
                   FailureIs(snapshot.Diagnostic, WaitSetThreadDispatcherError::CorrelatedStateChanged),
                   snapshot.BlockSequence == blocked.GetValue(), snapshot.RetainedClaimSequence == 0U,
                   snapshot.RetainedQueueReservationSequence == 0U, !repeated.HasValue(),
                   FailureIs(repeated.GetError(), WaitSetThreadDispatcherError::CorrelatedStateChanged),
                   fixture.QueueState().ReadyEntries == 0U, fixture.QueueState().ReservedEntries == 0U,
                   fixture.Set.GetSnapshot().ClaimedEntries == 0U);
    ASTERIA_EXPECT(runner, "a terminal fault with no retained action releases all object pins", removed.HasValue(),
                   terminated == ThreadError::None, unbound == SchedulingContextError::None,
                   waitSetRetired == ObjectRegistryError::Ok, threadRetired == ObjectRegistryError::Ok,
                   contextRetired == ObjectRegistryError::Ok, collectedCount == 3U);
}

void TestPreparedPinLifetime(TestContext& runner) noexcept
{
    WaitFixture fixture;
    const bool initialized = fixture.Initialize();
    bool prepared{false};
    ThreadBlockToken blocked{};
    ObjectRegistryError waitSetRetired{ObjectRegistryError::InvalidObject};
    ObjectRegistryError threadRetired{ObjectRegistryError::InvalidObject};
    ObjectRegistryError contextRetired{ObjectRegistryError::InvalidObject};
    ObjectRegistryError collectionWhilePinned{ObjectRegistryError::InvalidObject};
    {
        WaitSetThreadDispatcher dispatcher{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto result = dispatcher.Prepare(fixture.Request());
        prepared = result.HasValue() && (result.GetValue().State == WaitSetThreadDispatcherState::Prepared);
        blocked = fixture.Block();
        static_cast<void>(fixture.Context.Unbind(fixture.ThreadIdentity));
        waitSetRetired = fixture.Issuer.DestroyObject(fixture.WaitSetObject);
        threadRetired = fixture.Issuer.DestroyObject(fixture.ThreadObject);
        contextRetired = fixture.Issuer.DestroyObject(fixture.ContextObject);
        const auto collected = fixture.Issuer.CollectDestroyedObject();
        collectionWhilePinned = collected.HasValue() ? ObjectRegistryError::Ok : collected.GetError();
    }

    static_cast<void>(fixture.ThreadStorage.Terminate(1U));
    Foundation::Size collectedCount{0U};
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        if (fixture.Issuer.CollectDestroyedObject().HasValue())
        {
            ++collectedCount;
        }
    }
    fixture.WaitSetObject = ObjectId{};
    fixture.ThreadObject = ObjectId{};
    fixture.ContextObject = ObjectId{};

    ASTERIA_EXPECT(runner, "Prepare pins all three exact generations until dispatcher release", initialized, prepared,
                   blocked.IsValid(), waitSetRetired == ObjectRegistryError::Ok,
                   threadRetired == ObjectRegistryError::Ok, contextRetired == ObjectRegistryError::Ok,
                   collectionWhilePinned == ObjectRegistryError::NoDestructionReady, collectedCount == 3U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"WaitSetThreadDispatcherTests"};
    Asteria::Test::TestImmediateReadyAndDuplicateCompletion(runner);
    Asteria::Test::TestSignalBeforeBlockCommit(runner);
    Asteria::Test::TestCompletionConsumeAndReuse(runner);
    Asteria::Test::TestPreparationReceiptInvariantRejection(runner);
    Asteria::Test::TestAtomicCompletionReceiptMismatch(runner);
    Asteria::Test::TestStaleTransactionReceiptsCannotReplay(runner);
    Asteria::Test::TestCrossDispatcherReceiptsCannotSubstitute(runner);
    Asteria::Test::TestTransactionGenerationExhaustion(runner);
    Asteria::Test::TestTerminalPostFinishRejection(runner);
    Asteria::Test::TestSignalAfterBlockAndNoSignal(runner);
    Asteria::Test::TestExpiry(runner);
    Asteria::Test::TestAbortAndReset(runner);
    Asteria::Test::TestCompetingClaimCancelAndRetry(runner);
    Asteria::Test::TestTwoDispatchersShareWaitSet(runner);
    Asteria::Test::TestLastUnregisterRemainsRetryable(runner);
    Asteria::Test::TestIdentityAndPinRejection(runner);
    Asteria::Test::TestStaleAndForeignBlockTokens(runner);
    Asteria::Test::TestPreparedRunSequenceCannotReplay(runner);
    Asteria::Test::TestCompletedSnapshotGenerationSafety(runner);
    Asteria::Test::TestQueuePressureAndClaimRollback(runner);
    Asteria::Test::TestTerminalCorrelationDiagnostic(runner);
    Asteria::Test::TestPreparedPinLifetime(runner);
    return runner.Finish();
}
