// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/ProcessorRunController.hpp"
#include "Kernel/Time/Timer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"
#include <cstdio>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::Size;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::ProcessorRunController;
using Kernel::Scheduler::ProcessorRunDispatchMode;
using Kernel::Scheduler::ProcessorRunEntryFailure;
using Kernel::Scheduler::ProcessorRunError;
using Kernel::Scheduler::ProcessorRunExternalStopError;
using Kernel::Scheduler::ProcessorRunExternalStopHardwareState;
using Kernel::Scheduler::ProcessorRunExternalStopObservation;
using Kernel::Scheduler::ProcessorRunFailure;
using Kernel::Scheduler::ProcessorRunState;
using Kernel::Scheduler::ProcessorSchedulerError;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadState;
using Kernel::Time::ProcessorDeadlineSource;
using Kernel::Time::ProcessorDeadlineError;
using Kernel::Time::ProcessorDeadlineState;
using Kernel::Time::Timer;
using Kernel::Time::TimerId;
using Kernel::Time::TimerQueue;
using Kernel::Time::TimerState;
using Hal::Time::ILocalTimer;
using Hal::Time::LocalTimerError;
using Platform::Synchronization::SpinLock;

class ManualLocalTimer final : public ILocalTimer
{
public:
    explicit constexpr ManualLocalTimer(const Foundation::UInt64 ticks = 0U) noexcept : m_ticks{ticks}
    {
    }

    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override
    {
        ++m_readCalls;
        const Foundation::UInt64 observedTicks = m_ticks;
        if (m_readsBeforeTicksChange == 1U)
        {
            m_ticks = m_ticksAfterRead;
        }
        else
        {
            m_ticks += m_ticksPerRead;
        }
        m_readsBeforeTicksChange -= (m_readsBeforeTicksChange != 0U) ? 1U : 0U;
        return observedTicks;
    }

    [[nodiscard]] LocalTimerError ProgramDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        ++m_programCalls;
        m_lastProgrammedDeadline = deadlineTicks;
        if (m_nextProgramError != LocalTimerError::None)
        {
            const LocalTimerError error = m_nextProgramError;
            m_nextProgramError = LocalTimerError::None;
            if (m_nextProgramObservedTicks != 0U)
            {
                m_ticks = m_nextProgramObservedTicks;
                m_nextProgramObservedTicks = 0U;
            }
            return error;
        }
        if (deadlineTicks <= m_ticks)
        {
            return LocalTimerError::DeadlineReached;
        }
        m_armedDeadline = deadlineTicks;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError CancelDeadline() noexcept override
    {
        ++m_cancelCalls;
        if (m_nextCancelError != LocalTimerError::None)
        {
            const LocalTimerError error = m_nextCancelError;
            m_nextCancelError = LocalTimerError::None;
            return error;
        }
        m_armedDeadline = 0U;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError
    AcknowledgeExternallyMaskedDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        ++m_acknowledgeCalls;
        if (m_nextAcknowledgeError != LocalTimerError::None)
        {
            const LocalTimerError error = m_nextAcknowledgeError;
            m_nextAcknowledgeError = LocalTimerError::None;
            return error;
        }
        if ((deadlineTicks == 0U) || (deadlineTicks != m_armedDeadline))
        {
            return LocalTimerError::InvalidDeadline;
        }
        m_armedDeadline = 0U;
        return LocalTimerError::None;
    }

    void SetTicks(const Foundation::UInt64 ticks) noexcept
    {
        m_ticks = ticks;
    }

    /// The ticks change to the given value after the given number of reads.
    void SetTicksAfterReads(const Size reads, const Foundation::UInt64 ticks) noexcept
    {
        m_ticksAfterRead = ticks;
        m_readsBeforeTicksChange = reads;
    }

    void SetTicksPerRead(const Foundation::UInt64 ticks) noexcept
    {
        m_ticksPerRead = ticks;
    }

