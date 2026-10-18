// SPDX-License-Identifier: Apache-2.0
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
#include "Kernel/Time/TimerNotificationDelivery.hpp"

extern "C" [[gnu::used, gnu::noinline]] Asteria::Foundation::UInt64 AsteriaTimerNotificationDeliveryTargetFixture(
    Asteria::Kernel::Time::TimerNotificationDelivery& delivery, const Asteria::Kernel::Time::TimerId timerId,
    Asteria::Kernel::Time::Timer& timer, const Asteria::Kernel::Ipc::NotificationId notificationId,
    Asteria::Kernel::Ipc::Notification& notification) noexcept
{
    const auto result = delivery.Deliver(timerId, timer, notificationId, notification);
    if (result.HasValue())
    {
        return result.GetValue().ClaimSequence ^ result.GetValue().Expiration.ObservedTicks;
    }
    return result.GetErrorReference().ClaimSequence ^
           static_cast<Asteria::Foundation::UInt64>(result.GetErrorReference().Error);
}

#else

#include <cstddef>
#include <cstdio>

#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Time/TimerNotificationDelivery.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationError;
using Kernel::Ipc::NotificationId;
using Kernel::Ipc::NotificationState;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Time::Timer;
using Kernel::Time::TimerError;
using Kernel::Time::TimerId;
using Kernel::Time::TimerNotificationDelivery;
using Kernel::Time::TimerNotificationDeliveryError;
using Kernel::Time::TimerNotificationDeliveryFailure;
using Kernel::Time::TimerQueue;
using Kernel::Time::TimerQueueError;
using Kernel::Time::TimerQueueExpirationClaim;
using Kernel::Time::TimerState;
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

[[nodiscard]] ObjectId RegisterTimer(Timer& timer, RootCapabilityIssuer& issuer,
                                     const Foundation::UInt64 owner) noexcept
{
    if (timer.Initialize(ResourceDomainId{owner}) != TimerError::None)
    {
        return {};
    }
    const auto object = issuer.CreateObject(timer);
    return object.HasValue() ? object.GetValue() : ObjectId{};
}

[[nodiscard]] ObjectId RegisterNotification(Notification& notification, RootCapabilityIssuer& issuer,
                                            const Foundation::UInt64 owner) noexcept
{
    if (notification.Initialize(ResourceDomainId{owner}) != NotificationError::None)
    {
        return {};
    }
    const auto object = issuer.CreateObject(notification);
    return object.HasValue() ? object.GetValue() : ObjectId{};
}

[[nodiscard]] bool RetireObject(RootCapabilityIssuer& issuer, const ObjectId object,
                                void* const expectedStorage) noexcept
{
    if (!object.IsValid() || (issuer.DestroyObject(object) != ObjectRegistryError::Ok))
    {
        return false;
    }
    const auto collected = issuer.CollectDestroyedObject();
    return collected.HasValue() && (collected.GetValue().Object.GetValue() == object.GetValue()) &&
           (collected.GetValue().Instance == expectedStorage);
}

[[nodiscard]] bool FailureIs(const TimerNotificationDeliveryFailure& failure,
                             const TimerNotificationDeliveryError error, const Foundation::UInt8 pending,
                             const Foundation::UInt8 claimed = 0U, const Foundation::UInt8 retired = 0U) noexcept
{
    return (failure.Error == error) && (failure.QueueState.PendingEntries == pending) &&
           (failure.QueueState.ClaimedEntries == claimed) && (failure.NotificationPublished == 0U) &&
           (failure.QueueCommitted == 0U) && (failure.Cause == TimerNotificationDeliveryError::None) &&
           (failure.ClaimRetired == retired);
}

