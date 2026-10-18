// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/DispatchCoordinator.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectDestructionRecord;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::DispatchAdmission;
using Kernel::Scheduler::DispatchCompletion;
using Kernel::Scheduler::DispatchCoordinator;
using Kernel::Scheduler::DispatchCoordinatorError;
using Kernel::Scheduler::DispatchCoordinatorFailure;
using Kernel::Scheduler::DispatchCoordinatorSnapshot;
using Kernel::Scheduler::DispatchCoordinatorState;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextSnapshot;
using Kernel::Scheduler::SchedulingContextState;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadSnapshot;
using Kernel::Scheduler::ThreadState;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

class TestEndpointObject final : public IKernelObject
{
public:
    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::Endpoint;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{1U};
    }
};

[[nodiscard]] SchedulingContextConfiguration MakeConfiguration(const Foundation::UInt8 affinity = 0x0FU) noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 50U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 5U,
        .Priority = 9U,
        .Criticality = SchedulingCriticality::Normal,
        .CpuAffinityMask = affinity,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size size) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ThreadSnapshotsEqual(const ThreadSnapshot& left, const ThreadSnapshot& right) noexcept
{
    return left.BlockToken.IsEqualTo(right.BlockToken) && (left.RunSequence == right.RunSequence) &&
           (left.ExitCode == right.ExitCode) && (left.State == right.State) &&
           (left.RunningProcessor.GetValue() == right.RunningProcessor.GetValue()) &&
           BytesAreZero(left.Reserved, sizeof(left.Reserved)) && BytesAreZero(right.Reserved, sizeof(right.Reserved));
}

[[nodiscard]] bool ThreadSnapshotsDifferOnlyByNextRun(const ThreadSnapshot& before,
                                                      const ThreadSnapshot& after) noexcept
{
    return before.BlockToken.IsEqualTo(after.BlockToken) && (after.RunSequence == (before.RunSequence + 1U)) &&
           (before.ExitCode == after.ExitCode) && (before.State == after.State) &&
           (before.RunningProcessor.GetValue() == after.RunningProcessor.GetValue()) &&
           BytesAreZero(before.Reserved, sizeof(before.Reserved)) &&
           BytesAreZero(after.Reserved, sizeof(after.Reserved));
}

[[nodiscard]] bool ContextSnapshotsEqual(const SchedulingContextSnapshot& left,
                                         const SchedulingContextSnapshot& right) noexcept
{
    return left.BaseThread.IsEqualTo(right.BaseThread) && left.CurrentThread.IsEqualTo(right.CurrentThread) &&
           (left.RemainingBudgetTicks == right.RemainingBudgetTicks) &&
           (left.NextReplenishmentTicks == right.NextReplenishmentTicks) &&
           (left.AbsoluteDeadlineTicks == right.AbsoluteDeadlineTicks) && (left.RunStartTicks == right.RunStartTicks) &&
           (left.State == right.State) && (left.RunningProcessor.GetValue() == right.RunningProcessor.GetValue()) &&
           (left.DonationDepth == right.DonationDepth) && (left.IsBudgetExhausted == right.IsBudgetExhausted) &&
           BytesAreZero(left.Reserved, sizeof(left.Reserved)) && BytesAreZero(right.Reserved, sizeof(right.Reserved));
}

