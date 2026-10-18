// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Kernel/System/WaitSetSystemCallDispatcher.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationError;
using Kernel::Ipc::NotificationId;
using Kernel::Ipc::WaitSet;
using Kernel::Ipc::WaitSetError;
using Kernel::Ipc::WaitSetId;
using Kernel::Ipc::WaitSetRegistrationToken;
using Kernel::Ipc::WaitSetThreadDispatcher;
using Kernel::Ipc::WaitSetThreadDispatcherError;
using Kernel::Ipc::WaitSetThreadDispatcherFailure;
using Kernel::Ipc::WaitSetThreadDispatcherReceipt;
using Kernel::Ipc::WaitSetThreadDispatcherSnapshot;
using Kernel::Ipc::WaitSetThreadDispatcherState;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::BlockedRunCommitOutcome;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::ProcessorActiveExecutionBinding;
using Kernel::Scheduler::ProcessorScheduler;
using Kernel::Scheduler::ProcessorSchedulerState;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::System::Detail::RejectUnavailableCompletion;
using Kernel::System::Detail::ValidateCompletionSnapshot;
using Kernel::System::Detail::ValidatePreparedReceipt;
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallDispatcher;
using Kernel::System::SystemCallNumber;
using Kernel::System::SystemCallRequest;
using Kernel::System::WaitSetSystemCallDispatchError;
using Kernel::System::WaitSetSystemCallDispatcher;
using Kernel::System::WaitSetSystemCallDispatchFailure;
using Kernel::System::WaitSetSystemCallDisposition;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

constexpr UInt64 WaitTag{0xA57E'0000'0000'0042ULL};

[[nodiscard]] constexpr SchedulingContextConfiguration Configuration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 200U,
        .PeriodTicks = 1000U,
        .RelativeDeadlineTicks = 800U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr CapabilityRights WaitRights() noexcept
{
    return CapabilityRights{CapabilityRight::Wait};
}

template <Foundation::Size Capacity>
[[nodiscard]] bool ReservedIsZero(const Foundation::UInt8 (&reserved)[Capacity]) noexcept
{
    bool zero{true};
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        zero = zero && (reserved[index] == 0U);
    }
    return zero;
}

[[nodiscard]] constexpr SystemCallRequest MakeRequest(const CapabilityHandle handle) noexcept
{
    return SystemCallRequest{
        .Number = static_cast<UInt64>(SystemCallNumber::WaitSetWait),
        .Arguments =
            {
                handle.GetSelector(),
                handle.GetAuthenticator(),
                0U,
                0U,
                0U,
                0U,
            },
    };
}

class WaitSystemCallFixture final
{
public:
    WaitSystemCallFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          Scheduler{Registry, Clock, CpuId{0U}}, Set{Registry}, Context{Registry},
          ThreadDispatcher{Registry, Scheduler.GetWakeFacade()}, Dispatcher{Registry, ThreadDispatcher}
    {
    }

    ~WaitSystemCallFixture() noexcept
    {
        if (Scheduler.GetSnapshot().State == ProcessorSchedulerState::Active)
        {
            static_cast<void>(Scheduler.Finish(ThreadRunDisposition::Exited, 1U));
        }
    }

    WaitSystemCallFixture(const WaitSystemCallFixture&) = delete;
    WaitSystemCallFixture& operator=(const WaitSystemCallFixture&) = delete;
    WaitSystemCallFixture(WaitSystemCallFixture&&) = delete;
    WaitSystemCallFixture& operator=(WaitSystemCallFixture&&) = delete;

