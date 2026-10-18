// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"
#include <atomic>
#include <cstdio>
#include <functional>
#include <optional>
#include <thread>
#include "Kernel/Scheduler/ProcessorSet.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::Size;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::DispatchCoordinatorError;
using Kernel::Scheduler::MaximumReadyQueueEntries;
using Kernel::Scheduler::ProcessorScheduler;
using Kernel::Scheduler::ProcessorActiveExecutionBindingError;
using Kernel::Scheduler::ProcessorSchedulerError;
using Kernel::Scheduler::ProcessorSchedulerFailure;
using Kernel::Scheduler::ProcessorSchedulerSnapshot;
using Kernel::Scheduler::ProcessorSchedulerState;
using Kernel::Scheduler::ReadyQueueError;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingContextState;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadState;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

[[nodiscard]] constexpr SchedulingContextConfiguration
MakeConfiguration(const Foundation::UInt8 priority = 8U, const Foundation::UInt8 affinity = 0x0FU,
                  const Foundation::UInt64 budget = 20U, const Foundation::UInt64 deadline = 80U,
                  const Foundation::UInt64 period = 100U) noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = budget,
        .PeriodTicks = period,
        .RelativeDeadlineTicks = deadline,
        .Weight = 10U,
        .Priority = priority,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = affinity,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Size size) noexcept
{
    for (Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool SnapshotIs(const ProcessorSchedulerSnapshot& snapshot, const ProcessorSchedulerState state,
                              const Foundation::UInt8 ready, const Foundation::UInt8 claimed, const CpuId processor,
                              const ThreadId activeThread = {}, const SchedulingContextId activeContext = {}) noexcept
{
    return (snapshot.State == state) && snapshot.ActiveThread.IsEqualTo(activeThread) &&
           snapshot.ActiveContext.IsEqualTo(activeContext) && (snapshot.Processor.GetValue() == processor.GetValue()) &&
           (snapshot.Queue.ReadyEntries == ready) && (snapshot.Queue.ClaimedEntries == claimed) &&
           (snapshot.Queue.Processor.GetValue() == processor.GetValue()) &&
           BytesAreZero(snapshot.Queue.Reserved, sizeof(snapshot.Queue.Reserved)) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] bool FailureIs(const ProcessorSchedulerFailure& failure, const ProcessorSchedulerError error,
                             const Foundation::UInt64 retryAtTicks = 0U,
                             const ReadyQueueError queue = ReadyQueueError::None,
                             const DispatchCoordinatorError dispatch = DispatchCoordinatorError::None,
                             const SchedulingContextError context = SchedulingContextError::None,
                             const ThreadError thread = ThreadError::None) noexcept
{
    return (failure.Error == error) && (failure.RetryAtTicks == retryAtTicks) && (failure.Queue.Error == queue) &&
           (failure.Queue.ContextPin == ObjectPinError::Ok) && (failure.Queue.ThreadPin == ObjectPinError::Ok) &&
           (failure.Queue.Reserved == 0U) && (failure.Dispatch.Error == dispatch) &&
           (failure.Dispatch.Context == context) && (failure.Dispatch.Thread == thread) &&
           BytesAreZero(failure.Dispatch.Reserved, sizeof(failure.Dispatch.Reserved)) &&
           BytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

class RegisteredPair final
{
public:
    RegisteredPair(ObjectRegistry& registry, RootCapabilityIssuer& issuer,
                   const SchedulingContextConfiguration configuration = MakeConfiguration(),
                   const Foundation::UInt64 initialTicks = 0U) noexcept
        : Context{registry}, m_issuer{issuer}
    {
        if ((ThreadStorage.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (Context.Initialize(configuration, ResourceDomainId{7U}, MonotonicTime{initialTicks}) !=
             SchedulingContextError::None))
        {
            return;
        }
        const auto threadResult = m_issuer.CreateObject(ThreadStorage);
        const auto contextResult = m_issuer.CreateObject(Context);
        if (!threadResult.HasValue() || !contextResult.HasValue())
        {
            return;
        }
        ThreadObject = threadResult.GetValue();
        ContextObject = contextResult.GetValue();
        ThreadIdentity = ThreadId{registry, ThreadObject};
        ContextIdentity = SchedulingContextId{registry, ContextObject};
        Ready = (Context.Bind(ThreadIdentity) == SchedulingContextError::None) &&
                (ThreadStorage.MakeReady() == ThreadError::None);
    }

    ~RegisteredPair() noexcept
    {
        if (ThreadIdentity.IsValid())
        {
            static_cast<void>(Context.Unbind(ThreadIdentity));
        }
        if (ContextObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ContextObject));
        }
        if (ThreadObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ThreadObject));
        }
        for (Size index{0U}; index < 2U; ++index)
        {
            static_cast<void>(m_issuer.CollectDestroyedObject());
        }
    }

    RegisteredPair(const RegisteredPair&) = delete;
    RegisteredPair& operator=(const RegisteredPair&) = delete;
    RegisteredPair(RegisteredPair&&) = delete;
    RegisteredPair& operator=(RegisteredPair&&) = delete;

    Thread ThreadStorage;
    SchedulingContext Context;
    ObjectId ThreadObject{};
    ObjectId ContextObject{};
    ThreadId ThreadIdentity{};
    SchedulingContextId ContextIdentity{};
    bool Ready{false};

private:
    RootCapabilityIssuer& m_issuer;
};

