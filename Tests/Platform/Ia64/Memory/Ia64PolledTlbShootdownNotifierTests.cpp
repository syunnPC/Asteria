// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Platform/Ia64/Memory/Ia64PolledTlbShootdownNotifier.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Hal::Memory::ITlbShootdownNotifier;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifier;
using Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierInitializationError;
using Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierSnapshot;
using Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierState;
using Platform::Ia64::Memory::Ia64PolledTlbShootdownPollResult;

[[nodiscard]] bool IsInitialSnapshot(const Ia64PolledTlbShootdownNotifierSnapshot& snapshot) noexcept
{
    return (snapshot.ProfiledProcessorMask == 0U) && (snapshot.PendingProcessorMask == 0U) &&
           (snapshot.State == Ia64PolledTlbShootdownNotifierState::Uninitialized) && (snapshot.Reserved == 0U);
}

void TestInitializationAndValidation(TestContext& runner) noexcept
{
    Ia64PolledTlbShootdownNotifier notifier;
    ITlbShootdownNotifier& interface = notifier;

    interface.Notify(0x01U);
    ASTERIA_EXPECT(runner, "an uninitialized notifier remains inert", IsInitialSnapshot(notifier.GetSnapshot()),
                   notifier.Poll(CpuId{0U}) == Ia64PolledTlbShootdownPollResult::NotReady);

    ASTERIA_EXPECT(runner, "empty and out-of-range profiled masks are retryable failures",
                   notifier.Initialize(0U) == Ia64PolledTlbShootdownNotifierInitializationError::InvalidProcessorMask,
                   notifier.Initialize(0x10U) ==
                       Ia64PolledTlbShootdownNotifierInitializationError::InvalidProcessorMask,
                   IsInitialSnapshot(notifier.GetSnapshot()));

    constexpr Foundation::ProcessorMask ProfiledProcessorMask{0x0BU};
    ASTERIA_EXPECT(runner, "one valid profiled mask initializes exactly once",
                   notifier.Initialize(ProfiledProcessorMask) ==
                       Ia64PolledTlbShootdownNotifierInitializationError::None,
                   notifier.Initialize(ProfiledProcessorMask) ==
                       Ia64PolledTlbShootdownNotifierInitializationError::AlreadyInitialized);

    const Ia64PolledTlbShootdownNotifierSnapshot snapshot = notifier.GetSnapshot();
    ASTERIA_EXPECT(runner, "successful initialization publishes an empty ready profile",
                   snapshot.ProfiledProcessorMask == ProfiledProcessorMask, snapshot.PendingProcessorMask == 0U,
                   snapshot.State == Ia64PolledTlbShootdownNotifierState::Ready, snapshot.Reserved == 0U);

    ASTERIA_EXPECT(runner, "polling rejects unprofiled and out-of-range CPU indexes",
                   notifier.Poll(CpuId{2U}) == Ia64PolledTlbShootdownPollResult::InvalidProcessor,
                   notifier.Poll(CpuId{4U}) == Ia64PolledTlbShootdownPollResult::InvalidProcessor);

    interface.Notify(0U);
    interface.Notify(0x04U);
    interface.Notify(0x10U);
    interface.Notify(0x05U);
    ASTERIA_EXPECT(runner, "invalid target masks cannot partially latch a profiled CPU",
                   notifier.GetSnapshot().PendingProcessorMask == 0U);
}

void TestCoalescingAndIsolation(TestContext& runner) noexcept
{
    Ia64PolledTlbShootdownNotifier notifier;
    static_cast<void>(notifier.Initialize(0x0BU));
    ITlbShootdownNotifier& interface = notifier;

    interface.Notify(0x0AU);
    interface.Notify(0x02U);
    ASTERIA_EXPECT(runner, "repeated requests coalesce in independent per-CPU slots",
                   notifier.GetSnapshot().PendingProcessorMask == 0x0AU);

    ASTERIA_EXPECT(runner, "one poll consumes only the calling CPU's coalesced latch",
                   notifier.Poll(CpuId{1U}) == Ia64PolledTlbShootdownPollResult::Pending,
                   notifier.Poll(CpuId{1U}) == Ia64PolledTlbShootdownPollResult::Empty,
                   notifier.GetSnapshot().PendingProcessorMask == 0x08U);

    interface.Notify(0x01U);
    ASTERIA_EXPECT(runner, "profiled CPUs claim notifications without cross-slot interference",
                   notifier.Poll(CpuId{3U}) == Ia64PolledTlbShootdownPollResult::Pending,
                   notifier.Poll(CpuId{0U}) == Ia64PolledTlbShootdownPollResult::Pending,
                   notifier.GetSnapshot().PendingProcessorMask == 0U);
}

