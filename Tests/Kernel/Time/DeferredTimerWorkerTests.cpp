// SPDX-License-Identifier: Apache-2.0
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
#include "Kernel/Time/DeferredTimerWorker.hpp"

extern "C" [[gnu::used, gnu::noinline]] Asteria::Foundation::UInt64
AsteriaDeferredTimerWorkerTargetFixture(Asteria::Kernel::Time::DeferredTimerWorker& worker) noexcept
{
    const auto outcome = worker.DrainDueRetained();
    if (outcome == Asteria::Kernel::Time::DeferredTimerDrainOutcome::Drained)
    {
        const auto& receipt = worker.GetRetainedDrainReceipt();
        return receipt.LastClaimSequence ^ receipt.DeliveredCount;
    }
    const auto& failure = worker.GetRetainedDrainFailure();
    return failure.RegistrationSequence ^ static_cast<Asteria::Foundation::UInt64>(failure.Error);
}

#else

#include <cstddef>
#include <cstdio>
#include <optional>

#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Time/DeferredTimerWorker.hpp"
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
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Time::Detail::ClassifyDeliveryRejection;
using Kernel::Time::DeferredTimerDrainStop;
using Kernel::Time::DeferredTimerDrainOutcome;
using Kernel::Time::DeferredTimerRegistrationToken;
using Kernel::Time::DeferredTimerWorker;
using Kernel::Time::DeferredTimerWorkerError;
using Kernel::Time::DeferredTimerWorkerSequencePolicy;
using Kernel::Time::DeferredTimerWorkerState;
using Kernel::Time::MaximumDeferredTimerBindings;
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

static_assert(MaximumDeferredTimerBindings == 32U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{9U};
        return !DeferredTimerWorkerSequencePolicy::TryAdvance(DeferredTimerWorkerSequencePolicy::Maximum, next) &&
               (next == 9U);
    }());

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

[[nodiscard]] bool ConsumeNotification(Notification& notification) noexcept
{
    const auto claim = notification.Claim();
    return claim.HasValue() && notification.CommitClaim(claim.GetValue()).HasValue();
}