void TestFourProcessorHomes(TestContext& runner) noexcept
{
    SpinLock lock;
    TestSupport::TestObjectRegistry registry{lock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{10U};
    Kernel::Scheduler::ProcessorSet processors;
    ProcessorScheduler schedulers[4]{{registry, clock, CpuId{0U}},
                                     {registry, clock, CpuId{1U}},
                                     {registry, clock, CpuId{2U}},
                                     {registry, clock, CpuId{3U}}};
    Kernel::Scheduler::ProcessorSchedulerWakeFacade* homes[4U]{
        &schedulers[0U].GetWakeFacade(), &schedulers[1U].GetWakeFacade(), &schedulers[2U].GetWakeFacade(),
        &schedulers[3U].GetWakeFacade()};
    for (Size index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        homes[index]->BindHomes(homes);
    }
    RegisteredPair pairs[4]{{registry, issuer, MakeConfiguration(8U, 1U)},
                            {registry, issuer, MakeConfiguration(8U, 2U)},
                            {registry, issuer, MakeConfiguration(8U, 4U)},
                            {registry, issuer, MakeConfiguration(8U, 8U)}};
    bool valid{true};
    for (Foundation::UInt8 home{0U}; home < 4U; ++home)
    {
        valid = valid && processors.Admit(CpuId{home}) && pairs[home].Ready;
        auto& pair = pairs[home];
        for (Foundation::UInt8 poster{0U}; poster < 4U; ++poster)
        {
            valid = valid && homes[poster]->GetHomeProcessor(pair.Context).GetValue() == home;
        }
        // A wake/enqueue can be posted by any CPU, but affinity admits only
        // the selected home and queue membership admits only one queue.
        for (Foundation::UInt8 poster{0U}; poster < 4U; ++poster)
        {
            Platform::Synchronization::SetHostedProcessor(CpuId{poster});
            for (Foundation::UInt8 queue{0U}; queue < 4U; ++queue)
            {
                if (queue != home)
                {
                    valid = valid &&
                            !schedulers[queue]
                                 .Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage)
                                 .HasValue();
                }
            }
        }
        valid = valid && schedulers[home]
                             .Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage)
                             .HasValue();
        const auto run = schedulers[home].DispatchNext();
        valid = valid && run.HasValue() &&
                pair.ThreadStorage.GetSnapshot().State == Kernel::Scheduler::ThreadState::Running;
        const auto blocked = schedulers[home].Finish(ThreadRunDisposition::Blocked);
        valid = valid && blocked.HasValue();
        const auto token = pair.ThreadStorage.GetSnapshot().BlockToken;
        const auto reservation = homes[(home + 1U) % 4U]->ReserveBlockedEnqueue(
            pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage, token);
        valid = valid && reservation.HasValue() && pair.ThreadStorage.Wake(token) == ThreadError::None &&
                homes[(home + 2U) % 4U]->CommitReservedEnqueue(reservation.GetValue()).HasValue();
        valid = valid && schedulers[home].Remove(pair.ThreadIdentity).HasValue() &&
                pair.ThreadStorage.Terminate(42U) == ThreadError::None;
        if (home != 0U)
        {
            valid = valid && processors.BeginOfflining(CpuId{home}) && processors.FinishOfflining(CpuId{home});
        }
    }
    valid = valid && processors.OnlineMask() == 1U && processors.SilentOnline(0xEU) == 0U &&
            !processors.BeginOfflining(CpuId{0U});
    Platform::Synchronization::SetHostedProcessor(CpuId{});
    ASTERIA_EXPECT(runner, "four processor queues preserve homes through dispatch, remote wake, terminate and offline",
                   valid);
}

void TestDispatchLifecycle(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    RegisteredPair first{registry, issuer};
    RegisteredPair second{registry, issuer};
    ManualClock clock{10U};
    ProcessorScheduler scheduler{registry, clock, CpuId{0U}};

    const auto firstEnqueue =
        scheduler.Enqueue(first.ContextIdentity, first.Context, first.ThreadIdentity, first.ThreadStorage);
    const auto secondEnqueue =
        scheduler.Enqueue(second.ContextIdentity, second.Context, second.ThreadIdentity, second.ThreadStorage);
    const auto admission = scheduler.DispatchNext();
    ASTERIA_EXPECT(
        runner, "DispatchNext retains the selected claim through the active run", first.Ready, second.Ready,
        firstEnqueue.HasValue(), secondEnqueue.HasValue(), admission.HasValue(),
        admission.GetValue().Context.IsEqualTo(first.ContextIdentity),
        admission.GetValue().ContextStorage == &first.Context,
        admission.GetValue().ThreadStorage == &first.ThreadStorage, admission.GetValue().Dispatch.StartTicks == 10U,
        admission.GetValue().Dispatch.Context.MaximumRunTicks == 20U, admission.GetValue().StopAtTicks == 30U,
        SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Active, 1U, 1U, CpuId{0U}, first.ThreadIdentity,
                   first.ContextIdentity));

    const auto alreadyActive = scheduler.DispatchNext();
    const auto invalidFinish = scheduler.Finish(static_cast<ThreadRunDisposition>(0U));
    ASTERIA_EXPECT(
        runner, "invalid and duplicate operations preserve the active transaction", !alreadyActive.HasValue(),
        FailureIs(alreadyActive.GetError(), ProcessorSchedulerError::AlreadyActive), !invalidFinish.HasValue(),
        FailureIs(invalidFinish.GetError(), ProcessorSchedulerError::DispatchRejected, 0U, ReadyQueueError::None,
                  DispatchCoordinatorError::InvalidDisposition, SchedulingContextError::None,
                  ThreadError::InvalidDisposition),
        scheduler.GetSnapshot().State == ProcessorSchedulerState::Active);

    clock.Set(15U);
    const auto firstCompletion = scheduler.Finish(ThreadRunDisposition::Ready);
    ASTERIA_EXPECT(runner, "a Ready completion keeps its pins and takes a new FIFO position",
                   firstCompletion.HasValue(), firstCompletion.GetValue().Context.ElapsedTicks == 5U,
                   firstCompletion.GetValue().Thread.State == ThreadState::Ready,
                   SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 2U, 0U, CpuId{0U}));

    const auto secondAdmission = scheduler.DispatchNext();
    clock.Set(20U);
    const auto secondCompletion = scheduler.Finish(ThreadRunDisposition::Blocked);
    ASTERIA_EXPECT(runner, "a Blocked completion consumes its retained queue slot", secondAdmission.HasValue(),
                   secondAdmission.GetValue().Dispatch.Context.Thread.IsEqualTo(second.ThreadIdentity),
                   secondCompletion.HasValue(), secondCompletion.GetValue().Thread.State == ThreadState::Blocked,
                   SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 1U, 0U, CpuId{0U}));

    const auto removeBlocked = scheduler.Remove(second.ThreadIdentity);
    const auto removeFirst = scheduler.Remove(first.ThreadIdentity);
    const ThreadError wake = second.ThreadStorage.Wake(secondCompletion.GetValue().Thread.BlockToken);
    const auto reenqueueSecond =
        scheduler.Enqueue(second.ContextIdentity, second.Context, second.ThreadIdentity, second.ThreadStorage);
    const auto removeSecond = scheduler.Remove(second.ThreadIdentity);
    ASTERIA_EXPECT(
        runner, "woken and ordinary Ready objects can re-enter and leave placement", !removeBlocked.HasValue(),
        FailureIs(removeBlocked.GetError(), ProcessorSchedulerError::QueueRejected, 0U, ReadyQueueError::NotFound),
        removeFirst.HasValue(), wake == ThreadError::None, reenqueueSecond.HasValue(), removeSecond.HasValue(),
        SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 0U, 0U, CpuId{0U}));
}

