// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Ipc::MaximumWaitSetEntries;
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationError;
using Kernel::Ipc::NotificationId;
using Kernel::Ipc::NotificationSnapshot;
using Kernel::Ipc::NotificationState;
using Kernel::Ipc::WaitSet;
using Kernel::Ipc::WaitSetClaimToken;
using Kernel::Ipc::WaitSetError;
using Kernel::Ipc::WaitSetFailure;
using Kernel::Ipc::WaitSetRegistrationToken;
using Kernel::Ipc::WaitSetSequencePolicy;
using Kernel::Ipc::WaitSetSnapshot;
using Kernel::Ipc::WaitSetState;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
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

[[nodiscard]] bool FailureIs(const WaitSetFailure& failure, const WaitSetError error,
                             const NotificationError notificationError = NotificationError::None,
                             const ObjectPinError pinError = ObjectPinError::Ok) noexcept
{
    return (failure.Error == error) && (failure.Notification.Error == notificationError) &&
           BytesAreZero(failure.Notification.Reserved, sizeof(failure.Notification.Reserved)) &&
           (failure.Pin == pinError) && BytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

[[nodiscard]] bool SnapshotIs(const WaitSetSnapshot& snapshot, const WaitSetState state,
                              const Foundation::Size registeredEntries,
                              const Foundation::UInt64 lastRegistrationSequence,
                              const Foundation::UInt64 lastClaimSequence, const Foundation::Size nextScanIndex,
                              const Foundation::Size claimedEntries = 0U) noexcept
{
    return (snapshot.LastRegistrationSequence == lastRegistrationSequence) &&
           (snapshot.LastClaimSequence == lastClaimSequence) && (snapshot.State == state) &&
           (snapshot.RegisteredEntries == registeredEntries) && (snapshot.ClaimedEntries == claimedEntries) &&
           (snapshot.NextScanIndex == nextScanIndex) && BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] bool NotificationSnapshotsEqual(const NotificationSnapshot& first,
                                              const NotificationSnapshot& second) noexcept
{
    return (first.LastClaimSequence == second.LastClaimSequence) && (first.State == second.State) &&
           (first.Pending == second.Pending) && BytesAreZero(first.Reserved, sizeof(first.Reserved)) &&
           BytesAreZero(second.Reserved, sizeof(second.Reserved));
}

[[nodiscard]] ObjectId CreateNotification(RootCapabilityIssuer& issuer, Notification& notification,
                                          const Foundation::UInt64 owner) noexcept
{
    if (notification.Initialize(ResourceDomainId{owner}) != NotificationError::None)
    {
        return ObjectId{};
    }
    const auto created = issuer.CreateObject(notification);
    return created.HasValue() ? created.GetValue() : ObjectId{};
}

void TestInitializationAndRegistration(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification notification;
    WaitSet waitSet{registry};

    ASTERIA_EXPECT(runner, "default WaitSet is inert and exposes unpublished metadata", !waitSet.IsInitialized(),
                   waitSet.GetObjectType() == ObjectType::WaitSet, waitSet.GetChargeOwner().GetValue() == 0U,
                   SnapshotIs(waitSet.GetSnapshot(), WaitSetState::Uninitialized, 0U, 0U, 0U, 0U));

    const auto uninitializedRegistration = waitSet.RegisterNotification({}, notification, 1U);
    const auto uninitializedClaim = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "uninitialized WaitSet operations are failure-atomic", !uninitializedRegistration.HasValue(),
                   FailureIs(uninitializedRegistration.GetError(), WaitSetError::NotInitialized),
                   !uninitializedClaim.HasValue(),
                   FailureIs(uninitializedClaim.GetError(), WaitSetError::NotInitialized),
                   FailureIs(waitSet.Unregister({}).GetError(), WaitSetError::NotInitialized),
                   FailureIs(waitSet.CommitClaim({}).GetError(), WaitSetError::NotInitialized),
                   FailureIs(waitSet.CancelClaim({}).GetError(), WaitSetError::NotInitialized));

    const auto premature = issuer.CreateObject(waitSet);
    ASTERIA_EXPECT(runner, "ObjectRegistry rejects an uninitialized WaitSet", !premature.HasValue(),
                   premature.GetError() == ObjectRegistryError::InvalidArgument);

    ASTERIA_EXPECT(runner, "invalid initialization preserves the complete WaitSet",
                   waitSet.Initialize(ResourceDomainId{}) == WaitSetError::InvalidConfiguration,
                   !waitSet.IsInitialized(),
                   SnapshotIs(waitSet.GetSnapshot(), WaitSetState::Uninitialized, 0U, 0U, 0U, 0U));

    ASTERIA_EXPECT(runner, "valid initialization release-publishes one ready WaitSet",
                   waitSet.Initialize(ResourceDomainId{31U}) == WaitSetError::None, waitSet.IsInitialized(),
                   waitSet.GetChargeOwner().GetValue() == 31U,
                   SnapshotIs(waitSet.GetSnapshot(), WaitSetState::Ready, 0U, 0U, 0U, 0U));

    ASTERIA_EXPECT(runner, "WaitSet initialization is single-use",
                   waitSet.Initialize(ResourceDomainId{32U}) == WaitSetError::AlreadyInitialized,
                   waitSet.GetChargeOwner().GetValue() == 31U);

    const auto registered = issuer.CreateObject(waitSet);
    ASTERIA_EXPECT(runner, "an empty WaitSet participates in registry lifetime management", registered.HasValue(),
                   issuer.DestroyObject(registered.GetValue()) == ObjectRegistryError::Ok,
                   issuer.CollectDestroyedObject().HasValue());
}