void TestRegistrationValidationAndSharedNotification(TestContext& runner) noexcept
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
    Timer failureAtomicTimer{clock};
    Timer foreignTimer{clock};
    Notification sharedNotification;
    Notification secondNotification;
    Notification foreignNotification;
    const ObjectId firstTimerObject = RegisterTimer(firstTimer, issuer, 1U);
    const ObjectId secondTimerObject = RegisterTimer(secondTimer, issuer, 2U);
    const ObjectId failureAtomicTimerObject = RegisterTimer(failureAtomicTimer, issuer, 3U);
    const ObjectId foreignTimerObject = RegisterTimer(foreignTimer, foreignIssuer, 4U);
    const ObjectId sharedNotificationObject = RegisterNotification(sharedNotification, issuer, 5U);
    const ObjectId secondNotificationObject = RegisterNotification(secondNotification, issuer, 6U);
    const ObjectId foreignNotificationObject = RegisterNotification(foreignNotification, foreignIssuer, 7U);
    const TimerId firstTimerId{registry, firstTimerObject};
    const TimerId secondTimerId{registry, secondTimerObject};
    const TimerId failureAtomicTimerId{registry, failureAtomicTimerObject};
    const TimerId foreignTimerId{foreignRegistry, foreignTimerObject};
    const NotificationId sharedNotificationId{registry, sharedNotificationObject};
    const NotificationId secondNotificationId{registry, secondNotificationObject};
    const NotificationId foreignNotificationId{foreignRegistry, foreignNotificationObject};

    bool failureAtomicCollected{false};
    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        DeferredTimerWorker otherWorker{registry, queue};
        const auto invalidTimer = worker.Register({}, firstTimer, sharedNotificationId, sharedNotification);
        const auto foreignTimerResult =
            worker.Register(foreignTimerId, foreignTimer, sharedNotificationId, sharedNotification);
        const auto invalidNotification = worker.Register(firstTimerId, firstTimer, {}, sharedNotification);
        const auto foreignNotificationResult =
            worker.Register(firstTimerId, firstTimer, foreignNotificationId, foreignNotification);
        const auto timerStorageMismatch =
            worker.Register(firstTimerId, secondTimer, sharedNotificationId, sharedNotification);
        const auto notificationStorageMismatch =
            worker.Register(failureAtomicTimerId, failureAtomicTimer, sharedNotificationId, secondNotification);
        const NotificationId wrongTypeNotification{registry, secondTimerObject};
        const auto notificationTypeMismatch =
            worker.Register(failureAtomicTimerId, failureAtomicTimer, wrongTypeNotification, sharedNotification);
        const ObjectRegistryError failureAtomicDestroyed = issuer.DestroyObject(failureAtomicTimerObject);
        const auto failureAtomicCollection = issuer.CollectDestroyedObject();
        failureAtomicCollected =
            failureAtomicCollection.HasValue() && (failureAtomicCollection.GetValue().Instance == &failureAtomicTimer);

        const auto firstBinding = worker.Register(firstTimerId, firstTimer, sharedNotificationId, sharedNotification);
        const auto secondBinding =
            worker.Register(secondTimerId, secondTimer, sharedNotificationId, sharedNotification);
        const auto duplicate = worker.Register(firstTimerId, firstTimer, secondNotificationId, secondNotification);
        const auto snapshot = worker.GetSnapshot();
        ASTERIA_EXPECT(
            runner,
            "registration validates provenance and exact storage while every failed second pin is failure-atomic",
            !invalidTimer.HasValue(), invalidTimer.GetError().Error == DeferredTimerWorkerError::InvalidTimer,
            !foreignTimerResult.HasValue(),
            foreignTimerResult.GetError().Error == DeferredTimerWorkerError::TimerRegistryMismatch,
            !invalidNotification.HasValue(),
            invalidNotification.GetError().Error == DeferredTimerWorkerError::InvalidNotification,
            !foreignNotificationResult.HasValue(),
            foreignNotificationResult.GetError().Error == DeferredTimerWorkerError::NotificationRegistryMismatch,
            !timerStorageMismatch.HasValue(),
            timerStorageMismatch.GetError().Error == DeferredTimerWorkerError::TimerInstanceMismatch,
            !notificationStorageMismatch.HasValue(),
            notificationStorageMismatch.GetError().Error == DeferredTimerWorkerError::NotificationInstanceMismatch,
            notificationStorageMismatch.GetError().TimerPin == ObjectPinError::Ok,
            notificationStorageMismatch.GetError().NotificationPin == ObjectPinError::Ok,
            !notificationTypeMismatch.HasValue(),
            notificationTypeMismatch.GetError().Error == DeferredTimerWorkerError::NotificationPinRejected,
            notificationTypeMismatch.GetError().NotificationPin == ObjectPinError::TypeMismatch,
            failureAtomicDestroyed == ObjectRegistryError::Ok, failureAtomicCollected);
        ASTERIA_EXPECT(
            runner, "two Timers may share one exact Notification while duplicate Timer bindings are rejected",
            firstBinding.HasValue(), secondBinding.HasValue(), firstBinding.GetValue().Registration.GetSequence() == 1U,
            secondBinding.GetValue().Registration.GetSequence() == 2U, !duplicate.HasValue(),
            duplicate.GetError().Error == DeferredTimerWorkerError::DuplicateTimer,
            snapshot.State == DeferredTimerWorkerState::Ready, snapshot.RegisteredBindings == 2U,
            snapshot.LastRegistrationSequence == 2U, BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved)));

        if (firstBinding.HasValue() && secondBinding.HasValue())
        {
            const auto firstRemoved = worker.Unregister(firstBinding.GetValue().Registration);
            const auto stale = worker.Unregister(firstBinding.GetValue().Registration);
            const auto foreignToken = otherWorker.Unregister(secondBinding.GetValue().Registration);
            const auto secondRemoved = worker.Unregister(secondBinding.GetValue().Registration);
            ASTERIA_EXPECT(runner, "unregistration accepts only the exact live worker token and makes copies stale",
                           firstRemoved.HasValue(), !stale.HasValue(),
                           stale.GetError().Error == DeferredTimerWorkerError::StaleRegistration,
                           !foreignToken.HasValue(),
                           foreignToken.GetError().Error == DeferredTimerWorkerError::InvalidRegistration,
                           secondRemoved.HasValue(), secondRemoved.GetValue().RegisteredBindings == 0U);
        }
    }

    ASTERIA_EXPECT(runner, "registration validation retains no rejected generation", failureAtomicCollected,
                   RetireObject(issuer, firstTimerObject, &firstTimer),
                   RetireObject(issuer, secondTimerObject, &secondTimer),
                   RetireObject(issuer, sharedNotificationObject, &sharedNotification),
                   RetireObject(issuer, secondNotificationObject, &secondNotification),
                   RetireObject(foreignIssuer, foreignTimerObject, &foreignTimer),
                   RetireObject(foreignIssuer, foreignNotificationObject, &foreignNotification));
}

void TestUnregistrationRequiresDisarmedTimer(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 11U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 12U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto binding = worker.Register(timerId, timer, notificationId, notification);
        const auto armed = queue.Arm(timerId, timer, MonotonicTime{20U});
        const auto managedRemoval =
            binding.HasValue() ? worker.Unregister(binding.GetValue().Registration) : worker.Unregister({});
        const auto cancelled = armed.HasValue() ? queue.Cancel(timerId, timer, armed.GetValue().Timer.Token)
                                                : queue.Cancel(timerId, timer, {});
        const auto removed =
            binding.HasValue() ? worker.Unregister(binding.GetValue().Registration) : worker.Unregister({});
        const auto rebound = worker.Register(timerId, timer, notificationId, notification);
        const auto oldToken =
            binding.HasValue() ? worker.Unregister(binding.GetValue().Registration) : worker.Unregister({});
        const auto finalRemoval =
            rebound.HasValue() ? worker.Unregister(rebound.GetValue().Registration) : worker.Unregister({});
        ASTERIA_EXPECT(
            runner, "unregistration requires the exact Timer to be safely Disarmed and never reuses a sequence",
            binding.HasValue(), armed.HasValue(), !managedRemoval.HasValue(),
            managedRemoval.GetError().Error == DeferredTimerWorkerError::TimerNotDisarmed,
            managedRemoval.GetError().Timer == TimerState::Armed, cancelled.HasValue(), removed.HasValue(),
            rebound.HasValue(), rebound.GetValue().Registration.GetSequence() == 2U, !oldToken.HasValue(),
            oldToken.GetError().Error == DeferredTimerWorkerError::StaleRegistration, finalRemoval.HasValue());
    }

    ASTERIA_EXPECT(runner, "unregistration releases both binding pins", RetireObject(issuer, timerObject, &timer),
                   RetireObject(issuer, notificationObject, &notification));
}