void TestBudgetDeferral(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    const SchedulingContextConfiguration highConfiguration = MakeConfiguration(200U, 0x01U, 10U, 20U, 100U);
    const SchedulingContextConfiguration lowConfiguration = MakeConfiguration(1U, 0x01U, 10U, 20U, 100U);
    RegisteredPair high{registry, issuer, highConfiguration};
    RegisteredPair low{registry, issuer, lowConfiguration};
    static_cast<void>(high.Context.StartRun(high.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}));
    static_cast<void>(high.Context.EndRun(CpuId{0U}, MonotonicTime{10U}));
    ManualClock clock{15U};
    ProcessorScheduler scheduler{registry, clock, CpuId{0U}};
    const auto highEnqueue =
        scheduler.Enqueue(high.ContextIdentity, high.Context, high.ThreadIdentity, high.ThreadStorage);
    const auto lowEnqueue = scheduler.Enqueue(low.ContextIdentity, low.Context, low.ThreadIdentity, low.ThreadStorage);
    const auto admission = scheduler.DispatchNext();
    ASTERIA_EXPECT(runner, "an exhausted higher-ranked entry does not hide an eligible peer", high.Ready, low.Ready,
                   highEnqueue.HasValue(), lowEnqueue.HasValue(), admission.HasValue(),
                   admission.GetValue().Dispatch.Context.Thread.IsEqualTo(low.ThreadIdentity),
                   high.Context.GetSnapshot().State == SchedulingContextState::BudgetExhausted,
                   high.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Active, 1U, 1U, CpuId{0U},
                              low.ThreadIdentity, low.ContextIdentity));

    clock.Set(20U);
    const auto completion = scheduler.Finish(ThreadRunDisposition::Ready);
    const auto removeHigh = scheduler.Remove(high.ThreadIdentity);
    const auto removeLow = scheduler.Remove(low.ThreadIdentity);
    ASTERIA_EXPECT(runner, "temporary admission deferral restores every skipped queue claim", completion.HasValue(),
                   removeHigh.HasValue(), removeLow.HasValue());
}