void TestHappyPathAndLifetime(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{4U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 31U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 32U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};

    bool delivered{false};
    {
        TimerQueue queue{registry, clock};
        TimerNotificationDelivery delivery{registry, queue};
        const NotificationError seededSignal = notification.Signal();
        const auto retainedNotificationClaim = notification.Claim();
        const auto armed = queue.Arm(timerId, timer, MonotonicTime{10U});
        clock.Set(10U);
        const auto result = delivery.Deliver(timerId, timer, notificationId, notification);
        const auto timerSnapshot = timer.GetSnapshot();
        const auto notificationSnapshot = notification.GetSnapshot();
        const auto queueState = queue.GetState();
        delivered = armed.HasValue() && result.HasValue() && result.GetValue().TimerIdentity.IsEqualTo(timerId) &&
                    result.GetValue().NotificationIdentity.IsEqualTo(notificationId) &&
                    result.GetValue().Expiration.Token.IsEqualTo(armed.GetValue().Timer.Token) &&
                    (result.GetValue().Expiration.DeadlineTicks == 10U) &&
                    (result.GetValue().Expiration.ObservedTicks == 10U) && (result.GetValue().ClaimSequence == 1U) &&
                    (result.GetValue().NotificationPublished == 1U) && (result.GetValue().QueueCommitted == 1U) &&
                    BytesAreZero(result.GetValue().Reserved, sizeof(result.GetValue().Reserved)) &&
                    (queueState.ScheduledEntries == 0U) && (queueState.PendingEntries == 0U) &&
                    (queueState.ClaimedEntries == 0U) && (timerSnapshot.State == TimerState::Disarmed) &&
                    (result.GetValue().NotificationState.State == NotificationState::Claimed) &&
                    (result.GetValue().NotificationState.Pending == 1U) &&
                    (notificationSnapshot.State == NotificationState::Claimed) && (notificationSnapshot.Pending == 1U);

        const bool retainedConsumed = retainedNotificationClaim.HasValue() &&
                                      notification.CommitClaim(retainedNotificationClaim.GetValue()).HasValue();
        const auto notificationClaim = notification.Claim();
        const bool consumed =
            notificationClaim.HasValue() && notification.CommitClaim(notificationClaim.GetValue()).HasValue();
        ASTERIA_EXPECT(runner, "delivery publishes, commits, acknowledges, and exposes exact diagnostics",
                       seededSignal == NotificationError::None, retainedConsumed, delivered, consumed);
    }

    ASTERIA_EXPECT(runner, "successful delivery releases both independent registry pins exactly", delivered,
                   RetireObject(issuer, timerObject, &timer), RetireObject(issuer, notificationObject, &notification));
}

void TestEarlyAndStaleClaimPaths(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{1U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 41U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 42U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};

    {
        TimerQueue queue{registry, clock};
        TimerNotificationDelivery delivery{registry, queue};
        const auto empty = delivery.Deliver(timerId, timer, notificationId, notification);
        const auto armed = queue.Arm(timerId, timer, MonotonicTime{8U});
        const auto early = delivery.Deliver(timerId, timer, notificationId, notification);
        ASTERIA_EXPECT(runner, "empty and early queues fail without a claim or notification mutation",
                       !empty.HasValue(), empty.GetError().Error == TimerNotificationDeliveryError::QueueClaimRejected,
                       empty.GetError().Queue.Error == TimerQueueError::Empty, !early.HasValue(),
                       early.GetError().Queue.Error == TimerQueueError::NotDue,
                       early.GetError().Queue.Timer.Error == TimerError::NotExpired,
                       early.GetError().Queue.Timer.ObservedTicks == 1U, queue.GetState().ScheduledEntries == 1U,
                       timer.GetSnapshot().State == TimerState::Armed, notification.GetSnapshot().Pending == 0U);

        if (!armed.HasValue())
        {
            return;
        }
        clock.Set(8U);
        const auto externalClaim = queue.ClaimDue();
        const auto whileClaimed = delivery.Deliver(timerId, timer, notificationId, notification);
        const bool restored =
            externalClaim.HasValue() && queue.CancelClaim(externalClaim.GetValue().GetClaimToken()).HasValue();
        const auto invalidNotification = delivery.Deliver(timerId, timer, {}, notification);
        const auto staleCommit = externalClaim.HasValue() ? queue.CommitClaim(externalClaim.GetValue().GetClaimToken())
                                                          : queue.CommitClaim({});
        const auto retried = delivery.Deliver(timerId, timer, notificationId, notification);
        ASTERIA_EXPECT(
            runner, "claimed work is preserved and stale public tokens cannot commit a retry", externalClaim.HasValue(),
            !whileClaimed.HasValue(), whileClaimed.GetError().Queue.Error == TimerQueueError::Empty,
            whileClaimed.GetError().QueueState.ClaimedEntries == 1U, restored, !invalidNotification.HasValue(),
            FailureIs(invalidNotification.GetError(), TimerNotificationDeliveryError::InvalidNotification, 1U),
            invalidNotification.GetError().ClaimSequence == 2U, !staleCommit.HasValue(),
            staleCommit.GetError().Error == TimerQueueError::StaleClaim, retried.HasValue(),
            retried.GetValue().ClaimSequence == 3U);

        const auto notificationClaim = notification.Claim();
        if (notificationClaim.HasValue())
        {
            static_cast<void>(notification.CommitClaim(notificationClaim.GetValue()));
        }
    }

    ASTERIA_EXPECT(runner, "early and retry paths leave no retained generation",
                   RetireObject(issuer, timerObject, &timer), RetireObject(issuer, notificationObject, &notification));
}