void TestRegistrationIdentityAndLifetime(TestContext& runner) noexcept
{
    SpinLock firstLock;
    SpinLock secondLock;
    Asteria::TestSupport::TestObjectRegistry firstRegistry{firstLock};
    Asteria::TestSupport::TestObjectRegistry secondRegistry{secondLock};
    RootCapabilityIssuer firstIssuer{firstRegistry};
    RootCapabilityIssuer secondIssuer{secondRegistry};
    Notification first;
    Notification second;
    Notification foreign;
    Notification quiescing;
    WaitSet waitSet{firstRegistry};
    WaitSet otherWaitSet{firstRegistry};
    static_cast<void>(waitSet.Initialize(ResourceDomainId{41U}));
    static_cast<void>(otherWaitSet.Initialize(ResourceDomainId{42U}));

    const ObjectId firstObject = CreateNotification(firstIssuer, first, 51U);
    const ObjectId secondObject = CreateNotification(firstIssuer, second, 52U);
    const ObjectId foreignObject = CreateNotification(secondIssuer, foreign, 53U);
    const ObjectId quiescingObject = CreateNotification(firstIssuer, quiescing, 54U);
    const NotificationId firstId{firstRegistry, firstObject};
    const NotificationId secondId{firstRegistry, secondObject};
    const NotificationId foreignId{secondRegistry, foreignObject};
    const NotificationId quiescingId{firstRegistry, quiescingObject};

    const WaitSetSnapshot initial = waitSet.GetSnapshot();
    const auto invalid = waitSet.RegisterNotification({}, first, 1U);
    const auto wrongRegistry = waitSet.RegisterNotification(foreignId, foreign, 2U);
    const auto wrongInstance = waitSet.RegisterNotification(firstId, second, 3U);
    ASTERIA_EXPECT(
        runner, "invalid identities and mismatched storage leave registration unchanged", firstObject.IsValid(),
        secondObject.IsValid(), foreignObject.IsValid(), quiescingObject.IsValid(), !invalid.HasValue(),
        FailureIs(invalid.GetError(), WaitSetError::InvalidNotification), !wrongRegistry.HasValue(),
        FailureIs(wrongRegistry.GetError(), WaitSetError::NotificationRegistryMismatch), !wrongInstance.HasValue(),
        FailureIs(wrongInstance.GetError(), WaitSetError::NotificationInstanceMismatch),
        SnapshotIs(waitSet.GetSnapshot(), initial.State, initial.RegisteredEntries, initial.LastRegistrationSequence,
                   initial.LastClaimSequence, initial.NextScanIndex));

    static_cast<void>(first.Signal());
    const auto directClaim = first.Claim();
    const WaitSetSnapshot beforeClaimedAttach = waitSet.GetSnapshot();
    const NotificationSnapshot beforeClaimedAttachNotification = first.GetSnapshot();
    const auto claimedAttach = waitSet.RegisterNotification(firstId, first, 70U);
    const NotificationSnapshot afterClaimedAttachNotification = first.GetSnapshot();
    bool directClaimResolved{false};
    if (directClaim.HasValue())
    {
        directClaimResolved = first.CommitClaim(directClaim.GetValue()).HasValue();
    }
    ASTERIA_EXPECT(
        runner, "registration cannot steal or alter an active direct claim", directClaim.HasValue(),
        !claimedAttach.HasValue(),
        FailureIs(claimedAttach.GetError(), WaitSetError::NotificationRejected, NotificationError::ClaimActive),
        SnapshotIs(waitSet.GetSnapshot(), beforeClaimedAttach.State, beforeClaimedAttach.RegisteredEntries,
                   beforeClaimedAttach.LastRegistrationSequence, beforeClaimedAttach.LastClaimSequence,
                   beforeClaimedAttach.NextScanIndex),
        NotificationSnapshotsEqual(beforeClaimedAttachNotification, afterClaimedAttachNotification),
        directClaimResolved);

    const auto registration = waitSet.RegisterNotification(firstId, first, 71U);
    const auto managedCommit =
        first.CommitClaim(directClaim.HasValue() ? directClaim.GetValue() : Kernel::Ipc::NotificationClaimToken{});
    const auto managedCancel =
        first.CancelClaim(directClaim.HasValue() ? directClaim.GetValue() : Kernel::Ipc::NotificationClaimToken{});
    ASTERIA_EXPECT(runner, "registration pins one exact generation and transfers claim ownership",
                   registration.HasValue(), registration.GetValue().Registration.IsValid(),
                   registration.GetValue().Registration.GetSequence() == 1U,
                   registration.GetValue().Notification.IsEqualTo(firstId), registration.GetValue().Tag == 71U,
                   SnapshotIs(waitSet.GetSnapshot(), WaitSetState::Ready, 1U, 1U, 0U, 0U), !first.Claim().HasValue(),
                   first.Claim().GetError().Error == NotificationError::ManagedByWaitSet, !managedCommit.HasValue(),
                   managedCommit.GetError().Error == NotificationError::ManagedByWaitSet, !managedCancel.HasValue(),
                   managedCancel.GetError().Error == NotificationError::ManagedByWaitSet);
    if (!registration.HasValue())
    {
        return;
    }

    const auto duplicate = waitSet.RegisterNotification(firstId, first, 72U);
    const NotificationSnapshot beforeCrossSet = first.GetSnapshot();
    const auto crossSet = otherWaitSet.RegisterNotification(firstId, first, 73U);
    const NotificationSnapshot afterCrossSet = first.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "one Notification can belong to only one WaitSet", !duplicate.HasValue(),
        FailureIs(duplicate.GetError(), WaitSetError::DuplicateNotification), !crossSet.HasValue(),
        FailureIs(crossSet.GetError(), WaitSetError::NotificationRejected, NotificationError::ManagedByWaitSet),
        SnapshotIs(otherWaitSet.GetSnapshot(), WaitSetState::Ready, 0U, 0U, 0U, 0U),
        NotificationSnapshotsEqual(beforeCrossSet, afterCrossSet));

    static_cast<void>(firstIssuer.DestroyObject(quiescingObject));
    const auto rejectedQuiescing = waitSet.RegisterNotification(quiescingId, quiescing, 74U);
    ASTERIA_EXPECT(runner, "a quiescing Notification cannot gain a new WaitSet pin", !rejectedQuiescing.HasValue(),
                   FailureIs(rejectedQuiescing.GetError(), WaitSetError::NotificationPinRejected,
                             NotificationError::None, ObjectPinError::ObjectNotFound),
                   firstIssuer.CollectDestroyedObject().HasValue());

    static_cast<void>(firstIssuer.DestroyObject(firstObject));
    ASTERIA_EXPECT(runner, "the registration pin prevents collection of its Notification",
                   !firstIssuer.CollectDestroyedObject().HasValue());

    const auto foreignRemoval = otherWaitSet.Unregister(registration.GetValue().Registration);
    const auto removed = waitSet.Unregister(registration.GetValue().Registration);
    ASTERIA_EXPECT(runner, "Unregister releases the exact pin and makes token copies stale", !foreignRemoval.HasValue(),
                   FailureIs(foreignRemoval.GetError(), WaitSetError::InvalidRegistration), removed.HasValue(),
                   SnapshotIs(removed.GetValue(), WaitSetState::Ready, 0U, 1U, 0U, 0U),
                   firstIssuer.CollectDestroyedObject().HasValue(),
                   FailureIs(waitSet.Unregister(registration.GetValue().Registration).GetError(),
                             WaitSetError::StaleRegistration));

    const auto secondRegistration = waitSet.RegisterNotification(secondId, second, 81U);
    WaitSetRegistrationToken oldSecondRegistration{};
    if (secondRegistration.HasValue())
    {
        oldSecondRegistration = secondRegistration.GetValue().Registration;
        static_cast<void>(waitSet.Unregister(oldSecondRegistration));
    }
    const auto replacementRegistration = waitSet.RegisterNotification(secondId, second, 82U);
    ASTERIA_EXPECT(runner, "registration sequence prevents unregister ABA after slot reuse",
                   secondRegistration.HasValue(), replacementRegistration.HasValue(),
                   oldSecondRegistration.GetSequence() == 2U,
                   replacementRegistration.GetValue().Registration.GetSequence() == 3U,
                   FailureIs(waitSet.Unregister(oldSecondRegistration).GetError(), WaitSetError::StaleRegistration),
                   SnapshotIs(waitSet.GetSnapshot(), WaitSetState::Ready, 1U, 3U, 0U, 0U));
    if (replacementRegistration.HasValue())
    {
        static_cast<void>(waitSet.Unregister(replacementRegistration.GetValue().Registration));
    }

    static_cast<void>(firstIssuer.DestroyObject(secondObject));
    static_cast<void>(secondIssuer.DestroyObject(foreignObject));
    ASTERIA_EXPECT(runner, "registration failure paths retain no unrelated object pin",
                   firstIssuer.CollectDestroyedObject().HasValue(), secondIssuer.CollectDestroyedObject().HasValue());
}

