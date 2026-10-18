// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <optional>

#include "Kernel/Ipc/SynchronousCallDispatcher.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Synchronization/AtomicReferenceCount.hpp"

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
using Kernel::Ipc::SynchronousCallDispatcher;
using Kernel::Ipc::SynchronousCallDispatcherError;
using Kernel::Ipc::SynchronousCallDispatcherState;
using Kernel::Ipc::SynchronousCallPrepareRequest;
using Kernel::Ipc::SynchronousReplyDispatcher;
using Kernel::Ipc::SynchronousReplyDispatcherError;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::BlockedRunCommitOutcome;
using Kernel::Scheduler::MaximumReadyQueueEntries;
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
using Kernel::Scheduler::ThreadBlockToken;
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
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr EndpointConfiguration SingleMessageConfiguration() noexcept
{
    return EndpointConfiguration{
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

class CallFixture final
{
public:
    explicit CallFixture(const EndpointConfiguration callConfiguration = EndpointConfiguration::Default(),
                         const Foundation::UInt32 maximumReferencesPerObject =
                             Platform::Synchronization::AtomicReferenceCount::MaximumValue) noexcept
        : Registry{Synchronization, maximumReferencesPerObject}, Revocations{Synchronization}, Issuer{Registry},
          ServerSpace{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          CallerSpace{Registry, Revocations, Clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          CallEndpoint{ServerSpace, ResourceDomainId{8U}, callConfiguration},
          ReplyEndpoint{CallerSpace, ResourceDomainId{7U}}, WakeQueue{Registry, CpuId{0U}}, Context{Registry}
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
        const auto callEndpointObject = Issuer.CreateObject(CallEndpoint);
        const auto replyEndpointObject = Issuer.CreateObject(ReplyEndpoint);
        const auto endpointDomain = Revocations.CreateRoot();
        if (!callerObject.HasValue() || !serverObject.HasValue() || !contextObject.HasValue() ||
            !callEndpointObject.HasValue() || !replyEndpointObject.HasValue() || !endpointDomain.HasValue())
        {
            return false;
        }
        CallerObject = callerObject.GetValue();
        ServerObject = serverObject.GetValue();
        ContextObject = contextObject.GetValue();
        CallEndpointObject = callEndpointObject.GetValue();
        ReplyEndpointObject = replyEndpointObject.GetValue();
        EndpointDomain = endpointDomain.GetValue();
        CallerId = ThreadId{Registry, CallerObject};
        ServerId = ThreadId{Registry, ServerObject};
        ContextId = SchedulingContextId{Registry, ContextObject};
        const auto callEndpointCapability = Issuer.Mint(
            CallerSpace, CallEndpointObject, CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read),
            EndpointDomain, MonotonicTime::Infinite(), CallEndpointSelector);
        if (!callEndpointCapability.HasValue())
        {
            return false;
        }
        CallEndpointCapability = callEndpointCapability.GetValue();
        return (Context.Bind(CallerId) == SchedulingContextError::None) && (Caller.MakeReady() == ThreadError::None) &&
               (Server.MakeReady() == ThreadError::None) &&
               Context.StartRun(CallerId, CpuId{0U}, MonotonicTime{0U}).HasValue() &&
               (Caller.BeginRun(CpuId{0U}) == ThreadError::None);
    }

    [[nodiscard]] CapabilityHandle MintCallEndpointCapability(const CapabilityRights rights,
                                                              const UInt64 sealedObjectSelector) noexcept
    {
        const auto minted = Issuer.Mint(CallerSpace, CallEndpointObject, rights, EndpointDomain,
                                        MonotonicTime::Infinite(), sealedObjectSelector);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] CapabilityHandle MintReplyEndpointCapability(const CapabilityRights rights,
                                                               const UInt64 sealedObjectSelector) noexcept
    {
        const auto minted = Issuer.Mint(CallerSpace, ReplyEndpointObject, rights, EndpointDomain,
                                        MonotonicTime::Infinite(), sealedObjectSelector);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] bool ConfigureToken(ReplyToken& token, ObjectId& tokenObject,
                                      CapabilityHandle& tokenCapability) noexcept
    {
        const auto domain = Revocations.CreateRoot();
        if ((token.Configure(ResourceDomainId{7U}).Error != ReplyTokenError::None) || !domain.HasValue())
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(token);
        if (!registered.HasValue())
        {
            return false;
        }
        tokenObject = registered.GetValue();
        const auto minted = Issuer.Mint(CallerSpace, tokenObject,
                                        CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Transfer),
                                        domain.GetValue(), MonotonicTime::Infinite(), 0xA57E'CA11U);
        if (!minted.HasValue())
        {
            return false;
        }
        tokenCapability = minted.GetValue();
        return true;
    }

    [[nodiscard]] SynchronousCallPrepareRequest MakeRequest(const MessageEnvelope& call,
                                                            const CapabilityHandle tokenCapability) noexcept
    {
        return SynchronousCallPrepareRequest{
            .CallerSpace = &CallerSpace,
            .Call = &call,
            .Caller = CallerId,
            .SchedulingContext = ContextId,
            .CallEndpointCapability = CallEndpointCapability,
            .ReplyTokenCapability = tokenCapability,
            .Sender = ResourceDomainId{7U},
        };
    }

    [[nodiscard]] ThreadBlockToken BlockCaller() noexcept
    {
        const auto blocked = Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        const auto stopped = Context.EndRun(CpuId{0U}, MonotonicTime{1U});
        return blocked.HasValue() && stopped.HasValue() ? blocked.GetValue().BlockToken : ThreadBlockToken{};
    }

    [[nodiscard]] bool RetireToken(const ObjectId tokenObject) noexcept
    {
        return (Issuer.DestroyObject(tokenObject) == ObjectRegistryError::Ok) &&
               Issuer.CollectDestroyedObject().HasValue();
    }

    [[nodiscard]] bool Retire() noexcept
    {
        static_cast<void>(CallEndpoint.DiscardUnread());
        static_cast<void>(ReplyEndpoint.DiscardUnread());

        if (CallerSpace.Close(CallEndpointCapability) != CapabilityError::Ok)
        {
            return false;
        }

        const SchedulingContextState contextState = Context.GetSnapshot().State;
        if ((contextState == SchedulingContextState::Running) &&
            !Context.EndRun(CpuId{0U}, MonotonicTime{1U}).HasValue())
        {
            return false;
        }
        const ThreadState callerState = Caller.GetSnapshot().State;
        if (callerState == ThreadState::Running)
        {
            if (!Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Ready).HasValue())
            {
                return false;
            }
        }
        else if (callerState == ThreadState::Blocked)
        {
            const auto snapshot = Caller.GetSnapshot();
            if (Caller.Wake(snapshot.BlockToken) != ThreadError::None)
            {
                return false;
            }
        }
        if ((WakeQueue.GetState().ReadyEntries != 0U) && (WakeQueue.Remove(CallerId) != ReadyQueueError::None))
        {
            return false;
        }
        if ((Context.GetSnapshot().DonationDepth != 0U) || (Context.Unbind(CallerId) != SchedulingContextError::None) ||
            (Caller.Terminate(0U) != ThreadError::None) || (Server.Terminate(0U) != ThreadError::None))
        {
            return false;
        }
        if ((Issuer.DestroyObject(CallEndpointObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(ReplyEndpointObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(ContextObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(CallerObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(ServerObject) != ObjectRegistryError::Ok))
        {
            return false;
        }
        for (Foundation::UInt8 index{0U}; index < 5U; ++index)
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
    CapabilitySpace ServerSpace;
    CapabilitySpace CallerSpace;
    Endpoint CallEndpoint;
    Endpoint ReplyEndpoint;
    ReadyQueue WakeQueue;
    Thread Caller;
    Thread Server;
    SchedulingContext Context;
    ObjectId CallerObject{};
    ObjectId ServerObject{};
    ObjectId ContextObject{};
    ObjectId CallEndpointObject{};
    ObjectId ReplyEndpointObject{};
    RevocationDomainId EndpointDomain{};
    CapabilityHandle CallEndpointCapability{};
    ThreadId CallerId{};
    ThreadId ServerId{};
    SchedulingContextId ContextId{};

    static constexpr UInt64 CallEndpointSelector{0xE11D'BEEFU};
};

[[nodiscard]] bool CancelPublishedCall(CallFixture& fixture, SynchronousReplyDispatcher& replyDispatcher,
                                       const UInt64 transaction) noexcept
{
    MessageEnvelope receivedCall{};
    const auto received = fixture.CallEndpoint.Receive(receivedCall);
    if (!received.HasValue() || (receivedCall.TransactionIdentifier != transaction) ||
        (receivedCall.AttachmentCount != 1U))
    {
        return false;
    }
    const CapabilityHandle serverTokenCapability = receivedCall.Attachments[0U].Handle;
    const auto cancelled = replyDispatcher.Cancel();
    return serverTokenCapability.IsValid() && cancelled.HasValue() &&
           (fixture.ServerSpace.Close(serverTokenCapability) == CapabilityError::Ok) &&
           (fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None);
}

void TestPrepareIsInertAndCancellable(TestContext& runner) noexcept
{
    CallFixture fixture;
    ASTERIA_EXPECT(runner, "prepare fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        const CapabilityHandle wrongEndpointCapability =
            fixture.MintReplyEndpointCapability(CapabilityRights{CapabilityRight::Invoke}, 0xBAD0'E11DU);
        const CapabilityHandle noInvokeEndpointCapability =
            fixture.MintCallEndpointCapability(CapabilityRights{CapabilityRight::Read}, 0xBAD0'CA11U);
        {
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            MessageEnvelope finite = MakeMessage(MessageKind::SynchronousCall, 0x1111U);
            finite.Deadline = MonotonicTime{100U};
            const auto rejected = dispatcher.Prepare(fixture.MakeRequest(finite, tokenCapability));
            const MessageEnvelope wrongEndpointCall = MakeMessage(MessageKind::SynchronousCall, 0x1111'0001U);
            SynchronousCallPrepareRequest wrongEndpointRequest =
                fixture.MakeRequest(wrongEndpointCall, tokenCapability);
            wrongEndpointRequest.CallEndpointCapability = wrongEndpointCapability;
            const auto wrongEndpointRejected = dispatcher.Prepare(wrongEndpointRequest);
            const auto wrongEndpointSnapshot = dispatcher.GetSnapshot();
            const MessageEnvelope noInvokeCall = MakeMessage(MessageKind::SynchronousCall, 0x1111'0002U);
            SynchronousCallPrepareRequest noInvokeRequest = fixture.MakeRequest(noInvokeCall, tokenCapability);
            noInvokeRequest.CallEndpointCapability = noInvokeEndpointCapability;
            const auto noInvokeRejected = dispatcher.Prepare(noInvokeRequest);
            MessageEnvelope malformed = MakeMessage(MessageKind::SynchronousCall, 0x1112U);
            malformed.Reserved = 1U;
            const auto malformedRejected = dispatcher.Prepare(fixture.MakeRequest(malformed, tokenCapability));
            const auto failedSnapshot = dispatcher.GetSnapshot();
            const MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, 0x1122'3344U, 0xA5U);
            const auto prepared = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));
            const MessageEnvelope replacement = MakeMessage(MessageKind::SynchronousCall, 0x9988'7766U, 0x3CU);
            const auto duplicate = dispatcher.Prepare(fixture.MakeRequest(replacement, tokenCapability));
            const auto preparedSnapshot = dispatcher.GetSnapshot();
            bool sourceCapabilityIntact{false};
            {
                const auto lookup =
                    fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
                sourceCapabilityIntact = lookup.HasValue();
            }
            const auto cancelled = dispatcher.CancelPrepared();
            ASTERIA_EXPECT(
                runner, "Prepare validates and snapshots without external mutation", tokenReady,
                wrongEndpointCapability.IsValid(), noInvokeEndpointCapability.IsValid(),
                rejected.Error == SynchronousCallDispatcherError::UnsupportedDeadline,
                wrongEndpointRejected.Error == SynchronousCallDispatcherError::CallEndpointCapabilityMismatch,
                wrongEndpointSnapshot.State == SynchronousCallDispatcherState::Fresh,
                wrongEndpointSnapshot.RunSequence == 0U, !wrongEndpointSnapshot.Caller.IsValid(),
                noInvokeRejected.Error == SynchronousCallDispatcherError::CallEndpointCapabilityRejected,
                noInvokeRejected.Capability == CapabilityError::RightsDenied,
                malformedRejected.Error == SynchronousCallDispatcherError::MessageValidationRejected,
                failedSnapshot.State == SynchronousCallDispatcherState::Fresh, !failedSnapshot.Caller.IsValid(),
                !failedSnapshot.SchedulingContext.IsValid(), !failedSnapshot.BlockToken.IsValid(),
                failedSnapshot.TransactionIdentifier == 0U, failedSnapshot.RunSequence == 0U,
                failedSnapshot.EndpointSequence == 0U, failedSnapshot.Diagnostic.TransactionIdentifier == 0x1112U,
                prepared.Error == SynchronousCallDispatcherError::None,
                duplicate.Error == SynchronousCallDispatcherError::AlreadyPrepared,
                preparedSnapshot.State == SynchronousCallDispatcherState::Prepared,
                preparedSnapshot.Diagnostic.Error == SynchronousCallDispatcherError::None,
                preparedSnapshot.TransactionIdentifier == call.TransactionIdentifier,
                fixture.CallEndpoint.GetQueueState().QueuedMessages == 0U, sourceCapabilityIntact,
                token.GetSnapshot().State == ReplyTokenState::Available,
                cancelled.Error == SynchronousCallDispatcherError::None,
                dispatcher.GetSnapshot().State == SynchronousCallDispatcherState::Fresh);
        }
        ASTERIA_EXPECT(runner, "cancelled preparation leaves the temporary token capability owned", tokenReady,
                       fixture.CallerSpace.Close(wrongEndpointCapability) == CapabilityError::Ok,
                       fixture.CallerSpace.Close(noInvokeEndpointCapability) == CapabilityError::Ok,
                       fixture.CallerSpace.Close(tokenCapability) == CapabilityError::Ok,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "prepare fixture retires cleanly", fixture.Retire());
}

void TestCallReplyTransaction(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x2233'4455'6677'8899ULL};
    CallFixture fixture;
    ASTERIA_EXPECT(runner, "call-reply fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        const CapabilityHandle userCapability = fixture.MintCallEndpointCapability(
            CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Transfer), 0xC411'A77AU);
        CapabilityHandle serverTokenCapability{};
        CapabilityHandle serverUserCapability{};
        {
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, Transaction, 0x5AU);
            call.Attachments[0U] = Kernel::Ipc::CapabilityAttachment{
                .Handle = userCapability,
                .RequestedDeadline = MonotonicTime::Infinite(),
                .RequestedRights = CapabilityRights{CapabilityRight::Invoke},
                .ExpectedType = ObjectType::Endpoint,
                .TransferMode = Kernel::Ipc::CapabilityTransferMode::Move,
                .Reserved = 0U,
            };
            call.AttachmentCount = 1U;
            const auto prepared = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));
            const MessageEnvelope replacement = MakeMessage(MessageKind::SynchronousCall, Transaction + 1U, 0xE1U);
            const auto duplicate = dispatcher.Prepare(fixture.MakeRequest(replacement, tokenCapability));
            const ThreadBlockToken blockToken = fixture.BlockCaller();
            const BlockedRunCommitOutcome committed = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto committedSnapshot = dispatcher.GetSnapshot();
            const BlockedRunCommitOutcome repeated = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto repeatedSnapshot = dispatcher.GetSnapshot();
            MessageEnvelope receivedCall{};
            const auto received = fixture.CallEndpoint.Receive(receivedCall);
            if (received.HasValue() && (receivedCall.AttachmentCount == 2U))
            {
                serverUserCapability = receivedCall.Attachments[0U].Handle;
                serverTokenCapability = receivedCall.Attachments[1U].Handle;
            }
            bool serverUserAuthorityValid{false};
            if (serverUserCapability.IsValid())
            {
                const auto lookup =
                    fixture.ServerSpace.Lookup(serverUserCapability, ObjectType::Endpoint, CapabilityOperation::Invoke);
                serverUserAuthorityValid =
                    lookup.HasValue() && (&lookup.GetValue().GetObject() == &fixture.CallEndpoint) &&
                    lookup.GetValue().GetDescriptor().Rights.IsEqualTo(CapabilityRights{CapabilityRight::Invoke});
            }
            bool serverAuthorityValid{false};
            if (serverTokenCapability.IsValid())
            {
                const auto lookup = fixture.ServerSpace.Lookup(serverTokenCapability, ObjectType::ReplyToken,
                                                               CapabilityOperation::Invoke);
                serverAuthorityValid = lookup.HasValue() && lookup.GetValue().GetDescriptor().Rights.IsEqualTo(
                                                                CapabilityRights{CapabilityRight::Invoke});
            }
            const auto accepted = replyDispatcher.Accept(fixture.ServerId, fixture.ServerSpace, serverTokenCapability,
                                                         ResourceDomainId{8U});
            const MessageEnvelope reply = MakeMessage(MessageKind::Reply, Transaction, 0xC3U);
            const auto replied = replyDispatcher.CompleteReply(reply);
            const Foundation::Size serverCapabilitiesAfterReply = fixture.ServerSpace.GetActiveCount();
            MessageEnvelope receivedReply{};
            const auto replyReceived = fixture.ReplyEndpoint.Receive(receivedReply);
            const auto sourceLookup =
                fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
            const auto userSourceLookup =
                fixture.CallerSpace.Lookup(userCapability, ObjectType::Endpoint, CapabilityOperation::Invoke);
            ASTERIA_EXPECT(
                runner, "blocked Call moves one hidden token and Reply wakes its exact caller", tokenReady,
                userCapability.IsValid(), prepared.Error == SynchronousCallDispatcherError::None,
                duplicate.Error == SynchronousCallDispatcherError::AlreadyPrepared, blockToken.IsValid(),
                committed == BlockedRunCommitOutcome::Accepted, repeated == BlockedRunCommitOutcome::Accepted,
                committedSnapshot.State == SynchronousCallDispatcherState::Committed,
                committedSnapshot.EndpointSequence == 1U,
                committedSnapshot.Diagnostic.Error == SynchronousCallDispatcherError::None,
                repeatedSnapshot.State == SynchronousCallDispatcherState::Committed,
                repeatedSnapshot.Diagnostic.Error == SynchronousCallDispatcherError::None,
                repeatedSnapshot.EndpointSequence == 1U, received.HasValue(),
                received.GetValue().SenderEndpointRights.IsEqualTo(
                    CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read)),
                received.GetValue().SenderEndpointSealedObjectSelector == CallFixture::CallEndpointSelector,
                receivedCall.Kind == MessageKind::SynchronousCall, receivedCall.TransactionIdentifier == Transaction,
                receivedCall.Payload[0U] == 0x5AU, receivedCall.AttachmentCount == 2U,
                receivedCall.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                receivedCall.Attachments[0U].TransferMode == Kernel::Ipc::CapabilityTransferMode::Move,
                serverUserAuthorityValid, receivedCall.Attachments[1U].ExpectedType == ObjectType::ReplyToken,
                receivedCall.Attachments[1U].TransferMode == Kernel::Ipc::CapabilityTransferMode::Move,
                serverAuthorityValid, !sourceLookup.HasValue(),
                sourceLookup.GetError() == CapabilityError::InvalidHandle, !userSourceLookup.HasValue(),
                userSourceLookup.GetError() == CapabilityError::InvalidHandle,
                accepted.Error == SynchronousReplyDispatcherError::None, replied.HasValue(), replyReceived.HasValue(),
                receivedReply.Kind == MessageKind::Reply, receivedReply.TransactionIdentifier == Transaction,
                receivedReply.Payload[0U] == 0xC3U, fixture.Caller.GetSnapshot().State == ThreadState::Ready,
                fixture.WakeQueue.GetState().ReadyEntries == 1U, token.GetSnapshot().State == ReplyTokenState::Replied);
            ASTERIA_EXPECT(runner,
                           "the reply path consumes the one-shot ReplyToken capability it delivered to the server",
                           serverCapabilitiesAfterReply == 1U,
                           fixture.ServerSpace.Close(serverTokenCapability) == CapabilityError::InvalidHandle);
            ASTERIA_EXPECT(runner, "completed transaction releases queue and server authority",
                           fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None,
                           fixture.ServerSpace.Close(serverUserCapability) == CapabilityError::Ok,
                           fixture.ServerSpace.GetActiveCount() == 0U);
        }
        ASTERIA_EXPECT(runner, "completed call token retires after both dispatchers release it", tokenReady,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "call-reply fixture retires cleanly", fixture.Retire());
}

void TestBackpressureCancelsBlockedCaller(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x3344'5566'7788'99AAULL};
    CallFixture fixture{SingleMessageConfiguration()};
    ASTERIA_EXPECT(runner, "backpressure fixture initializes", fixture.Initialize());
    const MessageEnvelope filler = MakeMessage(MessageKind::Asynchronous, Transaction - 1U);
    const auto filled =
        fixture.CallEndpoint.Send(fixture.CallerSpace, ResourceDomainId{7U}, EndpointSendClass::Normal, filler);
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        {
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            const MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, Transaction);
            const auto prepared = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));
            const ThreadBlockToken blockToken = fixture.BlockCaller();

            Thread fillerThreads[MaximumReadyQueueEntries]{};
            std::optional<SchedulingContext> fillerContexts[MaximumReadyQueueEntries]{};
            ObjectId fillerThreadObjects[MaximumReadyQueueEntries]{};
            ObjectId fillerContextObjects[MaximumReadyQueueEntries]{};
            ThreadId fillerThreadIds[MaximumReadyQueueEntries]{};
            SchedulingContextId fillerContextIds[MaximumReadyQueueEntries]{};
            bool queueFilled{true};
            for (Foundation::Size index{0U}; index < MaximumReadyQueueEntries; ++index)
            {
                fillerContexts[index].emplace(fixture.Registry);
                queueFilled =
                    queueFilled && (fillerThreads[index].Initialize(ResourceDomainId{31U}) == ThreadError::None) &&
                    (fillerContexts[index]->Initialize(Configuration(), ResourceDomainId{31U}, MonotonicTime{0U}) ==
                     SchedulingContextError::None);
                const auto thread = fixture.Issuer.CreateObject(fillerThreads[index]);
                const auto context = fixture.Issuer.CreateObject(*fillerContexts[index]);
                queueFilled = queueFilled && thread.HasValue() && context.HasValue();
                if (!thread.HasValue() || !context.HasValue())
                {
                    break;
                }
                fillerThreadObjects[index] = thread.GetValue();
                fillerContextObjects[index] = context.GetValue();
                fillerThreadIds[index] = ThreadId{fixture.Registry, fillerThreadObjects[index]};
                fillerContextIds[index] = SchedulingContextId{fixture.Registry, fillerContextObjects[index]};
                queueFilled = queueFilled &&
                              (fillerContexts[index]->Bind(fillerThreadIds[index]) == SchedulingContextError::None) &&
                              (fillerThreads[index].MakeReady() == ThreadError::None) &&
                              fixture.WakeQueue
                                  .Enqueue(fillerContextIds[index], *fillerContexts[index], fillerThreadIds[index],
                                           fillerThreads[index])
                                  .HasValue();
            }

            const auto committed = dispatcher.CommitBlocked(blockToken);
            const auto pressureSnapshot = dispatcher.GetSnapshot();
            const auto wrongState = dispatcher.CancelPrepared();
            const auto preservedPressure = dispatcher.GetSnapshot();
            const ReadyQueueError freed = fixture.WakeQueue.Remove(fillerThreadIds[0U]);
            const auto cancelled = dispatcher.PollCancellation();
            const auto cancelledSnapshot = dispatcher.GetSnapshot();
            const auto terminalWrongState = dispatcher.CommitBlocked(blockToken);
            const auto preservedTerminal = dispatcher.GetSnapshot();
            const BlockedRunCommitOutcome terminalObserver = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto preservedAfterObserver = dispatcher.GetSnapshot();
            const auto sourceLookup =
                fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
            bool queueClean = fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None;
            for (Foundation::Size index{0U}; index < MaximumReadyQueueEntries; ++index)
            {
                if (index != 0U)
                {
                    queueClean =
                        (fixture.WakeQueue.Remove(fillerThreadIds[index]) == ReadyQueueError::None) && queueClean;
                }
                if (fillerThreads[index].IsInitialized())
                {
                    queueClean = (fillerThreads[index].Terminate(1U) == ThreadError::None) && queueClean;
                }
                if (fillerContexts[index].has_value())
                {
                    queueClean =
                        (fillerContexts[index]->Unbind(fillerThreadIds[index]) == SchedulingContextError::None) &&
                        queueClean;
                }
                if (fillerContextObjects[index].IsValid())
                {
                    queueClean =
                        (fixture.Issuer.DestroyObject(fillerContextObjects[index]) == ObjectRegistryError::Ok) &&
                        queueClean;
                }
                if (fillerThreadObjects[index].IsValid())
                {
                    queueClean =
                        (fixture.Issuer.DestroyObject(fillerThreadObjects[index]) == ObjectRegistryError::Ok) &&
                        queueClean;
                }
            }
            for (Foundation::Size index{0U}; index < (MaximumReadyQueueEntries * 2U); ++index)
            {
                queueClean = fixture.Issuer.CollectDestroyedObject().HasValue() && queueClean;
            }
            ASTERIA_EXPECT(
                runner, "queue-pressure retry preserves failure and wakes the caller", filled.HasValue(), tokenReady,
                queueFilled, prepared.Error == SynchronousCallDispatcherError::None, blockToken.IsValid(),
                committed.Error == SynchronousCallDispatcherError::CancellationPending,
                pressureSnapshot.State == SynchronousCallDispatcherState::CancellationPending,
                pressureSnapshot.Diagnostic.Error == SynchronousCallDispatcherError::CancellationPending,
                pressureSnapshot.Diagnostic.Endpoint.Code == EndpointErrorCode::WouldBlock,
                pressureSnapshot.Diagnostic.Queue.Error == ReadyQueueError::QueueFull,
                wrongState.Error == SynchronousCallDispatcherError::InvalidState,
                preservedPressure.State == SynchronousCallDispatcherState::CancellationPending,
                preservedPressure.Diagnostic.Error == SynchronousCallDispatcherError::CancellationPending,
                preservedPressure.Diagnostic.Endpoint.Code == EndpointErrorCode::WouldBlock,
                preservedPressure.Diagnostic.Queue.Error == ReadyQueueError::QueueFull, freed == ReadyQueueError::None,
                cancelled.Error == SynchronousCallDispatcherError::EndpointSendRejected,
                cancelledSnapshot.State == SynchronousCallDispatcherState::Cancelled,
                cancelledSnapshot.Diagnostic.Error == SynchronousCallDispatcherError::EndpointSendRejected,
                cancelledSnapshot.Diagnostic.Endpoint.Code == EndpointErrorCode::WouldBlock,
                terminalWrongState.Error == SynchronousCallDispatcherError::AlreadyCompleted,
                preservedTerminal.State == SynchronousCallDispatcherState::Cancelled,
                preservedTerminal.Diagnostic.Error == SynchronousCallDispatcherError::EndpointSendRejected,
                preservedTerminal.Diagnostic.Endpoint.Code == EndpointErrorCode::WouldBlock,
                terminalObserver == BlockedRunCommitOutcome::Accepted,
                preservedAfterObserver.State == SynchronousCallDispatcherState::Cancelled,
                preservedAfterObserver.Diagnostic.Error == SynchronousCallDispatcherError::EndpointSendRejected,
                preservedAfterObserver.Diagnostic.Endpoint.Code == EndpointErrorCode::WouldBlock,
                fixture.CallEndpoint.GetQueueState().QueuedMessages == 1U, !sourceLookup.HasValue(),
                sourceLookup.GetError() == CapabilityError::InvalidHandle,
                token.GetSnapshot().State == ReplyTokenState::Cancelled,
                fixture.Caller.GetSnapshot().State == ThreadState::Ready, queueClean,
                fixture.WakeQueue.GetState().ReadyEntries == 0U);
            ASTERIA_EXPECT(runner, "cancelled caller and queue-pressure fixtures retire cleanly", queueClean);
        }
        ASTERIA_EXPECT(runner, "cancelled call token retires after dispatcher teardown", tokenReady,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "backpressure fixture retires cleanly", fixture.Retire());
}

// The Call is published only after its caller blocked; authority revoked in
// between must stop it, and the caller is recovered like any failed send.
void TestRevokedEndpointAuthorityStopsBlockedCall(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x3A3A'4B4B'5C5C'6D6DULL};
    CallFixture fixture;
    ASTERIA_EXPECT(runner, "revoked-authority fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        {
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            const MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, Transaction);
            const auto prepared = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));
            const auto revoked = fixture.Revocations.Revoke(fixture.EndpointDomain);
            const ThreadBlockToken blockToken = fixture.BlockCaller();
            const BlockedRunCommitOutcome committed = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto snapshot = dispatcher.GetSnapshot();
            const auto tokenSource =
                fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
            ASTERIA_EXPECT(
                runner, "a Call whose Endpoint authority is revoked before publication wakes its caller unpublished",
                tokenReady, prepared.Error == SynchronousCallDispatcherError::None,
                revoked == Kernel::Object::RevocationError::Ok, blockToken.IsValid(),
                committed == BlockedRunCommitOutcome::Accepted,
                snapshot.State == SynchronousCallDispatcherState::Cancelled,
                snapshot.Diagnostic.Error == SynchronousCallDispatcherError::EndpointSendRejected,
                snapshot.Diagnostic.Endpoint.Code == EndpointErrorCode::InvocationRejected,
                snapshot.Diagnostic.Endpoint.Capability == CapabilityError::Revoked,
                fixture.CallEndpoint.GetQueueState().QueuedMessages == 0U, fixture.ServerSpace.GetActiveCount() == 0U,
                !tokenSource.HasValue(), tokenSource.GetError() == CapabilityError::InvalidHandle,
                token.GetSnapshot().State == ReplyTokenState::Cancelled,
                fixture.Caller.GetSnapshot().State == ThreadState::Ready,
                fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None);
        }
        ASTERIA_EXPECT(runner, "revoked-authority call token retires", tokenReady, fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "revoked-authority fixture retires cleanly", fixture.Retire());
}

void TestUnreadyReplyPreflightAndFailedPrepareObserver(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x4455'6677'8899'AABBU};
    CallFixture fixture;
    ASTERIA_EXPECT(runner, "reply-preflight fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        {
            const EndpointConfiguration unavailableConfiguration{};
            Endpoint unavailableReplyEndpoint{fixture.CallerSpace, ResourceDomainId{7U}, unavailableConfiguration};
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, unavailableReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            const MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, Transaction);
            const auto preflight = replyDispatcher.PreflightInitialize();
            const auto rejected = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));
            const auto retained = dispatcher.GetSnapshot();
            const auto replyRetained = replyDispatcher.GetSnapshot();
            const BlockedRunCommitOutcome observer = dispatcher.CommitBlockedFromFinishedRun({});
            const auto afterObserver = dispatcher.GetSnapshot();
            const auto source =
                fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
            ASTERIA_EXPECT(runner,
                           "unready reply delivery is rejected before blocking and retained across observer misuse",
                           tokenReady, preflight.Error == SynchronousReplyDispatcherError::InvalidConfiguration,
                           rejected.Error == SynchronousCallDispatcherError::InvalidConfiguration,
                           rejected.Reply == SynchronousReplyDispatcherError::InvalidConfiguration,
                           retained.State == SynchronousCallDispatcherState::Fresh, retained.RunSequence == 0U,
                           !retained.Caller.IsValid(),
                           replyRetained.State == Kernel::Ipc::SynchronousReplyDispatcherState::Uninitialized,
                           retained.Diagnostic.Error == rejected.Error, retained.Diagnostic.Reply == rejected.Reply,
                           retained.Diagnostic.TransactionIdentifier == Transaction,
                           observer == BlockedRunCommitOutcome::TerminalRejected, afterObserver.State == retained.State,
                           afterObserver.RunSequence == retained.RunSequence,
                           afterObserver.Diagnostic.Error == retained.Diagnostic.Error,
                           afterObserver.Diagnostic.Reply == retained.Diagnostic.Reply,
                           afterObserver.Diagnostic.TransactionIdentifier == retained.Diagnostic.TransactionIdentifier,
                           source.HasValue(), token.GetSnapshot().State == ReplyTokenState::Available,
                           fixture.Caller.GetSnapshot().State == ThreadState::Running,
                           fixture.CallEndpoint.GetQueueState().QueuedMessages == 0U);
        }
        ASTERIA_EXPECT(runner, "reply-preflight token remains caller-owned and retires", tokenReady,
                       fixture.CallerSpace.Close(tokenCapability) == CapabilityError::Ok,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "reply-preflight fixture retires cleanly", fixture.Retire());
}