void TestExactIdentityValidation(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    SpinLock foreignRegistryLock;
    Asteria::TestSupport::TestObjectRegistry foreignRegistry{foreignRegistryLock};
    RootCapabilityIssuer foreignIssuer{foreignRegistry};
    ManualClock clock{0U};
    Timer firstTimer{clock};
    Timer secondTimer{clock};
    Timer foreignTimer{clock};
    Notification firstNotification;
    Notification secondNotification;
    Notification foreignNotification;
    Notification uninitializedNotification;
    const ObjectId firstTimerObject = RegisterTimer(firstTimer, issuer, 51U);
    const ObjectId secondTimerObject = RegisterTimer(secondTimer, issuer, 52U);
    const ObjectId foreignTimerObject = RegisterTimer(foreignTimer, foreignIssuer, 53U);
    const ObjectId firstNotificationObject = RegisterNotification(firstNotification, issuer, 54U);
    const ObjectId secondNotificationObject = RegisterNotification(secondNotification, issuer, 55U);
    const ObjectId foreignNotificationObject = RegisterNotification(foreignNotification, foreignIssuer, 56U);
    const TimerId firstTimerId{registry, firstTimerObject};
    const TimerId secondTimerId{registry, secondTimerObject};
    const TimerId foreignTimerId{foreignRegistry, foreignTimerObject};
    const NotificationId firstNotificationId{registry, firstNotificationObject};
    const NotificationId secondNotificationId{registry, secondNotificationObject};
    const NotificationId foreignNotificationId{foreignRegistry, foreignNotificationObject};

    {
        TimerQueue queue{registry, clock};
        TimerNotificationDelivery delivery{registry, queue};
        const auto firstArm = queue.Arm(firstTimerId, firstTimer, MonotonicTime{10U});
        const auto secondArm = queue.Arm(secondTimerId, secondTimer, MonotonicTime{20U});
        clock.Set(10U);
        const auto differentEarliest =
            delivery.Deliver(secondTimerId, secondTimer, firstNotificationId, firstNotification);
        const auto foreignTimerIdentity =
            delivery.Deliver(foreignTimerId, firstTimer, firstNotificationId, firstNotification);
        const auto foreignNotificationResult =
            delivery.Deliver(firstTimerId, firstTimer, foreignNotificationId, foreignNotification);
        const auto notificationInstance =
            delivery.Deliver(firstTimerId, firstTimer, firstNotificationId, uninitializedNotification);
        const NotificationId wrongTypeNotification{registry, secondTimerObject};
        const auto wrongType = delivery.Deliver(firstTimerId, firstTimer, wrongTypeNotification, firstNotification);
        const auto deliveredFirst = delivery.Deliver(firstTimerId, firstTimer, firstNotificationId, firstNotification);

        ASTERIA_EXPECT(
            runner, "foreign, unproven-storage, and wrong-type identities retry Pending exactly", firstArm.HasValue(),
            secondArm.HasValue(), !differentEarliest.HasValue(),
            FailureIs(differentEarliest.GetError(), TimerNotificationDeliveryError::ClaimedTimerMismatch, 1U),
            differentEarliest.GetError().TimerIdentity.IsEqualTo(firstTimerId), !foreignTimerIdentity.HasValue(),
            FailureIs(foreignTimerIdentity.GetError(), TimerNotificationDeliveryError::TimerRegistryMismatch, 1U),
            !foreignNotificationResult.HasValue(),
            FailureIs(foreignNotificationResult.GetError(),
                      TimerNotificationDeliveryError::NotificationRegistryMismatch, 1U),
            !notificationInstance.HasValue(),
            FailureIs(notificationInstance.GetError(), TimerNotificationDeliveryError::NotificationInstanceMismatch,
                      1U),
            notificationInstance.GetError().NotificationPin == ObjectPinError::Ok,
            notificationInstance.GetError().Notification == NotificationError::None,
            notificationInstance.GetError().NotificationState.State == NotificationState::Uninitialized,
            notificationInstance.GetError().NotificationState.Pending == 0U, !uninitializedNotification.IsInitialized(),
            !wrongType.HasValue(),
            FailureIs(wrongType.GetError(), TimerNotificationDeliveryError::NotificationPinRejected, 1U),
            wrongType.GetError().NotificationPin == ObjectPinError::TypeMismatch, deliveredFirst.HasValue(),
            firstTimer.GetSnapshot().State == TimerState::Disarmed, queue.GetState().ScheduledEntries == 1U);

        const auto firstClaim = firstNotification.Claim();
        if (firstClaim.HasValue())
        {
            static_cast<void>(firstNotification.CommitClaim(firstClaim.GetValue()));
        }
        clock.Set(20U);
        const auto deliveredSecond =
            delivery.Deliver(secondTimerId, secondTimer, secondNotificationId, secondNotification);
        const auto secondClaim = secondNotification.Claim();
        if (secondClaim.HasValue())
        {
            static_cast<void>(secondNotification.CommitClaim(secondClaim.GetValue()));
        }
        ASTERIA_EXPECT(runner, "different-earliest rejection leaves later work ordered and deliverable",
                       deliveredSecond.HasValue(), queue.GetState().ScheduledEntries == 0U);
    }

    ASTERIA_EXPECT(runner, "identity validation releases local and foreign object generations",
                   RetireObject(issuer, firstTimerObject, &firstTimer),
                   RetireObject(issuer, secondTimerObject, &secondTimer),
                   RetireObject(issuer, firstNotificationObject, &firstNotification),
                   RetireObject(issuer, secondNotificationObject, &secondNotification),
                   RetireObject(foreignIssuer, foreignTimerObject, &foreignTimer),
                   RetireObject(foreignIssuer, foreignNotificationObject, &foreignNotification));
}

