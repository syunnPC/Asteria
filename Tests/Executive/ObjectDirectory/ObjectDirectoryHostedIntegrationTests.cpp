// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Executive/ObjectDirectory/ObjectDirectoryServiceCore.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Executive::ObjectDirectory::BindIfAbsentRequest;
using Executive::ObjectDirectory::BindingVersion;
using Executive::ObjectDirectory::DirectoryCoreError;
using Executive::ObjectDirectory::DirectoryMessageContext;
using Executive::ObjectDirectory::DirectoryName;
using Executive::ObjectDirectory::DirectoryNameView;
using Executive::ObjectDirectory::DirectoryStatus;
using Executive::ObjectDirectory::InterfaceVersionRange;
using Executive::ObjectDirectory::MutationReply;
using Executive::ObjectDirectory::ObjectDirectoryProtocol;
using Executive::ObjectDirectory::ObjectDirectoryProtocolError;
using Executive::ObjectDirectory::ObjectDirectoryServiceConfiguration;
using Executive::ObjectDirectory::ObjectDirectoryServiceCore;
using Executive::ObjectDirectory::ObjectDirectoryServiceDispatchOutcome;
using Executive::ObjectDirectory::ObjectDirectoryServiceError;
using Executive::ObjectDirectory::ObjectDirectoryServiceFailure;
using Executive::ObjectDirectory::RemoveIfVersionRequest;
using Executive::ObjectDirectory::ReplaceIfVersionRequest;
using Executive::ObjectDirectory::ResolveReply;
using Executive::ObjectDirectory::ResolveRequest;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointReceiveMetadata;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::IpcDeliveryCodec;
using Kernel::Ipc::IpcDeliveryCodecError;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Ipc::WritableByteView;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