[[nodiscard]] bool CoordinatorSnapshotIs(const DispatchCoordinatorSnapshot& snapshot,
                                         const DispatchCoordinatorState state, const ThreadId thread = {},
                                         const CpuId processor = {}) noexcept
{
    return (snapshot.State == state) && snapshot.Thread.IsEqualTo(thread) &&
           (snapshot.Processor.GetValue() == processor.GetValue()) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] bool FailureIs(const DispatchCoordinatorFailure& failure, const DispatchCoordinatorError error,
                             const ObjectPinError pin = ObjectPinError::Ok,
                             const SchedulingContextError context = SchedulingContextError::None,
                             const ThreadError thread = ThreadError::None) noexcept
{
    return (failure.Error == error) && (failure.Pin == pin) && (failure.Context == context) &&
           (failure.Thread == thread) && BytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

class BoundFixture final
{
public:
    BoundFixture(const bool makeReady = true, const Foundation::UInt8 affinity = 0x0FU) noexcept
        : Registry{RegistryLock}, Issuer{Registry}, Context{Registry}, Clock{10U}, Coordinator{Registry, Clock}
    {
        if (ThreadObject.Initialize(ResourceDomainId{7U}) != ThreadError::None)
        {
            return;
        }
        const auto object = Issuer.CreateObject(ThreadObject);
        if (!object.HasValue())
        {
            return;
        }
        Object = object.GetValue();
        Identity = ThreadId{Registry, Object};
        if ((Context.Initialize(MakeConfiguration(affinity), ResourceDomainId{7U}, MonotonicTime{0U}) !=
             SchedulingContextError::None) ||
            (Context.Bind(Identity) != SchedulingContextError::None))
        {
            return;
        }
        if (makeReady && (ThreadObject.MakeReady() != ThreadError::None))
        {
            return;
        }
        Ready = true;
    }

    ~BoundFixture() noexcept
    {
        if (!Object.IsValid())
        {
            return;
        }
        static_cast<void>(Context.Unbind(Identity));
        static_cast<void>(Issuer.DestroyObject(Object));
        static_cast<void>(Issuer.CollectDestroyedObject());
    }

    [[nodiscard]] bool RetireThread() noexcept
    {
        if (!Object.IsValid() || (Context.Unbind(Identity) != SchedulingContextError::None) ||
            (Issuer.DestroyObject(Object) != ObjectRegistryError::Ok))
        {
            return false;
        }
        const auto collected = Issuer.CollectDestroyedObject();
        if (!collected.HasValue() || (collected.GetValue().Object.GetValue() != Object.GetValue()) ||
            (collected.GetValue().Instance != &ThreadObject))
        {
            return false;
        }
        Object = ObjectId{};
        Identity = ThreadId{};
        return true;
    }

    SpinLock RegistryLock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    Thread ThreadObject;
    SchedulingContext Context;
    ManualClock Clock;
    DispatchCoordinator Coordinator;
    ObjectId Object{};
    ThreadId Identity{};
    bool Ready{false};
};

void TestStartAndFinish(TestContext& runner) noexcept
{
    BoundFixture fixture;
    ASTERIA_EXPECT(runner, "a valid fixture begins with one idle coordinator", fixture.Ready,
                   CoordinatorSnapshotIs(fixture.Coordinator.GetSnapshot(), DispatchCoordinatorState::Idle));

    const auto admission =
        fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{1U});
    ASTERIA_EXPECT(runner, "Start atomically admits the same Thread and CPU budget", admission.HasValue(),
                   admission.GetValue().Context.Thread.IsEqualTo(fixture.Identity),
                   admission.GetValue().StartTicks == 10U, admission.GetValue().Context.MaximumRunTicks == 50U,
                   admission.GetValue().Context.Processor.GetValue() == 1U,
                   admission.GetValue().Thread.State == ThreadState::Running,
                   admission.GetValue().Thread.RunningProcessor.GetValue() == 1U, fixture.Clock.GetReadCount() == 1U,
                   CoordinatorSnapshotIs(fixture.Coordinator.GetSnapshot(), DispatchCoordinatorState::Active,
                                         fixture.Identity, CpuId{1U}),
                   fixture.Context.GetSnapshot().State == SchedulingContextState::Running);

    const ThreadSnapshot activeThread = fixture.ThreadObject.GetSnapshot();
    const SchedulingContextSnapshot activeContext = fixture.Context.GetSnapshot();
    const auto duplicate =
        fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{1U});
    ASTERIA_EXPECT(runner, "a second Start cannot disturb the active transaction", !duplicate.HasValue(),
                   FailureIs(duplicate.GetError(), DispatchCoordinatorError::AlreadyActive),
                   ThreadSnapshotsEqual(activeThread, fixture.ThreadObject.GetSnapshot()),
                   ContextSnapshotsEqual(activeContext, fixture.Context.GetSnapshot()),
                   fixture.Clock.GetReadCount() == 1U);

    const auto invalidDisposition = fixture.Coordinator.Finish(static_cast<ThreadRunDisposition>(0xFFU));
    const auto invalidExitCode = fixture.Coordinator.Finish(ThreadRunDisposition::Ready, 1U);
    ASTERIA_EXPECT(
        runner, "invalid completion input preserves both active state machines", !invalidDisposition.HasValue(),
        FailureIs(invalidDisposition.GetError(), DispatchCoordinatorError::InvalidDisposition, ObjectPinError::Ok,
                  SchedulingContextError::None, ThreadError::InvalidDisposition),
        !invalidExitCode.HasValue(),
        FailureIs(invalidExitCode.GetError(), DispatchCoordinatorError::InvalidDisposition, ObjectPinError::Ok,
                  SchedulingContextError::None, ThreadError::InvalidDisposition),
        ThreadSnapshotsEqual(activeThread, fixture.ThreadObject.GetSnapshot()),
        ContextSnapshotsEqual(activeContext, fixture.Context.GetSnapshot()), fixture.Clock.GetReadCount() == 1U);

    fixture.Clock.Set(5U);
    const auto reversed = fixture.Coordinator.Finish(ThreadRunDisposition::Ready);
    ASTERIA_EXPECT(runner, "a recoverable clock failure leaves the run active for retry", !reversed.HasValue(),
                   FailureIs(reversed.GetError(), DispatchCoordinatorError::SchedulingContextRejected,
                             ObjectPinError::Ok, SchedulingContextError::TimeReversed),
                   ThreadSnapshotsEqual(activeThread, fixture.ThreadObject.GetSnapshot()),
                   ContextSnapshotsEqual(activeContext, fixture.Context.GetSnapshot()),
                   fixture.Coordinator.GetSnapshot().State == DispatchCoordinatorState::Active,
                   fixture.Clock.GetReadCount() == 2U);

    fixture.Clock.Set(15U);
    const auto completion = fixture.Coordinator.Finish(ThreadRunDisposition::Ready);
    ASTERIA_EXPECT(runner, "Finish commits accounting before returning the Thread to Ready", completion.HasValue(),
                   completion.GetValue().Context.Thread.IsEqualTo(fixture.Identity),
                   completion.GetValue().Context.ElapsedTicks == 5U, completion.GetValue().Context.ChargedTicks == 5U,
                   completion.GetValue().Context.RemainingBudgetTicks == 45U,
                   completion.GetValue().Thread.State == ThreadState::Ready,
                   !completion.GetValue().Thread.RunningProcessor.IsValid(), fixture.Clock.GetReadCount() == 3U,
                   CoordinatorSnapshotIs(fixture.Coordinator.GetSnapshot(), DispatchCoordinatorState::Idle));

    const auto idleFinish = fixture.Coordinator.Finish(ThreadRunDisposition::Ready);
    ASTERIA_EXPECT(runner, "Finish rejects an idle coordinator without touching either object", !idleFinish.HasValue(),
                   FailureIs(idleFinish.GetError(), DispatchCoordinatorError::NotActive),
                   fixture.Clock.GetReadCount() == 3U);
    ASTERIA_EXPECT(runner, "completed dispatch releases its transient generation pin", fixture.RetireThread());
}