    void FailNextProgram(const LocalTimerError error, const Foundation::UInt64 observedTicks = 0U) noexcept
    {
        m_nextProgramError = error;
        m_nextProgramObservedTicks = observedTicks;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetArmedDeadline() const noexcept
    {
        return m_armedDeadline;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetLastProgrammedDeadline() const noexcept
    {
        return m_lastProgrammedDeadline;
    }

    [[nodiscard]] constexpr Size GetProgramCalls() const noexcept
    {
        return m_programCalls;
    }

    [[nodiscard]] constexpr Size GetReadCalls() const noexcept
    {
        return m_readCalls;
    }

    [[nodiscard]] constexpr Size GetCancelCalls() const noexcept
    {
        return m_cancelCalls;
    }

    [[nodiscard]] constexpr Size GetAcknowledgeCalls() const noexcept
    {
        return m_acknowledgeCalls;
    }

    void FailNextAcknowledge(const LocalTimerError error) noexcept
    {
        m_nextAcknowledgeError = error;
    }

private:
    mutable Foundation::UInt64 m_ticks{0U};
    mutable Foundation::UInt64 m_ticksAfterRead{0U};
    mutable Foundation::UInt64 m_ticksPerRead{0U};
    Foundation::UInt64 m_armedDeadline{0U};
    Foundation::UInt64 m_lastProgrammedDeadline{0U};
    Foundation::UInt64 m_nextProgramObservedTicks{0U};
    mutable Size m_readCalls{0U};
    Size m_programCalls{0U};
    Size m_cancelCalls{0U};
    Size m_acknowledgeCalls{0U};
    LocalTimerError m_nextProgramError{LocalTimerError::None};
    LocalTimerError m_nextCancelError{LocalTimerError::None};
    LocalTimerError m_nextAcknowledgeError{LocalTimerError::None};
    mutable Size m_readsBeforeTicksChange{0U};
};

[[nodiscard]] constexpr SchedulingContextConfiguration
MakeConfiguration(const Foundation::UInt8 priority = 8U, const Foundation::UInt64 budget = 20U,
                  const Foundation::UInt64 deadline = 80U, const Foundation::UInt64 period = 100U) noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = budget,
        .PeriodTicks = period,
        .RelativeDeadlineTicks = deadline,
        .Weight = 10U,
        .Priority = priority,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

class RegisteredPair final
{
public:
    RegisteredPair(ObjectRegistry& registry, RootCapabilityIssuer& issuer,
                   const SchedulingContextConfiguration configuration = MakeConfiguration()) noexcept
        : Context{registry}, m_issuer{issuer}
    {
        if ((ThreadStorage.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (Context.Initialize(configuration, ResourceDomainId{7U}, MonotonicTime{0U}) !=
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

[[nodiscard]] bool
FailureIs(const ProcessorRunFailure& failure, const ProcessorRunError error, const bool activeRunRetained = false,
          const ProcessorRunExternalStopError externalStop = ProcessorRunExternalStopError::None) noexcept
{
    return (failure.Error == error) &&
           (failure.ActiveRunRetained == static_cast<Foundation::UInt8>(activeRunRetained ? 1U : 0U)) &&
           (failure.ExternalStop == externalStop) && BytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

[[nodiscard]] constexpr Foundation::UInt8 SourceMask(const ProcessorDeadlineSource source) noexcept
{
    return static_cast<Foundation::UInt8>(source);
}

[[nodiscard]] ProcessorRunExternalStopObservation
MakeExternalStop(const Kernel::Scheduler::ProcessorRunAdmission& admission, const Foundation::UInt64 observedTicks,
                 const Foundation::UInt64 externalFrameToken = 1U) noexcept
{
    return ProcessorRunExternalStopObservation{
        .Thread = admission.Scheduler.Dispatch.Context.Thread,
        .SchedulingContext = admission.Scheduler.Context,
        .RunSequence = admission.Scheduler.Dispatch.Thread.RunSequence,
        .ProgrammedDeadlineTicks = admission.Deadline.SelectedDeadlineTicks,
        .ObservedTicks = observedTicks,
        .ExternalFrameToken = externalFrameToken,
        .Processor = admission.Scheduler.Dispatch.Context.Processor,
        .HardwareState = ProcessorRunExternalStopHardwareState::TimerMaskedReadBackAndInterruptAcknowledged,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

template <typename TMutation>
[[nodiscard, gnu::noinline]] bool
ExternalStopObservationRejectionIsTerminal(const TMutation& mutation,
                                           const ProcessorRunExternalStopError expectedError) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer{10U};
    TimerQueue queue{registry, timer};
    RegisteredPair pair{registry, issuer};
    ProcessorRunController controller{registry, queue, timer, CpuId{0U}};
    const auto enqueued =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto admission = controller.DispatchNext();
    if (!pair.Ready || !enqueued.HasValue() || !admission.HasValue())
    {
        return false;
    }

    ProcessorRunExternalStopObservation observation = MakeExternalStop(admission.GetValue(), 30U);
    mutation(observation, registry);
    const auto before = controller.GetSnapshot();
    const Size reads = timer.GetReadCalls();
    const Size programs = timer.GetProgramCalls();
    const Size cancels = timer.GetCancelCalls();
    const Size acknowledgements = timer.GetAcknowledgeCalls();
    const Foundation::UInt64 armedDeadline = timer.GetArmedDeadline();
    const auto rejected = controller.FinishExternallyStoppedRunAsReady(observation);
    const auto after = controller.GetSnapshot();

    return !rejected.HasValue() &&
           FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true, expectedError) &&
           (after.State == ProcessorRunState::Faulted) &&
           (after.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active) &&
           after.Scheduler.ActiveThread.IsEqualTo(pair.ThreadIdentity) &&
           (after.Scheduler.Queue.ReadyEntries == before.Scheduler.Queue.ReadyEntries) &&
           (after.Scheduler.Queue.NextSequence == before.Scheduler.Queue.NextSequence) &&
           (after.Deadline.State == ProcessorDeadlineState::Armed) &&
           (after.Deadline.SelectedDeadlineTicks == before.Deadline.SelectedDeadlineTicks) &&
           (pair.ThreadStorage.GetSnapshot().State == ThreadState::Running) && (timer.GetReadCalls() == reads) &&
           (timer.GetProgramCalls() == programs) && (timer.GetCancelCalls() == cancels) &&
           (timer.GetAcknowledgeCalls() == acknowledgements) && (timer.GetArmedDeadline() == armedDeadline);
}

[[nodiscard]] ObjectId RegisterTimer(Timer& timer, RootCapabilityIssuer& issuer) noexcept
{
    if (timer.Initialize(ResourceDomainId{9U}) != Kernel::Time::TimerError::None)
    {
        return {};
    }
    const auto result = issuer.CreateObject(timer);
    return result.HasValue() ? result.GetValue() : ObjectId{};
}

[[nodiscard]] bool RetireTimer(const ObjectId object, RootCapabilityIssuer& issuer) noexcept
{
    return object.IsValid() && (issuer.DestroyObject(object) == Kernel::Object::ObjectRegistryError::Ok) &&
           issuer.CollectDestroyedObject().HasValue();
}

void TestRunLifecycle(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair pair{registry, issuer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U}};

    const auto enqueued =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto admission = controller.DispatchNext();
    const auto active = controller.GetSnapshot();
    const auto activeBinding = controller.GetActiveExecutionBinding();
    ASTERIA_EXPECT(
        runner, "a run is published only after its absolute stop is armed", pair.Ready, enqueued.HasValue(),
        admission.HasValue(), admission.GetValue().Scheduler.Context.IsEqualTo(pair.ContextIdentity),
        admission.GetValue().Scheduler.StopAtTicks == 30U, admission.GetValue().Deadline.SelectedDeadlineTicks == 30U,
        admission.GetValue().Deadline.SelectedSources == SourceMask(ProcessorDeadlineSource::RunStop),
        admission.GetValue().Deadline.State == ProcessorDeadlineState::Armed, active.State == ProcessorRunState::Active,
        active.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active, activeBinding.HasValue(),
        activeBinding.GetValue().ThreadIdentity.IsEqualTo(pair.ThreadIdentity),
        activeBinding.GetValue().ThreadStorage == &pair.ThreadStorage,
        activeBinding.GetValue().SchedulingContextIdentity.IsEqualTo(pair.ContextIdentity),
        activeBinding.GetValue().SchedulingContextStorage == &pair.Context,
        activeBinding.GetValue().Processor.GetValue() == 0U,
        activeBinding.GetValue().RunSequence == admission.GetValue().Scheduler.Dispatch.Thread.RunSequence,
        controller.GetWakeFacade().GetProcessor().GetValue() == 0U, localTimer.GetArmedDeadline() == 30U);

    const auto duplicateDispatch = controller.DispatchNext();
    ASTERIA_EXPECT(runner, "an active run cannot publish a second admission", !duplicateDispatch.HasValue(),
                   FailureIs(duplicateDispatch.GetError(), ProcessorRunError::AlreadyActive, true));

    const auto duplicateEnqueue =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto activeRemoval = controller.Remove(pair.ThreadIdentity);
    ASTERIA_EXPECT(runner, "queue rejections report that the active run remains retained", !duplicateEnqueue.HasValue(),
                   FailureIs(duplicateEnqueue.GetError(), ProcessorRunError::SchedulerRejected, true),
                   !activeRemoval.HasValue(),
                   FailureIs(activeRemoval.GetError(), ProcessorRunError::SchedulerRejected, true));

    const auto rejectedCompletion = controller.Finish(static_cast<ThreadRunDisposition>(0U));
    const auto retained = controller.GetSnapshot();
    ASTERIA_EXPECT(runner, "an invalid completion retains the exact active run for retry",
                   !rejectedCompletion.HasValue(),
                   FailureIs(rejectedCompletion.GetError(), ProcessorRunError::CompletionRejected, true),
                   retained.State == ProcessorRunState::Active,
                   retained.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active,
                   pair.ThreadStorage.GetSnapshot().State == ThreadState::Running);

    localTimer.SetTicks(15U);
    const auto completion = controller.Finish(ThreadRunDisposition::Ready);
    const auto removal = controller.Remove(pair.ThreadIdentity);
    const auto empty = controller.DispatchNext();
    const auto idle = controller.GetSnapshot();
    const auto idleBinding = controller.GetActiveExecutionBinding();
    ASTERIA_EXPECT(runner, "completion returns idle and an empty selection disarms the old run stop",
                   completion.HasValue(), completion.GetValue().Thread.State == ThreadState::Ready, removal.HasValue(),
                   !empty.HasValue(), FailureIs(empty.GetError(), ProcessorRunError::NoRunnableThread),
                   empty.GetError().Scheduler.Error == ProcessorSchedulerError::NoRunnableThread,
                   empty.GetError().DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
                   empty.GetError().DeadlineReceipt.Reprogrammed == 1U, idle.State == ProcessorRunState::Idle,
                   !idleBinding.HasValue(),
                   idleBinding.GetError().Error == Kernel::Scheduler::ProcessorActiveExecutionBindingError::NotActive,
                   idle.Deadline.State == ProcessorDeadlineState::Disarmed, localTimer.GetCancelCalls() == 1U);
}

void TestIdleAndDeferredRetry(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{15U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair exhausted{registry, issuer, MakeConfiguration(200U, 10U, 20U, 100U)};
    const bool exhaustedBudget =
        exhausted.Context.StartRun(exhausted.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
        exhausted.Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue();
    ProcessorRunController idleController{registry, timerQueue, localTimer, CpuId{0U}};
    const auto exhaustedEnqueue = idleController.Enqueue(exhausted.ContextIdentity, exhausted.Context,
                                                         exhausted.ThreadIdentity, exhausted.ThreadStorage);
    const auto idleRetry = idleController.DispatchNext();
    ASTERIA_EXPECT(runner, "an idle CPU arms the earliest deferred replenishment", exhausted.Ready, exhaustedBudget,
                   exhaustedEnqueue.HasValue(), !idleRetry.HasValue(),
                   FailureIs(idleRetry.GetError(), ProcessorRunError::NoRunnableThread),
                   idleRetry.GetError().Scheduler.RetryAtTicks == 100U,
                   idleRetry.GetError().DeadlineReceipt.SelectedDeadlineTicks == 100U,
                   idleRetry.GetError().DeadlineReceipt.SelectedSources ==
                       SourceMask(ProcessorDeadlineSource::SchedulerRetry),
                   localTimer.GetArmedDeadline() == 100U);

    localTimer.SetTicks(100U);
    const auto replenished = idleController.DispatchNext();
    localTimer.SetTicks(105U);
    const auto replenishedCompletion = idleController.Finish(ThreadRunDisposition::Ready);
    const auto exhaustedRemoval = idleController.Remove(exhausted.ThreadIdentity);
    const auto disarmed = idleController.DispatchNext();
    ASTERIA_EXPECT(runner, "the replenishment boundary admits work and later removes its wakeup",
                   replenished.HasValue(), replenished.GetValue().Scheduler.StopAtTicks == 110U,
                   replenished.GetValue().Deadline.SelectedDeadlineTicks == 110U, replenishedCompletion.HasValue(),
                   exhaustedRemoval.HasValue(), !disarmed.HasValue(),
                   disarmed.GetError().DeadlineReceipt.State == ProcessorDeadlineState::Disarmed);

    ManualLocalTimer deferredTimer{15U};
    TimerQueue deferredTimerQueue{registry, deferredTimer};
    RegisteredPair earlyDeferred{registry, issuer, MakeConfiguration(200U, 10U, 20U, 60U)};
    RegisteredPair eligible{registry, issuer, MakeConfiguration(50U, 100U, 200U, 300U)};
    RegisteredPair eligiblePeer{registry, issuer, MakeConfiguration(50U, 100U, 200U, 300U)};
    const bool preparedDeferred =
        earlyDeferred.Context.StartRun(earlyDeferred.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
        earlyDeferred.Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue();
    ProcessorRunController deferredController{registry, deferredTimerQueue, deferredTimer, CpuId{0U}};
    const bool deferredEnqueued =
        deferredController
            .Enqueue(earlyDeferred.ContextIdentity, earlyDeferred.Context, earlyDeferred.ThreadIdentity,
                     earlyDeferred.ThreadStorage)
            .HasValue() &&
        deferredController
            .Enqueue(eligible.ContextIdentity, eligible.Context, eligible.ThreadIdentity, eligible.ThreadStorage)
            .HasValue();
    const auto deferredAdmission = deferredController.DispatchNext();
    deferredTimer.SetTicks(20U);
    const auto deferredCompletion = deferredController.Finish(ThreadRunDisposition::Ready);
    const auto deferredRemoval = deferredController.Remove(earlyDeferred.ThreadIdentity);
    const auto eligibleRemoval = deferredController.Remove(eligible.ThreadIdentity);
    const auto deferredDisarm = deferredController.DispatchNext();
    ASTERIA_EXPECT(runner, "a skipped higher-ranked replenishment preempts a longer eligible run", earlyDeferred.Ready,
                   eligible.Ready, preparedDeferred, deferredEnqueued, deferredAdmission.HasValue(),
                   deferredAdmission.GetValue().Scheduler.Context.IsEqualTo(eligible.ContextIdentity),
                   deferredAdmission.GetValue().Scheduler.StopAtTicks == 115U,
                   deferredAdmission.GetValue().Scheduler.SchedulerRetryAtTicks == 60U,
                   deferredAdmission.GetValue().Deadline.SelectedDeadlineTicks == 60U,
                   deferredAdmission.GetValue().Deadline.SelectedSources ==
                       SourceMask(ProcessorDeadlineSource::SchedulerRetry),
                   deferredCompletion.HasValue(), deferredRemoval.HasValue(), eligibleRemoval.HasValue(),
                   !deferredDisarm.HasValue());
}

void TestTimerQueueBoundary(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    Timer timer{localTimer};
    const ObjectId timerObject = RegisterTimer(timer, issuer);
    bool lifecyclePassed{false};
    {
        TimerQueue timerQueue{registry, localTimer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U}};
        const auto armed = timerQueue.Arm(TimerId{registry, timerObject}, timer, MonotonicTime{25U});
        const auto enqueued =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
        const auto admission = controller.DispatchNext();
        localTimer.SetTicks(15U);
        const auto completion = controller.Finish(ThreadRunDisposition::Ready);
        const auto removal = controller.Remove(pair.ThreadIdentity);
        const auto cancelled =
            armed.HasValue()
                ? timerQueue.Cancel(TimerId{registry, timerObject}, timer, armed.GetValue().Timer.Token)
                : Foundation::Result<Kernel::Time::TimerQueueCancelReceipt, Kernel::Time::TimerQueueFailure>::Failure(
                      {});
        const auto empty = controller.DispatchNext();
        lifecyclePassed =
            timerObject.IsValid() && pair.Ready && armed.HasValue() && enqueued.HasValue() && admission.HasValue() &&
            (admission.GetValue().Scheduler.StopAtTicks == 30U) &&
            (admission.GetValue().Deadline.SelectedDeadlineTicks == 25U) &&
            (admission.GetValue().Deadline.SelectedSources == SourceMask(ProcessorDeadlineSource::TimerQueue)) &&
            completion.HasValue() && removal.HasValue() && cancelled.HasValue() && !empty.HasValue() &&
            (empty.GetError().DeadlineReceipt.State == ProcessorDeadlineState::Disarmed) &&
            (localTimer.GetLastProgrammedDeadline() == 25U);
    }
    ASTERIA_EXPECT(runner, "the earliest TimerQueue boundary overrides a later run stop", lifecyclePassed,
                   RetireTimer(timerObject, issuer));
}

void TestPendingTimerAndCrossedDeadline(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{0U};
    Timer timer{localTimer};
    const ObjectId timerObject = RegisterTimer(timer, issuer);
    bool pendingPassed{false};
    {
        TimerQueue timerQueue{registry, localTimer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U}};
        const auto armed = timerQueue.Arm(TimerId{registry, timerObject}, timer, MonotonicTime{10U});
        localTimer.SetTicks(10U);
        const auto firstClaim = timerQueue.ClaimDue();
        const Size claimedProgramCalls = localTimer.GetProgramCalls();
        const auto claimedBlocked = controller.DispatchNext();
        const auto pending =
            firstClaim.HasValue()
                ? timerQueue.CancelClaim(firstClaim.GetValue().GetClaimToken())
                : Foundation::Result<Kernel::Time::TimerQueueState, Kernel::Time::TimerQueueFailure>::Failure({});
        const auto enqueued =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
        const Size programCallsBefore = localTimer.GetProgramCalls();
        const auto blocked = controller.DispatchNext();
        const auto secondClaim = timerQueue.ClaimDue();
        const Kernel::Time::TimerExpiration expiration =
            secondClaim.HasValue() ? secondClaim.GetValue().GetExpiration() : Kernel::Time::TimerExpiration{};
        const auto committed =
            secondClaim.HasValue()
                ? timerQueue.CommitClaim(secondClaim.GetValue().GetClaimToken())
                : Foundation::Result<Kernel::Time::TimerQueueState, Kernel::Time::TimerQueueFailure>::Failure({});
        const auto acknowledged = timer.Acknowledge(expiration.Token);

        localTimer.SetTicks(11U);
        const auto admission = controller.DispatchNext();
        localTimer.SetTicks(16U);
        const auto completion = controller.Finish(ThreadRunDisposition::Ready);
        const auto removal = controller.Remove(pair.ThreadIdentity);
        const auto empty = controller.DispatchNext();
        pendingPassed =
            timerObject.IsValid() && pair.Ready && armed.HasValue() && firstClaim.HasValue() &&
            !claimedBlocked.HasValue() && FailureIs(claimedBlocked.GetError(), ProcessorRunError::TimerWorkPending) &&
            (claimedBlocked.GetError().TimerQueue.ClaimedEntries == 1U) &&
            (localTimer.GetProgramCalls() == claimedProgramCalls + 1U) && pending.HasValue() && enqueued.HasValue() &&
            !blocked.HasValue() && FailureIs(blocked.GetError(), ProcessorRunError::TimerWorkPending) &&
            (blocked.GetError().TimerQueue.PendingEntries == 1U) &&
            (localTimer.GetProgramCalls() == programCallsBefore + 1U) && secondClaim.HasValue() &&
            committed.HasValue() && acknowledged.HasValue() &&
            (acknowledged.GetValue().State == TimerState::Disarmed) && admission.HasValue() && completion.HasValue() &&
            removal.HasValue() && !empty.HasValue();
    }
    ASTERIA_EXPECT(runner, "claimed or pending timer delivery blocks entry until it resolves", pendingPassed,
                   RetireTimer(timerObject, issuer));

    ManualLocalTimer crossedTimer{10U};
    TimerQueue crossedQueue{registry, crossedTimer};
    RegisteredPair crossedPair{registry, issuer};
    ProcessorRunController crossedController{registry, crossedQueue, crossedTimer, CpuId{0U}};
    const auto crossedEnqueue = crossedController.Enqueue(crossedPair.ContextIdentity, crossedPair.Context,
                                                          crossedPair.ThreadIdentity, crossedPair.ThreadStorage);
    crossedTimer.FailNextProgram(LocalTimerError::DeadlineReached, 30U);
    const auto crossed = crossedController.DispatchNext();
    const auto crossedSnapshot = crossedController.GetSnapshot();
    const auto crossedRemoval = crossedController.Remove(crossedPair.ThreadIdentity);
    const auto crossedEmpty = crossedController.DispatchNext();
    ASTERIA_EXPECT(runner, "a deadline crossed before context entry rolls the run back to Ready", crossedPair.Ready,
                   crossedEnqueue.HasValue(), !crossed.HasValue(),
                   FailureIs(crossed.GetError(), ProcessorRunError::DeadlineReached),
                   crossed.GetError().Deadline.ObservedTicks == 30U, crossedSnapshot.State == ProcessorRunState::Idle,
                   crossedSnapshot.Scheduler.Queue.ReadyEntries == 1U,
                   crossedPair.ThreadStorage.GetSnapshot().State == ThreadState::Ready, crossedRemoval.HasValue(),
                   !crossedEmpty.HasValue());
}

void TestCooperativeBootstrapLifecycle(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair pair{registry, issuer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    const auto enqueued =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto admission = controller.DispatchNext();
    const Size readsBeforeValidation = localTimer.GetReadCalls();
    ProcessorRunEntryFailure entryFailure{};
    const bool entryAccepted =
        admission.HasValue() && controller.ValidateActiveAdmission(admission.GetValue(), entryFailure);
    const bool validationReadTimerOnce = localTimer.GetReadCalls() == (readsBeforeValidation + 1U);
    const auto activeRefresh = controller.RefreshTimerQueueDeadline();
    localTimer.SetTicks(15U);
    const auto completion = controller.Finish(ThreadRunDisposition::Exited, 37U);
    const auto empty = controller.DispatchNext();
    const auto snapshot = controller.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "one cooperative bootstrap run exits with no deadline hardware writes", pair.Ready, enqueued.HasValue(),
        admission.HasValue(), admission.GetValue().Mode == ProcessorRunDispatchMode::CooperativeBootstrap,
        BytesAreZero(admission.GetValue().Reserved, sizeof(admission.GetValue().Reserved)),
        admission.GetValue().Deadline.SelectedDeadlineTicks == 0U,
        admission.GetValue().Deadline.ObservedTicks == admission.GetValue().Scheduler.Dispatch.StartTicks,
        admission.GetValue().Deadline.State == ProcessorDeadlineState::Disarmed,
        admission.GetValue().Deadline.SelectedSources == 0U, admission.GetValue().Deadline.Reprogrammed == 0U,
        entryAccepted, validationReadTimerOnce, !activeRefresh.HasValue(),
        FailureIs(activeRefresh.GetError(), ProcessorRunError::AlreadyActive, true), completion.HasValue(),
        completion.GetValue().Thread.State == ThreadState::Exited, completion.GetValue().Thread.ExitCode == 37U,
        !empty.HasValue(), FailureIs(empty.GetError(), ProcessorRunError::NoRunnableThread),
        empty.GetError().DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
        empty.GetError().DeadlineReceipt.Reprogrammed == 0U, snapshot.State == ProcessorRunState::Idle,
        snapshot.Mode == ProcessorRunDispatchMode::CooperativeBootstrap,
        snapshot.Deadline.State == ProcessorDeadlineState::Disarmed, localTimer.GetProgramCalls() == 0U,
        localTimer.GetCancelCalls() == 0U);
}

void TestCooperativeBootstrapDeadlineValidation(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair pair{registry, issuer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    const auto enqueued =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto admission = controller.DispatchNext();
    const Size readsBeforeValidation = localTimer.GetReadCalls();
    if (admission.HasValue())
    {
        localTimer.SetTicks(admission.GetValue().Scheduler.StopAtTicks);
    }
    ProcessorRunEntryFailure entryFailure{};
    const bool entryAccepted =
        admission.HasValue() && controller.ValidateActiveAdmission(admission.GetValue(), entryFailure);
    const Size readsAfterValidation = localTimer.GetReadCalls();
    const auto retainedSnapshot = controller.GetSnapshot();
    const auto rollback = controller.RollbackActiveAdmission();
    const auto rolledBackSnapshot = controller.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "cooperative entry rejects its exact stop boundary and requeues without timer writes", pair.Ready,
        enqueued.HasValue(), admission.HasValue(), !entryAccepted, readsAfterValidation == (readsBeforeValidation + 1U),
        localTimer.GetReadCalls() == (readsAfterValidation + 1U),
        entryFailure.Error == ProcessorRunError::DeadlineReached,
        entryFailure.Deadline.Error == ProcessorDeadlineError::DeadlineReached,
        entryFailure.Deadline.SelectedDeadlineTicks == admission.GetValue().Scheduler.StopAtTicks,
        entryFailure.Deadline.ObservedTicks == admission.GetValue().Scheduler.StopAtTicks,
        entryFailure.Deadline.HardwareError == LocalTimerError::None,
        entryFailure.Deadline.AffectedSources ==
            static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::RunStop),
        entryFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
        entryFailure.DeadlineReceipt.ObservedTicks == admission.GetValue().Scheduler.Dispatch.StartTicks,
        entryFailure.ActiveRunRetained == 1U, retainedSnapshot.State == ProcessorRunState::Active,
        retainedSnapshot.Scheduler.ActiveThread.IsEqualTo(pair.ThreadIdentity), rollback.HasValue(),
        rollback.GetValue().State == ProcessorDeadlineState::Disarmed,
        rolledBackSnapshot.State == ProcessorRunState::Idle, rolledBackSnapshot.Scheduler.Queue.ReadyEntries == 1U,
        pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready, localTimer.GetProgramCalls() == 0U,
        localTimer.GetCancelCalls() == 0U);
}

void TestCooperativeBootstrapClockFailures(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};

    ManualLocalTimer reversedTimer{10U};
    TimerQueue reversedQueue{registry, reversedTimer};
    RegisteredPair reversedPair{registry, issuer};
    ProcessorRunController reversedController{registry, reversedQueue, reversedTimer, CpuId{0U},
                                              ProcessorRunDispatchMode::CooperativeBootstrap};
    const auto reversedEnqueue = reversedController.Enqueue(reversedPair.ContextIdentity, reversedPair.Context,
                                                            reversedPair.ThreadIdentity, reversedPair.ThreadStorage);
    const auto reversedAdmission = reversedController.DispatchNext();
    reversedTimer.SetTicks(9U);
    const Size readsBeforeReversal = reversedTimer.GetReadCalls();
    ProcessorRunEntryFailure reversedFailure{};
    const bool reversedAccepted = reversedAdmission.HasValue() && reversedController.ValidateActiveAdmission(
                                                                      reversedAdmission.GetValue(), reversedFailure);
    const auto reversedSnapshot = reversedController.GetSnapshot();

    constexpr Foundation::UInt64 maximumUInt64{~Foundation::UInt64{0U}};
    ManualLocalTimer exhaustedTimer{10U};
    TimerQueue exhaustedQueue{registry, exhaustedTimer};
    RegisteredPair exhaustedPair{registry, issuer};
    ProcessorRunController exhaustedController{registry, exhaustedQueue, exhaustedTimer, CpuId{0U},
                                               ProcessorRunDispatchMode::CooperativeBootstrap};
    const auto exhaustedEnqueue =
        exhaustedController.Enqueue(exhaustedPair.ContextIdentity, exhaustedPair.Context, exhaustedPair.ThreadIdentity,
                                    exhaustedPair.ThreadStorage);
    const auto exhaustedAdmission = exhaustedController.DispatchNext();
    exhaustedTimer.SetTicks(maximumUInt64);
    const Size readsBeforeExhaustion = exhaustedTimer.GetReadCalls();
    ProcessorRunEntryFailure exhaustedFailure{};
    const bool exhaustedAccepted =
        exhaustedAdmission.HasValue() &&
        exhaustedController.ValidateActiveAdmission(exhaustedAdmission.GetValue(), exhaustedFailure);
    const auto exhaustedSnapshot = exhaustedController.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "cooperative entry maps reversed and exhausted clocks without timer writes", reversedPair.Ready,
        reversedEnqueue.HasValue(), reversedAdmission.HasValue(), !reversedAccepted,
        reversedTimer.GetReadCalls() == (readsBeforeReversal + 1U),
        reversedFailure.Error == ProcessorRunError::DeadlineRejected,
        reversedFailure.Deadline.Error == ProcessorDeadlineError::ClockReversed,
        reversedFailure.Deadline.ObservedTicks == 9U,
        reversedFailure.Deadline.SelectedDeadlineTicks == reversedAdmission.GetValue().Scheduler.StopAtTicks,
        reversedFailure.Deadline.AffectedSources == static_cast<Foundation::UInt8>(ProcessorDeadlineSource::RunStop),
        reversedFailure.ActiveRunRetained == 1U, reversedSnapshot.State == ProcessorRunState::Faulted,
        reversedSnapshot.Deadline.State == ProcessorDeadlineState::Disarmed, reversedTimer.GetProgramCalls() == 0U,
        reversedTimer.GetCancelCalls() == 0U, exhaustedPair.Ready, exhaustedEnqueue.HasValue(),
        exhaustedAdmission.HasValue(), !exhaustedAccepted,
        exhaustedTimer.GetReadCalls() == (readsBeforeExhaustion + 1U),
        exhaustedFailure.Error == ProcessorRunError::DeadlineRejected,
        exhaustedFailure.Deadline.Error == ProcessorDeadlineError::ClockExhausted,
        exhaustedFailure.Deadline.ObservedTicks == maximumUInt64,
        exhaustedFailure.Deadline.SelectedDeadlineTicks == exhaustedAdmission.GetValue().Scheduler.StopAtTicks,
        exhaustedFailure.Deadline.AffectedSources == static_cast<Foundation::UInt8>(ProcessorDeadlineSource::RunStop),
        exhaustedFailure.ActiveRunRetained == 1U, exhaustedSnapshot.State == ProcessorRunState::Faulted,
        exhaustedSnapshot.Deadline.State == ProcessorDeadlineState::Disarmed, exhaustedTimer.GetProgramCalls() == 0U,
        exhaustedTimer.GetCancelCalls() == 0U);
}

void TestCooperativeBootstrapRejectsDeferredWork(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{15U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair deferred{registry, issuer, MakeConfiguration(200U, 10U, 20U, 100U)};
    RegisteredPair eligible{registry, issuer, MakeConfiguration(50U, 100U, 200U, 300U)};
    RegisteredPair eligiblePeer{registry, issuer, MakeConfiguration(50U, 100U, 200U, 300U)};
    const bool exhaustedBudget =
        deferred.Context.StartRun(deferred.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
        deferred.Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue();
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    const bool enqueued =
        controller.Enqueue(deferred.ContextIdentity, deferred.Context, deferred.ThreadIdentity, deferred.ThreadStorage)
            .HasValue() &&
        controller.Enqueue(eligible.ContextIdentity, eligible.Context, eligible.ThreadIdentity, eligible.ThreadStorage)
            .HasValue() &&
        controller
            .Enqueue(eligiblePeer.ContextIdentity, eligiblePeer.Context, eligiblePeer.ThreadIdentity,
                     eligiblePeer.ThreadStorage)
            .HasValue();
    const auto queueBefore = controller.GetSnapshot().Scheduler.Queue;
    const auto deferredBefore = deferred.Context.GetSnapshot();
    const auto eligibleBefore = eligible.Context.GetSnapshot();
    localTimer.SetTicksPerRead(1U);
    const auto rejected = controller.DispatchNext();
    const auto snapshot = controller.GetSnapshot();
    const auto deferredAfter = deferred.Context.GetSnapshot();
    const auto eligibleAfter = eligible.Context.GetSnapshot();
    const auto eligibleThreadAfter = eligible.ThreadStorage.GetSnapshot();
    const auto deferredRemoval = controller.Remove(deferred.ThreadIdentity);
    localTimer.SetTicksPerRead(0U);
    localTimer.SetTicks(20U);
    const auto firstEligible = controller.DispatchNext();
    localTimer.SetTicks(25U);
    const auto completion = controller.Finish(ThreadRunDisposition::Exited);
    const auto eligiblePeerRemoval = controller.Remove(eligiblePeer.ThreadIdentity);

    ASTERIA_EXPECT(
        runner, "cooperative deferred rejection preserves budget and FIFO order", deferred.Ready, eligible.Ready,
        eligiblePeer.Ready, exhaustedBudget, enqueued, !rejected.HasValue(),
        FailureIs(rejected.GetError(), ProcessorRunError::DeferredWorkPending),
        rejected.GetError().Scheduler.RetryAtTicks == 100U, snapshot.State == ProcessorRunState::Idle,
        snapshot.Scheduler.Queue.ReadyEntries == 3U, snapshot.Scheduler.Queue.NextSequence == queueBefore.NextSequence,
        deferredAfter.RemainingBudgetTicks == deferredBefore.RemainingBudgetTicks,
        eligibleAfter.RemainingBudgetTicks == eligibleBefore.RemainingBudgetTicks,
        deferred.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
        eligibleThreadAfter.State == ThreadState::Ready, deferredRemoval.HasValue(), firstEligible.HasValue(),
        firstEligible.GetValue().Scheduler.ThreadStorage == &eligible.ThreadStorage, completion.HasValue(),
        eligiblePeerRemoval.HasValue(), localTimer.GetProgramCalls() == 0U, localTimer.GetCancelCalls() == 0U);
}

void TestCooperativeBootstrapRejectsTimerWork(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    Timer timer{localTimer};
    const ObjectId timerObject = RegisterTimer(timer, issuer);
    bool lifecyclePassed{false};
    {
        TimerQueue timerQueue{registry, localTimer};
        ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U},
                                          ProcessorRunDispatchMode::CooperativeBootstrap};
        const auto armed = timerQueue.Arm(TimerId{registry, timerObject}, timer, MonotonicTime{25U});
        const auto rejected = controller.DispatchNext();
        const auto cancelled =
            armed.HasValue()
                ? timerQueue.Cancel(TimerId{registry, timerObject}, timer, armed.GetValue().Timer.Token)
                : Foundation::Result<Kernel::Time::TimerQueueCancelReceipt, Kernel::Time::TimerQueueFailure>::Failure(
                      {});
        lifecyclePassed = timerObject.IsValid() && armed.HasValue() && !rejected.HasValue() &&
                          FailureIs(rejected.GetError(), ProcessorRunError::TimerWorkPending) &&
                          (rejected.GetError().TimerQueue.ScheduledEntries == 1U) &&
                          (rejected.GetError().TimerQueue.NextDeadlineTicks == 25U) && cancelled.HasValue() &&
                          (localTimer.GetProgramCalls() == 0U) && (localTimer.GetCancelCalls() == 0U);
    }
    ASTERIA_EXPECT(runner, "cooperative bootstrap rejects advertised TimerQueue work without hardware writes",
                   lifecyclePassed, RetireTimer(timerObject, issuer));
}

void TestCooperativeBootstrapRequiresDeadlineRefresh(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    Timer timer{localTimer};
    const ObjectId timerObject = RegisterTimer(timer, issuer);
    bool lifecyclePassed{false};
    {
        TimerQueue timerQueue{registry, localTimer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U},
                                          ProcessorRunDispatchMode::CooperativeBootstrap};
        const TimerId timerId{registry, timerObject};
        const auto armed = timerQueue.Arm(timerId, timer, MonotonicTime{25U});
        const auto scheduledRefresh = controller.RefreshTimerQueueDeadline();
        const auto cancelled = armed.HasValue() ? timerQueue.Cancel(timerId, timer, armed.GetValue().Timer.Token)
                                                : timerQueue.Cancel(timerId, timer, {});
        const auto enqueued =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
        const Size readsBeforeReject = localTimer.GetReadCalls();
        const Size programsBeforeReject = localTimer.GetProgramCalls();
        const Size cancelsBeforeReject = localTimer.GetCancelCalls();
        const auto firstRejected = controller.DispatchNext();
        const auto secondRejected = controller.DispatchNext();
        const auto rejectedSnapshot = controller.GetSnapshot();
        const bool repeatedRejectionIsInert = (localTimer.GetReadCalls() == readsBeforeReject) &&
                                              (localTimer.GetProgramCalls() == programsBeforeReject) &&
                                              (localTimer.GetCancelCalls() == cancelsBeforeReject);
        const auto emptyRefresh = controller.RefreshTimerQueueDeadline();
        const Size programsBeforeRetry = localTimer.GetProgramCalls();
        const Size cancelsBeforeRetry = localTimer.GetCancelCalls();
        const auto admitted = controller.DispatchNext();
        const auto completed = controller.Finish(ThreadRunDisposition::Ready);
        const auto completedSnapshot = controller.GetSnapshot();

        lifecyclePassed =
            pair.Ready && timerObject.IsValid() && armed.HasValue() && scheduledRefresh.HasValue() &&
            (scheduledRefresh.GetValue().State == ProcessorDeadlineState::Armed) &&
            (scheduledRefresh.GetValue().SelectedDeadlineTicks == 25U) &&
            (scheduledRefresh.GetValue().SelectedSources == SourceMask(ProcessorDeadlineSource::TimerQueue)) &&
            cancelled.HasValue() && enqueued.HasValue() && !firstRejected.HasValue() &&
            FailureIs(firstRejected.GetError(), ProcessorRunError::DeadlineRefreshRequired) &&
            (firstRejected.GetError().Deadline.Error == ProcessorDeadlineError::None) &&
            (firstRejected.GetError().DeadlineReceipt.State == ProcessorDeadlineState::Armed) &&
            (firstRejected.GetError().DeadlineReceipt.SelectedDeadlineTicks == 25U) &&
            (firstRejected.GetError().DeadlineReceipt.SelectedSources ==
             SourceMask(ProcessorDeadlineSource::TimerQueue)) &&
            (firstRejected.GetError().DeadlineReceipt.ObservedTicks == 0U) &&
            (firstRejected.GetError().DeadlineReceipt.Reprogrammed == 0U) && !secondRejected.HasValue() &&
            FailureIs(secondRejected.GetError(), ProcessorRunError::DeadlineRefreshRequired) &&
            (secondRejected.GetError().DeadlineReceipt.State == firstRejected.GetError().DeadlineReceipt.State) &&
            (secondRejected.GetError().DeadlineReceipt.SelectedDeadlineTicks ==
             firstRejected.GetError().DeadlineReceipt.SelectedDeadlineTicks) &&
            (secondRejected.GetError().DeadlineReceipt.SelectedSources ==
             firstRejected.GetError().DeadlineReceipt.SelectedSources) &&
            repeatedRejectionIsInert && (rejectedSnapshot.State == ProcessorRunState::Idle) &&
            (rejectedSnapshot.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Idle) &&
            (rejectedSnapshot.Scheduler.Queue.ReadyEntries == 1U) &&
            (pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready) && emptyRefresh.HasValue() &&
            (emptyRefresh.GetValue().State == ProcessorDeadlineState::Disarmed) &&
            (emptyRefresh.GetValue().SelectedDeadlineTicks == 0U) && (emptyRefresh.GetValue().SelectedSources == 0U) &&
            (emptyRefresh.GetValue().Reprogrammed == 1U) && admitted.HasValue() &&
            (admitted.GetValue().Mode == ProcessorRunDispatchMode::CooperativeBootstrap) &&
            (admitted.GetValue().Deadline.State == ProcessorDeadlineState::Disarmed) &&
            (admitted.GetValue().Deadline.SelectedDeadlineTicks == 0U) &&
            (admitted.GetValue().Deadline.SelectedSources == 0U) &&
            (localTimer.GetProgramCalls() == programsBeforeRetry) &&
            (localTimer.GetCancelCalls() == cancelsBeforeRetry) && completed.HasValue() &&
            (completedSnapshot.State == ProcessorRunState::Idle) &&
            (completedSnapshot.Deadline.State == ProcessorDeadlineState::Disarmed) &&
            (pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready);
    }
    ASTERIA_EXPECT(runner, "cooperative stale TimerQueue deadline requires an inert empty refresh before admission",
                   lifecyclePassed, RetireTimer(timerObject, issuer));
}

void TestRetainedAdmissionStorageIdentity(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair admitted{registry, issuer};
    RegisteredPair forgedStorage{registry, issuer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U}};
    const auto enqueued =
        controller.Enqueue(admitted.ContextIdentity, admitted.Context, admitted.ThreadIdentity, admitted.ThreadStorage);
    const auto admission = controller.DispatchNext();
    Kernel::Scheduler::ProcessorRunAdmission replayed =
        admission.HasValue() ? admission.GetValue() : Kernel::Scheduler::ProcessorRunAdmission{};
    replayed.Scheduler.ThreadStorage = &forgedStorage.ThreadStorage;
    replayed.Scheduler.ContextStorage = &forgedStorage.Context;
    const Size readsBeforeValidation = localTimer.GetReadCalls();
    ProcessorRunEntryFailure entryFailure{};
    const bool accepted = admission.HasValue() && controller.ValidateActiveAdmissionIdentity(replayed, entryFailure);
    const auto snapshot = controller.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "retained admission validation rejects forged scheduler storage", admitted.Ready, forgedStorage.Ready,
        enqueued.HasValue(), admission.HasValue(), !accepted,
        entryFailure.Error == ProcessorRunError::ActiveAdmissionMismatch, entryFailure.ActiveRunRetained == 1U,
        entryFailure.DeadlineReceipt.SelectedDeadlineTicks == admission.GetValue().Deadline.SelectedDeadlineTicks,
        localTimer.GetReadCalls() == readsBeforeValidation, snapshot.State == ProcessorRunState::Faulted,
        snapshot.Scheduler.ActiveThread.IsEqualTo(admitted.ThreadIdentity),
        admitted.ThreadStorage.GetSnapshot().State == ThreadState::Running,
        forgedStorage.ThreadStorage.GetSnapshot().State == ThreadState::Ready);
}

void TestRetainedAdmissionRunSequence(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair pair{registry, issuer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U}};
    const auto enqueued =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto admission = controller.DispatchNext();
    Kernel::Scheduler::ProcessorRunAdmission replayed =
        admission.HasValue() ? admission.GetValue() : Kernel::Scheduler::ProcessorRunAdmission{};
    ++replayed.Scheduler.Dispatch.Thread.RunSequence;
    ProcessorRunEntryFailure entryFailure{};
    const bool accepted = admission.HasValue() && controller.ValidateActiveAdmissionIdentity(replayed, entryFailure);

    ASTERIA_EXPECT(runner, "retained admission validation includes the exact Thread run sequence", pair.Ready,
                   enqueued.HasValue(), admission.HasValue(),
                   admission.GetValue().Scheduler.Dispatch.Thread.RunSequence != 0U, !accepted,
                   entryFailure.Error == ProcessorRunError::ActiveAdmissionMismatch,
                   entryFailure.ActiveRunRetained == 1U, controller.GetSnapshot().State == ProcessorRunState::Faulted);
}

void TestCooperativeAdmissionModeMismatch(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair pair{registry, issuer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    const auto enqueued =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    const auto admission = controller.DispatchNext();
    Kernel::Scheduler::ProcessorRunAdmission replayed =
        admission.HasValue() ? admission.GetValue() : Kernel::Scheduler::ProcessorRunAdmission{};
    replayed.Mode = ProcessorRunDispatchMode::HardwareDeadline;
    ProcessorRunEntryFailure entryFailure{};
    const bool accepted = admission.HasValue() && controller.ValidateActiveAdmission(replayed, entryFailure);
    const auto snapshot = controller.GetSnapshot();

    ASTERIA_EXPECT(runner, "a cooperative admission cannot be replayed as hardware-deadline work", pair.Ready,
                   enqueued.HasValue(), admission.HasValue(), !accepted,
                   entryFailure.Error == ProcessorRunError::ActiveAdmissionMismatch,
                   entryFailure.ActiveRunRetained == 1U, snapshot.State == ProcessorRunState::Faulted,
                   snapshot.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active,
                   localTimer.GetProgramCalls() == 0U, localTimer.GetCancelCalls() == 0U);
}

void TestTimerQueueDeadlineRefresh(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};

    ManualLocalTimer hardwareTimer{5U};
    TimerQueue hardwareQueue{registry, hardwareTimer};
    Timer hardwareQueueTimer{hardwareTimer};
    const ObjectId hardwareTimerObject = RegisterTimer(hardwareQueueTimer, issuer);
    const TimerId hardwareTimerId{registry, hardwareTimerObject};
    ProcessorRunController hardwareController{registry, hardwareQueue, hardwareTimer, CpuId{0U}};
    const auto hardwareArm = hardwareQueue.Arm(hardwareTimerId, hardwareQueueTimer, MonotonicTime{20U});
    const auto hardwareScheduled = hardwareController.RefreshTimerQueueDeadline();
    const auto hardwareCancel = hardwareArm.HasValue() ? hardwareQueue.Cancel(hardwareTimerId, hardwareQueueTimer,
                                                                              hardwareArm.GetValue().Timer.Token)
                                                       : hardwareQueue.Cancel(hardwareTimerId, hardwareQueueTimer, {});
    const auto hardwareEmpty = hardwareController.RefreshTimerQueueDeadline();
    ASTERIA_EXPECT(runner, "hardware mode refresh installs only TimerQueue and disarms an empty queue",
                   hardwareArm.HasValue(), hardwareScheduled.HasValue(),
                   hardwareScheduled.GetValue().SelectedDeadlineTicks == 20U,
                   hardwareScheduled.GetValue().State == ProcessorDeadlineState::Armed,
                   hardwareScheduled.GetValue().SelectedSources == SourceMask(ProcessorDeadlineSource::TimerQueue),
                   hardwareScheduled.GetValue().Reprogrammed == 1U, hardwareCancel.HasValue(), hardwareEmpty.HasValue(),
                   hardwareEmpty.GetValue().SelectedDeadlineTicks == 0U,
                   hardwareEmpty.GetValue().State == ProcessorDeadlineState::Disarmed,
                   hardwareEmpty.GetValue().SelectedSources == 0U, hardwareEmpty.GetValue().Reprogrammed == 1U,
                   hardwareTimer.GetProgramCalls() == 1U, hardwareTimer.GetCancelCalls() == 1U,
                   RetireTimer(hardwareTimerObject, issuer));

    ManualLocalTimer cooperativeTimer{6U};
    TimerQueue cooperativeQueue{registry, cooperativeTimer};
    Timer cooperativeQueueTimer{cooperativeTimer};
    const ObjectId cooperativeTimerObject = RegisterTimer(cooperativeQueueTimer, issuer);
    const TimerId cooperativeTimerId{registry, cooperativeTimerObject};
    ProcessorRunController cooperativeController{registry, cooperativeQueue, cooperativeTimer, CpuId{0U},
                                                 ProcessorRunDispatchMode::CooperativeBootstrap};
    const auto cooperativeEmptyBefore = cooperativeController.RefreshTimerQueueDeadline();
    const auto cooperativeArm = cooperativeQueue.Arm(cooperativeTimerId, cooperativeQueueTimer, MonotonicTime{24U});
    const auto cooperativeScheduled = cooperativeController.RefreshTimerQueueDeadline();
    const auto cooperativeCancel =
        cooperativeArm.HasValue()
            ? cooperativeQueue.Cancel(cooperativeTimerId, cooperativeQueueTimer, cooperativeArm.GetValue().Timer.Token)
            : cooperativeQueue.Cancel(cooperativeTimerId, cooperativeQueueTimer, {});
    const auto cooperativeEmptyAfter = cooperativeController.RefreshTimerQueueDeadline();
    ASTERIA_EXPECT(runner, "cooperative mode explicitly refreshes scheduled and empty timer queues",
                   cooperativeEmptyBefore.HasValue(),
                   cooperativeEmptyBefore.GetValue().State == ProcessorDeadlineState::Disarmed,
                   cooperativeArm.HasValue(), cooperativeScheduled.HasValue(),
                   cooperativeScheduled.GetValue().SelectedDeadlineTicks == 24U,
                   cooperativeScheduled.GetValue().SelectedSources == SourceMask(ProcessorDeadlineSource::TimerQueue),
                   cooperativeCancel.HasValue(), cooperativeEmptyAfter.HasValue(),
                   cooperativeEmptyAfter.GetValue().State == ProcessorDeadlineState::Disarmed,
                   cooperativeTimer.GetProgramCalls() == 1U, cooperativeTimer.GetCancelCalls() == 1U,
                   cooperativeController.GetSnapshot().Mode == ProcessorRunDispatchMode::CooperativeBootstrap,
                   RetireTimer(cooperativeTimerObject, issuer));

    ManualLocalTimer reachedTimer{10U};
    TimerQueue reachedQueue{registry, reachedTimer};
    Timer reachedQueueTimer{reachedTimer};
    const ObjectId reachedTimerObject = RegisterTimer(reachedQueueTimer, issuer);
    const TimerId reachedTimerId{registry, reachedTimerObject};
    ProcessorRunController reachedController{registry, reachedQueue, reachedTimer, CpuId{0U}};
    const auto reachedArm = reachedQueue.Arm(reachedTimerId, reachedQueueTimer, MonotonicTime{15U});
    reachedTimer.SetTicks(15U);
    const auto reached = reachedController.RefreshTimerQueueDeadline(10U);
    const auto reachedSnapshot = reachedController.GetSnapshot();
    const auto reachedCancel = reachedArm.HasValue() ? reachedQueue.Cancel(reachedTimerId, reachedQueueTimer,
                                                                           reachedArm.GetValue().Timer.Token)
                                                     : reachedQueue.Cancel(reachedTimerId, reachedQueueTimer, {});
    ASTERIA_EXPECT(
        runner, "a reached TimerQueue boundary is nonterminal and performs no hardware write", reachedArm.HasValue(),
        !reached.HasValue(), FailureIs(reached.GetError(), ProcessorRunError::DeadlineReached),
        reached.GetError().Deadline.Error == ProcessorDeadlineError::DeadlineReached,
        reached.GetError().Deadline.SelectedDeadlineTicks == 15U, reached.GetError().Deadline.ObservedTicks == 15U,
        reachedSnapshot.State == ProcessorRunState::Idle,
        reachedSnapshot.Deadline.State == ProcessorDeadlineState::Disarmed, reachedTimer.GetProgramCalls() == 0U,
        reachedTimer.GetCancelCalls() == 0U, reachedCancel.HasValue(), RetireTimer(reachedTimerObject, issuer));

    ManualLocalTimer reversedTimer{5U};
    TimerQueue reversedQueue{registry, reversedTimer};
    ProcessorRunController reversedController{registry, reversedQueue, reversedTimer, CpuId{0U}};
    const auto reversed = reversedController.RefreshTimerQueueDeadline(6U);
    const auto reversedSnapshot = reversedController.GetSnapshot();
    ASTERIA_EXPECT(runner, "minimum observed ticks rejects reversal and faults before hardware mutation",
                   !reversed.HasValue(), FailureIs(reversed.GetError(), ProcessorRunError::DeadlineRejected),
                   reversed.GetError().Deadline.Error == ProcessorDeadlineError::ClockReversed,
                   reversed.GetError().Deadline.ObservedTicks == 5U,
                   reversedSnapshot.State == ProcessorRunState::Faulted,
                   reversedSnapshot.Deadline.State == ProcessorDeadlineState::Faulted,
                   reversedTimer.GetProgramCalls() == 0U, reversedTimer.GetCancelCalls() == 0U);

    ManualLocalTimer faultTimer{10U};
    TimerQueue faultQueue{registry, faultTimer};
    Timer faultQueueTimer{faultTimer};
    const ObjectId faultTimerObject = RegisterTimer(faultQueueTimer, issuer);
    const TimerId faultTimerId{registry, faultTimerObject};
    ProcessorRunController faultController{registry, faultQueue, faultTimer, CpuId{0U}};
    const auto faultArm = faultQueue.Arm(faultTimerId, faultQueueTimer, MonotonicTime{30U});
    faultTimer.FailNextProgram(LocalTimerError::HardwareFailure);
    const auto faulted = faultController.RefreshTimerQueueDeadline();
    const Size faultReads = faultTimer.GetReadCalls();
    const Size faultPrograms = faultTimer.GetProgramCalls();
    const auto repeatedFault = faultController.RefreshTimerQueueDeadline();
    const auto faultCancel = faultArm.HasValue()
                                 ? faultQueue.Cancel(faultTimerId, faultQueueTimer, faultArm.GetValue().Timer.Token)
                                 : faultQueue.Cancel(faultTimerId, faultQueueTimer, {});
    ASTERIA_EXPECT(
        runner, "hardware rejection faults refresh and later calls remain hardware-inert", faultArm.HasValue(),
        !faulted.HasValue(), FailureIs(faulted.GetError(), ProcessorRunError::DeadlineRejected),
        faulted.GetError().Deadline.Error == ProcessorDeadlineError::HardwareRejected,
        faulted.GetError().Deadline.HardwareError == LocalTimerError::HardwareFailure, !repeatedFault.HasValue(),
        FailureIs(repeatedFault.GetError(), ProcessorRunError::Faulted),
        faultController.GetSnapshot().State == ProcessorRunState::Faulted, faultTimer.GetReadCalls() == faultReads,
        faultTimer.GetProgramCalls() == faultPrograms, faultCancel.HasValue(), RetireTimer(faultTimerObject, issuer));

    ManualLocalTimer activeTimer{10U};
    TimerQueue activeQueue{registry, activeTimer};
    RegisteredPair activePair{registry, issuer};
    ProcessorRunController activeController{registry, activeQueue, activeTimer, CpuId{0U}};
    const auto activeEnqueue = activeController.Enqueue(activePair.ContextIdentity, activePair.Context,
                                                        activePair.ThreadIdentity, activePair.ThreadStorage);
    const auto activeAdmission = activeController.DispatchNext();
    const Size activeReads = activeTimer.GetReadCalls();
    const Size activePrograms = activeTimer.GetProgramCalls();
    const Size activeCancels = activeTimer.GetCancelCalls();
    const auto activeRefresh = activeController.RefreshTimerQueueDeadline();
    const bool activeRefreshWroteNoHardware = (activeTimer.GetReadCalls() == activeReads + 1U) &&
                                              (activeTimer.GetProgramCalls() == activePrograms) &&
                                              (activeTimer.GetCancelCalls() == activeCancels);
    const ProcessorRunState activeRefreshState = activeController.GetSnapshot().State;
    const auto activeFinish = activeController.Finish(ThreadRunDisposition::Ready);
    const auto activeIdleRefresh = activeController.RefreshTimerQueueDeadline();
    ASTERIA_EXPECT(runner,
                   "an active refresh keeps an unchanged run stop without a hardware "
                   "write and leaves normal completion unchanged",
                   activePair.Ready, activeEnqueue.HasValue(), activeAdmission.HasValue(), activeRefresh.HasValue(),
                   activeRefresh.GetValue().SelectedDeadlineTicks == 30U,
                   activeRefresh.GetValue().SelectedSources == SourceMask(ProcessorDeadlineSource::RunStop),
                   activeRefresh.GetValue().Reprogrammed == 0U, activeRefreshWroteNoHardware,
                   activeRefreshState == ProcessorRunState::Active, activeFinish.HasValue(),
                   activeCancels + 1U == activeTimer.GetCancelCalls(), activeIdleRefresh.HasValue());

    ManualLocalTimer pendingTimer{0U};
    TimerQueue pendingQueue{registry, pendingTimer};
    Timer pendingQueueTimer{pendingTimer};
    const ObjectId pendingTimerObject = RegisterTimer(pendingQueueTimer, issuer);
    const TimerId pendingTimerId{registry, pendingTimerObject};
    ProcessorRunController pendingController{registry, pendingQueue, pendingTimer, CpuId{0U}};
    const auto pendingArm = pendingQueue.Arm(pendingTimerId, pendingQueueTimer, MonotonicTime{12U});
    pendingTimer.SetTicks(12U);
    const auto claimed = pendingQueue.ClaimDue();
    const Size claimedReads = pendingTimer.GetReadCalls();
    const Size claimedPrograms = pendingTimer.GetProgramCalls();
    const Size claimedCancels = pendingTimer.GetCancelCalls();
    const auto claimedRefresh = pendingController.RefreshTimerQueueDeadline();
    const auto restored = claimed.HasValue() ? pendingQueue.CancelClaim(claimed.GetValue().GetClaimToken())
                                             : pendingQueue.CancelClaim({});
    const auto pendingRefresh = pendingController.RefreshTimerQueueDeadline();
    const auto reclaimed = pendingQueue.ClaimDue();
    bool consumed{false};
    if (reclaimed.HasValue())
    {
        consumed = pendingQueue.CommitClaim(reclaimed.GetValue().GetClaimToken()).HasValue() &&
                   pendingQueueTimer.Acknowledge(reclaimed.GetValue().GetExpiration().Token).HasValue();
    }
    const auto drainedRefresh = pendingController.RefreshTimerQueueDeadline();
    ASTERIA_EXPECT(
        runner, "Claimed and Pending refreshes reject without hardware and drain cleanly", pendingArm.HasValue(),
        claimed.HasValue(), !claimedRefresh.HasValue(),
        FailureIs(claimedRefresh.GetError(), ProcessorRunError::TimerWorkPending),
        claimedRefresh.GetError().TimerQueue.ClaimedEntries == 1U, restored.HasValue(), !pendingRefresh.HasValue(),
        FailureIs(pendingRefresh.GetError(), ProcessorRunError::TimerWorkPending),
        pendingRefresh.GetError().TimerQueue.PendingEntries == 1U, pendingTimer.GetReadCalls() == claimedReads + 1U,
        pendingTimer.GetProgramCalls() == claimedPrograms, pendingTimer.GetCancelCalls() == claimedCancels, consumed,
        drainedRefresh.HasValue(), RetireTimer(pendingTimerObject, issuer));
}

// A TimerQueue deadline or a scheduler retry that fires before the active
// run's stop consumes the one-shot. Refreshing the deadline re-arms the
// sources that remain: the queue's work is reported until it is drained, a
// fired retry waits for the next dispatch, and the run stop keeps bounding the
// run, which then ends as an external stop.
void TestActiveDeadlineRefresh(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    Timer timer{localTimer};
    const ObjectId timerObject = RegisterTimer(timer, issuer);
    const TimerId timerId{registry, timerObject};
    {
        TimerQueue queue{registry, localTimer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, localTimer, CpuId{0U}};
        const auto armed = queue.Arm(timerId, timer, MonotonicTime{25U});
        const auto enqueued =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
        const auto admission = controller.DispatchNext();
        localTimer.SetTicks(26U);
        const Size readsBeforeDrain = localTimer.GetReadCalls();
        const Size programsBeforeDrain = localTimer.GetProgramCalls();
        const auto undrained = controller.RefreshTimerQueueDeadline(25U);
        const bool undrainedSampledOnceWithoutWrite = (localTimer.GetReadCalls() == readsBeforeDrain + 1U) &&
                                                      (localTimer.GetProgramCalls() == programsBeforeDrain);
        const auto claim = queue.ClaimDue();
        const bool drained = claim.HasValue() && queue.CommitClaim(claim.GetValue().GetClaimToken()).HasValue() &&
                             timer.Acknowledge(claim.GetValue().GetExpiration().Token).HasValue();
        const auto refreshed = controller.RefreshTimerQueueDeadline(26U);
        const Foundation::UInt64 rearmedTicks = localTimer.GetArmedDeadline();
        ProcessorRunExternalStopObservation stop =
            admission.HasValue() ? MakeExternalStop(admission.GetValue(), 31U) : ProcessorRunExternalStopObservation{};
        stop.ProgrammedDeadlineTicks = refreshed.HasValue() ? refreshed.GetValue().SelectedDeadlineTicks : 0U;
        const auto stopped = controller.FinishExternallyStoppedRunAsReady(stop);
        const auto removal = controller.Remove(pair.ThreadIdentity);
        ASTERIA_EXPECT(runner,
                       "a fired TimerQueue deadline re-arms the active run stop once the "
                       "queue is drained",
                       timerObject.IsValid(), pair.Ready, armed.HasValue(), enqueued.HasValue(), admission.HasValue(),
                       admission.GetValue().Scheduler.StopAtTicks == 30U,
                       admission.GetValue().Deadline.SelectedSources == SourceMask(ProcessorDeadlineSource::TimerQueue),
                       !undrained.HasValue(), FailureIs(undrained.GetError(), ProcessorRunError::DeadlineReached, true),
                       undrained.GetError().Deadline.AffectedSources == SourceMask(ProcessorDeadlineSource::TimerQueue),
                       undrainedSampledOnceWithoutWrite, drained, refreshed.HasValue(),
                       refreshed.GetValue().SelectedDeadlineTicks == 30U,
                       refreshed.GetValue().SelectedSources == SourceMask(ProcessorDeadlineSource::RunStop),
                       refreshed.GetValue().Reprogrammed == 1U, rearmedTicks == 30U, stopped.HasValue(),
                       stopped.GetValue().Thread.State == ThreadState::Ready, removal.HasValue());
    }
    ASTERIA_EXPECT(runner, "the drained Timer generation is released", RetireTimer(timerObject, issuer));

    ManualLocalTimer retryTimer{15U};
    TimerQueue retryQueue{registry, retryTimer};
    RegisteredPair deferred{registry, issuer, MakeConfiguration(200U, 10U, 20U, 60U)};
    RegisteredPair eligible{registry, issuer, MakeConfiguration(50U, 100U, 200U, 300U)};
    const bool exhausted =
        deferred.Context.StartRun(deferred.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
        deferred.Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue();
    ProcessorRunController retryController{registry, retryQueue, retryTimer, CpuId{0U}};
    const bool retryEnqueued =
        retryController
            .Enqueue(deferred.ContextIdentity, deferred.Context, deferred.ThreadIdentity, deferred.ThreadStorage)
            .HasValue() &&
        retryController
            .Enqueue(eligible.ContextIdentity, eligible.Context, eligible.ThreadIdentity, eligible.ThreadStorage)
            .HasValue();
    const auto retryAdmission = retryController.DispatchNext();
    retryTimer.SetTicks(61U);
    const auto retryRefresh = retryController.RefreshTimerQueueDeadline(60U);
    const auto retrySnapshot = retryController.GetSnapshot();
    retryTimer.SetTicks(70U);
    const auto retryCompletion = retryController.Finish(ThreadRunDisposition::Ready);
    const bool retryRemoved = retryController.Remove(deferred.ThreadIdentity).HasValue() &&
                              retryController.Remove(eligible.ThreadIdentity).HasValue();
    ASTERIA_EXPECT(runner, "a fired scheduler retry re-arms the active run stop", deferred.Ready, eligible.Ready,
                   exhausted, retryEnqueued, retryAdmission.HasValue(),
                   retryAdmission.GetValue().Scheduler.StopAtTicks == 115U,
                   retryAdmission.GetValue().Deadline.SelectedDeadlineTicks == 60U, retryRefresh.HasValue(),
                   retryRefresh.GetValue().SelectedDeadlineTicks == 115U,
                   retryRefresh.GetValue().SelectedSources == SourceMask(ProcessorDeadlineSource::RunStop),
                   retryTimer.GetArmedDeadline() == 115U, retrySnapshot.State == ProcessorRunState::Active,
                   retrySnapshot.Deadline.SelectedDeadlineTicks == 115U, retryCompletion.HasValue(), retryRemoved);
}

/// The user preemption path: a fired run stop of the active run is reported by the active refresh,
/// the run finishes Ready, and the next dispatch arms the processor deadline for a future tick,
/// never the stale one, so two CPU-bound threads alternate.
void TestFiredRunStopPreemption(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{0U};
    TimerQueue queue{registry, localTimer};
    RegisteredPair first{registry, issuer, MakeConfiguration(8U, 10U, 20U, 20U)};
    RegisteredPair second{registry, issuer, MakeConfiguration(8U, 10U, 20U, 20U)};
    ProcessorRunController controller{registry, queue, localTimer, CpuId{0U}};
    const bool enqueued =
        controller.Enqueue(first.ContextIdentity, first.Context, first.ThreadIdentity, first.ThreadStorage)
            .HasValue() &&
        controller.Enqueue(second.ContextIdentity, second.Context, second.ThreadIdentity, second.ThreadStorage)
            .HasValue();
    const auto firstAdmission = controller.DispatchNext();
    const Foundation::UInt64 firstArmed = localTimer.GetArmedDeadline();

    // The run stop fires while the first thread runs.
    localTimer.SetTicks(11U);
    const Size programsBeforeRefresh = localTimer.GetProgramCalls();
    const auto fired = controller.RefreshTimerQueueDeadline(10U);
    const bool firedWithoutWrite = localTimer.GetProgramCalls() == programsBeforeRefresh;
    const auto firstCompletion = controller.Finish(ThreadRunDisposition::Ready);
    const auto secondAdmission = controller.DispatchNext();
    const Foundation::UInt64 secondArmed = localTimer.GetArmedDeadline();
    ASTERIA_EXPECT(runner, "a fired run stop finishes the run Ready and arms the next run's future stop", first.Ready,
                   second.Ready, enqueued, firstAdmission.HasValue(),
                   firstAdmission.GetValue().Scheduler.ThreadStorage == &first.ThreadStorage,
                   firstAdmission.GetValue().Scheduler.StopAtTicks == 10U, firstArmed == 10U, !fired.HasValue(),
                   FailureIs(fired.GetError(), ProcessorRunError::DeadlineReached, true),
                   fired.GetError().Deadline.AffectedSources == SourceMask(ProcessorDeadlineSource::RunStop),
                   firedWithoutWrite, firstCompletion.HasValue(),
                   firstCompletion.GetValue().Thread.State == ThreadState::Ready, secondAdmission.HasValue(),
                   secondAdmission.GetValue().Scheduler.ThreadStorage == &second.ThreadStorage,
                   secondAdmission.GetValue().Deadline.Reprogrammed == 1U,
                   secondAdmission.GetValue().Deadline.SelectedDeadlineTicks > 11U,
                   secondArmed == secondAdmission.GetValue().Deadline.SelectedDeadlineTicks);

    // The second run stop fires after the first thread's replenishment.
    localTimer.SetTicks(secondArmed + 1U);
    const auto secondFired = controller.RefreshTimerQueueDeadline(secondArmed);
    const auto secondCompletion = controller.Finish(ThreadRunDisposition::Ready);
    const auto thirdAdmission = controller.DispatchNext();
    const Foundation::UInt64 thirdArmed = localTimer.GetArmedDeadline();
    const auto finalCompletion = controller.Finish(ThreadRunDisposition::Ready);
    const bool removed =
        controller.Remove(first.ThreadIdentity).HasValue() && controller.Remove(second.ThreadIdentity).HasValue();
    ASTERIA_EXPECT(runner, "the threads alternate and every dispatch arms a deadline after the current tick",
                   !secondFired.HasValue(), FailureIs(secondFired.GetError(), ProcessorRunError::DeadlineReached, true),
                   secondCompletion.HasValue(), thirdAdmission.HasValue(),
                   thirdAdmission.GetValue().Scheduler.ThreadStorage == &first.ThreadStorage,
                   thirdAdmission.GetValue().Deadline.Reprogrammed == 1U,
                   thirdAdmission.GetValue().Deadline.SelectedDeadlineTicks > (secondArmed + 1U),
                   thirdArmed == thirdAdmission.GetValue().Deadline.SelectedDeadlineTicks, finalCompletion.HasValue(),
                   removed);
}

void TestExternalStopCompletion(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer localTimer{10U};
    TimerQueue timerQueue{registry, localTimer};
    RegisteredPair first{registry, issuer};
    RegisteredPair second{registry, issuer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U}};

    const bool enqueued =
        controller.Enqueue(first.ContextIdentity, first.Context, first.ThreadIdentity, first.ThreadStorage)
            .HasValue() &&
        controller.Enqueue(second.ContextIdentity, second.Context, second.ThreadIdentity, second.ThreadStorage)
            .HasValue();
    const auto admission = controller.DispatchNext();
    const Size readsBeforeStop = localTimer.GetReadCalls();
    const auto completion =
        admission.HasValue()
            ? controller.FinishExternallyStoppedRunAsReady(MakeExternalStop(admission.GetValue(), 33U))
            : Foundation::Result<Kernel::Scheduler::DispatchCompletion, ProcessorRunFailure>::Failure({});
    const auto completedSnapshot = controller.GetSnapshot();
    const Size readsAfterStop = localTimer.GetReadCalls();

    localTimer.SetTicks(33U);
    const auto peerAdmission = controller.DispatchNext();
    localTimer.SetTicks(35U);
    const auto peerCompletion = controller.Finish(ThreadRunDisposition::Ready);
    localTimer.SetTicks(100U);
    const auto readmission = controller.DispatchNext();

    ASTERIA_EXPECT(runner,
                   "an external stop charges the captured tick, requeues at a fresh FIFO "
                   "position, and later readmits the exact thread generation",
                   first.Ready, second.Ready, enqueued, admission.HasValue(),
                   admission.GetValue().Scheduler.ThreadStorage == &first.ThreadStorage, completion.HasValue(),
                   completion.GetValue().Thread.State == ThreadState::Ready,
                   completion.GetValue().Context.ElapsedTicks == 23U, completion.GetValue().Context.ChargedTicks == 20U,
                   completion.GetValue().Context.OverrunTicks == 3U,
                   completion.GetValue().Context.RemainingBudgetTicks == 0U, readsAfterStop == readsBeforeStop,
                   localTimer.GetAcknowledgeCalls() == 1U, localTimer.GetCancelCalls() == 0U,
                   completedSnapshot.State == ProcessorRunState::Idle,
                   completedSnapshot.Deadline.State == ProcessorDeadlineState::Disarmed,
                   completedSnapshot.Scheduler.Queue.ReadyEntries == 2U, peerAdmission.HasValue(),
                   peerAdmission.GetValue().Scheduler.ThreadStorage == &second.ThreadStorage, peerCompletion.HasValue(),
                   readmission.HasValue(), readmission.GetValue().Scheduler.ThreadStorage == &first.ThreadStorage,
                   readmission.GetValue().Scheduler.Dispatch.Thread.RunSequence ==
                       (admission.GetValue().Scheduler.Dispatch.Thread.RunSequence + 1U));

    localTimer.SetTicks(105U);
    const auto cleanupCompletion = controller.Finish(ThreadRunDisposition::Ready);
    const auto firstRemoval = controller.Remove(first.ThreadIdentity);
    const auto secondRemoval = controller.Remove(second.ThreadIdentity);
    const auto empty = controller.DispatchNext();
    ASTERIA_EXPECT(runner, "the external-stop lifecycle remains removable after exact readmission",
                   cleanupCompletion.HasValue(), firstRemoval.HasValue(), secondRemoval.HasValue(), !empty.HasValue());
}

void TestExternalStopProtocolRejections(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};