void TestRoundRobinClaimAndCancellation(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification first;
    Notification second;
    Notification third;
    WaitSet waitSet{registry};
    static_cast<void>(waitSet.Initialize(ResourceDomainId{61U}));

    const ObjectId firstObject = CreateNotification(issuer, first, 71U);
    const ObjectId secondObject = CreateNotification(issuer, second, 72U);
    const ObjectId thirdObject = CreateNotification(issuer, third, 73U);
    const auto firstRegistration = waitSet.RegisterNotification(NotificationId{registry, firstObject}, first, 11U);
    const auto secondRegistration = waitSet.RegisterNotification(NotificationId{registry, secondObject}, second, 22U);
    const auto thirdRegistration = waitSet.RegisterNotification(NotificationId{registry, thirdObject}, third, 33U);
    if (!firstRegistration.HasValue() || !secondRegistration.HasValue() || !thirdRegistration.HasValue())
    {
        ASTERIA_EXPECT(runner, "three Notifications register for claim ordering", false);
        return;
    }

    static_cast<void>(second.Signal());
    static_cast<void>(third.Signal());
    ASTERIA_EXPECT(runner, "default claim tokens cannot alter a ready WaitSet",
                   FailureIs(waitSet.CommitClaim({}).GetError(), WaitSetError::InvalidClaim),
                   FailureIs(waitSet.CancelClaim({}).GetError(), WaitSetError::InvalidClaim));
    const auto secondClaim = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "ClaimReady selects the first pending registration from the cursor", secondClaim.HasValue(),
                   secondClaim.GetValue().Tag == 22U,
                   secondClaim.GetValue().Notification.IsEqualTo(NotificationId{registry, secondObject}),
                   secondClaim.GetValue().Registration.IsEqualTo(secondRegistration.GetValue().Registration),
                   secondClaim.GetValue().Claim.GetSequence() == 1U,
                   SnapshotIs(waitSet.GetSnapshot(), WaitSetState::Claimed, 3U, 3U, 1U, 2U, 1U));
    if (!secondClaim.HasValue())
    {
        return;
    }

    static_cast<void>(second.Signal());
    ASTERIA_EXPECT(
        runner, "an active claim freezes registration while later signals remain pending",
        FailureIs(waitSet.ClaimReady().GetError(), WaitSetError::ClaimActive),
        FailureIs(waitSet.Unregister(firstRegistration.GetValue().Registration).GetError(), WaitSetError::ClaimActive),
        waitSet.CommitClaim(secondClaim.GetValue().Claim).HasValue());

    const auto thirdClaim = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "round-robin selection reaches the next source and cancellation retries it",
                   thirdClaim.HasValue(), thirdClaim.GetValue().Tag == 33U,
                   thirdClaim.GetValue().Claim.GetSequence() == 2U,
                   waitSet.CancelClaim(thirdClaim.GetValue().Claim).HasValue());

    const auto repeatedSecond = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "a signal published during an earlier claim survives its commit", repeatedSecond.HasValue(),
                   repeatedSecond.GetValue().Tag == 22U,
                   waitSet.CommitClaim(repeatedSecond.GetValue().Claim).HasValue());

    const auto repeatedThird = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "cancelled readiness is delivered once and old claim tokens stay stale",
                   repeatedThird.HasValue(), repeatedThird.GetValue().Tag == 33U,
                   waitSet.CommitClaim(repeatedThird.GetValue().Claim).HasValue(),
                   FailureIs(waitSet.CommitClaim(thirdClaim.GetValue().Claim).GetError(), WaitSetError::StaleClaim),
                   FailureIs(waitSet.ClaimReady().GetError(), WaitSetError::WouldBlock));

    static_cast<void>(waitSet.Unregister(firstRegistration.GetValue().Registration));
    static_cast<void>(waitSet.Unregister(secondRegistration.GetValue().Registration));
    static_cast<void>(waitSet.Unregister(thirdRegistration.GetValue().Registration));
    static_cast<void>(issuer.DestroyObject(firstObject));
    static_cast<void>(issuer.DestroyObject(secondObject));
    static_cast<void>(issuer.DestroyObject(thirdObject));
    ASTERIA_EXPECT(runner, "resolved registrations release every retained Notification",
                   issuer.CollectDestroyedObject().HasValue(), issuer.CollectDestroyedObject().HasValue(),
                   issuer.CollectDestroyedObject().HasValue());
}