void TestStartFailureRollback(TestContext& runner) noexcept
{
    BoundFixture restricted{true, 0x01U};
    const ThreadSnapshot beforeThread = restricted.ThreadObject.GetSnapshot();
    const SchedulingContextSnapshot beforeContext = restricted.Context.GetSnapshot();
    const auto rejectedContext =
        restricted.Coordinator.Start(restricted.Context, restricted.ThreadObject, restricted.Identity, CpuId{1U});
    const ThreadSnapshot restrictedAfter = restricted.ThreadObject.GetSnapshot();
    ASTERIA_EXPECT(runner, "context admission failure rolls the transient Thread run back to Ready",
                   !rejectedContext.HasValue(),
                   FailureIs(rejectedContext.GetError(), DispatchCoordinatorError::SchedulingContextRejected,
                             ObjectPinError::Ok, SchedulingContextError::CpuNotAllowed),
                   ThreadSnapshotsDifferOnlyByNextRun(beforeThread, restrictedAfter),
                   ContextSnapshotsEqual(beforeContext, restricted.Context.GetSnapshot()),
                   CoordinatorSnapshotIs(restricted.Coordinator.GetSnapshot(), DispatchCoordinatorState::Idle));

    BoundFixture dormant{false};
    const ThreadSnapshot dormantThread = dormant.ThreadObject.GetSnapshot();
    const SchedulingContextSnapshot dormantContext = dormant.Context.GetSnapshot();
    const auto rejectedThread =
        dormant.Coordinator.Start(dormant.Context, dormant.ThreadObject, dormant.Identity, CpuId{0U});
    ASTERIA_EXPECT(runner, "Thread admission failure occurs before any budget state is committed",
                   !rejectedThread.HasValue(),
                   FailureIs(rejectedThread.GetError(), DispatchCoordinatorError::ThreadRejected, ObjectPinError::Ok,
                             SchedulingContextError::None, ThreadError::InvalidState),
                   ThreadSnapshotsEqual(dormantThread, dormant.ThreadObject.GetSnapshot()),
                   ContextSnapshotsEqual(dormantContext, dormant.Context.GetSnapshot()));

    Thread other;
    static_cast<void>(other.Initialize(ResourceDomainId{9U}));
    static_cast<void>(other.MakeReady());
    const auto otherObject = restricted.Issuer.CreateObject(other);
    const ObjectId otherObjectId = otherObject.HasValue() ? otherObject.GetValue() : ObjectId{};
    const ThreadId otherId{restricted.Registry, otherObjectId};
    const ThreadSnapshot otherReady = other.GetSnapshot();
    const auto notCurrent = restricted.Coordinator.Start(restricted.Context, other, otherId, CpuId{0U});
    const ThreadSnapshot otherRestored = other.GetSnapshot();
    ASTERIA_EXPECT(runner, "a non-current Thread is restored after context identity rejection", !notCurrent.HasValue(),
                   FailureIs(notCurrent.GetError(), DispatchCoordinatorError::SchedulingContextRejected,
                             ObjectPinError::Ok, SchedulingContextError::NotCurrentThread),
                   ThreadSnapshotsDifferOnlyByNextRun(otherReady, otherRestored),
                   ContextSnapshotsEqual(beforeContext, restricted.Context.GetSnapshot()));

    const auto wrongInstance = restricted.Coordinator.Start(restricted.Context, other, restricted.Identity, CpuId{0U});
    ASTERIA_EXPECT(runner, "the typed Thread reference must match the pinned registry object",
                   !wrongInstance.HasValue(),
                   FailureIs(wrongInstance.GetError(), DispatchCoordinatorError::ThreadInstanceMismatch),
                   ThreadSnapshotsEqual(otherRestored, other.GetSnapshot()),
                   CoordinatorSnapshotIs(restricted.Coordinator.GetSnapshot(), DispatchCoordinatorState::Idle));
    bool otherRetired = false;
    if (otherObject.HasValue() && (restricted.Issuer.DestroyObject(otherObject.GetValue()) == ObjectRegistryError::Ok))
    {
        const auto collected = restricted.Issuer.CollectDestroyedObject();
        otherRetired = collected.HasValue() &&
                       (collected.GetValue().Object.GetValue() == otherObject.GetValue().GetValue()) &&
                       (collected.GetValue().Instance == &other);
    }
    ASTERIA_EXPECT(runner, "every rejected Start releases all temporary generation pins", otherRetired,
                   restricted.RetireThread(), dormant.RetireThread());
}