constexpr UInt64 FirstDirectorySelector{0xD1EC'7000'0000'0001ULL};
constexpr UInt64 SecondDirectorySelector{0xD1EC'7000'0000'0002ULL};
constexpr UInt64 ReplySelector{0xD1EC'7000'0000'0010ULL};
constexpr UInt64 FirstTargetSelector{0xD1EC'7000'0000'0021ULL};
constexpr UInt64 SecondTargetSelector{0xD1EC'7000'0000'0022ULL};
constexpr UInt64 FirstDirectoryGeneration{0xA001'0000'0000'0001ULL};
constexpr UInt64 SecondDirectoryGeneration{0xA002'0000'0000'0001ULL};

struct ExchangeResult
{
    ObjectDirectoryServiceFailure DispatchFailure;
    EndpointReceiveMetadata RequestMetadata;
    EndpointReceiveMetadata ReplyMetadata;
    MessageEnvelope Reply;
    Size CleanupCount;
    bool RequestSent;
    bool RequestDelivered;
    bool WasDispatched;
    bool ReplySent;
    bool CleanupClosed;
    bool ReplyDelivered;
};

[[nodiscard]] constexpr CapabilityRights InvokeRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke};
}

[[nodiscard]] constexpr CapabilityRights ManageInvocationRights() noexcept
{
    return InvokeRights().With(CapabilityRight::Manage);
}

[[nodiscard]] constexpr CapabilityRights ReplySourceRights() noexcept
{
    return InvokeRights().With(CapabilityRight::Duplicate);
}

[[nodiscard]] constexpr CapabilityRights PublishedRights() noexcept
{
    return InvokeRights().With(CapabilityRight::Read);
}

[[nodiscard]] constexpr CapabilityRights TargetSourceRights() noexcept
{
    return PublishedRights().With(CapabilityRight::Duplicate).With(CapabilityRight::Revoke);
}

[[nodiscard]] constexpr bool HandlesAreEqual(const CapabilityHandle left, const CapabilityHandle right) noexcept
{
    return (left.GetSelector() == right.GetSelector()) && (left.GetAuthenticator() == right.GetAuthenticator());
}

[[nodiscard]] DirectoryName MakeName(const char* const text, const Size sizeBytes) noexcept
{
    UInt8 bytes[DirectoryName::MaximumSizeBytes]{};
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = static_cast<UInt8>(text[index]);
    }
    const auto result = DirectoryName::Create(DirectoryNameView{
        .Data = bytes,
        .SizeBytes = sizeBytes,
    });
    return result.HasValue() ? result.GetValue() : DirectoryName{};
}

[[nodiscard]] constexpr InterfaceIdentifier ServiceInterface() noexcept
{
    return InterfaceIdentifier{0x5100'0000'0000'0001ULL, 0xA57E'0000'0000'0001ULL};
}

[[nodiscard]] constexpr InterfaceVersionRange PublishedVersion() noexcept
{
    return InterfaceVersionRange{
        .Major = 3U,
        .MinimumMinor = 1U,
        .MaximumMinor = 5U,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr DirectoryMessageContext MakeContext(const UInt64 transactionIdentifier) noexcept
{
    return DirectoryMessageContext{
        .Trace =
            {
                .TraceIdentifierHigh = 0x1111'2222'3333'4444ULL,
                .TraceIdentifierLow = transactionIdentifier,
                .SpanIdentifier = 0x5555'6666'7777'8888ULL,
            },
        .Deadline = MonotonicTime{9'000U},
        .TransactionIdentifier = transactionIdentifier,
    };
}

[[nodiscard]] BindIfAbsentRequest MakeBindRequest(const DirectoryName& name, const CapabilityHandle targetEndpoint,
                                                  const CapabilityHandle replyEndpoint, const UInt64 serviceGeneration,
                                                  const UInt64 transactionIdentifier) noexcept
{
    return BindIfAbsentRequest{
        .Name = name,
        .ServiceInterface = ServiceInterface(),
        .ServiceGeneration = serviceGeneration,
        .Version = PublishedVersion(),
        .PublishedRights = PublishedRights(),
        .TargetEndpoint = targetEndpoint,
        .TargetDeadline = MonotonicTime{7'000U},
        .ReplyEndpoint = replyEndpoint,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] ReplaceIfVersionRequest
MakeReplaceRequest(const DirectoryName& name, const BindingVersion expectedVersion,
                   const CapabilityHandle targetEndpoint, const CapabilityHandle replyEndpoint,
                   const UInt64 serviceGeneration, const UInt64 transactionIdentifier) noexcept
{
    return ReplaceIfVersionRequest{
        .Name = name,
        .ExpectedVersion = expectedVersion,
        .ServiceInterface = ServiceInterface(),
        .ServiceGeneration = serviceGeneration,
        .Version = PublishedVersion(),
        .PublishedRights = PublishedRights(),
        .TargetEndpoint = targetEndpoint,
        .TargetDeadline = MonotonicTime{7'000U},
        .ReplyEndpoint = replyEndpoint,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] ResolveRequest MakeResolveRequest(const DirectoryName& name, const CapabilityHandle replyEndpoint,
                                                const UInt64 transactionIdentifier) noexcept
{
    return ResolveRequest{
        .Name = name,
        .ServiceInterface = ServiceInterface(),
        .RequestedDeadline = MonotonicTime{6'000U},
        .RequiredVersion =
            InterfaceVersionRange{
                .Major = 3U,
                .MinimumMinor = 2U,
                .MaximumMinor = 6U,
                .Reserved = 0U,
            },
        .RequiredRights = InvokeRights(),
        .ReplyEndpoint = replyEndpoint,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] RemoveIfVersionRequest MakeRemoveRequest(const DirectoryName& name, const BindingVersion expectedVersion,
                                                       const CapabilityHandle replyEndpoint,
                                                       const UInt64 transactionIdentifier) noexcept
{
    return RemoveIfVersionRequest{
        .Name = name,
        .ExpectedVersion = expectedVersion,
        .ReplyEndpoint = replyEndpoint,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] MessageEnvelope MakeProbeMessage(const UInt64 transactionIdentifier) noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0xCA11'0000'0000'0001ULL, 0xA57E'0000'0000'1001ULL};
    message.TransactionIdentifier = transactionIdentifier;
    message.Deadline = MonotonicTime{5'000U};
    message.MethodIdentifier = 1U;
    message.InterfaceVersionMajor = 1U;
    message.InterfaceVersionMinor = 0U;
    message.FixedPayloadSizeBytes = 1U;
    message.PayloadSizeBytes = 1U;
    message.Kind = MessageKind::Asynchronous;
    message.Cancellation = CancellationState::None;
    message.Payload[0U] = 0xA5U;
    return message;
}

[[nodiscard]] bool CloseMessageAttachments(CapabilitySpace& receiverSpace, MessageEnvelope& message) noexcept
{
    bool succeeded{true};
    const Size attachmentCount = message.AttachmentCount;
    for (Size index{0U}; index < attachmentCount; ++index)
    {
        if (receiverSpace.Close(message.Attachments[index].Handle) != CapabilityError::Ok)
        {
            succeeded = false;
        }
        message.Attachments[index] = {};
    }
    message.AttachmentCount = 0U;
    return succeeded;
}

[[nodiscard]] bool ReceiveCanonicalDelivery(Endpoint& endpoint, CapabilitySpace& receiverSpace,
                                            EndpointReceiveMetadata& outputMetadata,
                                            MessageEnvelope& outputMessage) noexcept
{
    MessageEnvelope receivedMessage{};
    const auto received = endpoint.Receive(receivedMessage);
    if (!received.HasValue())
    {
        return false;
    }

    UInt8 delivery[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    const auto encoded = IpcDeliveryCodec::Encode(received.GetValue(), receivedMessage,
                                                  WritableByteView{
                                                      .Data = delivery,
                                                      .SizeBytes = sizeof(delivery),
                                                  });
    if (!encoded.HasValue())
    {
        static_cast<void>(CloseMessageAttachments(receiverSpace, receivedMessage));
        return false;
    }

    EndpointReceiveMetadata decodedMetadata{};
    MessageEnvelope decodedMessage{};
    const auto decoded = IpcDeliveryCodec::Decode(
        ReadOnlyByteView{
            .Data = delivery,
            .SizeBytes = encoded.GetValue(),
        },
        decodedMetadata, decodedMessage);
    if ((decoded.Error != IpcDeliveryCodecError::None) || (decoded.Frame != MessageCodecError::None))
    {
        static_cast<void>(CloseMessageAttachments(receiverSpace, receivedMessage));
        return false;
    }

    outputMetadata = decodedMetadata;
    outputMessage = decodedMessage;
    return true;
}

[[nodiscard]] bool SendThroughCapability(CapabilitySpace& sourceSpace, const CapabilityHandle endpointCapability,
                                         const ResourceDomainId sender, const MessageEnvelope& message,
                                         Endpoint& expectedEndpoint) noexcept
{
    const auto lookup = sourceSpace.Lookup(endpointCapability, ObjectType::Endpoint, CapabilityOperation::Invoke);
    if (!lookup.HasValue())
    {
        return false;
    }

    Endpoint& endpoint = static_cast<Endpoint&>(lookup.GetValue().GetObject());
    if (&endpoint != &expectedEndpoint)
    {
        return false;
    }
    const auto sent =
        endpoint.SendFromInvocation(sourceSpace, endpointCapability, sender, EndpointSendClass::Normal, message);
    return sent.HasValue();
}

[[nodiscard]] bool CloseOutcomeCapabilities(CapabilitySpace& directorySpace,
                                            ObjectDirectoryServiceDispatchOutcome& outcome) noexcept
{
    bool succeeded{true};
    for (Size index{0U}; index < outcome.CapabilityCloseCount; ++index)
    {
        if (directorySpace.Close(outcome.CapabilitiesToClose[index]) != CapabilityError::Ok)
        {
            succeeded = false;
        }
        outcome.CapabilitiesToClose[index] = {};
    }
    outcome.CapabilityCloseCount = 0U;
    return succeeded;
}

[[nodiscard]] ExchangeResult CompleteExchange(ObjectDirectoryServiceCore& service, Endpoint& directoryEndpoint,
                                              const CapabilityHandle directoryCapability, CapabilitySpace& callerSpace,
                                              const ResourceDomainId caller, CapabilitySpace& directorySpace,
                                              Endpoint& replyEndpoint, CapabilitySpace& clientSpace,
                                              const MessageEnvelope& request) noexcept
{
    ExchangeResult result{};
    result.RequestSent = SendThroughCapability(callerSpace, directoryCapability, caller, request, directoryEndpoint);
    if (!result.RequestSent)
    {
        return result;
    }

    MessageEnvelope deliveredRequest{};
    result.RequestDelivered =
        ReceiveCanonicalDelivery(directoryEndpoint, directorySpace, result.RequestMetadata, deliveredRequest);
    if (!result.RequestDelivered)
    {
        return result;
    }

    ObjectDirectoryServiceDispatchOutcome outcome{};
    result.DispatchFailure = service.Dispatch(deliveredRequest, result.RequestMetadata, outcome);
    result.WasDispatched = true;
    if (outcome.HasPreparedReply)
    {
        result.ReplySent = SendThroughCapability(directorySpace, outcome.ReplyEndpoint, ResourceDomainId{0xD1ECU},
                                                 outcome.PreparedReply, replyEndpoint);
    }

    result.CleanupCount = outcome.CapabilityCloseCount;
    result.CleanupClosed = CloseOutcomeCapabilities(directorySpace, outcome);
    if (!result.ReplySent)
    {
        return result;
    }

    result.ReplyDelivered = ReceiveCanonicalDelivery(replyEndpoint, clientSpace, result.ReplyMetadata, result.Reply);
    return result;
}

[[nodiscard]] bool ExchangeCompleted(const ExchangeResult& exchange, const Size expectedCleanupCount) noexcept
{
    return exchange.RequestSent && exchange.RequestDelivered && exchange.WasDispatched &&
           (exchange.DispatchFailure.Code == ObjectDirectoryServiceError::None) && exchange.ReplySent &&
           exchange.CleanupClosed && (exchange.CleanupCount == expectedCleanupCount) && exchange.ReplyDelivered;
}

[[nodiscard]] ObjectDirectoryProtocolError ParseMutationReply(const Foundation::UInt32 methodIdentifier,
                                                              const MessageEnvelope& message,
                                                              MutationReply& output) noexcept
{
    switch (methodIdentifier)
    {
    case ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier:
        return ObjectDirectoryProtocol::ParseBindIfAbsentReply(message, output);

    case ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier:
        return ObjectDirectoryProtocol::ParseReplaceIfVersionReply(message, output);

    case ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier:
        return ObjectDirectoryProtocol::ParseRemoveIfVersionReply(message, output);

    default:
        return ObjectDirectoryProtocolError::InvalidMethod;
    }
}

[[nodiscard]] bool GetInvocableEndpointObject(CapabilitySpace& space, const CapabilityHandle handle,
                                              ObjectId& output) noexcept
{
    const auto lookup = space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
    if (!lookup.HasValue())
    {
        return false;
    }
    output = lookup.GetValue().GetDescriptor().Object;
    return true;
}

class HostedFixture final
{
public:
    HostedFixture() noexcept
        : m_clock{100U}, m_objects{m_synchronization}, m_revocations{m_synchronization}, m_issuer{m_objects},
          m_clientSpace{m_objects, m_revocations, m_clock, 0x1001'1001'1001'1001ULL, 0x2001'2001'2001'2001ULL},
          m_directorySpace{m_objects, m_revocations, m_clock, 0x1002'1002'1002'1002ULL, 0x2002'2002'2002'2002ULL},
          m_providerSpace{m_objects, m_revocations, m_clock, 0x1003'1003'1003'1003ULL, 0x2003'2003'2003'2003ULL},
          m_firstDirectoryEndpoint{m_directorySpace, ResourceDomainId{0xD101U}},
          m_secondDirectoryEndpoint{m_directorySpace, ResourceDomainId{0xD102U}},
          m_replyEndpoint{m_clientSpace, ResourceDomainId{0xC101U}},
          m_firstTargetEndpoint{m_providerSpace, ResourceDomainId{0xA101U}},
          m_secondTargetEndpoint{m_providerSpace, ResourceDomainId{0xA102U}},
          m_firstService{ObjectDirectoryServiceConfiguration{
              .Core =
                  {
                      .DirectoryGeneration = FirstDirectoryGeneration,
                      .LastIssuedRevision = 0U,
                  },
              .ExpectedSealedObjectSelector = FirstDirectorySelector,
          }},
          m_secondService{ObjectDirectoryServiceConfiguration{
              .Core =
                  {
                      .DirectoryGeneration = SecondDirectoryGeneration,
                      .LastIssuedRevision = 0U,
                  },
              .ExpectedSealedObjectSelector = SecondDirectorySelector,
          }}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        if (!m_revocations.IsReady() || !m_clientSpace.IsReady() || !m_directorySpace.IsReady() ||
            !m_providerSpace.IsReady() || !m_firstDirectoryEndpoint.IsReady() || !m_secondDirectoryEndpoint.IsReady() ||
            !m_replyEndpoint.IsReady() || !m_firstTargetEndpoint.IsReady() || !m_secondTargetEndpoint.IsReady())
        {
            return false;
        }

        bool succeeded = RegisterEndpoint(m_firstDirectoryEndpoint, m_firstDirectoryObject);
        succeeded = RegisterEndpoint(m_secondDirectoryEndpoint, m_secondDirectoryObject) && succeeded;
        succeeded = RegisterEndpoint(m_replyEndpoint, m_replyObject) && succeeded;
        succeeded = RegisterEndpoint(m_firstTargetEndpoint, m_firstTargetObject) && succeeded;
        succeeded = RegisterEndpoint(m_secondTargetEndpoint, m_secondTargetObject) && succeeded;
        if (!succeeded)
        {
            return false;
        }

        succeeded = CreateDomain(m_firstDirectoryDomain);
        succeeded = CreateDomain(m_secondDirectoryDomain) && succeeded;
        succeeded = CreateDomain(m_replyDomain) && succeeded;
        succeeded = CreateDomain(m_firstTargetDomain) && succeeded;
        succeeded = CreateDomain(m_secondTargetDomain) && succeeded;
        if (!succeeded)
        {
            return false;
        }

        succeeded = Mint(m_providerSpace, m_firstDirectoryObject, m_firstDirectoryDomain, ManageInvocationRights(),
                         FirstDirectorySelector, m_providerFirstDirectory);
        succeeded = Mint(m_clientSpace, m_firstDirectoryObject, m_firstDirectoryDomain, InvokeRights(),
                         FirstDirectorySelector, m_clientFirstDirectory) &&
                    succeeded;
        succeeded = Mint(m_providerSpace, m_secondDirectoryObject, m_secondDirectoryDomain, ManageInvocationRights(),
                         SecondDirectorySelector, m_providerSecondDirectory) &&
                    succeeded;
        succeeded = Mint(m_clientSpace, m_secondDirectoryObject, m_secondDirectoryDomain, InvokeRights(),
                         SecondDirectorySelector, m_clientSecondDirectory) &&
                    succeeded;
        succeeded =
            Mint(m_providerSpace, m_replyObject, m_replyDomain, ReplySourceRights(), ReplySelector, m_providerReply) &&
            succeeded;
        succeeded =
            Mint(m_clientSpace, m_replyObject, m_replyDomain, ReplySourceRights(), ReplySelector, m_clientReply) &&
            succeeded;
        succeeded = Mint(m_providerSpace, m_firstTargetObject, m_firstTargetDomain, TargetSourceRights(),
                         FirstTargetSelector, m_providerFirstTarget) &&
                    succeeded;
        succeeded = Mint(m_providerSpace, m_secondTargetObject, m_secondTargetDomain, TargetSourceRights(),
                         SecondTargetSelector, m_providerSecondTarget) &&
                    succeeded;
        return succeeded;
    }

    void Shutdown(TestContext& runner) noexcept
    {
        bool succeeded{true};
        succeeded = DiscardEndpoint(m_firstDirectoryEndpoint) && succeeded;
        succeeded = DiscardEndpoint(m_secondDirectoryEndpoint) && succeeded;
        succeeded = DiscardEndpoint(m_replyEndpoint) && succeeded;
        succeeded = DiscardEndpoint(m_firstTargetEndpoint) && succeeded;
        succeeded = DiscardEndpoint(m_secondTargetEndpoint) && succeeded;

        succeeded = DrainService(m_firstService) && succeeded;
        succeeded = DrainService(m_secondService) && succeeded;

        for (Size index{0U}; index < m_clientOwnedCount; ++index)
        {
            succeeded = CloseCapability(m_clientSpace, m_clientOwnedCapabilities[index]) && succeeded;
        }
        m_clientOwnedCount = 0U;

        succeeded = CloseCapability(m_clientSpace, m_clientFirstDirectory) && succeeded;
        succeeded = CloseCapability(m_clientSpace, m_clientSecondDirectory) && succeeded;
        succeeded = CloseCapability(m_clientSpace, m_clientReply) && succeeded;
        succeeded = CloseCapability(m_providerSpace, m_providerFirstDirectory) && succeeded;
        succeeded = CloseCapability(m_providerSpace, m_providerSecondDirectory) && succeeded;
        succeeded = CloseCapability(m_providerSpace, m_providerReply) && succeeded;
        succeeded = CloseCapability(m_providerSpace, m_providerFirstTarget) && succeeded;
        succeeded = CloseCapability(m_providerSpace, m_providerSecondTarget) && succeeded;

        succeeded = (m_clientSpace.GetActiveCount() == 0U) && succeeded;
        succeeded = (m_directorySpace.GetActiveCount() == 0U) && succeeded;
        succeeded = (m_providerSpace.GetActiveCount() == 0U) && succeeded;
        succeeded = DestroyRegisteredObjects() && succeeded;
        ASTERIA_EXPECT(runner, "teardown closes every capability once and collects all endpoint objects", succeeded);
    }

    [[nodiscard]] bool TrackClientCapability(const CapabilityHandle capability) noexcept
    {
        if (!capability.IsValid() || (m_clientOwnedCount == MaximumTrackedClientCapabilities))
        {
            return false;
        }
        for (Size index{0U}; index < m_clientOwnedCount; ++index)
        {
            if (HandlesAreEqual(m_clientOwnedCapabilities[index], capability))
            {
                return false;
            }
        }
        m_clientOwnedCapabilities[m_clientOwnedCount] = capability;
        ++m_clientOwnedCount;
        return true;
    }

    [[nodiscard]] ObjectDirectoryServiceCore& GetFirstService() noexcept
    {
        return m_firstService;
    }

    [[nodiscard]] ObjectDirectoryServiceCore& GetSecondService() noexcept
    {
        return m_secondService;
    }

    [[nodiscard]] Endpoint& GetFirstDirectoryEndpoint() noexcept
    {
        return m_firstDirectoryEndpoint;
    }

    [[nodiscard]] Endpoint& GetSecondDirectoryEndpoint() noexcept
    {
        return m_secondDirectoryEndpoint;
    }

    [[nodiscard]] Endpoint& GetReplyEndpoint() noexcept
    {
        return m_replyEndpoint;
    }

    [[nodiscard]] Endpoint& GetFirstTargetEndpoint() noexcept
    {
        return m_firstTargetEndpoint;
    }

    [[nodiscard]] CapabilitySpace& GetClientSpace() noexcept
    {
        return m_clientSpace;
    }

    [[nodiscard]] CapabilitySpace& GetDirectorySpace() noexcept
    {
        return m_directorySpace;
    }

    [[nodiscard]] CapabilitySpace& GetProviderSpace() noexcept
    {
        return m_providerSpace;
    }

    [[nodiscard]] CapabilityHandle GetProviderFirstDirectory() const noexcept
    {
        return m_providerFirstDirectory;
    }

    [[nodiscard]] CapabilityHandle GetProviderSecondDirectory() const noexcept
    {
        return m_providerSecondDirectory;
    }

    [[nodiscard]] CapabilityHandle GetClientFirstDirectory() const noexcept
    {
        return m_clientFirstDirectory;
    }

    [[nodiscard]] CapabilityHandle GetClientSecondDirectory() const noexcept
    {
        return m_clientSecondDirectory;
    }

    [[nodiscard]] CapabilityHandle GetProviderReply() const noexcept
    {
        return m_providerReply;
    }

    [[nodiscard]] CapabilityHandle GetClientReply() const noexcept
    {
        return m_clientReply;
    }

    [[nodiscard]] CapabilityHandle GetProviderFirstTarget() const noexcept
    {
        return m_providerFirstTarget;
    }

    [[nodiscard]] CapabilityHandle GetProviderSecondTarget() const noexcept
    {
        return m_providerSecondTarget;
    }

    [[nodiscard]] ObjectId GetFirstTargetObject() const noexcept
    {
        return m_firstTargetObject;
    }

    [[nodiscard]] ObjectId GetSecondTargetObject() const noexcept
    {
        return m_secondTargetObject;
    }

private:
    static constexpr Size MaximumTrackedClientCapabilities{4U};
    static constexpr Size RegisteredEndpointCount{5U};

    [[nodiscard]] bool RegisterEndpoint(Endpoint& endpoint, ObjectId& output) noexcept
    {
        const auto result = m_issuer.CreateObject(endpoint);
        if (!result.HasValue())
        {
            return false;
        }
        output = result.GetValue();
        ++m_registeredObjectCount;
        return true;
    }

    [[nodiscard]] bool CreateDomain(RevocationDomainId& output) noexcept
    {
        const auto result = m_revocations.CreateRoot();
        if (!result.HasValue())
        {
            return false;
        }
        output = result.GetValue();
        return true;
    }

    [[nodiscard]] bool Mint(CapabilitySpace& destination, const ObjectId object, const RevocationDomainId domain,
                            const CapabilityRights rights, const UInt64 sealedSelector,
                            CapabilityHandle& output) noexcept
    {
        const auto result =
            m_issuer.Mint(destination, object, rights, domain, MonotonicTime::Infinite(), sealedSelector);
        if (!result.HasValue())
        {
            return false;
        }
        output = result.GetValue();
        return true;
    }

    [[nodiscard]] static bool DiscardEndpoint(Endpoint& endpoint) noexcept
    {
        return endpoint.DiscardUnread().HasValue();
    }

    [[nodiscard]] bool DrainService(ObjectDirectoryServiceCore& service) noexcept
    {
        if (service.BeginQuiesce() != DirectoryCoreError::None)
        {
            return false;
        }
        for (Size index{0U}; index <= Executive::ObjectDirectory::ObjectDirectoryCore::MaximumBindings; ++index)
        {
            CapabilityHandle capability{};
            const DirectoryCoreError error = service.TakeNextOwnedEndpoint(capability);
            if (error == DirectoryCoreError::NoOwnedCapability)
            {
                return true;
            }
            if ((error != DirectoryCoreError::None) || !CloseCapability(m_directorySpace, capability))
            {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] static bool CloseCapability(CapabilitySpace& space, CapabilityHandle& capability) noexcept
    {
        if (!capability.IsValid())
        {
            return true;
        }
        if (space.Close(capability) != CapabilityError::Ok)
        {
            return false;
        }
        capability = {};
        return true;
    }

    [[nodiscard]] bool DestroyRegisteredObjects() noexcept
    {
        ObjectId objects[RegisteredEndpointCount]{
            m_firstDirectoryObject, m_secondDirectoryObject, m_replyObject, m_firstTargetObject, m_secondTargetObject,
        };
        bool succeeded{true};
        for (Size index{0U}; index < RegisteredEndpointCount; ++index)
        {
            if (!objects[index].IsValid())
            {
                continue;
            }
            if (m_issuer.DestroyObject(objects[index]) != ObjectRegistryError::Ok)
            {
                succeeded = false;
            }
        }
        for (Size index{0U}; index < m_registeredObjectCount; ++index)
        {
            const auto collected = m_issuer.CollectDestroyedObject();
            if (!collected.HasValue() || !collected.GetValue().IsFromRegistry(m_objects) ||
                (collected.GetValue().Instance == nullptr))
            {
                succeeded = false;
            }
        }
        m_firstDirectoryObject = {};
        m_secondDirectoryObject = {};
        m_replyObject = {};
        m_firstTargetObject = {};
        m_secondTargetObject = {};
        m_registeredObjectCount = 0U;
        return (m_objects.GetActiveCount() == 0U) && succeeded;
    }

    SpinLock m_synchronization;
    ManualClock m_clock;
    Asteria::TestSupport::TestObjectRegistry m_objects;
    RevocationRegistry m_revocations;
    RootCapabilityIssuer m_issuer;
    CapabilitySpace m_clientSpace;
    CapabilitySpace m_directorySpace;
    CapabilitySpace m_providerSpace;
    Endpoint m_firstDirectoryEndpoint;
    Endpoint m_secondDirectoryEndpoint;
    Endpoint m_replyEndpoint;
    Endpoint m_firstTargetEndpoint;
    Endpoint m_secondTargetEndpoint;
    ObjectDirectoryServiceCore m_firstService;
    ObjectDirectoryServiceCore m_secondService;
    ObjectId m_firstDirectoryObject{};
    ObjectId m_secondDirectoryObject{};
    ObjectId m_replyObject{};
    ObjectId m_firstTargetObject{};
    ObjectId m_secondTargetObject{};
    RevocationDomainId m_firstDirectoryDomain{};
    RevocationDomainId m_secondDirectoryDomain{};
    RevocationDomainId m_replyDomain{};
    RevocationDomainId m_firstTargetDomain{};
    RevocationDomainId m_secondTargetDomain{};
    CapabilityHandle m_providerFirstDirectory{};
    CapabilityHandle m_clientFirstDirectory{};
    CapabilityHandle m_providerSecondDirectory{};
    CapabilityHandle m_clientSecondDirectory{};
    CapabilityHandle m_providerReply{};
    CapabilityHandle m_clientReply{};
    CapabilityHandle m_providerFirstTarget{};
    CapabilityHandle m_providerSecondTarget{};
    CapabilityHandle m_clientOwnedCapabilities[MaximumTrackedClientCapabilities]{};
    Size m_clientOwnedCount{0U};
    Size m_registeredObjectCount{0U};
};

[[nodiscard]] MessageEnvelope BuildBind(const BindIfAbsentRequest& request,
                                        ObjectDirectoryProtocolError& error) noexcept
{
    MessageEnvelope message{};
    error = ObjectDirectoryProtocol::BuildBindIfAbsentRequest(request, message);
    return message;
}

[[nodiscard]] MessageEnvelope BuildReplace(const ReplaceIfVersionRequest& request,
                                           ObjectDirectoryProtocolError& error) noexcept
{
    MessageEnvelope message{};
    error = ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(request, message);
    return message;
}

[[nodiscard]] MessageEnvelope BuildResolve(const ResolveRequest& request, ObjectDirectoryProtocolError& error) noexcept
{
    MessageEnvelope message{};
    error = ObjectDirectoryProtocol::BuildResolveRequest(request, message);
    return message;
}

[[nodiscard]] MessageEnvelope BuildRemove(const RemoveIfVersionRequest& request,
                                          ObjectDirectoryProtocolError& error) noexcept
{
    MessageEnvelope message{};
    error = ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(request, message);
    return message;
}

void TestHostedEndToEndFlow(TestContext& runner) noexcept
{
    HostedFixture fixture;
    const bool initialized = fixture.Initialize();
    ASTERIA_EXPECT(runner, "real registry, capability spaces, and endpoint objects initialize", initialized);
    if (!initialized)
    {
        fixture.Shutdown(runner);
        return;
    }

    const DirectoryName name = MakeName("shared.service", 14U);
    ASTERIA_EXPECT(runner, "shared directory name is canonical", name.IsValid());

    ObjectDirectoryProtocolError buildError{};
    const BindIfAbsentRequest firstBindRequest =
        MakeBindRequest(name, fixture.GetProviderFirstTarget(), fixture.GetProviderReply(), 11U, 0x101U);
    const MessageEnvelope firstBindMessage = BuildBind(firstBindRequest, buildError);
    const ExchangeResult firstBind = CompleteExchange(
        fixture.GetFirstService(), fixture.GetFirstDirectoryEndpoint(), fixture.GetProviderFirstDirectory(),
        fixture.GetProviderSpace(), ResourceDomainId{0xB101U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), firstBindMessage);
    MutationReply firstBindReply{};
    const ObjectDirectoryProtocolError firstBindParse =
        ParseMutationReply(ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier, firstBind.Reply, firstBindReply);
    ASTERIA_EXPECT(runner, "bind traverses trusted invocation, AIPD delivery, dispatch, and copied reply endpoint",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(firstBind, 1U),
                   firstBindParse == ObjectDirectoryProtocolError::None,
                   firstBindReply.Status == DirectoryStatus::Success,
                   firstBindReply.Version.DirectoryGeneration == FirstDirectoryGeneration,
                   firstBind.RequestMetadata.SenderEndpointSealedObjectSelector == FirstDirectorySelector,
                   firstBind.RequestMetadata.SenderEndpointRights.IsEqualTo(ManageInvocationRights()),
                   fixture.GetDirectorySpace().GetActiveCount() == 1U);

    buildError = ObjectDirectoryProtocolError::None;
    const BindIfAbsentRequest secondBindRequest =
        MakeBindRequest(name, fixture.GetProviderSecondTarget(), fixture.GetProviderReply(), 22U, 0x102U);
    const MessageEnvelope secondBindMessage = BuildBind(secondBindRequest, buildError);
    const ExchangeResult secondBind = CompleteExchange(
        fixture.GetSecondService(), fixture.GetSecondDirectoryEndpoint(), fixture.GetProviderSecondDirectory(),
        fixture.GetProviderSpace(), ResourceDomainId{0xB102U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), secondBindMessage);
    MutationReply secondBindReply{};
    const ObjectDirectoryProtocolError secondBindParse =
        ParseMutationReply(ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier, secondBind.Reply, secondBindReply);
    ASTERIA_EXPECT(runner, "the same name binds independently in a second directory node",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(secondBind, 1U),
                   secondBindParse == ObjectDirectoryProtocolError::None,
                   secondBindReply.Status == DirectoryStatus::Success,
                   secondBindReply.Version.DirectoryGeneration == SecondDirectoryGeneration,
                   secondBindReply.Version.DirectoryGeneration != firstBindReply.Version.DirectoryGeneration,
                   fixture.GetDirectorySpace().GetActiveCount() == 2U);

    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope firstResolveMessage =
        BuildResolve(MakeResolveRequest(name, fixture.GetClientReply(), 0x201U), buildError);
    const ExchangeResult firstResolve = CompleteExchange(
        fixture.GetFirstService(), fixture.GetFirstDirectoryEndpoint(), fixture.GetClientFirstDirectory(),
        fixture.GetClientSpace(), ResourceDomainId{0xC201U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), firstResolveMessage);
    const bool trackedFirstResolution = firstResolve.ReplyDelivered && (firstResolve.Reply.AttachmentCount == 1U) &&
                                        fixture.TrackClientCapability(firstResolve.Reply.Attachments[0U].Handle);
    ResolveReply firstResolveReply{};
    const ObjectDirectoryProtocolError firstResolveParse =
        ObjectDirectoryProtocol::ParseResolveReply(firstResolve.Reply, firstResolveReply);
    ASTERIA_EXPECT(runner, "resolve copies the bound target into the client and preserves correlation metadata",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(firstResolve, 1U),
                   trackedFirstResolution, firstResolveParse == ObjectDirectoryProtocolError::None,
                   firstResolveReply.Status == DirectoryStatus::Success, firstResolveReply.ServiceGeneration == 11U,
                   firstResolveReply.SelectedMinor == 5U, firstResolveReply.GrantedRights.IsEqualTo(InvokeRights()),
                   firstResolve.ReplyMetadata.SenderEndpointSealedObjectSelector == ReplySelector);

    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope secondResolveMessage =
        BuildResolve(MakeResolveRequest(name, fixture.GetClientReply(), 0x202U), buildError);
    const ExchangeResult secondResolve = CompleteExchange(
        fixture.GetSecondService(), fixture.GetSecondDirectoryEndpoint(), fixture.GetClientSecondDirectory(),
        fixture.GetClientSpace(), ResourceDomainId{0xC202U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), secondResolveMessage);
    const bool trackedSecondResolution = secondResolve.ReplyDelivered && (secondResolve.Reply.AttachmentCount == 1U) &&
                                         fixture.TrackClientCapability(secondResolve.Reply.Attachments[0U].Handle);
    ResolveReply secondResolveReply{};
    const ObjectDirectoryProtocolError secondResolveParse =
        ObjectDirectoryProtocol::ParseResolveReply(secondResolve.Reply, secondResolveReply);
    ObjectId firstResolvedObject{};
    ObjectId secondResolvedObject{};
    const bool resolvedObjectsKnown =
        GetInvocableEndpointObject(fixture.GetClientSpace(), firstResolveReply.TargetEndpoint, firstResolvedObject) &&
        GetInvocableEndpointObject(fixture.GetClientSpace(), secondResolveReply.TargetEndpoint, secondResolvedObject);
    ASTERIA_EXPECT(runner, "same-name resolves remain isolated by the selected directory endpoint",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(secondResolve, 1U),
                   trackedSecondResolution, secondResolveParse == ObjectDirectoryProtocolError::None,
                   secondResolveReply.Status == DirectoryStatus::Success, secondResolveReply.ServiceGeneration == 22U,
                   resolvedObjectsKnown, firstResolvedObject.GetValue() == fixture.GetFirstTargetObject().GetValue(),
                   secondResolvedObject.GetValue() == fixture.GetSecondTargetObject().GetValue());

    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope replaceMessage =
        BuildReplace(MakeReplaceRequest(name, firstBindReply.Version, fixture.GetProviderSecondTarget(),
                                        fixture.GetProviderReply(), 33U, 0x301U),
                     buildError);
    const ExchangeResult replace = CompleteExchange(
        fixture.GetFirstService(), fixture.GetFirstDirectoryEndpoint(), fixture.GetProviderFirstDirectory(),
        fixture.GetProviderSpace(), ResourceDomainId{0xB301U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), replaceMessage);
    MutationReply replaceReply{};
    const ObjectDirectoryProtocolError replaceParse =
        ParseMutationReply(ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier, replace.Reply, replaceReply);
    ASTERIA_EXPECT(runner, "successful compare-and-swap rebind closes displaced directory custody",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(replace, 2U),
                   replaceParse == ObjectDirectoryProtocolError::None, replaceReply.Status == DirectoryStatus::Success,
                   replaceReply.Version.DirectoryGeneration == firstBindReply.Version.DirectoryGeneration,
                   replaceReply.Version.Revision > firstBindReply.Version.Revision,
                   fixture.GetDirectorySpace().GetActiveCount() == 2U);

    const Size providerCountBeforeStale = fixture.GetProviderSpace().GetActiveCount();
    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope staleMessage =
        BuildReplace(MakeReplaceRequest(name, firstBindReply.Version, fixture.GetProviderFirstTarget(),
                                        fixture.GetProviderReply(), 44U, 0x302U),
                     buildError);
    const ExchangeResult stale = CompleteExchange(fixture.GetFirstService(), fixture.GetFirstDirectoryEndpoint(),
                                                  fixture.GetProviderFirstDirectory(), fixture.GetProviderSpace(),
                                                  ResourceDomainId{0xB302U}, fixture.GetDirectorySpace(),
                                                  fixture.GetReplyEndpoint(), fixture.GetClientSpace(), staleMessage);
    MutationReply staleReply{};
    const ObjectDirectoryProtocolError staleParse =
        ParseMutationReply(ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier, stale.Reply, staleReply);
    ObjectId preservedProviderObject{};
    const bool providerTargetPreserved = GetInvocableEndpointObject(
        fixture.GetProviderSpace(), fixture.GetProviderFirstTarget(), preservedProviderObject);
    ASTERIA_EXPECT(runner,
                   "stale CAS rejection closes only transferred copies and preserves "
                   "the provider target handle",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(stale, 2U),
                   staleParse == ObjectDirectoryProtocolError::None,
                   staleReply.Status == DirectoryStatus::BindingChanged, staleReply.Version.DirectoryGeneration == 0U,
                   staleReply.Version.Revision == 0U, providerTargetPreserved,
                   preservedProviderObject.GetValue() == fixture.GetFirstTargetObject().GetValue(),
                   fixture.GetProviderSpace().GetActiveCount() == providerCountBeforeStale,
                   fixture.GetDirectorySpace().GetActiveCount() == 2U);

    const MessageEnvelope probe = MakeProbeMessage(0x401U);
    const bool oldTargetSend =
        SendThroughCapability(fixture.GetClientSpace(), firstResolveReply.TargetEndpoint, ResourceDomainId{0xC401U},
                              probe, fixture.GetFirstTargetEndpoint());
    EndpointReceiveMetadata probeMetadata{};
    MessageEnvelope deliveredProbe{};
    const bool oldTargetReceive = ReceiveCanonicalDelivery(fixture.GetFirstTargetEndpoint(), fixture.GetProviderSpace(),
                                                           probeMetadata, deliveredProbe);
    ASTERIA_EXPECT(runner, "a target resolved before rebind remains invocable after directory custody is replaced",
                   oldTargetSend, oldTargetReceive, deliveredProbe.TransactionIdentifier == probe.TransactionIdentifier,
                   deliveredProbe.Payload[0U] == probe.Payload[0U],
                   probeMetadata.SenderEndpointRights.IsEqualTo(InvokeRights()),
                   probeMetadata.SenderEndpointSealedObjectSelector == FirstTargetSelector);

    const CapabilityError revokeError = fixture.GetProviderSpace().Revoke(fixture.GetProviderFirstTarget());
    const bool oldTargetRevoked = [&fixture, &firstResolveReply]() noexcept
    {
        const auto lookup = fixture.GetClientSpace().Lookup(firstResolveReply.TargetEndpoint, ObjectType::Endpoint,
                                                            CapabilityOperation::Invoke);
        return !lookup.HasValue() && (lookup.GetError() == CapabilityError::Revoked);
    }();
    ASTERIA_EXPECT(runner, "explicit revocation, rather than rebind, invalidates the previously resolved target",
                   revokeError == CapabilityError::Ok, oldTargetRevoked);

    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope reboundResolveMessage =
        BuildResolve(MakeResolveRequest(name, fixture.GetClientReply(), 0x402U), buildError);
    const ExchangeResult reboundResolve = CompleteExchange(
        fixture.GetFirstService(), fixture.GetFirstDirectoryEndpoint(), fixture.GetClientFirstDirectory(),
        fixture.GetClientSpace(), ResourceDomainId{0xC402U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), reboundResolveMessage);
    const bool trackedReboundResolution = reboundResolve.ReplyDelivered &&
                                          (reboundResolve.Reply.AttachmentCount == 1U) &&
                                          fixture.TrackClientCapability(reboundResolve.Reply.Attachments[0U].Handle);
    ResolveReply reboundResolveReply{};
    const ObjectDirectoryProtocolError reboundResolveParse =
        ObjectDirectoryProtocol::ParseResolveReply(reboundResolve.Reply, reboundResolveReply);
    ObjectId reboundObject{};
    const bool reboundObjectKnown =
        GetInvocableEndpointObject(fixture.GetClientSpace(), reboundResolveReply.TargetEndpoint, reboundObject);
    ASTERIA_EXPECT(runner, "resolve after rebind returns a distinct capability for the replacement target",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(reboundResolve, 1U),
                   trackedReboundResolution, reboundResolveParse == ObjectDirectoryProtocolError::None,
                   reboundResolveReply.Status == DirectoryStatus::Success, reboundResolveReply.ServiceGeneration == 33U,
                   reboundObjectKnown, reboundObject.GetValue() == fixture.GetSecondTargetObject().GetValue(),
                   !HandlesAreEqual(reboundResolveReply.TargetEndpoint, secondResolveReply.TargetEndpoint));

    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope removeMessage =
        BuildRemove(MakeRemoveRequest(name, replaceReply.Version, fixture.GetProviderReply(), 0x501U), buildError);
    const ExchangeResult remove = CompleteExchange(fixture.GetFirstService(), fixture.GetFirstDirectoryEndpoint(),
                                                   fixture.GetProviderFirstDirectory(), fixture.GetProviderSpace(),
                                                   ResourceDomainId{0xB501U}, fixture.GetDirectorySpace(),
                                                   fixture.GetReplyEndpoint(), fixture.GetClientSpace(), removeMessage);
    MutationReply removeReply{};
    const ObjectDirectoryProtocolError removeParse =
        ParseMutationReply(ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier, remove.Reply, removeReply);
    ASTERIA_EXPECT(runner, "remove closes the first node's final target custody",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(remove, 2U),
                   removeParse == ObjectDirectoryProtocolError::None, removeReply.Status == DirectoryStatus::Success,
                   removeReply.Version.Revision > replaceReply.Version.Revision,
                   fixture.GetDirectorySpace().GetActiveCount() == 1U);

    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope missingMessage =
        BuildResolve(MakeResolveRequest(name, fixture.GetClientReply(), 0x502U), buildError);
    const ExchangeResult missing = CompleteExchange(
        fixture.GetFirstService(), fixture.GetFirstDirectoryEndpoint(), fixture.GetClientFirstDirectory(),
        fixture.GetClientSpace(), ResourceDomainId{0xC502U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), missingMessage);
    ResolveReply missingReply{};
    const ObjectDirectoryProtocolError missingParse =
        ObjectDirectoryProtocol::ParseResolveReply(missing.Reply, missingReply);
    ASTERIA_EXPECT(runner, "removed binding is no longer resolvable while the second node remains isolated",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(missing, 1U),
                   missingParse == ObjectDirectoryProtocolError::None, missingReply.Status == DirectoryStatus::NotFound,
                   !missingReply.TargetEndpoint.IsValid(), fixture.GetDirectorySpace().GetActiveCount() == 1U);

    buildError = ObjectDirectoryProtocolError::None;
    const MessageEnvelope secondRemoveMessage =
        BuildRemove(MakeRemoveRequest(name, secondBindReply.Version, fixture.GetProviderReply(), 0x503U), buildError);
    const ExchangeResult secondRemove = CompleteExchange(
        fixture.GetSecondService(), fixture.GetSecondDirectoryEndpoint(), fixture.GetProviderSecondDirectory(),
        fixture.GetProviderSpace(), ResourceDomainId{0xB503U}, fixture.GetDirectorySpace(), fixture.GetReplyEndpoint(),
        fixture.GetClientSpace(), secondRemoveMessage);
    MutationReply secondRemoveReply{};
    const ObjectDirectoryProtocolError secondRemoveParse = ParseMutationReply(
        ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier, secondRemove.Reply, secondRemoveReply);
    ASTERIA_EXPECT(runner, "removing the second node releases its independent same-name binding",
                   buildError == ObjectDirectoryProtocolError::None, ExchangeCompleted(secondRemove, 2U),
                   secondRemoveParse == ObjectDirectoryProtocolError::None,
                   secondRemoveReply.Status == DirectoryStatus::Success,
                   fixture.GetDirectorySpace().GetActiveCount() == 0U);

    fixture.Shutdown(runner);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectDirectoryHostedIntegrationTests"};
    Asteria::Test::TestHostedEndToEndFlow(runner);
    return runner.Finish();
}