    {
        ManualLocalTimer timer{10U};
        TimerQueue queue{registry, timer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, timer, CpuId{0U}};
        const bool prepared =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
        const auto admission = controller.DispatchNext();
        ProcessorRunExternalStopObservation observation = MakeExternalStop(admission.GetValue(), 30U);
        ++observation.RunSequence;
        const Size reads = timer.GetReadCalls();
        const auto rejected = controller.FinishExternallyStoppedRunAsReady(observation);
        const auto snapshot = controller.GetSnapshot();
        ASTERIA_EXPECT(runner,
                       "an external stop with a foreign run sequence is terminal and "
                       "retains the exact active transaction",
                       prepared, admission.HasValue(), !rejected.HasValue(),
                       FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                                 ProcessorRunExternalStopError::AdmissionMismatch),
                       snapshot.State == ProcessorRunState::Faulted,
                       snapshot.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active,
                       snapshot.Deadline.State == ProcessorDeadlineState::Armed,
                       pair.ThreadStorage.GetSnapshot().State == ThreadState::Running, timer.GetReadCalls() == reads,
                       timer.GetAcknowledgeCalls() == 0U, timer.GetArmedDeadline() == 30U);
    }

    {
        ManualLocalTimer timer{10U};
        TimerQueue queue{registry, timer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, timer, CpuId{0U}};
        const bool prepared =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
        const auto admission = controller.DispatchNext();
        const auto rejected = controller.FinishExternallyStoppedRunAsReady(MakeExternalStop(admission.GetValue(), 29U));
        ASTERIA_EXPECT(runner, "an early captured tick is terminal and never enters deadline rollback or retry",
                       prepared, admission.HasValue(), !rejected.HasValue(),
                       FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                                 ProcessorRunExternalStopError::CapturedTimeBeforeDeadline),
                       controller.GetSnapshot().Deadline.State == ProcessorDeadlineState::Armed,
                       timer.GetAcknowledgeCalls() == 0U);
    }

    {
        ManualLocalTimer timer{10U};
        TimerQueue queue{registry, timer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, timer, CpuId{0U}};
        const bool prepared =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
        const auto admission = controller.DispatchNext();
        ProcessorRunExternalStopObservation observation = MakeExternalStop(admission.GetValue(), 30U);
        observation.HardwareState = ProcessorRunExternalStopHardwareState::Unverified;
        const auto rejected = controller.FinishExternallyStoppedRunAsReady(observation);
        ASTERIA_EXPECT(runner, "external completion requires timer-mask readback and one interrupt acknowledgement",
                       prepared, admission.HasValue(), !rejected.HasValue(),
                       FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                                 ProcessorRunExternalStopError::HardwareEvidenceMissing),
                       timer.GetAcknowledgeCalls() == 0U);
    }

    {
        ManualLocalTimer timer{10U};
        TimerQueue queue{registry, timer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, timer, CpuId{0U},
                                          ProcessorRunDispatchMode::CooperativeBootstrap};
        const bool prepared =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
        const auto admission = controller.DispatchNext();
        const auto rejected = controller.FinishExternallyStoppedRunAsReady(MakeExternalStop(admission.GetValue(), 30U));
        ASTERIA_EXPECT(runner, "cooperative bootstrap admissions cannot use external-frame completion", prepared,
                       admission.HasValue(), !rejected.HasValue(),
                       FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                                 ProcessorRunExternalStopError::ModeMismatch),
                       timer.GetAcknowledgeCalls() == 0U);
    }

    constexpr Foundation::UInt64 objectGenerationIncrement{1ULL << 16U};
    const bool exactObservationRejected =
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                observation.Thread = {};
            },
            ProcessorRunExternalStopError::AdmissionMismatch) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry& mutationRegistry) noexcept
            {
                observation.Thread =
                    ThreadId{mutationRegistry, ObjectId{observation.Thread.GetValue() + objectGenerationIncrement}};
            },
            ProcessorRunExternalStopError::AdmissionMismatch) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                observation.SchedulingContext = {};
            },
            ProcessorRunExternalStopError::AdmissionMismatch) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry& mutationRegistry) noexcept
            {
                observation.SchedulingContext = SchedulingContextId{
                    mutationRegistry, ObjectId{observation.SchedulingContext.GetValue() + objectGenerationIncrement}};
            },
            ProcessorRunExternalStopError::AdmissionMismatch) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                observation.Processor = {};
            },
            ProcessorRunExternalStopError::AdmissionMismatch) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                observation.Processor = CpuId{1U};
            },
            ProcessorRunExternalStopError::AdmissionMismatch) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                ++observation.ProgrammedDeadlineTicks;
            },
            ProcessorRunExternalStopError::DeadlineMismatch) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                observation.ExternalFrameToken = 0U;
            },
            ProcessorRunExternalStopError::InvalidObservation) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                observation.Reserved[0U] = 1U;
            },
            ProcessorRunExternalStopError::InvalidObservation) &&
        ExternalStopObservationRejectionIsTerminal(
            [](ProcessorRunExternalStopObservation& observation, ObjectRegistry&) noexcept
            {
                observation.ObservedTicks = ~Foundation::UInt64{0U};
            },
            ProcessorRunExternalStopError::InvalidObservation);
    ASTERIA_EXPECT(runner,
                   "external-stop identity, deadline, token, reserved, and captured-time "
                   "mismatches are terminal and side-effect free",
                   exactObservationRejected);
}

