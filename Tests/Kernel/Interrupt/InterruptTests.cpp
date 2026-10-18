// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Interrupt/Interrupt.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Hal::Interrupt::IInterruptSource;
using Hal::Interrupt::InterruptSourceError;
using Kernel::Interrupt::Interrupt;
using Kernel::Interrupt::InterruptCompletion;
using Kernel::Interrupt::InterruptDelivery;
using Kernel::Interrupt::InterruptError;
using Kernel::Interrupt::InterruptEventSequencePolicy;
using Kernel::Interrupt::InterruptEventToken;
using Kernel::Interrupt::InterruptFailure;
using Kernel::Interrupt::InterruptSnapshot;
using Kernel::Interrupt::InterruptState;
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationError;
using Kernel::Ipc::NotificationId;
using Kernel::Ipc::WaitSet;
using Kernel::Ipc::WaitSetError;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;

class MockInterruptSource final : public IInterruptSource
{
public:
    [[nodiscard]] InterruptSourceError Claim() noexcept override
    {
        ++ClaimCalls;
        const InterruptSourceError injected = Consume(NextClaim);
        if (injected != InterruptSourceError::None)
        {
            return injected;
        }
        if (Failed || !Ready)
        {
            return Failed ? InterruptSourceError::HardwareFailure : InterruptSourceError::NotReady;
        }
        if (Claimed)
        {
            return InterruptSourceError::AlreadyClaimed;
        }
        Claimed = true;
        Masked = true;
        return InterruptSourceError::None;
    }

    [[nodiscard]] InterruptSourceError Release() noexcept override
    {
        ++ReleaseCalls;
        const InterruptSourceError injected = Consume(NextRelease);
        if (injected != InterruptSourceError::None)
        {
            MarkFailure(injected);
            return injected;
        }
        if (Failed)
        {
            return InterruptSourceError::HardwareFailure;
        }
        if (!Claimed)
        {
            return InterruptSourceError::NotClaimed;
        }
        if (!Masked)
        {
            return InterruptSourceError::NotReady;
        }
        Claimed = false;
        return InterruptSourceError::None;
    }

    [[nodiscard]] InterruptSourceError Unmask() noexcept override
    {
        ++UnmaskCalls;
        const InterruptSourceError injected = Consume(NextUnmask);
        if (injected != InterruptSourceError::None)
        {
            MarkFailure(injected);
            return injected;
        }
        const InterruptSourceError stateError = CheckClaimed();
        if (stateError != InterruptSourceError::None)
        {
            return stateError;
        }
        Masked = false;
        return InterruptSourceError::None;
    }

    [[nodiscard]] InterruptSourceError Mask() noexcept override
    {
        ++MaskCalls;
        const InterruptSourceError injected = Consume(NextMask);
        if (injected != InterruptSourceError::None)
        {
            MarkFailure(injected);
            return injected;
        }
        const InterruptSourceError stateError = CheckClaimed();
        if (stateError != InterruptSourceError::None)
        {
            return stateError;
        }
        Masked = true;
        return InterruptSourceError::None;
    }

    [[nodiscard]] InterruptSourceError MaskAndAcknowledge() noexcept override
    {
        ++MaskAndAcknowledgeCalls;
        const InterruptSourceError injected = Consume(NextMaskAndAcknowledge);
        if (injected != InterruptSourceError::None)
        {
            MarkFailure(injected);
            return injected;
        }
        const InterruptSourceError stateError = CheckClaimed();
        if (stateError != InterruptSourceError::None)
        {
            return stateError;
        }
        Masked = true;
        return InterruptSourceError::None;
    }

    void RecoverMasked() noexcept
    {
        Failed = false;
        Masked = true;
    }

    bool Ready{true};
    bool Claimed{false};
    bool Masked{true};
    bool Failed{false};
    InterruptSourceError NextClaim{InterruptSourceError::None};
    InterruptSourceError NextRelease{InterruptSourceError::None};
    InterruptSourceError NextUnmask{InterruptSourceError::None};
    InterruptSourceError NextMask{InterruptSourceError::None};
    InterruptSourceError NextMaskAndAcknowledge{InterruptSourceError::None};
    Foundation::UInt64 ClaimCalls{0U};
    Foundation::UInt64 ReleaseCalls{0U};
    Foundation::UInt64 UnmaskCalls{0U};
    Foundation::UInt64 MaskCalls{0U};
    Foundation::UInt64 MaskAndAcknowledgeCalls{0U};

private:
    [[nodiscard]] static InterruptSourceError Consume(InterruptSourceError& error) noexcept
    {
        const InterruptSourceError result = error;
        error = InterruptSourceError::None;
        return result;
    }