void TestRetryBoundaries(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    const SchedulingContextConfiguration configuration = MakeConfiguration(8U, 0x01U, 10U, 20U, 100U);

    RegisteredPair exhausted{registry, issuer, configuration};
    static_cast<void>(exhausted.Context.StartRun(exhausted.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}));
    static_cast<void>(exhausted.Context.EndRun(CpuId{0U}, MonotonicTime{10U}));
    ManualClock exhaustedClock{15U};
    ProcessorScheduler exhaustedScheduler{registry, exhaustedClock, CpuId{0U}};
    static_cast<void>(exhaustedScheduler.Enqueue(exhausted.ContextIdentity, exhausted.Context, exhausted.ThreadIdentity,
                                                 exhausted.ThreadStorage));
    const auto unavailable = exhaustedScheduler.DispatchNext();
    ASTERIA_EXPECT(
        runner, "all exhausted entries return the earliest future replenishment", !unavailable.HasValue(),
        FailureIs(unavailable.GetError(), ProcessorSchedulerError::NoRunnableThread, 100U, ReadyQueueError::Empty),
        SnapshotIs(exhaustedScheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 1U, 0U, CpuId{0U}));

    exhaustedClock.Set(100U);
    const auto replenished = exhaustedScheduler.DispatchNext();
    exhaustedClock.Set(105U);
    const auto replenishedCompletion = exhaustedScheduler.Finish(ThreadRunDisposition::Ready);
    const auto exhaustedRemoval = exhaustedScheduler.Remove(exhausted.ThreadIdentity);
    ASTERIA_EXPECT(runner, "the exact boundary admits a lazily replenished context", replenished.HasValue(),
                   replenished.GetValue().StopAtTicks == 110U, replenished.GetValue().SchedulerRetryAtTicks == 0U,
                   replenishedCompletion.HasValue(), exhaustedRemoval.HasValue());

    RegisteredPair expired{registry, issuer, configuration};
    ManualClock expiredClock{150U};
    ProcessorScheduler expiredScheduler{registry, expiredClock, CpuId{0U}};
    static_cast<void>(expiredScheduler.Enqueue(expired.ContextIdentity, expired.Context, expired.ThreadIdentity,
                                               expired.ThreadStorage));
    const auto deadlineExpired = expiredScheduler.DispatchNext();
    ASTERIA_EXPECT(
        runner, "deadline rollback still reports the next future period boundary", !deadlineExpired.HasValue(),
        FailureIs(deadlineExpired.GetError(), ProcessorSchedulerError::NoRunnableThread, 200U, ReadyQueueError::Empty),
        expired.Context.GetSnapshot().NextReplenishmentTicks == 100U);

    expiredClock.Set(200U);
    const auto nextPeriod = expiredScheduler.DispatchNext();
    expiredClock.Set(205U);
    const auto nextCompletion = expiredScheduler.Finish(ThreadRunDisposition::Ready);
    const auto expiredRemoval = expiredScheduler.Remove(expired.ThreadIdentity);
    ASTERIA_EXPECT(runner, "a future period becomes dispatchable without a stale retry loop", nextPeriod.HasValue(),
                   nextPeriod.GetValue().Dispatch.StartTicks == 200U, nextPeriod.GetValue().StopAtTicks == 210U,
                   nextCompletion.HasValue(), expiredRemoval.HasValue());

    RegisteredPair laterRetry{registry, issuer, MakeConfiguration(200U, 0x01U, 10U, 20U, 100U)};
    RegisteredPair earlierRetry{registry, issuer, MakeConfiguration(100U, 0x01U, 10U, 20U, 60U)};
    static_cast<void>(laterRetry.Context.StartRun(laterRetry.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}));
    static_cast<void>(laterRetry.Context.EndRun(CpuId{0U}, MonotonicTime{10U}));
    static_cast<void>(earlierRetry.Context.StartRun(earlierRetry.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}));
    static_cast<void>(earlierRetry.Context.EndRun(CpuId{0U}, MonotonicTime{10U}));
    ManualClock multipleClock{15U};
    ProcessorScheduler multipleScheduler{registry, multipleClock, CpuId{0U}};
    const auto laterEnqueue = multipleScheduler.Enqueue(laterRetry.ContextIdentity, laterRetry.Context,
                                                        laterRetry.ThreadIdentity, laterRetry.ThreadStorage);
    const auto earlierEnqueue = multipleScheduler.Enqueue(earlierRetry.ContextIdentity, earlierRetry.Context,
                                                          earlierRetry.ThreadIdentity, earlierRetry.ThreadStorage);
    const auto multipleDeferred = multipleScheduler.DispatchNext();
    ASTERIA_EXPECT(
        runner, "multiple deferred entries report the earliest retry and restore FIFO", laterRetry.Ready,
        earlierRetry.Ready, laterEnqueue.HasValue(), earlierEnqueue.HasValue(), !multipleDeferred.HasValue(),
        FailureIs(multipleDeferred.GetError(), ProcessorSchedulerError::NoRunnableThread, 60U, ReadyQueueError::Empty),
        SnapshotIs(multipleScheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 2U, 0U, CpuId{0U}));

    multipleClock.Set(60U);
    const auto earlierAdmission = multipleScheduler.DispatchNext();
    multipleClock.Set(65U);
    const auto earlierCompletion = multipleScheduler.Finish(ThreadRunDisposition::Ready);
    const auto laterRemoval = multipleScheduler.Remove(laterRetry.ThreadIdentity);
    const auto earlierRemoval = multipleScheduler.Remove(earlierRetry.ThreadIdentity);
    ASTERIA_EXPECT(
        runner, "the earliest replenished entry runs without losing deferred peers", earlierAdmission.HasValue(),
        earlierAdmission.GetValue().Context.IsEqualTo(earlierRetry.ContextIdentity),
        earlierAdmission.GetValue().StopAtTicks == 70U, earlierAdmission.GetValue().SchedulerRetryAtTicks == 100U,
        earlierCompletion.HasValue(), laterRemoval.HasValue(), earlierRemoval.HasValue());

    RegisteredPair earliestDeferred{registry, issuer, MakeConfiguration(200U, 0x01U, 10U, 20U, 60U)};
    RegisteredPair laterDeferred{registry, issuer, MakeConfiguration(100U, 0x01U, 10U, 20U, 100U)};
    RegisteredPair eligiblePeer{registry, issuer, MakeConfiguration(50U, 0x01U, 10U, 20U, 100U)};
    const bool preparedDifferentRetries =
        earliestDeferred.Context.StartRun(earliestDeferred.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
        earliestDeferred.Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue() &&
        laterDeferred.Context.StartRun(laterDeferred.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
        laterDeferred.Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue();
    ManualClock differentRetryClock{15U};
    ProcessorScheduler differentRetryScheduler{registry, differentRetryClock, CpuId{0U}};
    const bool enqueuedDifferentRetries = differentRetryScheduler
                                              .Enqueue(earliestDeferred.ContextIdentity, earliestDeferred.Context,
                                                       earliestDeferred.ThreadIdentity, earliestDeferred.ThreadStorage)
                                              .HasValue() &&
                                          differentRetryScheduler
                                              .Enqueue(laterDeferred.ContextIdentity, laterDeferred.Context,
                                                       laterDeferred.ThreadIdentity, laterDeferred.ThreadStorage)
                                              .HasValue() &&
                                          differentRetryScheduler
                                              .Enqueue(eligiblePeer.ContextIdentity, eligiblePeer.Context,
                                                       eligiblePeer.ThreadIdentity, eligiblePeer.ThreadStorage)
                                              .HasValue();
    const auto differentRetryAdmission = differentRetryScheduler.DispatchNext();
    differentRetryClock.Set(20U);
    const auto differentRetryCompletion = differentRetryScheduler.Finish(ThreadRunDisposition::Ready);
    const auto earliestDeferredRemoval = differentRetryScheduler.Remove(earliestDeferred.ThreadIdentity);
    const auto laterDeferredRemoval = differentRetryScheduler.Remove(laterDeferred.ThreadIdentity);
    const auto eligiblePeerRemoval = differentRetryScheduler.Remove(eligiblePeer.ThreadIdentity);
    ASTERIA_EXPECT(runner, "an eligible peer retains the minimum of differently ordered retries",
                   earliestDeferred.Ready, laterDeferred.Ready, eligiblePeer.Ready, preparedDifferentRetries,
                   enqueuedDifferentRetries, differentRetryAdmission.HasValue(),
                   differentRetryAdmission.GetValue().Context.IsEqualTo(eligiblePeer.ContextIdentity),
                   differentRetryAdmission.GetValue().SchedulerRetryAtTicks == 60U, differentRetryCompletion.HasValue(),
                   earliestDeferredRemoval.HasValue(), laterDeferredRemoval.HasValue(), eligiblePeerRemoval.HasValue());
}

void TestFullQueueDeferral(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    std::optional<RegisteredPair> pairs[MaximumReadyQueueEntries]{};
    bool fixtureReady{true};
    for (Size index{0U}; index < MaximumReadyQueueEntries; ++index)
    {
        // Thirty-one exhausted peers outrank the last, eligible entry by
        // priority; by deadline alone their next window (deadline 120) would
        // follow its current deadline 20.
        const bool exhausted = index != (MaximumReadyQueueEntries - 1U);
        pairs[index].emplace(registry, issuer, MakeConfiguration(exhausted ? 8U : 7U, 0x01U, 10U, 20U, 100U));
        fixtureReady = fixtureReady && pairs[index]->Ready;
        if (exhausted)
        {
            fixtureReady =
                fixtureReady &&
                pairs[index]->Context.StartRun(pairs[index]->ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
                pairs[index]->Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue();
        }
    }

    ManualClock clock{15U};
    ProcessorScheduler scheduler{registry, clock, CpuId{0U}};
    bool enqueuedAll{fixtureReady};
    for (Size index{0U}; index < MaximumReadyQueueEntries; ++index)
    {
        enqueuedAll = enqueuedAll && scheduler
                                         .Enqueue(pairs[index]->ContextIdentity, pairs[index]->Context,
                                                  pairs[index]->ThreadIdentity, pairs[index]->ThreadStorage)
                                         .HasValue();
    }

    const auto admission = scheduler.DispatchNext();
    const RegisteredPair& eligible = *pairs[MaximumReadyQueueEntries - 1U];
    clock.Set(20U);
    const auto completion = scheduler.Finish(ThreadRunDisposition::Ready);
    bool removedAll{true};
    for (Size index{0U}; index < MaximumReadyQueueEntries; ++index)
    {
        removedAll = removedAll && scheduler.Remove(pairs[index]->ThreadIdentity).HasValue();
    }
    ASTERIA_EXPECT(runner, "all thirty-two candidates are examined and deferred claims restored", enqueuedAll,
                   admission.HasValue(),
                   admission.GetValue().Dispatch.Context.Thread.IsEqualTo(eligible.ThreadIdentity),
                   admission.GetValue().SchedulerRetryAtTicks == 100U, completion.HasValue(), removedAll,
                   SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 0U, 0U, CpuId{0U}));
}

void TestFailureAndExit(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    RegisteredPair pair{registry, issuer};
    ManualClock clock{0U};
    ProcessorScheduler invalid{registry, clock, CpuId{4U}};
    const auto invalidEnqueue =
        invalid.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    ASTERIA_EXPECT(runner, "an invalid processor configuration is terminal before placement",
                   !invalidEnqueue.HasValue(),
                   FailureIs(invalidEnqueue.GetError(), ProcessorSchedulerError::InvalidConfiguration),
                   invalid.GetSnapshot().State == ProcessorSchedulerState::Faulted);

    ProcessorScheduler scheduler{registry, clock, CpuId{0U}};
    const auto empty = scheduler.DispatchNext();
    ASTERIA_EXPECT(runner, "an empty scheduler has no retry deadline", !empty.HasValue(),
                   FailureIs(empty.GetError(), ProcessorSchedulerError::NoRunnableThread, 0U, ReadyQueueError::Empty));

    static_cast<void>(scheduler.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage));
    clock.Set(~Foundation::UInt64{0U});
    const auto exhaustedClock = scheduler.DispatchNext();
    ASTERIA_EXPECT(runner, "clock exhaustion restores the claimed queue entry", !exhaustedClock.HasValue(),
                   FailureIs(exhaustedClock.GetError(), ProcessorSchedulerError::DispatchRejected, 0U,
                             ReadyQueueError::None, DispatchCoordinatorError::ClockExhausted),
                   exhaustedClock.GetError().Dispatch.ObservedTicks == ~Foundation::UInt64{0U},
                   SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 1U, 0U, CpuId{0U}));

    clock.Set(10U);
    const auto admission = scheduler.DispatchNext();
    const auto activeRemoval = scheduler.Remove(pair.ThreadIdentity);
    clock.Set(20U);
    const auto completion = scheduler.Finish(ThreadRunDisposition::Exited, 42U);
    ASTERIA_EXPECT(
        runner, "an Exited completion releases the active placement exactly once", admission.HasValue(),
        !activeRemoval.HasValue(),
        FailureIs(activeRemoval.GetError(), ProcessorSchedulerError::QueueRejected, 0U, ReadyQueueError::EntryClaimed),
        completion.HasValue(), completion.GetValue().Thread.State == ThreadState::Exited,
        completion.GetValue().Thread.ExitCode == 42U,
        SnapshotIs(scheduler.GetSnapshot(), ProcessorSchedulerState::Idle, 0U, 0U, CpuId{0U}));
}

