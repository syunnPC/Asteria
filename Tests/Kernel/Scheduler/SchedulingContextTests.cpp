// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <thread>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextSnapshot;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::ThreadId;
using Platform::Synchronization::SpinLock;

class TestEndpointObject final : public Kernel::Object::IKernelObject
{
public:
    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::Endpoint;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{2U};
    }
};

class ThreadObjectFixture final
{
public:
    static constexpr Foundation::Size ThreadCount{128U};

    ThreadObjectFixture() noexcept : m_registry{m_synchronization}, m_issuer{m_registry}
    {
        for (Foundation::Size index{0U}; index < ThreadCount; ++index)
        {
            if (m_threads[index].Initialize(ResourceDomainId{1U}) != Kernel::Scheduler::ThreadError::None)
            {
                m_ready = false;
                return;
            }
            const auto result = m_issuer.CreateObject(m_threads[index]);
            if (!result.HasValue())
            {
                m_ready = false;
                return;
            }
            m_identities[index] = result.GetValue();
        }
    }

    ~ThreadObjectFixture() noexcept
    {
        for (const ObjectId identity : m_identities)
        {
            if (identity.IsValid())
            {
                static_cast<void>(m_issuer.DestroyObject(identity));
            }
        }
        for (Foundation::Size index{0U}; index < ThreadCount; ++index)
        {
            if (!m_issuer.CollectDestroyedObject().HasValue())
            {
                break;
            }
        }
    }

    ThreadObjectFixture(const ThreadObjectFixture&) = delete;
    ThreadObjectFixture& operator=(const ThreadObjectFixture&) = delete;
    ThreadObjectFixture(ThreadObjectFixture&&) = delete;
    ThreadObjectFixture& operator=(ThreadObjectFixture&&) = delete;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_ready;
    }

    [[nodiscard]] ObjectRegistry& GetRegistry() noexcept
    {
        return m_registry;
    }

    [[nodiscard]] RootCapabilityIssuer& GetIssuer() noexcept
    {
        return m_issuer;
    }

    [[nodiscard]] ThreadId GetThread(const Foundation::UInt64 logicalId) const noexcept
    {
        if ((logicalId == 0U) || (logicalId > ThreadCount))
        {
            return ThreadId{};
        }
        const Foundation::Size index = static_cast<Foundation::Size>(logicalId - 1U);
        return ThreadId{m_registry, m_identities[index]};
    }

private:
    Kernel::Scheduler::Thread m_threads[ThreadCount]{};
    SpinLock m_synchronization;
    Asteria::TestSupport::TestObjectRegistry m_registry;
    RootCapabilityIssuer m_issuer;
    ObjectId m_identities[ThreadCount]{};
    bool m_ready{true};
};

ThreadObjectFixture* g_threadObjects{nullptr};

[[nodiscard]] ObjectRegistry& GetThreadRegistry() noexcept
{
    return g_threadObjects->GetRegistry();
}

[[nodiscard]] ThreadId Thread(const Foundation::UInt64 logicalId) noexcept
{
    return g_threadObjects->GetThread(logicalId);
}

[[nodiscard]] constexpr SchedulingContextConfiguration MakeConfiguration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 100U,
        .PeriodTicks = 1'000U,
        .RelativeDeadlineTicks = 400U,
        .Weight = 20U,
        .Priority = 200U,
        .Criticality = SchedulingCriticality::High,
        .CpuAffinityMask = 0x05U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool SnapshotsEqual(const SchedulingContextSnapshot& left,
                                  const SchedulingContextSnapshot& right) noexcept
{
    return left.BaseThread.IsEqualTo(right.BaseThread) && left.CurrentThread.IsEqualTo(right.CurrentThread) &&
           (left.RemainingBudgetTicks == right.RemainingBudgetTicks) &&
           (left.NextReplenishmentTicks == right.NextReplenishmentTicks) &&
           (left.AbsoluteDeadlineTicks == right.AbsoluteDeadlineTicks) && (left.RunStartTicks == right.RunStartTicks) &&
           (left.State == right.State) && (left.RunningProcessor.GetValue() == right.RunningProcessor.GetValue()) &&
           (left.DonationDepth == right.DonationDepth) && (left.IsBudgetExhausted == right.IsBudgetExhausted) &&
           (left.Reserved[0U] == right.Reserved[0U]) && (left.Reserved[1U] == right.Reserved[1U]) &&
           (left.Reserved[2U] == right.Reserved[2U]) && (left.Reserved[3U] == right.Reserved[3U]);
}

