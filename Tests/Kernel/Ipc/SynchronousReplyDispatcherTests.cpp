// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>

#include "Kernel/Ipc/SynchronousReplyDispatcher.hpp"
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
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointConfiguration;
using Kernel::Ipc::EndpointErrorCode;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::ReplyToken;
using Kernel::Ipc::ReplyTokenError;
using Kernel::Ipc::ReplyTokenState;
using Kernel::Ipc::SynchronousReplyDispatcher;
using Kernel::Ipc::SynchronousReplyDispatcherError;
using Kernel::Ipc::SynchronousReplyDispatcherFailure;
using Kernel::Ipc::SynchronousReplyDispatcherState;
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
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::ReadyQueue;
using Kernel::Scheduler::ReadyQueueError;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingContextState;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadState;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

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

[[nodiscard]] MessageEnvelope MakeMessage(const MessageKind kind, const UInt64 transaction,
                                          const Foundation::UInt8 marker = 0U) noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = transaction;
    message.Deadline = MonotonicTime::Infinite();
    message.MethodIdentifier = 7U;
    message.InterfaceVersionMajor = 1U;
    message.InterfaceVersionMinor = 0U;
    message.FixedPayloadSizeBytes = 1U;
    message.PayloadSizeBytes = 1U;
    message.Kind = kind;
    message.Cancellation = CancellationState::None;
    message.Payload[0U] = marker;
    return message;
}

[[nodiscard]] bool FailureIs(const SynchronousReplyDispatcherFailure& failure,
                             const SynchronousReplyDispatcherError error) noexcept
{
    bool reservedZero{true};
    for (const Foundation::UInt8 value : failure.Reserved)
    {
        reservedZero = reservedZero && (value == 0U);
    }
    return (failure.Error == error) && reservedZero;
}

/// Reads a constant tick and runs one armed hook inside the next read, which
/// the kernel performs while it holds the shared registry lock. A hook that
/// changes kernel state there breaks the clock contract on purpose: it
/// injects an exclusion breach at an exact point of a transaction.
class HookClock final : public Hal::Time::IMonotonicClock
{
public:
    using Hook = void (*)(void* context) noexcept;

    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override
    {
        const Hook hook = m_hook;
        m_hook = nullptr;
        if (hook != nullptr)
        {
            hook(m_context);
        }
        return 10U;
    }

    void ArmOnce(const Hook hook, void* const context) noexcept
    {
        m_hook = hook;
        m_context = context;
    }

private:
    mutable Hook m_hook{nullptr};
    void* m_context{nullptr};
};

