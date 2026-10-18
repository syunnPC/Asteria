// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Memory/PageFaultIngress.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointErrorCode;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::FaultToken;
using Kernel::Memory::FaultTokenFactory;
using Kernel::Memory::FaultTokenFactoryError;
using Kernel::Memory::PageFaultIngress;
using Kernel::Memory::PageFaultIngressError;
using Kernel::Memory::PageFaultIngressRequest;
using Kernel::Memory::PageFaultIngressState;
using Kernel::Memory::PagerFaultCompletionRequest;
using Kernel::Memory::PagerFaultDispatcher;
using Kernel::Memory::PagerProtocol;
using Kernel::Memory::PagerProtocolError;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityOperation;
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
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr FaultRecord UserFault() noexcept
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

[[nodiscard]] constexpr Kernel::Ipc::TraceContext Trace() noexcept
{
    return Kernel::Ipc::TraceContext{
        .TraceIdentifierHigh = 0x1122'3344'5566'7788ULL,
        .TraceIdentifierLow = 0x99AA'BBCC'DDEE'FF00ULL,
        .SpanIdentifier = 0x8877'6655'4433'2211ULL,
    };
}

[[nodiscard]] MessageEnvelope ControlMessage(const UInt64 transaction) noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = transaction;
    message.Deadline = MonotonicTime::Infinite();
    message.MethodIdentifier = 1U;
    message.InterfaceVersionMajor = 1U;
    message.Kind = MessageKind::Asynchronous;
    return message;
}