void TestDeadlineRanksCurrentWindow(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    // At tick 950 the idle context, last refreshed at tick 0, is in window
    // [900, 1000); the peer's current deadline is 960.
    RegisteredPair idle{registry, issuer, MakeConfiguration(8U, 0x01U, 10U, 100U, 100U)};
    RegisteredPair peer{registry, issuer, MakeConfiguration(8U, 0x01U, 10U, 400U, 1000U), 560U};
    ManualClock clock{950U};
    ProcessorScheduler scheduler{registry, clock, CpuId{0U}};
    const bool enqueued =
        scheduler.Enqueue(idle.ContextIdentity, idle.Context, idle.ThreadIdentity, idle.ThreadStorage).HasValue() &&
        scheduler.Enqueue(peer.ContextIdentity, peer.Context, peer.ThreadIdentity, peer.ThreadStorage).HasValue();
    const auto admission = scheduler.DispatchNext();
    const auto completion = scheduler.Finish(ThreadRunDisposition::Ready);
    const bool removed =
        scheduler.Remove(idle.ThreadIdentity).HasValue() && scheduler.Remove(peer.ThreadIdentity).HasValue();
    ASTERIA_EXPECT(runner, "DispatchNext ranks deadline entries by their windows at its clock sample", idle.Ready,
                   peer.Ready, enqueued, admission.HasValue(),
                   admission.GetValue().Dispatch.Context.Thread.IsEqualTo(peer.ThreadIdentity),
                   admission.GetValue().StopAtTicks == 960U, completion.HasValue(), removed);
}

