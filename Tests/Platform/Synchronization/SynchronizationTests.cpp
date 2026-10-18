// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"

#include "Foundation/Types.hpp"
#include "Platform/Synchronization/AtomicReferenceCount.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Platform::Synchronization::AtomicReferenceCount;
using Platform::Synchronization::SpinLock;
using Platform::Synchronization::SpinLockGuard;

void TestProcessorOwnersAndWaits(TestContext& runner) noexcept
{
    using Asteria::Platform::Synchronization::AtomicReferenceCount;
    using Asteria::Platform::Synchronization::BindProcessorProgress;
    using Asteria::Platform::Synchronization::HeldLockDepth;
    using Asteria::Platform::Synchronization::ProcessorProgressHooks;
    using Asteria::Platform::Synchronization::ProcessorWaitProgress;
    using Asteria::Platform::Synchronization::SetHostedProcessor;
    using Asteria::Platform::Synchronization::SpinLock;
    using Asteria::Platform::Synchronization::SpinLockGuard;
    using Kernel::Scheduler::CpuId;
    SpinLock lock;
    SetHostedProcessor(CpuId{3U});
    lock.Acquire();
    ASTERIA_EXPECT(runner, "owner is processor plus one and depth counts held locks", lock.GetOwner() == 4U,
                   HeldLockDepth() == 1U);
    lock.Release();
    ASTERIA_EXPECT(runner, "release clears owner and depth", lock.GetOwner() == 0U, HeldLockDepth() == 0U);
    SetHostedProcessor(CpuId{});
    lock.Acquire();
    ASTERIA_EXPECT(runner, "pre-identity lock owner is explicitly unknown", lock.GetOwner() == ~Foundation::UInt64{0U});
    lock.Release();
    Kernel::Scheduler::ProcessorSet set;
    ASTERIA_EXPECT(runner, "processor set publishes admitted and contained retirement masks", set.Admit(CpuId{0U}),
                   set.Admit(CpuId{1U}), set.OnlineMask() == 3U, set.BeginOfflining(CpuId{1U}), set.OnlineMask() == 1U,
                   set.SilentOnline(3U) == 1U, set.FinishOfflining(CpuId{1U}), !set.BeginOfflining(CpuId{0U}));
#ifndef NDEBUG
    const pid_t child = fork();
    if (child == 0)
    {
        BindProcessorProgress(
            CpuId{0U},
            ProcessorProgressHooks{
                nullptr, nullptr, nullptr, nullptr,
                [](const Kernel::Diagnostics::StopCode code, const Foundation::UInt64 detail) noexcept
                {
                    _exit(code == Kernel::Diagnostics::StopCode::InvariantViolated && detail == 1U ? 0 : 2);
                }});
        lock.Acquire();
        ProcessorWaitProgress();
        _exit(3);
    }
    int status{};
    ASTERIA_EXPECT(runner, "debug wait under any lock stops with its held depth", child > 0,
                   waitpid(child, &status, 0) == child, WIFEXITED(status), WEXITSTATUS(status) == 0);
#endif
}

void TestSpinLockMutualExclusion(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 IterationsPerWorker{20'000U};
    SpinLock lock;
    Foundation::UInt64 protectedCount{0U};

    const auto worker = [&]() noexcept
    {
        for (Foundation::UInt64 iteration{0U}; iteration < IterationsPerWorker; ++iteration)
        {
            SpinLockGuard guard{lock};
            ++protectedCount;
        }
    };

    std::thread firstWorker{worker};
    std::thread secondWorker{worker};
    std::thread thirdWorker{worker};
    std::thread fourthWorker{worker};
    firstWorker.join();
    secondWorker.join();
    thirdWorker.join();
    fourthWorker.join();

    ASTERIA_EXPECT(runner, "spin lock provides mutual exclusion under four-worker contention",
                   protectedCount == (IterationsPerWorker * 4U));
}

void TestSpinLockVisibility(TestContext& runner) noexcept
{
    SpinLock lock;
    bool isPublished{false};
    Foundation::UInt64 payload{0U};
    Foundation::UInt64 observedPayload{0U};

    std::thread reader{[&]() noexcept
                       {
                           bool hasObserved{false};
                           while (!hasObserved)
                           {
                               {
                                   SpinLockGuard guard{lock};
                                   if (isPublished)
                                   {
                                       observedPayload = payload;
                                       hasObserved = true;
                                   }
                               }
                               if (!hasObserved)
                               {
                                   std::this_thread::yield();
                               }
                           }
                       }};

    {
        SpinLockGuard guard{lock};
        payload = 0x1020'3040'5060'7080ULL;
        isPublished = true;
    }
    reader.join();

    ASTERIA_EXPECT(runner, "release and acquire ordering publishes protected payload writes",
                   observedPayload == 0x1020'3040'5060'7080ULL);
}

void TestAtomicReferenceCount(TestContext& runner) noexcept
{
    AtomicReferenceCount count;
    ASTERIA_EXPECT(runner, "reference count begins at zero", count.Load() == 0U);
    ASTERIA_EXPECT(runner, "first serialized reference acquisition succeeds", count.TryAcquire());
    ASTERIA_EXPECT(runner, "second serialized reference acquisition succeeds", count.TryAcquire());
    ASTERIA_EXPECT(runner, "third serialized reference acquisition succeeds", count.TryAcquire());
    ASTERIA_EXPECT(runner, "fourth serialized reference acquisition succeeds", count.TryAcquire());
    ASTERIA_EXPECT(runner, "serialized acquisitions update the reference count", count.Load() == 4U);

    std::thread firstWorker{[&count]() noexcept
                            {
                                count.Release();
                            }};
    std::thread secondWorker{[&count]() noexcept
                             {
                                 count.Release();
                             }};
    std::thread thirdWorker{[&count]() noexcept
                            {
                                count.Release();
                            }};
    std::thread fourthWorker{[&count]() noexcept
                             {
                                 count.Release();
                             }};
    firstWorker.join();
    secondWorker.join();
    thirdWorker.join();
    fourthWorker.join();

    ASTERIA_EXPECT(runner, "concurrent wait-free releases reach zero exactly", count.Load() == 0U);
    count.Reset();
    ASTERIA_EXPECT(runner, "exclusive reset preserves the zero state", count.Load() == 0U);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SynchronizationTests"};
    Asteria::Test::TestProcessorOwnersAndWaits(runner);
    Asteria::Test::TestSpinLockMutualExclusion(runner);
    Asteria::Test::TestSpinLockVisibility(runner);
    Asteria::Test::TestAtomicReferenceCount(runner);
    return runner.Finish();
}
