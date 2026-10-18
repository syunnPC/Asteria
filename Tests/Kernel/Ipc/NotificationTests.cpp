// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationClaimSequencePolicy;
using Kernel::Ipc::NotificationClaimToken;
using Kernel::Ipc::NotificationError;
using Kernel::Ipc::NotificationFailure;
using Kernel::Ipc::NotificationSnapshot;
using Kernel::Ipc::NotificationState;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;

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

[[nodiscard]] bool FailureIs(const NotificationFailure& failure, const NotificationError error) noexcept
{
    return (failure.Error == error) && BytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

[[nodiscard]] bool SnapshotIs(const NotificationSnapshot& snapshot, const NotificationState state, const bool pending,
                              const Foundation::UInt64 lastClaimSequence) noexcept
{
    return (snapshot.LastClaimSequence == lastClaimSequence) && (snapshot.State == state) &&
           (snapshot.Pending == static_cast<Foundation::UInt8>(pending)) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

void TestInitializationAndRegistration(TestContext& runner) noexcept
{
    Notification notification;
    ASTERIA_EXPECT(runner, "default Notification is inert and exposes unpublished metadata",
                   !notification.IsInitialized(), notification.GetObjectType() == ObjectType::Notification,
                   notification.GetChargeOwner().GetValue() == 0U,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Uninitialized, false, 0U));

    ASTERIA_EXPECT(runner, "uninitialized operations are failure-atomic",
                   notification.Signal() == NotificationError::NotInitialized, !notification.Claim().HasValue(),
                   FailureIs(notification.Claim().GetError(), NotificationError::NotInitialized),
                   !notification.CommitClaim({}).HasValue(),
                   FailureIs(notification.CommitClaim({}).GetError(), NotificationError::NotInitialized),
                   !notification.CancelClaim({}).HasValue(),
                   FailureIs(notification.CancelClaim({}).GetError(), NotificationError::NotInitialized),
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Uninitialized, false, 0U));

    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    const auto premature = issuer.CreateObject(notification);
    ASTERIA_EXPECT(runner, "ObjectRegistry rejects an uninitialized Notification", !premature.HasValue(),
                   premature.GetError() == ObjectRegistryError::InvalidArgument);

    ASTERIA_EXPECT(runner, "invalid initialization preserves the complete Notification",
                   notification.Initialize(ResourceDomainId{}) == NotificationError::InvalidConfiguration,
                   !notification.IsInitialized(), notification.GetChargeOwner().GetValue() == 0U,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Uninitialized, false, 0U));

    ASTERIA_EXPECT(runner, "valid initialization release-publishes one ready Notification",
                   notification.Initialize(ResourceDomainId{31U}) == NotificationError::None,
                   notification.IsInitialized(), notification.GetChargeOwner().GetValue() == 31U,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, false, 0U));

    ASTERIA_EXPECT(runner, "Notification initialization is single-use",
                   notification.Initialize(ResourceDomainId{32U}) == NotificationError::AlreadyInitialized,
                   notification.GetChargeOwner().GetValue() == 31U,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, false, 0U));

    const auto registered = issuer.CreateObject(notification);
    ASTERIA_EXPECT(runner, "a ready Notification participates in registry lifetime management", registered.HasValue(),
                   issuer.DestroyObject(registered.GetValue()) == ObjectRegistryError::Ok,
                   issuer.CollectDestroyedObject().HasValue());
}

