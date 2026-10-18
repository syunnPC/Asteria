// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <optional>
#include <thread>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/ReadyQueue.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::MaximumReadyQueueEntries;
using Kernel::Scheduler::ReadyQueue;
using Kernel::Scheduler::ReadyQueueError;
using Kernel::Scheduler::ReadyQueueFailure;
using Kernel::Scheduler::ReadyQueueState;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Platform::Synchronization::SpinLock;

[[nodiscard]] constexpr SchedulingContextConfiguration
MakeConfiguration(const SchedulingClass schedulingClass = SchedulingClass::DeadlineSporadic,
                  const SchedulingCriticality criticality = SchedulingCriticality::Normal,
                  const Foundation::UInt8 priority = 8U, const Foundation::UInt64 relativeDeadline = 80U,
                  const Foundation::UInt8 affinity = 0x0FU) noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = relativeDeadline,
        .Weight = 10U,
        .Priority = priority,
        .Criticality = criticality,
        .CpuAffinityMask = affinity,
        .Class = schedulingClass,
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

[[nodiscard]] bool FailureIs(const ReadyQueueFailure& failure, const ReadyQueueError error,
                             const ObjectPinError contextPin = ObjectPinError::Ok,
                             const ObjectPinError threadPin = ObjectPinError::Ok) noexcept
{
    return (failure.Error == error) && (failure.ContextPin == contextPin) && (failure.ThreadPin == threadPin) &&
           (failure.Reserved == 0U);
}

[[nodiscard]] bool StateIs(const ReadyQueueState& state, const Foundation::UInt8 ready, const Foundation::UInt8 claimed,
                           const CpuId processor, const Foundation::UInt8 reserved = 0U) noexcept
{
    return (state.ReadyEntries == ready) && (state.ClaimedEntries == claimed) && (state.ReservedEntries == reserved) &&
           (state.Processor.GetValue() == processor.GetValue()) && BytesAreZero(state.Reserved, sizeof(state.Reserved));
}

class RegisteredPair final
{
public:
    RegisteredPair(ObjectRegistry& registry, RootCapabilityIssuer& issuer,
                   const SchedulingContextConfiguration configuration = MakeConfiguration(),
                   const bool makeReady = true) noexcept
        : Context{registry}, m_registry{registry}, m_issuer{issuer}
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
        ThreadIdentity = ThreadId{m_registry, ThreadObject};
        ContextIdentity = SchedulingContextId{m_registry, ContextObject};
        if ((Context.Bind(ThreadIdentity) != SchedulingContextError::None) ||
            (makeReady && (ThreadStorage.MakeReady() != ThreadError::None)))
        {
            return;
        }
        Ready = true;
    }

    ~RegisteredPair() noexcept
    {
        static_cast<void>(Context.Unbind(ThreadIdentity));
        if (ContextObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ContextObject));
        }
        if (ThreadObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ThreadObject));
        }
        for (Foundation::Size index{0U}; index < 2U; ++index)
        {
            if (!m_issuer.CollectDestroyedObject().HasValue())
            {
                break;
            }
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
    ObjectRegistry& m_registry;
    RootCapabilityIssuer& m_issuer;
};

void TestQuiescingObjectsAreRejected(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ReadyQueue queue{registry, CpuId{0U}};
    RegisteredPair quiescingContext{registry, issuer};
    RegisteredPair quiescingThread{registry, issuer};
    static_cast<void>(issuer.DestroyObject(quiescingContext.ContextObject));
    static_cast<void>(issuer.DestroyObject(quiescingThread.ThreadObject));
    const auto rejectedContextPin = queue.Enqueue(quiescingContext.ContextIdentity, quiescingContext.Context,
                                                  quiescingContext.ThreadIdentity, quiescingContext.ThreadStorage);
    const auto rejectedThreadPin = queue.Enqueue(quiescingThread.ContextIdentity, quiescingThread.Context,
                                                 quiescingThread.ThreadIdentity, quiescingThread.ThreadStorage);
    ASTERIA_EXPECT(
        runner, "quiescing objects cannot enter the ready queue", !rejectedContextPin.HasValue(),
        FailureIs(rejectedContextPin.GetError(), ReadyQueueError::ContextPinRejected, ObjectPinError::ObjectNotFound),
        !rejectedThreadPin.HasValue(),
        FailureIs(rejectedThreadPin.GetError(), ReadyQueueError::ThreadPinRejected, ObjectPinError::Ok,
                  ObjectPinError::ObjectNotFound),
        StateIs(queue.GetState(), 0U, 0U, CpuId{0U}));
}