class ReplyFixture final
{
public:
    explicit ReplyFixture(const EndpointConfiguration endpointConfiguration = EndpointConfiguration::Default(),
                          Hal::Time::IMonotonicClock* const clock = nullptr) noexcept
        : ActiveClock{(clock != nullptr) ? *clock : Clock}, Registry{Synchronization}, Revocations{Synchronization},
          Issuer{Registry},
          ServerSpace{Registry, Revocations, ActiveClock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          CallerSpace{Registry, Revocations, ActiveClock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          ReplyEndpoint{CallerSpace, ResourceDomainId{7U}, endpointConfiguration}, WakeQueue{Registry, CpuId{0U}},
          Context{Registry}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        if ((Caller.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (Server.Initialize(ResourceDomainId{8U}) != ThreadError::None) ||
            (Context.Initialize(Configuration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }
        const auto callerObject = Issuer.CreateObject(Caller);
        const auto serverObject = Issuer.CreateObject(Server);
        const auto contextObject = Issuer.CreateObject(Context);
        if (!callerObject.HasValue() || !serverObject.HasValue() || !contextObject.HasValue())
        {
            return false;
        }
        CallerObject = callerObject.GetValue();
        ServerObject = serverObject.GetValue();
        ContextObject = contextObject.GetValue();
        CallerId = ThreadId{Registry, CallerObject};
        ServerId = ThreadId{Registry, ServerObject};
        ContextId = SchedulingContextId{Registry, ContextObject};
        if ((Context.Bind(CallerId) != SchedulingContextError::None) || (Caller.MakeReady() != ThreadError::None) ||
            (Server.MakeReady() != ThreadError::None) || (Caller.BeginRun(CpuId{0U}) != ThreadError::None))
        {
            return false;
        }
        const auto blocked = Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        if (!blocked.HasValue())
        {
            return false;
        }
        BlockToken = blocked.GetValue().BlockToken;
        const auto replyDomain = Revocations.CreateRoot();
        ReplyDomain = replyDomain.HasValue() ? replyDomain.GetValue() : RevocationDomainId{};
        return BlockToken.IsValid() && replyDomain.HasValue();
    }

    /// Gives the server the Invoke-only ReplyToken capability that a Call
    /// would have delivered.
    [[nodiscard]] CapabilityHandle MintReplyCapability(const ObjectId tokenObject) noexcept
    {
        const auto minted = Issuer.Mint(ServerSpace, tokenObject, CapabilityRights{CapabilityRight::Invoke},
                                        ReplyDomain, MonotonicTime::Infinite(), 0x5E1EC7U);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] bool InitializeAndRegisterToken(ReplyToken& token, ObjectId& tokenObject,
                                                  const UInt64 transaction) noexcept
    {
        const auto initialized =
            token.Initialize(Registry, ResourceDomainId{7U}, CallerId, ContextId, BlockToken, transaction);
        if (initialized.Error != ReplyTokenError::None)
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(token);
        if (!registered.HasValue())
        {
            return false;
        }
        tokenObject = registered.GetValue();
        return true;
    }

    [[nodiscard]] bool RetireToken(const ObjectId tokenObject) noexcept
    {
        return (Issuer.DestroyObject(tokenObject) == ObjectRegistryError::Ok) &&
               Issuer.CollectDestroyedObject().HasValue();
    }

    [[nodiscard]] bool RetireObjects() noexcept
    {
        static_cast<void>(ReplyEndpoint.DiscardUnread());
        const auto queueState = WakeQueue.GetState();
        if ((queueState.ReadyEntries != 0U) && (WakeQueue.Remove(CallerId) != ReadyQueueError::None))
        {
            return false;
        }
        if ((queueState.ClaimedEntries != 0U) || (queueState.ReservedEntries != 0U) ||
            (Context.GetSnapshot().DonationDepth != 0U))
        {
            return false;
        }
        if (Context.Unbind(CallerId) != SchedulingContextError::None)
        {
            return false;
        }
        const auto callerSnapshot = Caller.GetSnapshot();
        if ((callerSnapshot.State == ThreadState::Blocked) &&
            (Caller.Wake(callerSnapshot.BlockToken) != ThreadError::None))
        {
            return false;
        }
        if ((Caller.Terminate(0U) != ThreadError::None) || (Server.Terminate(0U) != ThreadError::None))
        {
            return false;
        }
        if ((Issuer.DestroyObject(ContextObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(CallerObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(ServerObject) != ObjectRegistryError::Ok))
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
    Hal::Time::IMonotonicClock& ActiveClock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace ServerSpace;
    CapabilitySpace CallerSpace;
    Endpoint ReplyEndpoint;
    ReadyQueue WakeQueue;
    Thread Caller;
    Thread Server;
    SchedulingContext Context;
    ObjectId CallerObject{};
    ObjectId ServerObject{};
    ObjectId ContextObject{};
    ThreadId CallerId{};
    ThreadId ServerId{};
    SchedulingContextId ContextId{};
    RevocationDomainId ReplyDomain{};
    Kernel::Scheduler::ThreadBlockToken BlockToken{};
};

void TestReplyCommit(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x1122'3344'5566'7788ULL};
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "reply fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction);
        SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
        const auto initialized = tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
        const auto accepted = dispatcher.Accept(fixture.ServerId, fixture.ServerSpace,
                                                fixture.MintReplyCapability(tokenObject), ResourceDomainId{8U});
        const MessageEnvelope reply = MakeMessage(MessageKind::Reply, Transaction, 0xA5U);
        const auto completed = dispatcher.CompleteReply(reply);
        MessageEnvelope received{};
        const auto delivery = fixture.ReplyEndpoint.Receive(received);
        const auto callerSnapshot = fixture.Caller.GetSnapshot();
        const auto contextSnapshot = fixture.Context.GetSnapshot();
        const auto queueState = fixture.WakeQueue.GetState();
        ASTERIA_EXPECT(runner, "reply publishes before exact wake and ready-queue visibility", tokenReady,
                       FailureIs(initialized, SynchronousReplyDispatcherError::None),
                       FailureIs(accepted, SynchronousReplyDispatcherError::None), completed.HasValue(),
                       completed.GetValue().Caller.IsEqualTo(fixture.CallerId),
                       completed.GetValue().TransactionIdentifier == Transaction,
                       completed.GetValue().EndpointSequence == 1U, completed.GetValue().QueueSequence == 1U,
                       completed.GetValue().State == SynchronousReplyDispatcherState::Replied, delivery.HasValue(),
                       received.Kind == MessageKind::Reply, received.TransactionIdentifier == Transaction,
                       received.Payload[0U] == 0xA5U, callerSnapshot.State == ThreadState::Ready,
                       contextSnapshot.CurrentThread.IsEqualTo(fixture.CallerId), contextSnapshot.DonationDepth == 0U,
                       queueState.ReadyEntries == 1U, queueState.ReservedEntries == 0U,
                       token.GetSnapshot().State == ReplyTokenState::Replied);

        const auto replay = dispatcher.CompleteReply(reply);
        ASTERIA_EXPECT(runner, "terminal reply cannot replay and releases dispatcher pins", !replay.HasValue(),
                       FailureIs(replay.GetError(), SynchronousReplyDispatcherError::AlreadyCompleted),
                       fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "reply fixture retires cleanly", fixture.RetireObjects());
}

void TestPendingAndAcceptedCancellation(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x2233'4455'6677'8899ULL};
    {
        ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "pending cancel fixture initializes", fixture.Initialize());
        {
            ReplyToken token;
            ObjectId tokenObject{};
            const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction);
            SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            const auto initialized =
                tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
            const auto cancelled = dispatcher.Cancel();
            MessageEnvelope output{};
            const auto noReply = fixture.ReplyEndpoint.Receive(output);
            ASTERIA_EXPECT(runner, "pending cancellation wakes without fabricating a reply", tokenReady,
                           FailureIs(initialized, SynchronousReplyDispatcherError::None), cancelled.HasValue(),
                           cancelled.GetValue().EndpointSequence == 0U, cancelled.GetValue().QueueSequence == 1U,
                           cancelled.GetValue().State == SynchronousReplyDispatcherState::Cancelled,
                           !noReply.HasValue(), noReply.GetError().Code == EndpointErrorCode::WouldBlock,
                           fixture.Caller.GetSnapshot().State == ThreadState::Ready,
                           fixture.WakeQueue.GetState().ReadyEntries == 1U,
                           token.GetSnapshot().State == ReplyTokenState::Cancelled,
                           fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                           fixture.RetireToken(tokenObject));
        }
        ASTERIA_EXPECT(runner, "pending cancel fixture retires cleanly", fixture.RetireObjects());
    }

    {
        ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "accepted cancel fixture initializes", fixture.Initialize());
        {
            ReplyToken token;
            ObjectId tokenObject{};
            const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction + 1U);
            SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            const auto initialized =
                tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
            const auto accepted = dispatcher.Accept(fixture.ServerId, fixture.ServerSpace,
                                                    fixture.MintReplyCapability(tokenObject), ResourceDomainId{8U});
            const auto cancelled = dispatcher.Cancel();
            ASTERIA_EXPECT(runner, "accepted cancellation returns donation before wake", tokenReady,
                           FailureIs(initialized, SynchronousReplyDispatcherError::None),
                           FailureIs(accepted, SynchronousReplyDispatcherError::None), cancelled.HasValue(),
                           fixture.Context.GetSnapshot().DonationDepth == 0U,
                           fixture.Context.GetSnapshot().CurrentThread.IsEqualTo(fixture.CallerId),
                           fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                           fixture.RetireToken(tokenObject));
        }
        ASTERIA_EXPECT(runner, "accepted cancel fixture retires cleanly", fixture.RetireObjects());
    }
}

void TestPreparationRollbackAndRetry(TestContext& runner) noexcept
{
    constexpr EndpointConfiguration OneMessage{
        .MaximumQueuedBytes = 504U,
        .ReservedControlBytes = 0U,
        .MaximumBytesPerSender = 504U,
        .MaximumQueuedMessages = 1U,
        .ReservedControlMessages = 0U,
        .MaximumMessagesPerSender = 1U,
        .MaximumQueuedAttachments = 4U,
        .ReservedControlAttachments = 0U,
        .MaximumAttachmentsPerSender = 4U,
        .Reserved = 0U,
    };
    constexpr UInt64 Transaction{0x3344'5566'7788'99AAULL};
    ReplyFixture fixture{OneMessage};
    ASTERIA_EXPECT(runner, "rollback fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction);
        SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
        const auto initialized = tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
        const auto accepted = dispatcher.Accept(fixture.ServerId, fixture.ServerSpace,
                                                fixture.MintReplyCapability(tokenObject), ResourceDomainId{8U});
        const auto filler =
            fixture.ReplyEndpoint.Send(fixture.ServerSpace, ResourceDomainId{9U}, EndpointSendClass::Normal,
                                       MakeMessage(MessageKind::Asynchronous, 1U, 0x11U));
        const MessageEnvelope reply = MakeMessage(MessageKind::Reply, Transaction, 0x22U);
        const auto blocked = dispatcher.CompleteReply(reply);
        const auto callerWhileBlocked = fixture.Caller.GetSnapshot();
        const auto queueAfterFailure = fixture.WakeQueue.GetState();
        MessageEnvelope fillerOutput{};
        const auto drained = fixture.ReplyEndpoint.Receive(fillerOutput);
        const auto retried = dispatcher.CompleteReply(reply);
        ASTERIA_EXPECT(runner, "endpoint backpressure rolls queue reservation back for retry", tokenReady,
                       FailureIs(initialized, SynchronousReplyDispatcherError::None),
                       FailureIs(accepted, SynchronousReplyDispatcherError::None), filler.HasValue(),
                       !blocked.HasValue(),
                       FailureIs(blocked.GetError(), SynchronousReplyDispatcherError::EndpointPreparationRejected),
                       blocked.GetError().Endpoint.Code == EndpointErrorCode::WouldBlock,
                       callerWhileBlocked.State == ThreadState::Blocked, queueAfterFailure.ReadyEntries == 0U,
                       queueAfterFailure.ReservedEntries == 0U,
                       dispatcher.GetSnapshot().State == SynchronousReplyDispatcherState::Replied, drained.HasValue(),
                       retried.HasValue(), fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "rollback fixture retires cleanly", fixture.RetireObjects());
}

void TestReplyAdmissionAndStoppedGates(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x3A4B'5C6D'7E8F'9012ULL};
    {
        static ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "reply validation fixture initializes", fixture.Initialize());
        {
            ReplyToken token;
            ObjectId tokenObject{};
            const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction);
            SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            const auto initialized =
                tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
            const CapabilityHandle replyCapability = fixture.MintReplyCapability(tokenObject);
            const auto wrongOwner =
                dispatcher.Accept(fixture.ServerId, fixture.ServerSpace, replyCapability, ResourceDomainId{9U});
            const auto pendingAfterWrongOwner = dispatcher.GetSnapshot();
            const auto tokenAfterWrongOwner = token.GetSnapshot();
            const auto contextAfterWrongOwner = fixture.Context.GetSnapshot();
            const auto accepted =
                dispatcher.Accept(fixture.ServerId, fixture.ServerSpace, replyCapability, ResourceDomainId{8U});

            MessageEnvelope wrongKind = MakeMessage(MessageKind::Asynchronous, Transaction, 0x21U);
            MessageEnvelope wrongCancellation = MakeMessage(MessageKind::Reply, Transaction, 0x22U);
            wrongCancellation.Cancellation = CancellationState::Requested;
            const MessageEnvelope wrongTransaction = MakeMessage(MessageKind::Reply, Transaction + 1U, 0x23U);
            const auto rejectedKind = dispatcher.CompleteReply(wrongKind);
            const auto rejectedCancellation = dispatcher.CompleteReply(wrongCancellation);
            const auto rejectedTransaction = dispatcher.CompleteReply(wrongTransaction);
            MessageEnvelope hidden{};
            const auto noReply = fixture.ReplyEndpoint.Receive(hidden);
            const auto acceptedSnapshot = dispatcher.GetSnapshot();
            const auto callerSnapshot = fixture.Caller.GetSnapshot();
            const auto contextSnapshot = fixture.Context.GetSnapshot();
            const auto tokenAfterInvalidReplies = token.GetSnapshot();
            const auto queueAfterInvalidReplies = fixture.WakeQueue.GetState();

            const MessageEnvelope validReply = MakeMessage(MessageKind::Reply, Transaction, 0x24U);
            const auto completed = dispatcher.CompleteReply(validReply);
            const auto delivered = fixture.ReplyEndpoint.Receive(hidden);
            ASTERIA_EXPECT(
                runner, "owner and reply validation reject inertly before a valid retry", tokenReady,
                FailureIs(initialized, SynchronousReplyDispatcherError::None),
                FailureIs(wrongOwner, SynchronousReplyDispatcherError::ServerOwnerMismatch),
                pendingAfterWrongOwner.State == SynchronousReplyDispatcherState::Pending,
                tokenAfterWrongOwner.State == ReplyTokenState::Pending, contextAfterWrongOwner.DonationDepth == 0U,
                FailureIs(accepted, SynchronousReplyDispatcherError::None), !rejectedKind.HasValue(),
                FailureIs(rejectedKind.GetError(), SynchronousReplyDispatcherError::InvalidReply),
                !rejectedCancellation.HasValue(),
                FailureIs(rejectedCancellation.GetError(), SynchronousReplyDispatcherError::InvalidReply),
                !rejectedTransaction.HasValue(),
                FailureIs(rejectedTransaction.GetError(), SynchronousReplyDispatcherError::InvalidReply),
                !noReply.HasValue(), noReply.GetError().Code == EndpointErrorCode::WouldBlock,
                acceptedSnapshot.State == SynchronousReplyDispatcherState::Accepted,
                callerSnapshot.State == ThreadState::Blocked, contextSnapshot.DonationDepth == 1U,
                contextSnapshot.CurrentThread.IsEqualTo(fixture.ServerId),
                tokenAfterInvalidReplies.State == ReplyTokenState::Accepted,
                queueAfterInvalidReplies.ReadyEntries == 0U, queueAfterInvalidReplies.ReservedEntries == 0U,
                fixture.WakeQueue.GetState().ReadyEntries == 1U, fixture.WakeQueue.GetState().ReservedEntries == 0U,
                completed.HasValue(), delivered.HasValue(), hidden.Payload[0U] == 0x24U,
                fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None, fixture.RetireToken(tokenObject));
        }
        ASTERIA_EXPECT(runner, "reply validation fixture retires cleanly", fixture.RetireObjects());
    }

    {
        ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "running server fixture initializes", fixture.Initialize());
        {
            ReplyToken token;
            ObjectId tokenObject{};
            const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction + 1U);
            SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            const auto initialized =
                tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
            const auto accepted = dispatcher.Accept(fixture.ServerId, fixture.ServerSpace,
                                                    fixture.MintReplyCapability(tokenObject), ResourceDomainId{8U});
            const ThreadError began = fixture.Server.BeginRun(CpuId{0U});
            const auto rejected = dispatcher.CompleteReply(MakeMessage(MessageKind::Reply, Transaction + 1U));
            const auto stopped = fixture.Server.FinishRun(CpuId{0U}, ThreadRunDisposition::Ready);
            const auto cancelled = dispatcher.Cancel();
            ASTERIA_EXPECT(runner, "a running server must stop before reply or cancellation commits", tokenReady,
                           FailureIs(initialized, SynchronousReplyDispatcherError::None),
                           FailureIs(accepted, SynchronousReplyDispatcherError::None), began == ThreadError::None,
                           !rejected.HasValue(),
                           FailureIs(rejected.GetError(), SynchronousReplyDispatcherError::ServerStillRunning),
                           dispatcher.GetSnapshot().State == SynchronousReplyDispatcherState::Cancelled,
                           stopped.HasValue(), cancelled.HasValue(),
                           fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                           fixture.RetireToken(tokenObject));
        }
        ASTERIA_EXPECT(runner, "running server fixture retires cleanly", fixture.RetireObjects());
    }

    {
        ReplyFixture fixture;
        ASTERIA_EXPECT(runner, "running context fixture initializes", fixture.Initialize());
        {
            ReplyToken token;
            ObjectId tokenObject{};
            const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction + 2U);
            SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            const auto initialized =
                tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
            const auto accepted = dispatcher.Accept(fixture.ServerId, fixture.ServerSpace,
                                                    fixture.MintReplyCapability(tokenObject), ResourceDomainId{8U});
            const auto began = fixture.Context.StartRun(fixture.ServerId, CpuId{0U}, MonotonicTime{10U});
            const auto rejected = dispatcher.CompleteReply(MakeMessage(MessageKind::Reply, Transaction + 2U));
            const auto stopped = fixture.Context.EndRun(CpuId{0U}, MonotonicTime{11U});
            const auto cancelled = dispatcher.Cancel();
            ASTERIA_EXPECT(
                runner, "a running donated context must stop before terminal publication", tokenReady,
                FailureIs(initialized, SynchronousReplyDispatcherError::None),
                FailureIs(accepted, SynchronousReplyDispatcherError::None), began.HasValue(), !rejected.HasValue(),
                FailureIs(rejected.GetError(), SynchronousReplyDispatcherError::ServerStillRunning), stopped.HasValue(),
                cancelled.HasValue(), fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                fixture.RetireToken(tokenObject));
        }
        ASTERIA_EXPECT(runner, "running context fixture retires cleanly", fixture.RetireObjects());
    }
}

