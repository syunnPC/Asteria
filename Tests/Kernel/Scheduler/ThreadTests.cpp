// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <thread>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/Thread.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadBlockSequencePolicy;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadRunSequencePolicy;
using Kernel::Scheduler::ThreadSnapshot;
using Kernel::Scheduler::ThreadState;
using Platform::Synchronization::SpinLock;

[[nodiscard]] bool SnapshotReservedIsZero(const ThreadSnapshot& snapshot) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(snapshot.Reserved); ++index)
    {
        if (snapshot.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

void TestRegistryAdmission(TestContext& runner) noexcept
{
    Thread thread;
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    RootCapabilityIssuer issuer{registry};
    const auto prematureRegistration = issuer.CreateObject(thread);
    ASTERIA_EXPECT(runner, "ObjectRegistry rejects an uninitialized Thread", !prematureRegistration.HasValue(),
                   prematureRegistration.GetError() == ObjectRegistryError::InvalidArgument,
                   registry.GetActiveCount() == 0U);

    static_cast<void>(thread.Initialize(ResourceDomainId{5U}));
    const auto registered = issuer.CreateObject(thread);
    ASTERIA_EXPECT(runner, "initialized Thread registers as a kernel object", registered.HasValue(),
                   registry.GetActiveCount() == 1U);
    if (registered.HasValue())
    {
        ASTERIA_EXPECT(runner, "unreferenced Thread completes registry destruction",
                       issuer.DestroyObject(registered.GetValue()) == ObjectRegistryError::Ok,
                       issuer.CollectDestroyedObject().HasValue(), registry.GetActiveCount() == 0U);
    }
}

void TestBlockTokenSequence(TestContext& runner) noexcept
{
    Foundation::UInt64 next{0xA5A5U};
    const bool reachesMaximum = ThreadBlockSequencePolicy::TryAdvance(ThreadBlockSequencePolicy::Maximum - 1U, next);
    const bool rejectsWrap = !ThreadBlockSequencePolicy::TryAdvance(ThreadBlockSequencePolicy::Maximum, next);
    ASTERIA_EXPECT(runner, "block-sequence policy reaches UInt64 maximum and rejects wrap atomically", reachesMaximum,
                   rejectsWrap, next == ThreadBlockSequencePolicy::Maximum);

    Foundation::UInt64 nextRun{0x5A5AU};
    const bool runReachesMaximum = ThreadRunSequencePolicy::TryAdvance(ThreadRunSequencePolicy::Maximum - 1U, nextRun);
    const bool runRejectsWrap = !ThreadRunSequencePolicy::TryAdvance(ThreadRunSequencePolicy::Maximum, nextRun);
    ASTERIA_EXPECT(runner, "run-sequence policy reaches UInt64 maximum and rejects wrap atomically", runReachesMaximum,
                   runRejectsWrap, nextRun == ThreadRunSequencePolicy::Maximum);

    Thread thread;
    static_cast<void>(thread.Initialize(ResourceDomainId{10U}));
    static_cast<void>(thread.MakeReady());
    bool exact = true;
    for (Foundation::UInt64 sequence{1U}; sequence <= 4'096U; ++sequence)
    {
        if (thread.BeginRun(CpuId{0U}) != ThreadError::None)
        {
            exact = false;
            break;
        }
        const auto blocked = thread.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        if (!blocked.HasValue() || (blocked.GetValue().BlockToken.GetValue() != sequence) ||
            (blocked.GetValue().RunSequence != sequence) ||
            (thread.Wake(blocked.GetValue().BlockToken) != ThreadError::None))
        {
            exact = false;
            break;
        }
    }
    ASTERIA_EXPECT(runner, "4,096 block generations advance monotonically without stale reuse", exact,
                   thread.GetSnapshot().State == ThreadState::Ready, !thread.GetSnapshot().BlockToken.IsValid());
}

void TestConcurrentInspection(TestContext& runner) noexcept
{
    Thread thread;
    static_cast<void>(thread.Initialize(ResourceDomainId{11U}));
    static_cast<void>(thread.MakeReady());
    std::atomic<bool> begin{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> exact{true};
    std::atomic<Foundation::UInt64> observations{0U};
    std::thread reader{[&]() noexcept
                       {
                           while (!begin.load(std::memory_order_acquire))
                           {
                               std::this_thread::yield();
                           }
                           for (Foundation::UInt64 attempt{0U}; attempt < 200'000U; ++attempt)
                           {
                               const ThreadSnapshot snapshot = thread.GetSnapshot();
                               const bool ready =
                                   (snapshot.State == ThreadState::Ready) && !snapshot.RunningProcessor.IsValid();
                               const bool running = (snapshot.State == ThreadState::Running) &&
                                                    (snapshot.RunningProcessor.GetValue() == 0U);
                               if ((!ready && !running) || snapshot.BlockToken.IsValid() || (snapshot.ExitCode != 0U) ||
                                   !SnapshotReservedIsZero(snapshot))
                               {
                                   exact.store(false, std::memory_order_relaxed);
                                   break;
                               }
                               static_cast<void>(observations.fetch_add(1U, std::memory_order_relaxed));
                               if (stop.load(std::memory_order_acquire))
                               {
                                   break;
                               }
                           }
                       }};

    begin.store(true, std::memory_order_release);
    bool transitionsExact = true;
    for (Foundation::UInt64 iteration{0U}; iteration < 10'000U; ++iteration)
    {
        if ((thread.BeginRun(CpuId{0U}) != ThreadError::None) ||
            !thread.FinishRun(CpuId{0U}, ThreadRunDisposition::Ready).HasValue())
        {
            transitionsExact = false;
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    ASTERIA_EXPECT(runner, "state inspection is serialized with repeated dispatch transitions", transitionsExact,
                   exact.load(std::memory_order_relaxed), observations.load(std::memory_order_relaxed) != 0U,
                   thread.GetSnapshot().State == ThreadState::Ready, thread.GetSnapshot().RunSequence == 10'000U);
}

void TestConcurrentMetadataPublication(TestContext& runner) noexcept
{
    Thread thread;
    std::atomic<bool> readerReady{false};
    std::atomic<bool> beginConcurrentAccess{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> exact{true};
    std::atomic<bool> observedPublishedOwner{false};
    std::atomic<Foundation::UInt64> readCount{0U};
    std::thread reader{[&]() noexcept
                       {
                           readerReady.store(true, std::memory_order_release);
                           bool beginObserved{false};
                           for (Foundation::UInt64 attempt{0U}; attempt < 100'000U; ++attempt)
                           {
                               if (beginConcurrentAccess.load(std::memory_order_acquire))
                               {
                                   beginObserved = true;
                                   break;
                               }
                               std::this_thread::yield();
                           }
                           if (!beginObserved)
                           {
                               exact.store(false, std::memory_order_relaxed);
                               return;
                           }

                           for (Foundation::UInt64 iteration{0U};
                                (iteration < 1'000'000U) && !stop.load(std::memory_order_acquire); ++iteration)
                           {
                               const Foundation::UInt64 owner = thread.GetChargeOwner().GetValue();
                               const bool initialized = thread.IsInitialized();
                               const Foundation::UInt64 publishedOwner =
                                   initialized ? thread.GetChargeOwner().GetValue() : 0U;
                               if (((owner != 0U) && (owner != 19U)) || (initialized && (publishedOwner != 19U)))
                               {
                                   exact.store(false, std::memory_order_relaxed);
                               }
                               if ((owner == 19U) || (publishedOwner == 19U))
                               {
                                   observedPublishedOwner.store(true, std::memory_order_relaxed);
                               }
                               static_cast<void>(readCount.fetch_add(1U, std::memory_order_release));
                           }
                       }};

    bool observedReaderReady{false};
    for (Foundation::UInt64 attempt{0U}; attempt < 100'000U; ++attempt)
    {
        if (readerReady.load(std::memory_order_acquire))
        {
            observedReaderReady = true;
            break;
        }
        std::this_thread::yield();
    }
    beginConcurrentAccess.store(true, std::memory_order_release);
    const ThreadError error = thread.Initialize(ResourceDomainId{19U});
    bool observedConcurrentReads{false};
    for (Foundation::UInt64 attempt{0U}; attempt < 100'000U; ++attempt)
    {
        if ((readCount.load(std::memory_order_acquire) >= 1'024U) &&
            observedPublishedOwner.load(std::memory_order_relaxed))
        {
            observedConcurrentReads = true;
            break;
        }
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    ASTERIA_EXPECT(runner, "Thread metadata publishes atomically without blocking readers", observedReaderReady,
                   observedConcurrentReads, error == ThreadError::None, exact.load(std::memory_order_relaxed),
                   thread.IsInitialized(), thread.GetChargeOwner().GetValue() == 19U);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ThreadTests"};
    Asteria::Test::TestRegistryAdmission(runner);
    Asteria::Test::TestBlockTokenSequence(runner);
    Asteria::Test::TestConcurrentInspection(runner);
    Asteria::Test::TestConcurrentMetadataPublication(runner);
    return runner.Finish();
}