void TestRejectedCandidatesAreSkipped(TestContext& runner) noexcept
{
    SpinLock registryLock;
    // Binding and placement pin each Thread twice; a dispatch pin is a third.
    Asteria::TestSupport::TestObjectRegistry registry{registryLock, 3U};
    RootCapabilityIssuer issuer{registry};
    // The first context's samples start at tick 1000, so StartRun at the
    // scheduler's clock rejects it as TimeReversed.
    RegisteredPair reversed{registry, issuer, MakeConfiguration(30U, 0x01U), 1000U};
    RegisteredPair pinned{registry, issuer, MakeConfiguration(20U, 0x01U)};
    RegisteredPair eligible{registry, issuer, MakeConfiguration(10U, 0x01U)};
    ObjectPin extraPin{};
    const bool pinnedFull = registry.PinObject(pinned.ThreadObject, ObjectType::Thread, extraPin) == ObjectPinError::Ok;
    ManualClock clock{15U};
    ProcessorScheduler scheduler{registry, clock, CpuId{0U}};
    const bool enqueued =
        scheduler.Enqueue(reversed.ContextIdentity, reversed.Context, reversed.ThreadIdentity, reversed.ThreadStorage)
            .HasValue() &&
        scheduler.Enqueue(pinned.ContextIdentity, pinned.Context, pinned.ThreadIdentity, pinned.ThreadStorage)
            .HasValue() &&
        scheduler.Enqueue(eligible.ContextIdentity, eligible.Context, eligible.ThreadIdentity, eligible.ThreadStorage)
            .HasValue();
    const auto admission = scheduler.DispatchNext();
    const ProcessorSchedulerSnapshot active = scheduler.GetSnapshot();
    clock.Set(20U);
    const auto completion = scheduler.Finish(ThreadRunDisposition::Blocked);
    ASTERIA_EXPECT(runner,
                   "candidates whose Thread or SchedulingContext rejects admission do not "
                   "hide a lower-ranked eligible peer",
                   reversed.Ready, pinned.Ready, eligible.Ready, pinnedFull, enqueued, admission.HasValue(),
                   admission.GetValue().Dispatch.Context.Thread.IsEqualTo(eligible.ThreadIdentity),
                   SnapshotIs(active, ProcessorSchedulerState::Active, 2U, 1U, CpuId{0U}, eligible.ThreadIdentity,
                              eligible.ContextIdentity),
                   reversed.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   reversed.Context.GetSnapshot().State == SchedulingContextState::Bound,
                   pinned.ThreadStorage.GetSnapshot().State == ThreadState::Ready, completion.HasValue());

    const auto rejected = scheduler.DispatchNext();
    const ProcessorSchedulerSnapshot afterRejection = scheduler.GetSnapshot();
    const bool removed =
        scheduler.Remove(reversed.ThreadIdentity).HasValue() && scheduler.Remove(pinned.ThreadIdentity).HasValue();
    extraPin.Reset();
    ASTERIA_EXPECT(runner,
                   "when every candidate rejects admission the queue is restored and no "
                   "thread is runnable",
                   !rejected.HasValue(),
                   FailureIs(rejected.GetError(), ProcessorSchedulerError::NoRunnableThread, 0U, ReadyQueueError::Empty,
                             DispatchCoordinatorError::SchedulingContextRejected, SchedulingContextError::TimeReversed),
                   rejected.GetError().Dispatch.ObservedTicks == 20U,
                   SnapshotIs(afterRejection, ProcessorSchedulerState::Idle, 2U, 0U, CpuId{0U}), removed);
}