void TestAcceptRequiresReplyAuthority(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x5A5A'6B6B'7C7C'8D8DULL};
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "reply-authority fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ReplyToken otherToken;
        ObjectId tokenObject{};
        const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction);
        const auto otherConfigured = otherToken.Configure(ResourceDomainId{7U});
        const auto otherObject = fixture.Issuer.CreateObject(otherToken);
        const auto revokedDomain = fixture.Revocations.CreateRoot();
        const CapabilityHandle otherCapability =
            otherObject.HasValue() ? fixture.MintReplyCapability(otherObject.GetValue()) : CapabilityHandle{};
        const auto revokedCapability =
            (revokedDomain.HasValue() && tokenReady)
                ? fixture.Issuer.Mint(fixture.ServerSpace, tokenObject, CapabilityRights{CapabilityRight::Invoke},
                                      revokedDomain.GetValue(), MonotonicTime::Infinite(), 0U)
                : Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidArgument);
        const auto revoked = revokedDomain.HasValue() ? fixture.Revocations.Revoke(revokedDomain.GetValue())
                                                      : Kernel::Object::RevocationError::InvalidDomain;
        const CapabilityHandle replyCapability = fixture.MintReplyCapability(tokenObject);
        const Foundation::Size capabilitiesBefore = fixture.ServerSpace.GetActiveCount();
        SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
        const auto initialized = tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
        const auto withoutAuthority =
            dispatcher.Accept(fixture.ServerId, fixture.ServerSpace, CapabilityHandle{}, ResourceDomainId{8U});
        const auto withRevokedAuthority = dispatcher.Accept(
            fixture.ServerId, fixture.ServerSpace,
            revokedCapability.HasValue() ? revokedCapability.GetValue() : CapabilityHandle{}, ResourceDomainId{8U});
        const auto withOtherToken =
            dispatcher.Accept(fixture.ServerId, fixture.ServerSpace, otherCapability, ResourceDomainId{8U});
        const auto contextAfterRejections = fixture.Context.GetSnapshot();
        const Foundation::Size capabilitiesAfterRejections = fixture.ServerSpace.GetActiveCount();
        const auto accepted =
            dispatcher.Accept(fixture.ServerId, fixture.ServerSpace, replyCapability, ResourceDomainId{8U});
        const auto consumed = fixture.ServerSpace.Lookup(replyCapability, Kernel::Object::ObjectType::ReplyToken,
                                                         Kernel::Object::CapabilityOperation::Invoke);
        const auto cancelled = dispatcher.Cancel();
        ASTERIA_EXPECT(runner, "Accept requires the exact ReplyToken capability and consumes it", tokenReady,
                       otherConfigured.Error == ReplyTokenError::None, otherCapability.IsValid(),
                       revokedCapability.HasValue(), revoked == Kernel::Object::RevocationError::Ok,
                       replyCapability.IsValid(), FailureIs(initialized, SynchronousReplyDispatcherError::None),
                       FailureIs(withoutAuthority, SynchronousReplyDispatcherError::ReplyCapabilityRejected),
                       withoutAuthority.Capability == CapabilityError::InvalidHandle,
                       FailureIs(withRevokedAuthority, SynchronousReplyDispatcherError::ReplyCapabilityRejected),
                       withRevokedAuthority.Capability == CapabilityError::Revoked,
                       FailureIs(withOtherToken, SynchronousReplyDispatcherError::ReplyCapabilityMismatch),
                       contextAfterRejections.DonationDepth == 0U, capabilitiesAfterRejections == capabilitiesBefore,
                       FailureIs(accepted, SynchronousReplyDispatcherError::None),
                       fixture.ServerSpace.GetActiveCount() == (capabilitiesBefore - 1U), !consumed.HasValue(),
                       consumed.GetError() == CapabilityError::InvalidHandle, cancelled.HasValue(),
                       fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                       fixture.RetireToken(tokenObject), otherObject.HasValue(),
                       fixture.RetireToken(otherObject.GetValue()));
    }
    ASTERIA_EXPECT(runner, "reply-authority fixture retires cleanly", fixture.RetireObjects());
}