void TestIdentityAndPinFailures(TestContext& runner) noexcept
{
    BoundFixture fixture;
    const ThreadSnapshot beforeThread = fixture.ThreadObject.GetSnapshot();
    const SchedulingContextSnapshot beforeContext = fixture.Context.GetSnapshot();

    SpinLock foreignLock;
    Asteria::TestSupport::TestObjectRegistry foreignRegistry{foreignLock};
    const auto invalid = fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, ThreadId{}, CpuId{0U});
    const auto foreign = fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject,
                                                   ThreadId{foreignRegistry, fixture.Object}, CpuId{0U});
    const auto invalidCpu =
        fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{4U});
    ASTERIA_EXPECT(runner, "identity and CPU validation precede pinning and state changes", !invalid.HasValue(),
                   FailureIs(invalid.GetError(), DispatchCoordinatorError::InvalidThread), !foreign.HasValue(),
                   FailureIs(foreign.GetError(), DispatchCoordinatorError::ThreadRegistryMismatch),
                   !invalidCpu.HasValue(), FailureIs(invalidCpu.GetError(), DispatchCoordinatorError::InvalidCpu),
                   ThreadSnapshotsEqual(beforeThread, fixture.ThreadObject.GetSnapshot()),
                   ContextSnapshotsEqual(beforeContext, fixture.Context.GetSnapshot()));

    fixture.Clock.Set(MonotonicTime::Infinite().GetTicks());
    const auto exhaustedClock =
        fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{0U});
    ASTERIA_EXPECT(runner, "an exhausted clock sample releases the temporary pin without admission",
                   !exhaustedClock.HasValue(),
                   FailureIs(exhaustedClock.GetError(), DispatchCoordinatorError::ClockExhausted),
                   ThreadSnapshotsEqual(beforeThread, fixture.ThreadObject.GetSnapshot()),
                   ContextSnapshotsEqual(beforeContext, fixture.Context.GetSnapshot()));

    TestEndpointObject endpoint;
    const auto endpointObject = fixture.Issuer.CreateObject(endpoint);
    const ObjectId endpointObjectId = endpointObject.HasValue() ? endpointObject.GetValue() : ObjectId{};
    const auto wrongType = fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject,
                                                     ThreadId{fixture.Registry, endpointObjectId}, CpuId{0U});
    ASTERIA_EXPECT(
        runner, "pinning rejects a non-Thread registry generation exactly", !wrongType.HasValue(),
        FailureIs(wrongType.GetError(), DispatchCoordinatorError::ThreadPinRejected, ObjectPinError::TypeMismatch));
    bool endpointRetired = false;
    if (endpointObject.HasValue() &&
        (fixture.Issuer.DestroyObject(endpointObject.GetValue()) == ObjectRegistryError::Ok))
    {
        const auto collected = fixture.Issuer.CollectDestroyedObject();
        endpointRetired = collected.HasValue() &&
                          (collected.GetValue().Object.GetValue() == endpointObject.GetValue().GetValue()) &&
                          (collected.GetValue().Instance == &endpoint);
    }

    BoundFixture quiescing;
    const ObjectRegistryError destroyError = quiescing.Issuer.DestroyObject(quiescing.Object);
    const auto quiescingStart =
        quiescing.Coordinator.Start(quiescing.Context, quiescing.ThreadObject, quiescing.Identity, CpuId{0U});
    ASTERIA_EXPECT(runner, "a quiescing generation cannot acquire a dispatch pin",
                   destroyError == ObjectRegistryError::Ok, !quiescingStart.HasValue(),
                   FailureIs(quiescingStart.GetError(), DispatchCoordinatorError::ThreadPinRejected,
                             ObjectPinError::ObjectNotFound),
                   quiescing.ThreadObject.GetSnapshot().State == ThreadState::Ready,
                   quiescing.Context.GetSnapshot().State == SchedulingContextState::Bound);
    static_cast<void>(quiescing.Context.Unbind(quiescing.Identity));
    const auto collected = quiescing.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "failed dispatch pinning leaves no hidden lifetime reference", collected.HasValue(),
                   collected.GetValue().Object.GetValue() == quiescing.Object.GetValue(),
                   collected.GetValue().Instance == &quiescing.ThreadObject);
    if (collected.HasValue())
    {
        quiescing.Object = ObjectId{};
        quiescing.Identity = ThreadId{};
    }
    ASTERIA_EXPECT(runner, "identity and type failures release every temporary reference", endpointRetired,
                   fixture.RetireThread());
}