void TestOrderingNotDueAndRepeatedDrain(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer firstTimer{clock};
    Timer secondTimer{clock};
    Notification firstNotification;
    Notification secondNotification;
    const ObjectId firstTimerObject = RegisterTimer(firstTimer, issuer, 21U);
    const ObjectId secondTimerObject = RegisterTimer(secondTimer, issuer, 22U);
    const ObjectId firstNotificationObject = RegisterNotification(firstNotification, issuer, 23U);
    const ObjectId secondNotificationObject = RegisterNotification(secondNotification, issuer, 24U);
    const TimerId firstTimerId{registry, firstTimerObject};
    const TimerId secondTimerId{registry, secondTimerObject};
    const NotificationId firstNotificationId{registry, firstNotificationObject};
    const NotificationId secondNotificationId{registry, secondNotificationObject};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto secondBinding =
            worker.Register(secondTimerId, secondTimer, secondNotificationId, secondNotification);
        const auto firstBinding = worker.Register(firstTimerId, firstTimer, firstNotificationId, firstNotification);
        const auto firstArm = queue.Arm(firstTimerId, firstTimer, MonotonicTime{10U});
        const auto secondArm = queue.Arm(secondTimerId, secondTimer, MonotonicTime{10U});
        clock.Set(9U);
        const auto early = worker.DrainDue();
        clock.Set(10U);
        const auto drained = worker.DrainDue();
        const auto lastDelivery = worker.GetLastDeliveryReceipt();
        const auto repeated = worker.DrainDue();
        const bool firstConsumed = ConsumeNotification(firstNotification);
        const bool secondConsumed = ConsumeNotification(secondNotification);
        ASTERIA_EXPECT(
            runner,
            "DrainDue preserves equal-deadline queue order, reports NotDue, "
            "and resets per-drain evidence on repetition",
            secondBinding.HasValue(), firstBinding.HasValue(), firstArm.HasValue(), secondArm.HasValue(),
            early.HasValue(), early.GetValue().Stop == DeferredTimerDrainStop::NotDue,
            early.GetValue().DeliveredCount == 0U, early.GetValue().QueueState.NextDeadlineTicks == 10U,
            drained.HasValue(), drained.GetValue().Stop == DeferredTimerDrainStop::Empty,
            drained.GetValue().DeliveredCount == 2U, drained.GetValue().FirstTimerIdentity.IsEqualTo(firstTimerId),
            drained.GetValue().LastTimerIdentity.IsEqualTo(secondTimerId), drained.GetValue().FirstClaimSequence == 1U,
            drained.GetValue().LastClaimSequence == 2U, lastDelivery.TimerIdentity.IsEqualTo(secondTimerId),
            lastDelivery.ClaimSequence == 2U, worker.GetLastDeliveryReceipt().ClaimSequence == 0U, repeated.HasValue(),
            repeated.GetValue().Stop == DeferredTimerDrainStop::Empty, repeated.GetValue().DeliveredCount == 0U,
            !repeated.GetValue().FirstTimerIdentity.IsValid(), firstConsumed, secondConsumed);

        if (firstBinding.HasValue())
        {
            static_cast<void>(worker.Unregister(firstBinding.GetValue().Registration));
        }
        if (secondBinding.HasValue())
        {
            static_cast<void>(worker.Unregister(secondBinding.GetValue().Registration));
        }
    }

    ASTERIA_EXPECT(runner, "ordered repeated drain leaves no retained objects",
                   RetireObject(issuer, firstTimerObject, &firstTimer),
                   RetireObject(issuer, secondTimerObject, &secondTimer),
                   RetireObject(issuer, firstNotificationObject, &firstNotification),
                   RetireObject(issuer, secondNotificationObject, &secondNotification));
}

void TestSharedNotificationCoalescing(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer firstTimer{clock};
    Timer secondTimer{clock};
    Notification notification;
    const ObjectId firstTimerObject = RegisterTimer(firstTimer, issuer, 31U);
    const ObjectId secondTimerObject = RegisterTimer(secondTimer, issuer, 32U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 33U);
    const TimerId firstTimerId{registry, firstTimerObject};
    const TimerId secondTimerId{registry, secondTimerObject};
    const NotificationId notificationId{registry, notificationObject};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto firstBinding = worker.Register(firstTimerId, firstTimer, notificationId, notification);
        const auto secondBinding = worker.Register(secondTimerId, secondTimer, notificationId, notification);
        const auto firstArm = queue.Arm(firstTimerId, firstTimer, MonotonicTime{5U});
        const auto secondArm = queue.Arm(secondTimerId, secondTimer, MonotonicTime{6U});
        clock.Set(6U);
        const auto drained = worker.DrainDue();
        const bool consumed = ConsumeNotification(notification);
        const auto coalesced = notification.Claim();
        ASTERIA_EXPECT(
            runner, "two Timer deliveries to one shared Notification coalesce into one consumable readiness bit",
            firstBinding.HasValue(), secondBinding.HasValue(), firstArm.HasValue(), secondArm.HasValue(),
            drained.HasValue(), drained.GetValue().DeliveredCount == 2U, notification.GetSnapshot().Pending == 0U,
            consumed, !coalesced.HasValue(), coalesced.GetError().Error == NotificationError::WouldBlock);

        if (firstBinding.HasValue())
        {
            static_cast<void>(worker.Unregister(firstBinding.GetValue().Registration));
        }
        if (secondBinding.HasValue())
        {
            static_cast<void>(worker.Unregister(secondBinding.GetValue().Registration));
        }
    }

    ASTERIA_EXPECT(runner, "shared Notification bindings release independent pins",
                   RetireObject(issuer, firstTimerObject, &firstTimer),
                   RetireObject(issuer, secondTimerObject, &secondTimer),
                   RetireObject(issuer, notificationObject, &notification));
}