void TestCoalescingAndClaimResolution(TestContext& runner) noexcept
{
    Notification notification;
    static_cast<void>(notification.Initialize(ResourceDomainId{41U}));

    const auto empty = notification.Claim();
    ASTERIA_EXPECT(runner, "Claim reports an empty Notification without consuming a generation", !empty.HasValue(),
                   FailureIs(empty.GetError(), NotificationError::WouldBlock),
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, false, 0U));

    ASTERIA_EXPECT(runner, "multiple signals coalesce into one pending event",
                   notification.Signal() == NotificationError::None, notification.Signal() == NotificationError::None,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, true, 0U));

    const auto first = notification.Claim();
    ASTERIA_EXPECT(runner, "Claim atomically moves the pending event into generation one", first.HasValue(),
                   first.GetValue().IsValid(), first.GetValue().GetSequence() == 1U,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Claimed, false, 1U));
    if (!first.HasValue())
    {
        return;
    }

    const auto duplicate = notification.Claim();
    ASTERIA_EXPECT(runner, "a signal during an active claim remains pending for the next claim",
                   notification.Signal() == NotificationError::None, !duplicate.HasValue(),
                   FailureIs(duplicate.GetError(), NotificationError::ClaimActive),
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Claimed, true, 1U));

    const auto committed = notification.CommitClaim(first.GetValue());
    ASTERIA_EXPECT(runner, "Commit consumes only the exact claimed event and rejects replay", committed.HasValue(),
                   SnapshotIs(committed.GetValue(), NotificationState::Ready, true, 1U),
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, true, 1U),
                   !notification.CommitClaim(first.GetValue()).HasValue(),
                   FailureIs(notification.CommitClaim(first.GetValue()).GetError(), NotificationError::StaleClaim));

    const auto second = notification.Claim();
    if (!second.HasValue())
    {
        ASTERIA_EXPECT(runner, "the pending signal supplies a second claim", false);
        return;
    }
    const auto staleCancellation = notification.CancelClaim(first.GetValue());
    const auto cancellation = notification.CancelClaim(second.GetValue());
    ASTERIA_EXPECT(runner, "Cancel returns only the exact active claim to the pending bit",
                   second.GetValue().GetSequence() == 2U, !staleCancellation.HasValue(),
                   FailureIs(staleCancellation.GetError(), NotificationError::StaleClaim), cancellation.HasValue(),
                   SnapshotIs(cancellation.GetValue(), NotificationState::Ready, true, 2U));

    const auto third = notification.Claim();
    const bool consumed = third.HasValue() && (third.GetValue().GetSequence() == 3U) &&
                          notification.CommitClaim(third.GetValue()).HasValue();
    ASTERIA_EXPECT(runner, "a cancelled event can be reclaimed and committed exactly once", consumed,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, false, 3U));
}

void TestTokenIsolationAndSequencePolicy(TestContext& runner) noexcept
{
    Notification first;
    Notification second;
    static_cast<void>(first.Initialize(ResourceDomainId{51U}));
    static_cast<void>(second.Initialize(ResourceDomainId{52U}));
    static_cast<void>(first.Signal());
    static_cast<void>(second.Signal());
    const auto firstClaim = first.Claim();
    const auto secondClaim = second.Claim();
    if (!firstClaim.HasValue() || !secondClaim.HasValue())
    {
        ASTERIA_EXPECT(runner, "both Notifications produce independent claims", false);
        return;
    }

    const auto foreign = first.CommitClaim(secondClaim.GetValue());
    const auto invalid = first.CancelClaim({});
    ASTERIA_EXPECT(runner, "claim tokens are bound to one exact immovable Notification", !foreign.HasValue(),
                   FailureIs(foreign.GetError(), NotificationError::InvalidClaim), !invalid.HasValue(),
                   FailureIs(invalid.GetError(), NotificationError::InvalidClaim),
                   SnapshotIs(first.GetSnapshot(), NotificationState::Claimed, false, 1U));

    ASTERIA_EXPECT(runner, "foreign-token failures preserve both claim owners",
                   first.CommitClaim(firstClaim.GetValue()).HasValue(),
                   second.CancelClaim(secondClaim.GetValue()).HasValue());

    Foundation::UInt64 next{7U};
    const bool ordinaryAdvance = NotificationClaimSequencePolicy::TryAdvance(7U, next);
    Foundation::UInt64 preserved{19U};
    const bool wrapped =
        NotificationClaimSequencePolicy::TryAdvance(NotificationClaimSequencePolicy::Maximum, preserved);
    ASTERIA_EXPECT(runner, "claim sequence arithmetic rejects wrap without modifying output", ordinaryAdvance,
                   next == 8U, !wrapped, preserved == 19U);

    const auto pendingSecond = second.Claim();
    if (pendingSecond.HasValue())
    {
        static_cast<void>(second.CommitClaim(pendingSecond.GetValue()));
    }
}