void TestBlockedAndExitedCompletion(TestContext& runner) noexcept
{
    BoundFixture blocked;
    const auto admittedBlock =
        blocked.Coordinator.Start(blocked.Context, blocked.ThreadObject, blocked.Identity, CpuId{2U});
    blocked.Clock.Set(20U);
    const auto block = blocked.Coordinator.Finish(ThreadRunDisposition::Blocked);
    ASTERIA_EXPECT(runner, "Blocked completion returns its exact wake token after accounting", admittedBlock.HasValue(),
                   block.HasValue(), block.GetValue().Thread.State == ThreadState::Blocked,
                   block.GetValue().Thread.BlockToken.IsValid(), block.GetValue().Context.ElapsedTicks == 10U,
                   blocked.Context.GetSnapshot().State == SchedulingContextState::Bound,
                   blocked.ThreadObject.Wake(block.GetValue().Thread.BlockToken) == ThreadError::None);

    blocked.Clock.Set(25U);
    const auto admittedExit =
        blocked.Coordinator.Start(blocked.Context, blocked.ThreadObject, blocked.Identity, CpuId{3U});
    blocked.Clock.Set(30U);
    const auto exited = blocked.Coordinator.Finish(ThreadRunDisposition::Exited, 0x1234U);
    ASTERIA_EXPECT(runner, "Exited completion records its code only after budget accounting succeeds",
                   admittedExit.HasValue(), exited.HasValue(), exited.GetValue().Thread.State == ThreadState::Exited,
                   exited.GetValue().Thread.ExitCode == 0x1234U,
                   CoordinatorSnapshotIs(blocked.Coordinator.GetSnapshot(), DispatchCoordinatorState::Idle));

    const SchedulingContextSnapshot stoppedContext = blocked.Context.GetSnapshot();
    const auto terminalStart =
        blocked.Coordinator.Start(blocked.Context, blocked.ThreadObject, blocked.Identity, CpuId{0U});
    ASTERIA_EXPECT(runner, "an exited Thread is rejected before another budget run can begin",
                   !terminalStart.HasValue(),
                   FailureIs(terminalStart.GetError(), DispatchCoordinatorError::ThreadRejected, ObjectPinError::Ok,
                             SchedulingContextError::None, ThreadError::AlreadyExited),
                   ContextSnapshotsEqual(stoppedContext, blocked.Context.GetSnapshot()));
    ASTERIA_EXPECT(runner, "blocked and exited completions release the dispatch pin", blocked.RetireThread());
}