void TestPinFailuresRetainExactLifetime(TestContext& runner) noexcept
{
    SpinLock timerRegistryLock;
    Asteria::TestSupport::TestObjectRegistry timerRegistry{timerRegistryLock};
    RootCapabilityIssuer timerIssuer{timerRegistry};
    ManualClock timerClock{0U};
    Timer quiescingTimer{timerClock};
    Notification timerNotification;
    const ObjectId quiescingTimerObject = RegisterTimer(quiescingTimer, timerIssuer, 61U);
    const ObjectId timerNotificationObject = RegisterNotification(timerNotification, timerIssuer, 62U);
    const TimerId quiescingTimerId{timerRegistry, quiescingTimerObject};
    const NotificationId timerNotificationId{timerRegistry, timerNotificationObject};
    {
        TimerQueue queue{timerRegistry, timerClock};
        TimerNotificationDelivery delivery{timerRegistry, queue};
        const auto armed = queue.Arm(quiescingTimerId, quiescingTimer, MonotonicTime{5U});
        const ObjectRegistryError destroyed = timerIssuer.DestroyObject(quiescingTimerObject);
        timerClock.Set(5U);
        const auto rejected =
            delivery.Deliver(quiescingTimerId, quiescingTimer, timerNotificationId, timerNotification);
        const auto timerCollected = timerIssuer.CollectDestroyedObject();
        ASTERIA_EXPECT(runner,
                       "a Timer destroyed while queued has its expiration retired, "
                       "which releases the queue pin",
                       armed.HasValue(), destroyed == ObjectRegistryError::Ok, !rejected.HasValue(),
                       FailureIs(rejected.GetError(), TimerNotificationDeliveryError::TimerPinRejected, 0U, 0U, 1U),
                       rejected.GetError().TimerPin == ObjectPinError::ObjectNotFound,
                       rejected.GetError().QueueState.ScheduledEntries == 0U, queue.GetState().PendingEntries == 0U,
                       quiescingTimer.GetSnapshot().State == TimerState::Disarmed,
                       timerNotification.GetSnapshot().Pending == 0U, timerCollected.HasValue(),
                       timerCollected.GetValue().Instance == &quiescingTimer);
    }
    ASTERIA_EXPECT(runner, "retirement leaves the unused Notification unpinned",
                   RetireObject(timerIssuer, timerNotificationObject, &timerNotification));

    SpinLock notificationRegistryLock;
    Asteria::TestSupport::TestObjectRegistry notificationRegistry{notificationRegistryLock};
    RootCapabilityIssuer notificationIssuer{notificationRegistry};
    ManualClock notificationClock{0U};
    Timer notificationTimer{notificationClock};
    Notification quiescingNotification;
    Notification unprovenNotification;
    const ObjectId notificationTimerObject = RegisterTimer(notificationTimer, notificationIssuer, 63U);
    const ObjectId quiescingNotificationObject = RegisterNotification(quiescingNotification, notificationIssuer, 64U);
    const TimerId notificationTimerId{notificationRegistry, notificationTimerObject};
    const NotificationId quiescingNotificationId{notificationRegistry, quiescingNotificationObject};
    {
        TimerQueue queue{notificationRegistry, notificationClock};
        TimerNotificationDelivery delivery{notificationRegistry, queue};
        const auto armed = queue.Arm(notificationTimerId, notificationTimer, MonotonicTime{7U});
        const ObjectRegistryError destroyed = notificationIssuer.DestroyObject(quiescingNotificationObject);
        notificationClock.Set(7U);
        const auto rejected =
            delivery.Deliver(notificationTimerId, notificationTimer, quiescingNotificationId, unprovenNotification);
        const auto notificationCollected = notificationIssuer.CollectDestroyedObject();
        ASTERIA_EXPECT(
            runner, "quiescing Notification pin rejection never dereferences unproven storage and preserves retry",
            armed.HasValue(), destroyed == ObjectRegistryError::Ok, !rejected.HasValue(),
            FailureIs(rejected.GetError(), TimerNotificationDeliveryError::NotificationPinRejected, 1U),
            rejected.GetError().NotificationPin == ObjectPinError::ObjectNotFound,
            rejected.GetError().Notification == NotificationError::None,
            rejected.GetError().NotificationState.State == NotificationState::Uninitialized,
            !unprovenNotification.IsInitialized(), notificationCollected.HasValue(),
            notificationCollected.GetValue().Instance == &quiescingNotification, queue.GetState().PendingEntries == 1U,
            notificationTimer.GetSnapshot().State == TimerState::Expired);
    }
    ASTERIA_EXPECT(runner, "Notification pin failure releases its provisional independent Timer pin",
                   RetireObject(notificationIssuer, notificationTimerObject, &notificationTimer));
}

