// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Ipc/ReplyToken.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::UInt64;
using Kernel::Ipc::ReplyToken;
using Kernel::Ipc::ReplyTokenError;
using Kernel::Ipc::ReplyTokenFailure;
using Kernel::Ipc::ReplyTokenState;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
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
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingContextState;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Platform::Synchronization::SpinLock;

[[nodiscard]] constexpr SchedulingContextConfiguration Configuration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool FailureIs(const ReplyTokenFailure& failure, const ReplyTokenError error,
                             const SchedulingContextError schedulingError = SchedulingContextError::None,
                             const ObjectPinError pinError = ObjectPinError::Ok) noexcept
{
    bool reservedZero{true};
    for (const Foundation::UInt8 value : failure.Reserved)
    {
        reservedZero = reservedZero && (value == 0U);
    }
    return (failure.Error == error) && (failure.PinError == pinError) && (failure.SchedulingError == schedulingError) &&
           reservedZero;
}

[[nodiscard]] bool SnapshotsEqual(const Kernel::Ipc::ReplyTokenSnapshot& left,
                                  const Kernel::Ipc::ReplyTokenSnapshot& right) noexcept
{
    bool reservedEqual{true};
    for (Foundation::Size index{0U}; index < sizeof(left.Reserved); ++index)
    {
        reservedEqual = reservedEqual && (left.Reserved[index] == right.Reserved[index]);
    }
    return left.Caller.IsEqualTo(right.Caller) && left.SchedulingContext.IsEqualTo(right.SchedulingContext) &&
           left.Server.IsEqualTo(right.Server) && left.BlockToken.IsEqualTo(right.BlockToken) &&
           (left.TransactionIdentifier == right.TransactionIdentifier) && (left.State == right.State) && reservedEqual;
}

