// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Memory/PagerFaultDispatcher.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::FaultToken;
using Kernel::Memory::FaultTokenError;
using Kernel::Memory::FaultTokenState;
using Kernel::Memory::PagerFaultDispatcher;
using Kernel::Memory::PagerFaultDispatcherError;
using Kernel::Memory::PagerFaultDispatcherFailure;
using Kernel::Memory::PagerFaultDispatcherState;
using Kernel::Memory::PagerFaultCompletionRequest;
using Kernel::Memory::PagerFaultDeliveryRequest;
using Kernel::Memory::PagerFaultNotification;
using Kernel::Memory::PagerProtocol;
using Kernel::Memory::PagerProtocolError;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::ProcessorScheduler;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadBlockToken;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadState;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

class AddressSpaceStub final : public IKernelObject
{
public:
    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::AddressSpace;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{7U};
    }
};

[[nodiscard]] constexpr SchedulingContextConfiguration Configuration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x0FU,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr FaultRecord MakeFault() noexcept
{
    return FaultRecord{
        .VirtualAddress = 0x0000'0001'2345'6000ULL,
        .InstructionPointer = 0x0000'0000'0040'1234ULL,
        .ArchitectureSyndrome = 0xA55AU,
        .Access = FaultAccess::Write,
        .Cause = FaultCause::MissingMapping,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr Kernel::Ipc::TraceContext MakeTrace() noexcept
{
    return Kernel::Ipc::TraceContext{
        .TraceIdentifierHigh = 0x1122'3344'5566'7788ULL,
        .TraceIdentifierLow = 0x99AA'BBCC'DDEE'FF00ULL,
        .SpanIdentifier = 0x8877'6655'4433'2211ULL,
    };
}

[[nodiscard]] bool FailureIs(const PagerFaultDispatcherFailure& failure, const PagerFaultDispatcherError error) noexcept
{
    return (failure.Error == error) && (failure.Reserved == 0U);
}

class FaultFixture final
{
public:
    FaultFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          KernelSpace{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          PagerSpace{Registry, Revocations, Clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          PagerEndpoint{PagerSpace, ResourceDomainId{9U}}, Scheduler{Registry, Clock, CpuId{0U}}, Context{Registry}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        if ((Caller.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (Context.Initialize(Configuration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }
        const auto caller = Issuer.CreateObject(Caller);
        const auto context = Issuer.CreateObject(Context);
        const auto address = Issuer.CreateObject(AddressSpace);
        const auto domain = Revocations.CreateRoot();
        if (!caller.HasValue() || !context.HasValue() || !address.HasValue() || !domain.HasValue())
        {
            return false;
        }

        CallerObject = caller.GetValue();
        ContextObject = context.GetValue();
        AddressObject = address.GetValue();
        Domain = domain.GetValue();
        CallerId = ThreadId{Registry, CallerObject};
        ContextId = SchedulingContextId{Registry, ContextObject};
        AddressId = AddressSpaceId{Registry, AddressObject};
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

    [[nodiscard]] bool InitializeToken(FaultToken& token, ObjectId& tokenObject,
                                       CapabilityHandle& sourceHandle) noexcept
    {
        if (token.Initialize(Registry, ResourceDomainId{7U}, CallerId, AddressId, MakeFault()) != FaultTokenError::None)
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(token);
        if (!registered.HasValue())
        {
            return false;
        }
        tokenObject = registered.GetValue();
        const CapabilityRights rights = CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Transfer);
        const auto minted = Issuer.Mint(KernelSpace, tokenObject, rights, Domain, MonotonicTime::Infinite(), 0xFA17U);
        if (!minted.HasValue())
        {
            return false;
        }
        sourceHandle = minted.GetValue();
        return true;
    }

    [[nodiscard]] bool RetireToken(const ObjectId token, const CapabilityHandle receiverHandle = {}) noexcept
    {
        if (receiverHandle.IsValid() && (PagerSpace.Close(receiverHandle) != CapabilityError::Ok))
        {
            return false;
        }
        if (Issuer.DestroyObject(token) != ObjectRegistryError::Ok)
        {
            return false;
        }
        const auto collected = Issuer.CollectDestroyedObject();
        return collected.HasValue() && (collected.GetValue().Object.GetValue() == token.GetValue());
    }

    [[nodiscard]] bool RetireObjects() noexcept
    {
        static_cast<void>(PagerEndpoint.DiscardUnread());
        const auto queue = Scheduler.GetSnapshot().Queue;
        if ((queue.ReadyEntries != 0U) && !Scheduler.Remove(CallerId).HasValue())
        {
            return false;
        }
        if ((queue.ClaimedEntries != 0U) || (queue.ReservedEntries != 0U))
        {
            return false;
        }

        const auto thread = Caller.GetSnapshot();
        if ((thread.State == ThreadState::Blocked) && (Caller.Wake(thread.BlockToken) != ThreadError::None))
        {
            return false;
        }
        if ((Caller.GetSnapshot().State != ThreadState::Exited) && (Caller.Terminate(0U) != ThreadError::None))
        {
            return false;
        }
        if (Context.Unbind(CallerId) != SchedulingContextError::None)
        {
            return false;
        }
        if ((Issuer.DestroyObject(ContextObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(CallerObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(AddressObject) != ObjectRegistryError::Ok))
        {
            return false;
        }
        for (Foundation::UInt8 index{0U}; index < 3U; ++index)
        {
            if (!Issuer.CollectDestroyedObject().HasValue())
            {
                return false;
            }
        }
        return true;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace KernelSpace;
    CapabilitySpace PagerSpace;
    Endpoint PagerEndpoint;
    ProcessorScheduler Scheduler;
    SchedulingContext Context;
    Thread Caller;
    AddressSpaceStub AddressSpace;
    ObjectId CallerObject{};
    ObjectId ContextObject{};
    ObjectId AddressObject{};
    ThreadId CallerId{};
    SchedulingContextId ContextId{};
    AddressSpaceId AddressId{};
    ThreadBlockToken BlockToken{};
    RevocationDomainId Domain{};
};

[[nodiscard]] auto Deliver(PagerFaultDispatcher& dispatcher, FaultFixture& fixture, const ObjectId token,
                           const CapabilityHandle handle, const Kernel::Ipc::TraceContext& trace = MakeTrace()) noexcept
{
    return dispatcher.Deliver(PagerFaultDeliveryRequest{
        .SourceSpace = &fixture.KernelSpace,
        .FaultToken = token,
        .SchedulingContext = fixture.ContextId,
        .BlockToken = fixture.BlockToken,
        .Sender = ResourceDomainId{7U},
        .FaultTokenCapability = handle,
        .Trace = trace,
        .TransactionIdentifier = 0x1122'3344'5566'7788ULL,
        .Deadline = MonotonicTime::Infinite(),
        .SupervisionTag = 0U,
    });
}

[[nodiscard]] auto Complete(PagerFaultDispatcher& dispatcher, const Kernel::Memory::FaultResolutionClaimToken claim,
                            const FaultResolutionKind resolution, const UInt64 exitCode = 0U) noexcept
{
    return dispatcher.Complete(PagerFaultCompletionRequest{
        .Claim = claim,
        .ExitCode = exitCode,
        .Resolution = resolution,
        .Reserved = {},
    });
}

void TestMapCompletion(TestContext& runner) noexcept
{
    FaultFixture fixture;
    ASTERIA_EXPECT(runner, "map fixture initializes", fixture.Initialize());
    {
        FaultToken token;
        ObjectId tokenObject{};
        CapabilityHandle sourceHandle{};
        const bool tokenReady = fixture.InitializeToken(token, tokenObject, sourceHandle);
        PagerFaultDispatcher dispatcher{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        const auto delivered = tokenReady
                                   ? Deliver(dispatcher, fixture, tokenObject, sourceHandle)
                                   : decltype(Deliver(dispatcher, fixture, tokenObject, sourceHandle))::Failure({});

        MessageEnvelope message{};
        const auto received = fixture.PagerEndpoint.Receive(message);
        PagerFaultNotification notification{};
        const PagerProtocolError parsed = received.HasValue()
                                              ? PagerProtocol::ParseFaultNotification(message, notification)
                                              : PagerProtocolError::InvalidMessageContract;
        bool receiverCapabilityMatches{false};
        if (parsed == PagerProtocolError::None)
        {
            auto lookup =
                fixture.PagerSpace.Lookup(notification.FaultToken, ObjectType::FaultToken, CapabilityOperation::Invoke);
            receiverCapabilityMatches =
                lookup.HasValue() && (lookup.GetValue().GetDescriptor().Object.GetValue() == tokenObject.GetValue());
        }

        const auto claim = token.ClaimResolution();
        const auto completed = claim.HasValue()
                                   ? Complete(dispatcher, claim.GetValue().Token, FaultResolutionKind::Map)
                                   : decltype(Complete(dispatcher, {}, FaultResolutionKind::Map))::Failure({});
        const auto thread = fixture.Caller.GetSnapshot();
        const auto queue = fixture.Scheduler.GetSnapshot().Queue;
        ASTERIA_EXPECT(runner, "Map resolution publishes notification then wakes the exact blocked thread", tokenReady,
                       delivered.HasValue(), delivered.GetValue().EndpointSequence == 1U,
                       delivered.GetValue().QueueSequence == 0U,
                       delivered.GetValue().State == PagerFaultDispatcherState::Delivered, received.HasValue(),
                       parsed == PagerProtocolError::None, receiverCapabilityMatches, notification.FaultToken.IsValid(),
                       completed.HasValue(), completed.GetValue().Resolution == FaultResolutionKind::Map,
                       completed.GetValue().QueueSequence == 1U,
                       completed.GetValue().State == PagerFaultDispatcherState::Completed,
                       thread.State == ThreadState::Ready, queue.ReadyEntries == 1U, queue.ReservedEntries == 0U,
                       token.GetSnapshot().State == FaultTokenState::Resolved);

        const auto replay = Complete(
            dispatcher, claim.HasValue() ? claim.GetValue().Token : Kernel::Memory::FaultResolutionClaimToken{},
            FaultResolutionKind::Map);
        ASTERIA_EXPECT(runner, "completed dispatcher rejects replay", !replay.HasValue(),
                       FailureIs(replay.GetError(), PagerFaultDispatcherError::AlreadyCompleted));

        const bool retired = fixture.RetireToken(tokenObject, notification.FaultToken);
        ASTERIA_EXPECT(runner, "mapped FaultToken retires after receiver close", retired);
    }
    ASTERIA_EXPECT(runner, "map fixture retires all retained objects", fixture.RetireObjects());
}

void TestDeliveryFailureIsRetryable(TestContext& runner) noexcept
{
    FaultFixture fixture;
    ASTERIA_EXPECT(runner, "retry fixture initializes", fixture.Initialize());
    {
        FaultToken token;
        ObjectId tokenObject{};
        CapabilityHandle sourceHandle{};
        const bool tokenReady = fixture.InitializeToken(token, tokenObject, sourceHandle);
        PagerFaultDispatcher dispatcher{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        Kernel::Ipc::TraceContext invalidTrace = MakeTrace();
        invalidTrace.SpanIdentifier = 0U;
        const auto rejected = Deliver(dispatcher, fixture, tokenObject, sourceHandle, invalidTrace);
        bool sourceLookupValid{false};
        {
            auto sourceLookup =
                fixture.KernelSpace.Lookup(sourceHandle, ObjectType::FaultToken, CapabilityOperation::Invoke);
            sourceLookupValid = sourceLookup.HasValue();
        }
        const auto beforeRetryQueue = fixture.PagerEndpoint.GetQueueState();
        const auto retried = Deliver(dispatcher, fixture, tokenObject, sourceHandle);
        MessageEnvelope message{};
        const auto received = fixture.PagerEndpoint.Receive(message);
        const CapabilityHandle receiverHandle =
            received.HasValue() ? message.Attachments[0U].Handle : CapabilityHandle{};
        ASTERIA_EXPECT(
            runner, "delivery validation failure is inert and the exact attempt can retry", tokenReady,
            !rejected.HasValue(), FailureIs(rejected.GetError(), PagerFaultDispatcherError::ProtocolRejected),
            rejected.GetError().Protocol == PagerProtocolError::InvalidTraceContext, sourceLookupValid,
            beforeRetryQueue.QueuedMessages == 0U, token.GetSnapshot().State == FaultTokenState::Pending,
            fixture.Caller.GetSnapshot().State == ThreadState::Blocked, retried.HasValue(), received.HasValue());

        const auto claim = token.ClaimResolution();
        const auto terminated =
            claim.HasValue() ? Complete(dispatcher, claim.GetValue().Token, FaultResolutionKind::Terminate, 0xFA17U)
                             : decltype(Complete(dispatcher, {}, FaultResolutionKind::Terminate, 0xFA17U))::Failure({});
        ASTERIA_EXPECT(
            runner, "Terminate resolution exits without publishing a ready entry", terminated.HasValue(),
            terminated.GetValue().QueueSequence == 0U, fixture.Caller.GetSnapshot().State == ThreadState::Exited,
            fixture.Caller.GetSnapshot().ExitCode == 0xFA17U, fixture.Scheduler.GetSnapshot().Queue.ReadyEntries == 0U);

        const bool retired = fixture.RetireToken(tokenObject, receiverHandle);
        ASTERIA_EXPECT(runner, "terminated FaultToken retires after receiver close", retired);
    }
    ASTERIA_EXPECT(runner, "retry fixture retires all retained objects", fixture.RetireObjects());
}

void TestInvalidClaimPreservesDelivery(TestContext& runner) noexcept
{
    FaultFixture fixture;
    ASTERIA_EXPECT(runner, "claim fixture initializes", fixture.Initialize());
    {
        FaultToken token;
        ObjectId tokenObject{};
        CapabilityHandle sourceHandle{};
        const bool tokenReady = fixture.InitializeToken(token, tokenObject, sourceHandle);
        PagerFaultDispatcher dispatcher{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        const auto delivered = Deliver(dispatcher, fixture, tokenObject, sourceHandle);
        MessageEnvelope message{};
        const auto received = fixture.PagerEndpoint.Receive(message);
        const CapabilityHandle receiverHandle =
            received.HasValue() ? message.Attachments[0U].Handle : CapabilityHandle{};
        const auto invalidResolution = Complete(dispatcher, {}, static_cast<FaultResolutionKind>(0xffU));
        const auto rejected = Complete(dispatcher, {}, FaultResolutionKind::Map);
        const auto claim = token.ClaimResolution();
        const auto completed = claim.HasValue()
                                   ? Complete(dispatcher, claim.GetValue().Token, FaultResolutionKind::Redirect)
                                   : decltype(Complete(dispatcher, {}, FaultResolutionKind::Redirect))::Failure({});
        ASTERIA_EXPECT(runner, "claim rejection preserves the delivered transaction for exact retry", tokenReady,
                       delivered.HasValue(), received.HasValue(), !invalidResolution.HasValue(),
                       FailureIs(invalidResolution.GetError(), PagerFaultDispatcherError::InvalidResolution),
                       !rejected.HasValue(), FailureIs(rejected.GetError(), PagerFaultDispatcherError::TokenRejected),
                       rejected.GetError().Token == FaultTokenError::NotClaimed, completed.HasValue(),
                       completed.GetValue().Resolution == FaultResolutionKind::Redirect,
                       fixture.Caller.GetSnapshot().State == ThreadState::Ready);

        const bool retired = fixture.RetireToken(tokenObject, receiverHandle);
        ASTERIA_EXPECT(runner, "redirected FaultToken retires cleanly", retired);
    }
    ASTERIA_EXPECT(runner, "claim fixture retires all retained objects", fixture.RetireObjects());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"PagerFaultDispatcherTests"};
    Asteria::Test::TestMapCompletion(runner);
    Asteria::Test::TestDeliveryFailureIsRetryable(runner);
    Asteria::Test::TestInvalidClaimPreservesDelivery(runner);
    return runner.Finish();
}