    [[nodiscard]] bool Initialize() noexcept
    {
        if ((Event.Initialize(ResourceDomainId{11U}) != NotificationError::None) ||
            (Set.Initialize(ResourceDomainId{11U}) != WaitSetError::None) ||
            (ThreadStorage.Initialize(ResourceDomainId{11U}) != ThreadError::None) ||
            (Context.Initialize(Configuration(), ResourceDomainId{11U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }

        const auto notification = Issuer.CreateObject(Event);
        const auto waitSet = Issuer.CreateObject(Set);
        const auto thread = Issuer.CreateObject(ThreadStorage);
        const auto context = Issuer.CreateObject(Context);
        if (!notification.HasValue() || !waitSet.HasValue() || !thread.HasValue() || !context.HasValue())
        {
            return false;
        }

        NotificationObject = notification.GetValue();
        WaitSetObject = waitSet.GetValue();
        ThreadObject = thread.GetValue();
        ContextObject = context.GetValue();
        NotificationIdentity = NotificationId{Registry, NotificationObject};
        ThreadIdentity = ThreadId{Registry, ThreadObject};
        ContextIdentity = SchedulingContextId{Registry, ContextObject};

        const auto registered = Set.RegisterNotification(NotificationIdentity, Event, WaitTag);
        if (!registered.HasValue())
        {
            return false;
        }
        Registration = registered.GetValue().Registration;

        if ((Context.Bind(ThreadIdentity) != SchedulingContextError::None) ||
            (ThreadStorage.MakeReady() != ThreadError::None) ||
            !Scheduler.Enqueue(ContextIdentity, Context, ThreadIdentity, ThreadStorage).HasValue())
        {
            return false;
        }
        if (!DispatchNext())
        {
            return false;
        }

        const auto minted = Mint(WaitSetObject, WaitRights(), MonotonicTime::Infinite());
        if (!minted.HasValue())
        {
            return false;
        }
        WaitHandle = minted.GetValue();
        return true;
    }

    [[nodiscard]] bool DispatchNext() noexcept
    {
        const auto admission = Scheduler.DispatchNext();
        if (!admission.HasValue())
        {
            return false;
        }
        const auto active = Scheduler.GetActiveExecutionBinding(admission.GetValue());
        if (!active.HasValue())
        {
            return false;
        }
        Active = active.GetValue();
        return true;
    }

    [[nodiscard]] bool BlockSignalAndResume() noexcept
    {
        const auto finished = Scheduler.Finish(ThreadRunDisposition::Blocked);
        if (!finished.HasValue())
        {
            return false;
        }
        if (ThreadDispatcher.CommitBlockedFromFinishedRun(finished.GetValue().Thread.BlockToken) !=
            BlockedRunCommitOutcome::Accepted)
        {
            return false;
        }
        if (Event.Signal() != NotificationError::None)
        {
            return false;
        }
        const auto progressed = ThreadDispatcher.Poll();
        return progressed.HasValue() && (progressed.GetValue().State == WaitSetThreadDispatcherState::Completed) &&
               DispatchNext();
    }

    // Blocks the prepared wait, then expires it at now without a signal.
    [[nodiscard]] bool BlockExpireAndResume(const Foundation::UInt64 now) noexcept
    {
        const auto finished = Scheduler.Finish(ThreadRunDisposition::Blocked);
        if (!finished.HasValue() || (ThreadDispatcher.CommitBlockedFromFinishedRun(
                                         finished.GetValue().Thread.BlockToken) != BlockedRunCommitOutcome::Accepted))
        {
            return false;
        }
        const auto polled = ThreadDispatcher.Poll();
        const auto expired = ThreadDispatcher.Expire(now);
        return polled.HasValue() && (polled.GetValue().State == WaitSetThreadDispatcherState::Blocked) &&
               expired.HasValue() && expired.GetValue().TimedOut &&
               (expired.GetValue().State == WaitSetThreadDispatcherState::Completed) && DispatchNext();
    }

    // Blocks the prepared wait, then aborts it as a kill does.
    [[nodiscard]] bool BlockAbortAndResume() noexcept
    {
        const auto finished = Scheduler.Finish(ThreadRunDisposition::Blocked);
        if (!finished.HasValue() || (ThreadDispatcher.CommitBlockedFromFinishedRun(
                                         finished.GetValue().Thread.BlockToken) != BlockedRunCommitOutcome::Accepted))
        {
            return false;
        }
        const auto aborted = ThreadDispatcher.Abort();
        return aborted.HasValue() && aborted.GetValue().Aborted &&
               (aborted.GetValue().State == WaitSetThreadDispatcherState::Completed) && DispatchNext();
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Mint(const ObjectId object, const CapabilityRights rights, const MonotonicTime deadline) noexcept
    {
        const auto domain = Revocations.CreateRoot();
        if (!domain.HasValue())
        {
            return Foundation::Result<CapabilityHandle, CapabilityError>::Failure(
                CapabilityError::InvalidConfiguration);
        }
        return MintInDomain(object, rights, deadline, domain.GetValue());
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    MintInDomain(const ObjectId object, const CapabilityRights rights, const MonotonicTime deadline,
                 const RevocationDomainId domain) noexcept
    {
        return Issuer.Mint(Space, object, rights, domain, deadline, 0xA57EU);
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ProcessorScheduler Scheduler;
    Notification Event;
    WaitSet Set;
    Thread ThreadStorage;
    SchedulingContext Context;
    WaitSetThreadDispatcher ThreadDispatcher;
    WaitSetSystemCallDispatcher Dispatcher;
    ObjectId NotificationObject{};
    ObjectId WaitSetObject{};
    ObjectId ThreadObject{};
    ObjectId ContextObject{};
    NotificationId NotificationIdentity{};
    ThreadId ThreadIdentity{};
    SchedulingContextId ContextIdentity{};
    WaitSetRegistrationToken Registration{};
    CapabilityHandle WaitHandle{};
    ProcessorActiveExecutionBinding Active{};
};

void TestNumberAndArguments(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    SystemCallRequest malformed = MakeRequest(fixture.WaitHandle);
    malformed.Arguments[3U] = 1U;
    const auto rejected = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, malformed);
    SystemCallRequest tooLong = MakeRequest(fixture.WaitHandle);
    tooLong.Arguments[2U] = WaitSetSystemCallDispatcher::MaximumTimeoutMicroseconds + 1U;
    const auto overlong = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, tooLong);
    SystemCallRequest timed = MakeRequest(fixture.WaitHandle);
    timed.Arguments[2U] = 1U;
    const auto unconfigured = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, timed);
    const SystemCallRequest unknown{
        .Number = 9U,
        .Arguments = {},
    };
    const auto unknownResult = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, unknown);
    const auto ordinaryResult = SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(fixture.WaitHandle));

    ASTERIA_EXPECT(runner, "syscall values 1 through 7 are preserved and WaitSetWait is 8",
                   static_cast<UInt64>(SystemCallNumber::CapabilityClose) == 1U,
                   static_cast<UInt64>(SystemCallNumber::CapabilityQuery) == 2U,
                   static_cast<UInt64>(SystemCallNumber::CapabilityDerive) == 3U,
                   static_cast<UInt64>(SystemCallNumber::CapabilityDuplicate) == 4U,
                   static_cast<UInt64>(SystemCallNumber::CapabilityRevoke) == 5U,
                   static_cast<UInt64>(SystemCallNumber::IpcSend) == 6U,
                   static_cast<UInt64>(SystemCallNumber::IpcReceive) == 7U,
                   static_cast<UInt64>(SystemCallNumber::WaitSetWait) == 8U);
    ASTERIA_EXPECT(
        runner, "malformed arguments are bounded and syscall 8 is never a capability primitive", initialized,
        rejected.HasValue(), rejected.GetValue().Disposition == WaitSetSystemCallDisposition::Completed,
        rejected.GetValue().Response.Error == SystemCallError::InvalidArguments,
        overlong.HasValue() && (overlong.GetValue().Response.Error == SystemCallError::InvalidArguments),
        unconfigured.HasValue() && (unconfigured.GetValue().Response.Error == SystemCallError::InvalidConfiguration),
        unknownResult.HasValue(), unknownResult.GetValue().Response.Error == SystemCallError::UnknownSystemCall,
        ordinaryResult.Error == SystemCallError::UnknownSystemCall, ordinaryResult.Value0 == 0U,
        ordinaryResult.Value1 == 0U,
        fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

void TestCapabilityFailures(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    CapabilitySpace invalidSpace{fixture.Registry, fixture.Revocations, fixture.Clock, 0U, 0U};
    const auto missingWait =
        fixture.Mint(fixture.WaitSetObject, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime::Infinite());
    const auto wrongType = fixture.Mint(fixture.NotificationObject, WaitRights(), MonotonicTime::Infinite());
    const auto stale = fixture.Mint(fixture.WaitSetObject, WaitRights(), MonotonicTime::Infinite());
    const bool staleClosed = stale.HasValue() && (fixture.Space.Close(stale.GetValue()) == CapabilityError::Ok);

    const auto revokedDomain = fixture.Revocations.CreateRoot();
    const auto revoked =
        revokedDomain.HasValue()
            ? fixture.MintInDomain(fixture.WaitSetObject, WaitRights(), MonotonicTime::Infinite(),
                                   revokedDomain.GetValue())
            : Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidConfiguration);
    const bool wasRevoked = revokedDomain.HasValue() && (fixture.Revocations.Revoke(revokedDomain.GetValue()) ==
                                                         Kernel::Object::RevocationError::Ok);

    const auto expired = fixture.Mint(fixture.WaitSetObject, WaitRights(), MonotonicTime{20U});
    fixture.Clock.Set(30U);

    const auto invalidResult = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest({}));
    const auto configurationResult =
        fixture.Dispatcher.Dispatch(invalidSpace, fixture.Active, MakeRequest(fixture.WaitHandle));
    const auto deniedResult =
        missingWait.HasValue()
            ? fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(missingWait.GetValue()))
            : fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest({}));
    const auto typeResult =
        wrongType.HasValue()
            ? fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(wrongType.GetValue()))
            : fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest({}));
    const auto staleResult =
        stale.HasValue() ? fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(stale.GetValue()))
                         : fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest({}));
    const auto revokedResult =
        revoked.HasValue() ? fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(revoked.GetValue()))
                           : fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest({}));
    const auto expiredResult =
        expired.HasValue() ? fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(expired.GetValue()))
                           : fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest({}));

    ASTERIA_EXPECT(
        runner,
        "lookup maps configuration, invalid, denied, type, stale, revoked, and expired failures without suspension",
        initialized, staleClosed, wasRevoked, invalidResult.HasValue(),
        invalidResult.GetValue().Response.Error == SystemCallError::InvalidHandle, configurationResult.HasValue(),
        configurationResult.GetValue().Response.Error == SystemCallError::InvalidConfiguration, deniedResult.HasValue(),
        deniedResult.GetValue().Response.Error == SystemCallError::RightsDenied, typeResult.HasValue(),
        typeResult.GetValue().Response.Error == SystemCallError::TypeMismatch, staleResult.HasValue(),
        staleResult.GetValue().Response.Error == SystemCallError::InvalidHandle, revokedResult.HasValue(),
        revokedResult.GetValue().Response.Error == SystemCallError::Revoked, expiredResult.HasValue(),
        expiredResult.GetValue().Response.Error == SystemCallError::Expired,
        fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

void TestDestroyedCapabilityObject(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const ObjectRegistryError destroyed = fixture.Issuer.DestroyObject(fixture.WaitSetObject);
    const auto result = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));

    ASTERIA_EXPECT(runner, "a capability whose exact object generation is quiescing reports ObjectNotFound",
                   initialized, destroyed == ObjectRegistryError::Ok, result.HasValue(),
                   result.GetValue().Response.Error == SystemCallError::ObjectNotFound,
                   fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

void TestActiveBindingMismatch(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    Thread foreignThread;
    const bool foreignInitialized = foreignThread.Initialize(ResourceDomainId{11U}) == ThreadError::None;
    ProcessorActiveExecutionBinding mismatch = fixture.Active;
    mismatch.ThreadStorage = &foreignThread;
    const auto result = fixture.Dispatcher.Dispatch(fixture.Space, mismatch, MakeRequest(fixture.WaitHandle));

    ASTERIA_EXPECT(runner, "trusted active storage must match the exact registry identity before capability use",
                   initialized, foreignInitialized, !result.HasValue(),
                   result.GetError().Error == WaitSetSystemCallDispatchError::ThreadInstanceMismatch,
                   fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

void TestImmediateReadyAndReuse(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const NotificationError firstSignal = fixture.Event.Signal();
    const auto first = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));
    const NotificationError secondSignal = fixture.Event.Signal();
    const auto second = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));

    ASTERIA_EXPECT(
        runner, "immediate readiness returns exact tag and registration sequence and consumes completion for reuse",
        initialized, firstSignal == NotificationError::None, first.HasValue(),
        first.GetValue().Disposition == WaitSetSystemCallDisposition::Completed,
        first.GetValue().Response.Error == SystemCallError::None, first.GetValue().Response.Value0 == WaitTag,
        first.GetValue().Response.Value1 == fixture.Registration.GetSequence(), first.GetValue().Wait.Tag == WaitTag,
        ReservedIsZero(first.GetValue().Wait.Reserved), secondSignal == NotificationError::None, second.HasValue(),
        second.GetValue().Response.Value0 == WaitTag,
        second.GetValue().Response.Value1 == fixture.Registration.GetSequence(),
        fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

void TestDispatcherAndGenerationBinding(TestContext& runner) noexcept
{
    {
        WaitSystemCallFixture fixture;
        const bool initialized = fixture.Initialize();
        const WaitSetId waitSet{fixture.Registry, fixture.WaitSetObject};
        WaitSetThreadDispatcher substitute{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared = fixture.ThreadDispatcher.Prepare(Kernel::Ipc::WaitSetThreadPrepareRequest{
            .WaitSet = waitSet,
            .Thread = fixture.ThreadIdentity,
            .SchedulingContext = fixture.ContextIdentity,
            .DeadlineTicks = 0U,
        });
        WaitSetThreadDispatcherReceipt exact{};
        WaitSetThreadDispatcherReceipt foreignInstance{};
        WaitSetThreadDispatcherReceipt foreignGeneration{};
        if (prepared.HasValue())
        {
            exact = prepared.GetValue();
            foreignInstance = prepared.GetValue();
            foreignInstance.Dispatcher = &substitute;
            foreignGeneration = prepared.GetValue();
            ++foreignGeneration.TransactionGeneration;
        }
        const WaitSetThreadDispatcherSnapshot current = fixture.ThreadDispatcher.GetSnapshot();
        const WaitSetSystemCallDispatchError exactError =
            ValidatePreparedReceipt(fixture.ThreadDispatcher, current, exact, fixture.Active, waitSet);
        const WaitSetSystemCallDispatchError instanceError =
            ValidatePreparedReceipt(fixture.ThreadDispatcher, current, foreignInstance, fixture.Active, waitSet);
        const WaitSetSystemCallDispatchError generationError =
            ValidatePreparedReceipt(fixture.ThreadDispatcher, current, foreignGeneration, fixture.Active, waitSet);
        const WaitSetThreadDispatcherFailure rolledBack =
            prepared.HasValue() ? fixture.ThreadDispatcher.RejectReceiptInvariant(prepared.GetValue())
                                : WaitSetThreadDispatcherFailure{};
        const WaitSetThreadDispatcherSnapshot afterRollback = fixture.ThreadDispatcher.GetSnapshot();

        ASTERIA_EXPECT(runner,
                       "prepared validation binds the exact dispatcher and current "
                       "generation while an exact report remains rollback-safe",
                       initialized, prepared.HasValue(), exactError == WaitSetSystemCallDispatchError::None,
                       instanceError == WaitSetSystemCallDispatchError::DispatcherReceiptMismatch,
                       generationError == WaitSetSystemCallDispatchError::DispatcherReceiptMismatch,
                       rolledBack.Error == WaitSetThreadDispatcherError::ReceiptInvariantRejected,
                       afterRollback.State == WaitSetThreadDispatcherState::Idle);
    }

    {
        WaitSystemCallFixture fixture;
        const bool initialized = fixture.Initialize();
        WaitSetThreadDispatcher substitute{fixture.Registry, fixture.Scheduler.GetWakeFacade()};
        const auto prepared =
            fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));
        const bool resumed = prepared.HasValue() && fixture.BlockSignalAndResume();
        const WaitSetThreadDispatcherSnapshot exact = fixture.ThreadDispatcher.GetSnapshot();
        WaitSetThreadDispatcherSnapshot foreignInstance = exact;
        foreignInstance.Dispatcher = &substitute;
        WaitSetThreadDispatcherSnapshot foreignGeneration = exact;
        ++foreignGeneration.TransactionGeneration;
        // The SuspendPrepared receipt carries the generation that the syscall
        // transaction retains until its completion is consumed.
        const UInt64 retainedGeneration = prepared.HasValue() ? prepared.GetValue().Wait.TransactionGeneration : 0U;
        const WaitSetSystemCallDispatchError exactError =
            ValidateCompletionSnapshot(fixture.ThreadDispatcher, retainedGeneration, exact, fixture.Active);
        const WaitSetSystemCallDispatchError instanceError =
            ValidateCompletionSnapshot(fixture.ThreadDispatcher, retainedGeneration, foreignInstance, fixture.Active);
        const WaitSetSystemCallDispatchError generationError =
            ValidateCompletionSnapshot(fixture.ThreadDispatcher, retainedGeneration, foreignGeneration, fixture.Active);
        const WaitSetSystemCallDispatchError unretainedError =
            ValidateCompletionSnapshot(fixture.ThreadDispatcher, 0U, exact, fixture.Active);
        const auto consumed = fixture.Dispatcher.ConsumeCompletion(fixture.Active);

        ASTERIA_EXPECT(runner,
                       "completion validation binds the exact dispatcher and the "
                       "generation retained by the syscall transaction",
                       initialized, prepared.HasValue(), resumed, retainedGeneration != 0U,
                       exactError == WaitSetSystemCallDispatchError::None,
                       instanceError == WaitSetSystemCallDispatchError::DispatcherReceiptMismatch,
                       generationError == WaitSetSystemCallDispatchError::DispatcherReceiptMismatch,
                       unretainedError == WaitSetSystemCallDispatchError::DispatcherReceiptMismatch,
                       consumed.HasValue());
    }
}