void TestPreArmReferencePressureRetries(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x5566'7788'99AA'BBCCU};
    CallFixture fixture{EndpointConfiguration::Default(), 4U};
    ASTERIA_EXPECT(runner, "pre-Arm reference-pressure fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        {
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            const MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, Transaction);
            const auto prepared = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));
            ObjectPin firstContextPressure;
            ObjectPin secondContextPressure;
            ObjectPin thirdContextPressure;
            const ObjectPinError firstPinned =
                fixture.Registry.PinObject(fixture.ContextObject, ObjectType::SchedulingContext, firstContextPressure);
            const ObjectPinError secondPinned =
                fixture.Registry.PinObject(fixture.ContextObject, ObjectType::SchedulingContext, secondContextPressure);
            const ObjectPinError thirdPinned =
                fixture.Registry.PinObject(fixture.ContextObject, ObjectType::SchedulingContext, thirdContextPressure);
            const ThreadBlockToken blockToken = fixture.BlockCaller();
            const BlockedRunCommitOutcome initial = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto pressure = dispatcher.GetSnapshot();
            const auto tokenAfterPressure = token.GetSnapshot();
            ObjectPin firstCallerRollbackProbe;
            ObjectPin secondCallerRollbackProbe;
            const ObjectPinError firstRollbackProbe =
                fixture.Registry.PinObject(fixture.CallerObject, ObjectType::Thread, firstCallerRollbackProbe);
            const ObjectPinError secondRollbackProbe =
                fixture.Registry.PinObject(fixture.CallerObject, ObjectType::Thread, secondCallerRollbackProbe);
            const auto wrongCancellation = dispatcher.CancelPrepared();
            const auto preserved = dispatcher.GetSnapshot();
            const BlockedRunCommitOutcome repeated = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto tokenAfterRepeat = token.GetSnapshot();
            firstContextPressure.Reset();
            secondContextPressure.Reset();
            thirdContextPressure.Reset();
            firstCallerRollbackProbe.Reset();
            secondCallerRollbackProbe.Reset();
            const auto progressed = dispatcher.Poll();
            const auto completed = dispatcher.GetSnapshot();
            const auto source =
                fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
            const bool cancelled = CancelPublishedCall(fixture, replyDispatcher, Transaction);
            ASTERIA_EXPECT(
                runner, "pre-Arm ReferenceExhausted retains its phase and Poll completes exactly once", tokenReady,
                prepared.Error == SynchronousCallDispatcherError::None, firstPinned == ObjectPinError::Ok,
                secondPinned == ObjectPinError::Ok, thirdPinned == ObjectPinError::Ok, blockToken.IsValid(),
                initial == BlockedRunCommitOutcome::RetryableRejected,
                pressure.State == SynchronousCallDispatcherState::ArmPending, pressure.BlockToken.IsEqualTo(blockToken),
                pressure.Diagnostic.Error == SynchronousCallDispatcherError::ReplyTokenArmRejected,
                pressure.Diagnostic.Token.Error == ReplyTokenError::SchedulingContextPinUnavailable,
                pressure.Diagnostic.Token.PinError == ObjectPinError::ReferenceExhausted,
                tokenAfterPressure.State == ReplyTokenState::Available, firstRollbackProbe == ObjectPinError::Ok,
                secondRollbackProbe == ObjectPinError::Ok,
                wrongCancellation.Error == SynchronousCallDispatcherError::InvalidState,
                preserved.State == pressure.State, preserved.Diagnostic.Error == pressure.Diagnostic.Error,
                preserved.Diagnostic.Token.Error == pressure.Diagnostic.Token.Error,
                repeated == BlockedRunCommitOutcome::RetryableRejected,
                tokenAfterRepeat.State == ReplyTokenState::Available,
                progressed.Error == SynchronousCallDispatcherError::None,
                completed.State == SynchronousCallDispatcherState::Committed, completed.EndpointSequence == 1U,
                !source.HasValue(), source.GetError() == CapabilityError::InvalidHandle, cancelled,
                token.GetSnapshot().State == ReplyTokenState::Cancelled);
        }
        ASTERIA_EXPECT(runner, "pre-Arm reference-pressure token retires", tokenReady,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "pre-Arm reference-pressure fixture retires cleanly", fixture.Retire());
}