void TestConcurrentSignalAndClaim(TestContext& runner) noexcept
{
    Notification notification;
    static_cast<void>(notification.Initialize(ResourceDomainId{61U}));

    std::atomic<bool> startSignals{false};
    std::atomic<Foundation::Size> readySignalers{0U};
    NotificationError signalErrors[4]{
        NotificationError::None,
        NotificationError::None,
        NotificationError::None,
        NotificationError::None,
    };
    const auto signaler = [&](const Foundation::Size index) noexcept
    {
        static_cast<void>(readySignalers.fetch_add(1U, std::memory_order_release));
        bool started{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (startSignals.load(std::memory_order_acquire))
            {
                started = true;
                break;
            }
            std::this_thread::yield();
        }
        signalErrors[index] = started ? notification.Signal() : NotificationError::NotInitialized;
    };

    std::thread signalThreads[4]{
        std::thread{signaler, Foundation::Size{0U}},
        std::thread{signaler, Foundation::Size{1U}},
        std::thread{signaler, Foundation::Size{2U}},
        std::thread{signaler, Foundation::Size{3U}},
    };
    bool allSignalersReady{false};
    for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (readySignalers.load(std::memory_order_acquire) == 4U)
        {
            allSignalersReady = true;
            break;
        }
        std::this_thread::yield();
    }
    startSignals.store(true, std::memory_order_release);
    for (auto& thread : signalThreads)
    {
        thread.join();
    }

    bool everySignalSucceeded{true};
    for (const NotificationError error : signalErrors)
    {
        everySignalSucceeded = everySignalSucceeded && (error == NotificationError::None);
    }
    ASTERIA_EXPECT(runner, "concurrent ISR-style producers coalesce without a lock", allSignalersReady,
                   everySignalSucceeded, SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, true, 0U));

    std::atomic<bool> startClaims{false};
    std::atomic<Foundation::Size> readyClaimers{0U};
    NotificationClaimToken tokens[2]{};
    NotificationError claimErrors[2]{
        NotificationError::None,
        NotificationError::None,
    };
    const auto claimer = [&](const Foundation::Size index) noexcept
    {
        static_cast<void>(readyClaimers.fetch_add(1U, std::memory_order_release));
        bool started{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (startClaims.load(std::memory_order_acquire))
            {
                started = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!started)
        {
            claimErrors[index] = NotificationError::NotInitialized;
            return;
        }
        const auto claim = notification.Claim();
        if (claim.HasValue())
        {
            tokens[index] = claim.GetValue();
            claimErrors[index] = NotificationError::None;
        }
        else
        {
            claimErrors[index] = claim.GetError().Error;
        }
    };

    std::thread claimThreads[2]{
        std::thread{claimer, Foundation::Size{0U}},
        std::thread{claimer, Foundation::Size{1U}},
    };
    bool bothClaimersReady{false};
    for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (readyClaimers.load(std::memory_order_acquire) == 2U)
        {
            bothClaimersReady = true;
            break;
        }
        std::this_thread::yield();
    }
    startClaims.store(true, std::memory_order_release);
    for (auto& thread : claimThreads)
    {
        thread.join();
    }

    const Foundation::Size winner = tokens[0].IsValid() ? 0U : 1U;
    const Foundation::Size loser = winner == 0U ? 1U : 0U;
    ASTERIA_EXPECT(runner, "two concurrent consumers publish exactly one active claim", bothClaimersReady,
                   tokens[winner].IsValid(), !tokens[loser].IsValid(), claimErrors[winner] == NotificationError::None,
                   claimErrors[loser] == NotificationError::ClaimActive,
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Claimed, false, 1U));

    ASTERIA_EXPECT(runner, "the winning concurrent claim remains resolvable", tokens[winner].IsValid(),
                   notification.CommitClaim(tokens[winner]).HasValue(),
                   SnapshotIs(notification.GetSnapshot(), NotificationState::Ready, false, 1U));
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"NotificationTests"};
    Asteria::Test::TestInitializationAndRegistration(runner);
    Asteria::Test::TestCoalescingAndClaimResolution(runner);
    Asteria::Test::TestTokenIsolationAndSequencePolicy(runner);
    Asteria::Test::TestConcurrentSignalAndClaim(runner);
    return runner.Finish();
}