void TestOverlappingPublishers(TestContext& runner) noexcept
{
    Ia64PolledTlbShootdownNotifier notifier;
    static_cast<void>(notifier.Initialize(0x03U));
    ITlbShootdownNotifier& interface = notifier;
    std::atomic<Foundation::UInt32> readyPublisherCount{0U};
    std::atomic<bool> startPublishers{false};
    constexpr Foundation::UInt32 NotificationCount{1'024U};

    std::thread firstPublisher{[&interface, &readyPublisherCount, &startPublishers]()
                               {
                                   static_cast<void>(readyPublisherCount.fetch_add(1U, std::memory_order_acq_rel));
                                   while (!startPublishers.load(std::memory_order_acquire))
                                   {
                                       std::this_thread::yield();
                                   }
                                   for (Foundation::UInt32 index{0U}; index < NotificationCount; ++index)
                                   {
                                       interface.Notify(0x03U);
                                   }
                               }};
    std::thread secondPublisher{[&interface, &readyPublisherCount, &startPublishers]()
                                {
                                    static_cast<void>(readyPublisherCount.fetch_add(1U, std::memory_order_acq_rel));
                                    while (!startPublishers.load(std::memory_order_acquire))
                                    {
                                        std::this_thread::yield();
                                    }
                                    for (Foundation::UInt32 index{0U}; index < NotificationCount; ++index)
                                    {
                                        interface.Notify(0x02U);
                                    }
                                }};

    bool bothPublishersReady{false};
    constexpr Foundation::UInt32 MaximumReadyPollAttempts{1'000'000U};
    for (Foundation::UInt32 attempt{0U}; attempt < MaximumReadyPollAttempts; ++attempt)
    {
        if (readyPublisherCount.load(std::memory_order_acquire) == 2U)
        {
            bothPublishersReady = true;
            break;
        }
        std::this_thread::yield();
    }
    startPublishers.store(true, std::memory_order_release);
    firstPublisher.join();
    secondPublisher.join();

    ASTERIA_EXPECT(runner, "overlapping bounded publishers coalesce without losing a CPU latch", bothPublishersReady,
                   notifier.GetSnapshot().PendingProcessorMask == 0x03U,
                   notifier.Poll(CpuId{0U}) == Ia64PolledTlbShootdownPollResult::Pending,
                   notifier.Poll(CpuId{1U}) == Ia64PolledTlbShootdownPollResult::Pending,
                   notifier.Poll(CpuId{0U}) == Ia64PolledTlbShootdownPollResult::Empty,
                   notifier.Poll(CpuId{1U}) == Ia64PolledTlbShootdownPollResult::Empty);
}

void TestLateNotification(TestContext& runner) noexcept
{
    Ia64PolledTlbShootdownNotifier notifier;
    static_cast<void>(notifier.Initialize(0x01U));
    ITlbShootdownNotifier& interface = notifier;

    const Ia64PolledTlbShootdownPollResult priorPoll = notifier.Poll(CpuId{0U});
    std::thread latePublisher{[&interface]()
                              {
                                  interface.Notify(0x01U);
                              }};
    latePublisher.join();

    ASTERIA_EXPECT(runner, "a notification after an earlier poll remains claimable once",
                   priorPoll == Ia64PolledTlbShootdownPollResult::Empty,
                   notifier.Poll(CpuId{0U}) == Ia64PolledTlbShootdownPollResult::Pending,
                   notifier.Poll(CpuId{0U}) == Ia64PolledTlbShootdownPollResult::Empty);
}

void TestPublicationOrdering(TestContext& runner) noexcept
{
    Ia64PolledTlbShootdownNotifier notifier;
    static_cast<void>(notifier.Initialize(0x02U));
    ITlbShootdownNotifier& interface = notifier;
    Foundation::UInt64 publishedValue{0U};
    std::atomic<bool> producerComplete{false};

    std::thread producer{[&interface, &publishedValue, &producerComplete]()
                         {
                             publishedValue = 0x0123'4567'89AB'CDEFULL;
                             interface.Notify(0x02U);
                             producerComplete.store(true, std::memory_order_relaxed);
                         }};

    Ia64PolledTlbShootdownPollResult pollResult{Ia64PolledTlbShootdownPollResult::Empty};
    constexpr Foundation::UInt32 MaximumPollAttempts{1'000'000U};
    for (Foundation::UInt32 attempt{0U}; attempt < MaximumPollAttempts; ++attempt)
    {
        pollResult = notifier.Poll(CpuId{1U});
        if (pollResult == Ia64PolledTlbShootdownPollResult::Pending)
        {
            break;
        }
        if (producerComplete.load(std::memory_order_relaxed))
        {
            pollResult = notifier.Poll(CpuId{1U});
            break;
        }
        std::this_thread::yield();
    }

    const bool publicationObserved =
        (pollResult == Ia64PolledTlbShootdownPollResult::Pending) && (publishedValue == 0x0123'4567'89AB'CDEFULL);
    producer.join();
    ASTERIA_EXPECT(runner, "a pending poll acquires ordinary writes sequenced before Notify", publicationObserved);
    ASTERIA_EXPECT(runner, "the ordered publication remains a single coalesced claim",
                   notifier.Poll(CpuId{1U}) == Ia64PolledTlbShootdownPollResult::Empty);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64PolledTlbShootdownNotifierTests"};
    Asteria::Test::TestInitializationAndValidation(runner);
    Asteria::Test::TestCoalescingAndIsolation(runner);
    Asteria::Test::TestOverlappingPublishers(runner);
    Asteria::Test::TestLateNotification(runner);
    Asteria::Test::TestPublicationOrdering(runner);
    return runner.Finish();
}