void TestLateCompletionSurvivesUnavailableObservation(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const auto prepared = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));
    const WaitSetThreadDispatcherSnapshot observed = fixture.ThreadDispatcher.GetSnapshot();
    const bool resumed = prepared.HasValue() && (observed.State == WaitSetThreadDispatcherState::Prepared) &&
                         fixture.BlockSignalAndResume();
    const WaitSetSystemCallDispatchFailure unavailableFailure = RejectUnavailableCompletion(observed);
    const WaitSetThreadDispatcherSnapshot afterUnavailable = fixture.ThreadDispatcher.GetSnapshot();
    const auto consumed = fixture.Dispatcher.ConsumeCompletion(fixture.Active);

    ASTERIA_EXPECT(runner,
                   "a completion published after an unavailable observation remains "
                   "intact for the exact resumed consume",
                   initialized, prepared.HasValue(), resumed,
                   unavailableFailure.Error == WaitSetSystemCallDispatchError::DispatcherRejected,
                   unavailableFailure.Dispatcher.Error == WaitSetThreadDispatcherError::CompletionUnavailable,
                   afterUnavailable.State == WaitSetThreadDispatcherState::Completed,
                   afterUnavailable.TransactionGeneration == observed.TransactionGeneration, consumed.HasValue(),
                   consumed.GetValue().Response.Value0 == WaitTag);
}