    void MarkFailure(const InterruptSourceError error) noexcept
    {
        if (error == InterruptSourceError::HardwareFailure)
        {
            Failed = true;
        }
    }

    [[nodiscard]] InterruptSourceError CheckClaimed() const noexcept
    {
        if (Failed)
        {
            return InterruptSourceError::HardwareFailure;
        }
        if (!Claimed)
        {
            return InterruptSourceError::NotClaimed;
        }
        return InterruptSourceError::None;
    }
};

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

[[nodiscard]] bool FailureIs(const InterruptFailure& failure, const InterruptError error,
                             const InterruptSourceError source = InterruptSourceError::None,
                             const ObjectPinError pin = ObjectPinError::Ok,
                             const NotificationError notification = NotificationError::None) noexcept
{
    return (failure.Error == error) && (failure.Source == source) && (failure.Pin == pin) &&
           (failure.Notification.Error == notification) &&
           BytesAreZero(failure.Notification.Reserved, sizeof(failure.Notification.Reserved)) &&
           BytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

[[nodiscard]] bool SnapshotIs(const InterruptSnapshot& snapshot, const InterruptState state,
                              const Foundation::UInt64 sequence, const bool sourceClaimed,
                              const NotificationId notification = {}) noexcept
{
    const bool eventShape = state == InterruptState::Pending
                                ? snapshot.PendingEvent.IsValid() && (snapshot.PendingEvent.GetSequence() == sequence)
                                : !snapshot.PendingEvent.IsValid();
    const bool notificationShape =
        notification.IsValid() ? snapshot.Notification.IsEqualTo(notification) : !snapshot.Notification.IsValid();
    return (snapshot.State == state) && (snapshot.LastEventSequence == sequence) &&
           (snapshot.SourceClaimed == sourceClaimed) && eventShape && notificationShape &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
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

void TestInitializationAndOwnership(TestContext& runner) noexcept
{
    SpinLock firstLock;
    SpinLock secondLock;
    Asteria::TestSupport::TestObjectRegistry firstRegistry{firstLock};
    Asteria::TestSupport::TestObjectRegistry secondRegistry{secondLock};
    RootCapabilityIssuer firstIssuer{firstRegistry};
    RootCapabilityIssuer secondIssuer{secondRegistry};
    Notification primary;
    Notification alternate;
    Notification foreign;
    MockInterruptSource source;
    Interrupt interrupt{firstRegistry, source};

    const ObjectId primaryObject = CreateNotification(firstIssuer, primary, 11U);
    const ObjectId alternateObject = CreateNotification(firstIssuer, alternate, 12U);
    const ObjectId foreignObject = CreateNotification(secondIssuer, foreign, 13U);
    const NotificationId primaryId{firstRegistry, primaryObject};
    const NotificationId alternateId{firstRegistry, alternateObject};
    const NotificationId foreignId{secondRegistry, foreignObject};

    ASTERIA_EXPECT(runner, "default Interrupt is inert and exposes unpublished metadata", !interrupt.IsInitialized(),
                   interrupt.GetObjectType() == ObjectType::Interrupt, interrupt.GetChargeOwner().GetValue() == 0U,
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Uninitialized, 0U, false));

    const auto uninitializedHandle = interrupt.HandleInterrupt();
    const auto uninitializedAck = interrupt.Acknowledge({}, InterruptCompletion::KeepMasked);
    ASTERIA_EXPECT(runner, "uninitialized operations are side-effect free",
                   FailureIs(interrupt.Arm(), InterruptError::NotInitialized),
                   FailureIs(interrupt.Mask(), InterruptError::NotInitialized), !uninitializedHandle.HasValue(),
                   FailureIs(uninitializedHandle.GetError(), InterruptError::NotInitialized),
                   !uninitializedAck.HasValue(), FailureIs(uninitializedAck.GetError(), InterruptError::NotInitialized),
                   FailureIs(interrupt.Quiesce(), InterruptError::NotInitialized),
                   source.MaskAndAcknowledgeCalls == 0U);

    const auto premature = firstIssuer.CreateObject(interrupt);
    ASTERIA_EXPECT(runner, "ObjectRegistry rejects an uninitialized Interrupt", !premature.HasValue(),
                   premature.GetError() == ObjectRegistryError::InvalidArgument);

    const InterruptFailure invalidOwner = interrupt.Initialize(ResourceDomainId{}, primaryId, primary);
    const InterruptFailure invalidIdentity = interrupt.Initialize(ResourceDomainId{21U}, {}, primary);
    const InterruptFailure wrongRegistry = interrupt.Initialize(ResourceDomainId{21U}, foreignId, foreign);
    const InterruptFailure wrongInstance = interrupt.Initialize(ResourceDomainId{21U}, primaryId, alternate);
    ASTERIA_EXPECT(runner, "configuration, provenance, and storage failures preserve all state",
                   FailureIs(invalidOwner, InterruptError::InvalidConfiguration),
                   FailureIs(invalidIdentity, InterruptError::InvalidNotification),
                   FailureIs(wrongRegistry, InterruptError::NotificationRegistryMismatch),
                   FailureIs(wrongInstance, InterruptError::NotificationInstanceMismatch), source.ClaimCalls == 0U,
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Uninitialized, 0U, false));

    source.NextClaim = InterruptSourceError::NotReady;
    const InterruptFailure sourceRejected = interrupt.Initialize(ResourceDomainId{21U}, primaryId, primary);
    ASTERIA_EXPECT(runner, "source claim failure releases the provisional Notification pin",
                   FailureIs(sourceRejected, InterruptError::SourceRejected, InterruptSourceError::NotReady),
                   source.ClaimCalls == 1U, !source.Claimed,
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Uninitialized, 0U, false));

    const InterruptFailure initialized = interrupt.Initialize(ResourceDomainId{21U}, primaryId, primary);
    ASTERIA_EXPECT(runner, "valid initialization pins Notification and claims one masked source",
                   FailureIs(initialized, InterruptError::None), interrupt.IsInitialized(),
                   interrupt.GetChargeOwner().GetValue() == 21U, source.Claimed, source.Masked,
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Masked, 0U, true, primaryId));

    ASTERIA_EXPECT(
        runner, "Interrupt initialization is single-use",
        FailureIs(interrupt.Initialize(ResourceDomainId{22U}, primaryId, primary), InterruptError::AlreadyInitialized));

    Interrupt competitor{firstRegistry, source};
    const InterruptFailure competingClaim = competitor.Initialize(ResourceDomainId{23U}, alternateId, alternate);
    ASTERIA_EXPECT(runner, "one HAL source cannot be claimed by two Interrupt objects",
                   FailureIs(competingClaim, InterruptError::SourceRejected, InterruptSourceError::AlreadyClaimed),
                   !competitor.IsInitialized());

    const auto registeredInterrupt = firstIssuer.CreateObject(interrupt);
    static_cast<void>(firstIssuer.DestroyObject(primaryObject));
    ASTERIA_EXPECT(runner, "the Interrupt pin prevents Notification collection", registeredInterrupt.HasValue(),
                   !firstIssuer.CollectDestroyedObject().HasValue());

    source.NextRelease = InterruptSourceError::NotReady;
    const InterruptFailure rejectedRelease = interrupt.Quiesce();
    ASTERIA_EXPECT(runner, "a retryable Release failure retains source and Notification ownership",
                   FailureIs(rejectedRelease, InterruptError::SourceRejected, InterruptSourceError::NotReady),
                   source.Claimed, SnapshotIs(interrupt.GetSnapshot(), InterruptState::Masked, 0U, true, primaryId),
                   !firstIssuer.CollectDestroyedObject().HasValue());

    ASTERIA_EXPECT(runner, "Quiesce releases source ownership and the exact Notification pin",
                   FailureIs(interrupt.Quiesce(), InterruptError::None), !source.Claimed,
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Quiesced, 0U, false),
                   FailureIs(interrupt.Quiesce(), InterruptError::AlreadyQuiesced));

    if (registeredInterrupt.HasValue())
    {
        static_cast<void>(firstIssuer.DestroyObject(registeredInterrupt.GetValue()));
    }
    static_cast<void>(firstIssuer.DestroyObject(alternateObject));
    static_cast<void>(secondIssuer.DestroyObject(foreignObject));
    ASTERIA_EXPECT(runner, "initialization and ownership fixtures leave no retained object",
                   firstIssuer.CollectDestroyedObject().HasValue(), firstIssuer.CollectDestroyedObject().HasValue(),
                   firstIssuer.CollectDestroyedObject().HasValue(), secondIssuer.CollectDestroyedObject().HasValue());
}