void TestClaimedDeliveryAndAdversarialValidation(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 71U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 72U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};
    bool timerRetired{false};

    {
        TimerQueue queue{registry, clock};
        TimerQueue foreignQueue{registry, clock};
        TimerNotificationDelivery delivery{registry, queue};
        TimerNotificationDelivery foreignDelivery{registry, foreignQueue};
        const auto firstArm = queue.Arm(timerId, timer, MonotonicTime{10U});
        clock.Set(10U);
        const auto firstClaim = queue.ClaimDue();
        const auto rejected = firstClaim.HasValue()
                                  ? delivery.DeliverClaimed(firstClaim.GetValue(), {}, notification)
                                  : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, {}, notification);
        const auto notificationAfterRejected = notification.GetSnapshot();
        const auto retryClaim = queue.ClaimDue();
        const auto delivered = retryClaim.HasValue()
                                   ? delivery.DeliverClaimed(retryClaim.GetValue(), notificationId, notification)
                                   : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
        const auto firstNotificationClaim = notification.Claim();
        const bool firstConsumed =
            firstNotificationClaim.HasValue() && notification.CommitClaim(firstNotificationClaim.GetValue()).HasValue();
        ASTERIA_EXPECT(runner, "claimed delivery restores pre-publication rejection and consumes the exact retry claim",
                       firstArm.HasValue(), firstClaim.HasValue(), !rejected.HasValue(),
                       FailureIs(rejected.GetError(), TimerNotificationDeliveryError::InvalidNotification, 1U),
                       notificationAfterRejected.State == NotificationState::Ready,
                       notificationAfterRejected.Pending == 0U, retryClaim.HasValue(), delivered.HasValue(),
                       delivered.GetValue().TimerIdentity.IsEqualTo(timerId), delivered.GetValue().ClaimSequence == 2U,
                       firstConsumed);

        const auto secondArm = queue.Arm(timerId, timer, MonotonicTime{20U});
        clock.Set(20U);
        const auto staleClaim = queue.ClaimDue();
        const bool staleRestored =
            staleClaim.HasValue() && queue.CancelClaim(staleClaim.GetValue().GetClaimToken()).HasValue();
        const auto liveClaim = queue.ClaimDue();
        const auto staleAttempt =
            staleClaim.HasValue() ? delivery.DeliverClaimed(staleClaim.GetValue(), notificationId, notification)
                                  : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
        const auto notificationAfterStale = notification.GetSnapshot();
        const auto foreignAttempt =
            liveClaim.HasValue()
                ? foreignDelivery.DeliverClaimed(liveClaim.GetValue(), notificationId, notification)
                : foreignDelivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
        const auto notificationAfterForeign = notification.GetSnapshot();
        const auto finalDelivery =
            liveClaim.HasValue() ? delivery.DeliverClaimed(liveClaim.GetValue(), notificationId, notification)
                                 : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
        const auto finalNotificationClaim = notification.Claim();
        const bool finalConsumed =
            finalNotificationClaim.HasValue() && notification.CommitClaim(finalNotificationClaim.GetValue()).HasValue();
        const ObjectRegistryError timerDestroyed = issuer.DestroyObject(timerObject);
        const auto timerCollected = issuer.CollectDestroyedObject();
        timerRetired = (timerDestroyed == ObjectRegistryError::Ok) && timerCollected.HasValue() &&
                       (timerCollected.GetValue().Instance == &timer);
        const auto consumedAttempt =
            liveClaim.HasValue() ? delivery.DeliverClaimed(liveClaim.GetValue(), notificationId, notification)
                                 : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
        const auto notificationAfterConsumed = notification.GetSnapshot();
        const auto invalidClaim = delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
        const auto notificationAfterInvalid = notification.GetSnapshot();
        ASTERIA_EXPECT(
            runner,
            "claim adoption rejects stale, foreign, consumed, and invalid "
            "copies before publication without disturbing the live claim",
            secondArm.HasValue(), staleClaim.HasValue(), staleRestored, liveClaim.HasValue(), !staleAttempt.HasValue(),
            staleAttempt.GetError().Error == TimerNotificationDeliveryError::InvalidClaim,
            staleAttempt.GetError().Queue.Error == TimerQueueError::StaleClaim,
            staleAttempt.GetError().NotificationPublished == 0U, staleAttempt.GetError().QueueCommitted == 0U,
            staleAttempt.GetError().QueueState.ClaimedEntries == 1U,
            notificationAfterStale.State == NotificationState::Ready, notificationAfterStale.Pending == 0U,
            !foreignAttempt.HasValue(), foreignAttempt.GetError().Error == TimerNotificationDeliveryError::InvalidClaim,
            foreignAttempt.GetError().Queue.Error == TimerQueueError::InvalidClaim,
            foreignAttempt.GetError().NotificationPublished == 0U,
            notificationAfterForeign.State == NotificationState::Ready, notificationAfterForeign.Pending == 0U,
            finalDelivery.HasValue(), finalDelivery.GetValue().ClaimSequence == 4U, finalConsumed, timerRetired,
            !consumedAttempt.HasValue(),
            consumedAttempt.GetError().Error == TimerNotificationDeliveryError::InvalidClaim,
            consumedAttempt.GetError().Queue.Error == TimerQueueError::StaleClaim,
            consumedAttempt.GetError().NotificationPublished == 0U,
            notificationAfterConsumed.State == NotificationState::Ready, notificationAfterConsumed.Pending == 0U,
            !invalidClaim.HasValue(), invalidClaim.GetError().Error == TimerNotificationDeliveryError::InvalidClaim,
            invalidClaim.GetError().NotificationPublished == 0U,
            notificationAfterInvalid.State == NotificationState::Ready, notificationAfterInvalid.Pending == 0U,
            queue.GetState().PendingEntries == 0U, queue.GetState().ClaimedEntries == 0U,
            timer.GetSnapshot().State == TimerState::Disarmed);

        bool replacementRetired{false};
        const auto replacementObject = issuer.CreateObject(timer);
        if (!replacementObject.HasValue())
        {
            ASTERIA_EXPECT(runner, "retired Timer storage can be registered for ABA validation", false);
        }
        else
        {
            const TimerId replacementTimerId{registry, replacementObject.GetValue()};
            const auto replacementArm = queue.Arm(replacementTimerId, timer, MonotonicTime{30U});
            clock.Set(30U);
            const auto replacementClaim = queue.ClaimDue();
            const auto reusedStorageAttempt =
                liveClaim.HasValue()
                    ? delivery.DeliverClaimed(liveClaim.GetValue(), notificationId, notification)
                    : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
            const auto notificationAfterStorageReuse = notification.GetSnapshot();
            const auto replacementDelivery =
                replacementClaim.HasValue()
                    ? delivery.DeliverClaimed(replacementClaim.GetValue(), notificationId, notification)
                    : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
            const auto replacementNotificationClaim = notification.Claim();
            const bool replacementConsumed =
                replacementNotificationClaim.HasValue() &&
                notification.CommitClaim(replacementNotificationClaim.GetValue()).HasValue();
            replacementRetired = RetireObject(issuer, replacementObject.GetValue(), &timer);
            ASTERIA_EXPECT(runner,
                           "retired and re-registered Timer storage cannot revive an "
                           "old claim copy or publish before exact live validation",
                           replacementArm.HasValue(), replacementClaim.HasValue(), !reusedStorageAttempt.HasValue(),
                           reusedStorageAttempt.GetError().Error == TimerNotificationDeliveryError::InvalidClaim,
                           reusedStorageAttempt.GetError().Queue.Error == TimerQueueError::StaleClaim,
                           reusedStorageAttempt.GetError().NotificationPublished == 0U,
                           notificationAfterStorageReuse.State == NotificationState::Ready,
                           notificationAfterStorageReuse.Pending == 0U, replacementDelivery.HasValue(),
                           replacementDelivery.GetValue().ClaimSequence == 5U, replacementConsumed, replacementRetired);
        }
    }

    ASTERIA_EXPECT(runner, "claimed delivery releases every transient exact-generation pin", timerRetired,
                   RetireObject(issuer, notificationObject, &notification));
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TimerNotificationDeliveryTests"};
    Asteria::Test::TestHappyPathAndLifetime(runner);
    Asteria::Test::TestEarlyAndStaleClaimPaths(runner);
    Asteria::Test::TestExactIdentityValidation(runner);
    Asteria::Test::TestPinFailuresRetainExactLifetime(runner);
    Asteria::Test::TestClaimedDeliveryAndAdversarialValidation(runner);
    return runner.Finish();
}

#endif