void TestConcurrentMetadataPublication(TestContext& runner) noexcept
{
    SchedulingContext context{GetThreadRegistry()};
    std::atomic<bool> readerReady{false};
    std::atomic<bool> beginConcurrentAccess{false};
    std::atomic<Foundation::UInt64> readCount{0U};
    std::atomic<bool> stop{false};
    std::atomic<bool> exact{true};
    std::atomic<bool> observedPublishedOwner{false};
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
                               const Foundation::UInt64 owner = context.GetChargeOwner().GetValue();
                               if ((owner != 0U) && (owner != 19U))
                               {
                                   exact.store(false, std::memory_order_relaxed);
                               }
                               if (owner == 19U)
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
    const SchedulingContextError error =
        context.Initialize(MakeConfiguration(), ResourceDomainId{19U}, MonotonicTime{0U});
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

    ASTERIA_EXPECT(runner, "charge-owner metadata publishes atomically without blocking readers", observedReaderReady,
                   observedConcurrentReads, error == SchedulingContextError::None,
                   exact.load(std::memory_order_relaxed), observedPublishedOwner.load(std::memory_order_relaxed),
                   context.GetChargeOwner().GetValue() == 19U);
}

void TestThreadPinLifetime(TestContext& runner) noexcept
{
    ObjectRegistry& registry = GetThreadRegistry();
    RootCapabilityIssuer& issuer = g_threadObjects->GetIssuer();
    const ThreadId original = Thread(127U);

    SchedulingContext context{registry};
    static_cast<void>(context.Initialize(MakeConfiguration(), ResourceDomainId{30U}, MonotonicTime{0U}));
    ASTERIA_EXPECT(runner, "binding acquires a persistent pin for the exact Thread generation",
                   context.Bind(original) == SchedulingContextError::None);
    ASTERIA_EXPECT(runner, "a bound Thread may enter quiescing without invalidating the context",
                   issuer.DestroyObject(original.GetObjectId()) == ObjectRegistryError::Ok);

    SchedulingContext rejected{registry};
    static_cast<void>(rejected.Initialize(MakeConfiguration(), ResourceDomainId{31U}, MonotonicTime{0U}));
    const SchedulingContextSnapshot beforeRejectedBind = rejected.GetSnapshot();
    ASTERIA_EXPECT(runner, "quiescing Thread rejects new bindings without changing the context",
                   rejected.Bind(original) == SchedulingContextError::ThreadNotFound,
                   SnapshotsEqual(beforeRejectedBind, rejected.GetSnapshot()));
    const auto prematureCollection = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "bound context pin prevents Thread collection", !prematureCollection.HasValue(),
                   prematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    ASTERIA_EXPECT(runner, "unbinding releases the retained Thread generation",
                   context.Unbind(original) == SchedulingContextError::None);
    const auto collectedOriginal = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "unbound Thread becomes collectable after its final pin releases",
                   collectedOriginal.HasValue(), collectedOriginal.GetValue().Object.GetValue() == original.GetValue());
    ASTERIA_EXPECT(runner, "collected Thread generation remains stale",
                   rejected.Bind(original) == SchedulingContextError::ThreadNotFound);

    Kernel::Scheduler::Thread replacementObject;
    static_cast<void>(replacementObject.Initialize(ResourceDomainId{1U}));
    const auto replacementResult = issuer.CreateObject(replacementObject);
    ASTERIA_EXPECT(runner, "reused registry slot advances the Thread generation", replacementResult.HasValue(),
                   replacementResult.GetValue().GetValue() != original.GetValue());
    if (replacementResult.HasValue())
    {
        const ThreadId replacement{registry, replacementResult.GetValue()};
        static_cast<void>(context.Bind(replacement));
        const SchedulingContextSnapshot replacementBound = context.GetSnapshot();
        ASTERIA_EXPECT(runner, "stale generation cannot impersonate its replacement in bound operations",
                       context.Unbind(original) == SchedulingContextError::NotCurrentThread,
                       context.Donate(original, Thread(120U)) == SchedulingContextError::NotCurrentThread,
                       context.StartRun(original, CpuId{0U}, MonotonicTime{0U}).GetError() ==
                           SchedulingContextError::NotCurrentThread,
                       context.Donate(replacement, original) == SchedulingContextError::ThreadNotFound,
                       SnapshotsEqual(replacementBound, context.GetSnapshot()));
        static_cast<void>(context.Unbind(replacement));

        const ThreadId returnCaller = Thread(120U);
        static_cast<void>(context.Bind(returnCaller));
        static_cast<void>(context.Donate(returnCaller, replacement));
        const SchedulingContextSnapshot replacementDonated = context.GetSnapshot();
        ASTERIA_EXPECT(runner, "stale generation cannot return or run as a donated replacement",
                       context.ReturnDonation(original) == SchedulingContextError::NotCurrentThread,
                       context.StartRun(original, CpuId{0U}, MonotonicTime{0U}).GetError() ==
                           SchedulingContextError::NotCurrentThread,
                       SnapshotsEqual(replacementDonated, context.GetSnapshot()));
        static_cast<void>(context.ReturnDonation(replacement));
        static_cast<void>(context.Unbind(returnCaller));
        static_cast<void>(issuer.DestroyObject(replacement.GetObjectId()));
        static_cast<void>(issuer.CollectDestroyedObject());
    }

    TestEndpointObject endpointObject;
    const auto endpointResult = issuer.CreateObject(endpointObject);
    ASTERIA_EXPECT(runner, "type-check fixture registers an Endpoint", endpointResult.HasValue());
    if (endpointResult.HasValue())
    {
        const SchedulingContextSnapshot beforeWrongType = context.GetSnapshot();
        ASTERIA_EXPECT(runner, "binding rejects a live object whose type is not Thread",
                       context.Bind(ThreadId{registry, endpointResult.GetValue()}) ==
                           SchedulingContextError::ThreadTypeMismatch,
                       SnapshotsEqual(beforeWrongType, context.GetSnapshot()));
    }

    const ThreadId pinFailureCaller = Thread(122U);
    const ThreadId quiescingServer = Thread(121U);
    SchedulingContext pinFailures{registry};
    static_cast<void>(pinFailures.Initialize(MakeConfiguration(), ResourceDomainId{34U}, MonotonicTime{0U}));
    static_cast<void>(pinFailures.Bind(pinFailureCaller));
    if (endpointResult.HasValue())
    {
        const SchedulingContextSnapshot beforeWrongTypeDonation = pinFailures.GetSnapshot();
        ASTERIA_EXPECT(runner, "wrong-type donation is failure-atomic",
                       pinFailures.Donate(pinFailureCaller, ThreadId{registry, endpointResult.GetValue()}) ==
                           SchedulingContextError::ThreadTypeMismatch,
                       SnapshotsEqual(beforeWrongTypeDonation, pinFailures.GetSnapshot()));
    }
    static_cast<void>(issuer.DestroyObject(quiescingServer.GetObjectId()));
    const SchedulingContextSnapshot beforeQuiescingDonation = pinFailures.GetSnapshot();
    ASTERIA_EXPECT(runner, "quiescing donation is failure-atomic",
                   pinFailures.Donate(pinFailureCaller, quiescingServer) == SchedulingContextError::ThreadNotFound,
                   SnapshotsEqual(beforeQuiescingDonation, pinFailures.GetSnapshot()));
    const auto collectedUnpinnedServer = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "failed donation acquires no persistent server pin", collectedUnpinnedServer.HasValue(),
                   collectedUnpinnedServer.GetValue().Object.GetValue() == quiescingServer.GetValue());
    static_cast<void>(pinFailures.Unbind(pinFailureCaller));
    if (endpointResult.HasValue())
    {
        static_cast<void>(issuer.DestroyObject(endpointResult.GetValue()));
        static_cast<void>(issuer.CollectDestroyedObject());
    }

    const ThreadId caller = Thread(126U);
    const ThreadId server = Thread(125U);
    SchedulingContext donated{registry};
    static_cast<void>(donated.Initialize(MakeConfiguration(), ResourceDomainId{32U}, MonotonicTime{0U}));
    ASTERIA_EXPECT(runner, "donation retains its server Thread while that generation quiesces",
                   donated.Bind(caller) == SchedulingContextError::None,
                   donated.Donate(caller, server) == SchedulingContextError::None,
                   issuer.DestroyObject(server.GetObjectId()) == ObjectRegistryError::Ok);
    const auto donatedPrematureCollection = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "active donation prevents server collection", !donatedPrematureCollection.HasValue(),
                   donatedPrematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    ASTERIA_EXPECT(runner, "returning a donation releases its server pin",
                   donated.ReturnDonation(server) == SchedulingContextError::None);
    const auto collectedServer = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "returned server becomes collectable without disturbing the caller pin",
                   collectedServer.HasValue(), collectedServer.GetValue().Object.GetValue() == server.GetValue());
    static_cast<void>(donated.Unbind(caller));

    const ThreadId destructorThread = Thread(124U);
    {
        SchedulingContext scoped{registry};
        static_cast<void>(scoped.Initialize(MakeConfiguration(), ResourceDomainId{33U}, MonotonicTime{0U}));
        ASTERIA_EXPECT(runner, "context destructor fixture retains a quiescing Thread",
                       scoped.Bind(destructorThread) == SchedulingContextError::None,
                       issuer.DestroyObject(destructorThread.GetObjectId()) == ObjectRegistryError::Ok);
        const auto beforeDestructor = issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(runner, "quiescing Thread remains pinned until context destruction",
                       !beforeDestructor.HasValue(),
                       beforeDestructor.GetError() == ObjectRegistryError::NoDestructionReady);
    }
    const auto afterDestructor = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "SchedulingContext destruction releases every retained Thread pin",
                   afterDestructor.HasValue(),
                   afterDestructor.GetValue().Object.GetValue() == destructorThread.GetValue());
}

} // namespace Asteria::Test

int main()
{
    TestContext runner{"SchedulingContextTests"};
    Asteria::Test::ThreadObjectFixture threadObjects;
    if (!threadObjects.IsReady())
    {
        ASTERIA_EXPECT(runner, "scheduler fixture registers every test Thread object", false);
        return runner.Finish();
    }
    Asteria::Test::g_threadObjects = &threadObjects;
    Asteria::Test::TestConcurrentMetadataPublication(runner);
    Asteria::Test::TestThreadPinLifetime(runner);
    Asteria::Test::g_threadObjects = nullptr;
    return runner.Finish();
}