void TestPostArmReferencePressureRetries(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x6677'8899'AABB'CCDDU};
    CallFixture fixture{EndpointConfiguration::Default(), 4U};
    ASTERIA_EXPECT(runner, "post-Arm reference-pressure fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        {
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            const MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, Transaction);
            const auto prepared = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));
            ObjectPin callerPressure;
            const ObjectPinError pressurePinned =
                fixture.Registry.PinObject(fixture.CallerObject, ObjectType::Thread, callerPressure);
            const ThreadBlockToken blockToken = fixture.BlockCaller();
            const BlockedRunCommitOutcome initial = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto pressure = dispatcher.GetSnapshot();
            const auto tokenAfterArm = token.GetSnapshot();
            const auto replyAfterPressure = replyDispatcher.GetSnapshot();
            ObjectPin firstTokenRollbackProbe;
            ObjectPin secondTokenRollbackProbe;
            ObjectPin thirdTokenRollbackProbe;
            const ObjectPinError firstRollbackProbe =
                fixture.Registry.PinObject(tokenObject, ObjectType::ReplyToken, firstTokenRollbackProbe);
            const ObjectPinError secondRollbackProbe =
                fixture.Registry.PinObject(tokenObject, ObjectType::ReplyToken, secondTokenRollbackProbe);
            const ObjectPinError thirdRollbackProbe =
                fixture.Registry.PinObject(tokenObject, ObjectType::ReplyToken, thirdTokenRollbackProbe);
            const BlockedRunCommitOutcome repeated = dispatcher.CommitBlockedFromFinishedRun(blockToken);
            const auto tokenAfterRepeat = token.GetSnapshot();
            callerPressure.Reset();
            firstTokenRollbackProbe.Reset();
            secondTokenRollbackProbe.Reset();
            thirdTokenRollbackProbe.Reset();
            bool sourceIntactBeforeRetry{false};
            {
                const auto sourceBeforeRetry =
                    fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
                sourceIntactBeforeRetry = sourceBeforeRetry.HasValue();
            }
            const auto progressed = dispatcher.Poll();
            const auto completed = dispatcher.GetSnapshot();
            const bool cancelled = CancelPublishedCall(fixture, replyDispatcher, Transaction);
            ASTERIA_EXPECT(
                runner, "post-Arm ReferenceExhausted retries initialization without re-Arming", tokenReady,
                prepared.Error == SynchronousCallDispatcherError::None, pressurePinned == ObjectPinError::Ok,
                blockToken.IsValid(), initial == BlockedRunCommitOutcome::RetryableRejected,
                pressure.State == SynchronousCallDispatcherState::ReplyInitializationPending,
                pressure.Diagnostic.Error == SynchronousCallDispatcherError::ReplyDispatcherRejected,
                pressure.Diagnostic.Reply == SynchronousReplyDispatcherError::CallerPinRejected,
                pressure.Diagnostic.Pin == ObjectPinError::ReferenceExhausted,
                tokenAfterArm.State == ReplyTokenState::Pending, tokenAfterArm.BlockToken.IsEqualTo(blockToken),
                replyAfterPressure.State == Kernel::Ipc::SynchronousReplyDispatcherState::Uninitialized,
                firstRollbackProbe == ObjectPinError::Ok, secondRollbackProbe == ObjectPinError::Ok,
                thirdRollbackProbe == ObjectPinError::Ok, repeated == BlockedRunCommitOutcome::RetryableRejected,
                tokenAfterRepeat.State == ReplyTokenState::Pending, tokenAfterRepeat.BlockToken.IsEqualTo(blockToken),
                sourceIntactBeforeRetry, progressed.Error == SynchronousCallDispatcherError::None,
                completed.State == SynchronousCallDispatcherState::Committed, completed.EndpointSequence == 1U,
                cancelled, token.GetSnapshot().State == ReplyTokenState::Cancelled);
        }
        ASTERIA_EXPECT(runner, "post-Arm reference-pressure token retires", tokenReady,
                       fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "post-Arm reference-pressure fixture retires cleanly", fixture.Retire());
}