void TestDeliveryWaitSetAndTokens(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification notification;
    Notification foreignNotification;
    MockInterruptSource source;
    MockInterruptSource foreignSource;
    Interrupt interrupt{registry, source};
    Interrupt foreignInterrupt{registry, foreignSource};
    WaitSet waitSet{registry};
    static_cast<void>(waitSet.Initialize(ResourceDomainId{31U}));

    const ObjectId notificationObject = CreateNotification(issuer, notification, 32U);
    const ObjectId foreignObject = CreateNotification(issuer, foreignNotification, 33U);
    const NotificationId notificationId{registry, notificationObject};
    const NotificationId foreignId{registry, foreignObject};
    const InterruptFailure initialized = interrupt.Initialize(ResourceDomainId{34U}, notificationId, notification);
    const InterruptFailure foreignInitialized =
        foreignInterrupt.Initialize(ResourceDomainId{35U}, foreignId, foreignNotification);
    const auto registration = waitSet.RegisterNotification(notificationId, notification, 0xabcU);
    ASTERIA_EXPECT(runner, "Interrupt delivery can target a Notification owned by one WaitSet",
                   FailureIs(initialized, InterruptError::None), FailureIs(foreignInitialized, InterruptError::None),
                   registration.HasValue());
    if (!registration.HasValue())
    {
        return;
    }

    static_cast<void>(interrupt.Arm());
    source.NextMask = InterruptSourceError::NotReady;
    const InterruptFailure rejectedMask = interrupt.Mask();
    ASTERIA_EXPECT(runner, "Arm and Mask classify retryable HAL failure without changing state",
                   FailureIs(interrupt.Arm(), InterruptError::AlreadyArmed),
                   FailureIs(rejectedMask, InterruptError::SourceRejected, InterruptSourceError::NotReady),
                   !source.Masked, SnapshotIs(interrupt.GetSnapshot(), InterruptState::Armed, 0U, true, notificationId),
                   FailureIs(interrupt.Mask(), InterruptError::None),
                   FailureIs(interrupt.Mask(), InterruptError::AlreadyMasked), source.Masked);

    const Foundation::UInt64 callsBeforeSpurious = source.MaskAndAcknowledgeCalls;
    const auto spurious = interrupt.HandleInterrupt();
    const auto spuriousReady = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "a masked spurious IRQ is acknowledged without signaling or faulting", !spurious.HasValue(),
                   FailureIs(spurious.GetError(), InterruptError::SpuriousInterrupt),
                   source.MaskAndAcknowledgeCalls == callsBeforeSpurious + 1U,
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Masked, 0U, true, notificationId),
                   !spuriousReady.HasValue(), spuriousReady.GetError().Error == WaitSetError::WouldBlock,
                   FailureIs(interrupt.Arm(), InterruptError::None));

    // The masked edge was remembered, as a message source's pending bit:
    // arming delivered it as event 1.
    const InterruptSnapshot remembered = interrupt.GetSnapshot();
    const auto rememberedReady = waitSet.ClaimReady();
    if (rememberedReady.HasValue())
    {
        static_cast<void>(waitSet.CommitClaim(rememberedReady.GetValue().Claim));
    }
    const auto rememberedAck = interrupt.Acknowledge(remembered.PendingEvent, InterruptCompletion::Rearm);
    ASTERIA_EXPECT(runner, "an edge that arrived while masked is delivered when the Interrupt is armed",
                   remembered.State == InterruptState::Pending, remembered.LastEventSequence == 1U,
                   rememberedReady.HasValue(), rememberedAck.HasValue(),
                   rememberedAck.HasValue() &&
                       SnapshotIs(rememberedAck.GetValue(), InterruptState::Armed, 1U, true, notificationId));

    const Foundation::UInt64 callsBeforeDelivery = source.MaskAndAcknowledgeCalls;
    const auto firstDelivery = interrupt.HandleInterrupt();
    const auto duplicateDelivery = interrupt.HandleInterrupt();
    const auto readyClaim = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "ISR handling masks, acknowledges, coalesces, and signals WaitSet", firstDelivery.HasValue(),
                   firstDelivery.GetValue().NewEvent, firstDelivery.GetValue().Event.IsValid(),
                   firstDelivery.GetValue().Event.GetSequence() == 2U, duplicateDelivery.HasValue(),
                   !duplicateDelivery.GetValue().NewEvent,
                   duplicateDelivery.GetValue().Event.IsEqualTo(firstDelivery.GetValue().Event),
                   source.MaskAndAcknowledgeCalls == callsBeforeDelivery + 2U, source.Masked, readyClaim.HasValue(),
                   readyClaim.GetValue().Tag == 0xabcU);
    if (!firstDelivery.HasValue() || !readyClaim.HasValue())
    {
        return;
    }
    static_cast<void>(waitSet.CommitClaim(readyClaim.GetValue().Claim));

    const auto lateDuplicate = interrupt.HandleInterrupt();
    const auto lateDuplicateReady = waitSet.ClaimReady();
    ASTERIA_EXPECT(runner, "a duplicate IRQ cannot re-signal a consumed pending event", lateDuplicate.HasValue(),
                   !lateDuplicate.GetValue().NewEvent,
                   lateDuplicate.GetValue().Event.IsEqualTo(firstDelivery.GetValue().Event),
                   source.MaskAndAcknowledgeCalls == callsBeforeDelivery + 3U, !lateDuplicateReady.HasValue(),
                   lateDuplicateReady.GetError().Error == WaitSetError::WouldBlock);

    const auto defaultAck = interrupt.Acknowledge({}, InterruptCompletion::KeepMasked);
    const auto invalidCompletion =
        interrupt.Acknowledge(firstDelivery.GetValue().Event, static_cast<InterruptCompletion>(0xffU));
    ASTERIA_EXPECT(runner, "invalid resolution and teardown attempts preserve a pending event", !defaultAck.HasValue(),
                   FailureIs(defaultAck.GetError(), InterruptError::InvalidEvent), !invalidCompletion.HasValue(),
                   FailureIs(invalidCompletion.GetError(), InterruptError::InvalidCompletion),
                   FailureIs(interrupt.Mask(), InterruptError::EventPending),
                   FailureIs(interrupt.Quiesce(), InterruptError::EventPending),
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Pending, 2U, true, notificationId));

    // The foreign Interrupt's second event has the same sequence as this
    // one's pending event.
    static_cast<void>(foreignInterrupt.Arm());
    const auto foreignFirst = foreignInterrupt.HandleInterrupt();
    if (foreignFirst.HasValue())
    {
        static_cast<void>(foreignInterrupt.Acknowledge(foreignFirst.GetValue().Event, InterruptCompletion::Rearm));
    }
    const auto foreignDelivery = foreignInterrupt.HandleInterrupt();
    const auto foreignAck =
        interrupt.Acknowledge(foreignDelivery.HasValue() ? foreignDelivery.GetValue().Event : InterruptEventToken{},
                              InterruptCompletion::KeepMasked);
    ASTERIA_EXPECT(runner, "a same-sequence event token from another Interrupt is rejected", foreignDelivery.HasValue(),
                   foreignDelivery.HasValue() && (foreignDelivery.GetValue().Event.GetSequence() == 2U),
                   !foreignAck.HasValue(), FailureIs(foreignAck.GetError(), InterruptError::InvalidEvent));

    const auto firstAcknowledged =
        interrupt.Acknowledge(firstDelivery.GetValue().Event, InterruptCompletion::KeepMasked);
    const auto staleAck = interrupt.Acknowledge(firstDelivery.GetValue().Event, InterruptCompletion::KeepMasked);
    ASTERIA_EXPECT(runner, "exact acknowledgement keeps the source masked and makes copies stale",
                   firstAcknowledged.HasValue(),
                   SnapshotIs(firstAcknowledged.GetValue(), InterruptState::Masked, 2U, true, notificationId),
                   !staleAck.HasValue(), FailureIs(staleAck.GetError(), InterruptError::StaleEvent));

    if (foreignDelivery.HasValue())
    {
        static_cast<void>(
            foreignInterrupt.Acknowledge(foreignDelivery.GetValue().Event, InterruptCompletion::KeepMasked));
    }
    const auto foreignNotificationClaim = foreignNotification.Claim();
    if (foreignNotificationClaim.HasValue())
    {
        static_cast<void>(foreignNotification.CommitClaim(foreignNotificationClaim.GetValue()));
    }

    source.NextUnmask = InterruptSourceError::NotReady;
    const InterruptSnapshot beforeRejectedArm = interrupt.GetSnapshot();
    const InterruptFailure rejectedArm = interrupt.Arm();
    ASTERIA_EXPECT(runner, "nonterminal HAL rejection leaves the masked generation unchanged",
                   FailureIs(rejectedArm, InterruptError::SourceRejected, InterruptSourceError::NotReady),
                   SnapshotIs(interrupt.GetSnapshot(), beforeRejectedArm.State, beforeRejectedArm.LastEventSequence,
                              beforeRejectedArm.SourceClaimed, beforeRejectedArm.Notification));

    static_cast<void>(interrupt.Arm());
    const auto secondDelivery = interrupt.HandleInterrupt();
    const auto secondReady = waitSet.ClaimReady();
    if (secondReady.HasValue())
    {
        static_cast<void>(waitSet.CommitClaim(secondReady.GetValue().Claim));
    }
    source.NextUnmask = InterruptSourceError::NotReady;
    const auto rejectedRearm =
        interrupt.Acknowledge(secondDelivery.HasValue() ? secondDelivery.GetValue().Event : InterruptEventToken{},
                              InterruptCompletion::Rearm);
    ASTERIA_EXPECT(runner, "failed rearm retains the exact pending event for retry", secondDelivery.HasValue(),
                   secondDelivery.GetValue().Event.GetSequence() == 3U, secondReady.HasValue(),
                   !rejectedRearm.HasValue(),
                   FailureIs(rejectedRearm.GetError(), InterruptError::SourceRejected, InterruptSourceError::NotReady),
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Pending, 3U, true, notificationId));

    const auto rearmed =
        interrupt.Acknowledge(secondDelivery.HasValue() ? secondDelivery.GetValue().Event : InterruptEventToken{},
                              InterruptCompletion::Rearm);
    ASTERIA_EXPECT(runner, "retrying the same event rearms only after HAL success", rearmed.HasValue(),
                   SnapshotIs(rearmed.GetValue(), InterruptState::Armed, 3U, true, notificationId),
                   FailureIs(interrupt.Mask(), InterruptError::None));

    static_cast<void>(waitSet.Unregister(registration.GetValue().Registration));
    ASTERIA_EXPECT(runner, "resolved Interrupts release both dedicated sources",
                   FailureIs(interrupt.Quiesce(), InterruptError::None),
                   FailureIs(foreignInterrupt.Quiesce(), InterruptError::None));

    static_cast<void>(issuer.DestroyObject(notificationObject));
    static_cast<void>(issuer.DestroyObject(foreignObject));
    ASTERIA_EXPECT(runner, "delivery and token fixtures release both Notification pins",
                   issuer.CollectDestroyedObject().HasValue(), issuer.CollectDestroyedObject().HasValue());
}