// The donated context starts running inside the reply's capability transfer,
// after ServerIsStopped admitted the reply: returning the donation then fails,
// so the dispatcher faults. Its rollback must restore what the prepared reply
// moved instead of destroying it.
void TestFaultedReplyRestoresMovedAttachment(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x6B6B'7C7C'8D8D'9E9EULL};
    HookClock clock;
    ReplyFixture fixture{EndpointConfiguration::Default(), &clock};
    ASTERIA_EXPECT(runner, "faulted-reply fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ReplyToken movedObject;
        ObjectId tokenObject{};
        const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction);
        const auto movedConfigured = movedObject.Configure(ResourceDomainId{8U});
        const auto movedRegistered = fixture.Issuer.CreateObject(movedObject);
        const auto moved =
            movedRegistered.HasValue()
                ? fixture.Issuer.Mint(fixture.ServerSpace, movedRegistered.GetValue(),
                                      CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Transfer),
                                      fixture.ReplyDomain, MonotonicTime::Infinite(), 0x0B1EC7U)
                : Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidArgument);
        SynchronousReplyDispatcherFailure accepted{};
        Foundation::Result<Kernel::Ipc::SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure> replied =
            Foundation::Result<Kernel::Ipc::SynchronousReplyDispatcherReceipt,
                               SynchronousReplyDispatcherFailure>::Failure({});
        SynchronousReplyDispatcherState faultedState{SynchronousReplyDispatcherState::Uninitialized};
        {
            SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            static_cast<void>(dispatcher.Initialize(tokenObject));
            accepted = dispatcher.Accept(fixture.ServerId, fixture.ServerSpace,
                                         fixture.MintReplyCapability(tokenObject), ResourceDomainId{8U});
            MessageEnvelope reply = MakeMessage(MessageKind::Reply, Transaction, 0x3CU);
            reply.Attachments[0U] = Kernel::Ipc::CapabilityAttachment{
                .Handle = moved.HasValue() ? moved.GetValue() : CapabilityHandle{},
                .RequestedDeadline = MonotonicTime::Infinite(),
                .RequestedRights = CapabilityRights{CapabilityRight::Inspect},
                .ExpectedType = Kernel::Object::ObjectType::ReplyToken,
                .TransferMode = Kernel::Ipc::CapabilityTransferMode::Move,
                .Reserved = 0U,
            };
            reply.AttachmentCount = 1U;
            clock.ArmOnce(
                [](void* const context) noexcept
                {
                    ReplyFixture& owner = *static_cast<ReplyFixture*>(context);
                    static_cast<void>(owner.Context.StartRun(owner.ServerId, CpuId{0U}, MonotonicTime{10U}));
                },
                &fixture);
            replied = dispatcher.CompleteReply(reply);
            faultedState = dispatcher.GetSnapshot().State;
        }
        const auto restored = moved.HasValue()
                                  ? fixture.ServerSpace.Query(moved.GetValue())
                                  : Foundation::Result<Kernel::Object::CapabilityDescriptor, CapabilityError>::Failure(
                                        CapabilityError::InvalidHandle);
        const auto stopped = fixture.Context.EndRun(CpuId{0U}, MonotonicTime{10U});
        const auto cancelled = token.Cancel();
        const auto afterRollback =
            fixture.ReplyEndpoint.Send(fixture.ServerSpace, ResourceDomainId{8U}, EndpointSendClass::Normal,
                                       MakeMessage(MessageKind::Asynchronous, 1U, 0x11U));
        ASTERIA_EXPECT(runner, "a reply that faults after its preparation restores the capability it moved", tokenReady,
                       movedConfigured.Error == ReplyTokenError::None, moved.HasValue(),
                       FailureIs(accepted, SynchronousReplyDispatcherError::None), !replied.HasValue(),
                       FailureIs(replied.GetError(), SynchronousReplyDispatcherError::TokenRejected),
                       replied.GetError().Token.Error == ReplyTokenError::ReturnRejected,
                       faultedState == SynchronousReplyDispatcherState::Faulted, restored.HasValue(),
                       fixture.CallerSpace.GetActiveCount() == 0U, fixture.WakeQueue.GetState().ReservedEntries == 0U,
                       afterRollback.HasValue(), afterRollback.GetValue().Sequence == 1U, stopped.HasValue(),
                       cancelled.HasValue(), fixture.ServerSpace.Close(moved.GetValue()) == CapabilityError::Ok,
                       fixture.RetireToken(tokenObject), fixture.RetireToken(movedRegistered.GetValue()));
    }
    ASTERIA_EXPECT(runner, "faulted-reply fixture retires cleanly", fixture.RetireObjects());
}