class ReplyFixture final
{
public:
    constexpr ReplyFixture() noexcept : Registry{Synchronization}, Issuer{Registry}, Context{Registry}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        if ((Caller.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (Server.Initialize(ResourceDomainId{8U}) != ThreadError::None) ||
            (Other.Initialize(ResourceDomainId{9U}) != ThreadError::None) ||
            (Context.Initialize(Configuration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }

        const auto callerResult = Issuer.CreateObject(Caller);
        const auto serverResult = Issuer.CreateObject(Server);
        const auto otherResult = Issuer.CreateObject(Other);
        const auto contextResult = Issuer.CreateObject(Context);
        if (!callerResult.HasValue() || !serverResult.HasValue() || !otherResult.HasValue() ||
            !contextResult.HasValue())
        {
            return false;
        }

        CallerObject = callerResult.GetValue();
        ServerObject = serverResult.GetValue();
        OtherObject = otherResult.GetValue();
        ContextObject = contextResult.GetValue();
        CallerId = ThreadId{Registry, CallerObject};
        ServerId = ThreadId{Registry, ServerObject};
        OtherId = ThreadId{Registry, OtherObject};
        ContextId = SchedulingContextId{Registry, ContextObject};
        if ((Context.Bind(CallerId) != SchedulingContextError::None) || (Caller.MakeReady() != ThreadError::None) ||
            (Caller.BeginRun(CpuId{0U}) != ThreadError::None))
        {
            return false;
        }
        const auto blocked = Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        if (!blocked.HasValue())
        {
            return false;
        }
        BlockToken = blocked.GetValue().BlockToken;
        return BlockToken.IsValid();
    }

    [[nodiscard]] bool ReblockCaller() noexcept
    {
        if ((Caller.Wake(BlockToken) != ThreadError::None) || (Caller.BeginRun(CpuId{0U}) != ThreadError::None))
        {
            return false;
        }
        const auto blocked = Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        if (!blocked.HasValue())
        {
            return false;
        }
        BlockToken = blocked.GetValue().BlockToken;
        return BlockToken.IsValid();
    }

    [[nodiscard]] bool Retire() noexcept
    {
        const auto contextSnapshot = Context.GetSnapshot();
        const auto callerSnapshot = Caller.GetSnapshot();
        if (contextSnapshot.State == SchedulingContextState::Running || contextSnapshot.DonationDepth != 0U)
        {
            return false;
        }
        if (Context.Unbind(CallerId) != SchedulingContextError::None)
        {
            return false;
        }
        if ((callerSnapshot.State == Kernel::Scheduler::ThreadState::Blocked) &&
            (Caller.Wake(callerSnapshot.BlockToken) != ThreadError::None))
        {
            return false;
        }
        if (((callerSnapshot.State != Kernel::Scheduler::ThreadState::Blocked) &&
             (callerSnapshot.State != Kernel::Scheduler::ThreadState::Ready)) ||
            (Caller.Terminate(0U) != ThreadError::None) || (Server.Terminate(0U) != ThreadError::None) ||
            (Other.Terminate(0U) != ThreadError::None))
        {
            return false;
        }
        if ((Issuer.DestroyObject(ContextObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(CallerObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(ServerObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(OtherObject) != ObjectRegistryError::Ok))
        {
            return false;
        }
        for (Foundation::UInt8 index{0U}; index < 4U; ++index)
        {
            if (!Issuer.CollectDestroyedObject().HasValue())
            {
                return false;
            }
        }
        return true;
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    Thread Caller;
    Thread Server;
    Thread Other;
    SchedulingContext Context;
    ObjectId CallerObject{};
    ObjectId ServerObject{};
    ObjectId OtherObject{};
    ObjectId ContextObject{};
    ThreadId CallerId{};
    ThreadId ServerId{};
    ThreadId OtherId{};
    SchedulingContextId ContextId{};
    Kernel::Scheduler::ThreadBlockToken BlockToken{};
};

[[nodiscard]] ReplyTokenFailure InitializeToken(ReplyToken& token, ReplyFixture& fixture,
                                                const UInt64 transaction = 0x1122'3344'5566'7788ULL) noexcept
{
    return token.Initialize(fixture.Registry, ResourceDomainId{7U}, fixture.CallerId, fixture.ContextId,
                            fixture.BlockToken, transaction);
}

void TestInitializationFailures(TestContext& runner) noexcept
{
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "initialization fixture is ready", fixture.Initialize());

    ReplyToken token;
    const ReplyTokenFailure invalidOwner = token.Initialize(fixture.Registry, ResourceDomainId{}, fixture.CallerId,
                                                            fixture.ContextId, fixture.BlockToken, 1U);
    const ReplyTokenFailure invalidTransaction = token.Initialize(
        fixture.Registry, ResourceDomainId{7U}, fixture.CallerId, fixture.ContextId, fixture.BlockToken, 0U);
    const ReplyTokenFailure wrongCallerType =
        token.Initialize(fixture.Registry, ResourceDomainId{7U}, ThreadId{fixture.Registry, fixture.ContextObject},
                         fixture.ContextId, fixture.BlockToken, 1U);
    ASTERIA_EXPECT(runner, "invalid initialization is failure-atomic",
                   FailureIs(invalidOwner, ReplyTokenError::InvalidConfiguration),
                   FailureIs(invalidTransaction, ReplyTokenError::InvalidTransaction),
                   FailureIs(wrongCallerType, ReplyTokenError::CallerTypeMismatch, SchedulingContextError::None,
                             ObjectPinError::TypeMismatch),
                   !token.IsInitialized(), !token.GetChargeOwner().IsValid());

    SpinLock otherSynchronization;
    Asteria::TestSupport::TestObjectRegistry otherRegistry{otherSynchronization};
    const ReplyTokenFailure wrongCallerRegistry =
        token.Initialize(fixture.Registry, ResourceDomainId{7U}, ThreadId{otherRegistry, fixture.CallerObject},
                         fixture.ContextId, fixture.BlockToken, 1U);
    const ReplyTokenFailure wrongContextRegistry =
        token.Initialize(fixture.Registry, ResourceDomainId{7U}, fixture.CallerId,
                         SchedulingContextId{otherRegistry, fixture.ContextObject}, fixture.BlockToken, 1U);
    ASTERIA_EXPECT(runner, "registry provenance is exact for both retained identities",
                   FailureIs(wrongCallerRegistry, ReplyTokenError::CallerRegistryMismatch),
                   FailureIs(wrongContextRegistry, ReplyTokenError::SchedulingContextRegistryMismatch));

    const Kernel::Scheduler::ThreadBlockToken stale = fixture.BlockToken;
    ASTERIA_EXPECT(runner, "caller obtains a later block generation", fixture.ReblockCaller());
    const ReplyTokenFailure staleBlock =
        token.Initialize(fixture.Registry, ResourceDomainId{7U}, fixture.CallerId, fixture.ContextId, stale, 1U);
    ASTERIA_EXPECT(runner, "stale caller block generation is rejected without publication",
                   FailureIs(staleBlock, ReplyTokenError::InvalidBlockToken), !token.IsInitialized());

    ASTERIA_EXPECT(runner, "fixture creates a different current donation endpoint",
                   fixture.Context.Donate(fixture.CallerId, fixture.ServerId) == SchedulingContextError::None);
    const ReplyTokenFailure mismatchedContext = InitializeToken(token, fixture);
    ASTERIA_EXPECT(runner, "caller must be the context donation-chain endpoint",
                   FailureIs(mismatchedContext, ReplyTokenError::SchedulingContextCallerMismatch),
                   fixture.Context.ReturnDonation(fixture.ServerId) == SchedulingContextError::None);

    ASTERIA_EXPECT(runner, "initialization fixture retires cleanly", fixture.Retire());
}

void TestStateAndIdentityErrors(TestContext& runner) noexcept
{
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "state-matrix fixture is ready", fixture.Initialize());

    SpinLock otherSynchronization;
    Asteria::TestSupport::TestObjectRegistry otherRegistry{otherSynchronization};
    {
        ReplyToken token;
        const auto freshReply = token.CompleteReply(fixture.ServerId, 1U);
        ASTERIA_EXPECT(runner, "fresh token rejects accept, reply, and cancellation",
                       FailureIs(token.Accept(fixture.ServerId), ReplyTokenError::NotInitialized),
                       !freshReply.HasValue(), FailureIs(freshReply.GetError(), ReplyTokenError::NotInitialized),
                       !token.Cancel().HasValue());

        const ObjectId absentObject{(Foundation::UInt64{1U} << 16U) | 256U};
        const ReplyTokenFailure invalidCaller = token.Initialize(fixture.Registry, ResourceDomainId{7U}, ThreadId{},
                                                                 fixture.ContextId, fixture.BlockToken, 1U);
        const ReplyTokenFailure missingCaller =
            token.Initialize(fixture.Registry, ResourceDomainId{7U}, ThreadId{fixture.Registry, absentObject},
                             fixture.ContextId, fixture.BlockToken, 1U);
        const ReplyTokenFailure invalidContext = token.Initialize(
            fixture.Registry, ResourceDomainId{7U}, fixture.CallerId, SchedulingContextId{}, fixture.BlockToken, 1U);
        const ReplyTokenFailure missingContext =
            token.Initialize(fixture.Registry, ResourceDomainId{7U}, fixture.CallerId,
                             SchedulingContextId{fixture.Registry, absentObject}, fixture.BlockToken, 1U);
        const ReplyTokenFailure wrongContextType =
            token.Initialize(fixture.Registry, ResourceDomainId{7U}, fixture.CallerId,
                             SchedulingContextId{fixture.Registry, fixture.CallerObject}, fixture.BlockToken, 1U);
        const ReplyTokenFailure invalidBlock =
            token.Initialize(fixture.Registry, ResourceDomainId{7U}, fixture.CallerId, fixture.ContextId,
                             Kernel::Scheduler::ThreadBlockToken{}, 1U);
        ASTERIA_EXPECT(runner, "identity and block validation preserve a fresh token",
                       FailureIs(invalidCaller, ReplyTokenError::InvalidCaller),
                       FailureIs(missingCaller, ReplyTokenError::CallerNotFound, SchedulingContextError::None,
                                 ObjectPinError::ObjectNotFound),
                       FailureIs(invalidContext, ReplyTokenError::InvalidSchedulingContext),
                       FailureIs(missingContext, ReplyTokenError::SchedulingContextNotFound,
                                 SchedulingContextError::None, ObjectPinError::ObjectNotFound),
                       FailureIs(wrongContextType, ReplyTokenError::SchedulingContextTypeMismatch,
                                 SchedulingContextError::None, ObjectPinError::TypeMismatch),
                       FailureIs(invalidBlock, ReplyTokenError::InvalidBlockToken), !token.IsInitialized());

        ASTERIA_EXPECT(runner, "state-matrix caller leaves Blocked temporarily",
                       fixture.Caller.Wake(fixture.BlockToken) == ThreadError::None);
        const ReplyTokenFailure callerNotBlocked = InitializeToken(token, fixture);
        const auto blockedAgain = (fixture.Caller.BeginRun(CpuId{0U}) == ThreadError::None)
                                      ? fixture.Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked)
                                      : Foundation::Result<Kernel::Scheduler::ThreadSnapshot, ThreadError>::Failure(
                                            ThreadError::InvalidState);
        if (blockedAgain.HasValue())
        {
            fixture.BlockToken = blockedAgain.GetValue().BlockToken;
        }
        ASTERIA_EXPECT(runner, "caller must remain blocked on the correlated generation",
                       FailureIs(callerNotBlocked, ReplyTokenError::CallerNotBlocked), blockedAgain.HasValue());

        const bool contextStarted = fixture.Context.StartRun(fixture.CallerId, CpuId{0U}, MonotonicTime{1U}).HasValue();
        const ReplyTokenFailure contextRunning = InitializeToken(token, fixture);
        const bool contextStopped = contextStarted && fixture.Context.EndRun(CpuId{0U}, MonotonicTime{21U}).HasValue();
        ASTERIA_EXPECT(runner, "a running scheduling context cannot enter a call token", contextStopped,
                       FailureIs(contextRunning, ReplyTokenError::SchedulingContextNotReady));

        ASTERIA_EXPECT(runner, "budget-exhausted blocked caller can initialize after failures",
                       FailureIs(InitializeToken(token, fixture), ReplyTokenError::None),
                       fixture.Context.GetSnapshot().State == SchedulingContextState::BudgetExhausted);
        const auto pendingSnapshot = token.GetSnapshot();
        const ReplyTokenFailure secondInitialize = InitializeToken(token, fixture, 2U);
        const ReplyTokenFailure invalidServer = token.Accept(ThreadId{});
        const ReplyTokenFailure wrongRegistry = token.Accept(ThreadId{otherRegistry, fixture.ServerObject});
        const ReplyTokenFailure callerCycle = token.Accept(fixture.CallerId);
        ASTERIA_EXPECT(runner, "pending state rejects duplicate and invalid server operations atomically",
                       FailureIs(secondInitialize, ReplyTokenError::AlreadyInitialized),
                       FailureIs(invalidServer, ReplyTokenError::InvalidServer),
                       FailureIs(wrongRegistry, ReplyTokenError::ServerRegistryMismatch),
                       FailureIs(callerCycle, ReplyTokenError::DonationRejected, SchedulingContextError::DonationCycle),
                       SnapshotsEqual(pendingSnapshot, token.GetSnapshot()));

        ASTERIA_EXPECT(runner, "accepted token cannot acquire a second server",
                       FailureIs(token.Accept(fixture.ServerId), ReplyTokenError::None),
                       FailureIs(token.Accept(fixture.OtherId), ReplyTokenError::AlreadyAccepted));
        const auto cancelled = token.Cancel();
        ASTERIA_EXPECT(runner, "state-matrix token cancels and wakes its exact caller", cancelled.HasValue(),
                       fixture.Caller.Wake(cancelled.GetValue().BlockToken) == ThreadError::None);
    }

    ASTERIA_EXPECT(runner, "state-matrix fixture retires cleanly", fixture.Retire());
}

void TestPreconfiguredArm(TestContext& runner) noexcept
{
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "preconfigured-arm fixture is ready", fixture.Initialize());

    {
        ReplyToken token;
        const ReplyTokenFailure invalidConfiguration = token.Configure(ResourceDomainId{});
        const ReplyTokenFailure configured = token.Configure(ResourceDomainId{7U});
        const auto registered = fixture.Issuer.CreateObject(token);
        ASTERIA_EXPECT(runner, "an available token publishes ownership before registration",
                       FailureIs(invalidConfiguration, ReplyTokenError::InvalidConfiguration),
                       FailureIs(configured, ReplyTokenError::None), token.IsInitialized(),
                       token.GetSnapshot().State == ReplyTokenState::Available, token.GetChargeOwner().GetValue() == 7U,
                       registered.HasValue());

        const ReplyTokenFailure invalidArm =
            token.Arm(fixture.Registry, fixture.CallerId, fixture.ContextId, fixture.BlockToken, 0U);
        const ReplyTokenFailure armed = token.Arm(fixture.Registry, fixture.CallerId, fixture.ContextId,
                                                  fixture.BlockToken, 0x1122'3344'5566'7788ULL);
        ASTERIA_EXPECT(runner, "failed Arm preserves Available and successful Arm retains ownership",
                       FailureIs(invalidArm, ReplyTokenError::InvalidTransaction),
                       FailureIs(armed, ReplyTokenError::None), token.GetSnapshot().State == ReplyTokenState::Pending,
                       token.GetChargeOwner().GetValue() == 7U);

        const auto cancelled = token.Cancel();
        const bool completed =
            cancelled.HasValue() && (fixture.Caller.Wake(cancelled.GetValue().BlockToken) == ThreadError::None);
        const bool retired = registered.HasValue() &&
                             (fixture.Issuer.DestroyObject(registered.GetValue()) == ObjectRegistryError::Ok) &&
                             fixture.Issuer.CollectDestroyedObject().HasValue();
        ASTERIA_EXPECT(runner, "an armed preconfigured token cancels, wakes, and retires normally", completed, retired);
    }

    ASTERIA_EXPECT(runner, "preconfigured-arm fixture retires cleanly", fixture.Retire());
}

void TestReplyLifecycle(TestContext& runner) noexcept
{
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "reply fixture is ready", fixture.Initialize());

    {
        ReplyToken token;
        ASTERIA_EXPECT(runner, "initialization publishes one typed pending token",
                       FailureIs(InitializeToken(token, fixture), ReplyTokenError::None), token.IsInitialized(),
                       token.GetObjectType() == ObjectType::ReplyToken, token.GetChargeOwner().GetValue() == 7U);

        const auto tokenObject = fixture.Issuer.CreateObject(token);
        ASTERIA_EXPECT(runner, "reply token registers as a kernel object", tokenObject.HasValue());

        const auto premature = token.CompleteReply(fixture.ServerId, 1U);
        ASTERIA_EXPECT(runner, "reply requires a prior server acceptance", !premature.HasValue(),
                       FailureIs(premature.GetError(), ReplyTokenError::NotAccepted));

        const ReplyTokenFailure wrongServerType = token.Accept(ThreadId{fixture.Registry, fixture.ContextObject});
        ASTERIA_EXPECT(
            runner, "failed server pinning leaves the token pending",
            FailureIs(wrongServerType, ReplyTokenError::DonationRejected, SchedulingContextError::ThreadTypeMismatch),
            token.GetSnapshot().State == ReplyTokenState::Pending);

        ASTERIA_EXPECT(runner, "one server accepts the call and receives the budget",
                       FailureIs(token.Accept(fixture.ServerId), ReplyTokenError::None));
        const auto donated = fixture.Context.GetSnapshot();
        ASTERIA_EXPECT(runner, "accepted state and donation-chain endpoint agree",
                       donated.CurrentThread.IsEqualTo(fixture.ServerId), donated.DonationDepth == 1U,
                       token.GetSnapshot().State == ReplyTokenState::Accepted);

        const auto wrongReply = token.CompleteReply(fixture.OtherId, 0x1122'3344'5566'7788ULL);
        ASTERIA_EXPECT(runner, "a different server cannot consume the token", !wrongReply.HasValue(),
                       FailureIs(wrongReply.GetError(), ReplyTokenError::WrongServer),
                       token.GetSnapshot().State == ReplyTokenState::Accepted);

        const auto wrongTransaction = token.CompleteReply(fixture.ServerId, 0x1122'3344'5566'7789ULL);
        ASTERIA_EXPECT(runner, "a mismatched reply transaction cannot consume the token", !wrongTransaction.HasValue(),
                       FailureIs(wrongTransaction.GetError(), ReplyTokenError::TransactionMismatch),
                       token.GetSnapshot().State == ReplyTokenState::Accepted);

        const auto reply = token.CompleteReply(fixture.ServerId, 0x1122'3344'5566'7788ULL);
        ASTERIA_EXPECT(runner, "reply returns the exact caller wake identity and transaction", reply.HasValue(),
                       reply.GetValue().Caller.IsEqualTo(fixture.CallerId),
                       reply.GetValue().BlockToken.IsEqualTo(fixture.BlockToken),
                       reply.GetValue().TransactionIdentifier == 0x1122'3344'5566'7788ULL,
                       reply.GetValue().State == ReplyTokenState::Replied);
        const auto returned = fixture.Context.GetSnapshot();
        ASTERIA_EXPECT(runner, "successful reply returns donation before terminal publication",
                       returned.CurrentThread.IsEqualTo(fixture.CallerId), returned.DonationDepth == 0U,
                       token.GetSnapshot().State == ReplyTokenState::Replied);

        ASTERIA_EXPECT(runner, "a consumed reply token cannot be replayed or cancelled",
                       !token.CompleteReply(fixture.ServerId, 0x1122'3344'5566'7788ULL).HasValue(),
                       !token.Cancel().HasValue());

        ASTERIA_EXPECT(runner, "the retained completion remains valid through caller wake", reply.HasValue(),
                       fixture.Caller.Wake(reply.GetValue().BlockToken) == ThreadError::None);
        ASTERIA_EXPECT(runner, "completed reply token is independently collectable", tokenObject.HasValue(),
                       fixture.Issuer.DestroyObject(tokenObject.GetValue()) == ObjectRegistryError::Ok,
                       fixture.Issuer.CollectDestroyedObject().HasValue());
    }
    ASTERIA_EXPECT(runner, "reply fixture retires cleanly", fixture.Retire());
}

void TestCancellationAndReturnFailure(TestContext& runner) noexcept
{
    {
        ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "pending-cancel fixture is ready", fixture.Initialize());
        {
            ReplyToken token;
            const auto cancelled =
                FailureIs(InitializeToken(token, fixture), ReplyTokenError::None)
                    ? token.Cancel()
                    : Foundation::Result<Kernel::Ipc::ReplyTokenCompletion, ReplyTokenFailure>::Failure(
                          ReplyTokenFailure{});
            ASTERIA_EXPECT(runner, "pending cancellation consumes without creating a donation", cancelled.HasValue(),
                           cancelled.GetValue().State == ReplyTokenState::Cancelled,
                           cancelled.GetValue().Caller.IsEqualTo(fixture.CallerId),
                           token.GetSnapshot().State == ReplyTokenState::Cancelled);
            ASTERIA_EXPECT(runner, "pending cancellation retains its caller through wake", cancelled.HasValue(),
                           fixture.Caller.Wake(cancelled.GetValue().BlockToken) == ThreadError::None);
        }
        ASTERIA_EXPECT(runner, "pending-cancel fixture retires cleanly", fixture.Retire());
    }

    {
        ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "accepted-cancel fixture is ready", fixture.Initialize());
        {
            ReplyToken token;
            ASTERIA_EXPECT(runner, "accepted-cancel fixture donates once",
                           FailureIs(InitializeToken(token, fixture), ReplyTokenError::None),
                           FailureIs(token.Accept(fixture.ServerId), ReplyTokenError::None));
            const auto cancelled = token.Cancel();
            ASTERIA_EXPECT(runner, "accepted cancellation returns the donation before completion", cancelled.HasValue(),
                           cancelled.GetValue().State == ReplyTokenState::Cancelled,
                           fixture.Context.GetSnapshot().CurrentThread.IsEqualTo(fixture.CallerId));
            ASTERIA_EXPECT(runner, "accepted cancellation retains its caller through wake", cancelled.HasValue(),
                           fixture.Caller.Wake(cancelled.GetValue().BlockToken) == ThreadError::None);
        }
        ASTERIA_EXPECT(runner, "accepted-cancel fixture retires cleanly", fixture.Retire());
    }

    {
        ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "return-failure fixture is ready", fixture.Initialize());
        {
            ReplyToken token;
            ASTERIA_EXPECT(runner, "server starts one donated run",
                           FailureIs(InitializeToken(token, fixture), ReplyTokenError::None),
                           FailureIs(token.Accept(fixture.ServerId), ReplyTokenError::None),
                           fixture.Context.StartRun(fixture.ServerId, CpuId{0U}, MonotonicTime{1U}).HasValue());
            const auto rejected = token.CompleteReply(fixture.ServerId, 0x1122'3344'5566'7788ULL);
            ASTERIA_EXPECT(
                runner, "running-server reply failure preserves the accepted donation", !rejected.HasValue(),
                FailureIs(rejected.GetError(), ReplyTokenError::ReturnRejected, SchedulingContextError::AlreadyRunning),
                token.GetSnapshot().State == ReplyTokenState::Accepted);
            const auto rejectedCancellation = token.Cancel();
            ASTERIA_EXPECT(runner, "running-server cancellation also preserves the donation",
                           !rejectedCancellation.HasValue(),
                           FailureIs(rejectedCancellation.GetError(), ReplyTokenError::ReturnRejected,
                                     SchedulingContextError::AlreadyRunning),
                           token.GetSnapshot().State == ReplyTokenState::Accepted);
            const auto completed =
                fixture.Context.EndRun(CpuId{0U}, MonotonicTime{2U}).HasValue()
                    ? token.CompleteReply(fixture.ServerId, 0x1122'3344'5566'7788ULL)
                    : Foundation::Result<Kernel::Ipc::ReplyTokenCompletion, ReplyTokenFailure>::Failure(
                          ReplyTokenFailure{});
            ASTERIA_EXPECT(runner, "the same token completes and wakes after the server run stops",
                           completed.HasValue(),
                           fixture.Caller.Wake(completed.GetValue().BlockToken) == ThreadError::None);
        }
        ASTERIA_EXPECT(runner, "return-failure fixture retires cleanly", fixture.Retire());
    }
}

void TestTerminalPinRetention(TestContext& runner) noexcept
{
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "terminal-retention fixture is ready", fixture.Initialize());

    bool terminalPrepared{false};
    {
        ReplyToken token;
        const auto cancelled = FailureIs(InitializeToken(token, fixture), ReplyTokenError::None)
                                   ? token.Cancel()
                                   : Foundation::Result<Kernel::Ipc::ReplyTokenCompletion, ReplyTokenFailure>::Failure(
                                         ReplyTokenFailure{});
        terminalPrepared = cancelled.HasValue() &&
                           (fixture.Caller.Wake(cancelled.GetValue().BlockToken) == ThreadError::None) &&
                           (fixture.Context.Unbind(fixture.CallerId) == SchedulingContextError::None) &&
                           (fixture.Caller.Terminate(0U) == ThreadError::None) &&
                           (fixture.Issuer.DestroyObject(fixture.CallerObject) == ObjectRegistryError::Ok) &&
                           (fixture.Issuer.DestroyObject(fixture.ContextObject) == ObjectRegistryError::Ok);
        const auto prematureCollection = fixture.Issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(runner, "terminal token pins retain caller and context through wake use", terminalPrepared,
                       !prematureCollection.HasValue(),
                       prematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    }

    bool collectedRetainedObjects{terminalPrepared};
    for (Foundation::UInt8 index{0U}; index < 2U; ++index)
    {
        collectedRetainedObjects = collectedRetainedObjects && fixture.Issuer.CollectDestroyedObject().HasValue();
    }
    const bool retiredRemainingObjects =
        (fixture.Server.Terminate(0U) == ThreadError::None) && (fixture.Other.Terminate(0U) == ThreadError::None) &&
        (fixture.Issuer.DestroyObject(fixture.ServerObject) == ObjectRegistryError::Ok) &&
        (fixture.Issuer.DestroyObject(fixture.OtherObject) == ObjectRegistryError::Ok) &&
        fixture.Issuer.CollectDestroyedObject().HasValue() && fixture.Issuer.CollectDestroyedObject().HasValue();
    ASTERIA_EXPECT(runner, "terminal pin-retention fixture retires after token destruction", collectedRetainedObjects,
                   retiredRemainingObjects);
}

void TestNestedDonation(TestContext& runner) noexcept
{
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "nested-donation fixture is ready", fixture.Initialize());

    {
        ReplyToken outerToken;
        ASTERIA_EXPECT(runner, "outer call donates the caller context to its first server",
                       FailureIs(InitializeToken(outerToken, fixture), ReplyTokenError::None),
                       FailureIs(outerToken.Accept(fixture.ServerId), ReplyTokenError::None));

        const bool serverBlocked = (fixture.Server.MakeReady() == ThreadError::None) &&
                                   (fixture.Server.BeginRun(CpuId{0U}) == ThreadError::None);
        const auto serverBlock = serverBlocked
                                     ? fixture.Server.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked)
                                     : Foundation::Result<Kernel::Scheduler::ThreadSnapshot, ThreadError>::Failure(
                                           ThreadError::InvalidState);
        ASTERIA_EXPECT(runner, "first server blocks while making one nested call", serverBlock.HasValue());

        {
            ReplyToken innerToken;
            const ReplyTokenFailure innerInitialized =
                serverBlock.HasValue()
                    ? innerToken.Initialize(fixture.Registry, ResourceDomainId{8U}, fixture.ServerId, fixture.ContextId,
                                            serverBlock.GetValue().BlockToken, 0x8877'6655'4433'2211ULL)
                    : ReplyTokenFailure{};
            ASTERIA_EXPECT(runner, "inner call appends one exact nested donation edge",
                           FailureIs(innerInitialized, ReplyTokenError::None),
                           FailureIs(innerToken.Accept(fixture.OtherId), ReplyTokenError::None),
                           fixture.Context.GetSnapshot().DonationDepth == 2U);

            const auto innerReply = innerToken.CompleteReply(fixture.OtherId, 0x8877'6655'4433'2211ULL);
            ASTERIA_EXPECT(runner, "inner reply returns only the newest edge and wakes its server",
                           innerReply.HasValue(), fixture.Context.GetSnapshot().DonationDepth == 1U,
                           fixture.Context.GetSnapshot().CurrentThread.IsEqualTo(fixture.ServerId),
                           fixture.Server.Wake(innerReply.GetValue().BlockToken) == ThreadError::None);
        }

        const auto outerReply = outerToken.CompleteReply(fixture.ServerId, 0x1122'3344'5566'7788ULL);
        ASTERIA_EXPECT(runner, "outer reply restores and wakes the original caller", outerReply.HasValue(),
                       fixture.Context.GetSnapshot().DonationDepth == 0U,
                       fixture.Context.GetSnapshot().CurrentThread.IsEqualTo(fixture.CallerId),
                       fixture.Caller.Wake(outerReply.GetValue().BlockToken) == ThreadError::None);
    }

    ASTERIA_EXPECT(runner, "nested-donation fixture retires cleanly", fixture.Retire());
}

void TestConcurrentCompletion(TestContext& runner) noexcept
{
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "concurrent fixture is ready", fixture.Initialize());
    {
        ReplyToken token;
        ASTERIA_EXPECT(runner, "concurrent fixture reaches accepted state",
                       FailureIs(InitializeToken(token, fixture), ReplyTokenError::None),
                       FailureIs(token.Accept(fixture.ServerId), ReplyTokenError::None));

        std::atomic<bool> start{false};
        std::atomic<Foundation::UInt8> ready{0U};
        std::atomic<Foundation::UInt8> completedWorkers{0U};
        std::atomic<Foundation::Size> observations{0U};
        std::atomic<Foundation::Size> observerFailures{0U};
        bool replySucceeded{false};
        bool cancelSucceeded{false};
        ReplyTokenError replyError{ReplyTokenError::None};
        ReplyTokenError cancelError{ReplyTokenError::None};
        std::thread replyThread{[&]() noexcept
                                {
                                    static_cast<void>(ready.fetch_add(1U, std::memory_order_acq_rel));
                                    bool released{false};
                                    for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
                                    {
                                        if (start.load(std::memory_order_acquire))
                                        {
                                            released = true;
                                            break;
                                        }
                                        std::this_thread::yield();
                                    }
                                    if (!released)
                                    {
                                        replyError = ReplyTokenError::InvalidConfiguration;
                                        static_cast<void>(completedWorkers.fetch_add(1U, std::memory_order_release));
                                        return;
                                    }
                                    const auto result = token.CompleteReply(fixture.ServerId, 0x1122'3344'5566'7788ULL);
                                    replySucceeded = result.HasValue();
                                    if (!result.HasValue())
                                    {
                                        replyError = result.GetError().Error;
                                    }
                                    static_cast<void>(completedWorkers.fetch_add(1U, std::memory_order_release));
                                }};
        std::thread cancelThread{[&]() noexcept
                                 {
                                     static_cast<void>(ready.fetch_add(1U, std::memory_order_acq_rel));
                                     bool released{false};
                                     for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
                                     {
                                         if (start.load(std::memory_order_acquire))
                                         {
                                             released = true;
                                             break;
                                         }
                                         std::this_thread::yield();
                                     }
                                     if (!released)
                                     {
                                         cancelError = ReplyTokenError::InvalidConfiguration;
                                         static_cast<void>(completedWorkers.fetch_add(1U, std::memory_order_release));
                                         return;
                                     }
                                     const auto result = token.Cancel();
                                     cancelSucceeded = result.HasValue();
                                     if (!result.HasValue())
                                     {
                                         cancelError = result.GetError().Error;
                                     }
                                     static_cast<void>(completedWorkers.fetch_add(1U, std::memory_order_release));
                                 }};
        std::thread observerThread{
            [&]() noexcept
            {
                static_cast<void>(ready.fetch_add(1U, std::memory_order_acq_rel));
                bool released{false};
                for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
                {
                    if (start.load(std::memory_order_acquire))
                    {
                        released = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (!released)
                {
                    static_cast<void>(observerFailures.fetch_add(1U, std::memory_order_relaxed));
                    return;
                }

                for (Foundation::Size attempt{0U}; attempt < 1'000'000U; ++attempt)
                {
                    const auto snapshot = token.GetSnapshot();
                    static_cast<void>(observations.fetch_add(1U, std::memory_order_relaxed));
                    const bool knownState = (snapshot.State == ReplyTokenState::Accepted) ||
                                            (snapshot.State == ReplyTokenState::Replied) ||
                                            (snapshot.State == ReplyTokenState::Cancelled);
                    if (!token.IsInitialized() || !knownState || !snapshot.Caller.IsEqualTo(fixture.CallerId) ||
                        !snapshot.SchedulingContext.IsEqualTo(fixture.ContextId) ||
                        !snapshot.Server.IsEqualTo(fixture.ServerId) ||
                        !snapshot.BlockToken.IsEqualTo(fixture.BlockToken) ||
                        (snapshot.TransactionIdentifier != 0x1122'3344'5566'7788ULL))
                    {
                        static_cast<void>(observerFailures.fetch_add(1U, std::memory_order_relaxed));
                    }
                    if (completedWorkers.load(std::memory_order_acquire) == 2U)
                    {
                        break;
                    }
                    std::this_thread::yield();
                }
            }};
        bool bothReady{false};
        for (Foundation::Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (ready.load(std::memory_order_acquire) == 3U)
            {
                bothReady = true;
                break;
            }
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        replyThread.join();
        cancelThread.join();
        observerThread.join();

        const bool oneSuccess = replySucceeded != cancelSucceeded;
        const ReplyTokenError loserError = replySucceeded ? cancelError : replyError;
        const ReplyTokenState finalState = token.GetSnapshot().State;
        ASTERIA_EXPECT(runner, "concurrent reply and cancellation have exactly one terminal winner", bothReady,
                       oneSuccess, loserError == ReplyTokenError::AlreadyCompleted,
                       observations.load(std::memory_order_relaxed) != 0U,
                       observerFailures.load(std::memory_order_relaxed) == 0U,
                       (finalState == ReplyTokenState::Replied) || (finalState == ReplyTokenState::Cancelled),
                       fixture.Context.GetSnapshot().CurrentThread.IsEqualTo(fixture.CallerId));
        ASTERIA_EXPECT(runner, "the terminal race keeps the exact caller live through wake",
                       fixture.Caller.Wake(fixture.BlockToken) == ThreadError::None);
    }
    ASTERIA_EXPECT(runner, "concurrent fixture retires cleanly", fixture.Retire());
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ReplyTokenTests"};
    Asteria::Test::TestInitializationFailures(runner);
    Asteria::Test::TestStateAndIdentityErrors(runner);
    Asteria::Test::TestPreconfiguredArm(runner);
    Asteria::Test::TestReplyLifecycle(runner);
    Asteria::Test::TestCancellationAndReturnFailure(runner);
    Asteria::Test::TestTerminalPinRetention(runner);
    Asteria::Test::TestNestedDonation(runner);
    Asteria::Test::TestConcurrentCompletion(runner);
    return runner.Finish();
}