void TestCrossProcessorPlacement(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    RegisteredPair pair{registry, issuer, MakeConfiguration(8U, 0x03U)};
    ManualClock clock{0U};
    ProcessorScheduler first{registry, clock, CpuId{0U}};
    ProcessorScheduler second{registry, clock, CpuId{1U}};
    std::atomic<Size> readyWorkers{0U};
    std::atomic<Size> successes{0U};
    std::atomic<Size> duplicateFailures{0U};
    std::atomic<Foundation::UInt8> winner{0xFFU};
    std::atomic<bool> start{false};
    const auto worker = [&pair, &readyWorkers, &successes, &duplicateFailures, &winner,
                         &start](ProcessorScheduler& scheduler, const CpuId processor) noexcept
    {
        static_cast<void>(readyWorkers.fetch_add(1U, std::memory_order_release));
        for (Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (start.load(std::memory_order_acquire))
            {
                break;
            }
            std::this_thread::yield();
        }
        const auto result =
            scheduler.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
        if (result.HasValue())
        {
            static_cast<void>(successes.fetch_add(1U, std::memory_order_relaxed));
            winner.store(processor.GetValue(), std::memory_order_relaxed);
        }
        else if ((result.GetError().Error == ProcessorSchedulerError::QueueRejected) &&
                 (result.GetError().Queue.Error == ReadyQueueError::ContextAlreadyQueued))
        {
            static_cast<void>(duplicateFailures.fetch_add(1U, std::memory_order_relaxed));
        }
    };
    std::thread firstWorker{worker, std::ref(first), CpuId{0U}};
    std::thread secondWorker{worker, std::ref(second), CpuId{1U}};
    bool bothReady{false};
    for (Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (readyWorkers.load(std::memory_order_acquire) == 2U)
        {
            bothReady = true;
            break;
        }
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    firstWorker.join();
    secondWorker.join();

    const Foundation::UInt8 winnerValue = winner.load(std::memory_order_relaxed);
    const auto removal = winnerValue == 0U ? first.Remove(pair.ThreadIdentity) : second.Remove(pair.ThreadIdentity);
    ASTERIA_EXPECT(runner, "one runnable pair atomically belongs to only one CPU queue", pair.Ready, bothReady,
                   successes.load(std::memory_order_relaxed) == 1U,
                   duplicateFailures.load(std::memory_order_relaxed) == 1U, (winnerValue == 0U) || (winnerValue == 1U),
                   removal.HasValue(),
                   SnapshotIs(first.GetSnapshot(), ProcessorSchedulerState::Idle, 0U, 0U, CpuId{0U}),
                   SnapshotIs(second.GetSnapshot(), ProcessorSchedulerState::Idle, 0U, 0U, CpuId{1U}));
}

void TestConcurrentInspection(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    RegisteredPair pair{registry, issuer, MakeConfiguration(8U, 0x01U)};
    ManualClock clock{0U};
    ProcessorScheduler scheduler{registry, clock, CpuId{0U}};
    static_cast<void>(scheduler.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage));
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<Size> failures{0U};
    const auto dispatchWorker = [&scheduler, &start, &done, &failures]() noexcept
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (Size iteration{0U}; iteration < 1'024U; ++iteration)
        {
            const auto admission = scheduler.DispatchNext();
            const auto completion = scheduler.Finish(ThreadRunDisposition::Ready);
            if (!admission.HasValue() || !completion.HasValue())
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                break;
            }
        }
        done.store(true, std::memory_order_release);
    };
    const auto observerWorker = [&scheduler, &start, &done, &failures]() noexcept
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        do
        {
            const ProcessorSchedulerSnapshot snapshot = scheduler.GetSnapshot();
            const bool idle = (snapshot.State == ProcessorSchedulerState::Idle) &&
                              (snapshot.Queue.ReadyEntries == 1U) && (snapshot.Queue.ClaimedEntries == 0U) &&
                              !snapshot.ActiveThread.IsValid() && !snapshot.ActiveContext.IsValid();
            const bool active = (snapshot.State == ProcessorSchedulerState::Active) &&
                                (snapshot.Queue.ReadyEntries == 0U) && (snapshot.Queue.ClaimedEntries == 1U) &&
                                snapshot.ActiveThread.IsValid() && snapshot.ActiveContext.IsValid();
            if (!idle && !active)
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
            }
            std::this_thread::yield();
        } while (!done.load(std::memory_order_acquire));
    };
    std::thread dispatcher{dispatchWorker};
    std::thread observer{observerWorker};
    start.store(true, std::memory_order_release);
    dispatcher.join();
    observer.join();
    const auto removal = scheduler.Remove(pair.ThreadIdentity);
    ASTERIA_EXPECT(runner, "inspection serializes with repeated dispatch and Ready requeue",
                   failures.load(std::memory_order_relaxed) == 0U, removal.HasValue());
}

