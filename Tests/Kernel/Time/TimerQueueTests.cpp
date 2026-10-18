// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <thread>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Time/TimerQueue.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Time::Timer;
using Kernel::Time::TimerError;
using Kernel::Time::TimerId;
using Kernel::Time::TimerQueue;
using Kernel::Time::TimerQueueState;
using Platform::Synchronization::SpinLock;
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

[[nodiscard]] bool StateIs(const TimerQueueState& state, const Foundation::UInt8 scheduled,
                           const Foundation::UInt8 pending, const Foundation::UInt8 claimed,
                           const Foundation::UInt64 nextDeadline,
                           const Foundation::UInt64 nextClaimSequence = 1U) noexcept
{
    return (state.ScheduledEntries == scheduled) && (state.PendingEntries == pending) &&
           (state.ClaimedEntries == claimed) && (state.NextDeadlineTicks == nextDeadline) &&
           (state.NextClaimSequence == nextClaimSequence) && BytesAreZero(state.Reserved, sizeof(state.Reserved));
}

[[nodiscard]] ObjectId RegisterTimer(Timer& timer, RootCapabilityIssuer& issuer,
                                     const Foundation::UInt64 owner) noexcept
{
    if (timer.Initialize(ResourceDomainId{owner}) != TimerError::None)
    {
        return {};
    }
    const auto result = issuer.CreateObject(timer);
    return result.HasValue() ? result.GetValue() : ObjectId{};
}

[[nodiscard]] bool RetireObjects(RootCapabilityIssuer& issuer, const ObjectId* const objects,
                                 const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (!objects[index].IsValid() || (issuer.DestroyObject(objects[index]) != ObjectRegistryError::Ok))
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (!issuer.CollectDestroyedObject().HasValue())
        {
            return false;
        }
    }
    return true;
}

void TestConcurrentInspection(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    const ObjectId object = RegisterTimer(timer, issuer, 51U);
    const TimerId timerId{registry, object};

    {
        TimerQueue queue{registry, clock};
        std::atomic<bool> start{false};
        std::atomic<bool> done{false};
        std::atomic<Foundation::Size> ready{0U};
        std::atomic<Foundation::Size> failures{0U};

        const auto mutator = [&]() noexcept
        {
            static_cast<void>(ready.fetch_add(1U, std::memory_order_release));
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
            for (Foundation::Size iteration{0U}; iteration < 512U; ++iteration)
            {
                const Foundation::UInt64 deadline = iteration + 1U;
                const auto arm = queue.Arm(timerId, timer, MonotonicTime{deadline});
                clock.Set(deadline);
                const auto firstClaim = queue.ClaimDue();
                bool cancelled{false};
                if (firstClaim.HasValue())
                {
                    cancelled = queue.CancelClaim(firstClaim.GetValue().GetClaimToken()).HasValue();
                }
                const auto secondClaim = queue.ClaimDue();
                bool committed{false};
                bool acknowledged{false};
                if (secondClaim.HasValue())
                {
                    committed = queue.CommitClaim(secondClaim.GetValue().GetClaimToken()).HasValue();
                    acknowledged = timer.Acknowledge(secondClaim.GetValue().GetExpiration().Token).HasValue();
                }
                if (!arm.HasValue() || !firstClaim.HasValue() || !cancelled || !secondClaim.HasValue() || !committed ||
                    !acknowledged)
                {
                    static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                    break;
                }
            }
            done.store(true, std::memory_order_release);
        };
        const auto observer = [&]() noexcept
        {
            static_cast<void>(ready.fetch_add(1U, std::memory_order_release));
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
            for (Foundation::Size iteration{0U}; iteration < 1'000'000U; ++iteration)
            {
                const TimerQueueState state = queue.GetState();
                const bool empty = (state.ScheduledEntries == 0U) && (state.PendingEntries == 0U) &&
                                   (state.ClaimedEntries == 0U) && (state.NextDeadlineTicks == 0U) &&
                                   (state.NextClaimSequence != 0U) &&
                                   BytesAreZero(state.Reserved, sizeof(state.Reserved));
                const bool scheduled = (state.ScheduledEntries == 1U) && (state.PendingEntries == 0U) &&
                                       (state.ClaimedEntries == 0U) && (state.NextDeadlineTicks != 0U) &&
                                       (state.NextClaimSequence != 0U) &&
                                       BytesAreZero(state.Reserved, sizeof(state.Reserved));
                const bool pending = (state.ScheduledEntries == 0U) && (state.PendingEntries == 1U) &&
                                     (state.ClaimedEntries == 0U) && (state.NextDeadlineTicks == 0U) &&
                                     (state.NextClaimSequence != 0U) &&
                                     BytesAreZero(state.Reserved, sizeof(state.Reserved));
                const bool claimed = (state.ScheduledEntries == 0U) && (state.PendingEntries == 0U) &&
                                     (state.ClaimedEntries == 1U) && (state.NextDeadlineTicks == 0U) &&
                                     (state.NextClaimSequence != 0U) &&
                                     BytesAreZero(state.Reserved, sizeof(state.Reserved));
                if (!empty && !scheduled && !pending && !claimed)
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
            if (ready.load(std::memory_order_acquire) == 2U)
            {
                bothReady = true;
                break;
            }
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        first.join();
        second.join();
        ASTERIA_EXPECT(runner, "TimerQueue mutation and snapshots are serialized under contention", bothReady,
                       done.load(std::memory_order_acquire), failures.load(std::memory_order_relaxed) == 0U,
                       StateIs(queue.GetState(), 0U, 0U, 0U, 0U, 1'025U));
    }

    const ObjectId objects[]{object};
    ASTERIA_EXPECT(runner, "concurrent queue inspection leaves no retained generation",
                   RetireObjects(issuer, objects, 1U));
}

} // namespace Asteria::Test

int main()
{
    TestContext runner{"TimerQueueTests"};
    Asteria::Test::TestConcurrentInspection(runner);
    return runner.Finish();
}