void TestForeignClaimAndDestructorCleanup(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification first;
    Notification second;
    Notification detached;
    const ObjectId firstObject = CreateNotification(issuer, first, 101U);
    const ObjectId secondObject = CreateNotification(issuer, second, 102U);
    const ObjectId detachedObject = CreateNotification(issuer, detached, 103U);
    WaitSet firstSet{registry};
    WaitSet secondSet{registry};
    static_cast<void>(firstSet.Initialize(ResourceDomainId{111U}));
    static_cast<void>(secondSet.Initialize(ResourceDomainId{112U}));

    const auto firstRegistration = firstSet.RegisterNotification(NotificationId{registry, firstObject}, first, 1U);
    const auto secondRegistration = secondSet.RegisterNotification(NotificationId{registry, secondObject}, second, 2U);
    static_cast<void>(first.Signal());
    static_cast<void>(second.Signal());
    const auto firstClaim = firstSet.ClaimReady();
    const auto secondClaim = secondSet.ClaimReady();
    ASTERIA_EXPECT(runner, "a same-sequence claim token from another WaitSet is rejected", firstRegistration.HasValue(),
                   secondRegistration.HasValue(), firstClaim.HasValue(), secondClaim.HasValue(),
                   firstClaim.GetValue().Claim.GetSequence() == secondClaim.GetValue().Claim.GetSequence(),
                   FailureIs(firstSet.CommitClaim(secondClaim.GetValue().Claim).GetError(), WaitSetError::InvalidClaim),
                   SnapshotIs(firstSet.GetSnapshot(), WaitSetState::Claimed, 1U, 1U, 1U, 1U, 1U));
    if (firstClaim.HasValue())
    {
        static_cast<void>(firstSet.CommitClaim(firstClaim.GetValue().Claim));
    }
    if (secondClaim.HasValue())
    {
        static_cast<void>(secondSet.CommitClaim(secondClaim.GetValue().Claim));
    }
    if (firstRegistration.HasValue())
    {
        static_cast<void>(firstSet.Unregister(firstRegistration.GetValue().Registration));
    }
    if (secondRegistration.HasValue())
    {
        static_cast<void>(secondSet.Unregister(secondRegistration.GetValue().Registration));
    }

    {
        WaitSet scopedSet{registry};
        static_cast<void>(scopedSet.Initialize(ResourceDomainId{113U}));
        const auto registration =
            scopedSet.RegisterNotification(NotificationId{registry, detachedObject}, detached, 3U);
        ASTERIA_EXPECT(runner, "a scoped WaitSet retains one Notification before teardown", registration.HasValue());
    }
    static_cast<void>(detached.Signal());
    const auto detachedClaim = detached.Claim();
    ASTERIA_EXPECT(runner, "WaitSet teardown detaches registrations before its storage dies", detachedClaim.HasValue(),
                   detached.CommitClaim(detachedClaim.GetValue()).HasValue());

    static_cast<void>(issuer.DestroyObject(firstObject));
    static_cast<void>(issuer.DestroyObject(secondObject));
    static_cast<void>(issuer.DestroyObject(detachedObject));
    ASTERIA_EXPECT(runner, "foreign-claim and destructor fixtures release every object pin",
                   issuer.CollectDestroyedObject().HasValue(), issuer.CollectDestroyedObject().HasValue(),
                   issuer.CollectDestroyedObject().HasValue());
}