class IngressFixture final
{
public:
    IngressFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          KernelSpace{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          PagerSpace{Registry, Revocations, Clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          PagerEndpoint{PagerSpace, ResourceDomainId{9U}}, Scheduler{Registry, Clock, CpuId{0U}}, Context{Registry},
          Factory{Registry, Issuer}, Dispatcher{Registry, PagerEndpoint, Scheduler.GetWakeFacade()}
    {
    }

    [[nodiscard]] bool InitializeRunning() noexcept
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
        return Context.StartRun(CallerId, CpuId{0U}, MonotonicTime{10U}).HasValue();
    }

    [[nodiscard]] PageFaultIngressRequest Request() noexcept
    {
        return PageFaultIngressRequest{
            .SourceSpace = &KernelSpace,
            .Thread = CallerId,
            .AddressSpace = AddressId,
            .SchedulingContext = ContextId,
            .Fault = UserFault(),
            .Trace = Trace(),
            .ChargeOwner = ResourceDomainId{7U},
            .Sender = ResourceDomainId{7U},
            .RevocationDomain = Domain,
            .Deadline = MonotonicTime::Infinite(),
            .TransactionIdentifier = 0x1122'3344'5566'7788ULL,
            .SupervisionTag = 0x7A67U,
            .ProtectionDomainIdentity = 0x0102'0304'0506'0708ULL,
        };
    }

    [[nodiscard]] ThreadBlockToken FinishBlocked() noexcept
    {
        const auto context = Context.EndRun(CpuId{0U}, MonotonicTime{11U});
        if (!context.HasValue())
        {
            return {};
        }
        const auto thread = Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        return thread.HasValue() ? thread.GetValue().BlockToken : ThreadBlockToken{};
    }

    [[nodiscard]] bool Resolve(const ObjectId tokenObject, const Kernel::Object::CapabilityHandle receiverHandle,
                               const FaultResolutionKind resolution) noexcept
    {
        Kernel::Memory::FaultResolutionClaimToken claim{};
        {
            auto token = PagerSpace.Lookup(receiverHandle, ObjectType::FaultToken, CapabilityOperation::Invoke);
            if (!token.HasValue() || (token.GetValue().GetDescriptor().Object.GetValue() != tokenObject.GetValue()))
            {
                return false;
            }
            auto* const storage = static_cast<FaultToken*>(&token.GetValue().GetObject());
            const auto claimed = storage->ClaimResolution();
            if (!claimed.HasValue())
            {
                return false;
            }
            claim = claimed.GetValue().Token;
        }

        const auto completed = Dispatcher.Complete(PagerFaultCompletionRequest{
            .Claim = claim,
            .ExitCode = resolution == FaultResolutionKind::Terminate ? 0xFA17U : 0U,
            .Resolution = resolution,
            .Reserved = {},
        });
        return completed.HasValue();
    }

    [[nodiscard]] bool RetireToken(const ObjectId token, const Kernel::Object::CapabilityHandle receiverHandle) noexcept
    {
        if ((PagerSpace.Close(receiverHandle) != CapabilityError::Ok) || !Factory.Retire(token).HasValue())
        {
            return false;
        }
        const auto collected = Issuer.CollectDestroyedObject();
        return collected.HasValue() && Factory.Reclaim(collected.GetValue()).HasValue();
    }

    [[nodiscard]] bool ReclaimFactoryRollback() noexcept
    {
        const auto collected = Issuer.CollectDestroyedObject();
        return collected.HasValue() && Factory.Reclaim(collected.GetValue()).HasValue();
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
        if ((Context.Unbind(CallerId) != SchedulingContextError::None) ||
            (Issuer.DestroyObject(ContextObject) != ObjectRegistryError::Ok) ||
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
    Thread Caller;
    AddressSpaceStub AddressSpace;
    SchedulingContext Context;
    FaultTokenFactory Factory;
    PagerFaultDispatcher Dispatcher;
    ObjectId CallerObject{};
    ObjectId ContextObject{};
    ObjectId AddressObject{};
    ThreadId CallerId{};
    SchedulingContextId ContextId{};
    AddressSpaceId AddressId{};
    RevocationDomainId Domain{};
};

void TestSuccessfulIngress(TestContext& runner) noexcept
{
    IngressFixture fixture;
    const bool initialized = fixture.InitializeRunning();
    bool delivered{false};
    bool resolved{false};
    bool retired{false};
    {
        PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
        const auto prepared =
            initialized ? ingress.Prepare(fixture.Request()) : PageFaultIngressError::InvalidConfiguration;
        const auto beforeBlock = ingress.GetSnapshot();
        const ThreadBlockToken blockToken = fixture.FinishBlocked();
        ingress.CommitBlocked(blockToken);
        const auto afterBlock = ingress.GetSnapshot();

        MessageEnvelope message{};
        const auto received = fixture.PagerEndpoint.Receive(message);
        Kernel::Memory::PagerFaultNotification notification{};
        const PagerProtocolError parsed = received.HasValue()
                                              ? PagerProtocol::ParseFaultNotification(message, notification)
                                              : PagerProtocolError::InvalidMessageContract;
        delivered =
            (prepared == PageFaultIngressError::None) && (beforeBlock.State == PageFaultIngressState::Prepared) &&
            blockToken.IsValid() && (afterBlock.State == PageFaultIngressState::Delivered) &&
            (afterBlock.Error == PageFaultIngressError::None) && afterBlock.FaultToken.IsValid() &&
            (afterBlock.EndpointSequence == 1U) && received.HasValue() && (parsed == PagerProtocolError::None) &&
            (notification.Fault.VirtualAddress == UserFault().VirtualAddress) &&
            (notification.SupervisionTag == 0x7A67U) &&
            (notification.ProtectionDomainIdentity == 0x0102'0304'0506'0708ULL);
        resolved =
            delivered && fixture.Resolve(afterBlock.FaultToken, notification.FaultToken, FaultResolutionKind::Map);
        retired = resolved && fixture.RetireToken(afterBlock.FaultToken, notification.FaultToken);
    }

    ASTERIA_EXPECT(runner, "a blocked user fault is delivered exactly once", delivered);
    ASTERIA_EXPECT(runner, "a mapped fault resumes through the scheduler-owned ready queue", resolved,
                   fixture.Caller.GetSnapshot().State == ThreadState::Ready,
                   fixture.Scheduler.GetSnapshot().Queue.ReadyEntries == 1U);
    ASTERIA_EXPECT(runner, "the delivered factory token retires and reclaims", retired);
    ASTERIA_EXPECT(runner, "the successful ingress releases every retained object", fixture.RetireObjects());
}

void TestEndpointPressureRetries(TestContext& runner) noexcept
{
    IngressFixture fixture;
    bool setup = fixture.InitializeRunning();
    for (UInt64 index{1U}; setup && (index <= 8U); ++index)
    {
        setup = fixture.PagerEndpoint
                    .Send(fixture.KernelSpace, ResourceDomainId{7U}, EndpointSendClass::ReservedControl,
                          ControlMessage(index))
                    .HasValue();
    }

    bool pressureRetained{false};
    bool retried{false};
    bool retired{false};
    {
        PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
        const bool prepared = setup && (ingress.Prepare(fixture.Request()) == PageFaultIngressError::None);
        const ThreadBlockToken blockToken = fixture.FinishBlocked();
        ingress.CommitBlocked(blockToken);
        const auto pressured = ingress.GetSnapshot();
        pressureRetained = prepared && (pressured.State == PageFaultIngressState::TokenReady) &&
                           (pressured.Error == PageFaultIngressError::DeliveryRejected) &&
                           (pressured.Endpoint == EndpointErrorCode::WouldBlock) && pressured.FaultToken.IsValid() &&
                           (fixture.Caller.GetSnapshot().State == ThreadState::Blocked);

        MessageEnvelope discarded{};
        const bool freed = fixture.PagerEndpoint.Receive(discarded).HasValue();
        ingress.Retry();
        const auto delivered = ingress.GetSnapshot();
        MessageEnvelope message{};
        bool drained{freed};
        for (Foundation::UInt8 index{0U}; index < 8U; ++index)
        {
            drained = drained && fixture.PagerEndpoint.Receive(message).HasValue();
        }
        Kernel::Memory::PagerFaultNotification notification{};
        const PagerProtocolError parsed = drained ? PagerProtocol::ParseFaultNotification(message, notification)
                                                  : PagerProtocolError::InvalidMessageContract;
        retried = pressureRetained && (delivered.State == PageFaultIngressState::Delivered) &&
                  (delivered.Error == PageFaultIngressError::None) && (parsed == PagerProtocolError::None) &&
                  fixture.Resolve(delivered.FaultToken, notification.FaultToken, FaultResolutionKind::Terminate);
        retired = retried && fixture.RetireToken(delivered.FaultToken, notification.FaultToken);
    }

    ASTERIA_EXPECT(runner, "reserved Endpoint pressure retains a retryable blocked token", pressureRetained);
    ASTERIA_EXPECT(runner, "delivery retry publishes after reserved capacity becomes available", retried,
                   fixture.Caller.GetSnapshot().State == ThreadState::Exited);
    ASTERIA_EXPECT(runner, "retried delivery retires its exact token", retired);
    ASTERIA_EXPECT(runner, "the retry fixture releases every retained object", fixture.RetireObjects());
}

void TestCorrelationFailureDoesNotMint(TestContext& runner) noexcept
{
    IngressFixture fixture;
    const bool initialized = fixture.InitializeRunning();
    bool rejected{false};
    {
        PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
        const bool prepared = initialized && (ingress.Prepare(fixture.Request()) == PageFaultIngressError::None);
        const ThreadBlockToken blockToken = fixture.FinishBlocked();
        const bool woke = blockToken.IsValid() && (fixture.Caller.Wake(blockToken) == ThreadError::None);
        ingress.CommitBlocked(blockToken);
        const auto snapshot = ingress.GetSnapshot();
        const auto factory = fixture.Factory.GetSnapshot();
        rejected = prepared && woke && (snapshot.State == PageFaultIngressState::Faulted) &&
                   (snapshot.Error == PageFaultIngressError::CorrelatedStateChanged) &&
                   !snapshot.FaultToken.IsValid() && (factory.FreeSlots == FaultTokenFactory::MaximumTokens) &&
                   (factory.ActiveSlots == 0U) && (fixture.PagerEndpoint.GetQueueState().QueuedMessages == 0U);
    }

    ASTERIA_EXPECT(runner, "a changed block generation faults before minting pager authority", rejected);
    ASTERIA_EXPECT(runner, "the correlation failure releases every retained object", fixture.RetireObjects());
}

void TestTokenReadyCorrelationFailureRollsBack(TestContext& runner) noexcept
{
    IngressFixture fixture;
    bool setup = fixture.InitializeRunning();
    for (UInt64 index{1U}; setup && (index <= 8U); ++index)
    {
        setup = fixture.PagerEndpoint
                    .Send(fixture.KernelSpace, ResourceDomainId{7U}, EndpointSendClass::ReservedControl,
                          ControlMessage(index))
                    .HasValue();
    }

    bool rolledBack{false};
    {
        PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
        const bool prepared = setup && (ingress.Prepare(fixture.Request()) == PageFaultIngressError::None);
        const ThreadBlockToken blockToken = fixture.FinishBlocked();
        ingress.CommitBlocked(blockToken);
        const auto pressured = ingress.GetSnapshot();
        const bool woke = blockToken.IsValid() && (fixture.Caller.Wake(blockToken) == ThreadError::None);
        ingress.Retry();
        const auto failed = ingress.GetSnapshot();
        const auto factory = fixture.Factory.GetSnapshot();
        rolledBack = prepared && (pressured.State == PageFaultIngressState::TokenReady) &&
                     pressured.FaultToken.IsValid() && woke && (failed.State == PageFaultIngressState::Faulted) &&
                     (failed.Error == PageFaultIngressError::CorrelatedStateChanged) &&
                     (failed.FaultToken.GetValue() == pressured.FaultToken.GetValue()) && (factory.ActiveSlots == 0U) &&
                     (factory.QuiescingSlots == 1U) && (fixture.KernelSpace.GetActiveCount() == 0U);
    }

    ASTERIA_EXPECT(runner, "a changed TokenReady correlation rolls back unpublished authority", rolledBack);
    ASTERIA_EXPECT(runner, "TokenReady correlation rollback remains fully reclaimable",
                   fixture.ReclaimFactoryRollback(), fixture.RetireObjects());
}

void TestSupervisorFaultIsRejectedBeforePinning(TestContext& runner) noexcept
{
    IngressFixture fixture;
    const bool initialized = fixture.InitializeRunning();
    bool rejected{false};
    {
        PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
        PageFaultIngressRequest request = fixture.Request();
        request.Fault.Privilege = FaultPrivilege::Supervisor;
        const auto error = initialized ? ingress.Prepare(request) : PageFaultIngressError::None;
        const auto snapshot = ingress.GetSnapshot();
        rejected = (error == PageFaultIngressError::InvalidConfiguration) &&
                   (snapshot.State == PageFaultIngressState::Idle) && !snapshot.FaultToken.IsValid();
    }

    ASTERIA_EXPECT(runner, "wired supervisor faults never enter the external pager path", rejected);
    const auto context = fixture.Context.EndRun(CpuId{0U}, MonotonicTime{11U});
    const auto thread = fixture.Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Ready);
    ASTERIA_EXPECT(runner, "a rejected supervisor fault leaves the running transaction recoverable", context.HasValue(),
                   thread.HasValue(), fixture.RetireObjects());
}

void TestExpiredAuthorityIsTerminal(TestContext& runner) noexcept
{
    IngressFixture fixture;
    const bool initialized = fixture.InitializeRunning();
    bool rejected{false};
    {
        PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
        PageFaultIngressRequest request = fixture.Request();
        request.Deadline = MonotonicTime{1U};
        const bool prepared = initialized && (ingress.Prepare(request) == PageFaultIngressError::None);
        const ThreadBlockToken blockToken = fixture.FinishBlocked();
        ingress.CommitBlocked(blockToken);
        const auto snapshot = ingress.GetSnapshot();
        ingress.Retry();
        const auto afterRetry = ingress.GetSnapshot();
        const auto factory = fixture.Factory.GetSnapshot();
        rejected = prepared && blockToken.IsValid() && (snapshot.State == PageFaultIngressState::Faulted) &&
                   (snapshot.Error == PageFaultIngressError::FactoryRejected) &&
                   (snapshot.Factory == FaultTokenFactoryError::CapabilityRejected) &&
                   (afterRetry.State == snapshot.State) && (afterRetry.Error == snapshot.Error) &&
                   (afterRetry.Factory == snapshot.Factory) && (factory.QuiescingSlots == 1U) &&
                   (fixture.Caller.GetSnapshot().State == ThreadState::Blocked);
    }

    ASTERIA_EXPECT(runner, "an expired authority is terminal rather than retried as pressure", rejected);
    ASTERIA_EXPECT(runner, "terminal factory rollback releases its token and object pins",
                   fixture.ReclaimFactoryRollback(), fixture.RetireObjects());
}

void TestAuthorityExpiryDuringDeliveryRollsBack(TestContext& runner) noexcept
{
    IngressFixture fixture;
    const bool initialized = fixture.InitializeRunning();
    fixture.Clock.Set(10U);
    fixture.Clock.SetAdvanceOnRead(true);
    bool rolledBack{false};
    {
        PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
        PageFaultIngressRequest request = fixture.Request();
        request.Deadline = MonotonicTime{11U};
        const bool prepared = initialized && (ingress.Prepare(request) == PageFaultIngressError::None);
        const ThreadBlockToken blockToken = fixture.FinishBlocked();
        ingress.CommitBlocked(blockToken);
        const auto snapshot = ingress.GetSnapshot();
        const auto factory = fixture.Factory.GetSnapshot();
        rolledBack = prepared && blockToken.IsValid() && (snapshot.State == PageFaultIngressState::Faulted) &&
                     (snapshot.Error == PageFaultIngressError::DeliveryRejected) &&
                     (snapshot.Dispatcher == Kernel::Memory::PagerFaultDispatcherError::CapabilityLookupRejected) &&
                     snapshot.FaultToken.IsValid() && (factory.ActiveSlots == 0U) && (factory.QuiescingSlots == 1U) &&
                     (fixture.KernelSpace.GetActiveCount() == 0U) &&
                     (fixture.Caller.GetSnapshot().State == ThreadState::Blocked);
    }

    ASTERIA_EXPECT(runner, "authority expiry during delivery closes and retires unpublished state", rolledBack);
    ASTERIA_EXPECT(runner, "post-mint terminal rollback remains fully reclaimable", fixture.ReclaimFactoryRollback(),
                   fixture.RetireObjects());
}
void TestResetEndsOnlyADeliveredTransaction(TestContext& runner) noexcept
{
    IngressFixture fixture;
    PageFaultIngress ingress{fixture.Registry, fixture.Factory, fixture.Dispatcher};
    ASTERIA_EXPECT(runner, "Reset ends only a Delivered ingress or a Completed dispatcher",
                   ingress.Reset() == PageFaultIngressError::InvalidState,
                   ingress.GetSnapshot().State == PageFaultIngressState::Idle,
                   fixture.Dispatcher.Reset() == Kernel::Memory::PagerFaultDispatcherError::NotDelivered);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"PageFaultIngressTests"};
    Asteria::Test::TestSuccessfulIngress(runner);
    Asteria::Test::TestEndpointPressureRetries(runner);
    Asteria::Test::TestCorrelationFailureDoesNotMint(runner);
    Asteria::Test::TestTokenReadyCorrelationFailureRollsBack(runner);
    Asteria::Test::TestSupervisorFaultIsRejectedBeforePinning(runner);
    Asteria::Test::TestExpiredAuthorityIsTerminal(runner);
    Asteria::Test::TestAuthorityExpiryDuringDeliveryRollsBack(runner);
    Asteria::Test::TestResetEndsOnlyADeliveredTransaction(runner);
    return runner.Finish();
}