void TestReceiverSpaceExhaustionRollsBackTransfer(TestContext& runner) noexcept
{
    constexpr UInt64 Transaction{0x7788'99AA'BBCC'DDEEU};
    CallFixture fixture;
    ASTERIA_EXPECT(runner, "receiver-space exhaustion fixture initializes", fixture.Initialize());
    {
        ReplyToken token;
        ObjectId tokenObject{};
        CapabilityHandle tokenCapability{};
        const bool tokenReady = fixture.ConfigureToken(token, tokenObject, tokenCapability);
        const CapabilityHandle userCapability = fixture.MintCallEndpointCapability(
            CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Transfer), 0xBADC'0FFEU);
        {
            SynchronousReplyDispatcher replyDispatcher{fixture.Registry, fixture.ReplyEndpoint, fixture.WakeQueue};
            SynchronousCallDispatcher dispatcher{fixture.Registry, fixture.CallEndpoint, token, tokenObject,
                                                 replyDispatcher};
            MessageEnvelope call = MakeMessage(MessageKind::SynchronousCall, Transaction);
            call.Attachments[0U] = Kernel::Ipc::CapabilityAttachment{
                .Handle = userCapability,
                .RequestedDeadline = MonotonicTime::Infinite(),
                .RequestedRights = CapabilityRights{CapabilityRight::Invoke},
                .ExpectedType = ObjectType::Endpoint,
                .TransferMode = Kernel::Ipc::CapabilityTransferMode::Move,
                .Reserved = 0U,
            };
            call.AttachmentCount = 1U;
            const auto prepared = dispatcher.Prepare(fixture.MakeRequest(call, tokenCapability));

            CapabilityHandle filled[CapabilitySpace::MaximumSlots]{};
            bool receiverFilled{true};
            for (Foundation::Size index{0U}; index < CapabilitySpace::MaximumSlots; ++index)
            {
                const auto minted = fixture.Issuer.Mint(fixture.ServerSpace, fixture.CallEndpointObject,
                                                        CapabilityRights{CapabilityRight::Invoke},
                                                        fixture.EndpointDomain, MonotonicTime::Infinite(), index + 1U);
                if (!minted.HasValue())
                {
                    receiverFilled = false;
                    break;
                }
                filled[index] = minted.GetValue();
            }

            const ThreadBlockToken blockToken = fixture.BlockCaller();
            const auto committed = dispatcher.CommitBlocked(blockToken);
            const auto snapshot = dispatcher.GetSnapshot();
            bool userSourceIntact{false};
            {
                const auto source =
                    fixture.CallerSpace.Lookup(userCapability, ObjectType::Endpoint, CapabilityOperation::Invoke);
                userSourceIntact = source.HasValue();
            }
            bool endpointAuthorityIntact{false};
            {
                const auto authority = fixture.CallerSpace.Lookup(fixture.CallEndpointCapability, ObjectType::Endpoint,
                                                                  CapabilityOperation::Invoke);
                endpointAuthorityIntact = authority.HasValue();
            }
            const auto tokenSource =
                fixture.CallerSpace.Lookup(tokenCapability, ObjectType::ReplyToken, CapabilityOperation::Invoke);
            const Foundation::Size receiverCount = fixture.ServerSpace.GetActiveCount();
            bool cleaned = fixture.WakeQueue.Remove(fixture.CallerId) == ReadyQueueError::None;
            cleaned = (fixture.CallerSpace.Close(userCapability) == CapabilityError::Ok) && cleaned;
            for (Foundation::Size index{0U}; index < CapabilitySpace::MaximumSlots; ++index)
            {
                if (filled[index].IsValid())
                {
                    cleaned = (fixture.ServerSpace.Close(filled[index]) == CapabilityError::Ok) && cleaned;
                }
            }
            ASTERIA_EXPECT(runner, "receiver-space exhaustion rolls back every user and hidden capability move",
                           tokenReady, userCapability.IsValid(), prepared.Error == SynchronousCallDispatcherError::None,
                           receiverFilled, blockToken.IsValid(),
                           committed.Error == SynchronousCallDispatcherError::EndpointSendRejected,
                           snapshot.State == SynchronousCallDispatcherState::Cancelled,
                           snapshot.Diagnostic.Error == SynchronousCallDispatcherError::EndpointSendRejected,
                           snapshot.Diagnostic.Endpoint.Code == EndpointErrorCode::CapabilityTransferFailed,
                           snapshot.Diagnostic.Endpoint.Capability == CapabilityError::SpaceExhausted,
                           fixture.CallEndpoint.GetQueueState().QueuedMessages == 0U,
                           receiverCount == CapabilitySpace::MaximumSlots, userSourceIntact, endpointAuthorityIntact,
                           !tokenSource.HasValue(), tokenSource.GetError() == CapabilityError::InvalidHandle,
                           token.GetSnapshot().State == ReplyTokenState::Cancelled,
                           fixture.Caller.GetSnapshot().State == ThreadState::Ready, cleaned,
                           fixture.ServerSpace.GetActiveCount() == 0U);
        }
        ASTERIA_EXPECT(runner, "receiver-space exhaustion token retires", tokenReady, fixture.RetireToken(tokenObject));
    }
    ASTERIA_EXPECT(runner, "receiver-space exhaustion fixture retires cleanly", fixture.Retire());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SynchronousCallDispatcherTests"};
    Asteria::Test::TestPrepareIsInertAndCancellable(runner);
    Asteria::Test::TestCallReplyTransaction(runner);
    Asteria::Test::TestBackpressureCancelsBlockedCaller(runner);
    Asteria::Test::TestRevokedEndpointAuthorityStopsBlockedCall(runner);
    Asteria::Test::TestUnreadyReplyPreflightAndFailedPrepareObserver(runner);
    Asteria::Test::TestPreArmReferencePressureRetries(runner);
    Asteria::Test::TestPostArmReferencePressureRetries(runner);
    Asteria::Test::TestReceiverSpaceExhaustionRollsBackTransfer(runner);
    return runner.Finish();
}