void TestAuthorityLeaseEndsBeforeSuspension(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const auto prepared = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));
    const CapabilityError closed = fixture.Space.Close(fixture.WaitHandle);
    const ObjectRegistryError destroyed = fixture.Issuer.DestroyObject(fixture.WaitSetObject);
    const bool resumed = prepared.HasValue() &&
                         (prepared.GetValue().Disposition == WaitSetSystemCallDisposition::SuspendPrepared) &&
                         fixture.BlockSignalAndResume();
    const auto completed = fixture.Dispatcher.ConsumeCompletion(fixture.Active);
    const auto collected = fixture.Issuer.CollectDestroyedObject();

    ASTERIA_EXPECT(runner,
                   "the authority lease ends before suspension while exact dispatcher pins retain the transaction",
                   initialized, closed == CapabilityError::Ok, destroyed == ObjectRegistryError::Ok, resumed,
                   completed.HasValue(), completed.GetValue().Response.Value0 == WaitTag,
                   completed.GetValue().Response.Value1 == fixture.Registration.GetSequence(), collected.HasValue(),
                   collected.GetValue().Object.GetValue() == fixture.WaitSetObject.GetValue(),
                   collected.GetValue().Instance == &fixture.Set);
}

void TestSuspendResumeDoubleConsumeAndReuse(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const auto prepared = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));
    const bool resumed =
        prepared.HasValue() && (prepared.GetValue().Disposition == WaitSetSystemCallDisposition::SuspendPrepared) &&
        (prepared.GetValue().Wait.State == WaitSetThreadDispatcherState::Prepared) && fixture.BlockSignalAndResume();
    const auto completed = fixture.Dispatcher.ConsumeCompletion(fixture.Active);
    const auto duplicate = fixture.Dispatcher.ConsumeCompletion(fixture.Active);
    const NotificationError reuseSignal = fixture.Event.Signal();
    const auto reused = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));

    ASTERIA_EXPECT(runner, "an empty WaitSet prepares one explicit suspension", initialized, prepared.HasValue(),
                   prepared.GetValue().Disposition == WaitSetSystemCallDisposition::SuspendPrepared,
                   prepared.GetValue().Wait.State == WaitSetThreadDispatcherState::Prepared);
    ASTERIA_EXPECT(runner, "the scheduler block token, signal, wake, and next admission complete the retained wait",
                   resumed);
    ASTERIA_EXPECT(runner, "the resumed completion returns the exact tag and registration sequence",
                   completed.HasValue(), completed.GetValue().Disposition == WaitSetSystemCallDisposition::Completed,
                   completed.GetValue().Response.Error == SystemCallError::None,
                   completed.GetValue().Response.Value0 == WaitTag,
                   completed.GetValue().Response.Value1 == fixture.Registration.GetSequence());
    ASTERIA_EXPECT(runner, "the exact completion cannot be consumed twice", !duplicate.HasValue(),
                   duplicate.GetError().Error == WaitSetSystemCallDispatchError::DispatcherRejected,
                   duplicate.GetError().Dispatcher.Error == WaitSetThreadDispatcherError::CompletionUnavailable);
    ASTERIA_EXPECT(runner, "a consumed suspended transaction permits immediate dispatcher reuse",
                   reuseSignal == NotificationError::None, reused.HasValue(),
                   reused.GetValue().Response.Value0 == WaitTag,
                   reused.GetValue().Response.Value1 == fixture.Registration.GetSequence(),
                   fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

// A configured dispatcher turns argument 2 into a deadline of that many microseconds, and a wait
// that expires resumes with TimedOut and no tag.
void TestTimeout(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    fixture.Clock.Set(1000U);
    const bool configured = fixture.Dispatcher.ConfigureTimeouts(fixture.Clock, 3U) &&
                            !fixture.Dispatcher.ConfigureTimeouts(fixture.Clock, 3U);
    SystemCallRequest timed = MakeRequest(fixture.WaitHandle);
    timed.Arguments[2U] = 10U;
    const auto prepared = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, timed);
    const Foundation::UInt64 deadline = fixture.ThreadDispatcher.GetDeadline();
    const bool resumed = prepared.HasValue() &&
                         (prepared.GetValue().Disposition == WaitSetSystemCallDisposition::SuspendPrepared) &&
                         fixture.BlockExpireAndResume(deadline);
    const auto completed = fixture.Dispatcher.ConsumeCompletion(fixture.Active);
    const NotificationError signalled = fixture.Event.Signal();
    const auto immediate = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, timed);
    ASTERIA_EXPECT(
        runner, "a timed wait that expires resumes with TimedOut, and readiness still completes at once", initialized,
        configured, prepared.HasValue(), deadline == 1030U, resumed, completed.HasValue(),
        completed.GetValue().Response.Error == SystemCallError::TimedOut, completed.GetValue().Response.Value0 == 0U,
        completed.GetValue().Response.Value1 == 0U, signalled == NotificationError::None, immediate.HasValue(),
        immediate.GetValue().Response.Error == SystemCallError::None, immediate.GetValue().Response.Value0 == WaitTag,
        fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

// The killed thread resumes with an aborted completion, which it consumes like an expiry and
// recognizes by Aborted.
void TestAbortedWait(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const auto prepared = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));
    const bool resumed = prepared.HasValue() &&
                         (prepared.GetValue().Disposition == WaitSetSystemCallDisposition::SuspendPrepared) &&
                         fixture.BlockAbortAndResume();
    const auto completed = fixture.Dispatcher.ConsumeCompletion(fixture.Active);
    ASTERIA_EXPECT(runner, "an aborted wait is consumed as timed out and aborted", initialized, prepared.HasValue(),
                   resumed, completed.HasValue(), completed.GetValue().Response.Error == SystemCallError::TimedOut,
                   completed.GetValue().Wait.Aborted, completed.GetValue().Wait.TimedOut,
                   fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

// The run controller rolls back an admission whose run stop passed before
// entry (Finish(Ready)); the readmission that enters is a later run.
void TestResumeAfterRolledBackAdmission(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const auto prepared = fixture.Dispatcher.Dispatch(fixture.Space, fixture.Active, MakeRequest(fixture.WaitHandle));
    const UInt64 blockedRun = fixture.Active.RunSequence;
    const bool resumed = prepared.HasValue() &&
                         (prepared.GetValue().Disposition == WaitSetSystemCallDisposition::SuspendPrepared) &&
                         fixture.BlockSignalAndResume();
    const bool rolledBack =
        resumed && fixture.Scheduler.Finish(ThreadRunDisposition::Ready).HasValue() && fixture.DispatchNext();
    const auto completed = fixture.Dispatcher.ConsumeCompletion(fixture.Active);

    ASTERIA_EXPECT(runner, "a run after a rolled-back readmission consumes the completion", initialized, resumed,
                   rolledBack, fixture.Active.RunSequence == blockedRun + 2U, completed.HasValue(),
                   completed.GetValue().Response.Error == SystemCallError::None,
                   completed.GetValue().Response.Value0 == WaitTag,
                   completed.GetValue().Response.Value1 == fixture.Registration.GetSequence(),
                   fixture.ThreadDispatcher.GetSnapshot().State == WaitSetThreadDispatcherState::Idle);
}

void TestInvalidCompletedSnapshotIsTerminal(TestContext& runner) noexcept
{
    WaitSystemCallFixture fixture;
    const bool initialized = fixture.Initialize();
    const NotificationError signalled = fixture.Event.Signal();
    const auto immediate = fixture.ThreadDispatcher.Prepare(Kernel::Ipc::WaitSetThreadPrepareRequest{
        .WaitSet = WaitSetId{fixture.Registry, fixture.WaitSetObject},
        .Thread = fixture.ThreadIdentity,
        .SchedulingContext = fixture.ContextIdentity,
        .DeadlineTicks = 0U,
    });
    const auto rejected = fixture.Dispatcher.ConsumeCompletion(fixture.Active);
    const auto snapshot = fixture.ThreadDispatcher.GetSnapshot();

    ASTERIA_EXPECT(runner,
                   "a semantically invalid Completed snapshot is terminal without "
                   "discarding its receipt evidence",
                   initialized, signalled == NotificationError::None, immediate.HasValue(),
                   immediate.GetValue().State == WaitSetThreadDispatcherState::Completed, !rejected.HasValue(),
                   rejected.GetError().Error == WaitSetSystemCallDispatchError::DispatcherReceiptMismatch,
                   rejected.GetError().Dispatcher.Error == WaitSetThreadDispatcherError::ReceiptInvariantRejected,
                   snapshot.State == WaitSetThreadDispatcherState::Faulted,
                   snapshot.Notification.IsEqualTo(fixture.NotificationIdentity),
                   snapshot.RegistrationSequence == fixture.Registration.GetSequence(), snapshot.Tag == WaitTag);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"WaitSetSystemCallDispatcherTests"};
    Asteria::Test::TestNumberAndArguments(runner);
    Asteria::Test::TestCapabilityFailures(runner);
    Asteria::Test::TestDestroyedCapabilityObject(runner);
    Asteria::Test::TestActiveBindingMismatch(runner);
    Asteria::Test::TestImmediateReadyAndReuse(runner);
    Asteria::Test::TestDispatcherAndGenerationBinding(runner);
    Asteria::Test::TestLateCompletionSurvivesUnavailableObservation(runner);
    Asteria::Test::TestAuthorityLeaseEndsBeforeSuspension(runner);
    Asteria::Test::TestSuspendResumeDoubleConsumeAndReuse(runner);
    Asteria::Test::TestResumeAfterRolledBackAdmission(runner);
    Asteria::Test::TestTimeout(runner);
    Asteria::Test::TestAbortedWait(runner);
    Asteria::Test::TestInvalidCompletedSnapshotIsTerminal(runner);
    return runner.Finish();
}