void TestWakeFacadeOwnsExactQueue(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    RegisteredPair pair{registry, issuer};
    ManualClock clock{10U};
    ProcessorScheduler owner{registry, clock, CpuId{0U}};
    ProcessorScheduler foreign{registry, clock, CpuId{0U}};

    const auto enqueued = owner.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto admission = owner.DispatchNext();
    clock.Set(15U);
    const auto completion = owner.Finish(ThreadRunDisposition::Blocked);
    const auto reservation =
        completion.HasValue()
            ? owner.GetWakeFacade().ReserveBlockedEnqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity,
                                                          pair.ThreadStorage, completion.GetValue().Thread.BlockToken)
            : owner.GetWakeFacade().ReserveBlockedEnqueue({}, pair.Context, {}, pair.ThreadStorage, {});
    const auto foreignCommit = reservation.HasValue()
                                   ? foreign.GetWakeFacade().CommitReservedEnqueue(reservation.GetValue())
                                   : foreign.GetWakeFacade().CommitReservedEnqueue({});
    const auto ownerBeforeCommit = owner.GetSnapshot();
    const ThreadError wake = completion.HasValue() ? pair.ThreadStorage.Wake(completion.GetValue().Thread.BlockToken)
                                                   : ThreadError::InvalidBlockToken;
    const auto committed = reservation.HasValue() ? owner.GetWakeFacade().CommitReservedEnqueue(reservation.GetValue())
                                                  : owner.GetWakeFacade().CommitReservedEnqueue({});
    const auto staleCommit = reservation.HasValue()
                                 ? owner.GetWakeFacade().CommitReservedEnqueue(reservation.GetValue())
                                 : owner.GetWakeFacade().CommitReservedEnqueue({});
    const auto removed = owner.Remove(pair.ThreadIdentity);

    ASTERIA_EXPECT(runner, "wake facade resolves reservations only in its scheduler-owned queue", pair.Ready,
                   enqueued.HasValue(), admission.HasValue(), completion.HasValue(),
                   owner.GetWakeFacade().GetProcessor().GetValue() == 0U,
                   foreign.GetWakeFacade().GetProcessor().GetValue() == 0U, reservation.HasValue(),
                   !foreignCommit.HasValue(), foreignCommit.GetError().Error == ReadyQueueError::InvalidReservation,
                   ownerBeforeCommit.Queue.ReservedEntries == 1U, ownerBeforeCommit.Queue.ReadyEntries == 0U,
                   wake == ThreadError::None, committed.HasValue(), !staleCommit.HasValue(),
                   staleCommit.GetError().Error == ReadyQueueError::StaleReservation, removed.HasValue());
}

void TestActiveExecutionBinding(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    RegisteredPair pair{registry, issuer};
    ManualClock clock{10U};
    bool exact{false};
    bool mismatchFaulted{false};
    {
        ProcessorScheduler scheduler{registry, clock, CpuId{0U}};
        const auto enqueued =
            scheduler.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
        const auto admission = scheduler.DispatchNext();
        if (enqueued.HasValue() && admission.HasValue())
        {
            const auto binding = scheduler.GetActiveExecutionBinding(admission.GetValue());
            exact = binding.HasValue() && binding.GetValue().ThreadIdentity.IsEqualTo(pair.ThreadIdentity) &&
                    (binding.GetValue().ThreadStorage == &pair.ThreadStorage) &&
                    binding.GetValue().SchedulingContextIdentity.IsEqualTo(pair.ContextIdentity) &&
                    (binding.GetValue().SchedulingContextStorage == &pair.Context) &&
                    (binding.GetValue().Processor.GetValue() == 0U) &&
                    (binding.GetValue().RunSequence == admission.GetValue().Dispatch.Thread.RunSequence);

            auto mismatched = admission.GetValue();
            ++mismatched.Dispatch.Thread.RunSequence;
            const auto rejected = scheduler.GetActiveExecutionBinding(mismatched);
            const auto snapshot = scheduler.GetSnapshot();
            mismatchFaulted = !rejected.HasValue() &&
                              (rejected.GetError().Error == ProcessorActiveExecutionBindingError::AdmissionMismatch) &&
                              (snapshot.State == ProcessorSchedulerState::Faulted) &&
                              snapshot.ActiveThread.IsEqualTo(pair.ThreadIdentity) &&
                              (snapshot.Queue.ClaimedEntries == 1U);
        }
    }

    clock.Set(15U);
    const auto contextFinished = pair.Context.EndRun(CpuId{0U}, MonotonicTime{15U});
    const auto threadFinished = pair.ThreadStorage.FinishRun(CpuId{0U}, ThreadRunDisposition::Exited, 1U);
    ASTERIA_EXPECT(runner, "active binding is exact and a run-sequence mismatch faults retained state", pair.Ready,
                   exact, mismatchFaulted, contextFinished.HasValue(), threadFinished.HasValue());
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ProcessorSchedulerTests"};
    Asteria::Test::TestFourProcessorHomes(runner);
    Asteria::Test::TestDispatchLifecycle(runner);
    Asteria::Test::TestBudgetDeferral(runner);
    Asteria::Test::TestRetryBoundaries(runner);
    Asteria::Test::TestFullQueueDeferral(runner);
    Asteria::Test::TestFailureAndExit(runner);
    Asteria::Test::TestDeadlineRanksCurrentWindow(runner);
    Asteria::Test::TestRejectedCandidatesAreSkipped(runner);
    Asteria::Test::TestCrossProcessorPlacement(runner);
    Asteria::Test::TestConcurrentInspection(runner);
    Asteria::Test::TestWakeFacadeOwnsExactQueue(runner);
    Asteria::Test::TestActiveExecutionBinding(runner);
    return runner.Finish();
}