void TestCapacityAndDrainBound(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    std::optional<Timer> timers[MaximumDeferredTimerBindings + 1U];
    ObjectId timerObjects[MaximumDeferredTimerBindings + 1U]{};
    DeferredTimerRegistrationToken registrations[MaximumDeferredTimerBindings]{};
    Notification notification;
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 41U);
    const NotificationId notificationId{registry, notificationObject};
    bool setupSucceeded{notificationObject.IsValid()};
    for (Foundation::Size index{0U}; index < (MaximumDeferredTimerBindings + 1U); ++index)
    {
        timers[index].emplace(clock);
        timerObjects[index] = RegisterTimer(*timers[index], issuer, 42U + index);
        setupSucceeded = setupSucceeded && timerObjects[index].IsValid();
    }

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        for (Foundation::Size index{0U}; index < MaximumDeferredTimerBindings; ++index)
        {
            const auto binding =
                worker.Register(TimerId{registry, timerObjects[index]}, *timers[index], notificationId, notification);
            if (binding.HasValue())
            {
                registrations[index] = binding.GetValue().Registration;
            }
            else
            {
                setupSucceeded = false;
            }
        }
        const auto overflow = worker.Register(TimerId{registry, timerObjects[MaximumDeferredTimerBindings]},
                                              *timers[MaximumDeferredTimerBindings], notificationId, notification);
        for (Foundation::Size index{0U}; index < MaximumDeferredTimerBindings; ++index)
        {
            const auto armed = queue.Arm(TimerId{registry, timerObjects[index]}, *timers[index], MonotonicTime{100U});
            setupSucceeded = setupSucceeded && armed.HasValue();
        }
        clock.Set(100U);
        const auto drained = worker.DrainDue();
        const auto repeated = worker.DrainDue();
        const bool consumed = ConsumeNotification(notification);
        ASTERIA_EXPECT(
            runner, "capacity is exactly 32 and one DrainDue call performs no more than 32 successful deliveries",
            setupSucceeded, !overflow.HasValue(),
            overflow.GetError().Error == DeferredTimerWorkerError::BindingCapacityExceeded,
            overflow.GetError().TimerPin == ObjectPinError::Ok,
            overflow.GetError().NotificationPin == ObjectPinError::Ok, drained.HasValue(),
            drained.GetValue().Stop == DeferredTimerDrainStop::WorkLimit,
            drained.GetValue().DeliveredCount == MaximumDeferredTimerBindings,
            drained.GetValue().FirstClaimSequence == 1U,
            drained.GetValue().LastClaimSequence == MaximumDeferredTimerBindings,
            drained.GetValue().QueueState.ScheduledEntries == 0U, drained.GetValue().QueueState.PendingEntries == 0U,
            drained.GetValue().QueueState.ClaimedEntries == 0U, repeated.HasValue(),
            repeated.GetValue().Stop == DeferredTimerDrainStop::Empty, repeated.GetValue().DeliveredCount == 0U,
            consumed);

        for (Foundation::Size index{0U}; index < MaximumDeferredTimerBindings; ++index)
        {
            if (registrations[index].IsValid())
            {
                const auto removed = worker.Unregister(registrations[index]);
                setupSucceeded = setupSucceeded && removed.HasValue();
            }
        }
        ASTERIA_EXPECT(runner, "all capacity-bound registrations remain independently removable", setupSucceeded,
                       worker.GetSnapshot().RegisteredBindings == 0U);
    }

    bool retired = true;
    for (Foundation::Size index{0U}; index < (MaximumDeferredTimerBindings + 1U); ++index)
    {
        retired = RetireObject(issuer, timerObjects[index], &*timers[index]) && retired;
    }
    ASTERIA_EXPECT(runner, "capacity and drain-bound test releases every retained generation", retired,
                   RetireObject(issuer, notificationObject, &notification));
}

void TestMissingBindingRecovery(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 81U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 82U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto armed = queue.Arm(timerId, timer, MonotonicTime{10U});
        clock.Set(10U);
        const auto missing = worker.DrainDue();
        const auto binding = worker.Register(timerId, timer, notificationId, notification);
        const auto retried = worker.DrainDue();
        const bool consumed = ConsumeNotification(notification);
        ASTERIA_EXPECT(
            runner, "a missing binding restores Pending once and a later exact binding makes repeated drain succeed",
            armed.HasValue(), !missing.HasValue(),
            missing.GetError().Error == DeferredTimerWorkerError::BindingNotFound,
            missing.GetError().ClaimRestored == 1U, missing.GetError().State == DeferredTimerWorkerState::Ready,
            missing.GetError().QueueState.PendingEntries == 1U, timer.GetSnapshot().State == TimerState::Disarmed,
            binding.HasValue(), retried.HasValue(), retried.GetValue().DeliveredCount == 1U,
            retried.GetValue().LastClaimSequence == 2U, consumed);
        if (binding.HasValue())
        {
            static_cast<void>(worker.Unregister(binding.GetValue().Registration));
        }
    }

    ASTERIA_EXPECT(runner, "missing-binding recovery releases the queue and binding pins",
                   RetireObject(issuer, timerObject, &timer), RetireObject(issuer, notificationObject, &notification));
}