void TestExternalStopDeadlineSourceAndBackend(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};

    ManualLocalTimer queuedTimerBackend{10U};
    Timer queuedTimer{queuedTimerBackend};
    const ObjectId queuedTimerObject = RegisterTimer(queuedTimer, issuer);
    bool queuedPassed{false};
    {
        TimerQueue queue{registry, queuedTimerBackend};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, queuedTimerBackend, CpuId{0U}};
        const auto timerArm = queue.Arm(TimerId{registry, queuedTimerObject}, queuedTimer, MonotonicTime{25U});
        const bool prepared =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
        const auto admission = controller.DispatchNext();
        const auto rejected = controller.FinishExternallyStoppedRunAsReady(MakeExternalStop(admission.GetValue(), 30U));
        const auto cancelled =
            timerArm.HasValue()
                ? queue.Cancel(TimerId{registry, queuedTimerObject}, queuedTimer, timerArm.GetValue().Timer.Token)
                : Foundation::Result<Kernel::Time::TimerQueueCancelReceipt, Kernel::Time::TimerQueueFailure>::Failure(
                      {});
        queuedPassed = queuedTimerObject.IsValid() && timerArm.HasValue() && prepared && admission.HasValue() &&
                       !rejected.HasValue() && cancelled.HasValue() &&
                       FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                                 ProcessorRunExternalStopError::TimerQueueNotEmpty) &&
                       (queuedTimerBackend.GetAcknowledgeCalls() == 0U);
    }
    ASTERIA_EXPECT(runner, "external completion refuses to consume scheduled TimerQueue work", queuedPassed,
                   RetireTimer(queuedTimerObject, issuer));

    ManualLocalTimer tiedTimer{10U};
    Timer timer{tiedTimer};
    const ObjectId timerObject = RegisterTimer(timer, issuer);
    bool tiedPassed{false};
    {
        TimerQueue queue{registry, tiedTimer};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, tiedTimer, CpuId{0U}};
        const auto timerArm = queue.Arm(TimerId{registry, timerObject}, timer, MonotonicTime{30U});
        const bool prepared =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
        const auto admission = controller.DispatchNext();
        const auto cancelled =
            timerArm.HasValue()
                ? queue.Cancel(TimerId{registry, timerObject}, timer, timerArm.GetValue().Timer.Token)
                : Foundation::Result<Kernel::Time::TimerQueueCancelReceipt, Kernel::Time::TimerQueueFailure>::Failure(
                      {});
        const auto rejected = controller.FinishExternallyStoppedRunAsReady(MakeExternalStop(admission.GetValue(), 30U));
        tiedPassed = timerObject.IsValid() && timerArm.HasValue() && prepared && admission.HasValue() &&
                     cancelled.HasValue() &&
                     (admission.GetValue().Deadline.SelectedSources ==
                      static_cast<Foundation::UInt8>(SourceMask(ProcessorDeadlineSource::RunStop) |
                                                     SourceMask(ProcessorDeadlineSource::TimerQueue))) &&
                     !rejected.HasValue() &&
                     FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                               ProcessorRunExternalStopError::DeadlineMismatch) &&
                     (tiedTimer.GetAcknowledgeCalls() == 0U);
    }
    ASTERIA_EXPECT(runner,
                   "a tied RunStop and TimerQueue receipt is rejected even after the "
                   "queue entry is separately removed",
                   tiedPassed, RetireTimer(timerObject, issuer));

    {
        ManualLocalTimer retryTimer{10U};
        TimerQueue queue{registry, retryTimer};
        RegisteredPair deferred{registry, issuer, MakeConfiguration(200U, 10U, 20U, 30U)};
        RegisteredPair eligible{registry, issuer, MakeConfiguration(50U, 20U, 80U, 100U)};
        const bool deferredPrepared =
            deferred.Context.StartRun(deferred.ThreadIdentity, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
            deferred.Context.EndRun(CpuId{0U}, MonotonicTime{10U}).HasValue();
        ProcessorRunController controller{registry, queue, retryTimer, CpuId{0U}};
        const bool prepared =
            controller
                .Enqueue(deferred.ContextIdentity, deferred.Context, deferred.ThreadIdentity, deferred.ThreadStorage)
                .HasValue() &&
            controller
                .Enqueue(eligible.ContextIdentity, eligible.Context, eligible.ThreadIdentity, eligible.ThreadStorage)
                .HasValue();
        const auto admission = controller.DispatchNext();
        const Size reads = retryTimer.GetReadCalls();
        const Size programs = retryTimer.GetProgramCalls();
        const Size cancels = retryTimer.GetCancelCalls();
        const auto rejected =
            admission.HasValue()
                ? controller.FinishExternallyStoppedRunAsReady(MakeExternalStop(admission.GetValue(), 30U))
                : Foundation::Result<Kernel::Scheduler::DispatchCompletion, ProcessorRunFailure>::Failure({});
        ASTERIA_EXPECT(runner,
                       "a tied RunStop and scheduler retry cannot use external-frame "
                       "completion",
                       deferred.Ready, eligible.Ready, deferredPrepared, prepared, admission.HasValue(),
                       admission.GetValue().Scheduler.SchedulerRetryAtTicks == 30U,
                       admission.GetValue().Scheduler.StopAtTicks == 30U,
                       admission.GetValue().Deadline.SelectedSources ==
                           static_cast<Foundation::UInt8>(SourceMask(ProcessorDeadlineSource::RunStop) |
                                                          SourceMask(ProcessorDeadlineSource::SchedulerRetry)),
                       !rejected.HasValue(),
                       FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                                 ProcessorRunExternalStopError::DeadlineMismatch),
                       controller.GetSnapshot().State == ProcessorRunState::Faulted, retryTimer.GetReadCalls() == reads,
                       retryTimer.GetProgramCalls() == programs, retryTimer.GetCancelCalls() == cancels,
                       retryTimer.GetAcknowledgeCalls() == 0U, retryTimer.GetArmedDeadline() == 30U);
    }

    {
        ManualLocalTimer timerBackend{10U};
        TimerQueue queue{registry, timerBackend};
        RegisteredPair pair{registry, issuer};
        ProcessorRunController controller{registry, queue, timerBackend, CpuId{0U}};
        const bool prepared =
            controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
        const auto admission = controller.DispatchNext();
        timerBackend.FailNextAcknowledge(LocalTimerError::InvalidDeadline);
        const auto rejected = controller.FinishExternallyStoppedRunAsReady(MakeExternalStop(admission.GetValue(), 30U));
        const auto snapshot = controller.GetSnapshot();
        ASTERIA_EXPECT(runner,
                       "a backend software-state contradiction is terminal without "
                       "releasing the active scheduler claim",
                       prepared, admission.HasValue(), !rejected.HasValue(),
                       FailureIs(rejected.GetError(), ProcessorRunError::ExternalStopRejected, true,
                                 ProcessorRunExternalStopError::DeadlineConsumptionRejected),
                       rejected.GetError().Deadline.Error == ProcessorDeadlineError::HardwareRejected,
                       snapshot.State == ProcessorRunState::Faulted,
                       snapshot.Deadline.State == ProcessorDeadlineState::Faulted,
                       snapshot.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active,
                       timerBackend.GetAcknowledgeCalls() == 1U, timerBackend.GetArmedDeadline() == 30U);
    }
}