void TestHardwareFailureRecovery(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification notification;
    MockInterruptSource source;
    Interrupt interrupt{registry, source};
    const ObjectId notificationObject = CreateNotification(issuer, notification, 41U);
    const NotificationId notificationId{registry, notificationObject};
    static_cast<void>(interrupt.Initialize(ResourceDomainId{42U}, notificationId, notification));
    static_cast<void>(interrupt.Arm());

    source.NextMaskAndAcknowledge = InterruptSourceError::HardwareFailure;
    const auto failedDelivery = interrupt.HandleInterrupt();
    const Foundation::UInt64 callsAfterFailure = source.MaskAndAcknowledgeCalls;
    const auto repeatedDelivery = interrupt.HandleInterrupt();
    ASTERIA_EXPECT(
        runner, "a faulted Interrupt never touches the source before recovery", !failedDelivery.HasValue(),
        FailureIs(failedDelivery.GetError(), InterruptError::SourceRejected, InterruptSourceError::HardwareFailure),
        !repeatedDelivery.HasValue(), FailureIs(repeatedDelivery.GetError(), InterruptError::Faulted),
        source.MaskAndAcknowledgeCalls == callsAfterFailure,
        SnapshotIs(interrupt.GetSnapshot(), InterruptState::Faulted, 0U, true, notificationId),
        FailureIs(interrupt.Arm(), InterruptError::Faulted), FailureIs(interrupt.Mask(), InterruptError::Faulted));

    source.RecoverMasked();
    source.NextRelease = InterruptSourceError::HardwareFailure;
    const InterruptFailure failedRelease = interrupt.Quiesce();
    source.RecoverMasked();
    const InterruptFailure recovered = interrupt.Quiesce();
    ASTERIA_EXPECT(runner, "a recovered source remains retained until Release succeeds",
                   FailureIs(failedRelease, InterruptError::SourceRejected, InterruptSourceError::HardwareFailure),
                   FailureIs(recovered, InterruptError::None),
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Quiesced, 0U, false));

    static_cast<void>(issuer.DestroyObject(notificationObject));
    ASTERIA_EXPECT(runner, "fault recovery releases the retained Notification generation",
                   issuer.CollectDestroyedObject().HasValue());
}