void TestRetainedDrainFailureLifetime(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer missingTimer{clock};
    Timer operationTimer{clock};
    Notification notification;
    const ObjectId missingTimerObject = RegisterTimer(missingTimer, issuer, 83U);
    const ObjectId operationTimerObject = RegisterTimer(operationTimer, issuer, 84U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 85U);
    const TimerId missingTimerId{registry, missingTimerObject};
    const TimerId operationTimerId{registry, operationTimerObject};
    const NotificationId notificationId{registry, notificationObject};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto armed = queue.Arm(missingTimerId, missingTimer, MonotonicTime{10U});
        clock.Set(10U);
        const auto firstOutcome = worker.DrainDueRetained();
        const auto& retained = worker.GetRetainedDrainFailure();
        const Foundation::UInt64 firstNextClaimSequence = retained.QueueState.NextClaimSequence;
        const auto binding = worker.Register(operationTimerId, operationTimer, notificationId, notification);
        const bool retainedAcrossSuccessfulRegister = (retained.Error == DeferredTimerWorkerError::BindingNotFound) &&
                                                      retained.TimerIdentity.IsEqualTo(missingTimerId) &&
                                                      (retained.ClaimRestored == 1U) &&
                                                      (retained.QueueState.NextClaimSequence == firstNextClaimSequence);
        const auto duplicate = worker.Register(operationTimerId, operationTimer, notificationId, notification);
        const bool retainedAcrossFailedRegister = (retained.Error == DeferredTimerWorkerError::BindingNotFound) &&
                                                  (retained.QueueState.NextClaimSequence == firstNextClaimSequence);
        const auto removed = binding.HasValue() ? worker.Unregister(binding.GetValue().Registration)
                                                : worker.Unregister(DeferredTimerRegistrationToken{});
        const bool retainedAcrossSuccessfulUnregister =
            (retained.Error == DeferredTimerWorkerError::BindingNotFound) &&
            (retained.QueueState.NextClaimSequence == firstNextClaimSequence);
        const auto stale = binding.HasValue() ? worker.Unregister(binding.GetValue().Registration)
                                              : worker.Unregister(DeferredTimerRegistrationToken{});
        const bool retainedAcrossFailedUnregister = (retained.Error == DeferredTimerWorkerError::BindingNotFound) &&
                                                    (retained.QueueState.NextClaimSequence == firstNextClaimSequence);
        const auto secondOutcome = worker.DrainDueRetained();

        ASTERIA_EXPECT(runner,
                       "successful and failed registration operations preserve a "
                       "retained drain failure through successful and failed removal",
                       armed.HasValue(), firstOutcome == DeferredTimerDrainOutcome::Rejected,
                       firstNextClaimSequence == 2U, binding.HasValue(), retainedAcrossSuccessfulRegister,
                       !duplicate.HasValue(), duplicate.GetError().Error == DeferredTimerWorkerError::DuplicateTimer,
                       retainedAcrossFailedRegister, removed.HasValue(), retainedAcrossSuccessfulUnregister,
                       !stale.HasValue(), stale.GetError().Error == DeferredTimerWorkerError::StaleRegistration,
                       retainedAcrossFailedUnregister);
        ASTERIA_EXPECT(runner,
                       "the next drain replaces retained failure evidence with its own "
                       "diagnostic",
                       secondOutcome == DeferredTimerDrainOutcome::Rejected,
                       retained.Error == DeferredTimerWorkerError::BindingNotFound,
                       retained.TimerIdentity.IsEqualTo(missingTimerId), retained.ClaimRestored == 1U,
                       retained.QueueState.NextClaimSequence == 3U);

        const auto recoveryBinding = worker.Register(missingTimerId, missingTimer, notificationId, notification);
        const auto recovered = worker.DrainDue();
        if (recoveryBinding.HasValue())
        {
            static_cast<void>(worker.Unregister(recoveryBinding.GetValue().Registration));
        }
        ASTERIA_EXPECT(runner,
                       "retained-failure lifetime checks leave missing-binding recovery "
                       "operational",
                       recoveryBinding.HasValue(), recovered.HasValue(), recovered.GetValue().DeliveredCount == 1U,
                       ConsumeNotification(notification));
    }

    ASTERIA_EXPECT(runner, "retained-failure lifetime test releases all exact-generation pins",
                   RetireObject(issuer, missingTimerObject, &missingTimer),
                   RetireObject(issuer, operationTimerObject, &operationTimer),
                   RetireObject(issuer, notificationObject, &notification));
}