void TestInvalidState(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    ManualLocalTimer localTimer{0U};
    TimerQueue timerQueue{registry, localTimer};
    ProcessorRunController invalid{registry, timerQueue, localTimer, CpuId{4U}};
    const auto dispatch = invalid.DispatchNext();
    const auto finish = invalid.Finish(ThreadRunDisposition::Ready);
    const auto refresh = invalid.RefreshTimerQueueDeadline();
    const auto snapshot = invalid.GetSnapshot();
    ASTERIA_EXPECT(runner, "an invalid processor configuration is terminal without hardware writes",
                   !dispatch.HasValue(), FailureIs(dispatch.GetError(), ProcessorRunError::InvalidConfiguration),
                   !finish.HasValue(), FailureIs(finish.GetError(), ProcessorRunError::InvalidConfiguration),
                   !refresh.HasValue(), FailureIs(refresh.GetError(), ProcessorRunError::InvalidConfiguration),
                   snapshot.State == ProcessorRunState::Faulted,
                   snapshot.Deadline.State == ProcessorDeadlineState::Disarmed, localTimer.GetReadCalls() == 0U,
                   localTimer.GetProgramCalls() == 0U, localTimer.GetCancelCalls() == 0U);
}

void TestClockReversal(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};

    ManualLocalTimer programmingTimer{100U};
    TimerQueue programmingQueue{registry, programmingTimer};
    RegisteredPair programmingPair{registry, issuer};
    ProcessorRunController programmingController{registry, programmingQueue, programmingTimer, CpuId{0U}};
    const auto programmingEnqueue =
        programmingController.Enqueue(programmingPair.ContextIdentity, programmingPair.Context,
                                      programmingPair.ThreadIdentity, programmingPair.ThreadStorage);
    // The clock reverses after the scheduler's selection sample and the
    // admission's start sample, before the deadline controller's sample.
    programmingTimer.SetTicksAfterReads(2U, 10U);
    const auto programmingReversed = programmingController.DispatchNext();
    const auto programmingSnapshot = programmingController.GetSnapshot();
    ASTERIA_EXPECT(runner, "a clock reversal between scheduling and programming retains the run and faults",
                   programmingPair.Ready, programmingEnqueue.HasValue(), !programmingReversed.HasValue(),
                   FailureIs(programmingReversed.GetError(), ProcessorRunError::DeadlineRejected, true),
                   programmingReversed.GetError().Deadline.Error == ProcessorDeadlineError::ClockReversed,
                   programmingReversed.GetError().Deadline.ObservedTicks == 10U,
                   programmingTimer.GetProgramCalls() == 0U, programmingSnapshot.State == ProcessorRunState::Faulted,
                   programmingSnapshot.Deadline.State == ProcessorDeadlineState::Faulted,
                   programmingSnapshot.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active,
                   programmingPair.ThreadStorage.GetSnapshot().State == ThreadState::Running);

    ManualLocalTimer entryTimer{10U};
    TimerQueue entryQueue{registry, entryTimer};
    RegisteredPair entryPair{registry, issuer};
    ProcessorRunController entryController{registry, entryQueue, entryTimer, CpuId{0U}};
    const auto entryEnqueue = entryController.Enqueue(entryPair.ContextIdentity, entryPair.Context,
                                                      entryPair.ThreadIdentity, entryPair.ThreadStorage);
    const auto admission = entryController.DispatchNext();
    entryTimer.SetTicks(9U);
    ProcessorRunEntryFailure entryFailure{};
    const bool entryAccepted =
        admission.HasValue() && entryController.ValidateActiveAdmission(admission.GetValue(), entryFailure);
    const auto entrySnapshot = entryController.GetSnapshot();
    ASTERIA_EXPECT(runner, "a clock reversal during final entry validation is terminal and retains the run",
                   entryPair.Ready, entryEnqueue.HasValue(), admission.HasValue(), !entryAccepted,
                   entryFailure.Error == ProcessorRunError::DeadlineRejected,
                   entryFailure.Deadline.Error == ProcessorDeadlineError::ClockReversed,
                   entryFailure.Deadline.ObservedTicks == 9U, entryFailure.ActiveRunRetained == 1U,
                   BytesAreZero(entryFailure.Reserved, sizeof(entryFailure.Reserved)),
                   entrySnapshot.State == ProcessorRunState::Faulted,
                   entrySnapshot.Deadline.State == ProcessorDeadlineState::Faulted,
                   entrySnapshot.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active,
                   entryPair.ThreadStorage.GetSnapshot().State == ThreadState::Running);
}
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ProcessorRunControllerTests"};
    Asteria::Test::TestRunLifecycle(runner);
    Asteria::Test::TestIdleAndDeferredRetry(runner);
    Asteria::Test::TestTimerQueueBoundary(runner);
    Asteria::Test::TestPendingTimerAndCrossedDeadline(runner);
    Asteria::Test::TestCooperativeBootstrapLifecycle(runner);
    Asteria::Test::TestCooperativeBootstrapDeadlineValidation(runner);
    Asteria::Test::TestCooperativeBootstrapClockFailures(runner);
    Asteria::Test::TestCooperativeBootstrapRejectsDeferredWork(runner);
    Asteria::Test::TestCooperativeBootstrapRejectsTimerWork(runner);
    Asteria::Test::TestCooperativeBootstrapRequiresDeadlineRefresh(runner);
    Asteria::Test::TestRetainedAdmissionStorageIdentity(runner);
    Asteria::Test::TestRetainedAdmissionRunSequence(runner);
    Asteria::Test::TestCooperativeAdmissionModeMismatch(runner);
    Asteria::Test::TestTimerQueueDeadlineRefresh(runner);
    Asteria::Test::TestActiveDeadlineRefresh(runner);
    Asteria::Test::TestFiredRunStopPreemption(runner);
    Asteria::Test::TestExternalStopCompletion(runner);
    Asteria::Test::TestExternalStopProtocolRejections(runner);
    Asteria::Test::TestExternalStopDeadlineSourceAndBackend(runner);
    Asteria::Test::TestInvalidState(runner);
    Asteria::Test::TestClockReversal(runner);
    return runner.Finish();
}