void TestCapacityAndConcurrency(TestContext& runner) noexcept
{
    constexpr Foundation::Size PairCount{MaximumReadyQueueEntries + 1U};
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Thread threads[PairCount]{};
    std::optional<SchedulingContext> contexts[PairCount]{};
    ObjectId threadObjects[PairCount]{};
    ObjectId contextObjects[PairCount]{};
    ThreadId threadIds[PairCount]{};
    SchedulingContextId contextIds[PairCount]{};
    bool fixtureReady{true};

    for (Foundation::Size index{0U}; index < PairCount; ++index)
    {
        contexts[index].emplace(registry);
        fixtureReady = fixtureReady && (threads[index].Initialize(ResourceDomainId{11U}) == ThreadError::None) &&
                       (contexts[index]->Initialize(MakeConfiguration(), ResourceDomainId{11U}, MonotonicTime{0U}) ==
                        SchedulingContextError::None);
        const auto threadResult = issuer.CreateObject(threads[index]);
        const auto contextResult = issuer.CreateObject(*contexts[index]);
        fixtureReady = fixtureReady && threadResult.HasValue() && contextResult.HasValue();
        if (!threadResult.HasValue() || !contextResult.HasValue())
        {
            break;
        }
        threadObjects[index] = threadResult.GetValue();
        contextObjects[index] = contextResult.GetValue();
        threadIds[index] = ThreadId{registry, threadObjects[index]};
        contextIds[index] = SchedulingContextId{registry, contextObjects[index]};
        fixtureReady = fixtureReady && (contexts[index]->Bind(threadIds[index]) == SchedulingContextError::None) &&
                       (threads[index].MakeReady() == ThreadError::None);
    }

    {
        ReadyQueue queue{registry, CpuId{0U}};
        bool filled{fixtureReady};
        for (Foundation::Size index{0U}; index < MaximumReadyQueueEntries; ++index)
        {
            filled = filled &&
                     queue.Enqueue(contextIds[index], *contexts[index], threadIds[index], threads[index]).HasValue();
        }
        const auto overflow = queue.Enqueue(contextIds[MaximumReadyQueueEntries], *contexts[MaximumReadyQueueEntries],
                                            threadIds[MaximumReadyQueueEntries], threads[MaximumReadyQueueEntries]);
        const ReadyQueueState afterOverflow = queue.GetState();
        ASTERIA_EXPECT(
            runner, "the fixed-capacity queue rejects a thirty-third entry", filled, !overflow.HasValue(),
            FailureIs(overflow.GetError(), ReadyQueueError::QueueFull),
            StateIs(queue.GetState(), static_cast<Foundation::UInt8>(MaximumReadyQueueEntries), 0U, CpuId{0U}),
            afterOverflow.NextSequence == static_cast<Foundation::UInt64>(MaximumReadyQueueEntries + 1U));
    }

    {
        ReadyQueue queue{registry, CpuId{0U}};
        std::atomic<Foundation::Size> enqueuedCount{0U};
        std::atomic<Foundation::Size> enqueueFailures{0U};
        std::atomic<Foundation::Size> producersDone{0U};
        std::atomic<Foundation::Size> observations{0U};
        std::atomic<Foundation::Size> observerFailures{0U};
        std::atomic<Foundation::Size> readyWorkers{0U};
        std::atomic<bool> start{false};
        const auto enqueueWorker = [&queue, &contexts, &contextIds, &threads, &threadIds, &enqueuedCount,
                                    &enqueueFailures, &producersDone, &readyWorkers,
                                    &start](const Foundation::Size firstIndex) noexcept
        {
            static_cast<void>(readyWorkers.fetch_add(1U, std::memory_order_release));
            bool released{false};
            for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
            {
                if (start.load(std::memory_order_acquire))
                {
                    released = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (!released)
            {
                static_cast<void>(enqueueFailures.fetch_add(1U, std::memory_order_relaxed));
                static_cast<void>(producersDone.fetch_add(1U, std::memory_order_relaxed));
                return;
            }

            for (Foundation::Size offset{0U}; offset < 8U; ++offset)
            {
                const Foundation::Size index = firstIndex + offset;
                const auto result =
                    queue.Enqueue(contextIds[index], *contexts[index], threadIds[index], threads[index]);
                if (result.HasValue())
                {
                    static_cast<void>(enqueuedCount.fetch_add(1U, std::memory_order_relaxed));
                }
                else
                {
                    static_cast<void>(enqueueFailures.fetch_add(1U, std::memory_order_relaxed));
                }
            }
            static_cast<void>(producersDone.fetch_add(1U, std::memory_order_relaxed));
        };
        const auto observeWorker =
            [&queue, &producersDone, &observations, &observerFailures, &readyWorkers, &start]() noexcept
        {
            static_cast<void>(readyWorkers.fetch_add(1U, std::memory_order_release));
            bool released{false};
            for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
            {
                if (start.load(std::memory_order_acquire))
                {
                    released = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (!released)
            {
                static_cast<void>(observerFailures.fetch_add(1U, std::memory_order_relaxed));
                return;
            }

            for (Foundation::Size attempt{0U}; attempt < 1'000'000U; ++attempt)
            {
                const ReadyQueueState state = queue.GetState();
                static_cast<void>(observations.fetch_add(1U, std::memory_order_relaxed));
                if ((state.ReadyEntries > MaximumReadyQueueEntries) || (state.ClaimedEntries != 0U) ||
                    (state.Processor.GetValue() != 0U) || !BytesAreZero(state.Reserved, sizeof(state.Reserved)))
                {
                    static_cast<void>(observerFailures.fetch_add(1U, std::memory_order_relaxed));
                }
                if (producersDone.load(std::memory_order_relaxed) == 4U)
                {
                    break;
                }
                std::this_thread::yield();
            }
        };
        std::thread first{enqueueWorker, Foundation::Size{0U}};
        std::thread second{enqueueWorker, Foundation::Size{8U}};
        std::thread third{enqueueWorker, Foundation::Size{16U}};
        std::thread fourth{enqueueWorker, Foundation::Size{24U}};
        std::thread observer{observeWorker};
        bool allWorkersReady{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (readyWorkers.load(std::memory_order_acquire) == 5U)
            {
                allWorkersReady = true;
                break;
            }
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        first.join();
        second.join();
        third.join();
        fourth.join();
        observer.join();

        bool drained{true};
        for (Foundation::Size index{0U}; index < MaximumReadyQueueEntries; ++index)
        {
            const auto claim = queue.ClaimNext(MonotonicTime{0U});
            drained = drained && claim.HasValue() &&
                      (queue.CommitClaim(claim.GetValue().GetToken()) == ReadyQueueError::None);
        }
        ASTERIA_EXPECT(runner, "concurrent producers enqueue every disjoint identity exactly once", fixtureReady,
                       allWorkersReady, enqueuedCount.load(std::memory_order_relaxed) == MaximumReadyQueueEntries,
                       enqueueFailures.load(std::memory_order_relaxed) == 0U,
                       observations.load(std::memory_order_relaxed) != 0U,
                       observerFailures.load(std::memory_order_relaxed) == 0U, drained,
                       StateIs(queue.GetState(), 0U, 0U, CpuId{0U}));
    }

    {
        ReadyQueue queue{registry, CpuId{0U}};
        bool enqueued{fixtureReady};
        constexpr Foundation::Size ConcurrentEntries{MaximumReadyQueueEntries};
        for (Foundation::Size index{0U}; index < ConcurrentEntries; ++index)
        {
            enqueued = enqueued &&
                       queue.Enqueue(contextIds[index], *contexts[index], threadIds[index], threads[index]).HasValue();
        }

        std::atomic<Foundation::Size> committed{0U};
        std::atomic<Foundation::Size> failures{0U};
        std::atomic<Foundation::Size> readyWorkers{0U};
        std::atomic<bool> start{false};
        const auto worker = [&queue, &committed, &failures, &readyWorkers, &start]() noexcept
        {
            static_cast<void>(readyWorkers.fetch_add(1U, std::memory_order_release));
            bool released{false};
            for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
            {
                if (start.load(std::memory_order_acquire))
                {
                    released = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (!released)
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                return;
            }
            for (Foundation::Size attempt{0U}; attempt < 4'096U; ++attempt)
            {
                const auto claim = queue.ClaimNext(MonotonicTime{0U});
                if (claim.HasValue())
                {
                    if (queue.CommitClaim(claim.GetValue().GetToken()) != ReadyQueueError::None)
                    {
                        static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                    }
                    else
                    {
                        static_cast<void>(committed.fetch_add(1U, std::memory_order_relaxed));
                    }
                }
                else if (claim.GetError().Error != ReadyQueueError::Empty)
                {
                    static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                }

                const ReadyQueueState state = queue.GetState();
                if ((state.ReadyEntries == 0U) && (state.ClaimedEntries == 0U))
                {
                    break;
                }
                std::this_thread::yield();
            }
        };
        std::thread first{worker};
        std::thread second{worker};
        std::thread third{worker};
        std::thread fourth{worker};
        bool allWorkersReady{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (readyWorkers.load(std::memory_order_acquire) == 4U)
            {
                allWorkersReady = true;
                break;
            }
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        first.join();
        second.join();
        third.join();
        fourth.join();
        ASTERIA_EXPECT(runner, "concurrent claimers consume each retained entry exactly once", enqueued,
                       allWorkersReady, committed.load(std::memory_order_relaxed) == ConcurrentEntries,
                       failures.load(std::memory_order_relaxed) == 0U, StateIs(queue.GetState(), 0U, 0U, CpuId{0U}));
    }

    for (Foundation::Size index{0U}; index < PairCount; ++index)
    {
        if (contexts[index].has_value())
        {
            static_cast<void>(contexts[index]->Unbind(threadIds[index]));
        }
        if (contextObjects[index].IsValid())
        {
            static_cast<void>(issuer.DestroyObject(contextObjects[index]));
        }
        if (threadObjects[index].IsValid())
        {
            static_cast<void>(issuer.DestroyObject(threadObjects[index]));
        }
    }
    Foundation::Size collected{0U};
    for (Foundation::Size index{0U}; index < (PairCount * 2U); ++index)
    {
        if (issuer.CollectDestroyedObject().HasValue())
        {
            ++collected;
        }
    }
    ASTERIA_EXPECT(runner, "queue destruction releases every retained generation pin", fixtureReady,
                   collected == (PairCount * 2U));
}

} // namespace Asteria::Test

int main()
{
    TestContext runner{"ReadyQueueTests"};
    Asteria::Test::TestQuiescingObjectsAreRejected(runner);
    Asteria::Test::TestCapacityAndConcurrency(runner);
    return runner.Finish();
}