// An expiration that can never be delivered is retired: the Notification of
// its binding, or the Timer itself (bound or not), was destroyed while the
// Timer was queued. The drain consumes each one, continues with the Timer
// queued behind them, and every destroyed generation becomes collectable once
// its binding is removed.
void TestUndeliverableExpirationsRetire(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer orphanedTimer{clock};
    Timer destroyedTimer{clock};
    Timer unboundTimer{clock};
    Timer liveTimer{clock};
    Notification destroyedNotification;
    Notification notification;
    const ObjectId orphanedTimerObject = RegisterTimer(orphanedTimer, issuer, 91U);
    const ObjectId destroyedTimerObject = RegisterTimer(destroyedTimer, issuer, 92U);
    const ObjectId unboundTimerObject = RegisterTimer(unboundTimer, issuer, 93U);
    const ObjectId liveTimerObject = RegisterTimer(liveTimer, issuer, 94U);
    const ObjectId destroyedNotificationObject = RegisterNotification(destroyedNotification, issuer, 95U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 96U);
    const TimerId orphanedTimerId{registry, orphanedTimerObject};
    const TimerId destroyedTimerId{registry, destroyedTimerObject};
    const TimerId unboundTimerId{registry, unboundTimerObject};
    const TimerId liveTimerId{registry, liveTimerObject};
    const NotificationId destroyedNotificationId{registry, destroyedNotificationObject};
    const NotificationId notificationId{registry, notificationObject};
    bool unbound{false};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto orphanedBinding =
            worker.Register(orphanedTimerId, orphanedTimer, destroyedNotificationId, destroyedNotification);
        const auto destroyedBinding = worker.Register(destroyedTimerId, destroyedTimer, notificationId, notification);
        const auto liveBinding = worker.Register(liveTimerId, liveTimer, notificationId, notification);
        const bool armed = queue.Arm(orphanedTimerId, orphanedTimer, MonotonicTime{5U}).HasValue() &&
                           queue.Arm(destroyedTimerId, destroyedTimer, MonotonicTime{6U}).HasValue() &&
                           queue.Arm(unboundTimerId, unboundTimer, MonotonicTime{7U}).HasValue() &&
                           queue.Arm(liveTimerId, liveTimer, MonotonicTime{8U}).HasValue();
        const bool destroyed = (issuer.DestroyObject(destroyedNotificationObject) == ObjectRegistryError::Ok) &&
                               (issuer.DestroyObject(destroyedTimerObject) == ObjectRegistryError::Ok) &&
                               (issuer.DestroyObject(unboundTimerObject) == ObjectRegistryError::Ok);
        clock.Set(8U);
        const auto drained = worker.DrainDue();
        const auto released = issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(runner,
                       "undeliverable expirations are retired and the drain delivers "
                       "the Timer queued behind them",
                       orphanedBinding.HasValue(), destroyedBinding.HasValue(), liveBinding.HasValue(), armed,
                       destroyed, drained.HasValue(), drained.GetValue().Stop == DeferredTimerDrainStop::Empty,
                       drained.GetValue().DeliveredCount == 1U, drained.GetValue().RetiredCount == 3U,
                       BytesAreZero(drained.GetValue().Reserved, sizeof(drained.GetValue().Reserved)),
                       drained.GetValue().FirstTimerIdentity.IsEqualTo(liveTimerId),
                       drained.GetValue().QueueState.PendingEntries == 0U,
                       drained.GetValue().QueueState.ClaimedEntries == 0U,
                       drained.GetValue().State == DeferredTimerWorkerState::Ready,
                       worker.GetLastDeliveryFailure().Error == TimerNotificationDeliveryError::TimerPinRejected,
                       worker.GetLastDeliveryFailure().ClaimRetired == 1U,
                       orphanedTimer.GetSnapshot().State == TimerState::Disarmed,
                       destroyedTimer.GetSnapshot().State == TimerState::Disarmed,
                       unboundTimer.GetSnapshot().State == TimerState::Disarmed, released.HasValue(),
                       released.GetValue().Instance == &unboundTimer, ConsumeNotification(notification));
        unbound = orphanedBinding.HasValue() && destroyedBinding.HasValue() && liveBinding.HasValue() &&
                  worker.Unregister(orphanedBinding.GetValue().Registration).HasValue() &&
                  worker.Unregister(destroyedBinding.GetValue().Registration).HasValue() &&
                  worker.Unregister(liveBinding.GetValue().Registration).HasValue();
    }

    const auto first = issuer.CollectDestroyedObject();
    const auto second = issuer.CollectDestroyedObject();
    const void* const firstInstance = first.HasValue() ? first.GetValue().Instance : nullptr;
    const void* const secondInstance = second.HasValue() ? second.GetValue().Instance : nullptr;
    ASTERIA_EXPECT(runner,
                   "removing the bindings of retired expirations releases the "
                   "destroyed generations",
                   unbound,
                   ((firstInstance == &destroyedTimer) && (secondInstance == &destroyedNotification)) ||
                       ((firstInstance == &destroyedNotification) && (secondInstance == &destroyedTimer)),
                   RetireObject(issuer, orphanedTimerObject, &orphanedTimer),
                   RetireObject(issuer, liveTimerObject, &liveTimer),
                   RetireObject(issuer, notificationObject, &notification));
}

// A pin rejection that says nothing about an object's lifetime, here a
// saturated reference count, restores the claim, and a later drain delivers
// the same expiration.
void TestTransientPinFailureRetries(TestContext& runner) noexcept
{
    SpinLock registryLock;
    // The binding and the queue pin the Timer and delivery pins it a third
    // time; two extra Notification pins leave delivery none for the
    // Notification until one is released.
    Asteria::TestSupport::TestObjectRegistry registry{registryLock, 3U};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 97U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 98U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};
    ObjectPin firstExtraPin{};
    ObjectPin secondExtraPin{};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto binding = worker.Register(timerId, timer, notificationId, notification);
        const bool saturated =
            (registry.PinObject(notificationObject, ObjectType::Notification, firstExtraPin) == ObjectPinError::Ok) &&
            (registry.PinObject(notificationObject, ObjectType::Notification, secondExtraPin) == ObjectPinError::Ok);
        const auto armed = queue.Arm(timerId, timer, MonotonicTime{4U});
        clock.Set(4U);
        const auto rejected = worker.DrainDue();
        secondExtraPin.Reset();
        const auto retried = worker.DrainDue();
        ASTERIA_EXPECT(runner,
                       "a saturated Notification reference count restores the claim "
                       "and a later drain delivers it",
                       binding.HasValue(), saturated, armed.HasValue(), !rejected.HasValue(),
                       rejected.GetError().Error == DeferredTimerWorkerError::DeliveryRejected,
                       rejected.GetError().NotificationPin == ObjectPinError::ReferenceExhausted,
                       rejected.GetError().ClaimRestored == 1U, rejected.GetError().QueueState.PendingEntries == 1U,
                       retried.HasValue(), retried.GetValue().DeliveredCount == 1U,
                       retried.GetValue().QueueState.PendingEntries == 0U, ConsumeNotification(notification));
        firstExtraPin.Reset();
        if (binding.HasValue())
        {
            static_cast<void>(worker.Unregister(binding.GetValue().Registration));
        }
    }

    ASTERIA_EXPECT(runner, "the retried delivery leaves no retained generation",
                   RetireObject(issuer, timerObject, &timer), RetireObject(issuer, notificationObject, &notification));
}