void TestConcurrentStartSerialization(TestContext& runner) noexcept
{
    BoundFixture fixture;
    std::atomic<bool> begin{false};
    std::atomic<Foundation::UInt64> successes{0U};
    std::atomic<Foundation::UInt64> alreadyActive{0U};
    std::atomic<bool> exact{true};
    auto worker = [&]() noexcept
    {
        while (!begin.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        const auto result =
            fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{0U});
        if (result.HasValue())
        {
            static_cast<void>(successes.fetch_add(1U, std::memory_order_relaxed));
        }
        else if (FailureIs(result.GetError(), DispatchCoordinatorError::AlreadyActive))
        {
            static_cast<void>(alreadyActive.fetch_add(1U, std::memory_order_relaxed));
        }
        else
        {
            exact.store(false, std::memory_order_relaxed);
        }
    };

    std::thread first{worker};
    std::thread second{worker};
    begin.store(true, std::memory_order_release);
    first.join();
    second.join();
    fixture.Clock.Set(20U);
    std::atomic<bool> beginFinish{false};
    std::atomic<Foundation::UInt64> completed{0U};
    std::atomic<Foundation::UInt64> notActive{0U};
    auto finishWorker = [&]() noexcept
    {
        while (!beginFinish.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        const auto result = fixture.Coordinator.Finish(ThreadRunDisposition::Ready);
        if (result.HasValue())
        {
            static_cast<void>(completed.fetch_add(1U, std::memory_order_relaxed));
        }
        else if (FailureIs(result.GetError(), DispatchCoordinatorError::NotActive))
        {
            static_cast<void>(notActive.fetch_add(1U, std::memory_order_relaxed));
        }
        else
        {
            exact.store(false, std::memory_order_relaxed);
        }
    };
    std::thread firstFinish{finishWorker};
    std::thread secondFinish{finishWorker};
    beginFinish.store(true, std::memory_order_release);
    firstFinish.join();
    secondFinish.join();
    ASTERIA_EXPECT(runner, "concurrent Start and Finish calls each commit exactly once",
                   exact.load(std::memory_order_relaxed), successes.load(std::memory_order_relaxed) == 1U,
                   alreadyActive.load(std::memory_order_relaxed) == 1U, completed.load(std::memory_order_relaxed) == 1U,
                   notActive.load(std::memory_order_relaxed) == 1U,
                   fixture.ThreadObject.GetSnapshot().State == ThreadState::Ready,
                   CoordinatorSnapshotIs(fixture.Coordinator.GetSnapshot(), DispatchCoordinatorState::Idle));
    ASTERIA_EXPECT(runner, "serialized concurrent admission retains no completed pin", fixture.RetireThread());
}

void TestConcurrentSnapshotInspection(TestContext& runner) noexcept
{
    BoundFixture fixture;
    // Release/acquire phase words order only the test threads. No phase word is
    // published after a successful Start: an Active snapshot must become visible
    // through DispatchCoordinator's own serialization.
    std::atomic<bool> begin{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> exact{true};
    std::atomic<bool> firstInspectionComplete{false};
    std::atomic<bool> activeInspectionComplete{false};
    std::atomic<bool> stressPhaseReady{false};
    std::atomic<bool> stressInspectionComplete{false};
    std::atomic<Foundation::UInt64> observations{0U};
    std::atomic<Foundation::UInt64> activePhaseObservations{0U};
    std::atomic<Foundation::UInt64> stressPhaseObservations{0U};
    std::thread reader{
        [&]() noexcept
        {
            while (!begin.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            const DispatchCoordinatorSnapshot initialSnapshot = fixture.Coordinator.GetSnapshot();
            const bool initialSnapshotExact = CoordinatorSnapshotIs(initialSnapshot, DispatchCoordinatorState::Idle);
            if (!initialSnapshotExact)
            {
                exact.store(false, std::memory_order_relaxed);
            }
            else
            {
                static_cast<void>(observations.fetch_add(1U, std::memory_order_relaxed));
            }
            firstInspectionComplete.store(true, std::memory_order_release);

            while (!stop.load(std::memory_order_acquire))
            {
                const DispatchCoordinatorSnapshot activeSnapshot = fixture.Coordinator.GetSnapshot();
                const bool idle = CoordinatorSnapshotIs(activeSnapshot, DispatchCoordinatorState::Idle);
                const bool active = CoordinatorSnapshotIs(activeSnapshot, DispatchCoordinatorState::Active,
                                                          fixture.Identity, CpuId{0U});
                if (active)
                {
                    static_cast<void>(activePhaseObservations.fetch_add(1U, std::memory_order_relaxed));
                    activeInspectionComplete.store(true, std::memory_order_release);
                    break;
                }
                if (!idle)
                {
                    exact.store(false, std::memory_order_relaxed);
                    activeInspectionComplete.store(true, std::memory_order_release);
                    break;
                }
                static_cast<void>(observations.fetch_add(1U, std::memory_order_relaxed));
                std::this_thread::yield();
            }

            while (!stressPhaseReady.load(std::memory_order_acquire) && !stop.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_acquire))
            {
                const DispatchCoordinatorSnapshot stressSnapshot = fixture.Coordinator.GetSnapshot();
                const bool idle = CoordinatorSnapshotIs(stressSnapshot, DispatchCoordinatorState::Idle);
                const bool active = CoordinatorSnapshotIs(stressSnapshot, DispatchCoordinatorState::Active,
                                                          fixture.Identity, CpuId{0U});
                if (active)
                {
                    static_cast<void>(stressPhaseObservations.fetch_add(1U, std::memory_order_relaxed));
                    stressInspectionComplete.store(true, std::memory_order_release);
                    break;
                }
                if (!idle)
                {
                    exact.store(false, std::memory_order_relaxed);
                    stressInspectionComplete.store(true, std::memory_order_release);
                    break;
                }
                static_cast<void>(observations.fetch_add(1U, std::memory_order_relaxed));
                std::this_thread::yield();
            }

            for (Foundation::UInt64 attempt{0U}; (attempt < 200'000U) && !stop.load(std::memory_order_acquire);
                 ++attempt)
            {
                const DispatchCoordinatorSnapshot snapshot = fixture.Coordinator.GetSnapshot();
                const bool idle = CoordinatorSnapshotIs(snapshot, DispatchCoordinatorState::Idle);
                const bool active =
                    CoordinatorSnapshotIs(snapshot, DispatchCoordinatorState::Active, fixture.Identity, CpuId{0U});
                const bool snapshotExact = idle || active;
                if (!snapshotExact)
                {
                    exact.store(false, std::memory_order_relaxed);
                }
                else
                {
                    static_cast<void>(observations.fetch_add(1U, std::memory_order_relaxed));
                }
                if (!snapshotExact)
                {
                    break;
                }
            }
        }};

    begin.store(true, std::memory_order_release);
    while (!firstInspectionComplete.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    const auto activeAdmission =
        fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{0U});
    bool transitionsExact = activeAdmission.HasValue();
    while (transitionsExact && !activeInspectionComplete.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    if (transitionsExact)
    {
        fixture.Clock.Set(11U);
        transitionsExact = fixture.Coordinator.Finish(ThreadRunDisposition::Ready).HasValue();
    }

    stressPhaseReady.store(true, std::memory_order_release);
    if (transitionsExact)
    {
        fixture.Clock.Set(110U);
        transitionsExact =
            fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{0U}).HasValue();
    }
    while (transitionsExact && !stressInspectionComplete.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    if (transitionsExact)
    {
        fixture.Clock.Set(111U);
        transitionsExact = fixture.Coordinator.Finish(ThreadRunDisposition::Ready).HasValue();
    }

    for (Foundation::UInt64 iteration{1U}; iteration < 1'024U; ++iteration)
    {
        const Foundation::UInt64 startTicks = (iteration * 100U) + 110U;
        fixture.Clock.Set(startTicks);
        if (!fixture.Coordinator.Start(fixture.Context, fixture.ThreadObject, fixture.Identity, CpuId{0U}).HasValue())
        {
            transitionsExact = false;
            break;
        }
        fixture.Clock.Set(startTicks + 1U);
        if (!fixture.Coordinator.Finish(ThreadRunDisposition::Ready).HasValue())
        {
            transitionsExact = false;
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    ASTERIA_EXPECT(runner, "snapshot inspection serializes with repeated dispatch transactions", transitionsExact,
                   exact.load(std::memory_order_relaxed), observations.load(std::memory_order_relaxed) != 0U,
                   activePhaseObservations.load(std::memory_order_relaxed) == 1U,
                   stressPhaseObservations.load(std::memory_order_relaxed) == 1U,
                   CoordinatorSnapshotIs(fixture.Coordinator.GetSnapshot(), DispatchCoordinatorState::Idle));
    ASTERIA_EXPECT(runner, "snapshot inspection leaves no retained dispatch generation", fixture.RetireThread());
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DispatchCoordinatorTests"};
    Asteria::Test::TestStartAndFinish(runner);
    Asteria::Test::TestStartFailureRollback(runner);
    Asteria::Test::TestIdentityAndPinFailures(runner);
    Asteria::Test::TestBlockedAndExitedCompletion(runner);
    Asteria::Test::TestConcurrentStartSerialization(runner);
    Asteria::Test::TestConcurrentSnapshotInspection(runner);
    return runner.Finish();
}