void TestTerminalHalFailures(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification armNotification;
    Notification maskNotification;
    Notification rearmNotification;
    MockInterruptSource armSource;
    MockInterruptSource maskSource;
    MockInterruptSource rearmSource;
    Interrupt armInterrupt{registry, armSource};
    Interrupt maskInterrupt{registry, maskSource};
    Interrupt rearmInterrupt{registry, rearmSource};

    const ObjectId armObject = CreateNotification(issuer, armNotification, 61U);
    const ObjectId maskObject = CreateNotification(issuer, maskNotification, 62U);
    const ObjectId rearmObject = CreateNotification(issuer, rearmNotification, 63U);
    const NotificationId armId{registry, armObject};
    const NotificationId maskId{registry, maskObject};
    const NotificationId rearmId{registry, rearmObject};
    static_cast<void>(armInterrupt.Initialize(ResourceDomainId{64U}, armId, armNotification));
    static_cast<void>(maskInterrupt.Initialize(ResourceDomainId{65U}, maskId, maskNotification));
    static_cast<void>(rearmInterrupt.Initialize(ResourceDomainId{66U}, rearmId, rearmNotification));

    armSource.NextUnmask = InterruptSourceError::HardwareFailure;
    const InterruptFailure armFailure = armInterrupt.Arm();

    static_cast<void>(maskInterrupt.Arm());
    maskSource.NextMask = InterruptSourceError::HardwareFailure;
    const InterruptFailure maskFailure = maskInterrupt.Mask();

    static_cast<void>(rearmInterrupt.Arm());
    const auto delivery = rearmInterrupt.HandleInterrupt();
    rearmSource.NextUnmask = InterruptSourceError::HardwareFailure;
    const auto rearmFailure = rearmInterrupt.Acknowledge(
        delivery.HasValue() ? delivery.GetValue().Event : InterruptEventToken{}, InterruptCompletion::Rearm);

    ASTERIA_EXPECT(
        runner, "every terminal Arm, Mask, and Rearm HAL failure faults ownership",
        FailureIs(armFailure, InterruptError::SourceRejected, InterruptSourceError::HardwareFailure),
        SnapshotIs(armInterrupt.GetSnapshot(), InterruptState::Faulted, 0U, true, armId),
        FailureIs(maskFailure, InterruptError::SourceRejected, InterruptSourceError::HardwareFailure),
        SnapshotIs(maskInterrupt.GetSnapshot(), InterruptState::Faulted, 0U, true, maskId), !rearmFailure.HasValue(),
        FailureIs(rearmFailure.GetError(), InterruptError::SourceRejected, InterruptSourceError::HardwareFailure),
        SnapshotIs(rearmInterrupt.GetSnapshot(), InterruptState::Faulted, 1U, true, rearmId));

    armSource.RecoverMasked();
    maskSource.RecoverMasked();
    rearmSource.RecoverMasked();
    static_cast<void>(armInterrupt.Quiesce());
    static_cast<void>(maskInterrupt.Quiesce());
    static_cast<void>(rearmInterrupt.Quiesce());
    static_cast<void>(issuer.DestroyObject(armObject));
    static_cast<void>(issuer.DestroyObject(maskObject));
    static_cast<void>(issuer.DestroyObject(rearmObject));
    ASTERIA_EXPECT(runner, "terminal HAL fixtures recover, quiesce, and release all pins",
                   issuer.CollectDestroyedObject().HasValue(), issuer.CollectDestroyedObject().HasValue(),
                   issuer.CollectDestroyedObject().HasValue());
}