void TestPostpublicationClassification(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 101U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 102U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};

    {
        TimerQueue queue{registry, clock};
        TimerNotificationDelivery delivery{registry, queue};
        const auto armed = queue.Arm(timerId, timer, MonotonicTime{9U});
        clock.Set(9U);
        const auto prepublicationClaim = queue.ClaimDue();
        const auto prepublication = prepublicationClaim.HasValue()
                                        ? delivery.DeliverClaimed(prepublicationClaim.GetValue(), {}, notification)
                                        : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, {}, notification);
        const auto retryClaim = queue.ClaimDue();
        const auto delivered = retryClaim.HasValue()
                                   ? delivery.DeliverClaimed(retryClaim.GetValue(), notificationId, notification)
                                   : delivery.DeliverClaimed(TimerQueueExpirationClaim{}, notificationId, notification);
        TimerNotificationDeliveryFailure publicationFailure{};
        publicationFailure.Error = TimerNotificationDeliveryError::ClaimCommitRejected;
        publicationFailure.NotificationPublished = 1U;
        TimerNotificationDeliveryFailure committedFailure{};
        committedFailure.Error = TimerNotificationDeliveryError::TimerAcknowledgeRejected;
        committedFailure.QueueCommitted = 1U;
        TimerNotificationDeliveryFailure cancellationFailure{};
        cancellationFailure.Error = TimerNotificationDeliveryError::ClaimCancellationRejected;
        ASTERIA_EXPECT(runner,
                       "the worker policy distinguishes retryable pre-publication failure "
                       "from irreversible publication and restoration contradictions",
                       armed.HasValue(), !prepublication.HasValue(),
                       !DeferredTimerWorker::IsTerminalDeliveryFailure(prepublication.GetErrorReference()),
                       DeferredTimerWorker::IsTerminalDeliveryFailure(publicationFailure),
                       DeferredTimerWorker::IsTerminalDeliveryFailure(committedFailure),
                       DeferredTimerWorker::IsTerminalDeliveryFailure(cancellationFailure), retryClaim.HasValue(),
                       delivered.HasValue(), ConsumeNotification(notification));
    }

    ASTERIA_EXPECT(runner, "post-publication classification test resolves the retained live claim",
                   RetireObject(issuer, timerObject, &timer), RetireObject(issuer, notificationObject, &notification));
}