void TestCapacityAndSequencePolicy(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification notifications[MaximumWaitSetEntries + 1U]{};
    WaitSet waitSet{registry};
    static_cast<void>(waitSet.Initialize(ResourceDomainId{81U}));
    ObjectId objects[MaximumWaitSetEntries + 1U]{};
    WaitSetRegistrationToken registrations[MaximumWaitSetEntries]{};
    bool setupSucceeded{true};

    for (Foundation::Size index{0U}; index < MaximumWaitSetEntries + 1U; ++index)
    {
        objects[index] = CreateNotification(issuer, notifications[index], 100U + index);
        setupSucceeded = setupSucceeded && objects[index].IsValid();
    }
    for (Foundation::Size index{0U}; index < MaximumWaitSetEntries; ++index)
    {
        const auto registration = waitSet.RegisterNotification(NotificationId{registry, objects[index]},
                                                               notifications[index], 1'000U + index);
        if (registration.HasValue())
        {
            registrations[index] = registration.GetValue().Registration;
        }
        else
        {
            setupSucceeded = false;
        }
    }

    const WaitSetSnapshot full = waitSet.GetSnapshot();
    const auto overflow = waitSet.RegisterNotification(NotificationId{registry, objects[MaximumWaitSetEntries]},
                                                       notifications[MaximumWaitSetEntries], 2'000U);
    ASTERIA_EXPECT(runner, "WaitSet capacity rejects entry 33 without changing any sequence", setupSucceeded,
                   SnapshotIs(full, WaitSetState::Ready, MaximumWaitSetEntries, MaximumWaitSetEntries, 0U, 0U),
                   !overflow.HasValue(), FailureIs(overflow.GetError(), WaitSetError::SetFull),
                   SnapshotIs(waitSet.GetSnapshot(), full.State, full.RegisteredEntries, full.LastRegistrationSequence,
                              full.LastClaimSequence, full.NextScanIndex));

    Foundation::UInt64 next{7U};
    const bool ordinaryAdvance = WaitSetSequencePolicy::TryAdvance(7U, next);
    Foundation::UInt64 preserved{19U};
    const bool wrapped = WaitSetSequencePolicy::TryAdvance(WaitSetSequencePolicy::Maximum, preserved);
    ASTERIA_EXPECT(runner, "WaitSet sequence arithmetic rejects wrap without modifying output", ordinaryAdvance,
                   next == 8U, !wrapped, preserved == 19U);

    for (const WaitSetRegistrationToken registration : registrations)
    {
        static_cast<void>(waitSet.Unregister(registration));
    }
    for (const ObjectId object : objects)
    {
        static_cast<void>(issuer.DestroyObject(object));
    }
    bool allCollected{true};
    for (Foundation::Size index{0U}; index < MaximumWaitSetEntries + 1U; ++index)
    {
        allCollected = allCollected && issuer.CollectDestroyedObject().HasValue();
    }
    ASTERIA_EXPECT(runner, "full-set teardown releases every registration pin", allCollected,
                   SnapshotIs(waitSet.GetSnapshot(), WaitSetState::Ready, 0U, MaximumWaitSetEntries, 0U, 0U));
}

void TestConcurrentMembershipAndClaim(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification notification;
    WaitSet first{registry};
    WaitSet second{registry};
    static_cast<void>(first.Initialize(ResourceDomainId{91U}));
    static_cast<void>(second.Initialize(ResourceDomainId{92U}));
    const ObjectId object = CreateNotification(issuer, notification, 93U);
    const NotificationId identity{registry, object};
    WaitSet* const sets[2]{&first, &second};

    std::atomic<bool> startRegistration{false};
    std::atomic<Foundation::Size> readyRegistrars{0U};
    WaitSetRegistrationToken registrationTokens[2]{};
    WaitSetError registrationErrors[2]{WaitSetError::None, WaitSetError::None};
    NotificationError notificationErrors[2]{
        NotificationError::None,
        NotificationError::None,
    };
    const auto registrar = [&](const Foundation::Size index) noexcept
    {
        static_cast<void>(readyRegistrars.fetch_add(1U, std::memory_order_release));
        bool started{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (startRegistration.load(std::memory_order_acquire))
            {
                started = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!started)
        {
            registrationErrors[index] = WaitSetError::NotInitialized;
            return;
        }
        const auto result = sets[index]->RegisterNotification(identity, notification, 10U + index);
        if (result.HasValue())
        {
            registrationTokens[index] = result.GetValue().Registration;
        }
        else
        {
            registrationErrors[index] = result.GetError().Error;
            notificationErrors[index] = result.GetError().Notification.Error;
        }
    };

    std::thread registrars[2]{
        std::thread{registrar, Foundation::Size{0U}},
        std::thread{registrar, Foundation::Size{1U}},
    };
    bool registrarsReady{false};
    for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (readyRegistrars.load(std::memory_order_acquire) == 2U)
        {
            registrarsReady = true;
            break;
        }
        std::this_thread::yield();
    }
    startRegistration.store(true, std::memory_order_release);
    for (auto& thread : registrars)
    {
        thread.join();
    }

    const Foundation::Size registrationWinner = registrationTokens[0].IsValid() ? 0U : 1U;
    const Foundation::Size registrationLoser = registrationWinner == 0U ? 1U : 0U;
    ASTERIA_EXPECT(runner, "competing WaitSets establish exactly one global membership", registrarsReady,
                   registrationTokens[registrationWinner].IsValid(), !registrationTokens[registrationLoser].IsValid(),
                   registrationErrors[registrationLoser] == WaitSetError::NotificationRejected,
                   notificationErrors[registrationLoser] == NotificationError::ManagedByWaitSet);

    static_cast<void>(sets[registrationWinner]->Unregister(registrationTokens[registrationWinner]));
    const auto retry = sets[registrationLoser]->RegisterNotification(identity, notification, 21U);
    ASTERIA_EXPECT(runner, "the losing WaitSet can register after exact membership release", retry.HasValue());
    if (!retry.HasValue())
    {
        return;
    }

    static_cast<void>(notification.Signal());
    std::atomic<bool> startClaim{false};
    std::atomic<Foundation::Size> readyClaimers{0U};
    WaitSetClaimToken claimTokens[2]{};
    WaitSetError claimErrors[2]{WaitSetError::None, WaitSetError::None};
    const auto claimer = [&](const Foundation::Size index) noexcept
    {
        static_cast<void>(readyClaimers.fetch_add(1U, std::memory_order_release));
        bool started{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (startClaim.load(std::memory_order_acquire))
            {
                started = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!started)
        {
            claimErrors[index] = WaitSetError::NotInitialized;
            return;
        }
        const auto result = sets[registrationLoser]->ClaimReady();
        if (result.HasValue())
        {
            claimTokens[index] = result.GetValue().Claim;
        }
        else
        {
            claimErrors[index] = result.GetError().Error;
        }
    };

    std::thread claimers[2]{
        std::thread{claimer, Foundation::Size{0U}},
        std::thread{claimer, Foundation::Size{1U}},
    };
    bool claimersReady{false};
    for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (readyClaimers.load(std::memory_order_acquire) == 2U)
        {
            claimersReady = true;
            break;
        }
        std::this_thread::yield();
    }
    startClaim.store(true, std::memory_order_release);
    for (auto& thread : claimers)
    {
        thread.join();
    }

    const Foundation::Size claimWinner = claimTokens[0].IsValid() ? 0U : 1U;
    const Foundation::Size claimLoser = claimWinner == 0U ? 1U : 0U;
    ASTERIA_EXPECT(runner, "two concurrent consumers publish exactly one WaitSet claim", claimersReady,
                   claimTokens[claimWinner].IsValid(), !claimTokens[claimLoser].IsValid(),
                   claimErrors[claimLoser] == WaitSetError::ClaimActive,
                   sets[registrationLoser]->CommitClaim(claimTokens[claimWinner]).HasValue());

    static_cast<void>(sets[registrationLoser]->Unregister(retry.GetValue().Registration));
    static_cast<void>(issuer.DestroyObject(object));
    ASTERIA_EXPECT(runner, "concurrent membership and claim teardown release the object pin",
                   issuer.CollectDestroyedObject().HasValue());
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"WaitSetTests"};
    Asteria::Test::TestInitializationAndRegistration(runner);
    Asteria::Test::TestRegistrationIdentityAndLifetime(runner);
    Asteria::Test::TestRoundRobinClaimAndCancellation(runner);
    Asteria::Test::TestForeignClaimAndDestructorCleanup(runner);
    Asteria::Test::TestCapacityAndSequencePolicy(runner);
    Asteria::Test::TestConcurrentMembershipAndClaim(runner);
    return runner.Finish();
}