void TestSnapshotConcurrency(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Notification notification;
    MockInterruptSource source;
    Interrupt interrupt{registry, source};
    const ObjectId notificationObject = CreateNotification(issuer, notification, 51U);
    const NotificationId notificationId{registry, notificationObject};
    static_cast<void>(interrupt.Initialize(ResourceDomainId{52U}, notificationId, notification));

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::thread observer{[&]() noexcept
                         {
                             bool began{false};
                             for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
                             {
                                 if (start.load(std::memory_order_acquire))
                                 {
                                     began = true;
                                     break;
                                 }
                                 std::this_thread::yield();
                             }
                             if (!began)
                             {
                                 failed.store(true, std::memory_order_release);
                                 return;
                             }
                             for (Foundation::Size iteration{0U}; iteration < 16'384U; ++iteration)
                             {
                                 const InterruptSnapshot snapshot = interrupt.GetSnapshot();
                                 if (!snapshot.Notification.IsEqualTo(notificationId) || !snapshot.SourceClaimed ||
                                     (snapshot.State == InterruptState::Uninitialized) ||
                                     (snapshot.State == InterruptState::Quiesced) ||
                                     (snapshot.State == InterruptState::Faulted))
                                 {
                                     failed.store(true, std::memory_order_release);
                                     return;
                                 }
                             }
                         }};

    start.store(true, std::memory_order_release);
    constexpr Foundation::Size Iterations{1'024U};
    for (Foundation::Size iteration{0U}; iteration < Iterations; ++iteration)
    {
        if (interrupt.Arm().Error != InterruptError::None)
        {
            failed.store(true, std::memory_order_release);
            break;
        }
        const auto delivery = interrupt.HandleInterrupt();
        if (!delivery.HasValue())
        {
            failed.store(true, std::memory_order_release);
            break;
        }
        const auto notificationClaim = notification.Claim();
        if (!notificationClaim.HasValue() || !notification.CommitClaim(notificationClaim.GetValue()).HasValue())
        {
            failed.store(true, std::memory_order_release);
            break;
        }
        if (!interrupt.Acknowledge(delivery.GetValue().Event, InterruptCompletion::KeepMasked).HasValue())
        {
            failed.store(true, std::memory_order_release);
            break;
        }
    }
    observer.join();

    ASTERIA_EXPECT(runner, "ISR publication and concurrent snapshots remain race-free",
                   !failed.load(std::memory_order_acquire),
                   SnapshotIs(interrupt.GetSnapshot(), InterruptState::Masked, Iterations, true, notificationId));

    Foundation::UInt64 next{9U};
    const bool advanced = InterruptEventSequencePolicy::TryAdvance(9U, next);
    Foundation::UInt64 preserved{19U};
    const bool wrapped = InterruptEventSequencePolicy::TryAdvance(InterruptEventSequencePolicy::Maximum, preserved);
    ASTERIA_EXPECT(runner, "event sequence arithmetic rejects wrap without modifying output", advanced, next == 10U,
                   !wrapped, preserved == 19U);

    static_cast<void>(interrupt.Quiesce());
    static_cast<void>(issuer.DestroyObject(notificationObject));
    ASTERIA_EXPECT(runner, "concurrency fixture quiesces before releasing object storage",
                   issuer.CollectDestroyedObject().HasValue());
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InterruptTests"};
    Asteria::Test::TestInitializationAndOwnership(runner);
    Asteria::Test::TestDeliveryWaitSetAndTokens(runner);
    Asteria::Test::TestHardwareFailureRecovery(runner);
    Asteria::Test::TestTerminalHalFailures(runner);
    Asteria::Test::TestSnapshotConcurrency(runner);
    return runner.Finish();
}