void TestReplyCancelRace(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x4455'6677'8899'AABBULL};
    ReplyFixture fixture;
    ASTERIA_EXPECT(runner, "reply-cancel race fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        const bool tokenReady = fixture.InitializeAndRegisterToken(token, tokenObject, Transaction);
        SynchronousReplyDispatcher dispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
        const auto initialized = tokenReady ? dispatcher.Initialize(tokenObject) : SynchronousReplyDispatcherFailure{};
        const auto accepted = dispatcher.Accept(fixture.ServerId, fixture.ServerSpace,
                                                fixture.MintReplyCapability(tokenObject), ResourceDomainId{8U});
        const MessageEnvelope reply = MakeMessage(MessageKind::Reply, Transaction, 0x5AU);
        std::atomic<bool> begin{false};
        std::atomic<bool> replySucceeded{false};
        std::atomic<bool> cancelSucceeded{false};
        SynchronousReplyDispatcherError replyError{SynchronousReplyDispatcherError::None};
        SynchronousReplyDispatcherError cancelError{SynchronousReplyDispatcherError::None};

        std::thread replyThread{[&]() noexcept
                                {
                                    while (!begin.load())
                                    {
                                        std::this_thread::yield();
                                    }
                                    const auto result = dispatcher.CompleteReply(reply);
                                    replySucceeded.store(result.HasValue());
                                    if (!result.HasValue())
                                    {
                                        replyError = result.GetError().Error;
                                    }
                                }};
        std::thread cancelThread{[&]() noexcept
                                 {
                                     while (!begin.load())
                                     {
                                         std::this_thread::yield();
                                     }
                                     const auto result = dispatcher.Cancel();
                                     cancelSucceeded.store(result.HasValue());
                                     if (!result.HasValue())
                                     {
                                         cancelError = result.GetError().Error;
                                     }
                                 }};
        begin.store(true);
        replyThread.join();
        cancelThread.join();

        MessageEnvelope output{};
        const auto delivered = fixture.ReplyEndpoint.Receive(output);
        const bool exactlyOne = replySucceeded.load() != cancelSucceeded.load();
        const bool loserTyped = replySucceeded.load()
                                    ? (cancelError == SynchronousReplyDispatcherError::AlreadyCompleted)
                                    : (replyError == SynchronousReplyDispatcherError::AlreadyCompleted);
        ASTERIA_EXPECT(
            runner, "concurrent reply and cancel have one terminal winner", tokenReady,
            FailureIs(initialized, SynchronousReplyDispatcherError::None),
            FailureIs(accepted, SynchronousReplyDispatcherError::None), exactlyOne, loserTyped,
            delivered.HasValue() == replySucceeded.load(), fixture.Caller.GetSnapshot().State == ThreadState::Ready,
            fixture.WakeQueue.GetState().ReadyEntries == 1U, fixture.WakeQueue.GetState().ReservedEntries == 0U,
            fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None, fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "reply-cancel race fixture retires cleanly", fixture.RetireObjects());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SynchronousReplyDispatcherTests"};
    Asteria::Test::TestReplyCommit(runner);
    Asteria::Test::TestPendingAndAcceptedCancellation(runner);
    Asteria::Test::TestPreparationRollbackAndRetry(runner);
    Asteria::Test::TestReplyAdmissionAndStoppedGates(runner);
    Asteria::Test::TestAcceptRequiresReplyAuthority(runner);
    Asteria::Test::TestFaultedReplyRestoresMovedAttachment(runner);
    Asteria::Test::TestReplyCancelRace(runner);
    return runner.Finish();
}
