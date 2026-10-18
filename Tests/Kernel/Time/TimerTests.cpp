// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <thread>

#include "Kernel/Time/Timer.hpp"

#include "Support/ManualClock.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Object::MonotonicTime;
using Kernel::Object::ResourceDomainId;
using Kernel::Time::Timer;
using Kernel::Time::TimerSnapshot;
using Kernel::Time::TimerState;
using Kernel::Time::TimerToken;
using TestSupport::ManualClock;

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

[[nodiscard]] bool SnapshotIs(const TimerSnapshot& snapshot, const TimerState state,
                              const Foundation::UInt64 deadlineTicks = 0U, const TimerToken token = {}) noexcept
{
    return (snapshot.State == state) && (snapshot.DeadlineTicks == deadlineTicks) && snapshot.Token.IsEqualTo(token) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

void TestConcurrentInspection(TestContext& runner) noexcept
{
    ManualClock clock{0U};
    Timer timer{clock};
    static_cast<void>(timer.Initialize(ResourceDomainId{61U}));
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<Foundation::Size> failures{0U};
    std::atomic<Foundation::Size> readyWorkers{0U};

    const auto mutator = [&]() noexcept
    {
        static_cast<void>(readyWorkers.fetch_add(1U, std::memory_order_release));
        bool started{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (start.load(std::memory_order_acquire))
            {
                started = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!started)
        {
            static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
            done.store(true, std::memory_order_release);
            return;
        }
        for (Foundation::Size iteration{0U}; iteration < 4'096U; ++iteration)
        {
            const auto arm = timer.ArmAt(MonotonicTime{iteration + 1U});
            if (!arm.HasValue())
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                break;
            }
            clock.Set(iteration + 1U);
            std::this_thread::yield();
            if (!timer.TryExpire(arm.GetValue().Token).HasValue())
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                break;
            }
            std::this_thread::yield();
            if (!timer.Acknowledge(arm.GetValue().Token).HasValue())
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                break;
            }
        }
        done.store(true, std::memory_order_release);
    };
    const auto observer = [&]() noexcept
    {
        static_cast<void>(readyWorkers.fetch_add(1U, std::memory_order_release));
        bool started{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (start.load(std::memory_order_acquire))
            {
                started = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!started)
        {
            static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
            return;
        }
        for (Foundation::Size observation{0U}; observation < 1'000'000U; ++observation)
        {
            const TimerSnapshot snapshot = timer.GetSnapshot();
            const bool disarmed =
                (snapshot.State == TimerState::Disarmed) && !snapshot.Token.IsValid() && (snapshot.DeadlineTicks == 0U);
            const bool armed =
                (snapshot.State == TimerState::Armed) && snapshot.Token.IsValid() && (snapshot.DeadlineTicks != 0U);
            const bool expired =
                (snapshot.State == TimerState::Expired) && snapshot.Token.IsValid() && (snapshot.DeadlineTicks != 0U);
            if ((!disarmed && !armed && !expired) || !BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved)))
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                break;
            }
            if (done.load(std::memory_order_acquire))
            {
                break;
            }
            std::this_thread::yield();
        }
    };

    std::thread first{mutator};
    std::thread second{observer};
    bool bothReady{false};
    for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (readyWorkers.load(std::memory_order_acquire) == 2U)
        {
            bothReady = true;
            break;
        }
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    first.join();
    second.join();
    ASTERIA_EXPECT(runner, "Timer expiration and inspection are serialized under contention", bothReady,
                   done.load(std::memory_order_acquire), failures.load(std::memory_order_relaxed) == 0U,
                   SnapshotIs(timer.GetSnapshot(), TimerState::Disarmed));
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TimerTests"};
    Asteria::Test::TestConcurrentInspection(runner);
    return runner.Finish();
}