void TestWorkerTerminalTransition(TestContext& runner) noexcept
{
    TimerNotificationDeliveryFailure publicationFailure{};
    publicationFailure.Error = TimerNotificationDeliveryError::ClaimCommitRejected;
    publicationFailure.NotificationPublished = 1U;
    TimerNotificationDeliveryFailure committedFailure{};
    committedFailure.Error = TimerNotificationDeliveryError::TimerAcknowledgeRejected;
    committedFailure.QueueCommitted = 1U;
    TimerNotificationDeliveryFailure cancellationFailure{};
    cancellationFailure.Error = TimerNotificationDeliveryError::ClaimCancellationRejected;
    TimerNotificationDeliveryFailure prepublicationFailure{};
    prepublicationFailure.Error = TimerNotificationDeliveryError::NotificationPinRejected;
    const auto publication = ClassifyDeliveryRejection(publicationFailure);
    const auto committed = ClassifyDeliveryRejection(committedFailure);
    const auto cancellation = ClassifyDeliveryRejection(cancellationFailure);
    const auto prepublication = ClassifyDeliveryRejection(prepublicationFailure);
    ASTERIA_EXPECT(runner,
                   "a post-publication contradiction or failed claim restoration faults "
                   "the worker while a pre-publication rejection remains retryable",
                   publication.State == DeferredTimerWorkerState::Faulted,
                   publication.Error == DeferredTimerWorkerError::DeliveryTerminal, publication.ClaimRestored == 0U,
                   committed.State == DeferredTimerWorkerState::Faulted,
                   committed.Error == DeferredTimerWorkerError::DeliveryTerminal,
                   cancellation.State == DeferredTimerWorkerState::Faulted,
                   cancellation.Error == DeferredTimerWorkerError::DeliveryTerminal, cancellation.ClaimRestored == 0U,
                   prepublication.State == DeferredTimerWorkerState::Ready,
                   prepublication.Error == DeferredTimerWorkerError::DeliveryRejected,
                   prepublication.ClaimRestored == 1U);

    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    Timer secondTimer{clock};
    Timer exhaustedTimer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 103U);
    const ObjectId secondTimerObject = RegisterTimer(secondTimer, issuer, 104U);
    const ObjectId exhaustedTimerObject = RegisterTimer(exhaustedTimer, issuer, 106U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 105U);
    const TimerId timerId{registry, timerObject};
    const TimerId secondTimerId{registry, secondTimerObject};
    const TimerId exhaustedTimerId{registry, exhaustedTimerObject};
    const NotificationId notificationId{registry, notificationObject};

    {
        TimerQueue queue{registry, clock};
        DeferredTimerWorker worker{registry, queue};
        const auto binding = worker.Register(timerId, timer, notificationId, notification);
        const auto armed = queue.Arm(exhaustedTimerId, exhaustedTimer, MonotonicTime{20U});

        // An exhausted clock makes the queue reject the earliest claim, which
        // is the worker's terminal QueueRejected contradiction.
        clock.Set(MonotonicTime::Infinite().GetTicks());
        const auto rejectedClaim = worker.DrainDue();
        const auto faultedDrain = worker.DrainDue();
        const auto faultedRegistration = worker.Register(secondTimerId, secondTimer, notificationId, notification);
        const auto safeUnregister = binding.HasValue() ? worker.Unregister(binding.GetValue().Registration)
                                                       : worker.Unregister(DeferredTimerRegistrationToken{});
        const auto cancelled = armed.HasValue()
                                   ? queue.Cancel(exhaustedTimerId, exhaustedTimer, armed.GetValue().Timer.Token)
                                   : queue.Cancel(exhaustedTimerId, exhaustedTimer, {});
        ASTERIA_EXPECT(runner,
                       "a terminal queue contradiction faults the real worker "
                       "transition, blocks drain and registration, and preserves safe "
                       "unregistration",
                       binding.HasValue(), armed.HasValue(), !rejectedClaim.HasValue(),
                       rejectedClaim.GetError().Error == DeferredTimerWorkerError::QueueRejected,
                       rejectedClaim.GetError().Queue.Error == TimerQueueError::TimerRejected,
                       rejectedClaim.GetError().State == DeferredTimerWorkerState::Faulted, !faultedDrain.HasValue(),
                       faultedDrain.GetError().Error == DeferredTimerWorkerError::Faulted,
                       !faultedRegistration.HasValue(),
                       faultedRegistration.GetError().Error == DeferredTimerWorkerError::Faulted,
                       safeUnregister.HasValue(), safeUnregister.GetValue().State == DeferredTimerWorkerState::Faulted,
                       safeUnregister.GetValue().RegisteredBindings == 0U, cancelled.HasValue());
    }

    ASTERIA_EXPECT(runner, "the terminal transition test releases all exact-generation pins",
                   RetireObject(issuer, timerObject, &timer), RetireObject(issuer, secondTimerObject, &secondTimer),
                   RetireObject(issuer, exhaustedTimerObject, &exhaustedTimer),
                   RetireObject(issuer, notificationObject, &notification));
}

void TestDestructorRetainsExactPins(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualClock clock{0U};
    Timer timer{clock};
    Notification notification;
    const ObjectId timerObject = RegisterTimer(timer, issuer, 111U);
    const ObjectId notificationObject = RegisterNotification(notification, issuer, 112U);
    const TimerId timerId{registry, timerObject};
    const NotificationId notificationId{registry, notificationObject};
    bool collectedEarly{false};

    {
        TimerQueue queue{registry, clock};
        {
            DeferredTimerWorker worker{registry, queue};
            const auto binding = worker.Register(timerId, timer, notificationId, notification);
            const ObjectRegistryError timerDestroyed = issuer.DestroyObject(timerObject);
            const ObjectRegistryError notificationDestroyed = issuer.DestroyObject(notificationObject);
            collectedEarly = issuer.CollectDestroyedObject().HasValue();
            ASTERIA_EXPECT(runner, "a live binding retains both exact quiescing generations", binding.HasValue(),
                           timerDestroyed == ObjectRegistryError::Ok, notificationDestroyed == ObjectRegistryError::Ok,
                           !collectedEarly);
        }
    }

    const auto firstCollection = issuer.CollectDestroyedObject();
    const auto secondCollection = issuer.CollectDestroyedObject();
    const bool timerCollected = (firstCollection.HasValue() && (firstCollection.GetValue().Instance == &timer)) ||
                                (secondCollection.HasValue() && (secondCollection.GetValue().Instance == &timer));
    const bool notificationCollected =
        (firstCollection.HasValue() && (firstCollection.GetValue().Instance == &notification)) ||
        (secondCollection.HasValue() && (secondCollection.GetValue().Instance == &notification));
    ASTERIA_EXPECT(runner, "worker destruction releases both retained exact-generation pins", !collectedEarly,
                   timerCollected, notificationCollected);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DeferredTimerWorkerTests"};
    Asteria::Test::TestRegistrationValidationAndSharedNotification(runner);
    Asteria::Test::TestUnregistrationRequiresDisarmedTimer(runner);
    Asteria::Test::TestOrderingNotDueAndRepeatedDrain(runner);
    Asteria::Test::TestSharedNotificationCoalescing(runner);
    Asteria::Test::TestCapacityAndDrainBound(runner);
    Asteria::Test::TestMissingBindingRecovery(runner);
    Asteria::Test::TestRetainedDrainFailureLifetime(runner);
    Asteria::Test::TestUndeliverableExpirationsRetire(runner);
    Asteria::Test::TestTransientPinFailureRetries(runner);
    Asteria::Test::TestPostpublicationClassification(runner);
    Asteria::Test::TestWorkerTerminalTransition(runner);
    Asteria::Test::TestDestructorRetainsExactPins(runner);
    return runner.Finish();
}

#endif
