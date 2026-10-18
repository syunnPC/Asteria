// SPDX-License-Identifier: Apache-2.0

#include "Executive/ObjectDirectory/ObjectDirectoryServiceCore.hpp"

#include "Kernel/Ipc/EndpointTypes.hpp"

namespace Asteria::Executive::ObjectDirectory
{
namespace
{
[[nodiscard]] constexpr bool HandlesAreEqual(const Kernel::Object::CapabilityHandle left,
                                             const Kernel::Object::CapabilityHandle right) noexcept
{
    return (left.GetSelector() == right.GetSelector()) && (left.GetAuthenticator() == right.GetAuthenticator());
}

[[nodiscard]] constexpr bool InterfaceIsZero(const Kernel::Ipc::InterfaceIdentifier interface) noexcept
{
    return (interface.GetLow() == 0U) && (interface.GetHigh() == 0U);
}

[[nodiscard]] bool AttachmentIsZero(const Kernel::Ipc::CapabilityAttachment& attachment) noexcept
{
    return !attachment.Handle.IsValid() && (attachment.Handle.GetAuthenticator() == 0U) &&
           (attachment.RequestedDeadline.GetTicks() == 0U) && (attachment.RequestedRights.GetRawValue() == 0U) &&
           (attachment.ExpectedType == Kernel::Object::ObjectType::Invalid) &&
           (attachment.TransferMode == Kernel::Ipc::CapabilityTransferMode::Invalid) && (attachment.Reserved == 0U);
}

[[nodiscard]] bool MessageIsZero(const Kernel::Ipc::MessageEnvelope& message) noexcept
{
    if (!InterfaceIsZero(message.Interface) || (message.TransactionIdentifier != 0U) ||
        (message.Deadline.GetTicks() != 0U) || (message.Trace.TraceIdentifierHigh != 0U) ||
        (message.Trace.TraceIdentifierLow != 0U) || (message.Trace.SpanIdentifier != 0U) ||
        (message.MethodIdentifier != 0U) || (message.InterfaceVersionMajor != 0U) ||
        (message.InterfaceVersionMinor != 0U) || (message.FixedPayloadSizeBytes != 0U) ||
        (message.PayloadSizeBytes != 0U) || (message.Kind != Kernel::Ipc::MessageKind::Invalid) ||
        (message.Cancellation != Kernel::Ipc::CancellationState::None) || (message.VariablePayloadCount != 0U) ||
        (message.AttachmentCount != 0U) || (message.Reserved != 0U) || (message.ReservedAlignment != 0U))
    {
        return false;
    }

    for (const Kernel::Ipc::CapabilityAttachment& attachment : message.Attachments)
    {
        if (!AttachmentIsZero(attachment))
        {
            return false;
        }
    }
    for (const Kernel::Ipc::VariablePayloadDescriptor& descriptor : message.VariablePayloads)
    {
        if ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U))
        {
            return false;
        }
    }
    for (const Foundation::UInt8 value : message.Payload)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool HasRight(const Kernel::Object::CapabilityRights rights,
                                      const Kernel::Object::CapabilityRight right) noexcept
{
    return rights.ContainsAll(Kernel::Object::CapabilityRights{right});
}

[[nodiscard]] constexpr BindingVersion ZeroBindingVersion() noexcept
{
    return BindingVersion{
        .DirectoryGeneration = 0U,
        .Revision = 0U,
    };
}
} // namespace

ObjectDirectoryServiceFailure ObjectDirectoryServiceCore::MakeFailure(const ObjectDirectoryServiceError service,
                                                                      const ObjectDirectoryProtocolError protocol,
                                                                      const DirectoryCoreError core) noexcept
{
    return ObjectDirectoryServiceFailure{
        .Code = service,
        .Protocol = protocol,
        .Core = core,
        .Reserved = 0U,
    };
}

bool ObjectDirectoryServiceCore::IsOutcomePristine(const ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    if (!MessageIsZero(output.PreparedReply) || output.ReplyEndpoint.IsValid() ||
        (output.ReplyEndpoint.GetAuthenticator() != 0U) || (output.CapabilityCloseCount != 0U) ||
        output.HasPreparedReply)
    {
        return false;
    }
    for (const Kernel::Object::CapabilityHandle capability : output.CapabilitiesToClose)
    {
        if (capability.IsValid() || (capability.GetAuthenticator() != 0U))
        {
            return false;
        }
    }
    for (const Foundation::UInt8 value : output.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

bool ObjectDirectoryServiceCore::AddCleanupCapability(const Kernel::Object::CapabilityHandle capability,
                                                      ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    if (!capability.IsValid())
    {
        return true;
    }
    for (Foundation::Size index{0U}; index < output.CapabilityCloseCount; ++index)
    {
        if (HandlesAreEqual(output.CapabilitiesToClose[index], capability))
        {
            return true;
        }
    }
    if (output.CapabilityCloseCount >= Kernel::Ipc::MessageEnvelope::MaximumCapabilityAttachments)
    {
        return false;
    }
    output.CapabilitiesToClose[output.CapabilityCloseCount] = capability;
    ++output.CapabilityCloseCount;
    return true;
}

void ObjectDirectoryServiceCore::CollectReceivedCapabilities(const Kernel::Ipc::MessageEnvelope& message,
                                                             ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    const Foundation::Size declaredCount =
        message.AttachmentCount <= Kernel::Ipc::MessageEnvelope::MaximumCapabilityAttachments
            ? message.AttachmentCount
            : Kernel::Ipc::MessageEnvelope::MaximumCapabilityAttachments;
    for (Foundation::Size index{0U}; index < declaredCount; ++index)
    {
        static_cast<void>(AddCleanupCapability(message.Attachments[index].Handle, output));
    }
}

void ObjectDirectoryServiceCore::RemoveCleanupCapability(const Kernel::Object::CapabilityHandle capability,
                                                         ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    for (Foundation::Size index{0U}; index < output.CapabilityCloseCount; ++index)
    {
        if (!HandlesAreEqual(output.CapabilitiesToClose[index], capability))
        {
            continue;
        }
        const Foundation::Size lastIndex = output.CapabilityCloseCount - 1U;
        output.CapabilitiesToClose[index] = output.CapabilitiesToClose[lastIndex];
        output.CapabilitiesToClose[lastIndex] = {};
        --output.CapabilityCloseCount;
        return;
    }
}

DirectoryStatus ObjectDirectoryServiceCore::MapMutationStatus(const DirectoryCoreError error) noexcept
{
    switch (error)
    {
    case DirectoryCoreError::None:
        return DirectoryStatus::Success;

    case DirectoryCoreError::AlreadyBound:
        return DirectoryStatus::AlreadyBound;

    case DirectoryCoreError::NotFound:
        return DirectoryStatus::NotFound;

    case DirectoryCoreError::BindingChanged:
        return DirectoryStatus::BindingChanged;

    case DirectoryCoreError::CapacityExceeded:
    case DirectoryCoreError::RevisionExhausted:
        return DirectoryStatus::ResourceExhausted;

    case DirectoryCoreError::LeaseUnavailable:
        return DirectoryStatus::LeaseUnavailable;

    case DirectoryCoreError::InvalidName:
    case DirectoryCoreError::InvalidCandidate:
    case DirectoryCoreError::InvalidInterface:
    case DirectoryCoreError::InvalidVersion:
    case DirectoryCoreError::InvalidRights:
    case DirectoryCoreError::CapabilityAlreadyOwned:
        return DirectoryStatus::InvalidRequest;

    case DirectoryCoreError::InvalidConfiguration:
    case DirectoryCoreError::NotReady:
    case DirectoryCoreError::OutputOccupied:
    case DirectoryCoreError::NoOwnedCapability:
        return DirectoryStatus::InternalFailure;
    }
    return DirectoryStatus::InternalFailure;
}

DirectoryStatus ObjectDirectoryServiceCore::MapResolveStatus(const DirectoryCoreError error) noexcept
{
    switch (error)
    {
    case DirectoryCoreError::None:
        return DirectoryStatus::Success;

    case DirectoryCoreError::NotFound:
        return DirectoryStatus::NotFound;

    case DirectoryCoreError::InvalidInterface:
    case DirectoryCoreError::InvalidVersion:
        return DirectoryStatus::NoCompatibleVersion;

    case DirectoryCoreError::InvalidRights:
        return DirectoryStatus::RightsUnavailable;

    case DirectoryCoreError::LeaseUnavailable:
        return DirectoryStatus::LeaseUnavailable;

    case DirectoryCoreError::CapacityExceeded:
    case DirectoryCoreError::RevisionExhausted:
        return DirectoryStatus::ResourceExhausted;

    case DirectoryCoreError::InvalidName:
    case DirectoryCoreError::InvalidCandidate:
    case DirectoryCoreError::CapabilityAlreadyOwned:
        return DirectoryStatus::InvalidRequest;

    case DirectoryCoreError::InvalidConfiguration:
    case DirectoryCoreError::NotReady:
    case DirectoryCoreError::AlreadyBound:
    case DirectoryCoreError::BindingChanged:
    case DirectoryCoreError::OutputOccupied:
    case DirectoryCoreError::NoOwnedCapability:
        return DirectoryStatus::InternalFailure;
    }
    return DirectoryStatus::InternalFailure;
}

ObjectDirectoryServiceFailure ObjectDirectoryServiceCore::BuildMutationReply(
    const Foundation::UInt32 methodIdentifier, const DirectoryStatus status, const BindingVersion version,
    const DirectoryMessageContext& context, const Kernel::Object::CapabilityHandle replyEndpoint,
    const DirectoryCoreError coreError, ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    const MutationReply reply{
        .Status = status,
        .Version = status == DirectoryStatus::Success ? version : ZeroBindingVersion(),
        .Context = context,
    };

    ObjectDirectoryProtocolError protocolError{ObjectDirectoryProtocolError::InvalidMethod};
    switch (methodIdentifier)
    {
    case ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier:
        protocolError = ObjectDirectoryProtocol::BuildBindIfAbsentReply(reply, output.PreparedReply);
        break;

    case ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier:
        protocolError = ObjectDirectoryProtocol::BuildReplaceIfVersionReply(reply, output.PreparedReply);
        break;

    case ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier:
        protocolError = ObjectDirectoryProtocol::BuildRemoveIfVersionReply(reply, output.PreparedReply);
        break;

    default:
        break;
    }
    if (protocolError != ObjectDirectoryProtocolError::None)
    {
        return MakeFailure(ObjectDirectoryServiceError::InternalFailure, protocolError, coreError);
    }

    output.ReplyEndpoint = replyEndpoint;
    output.HasPreparedReply = true;
    return MakeFailure(ObjectDirectoryServiceError::None, ObjectDirectoryProtocolError::None, coreError);
}

ObjectDirectoryServiceFailure ObjectDirectoryServiceCore::BuildResolveReply(
    const DirectoryStatus status, const DirectoryResolution& resolution, const DirectoryMessageContext& context,
    const Kernel::Object::CapabilityHandle replyEndpoint, const DirectoryCoreError coreError,
    ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    const bool succeeded = status == DirectoryStatus::Success;
    const ResolveReply reply{
        .Status = status,
        .Version = succeeded ? resolution.Version : ZeroBindingVersion(),
        .ServiceInterface = succeeded ? resolution.ServiceInterface : Kernel::Ipc::InterfaceIdentifier{},
        .ServiceGeneration = succeeded ? resolution.ServiceGeneration : 0U,
        .GrantedDeadline = succeeded ? resolution.GrantedDeadline : Kernel::Object::MonotonicTime{},
        .Major = succeeded ? resolution.Major : Foundation::UInt16{0U},
        .SelectedMinor = succeeded ? resolution.SelectedMinor : Foundation::UInt16{0U},
        .GrantedRights = succeeded ? resolution.GrantedRights : Kernel::Object::CapabilityRights{},
        .TargetEndpoint = succeeded ? resolution.TargetEndpoint : Kernel::Object::CapabilityHandle{},
        .Context = context,
    };
    const ObjectDirectoryProtocolError protocolError =
        ObjectDirectoryProtocol::BuildResolveReply(reply, output.PreparedReply);
    if (protocolError != ObjectDirectoryProtocolError::None)
    {
        return MakeFailure(ObjectDirectoryServiceError::InternalFailure, protocolError, coreError);
    }

    output.ReplyEndpoint = replyEndpoint;
    output.HasPreparedReply = true;
    return MakeFailure(ObjectDirectoryServiceError::None, ObjectDirectoryProtocolError::None, coreError);
}

[[gnu::noinline]] ObjectDirectoryServiceFailure
ObjectDirectoryServiceCore::DispatchResolve(const Kernel::Ipc::MessageEnvelope& message,
                                            ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    ResolveRequest request{};
    const ObjectDirectoryProtocolError protocolError = ObjectDirectoryProtocol::ParseResolveRequest(message, request);
    if (protocolError != ObjectDirectoryProtocolError::None)
    {
        return MakeFailure(ObjectDirectoryServiceError::ProtocolFailure, protocolError, DirectoryCoreError::None);
    }

    const DirectoryResolveQuery query{
        .ServiceInterface = request.ServiceInterface,
        .RequestedDeadline = request.RequestedDeadline,
        .RequiredVersion = request.RequiredVersion,
        .RequiredRights = request.RequiredRights,
        .Reserved = {},
    };
    DirectoryResolution resolution{};
    const DirectoryCoreError coreError = m_core.Resolve(request.Name, query, resolution);
    return BuildResolveReply(MapResolveStatus(coreError), resolution, request.Context, request.ReplyEndpoint, coreError,
                             output);
}

[[gnu::noinline]] ObjectDirectoryServiceFailure
ObjectDirectoryServiceCore::DispatchBindIfAbsent(const Kernel::Ipc::MessageEnvelope& message, const bool hasManage,
                                                 ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    BindIfAbsentRequest request{};
    const ObjectDirectoryProtocolError protocolError =
        ObjectDirectoryProtocol::ParseBindIfAbsentRequest(message, request);
    if (protocolError != ObjectDirectoryProtocolError::None)
    {
        return MakeFailure(ObjectDirectoryServiceError::ProtocolFailure, protocolError, DirectoryCoreError::None);
    }
    if (!hasManage)
    {
        return BuildMutationReply(ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier,
                                  DirectoryStatus::PermissionDenied, ZeroBindingVersion(), request.Context,
                                  request.ReplyEndpoint, DirectoryCoreError::None, output);
    }

    DirectoryBindingCandidate candidate{
        .Custody = request.TargetEndpoint,
        .ServiceInterface = request.ServiceInterface,
        .ServiceGeneration = request.ServiceGeneration,
        .TargetDeadline = request.TargetDeadline,
        .PublishedRights = request.PublishedRights,
        .CustodyRights = message.Attachments[1U].RequestedRights,
        .Type = message.Attachments[1U].ExpectedType,
        .Reserved = {},
        .Version = request.Version,
    };
    DirectoryMutationReceipt receipt{};
    const DirectoryCoreError coreError = m_core.BindIfAbsent(request.Name, candidate, receipt);
    if ((coreError == DirectoryCoreError::None) && !candidate.Custody.IsValid() &&
        (candidate.Custody.GetAuthenticator() == 0U))
    {
        RemoveCleanupCapability(request.TargetEndpoint, output);
    }
    return BuildMutationReply(ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier, MapMutationStatus(coreError),
                              receipt.Version, request.Context, request.ReplyEndpoint, coreError, output);
}

[[gnu::noinline]] ObjectDirectoryServiceFailure
ObjectDirectoryServiceCore::DispatchReplaceIfVersion(const Kernel::Ipc::MessageEnvelope& message, const bool hasManage,
                                                     ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    ReplaceIfVersionRequest request{};
    const ObjectDirectoryProtocolError protocolError =
        ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(message, request);
    if (protocolError != ObjectDirectoryProtocolError::None)
    {
        return MakeFailure(ObjectDirectoryServiceError::ProtocolFailure, protocolError, DirectoryCoreError::None);
    }
    if (!hasManage)
    {
        return BuildMutationReply(ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier,
                                  DirectoryStatus::PermissionDenied, ZeroBindingVersion(), request.Context,
                                  request.ReplyEndpoint, DirectoryCoreError::None, output);
    }

    DirectoryBindingCandidate candidate{
        .Custody = request.TargetEndpoint,
        .ServiceInterface = request.ServiceInterface,
        .ServiceGeneration = request.ServiceGeneration,
        .TargetDeadline = request.TargetDeadline,
        .PublishedRights = request.PublishedRights,
        .CustodyRights = message.Attachments[1U].RequestedRights,
        .Type = message.Attachments[1U].ExpectedType,
        .Reserved = {},
        .Version = request.Version,
    };
    DirectoryMutationReceipt receipt{};
    const DirectoryCoreError coreError =
        m_core.ReplaceIfVersion(request.Name, request.ExpectedVersion, candidate, receipt);
    if ((coreError == DirectoryCoreError::None) && !candidate.Custody.IsValid() &&
        (candidate.Custody.GetAuthenticator() == 0U))
    {
        RemoveCleanupCapability(request.TargetEndpoint, output);
        if (!AddCleanupCapability(receipt.DisplacedEndpoint, output))
        {
            return MakeFailure(ObjectDirectoryServiceError::InternalFailure, ObjectDirectoryProtocolError::None,
                               coreError);
        }
    }
    return BuildMutationReply(ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier, MapMutationStatus(coreError),
                              receipt.Version, request.Context, request.ReplyEndpoint, coreError, output);
}

[[gnu::noinline]] ObjectDirectoryServiceFailure
ObjectDirectoryServiceCore::DispatchRemoveIfVersion(const Kernel::Ipc::MessageEnvelope& message, const bool hasManage,
                                                    ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    RemoveIfVersionRequest request{};
    const ObjectDirectoryProtocolError protocolError =
        ObjectDirectoryProtocol::ParseRemoveIfVersionRequest(message, request);
    if (protocolError != ObjectDirectoryProtocolError::None)
    {
        return MakeFailure(ObjectDirectoryServiceError::ProtocolFailure, protocolError, DirectoryCoreError::None);
    }
    if (!hasManage)
    {
        return BuildMutationReply(ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier,
                                  DirectoryStatus::PermissionDenied, ZeroBindingVersion(), request.Context,
                                  request.ReplyEndpoint, DirectoryCoreError::None, output);
    }

    DirectoryMutationReceipt receipt{};
    const DirectoryCoreError coreError = m_core.RemoveIfVersion(request.Name, request.ExpectedVersion, receipt);
    if ((coreError == DirectoryCoreError::None) && !AddCleanupCapability(receipt.DisplacedEndpoint, output))
    {
        return MakeFailure(ObjectDirectoryServiceError::InternalFailure, ObjectDirectoryProtocolError::None, coreError);
    }
    return BuildMutationReply(ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier, MapMutationStatus(coreError),
                              receipt.Version, request.Context, request.ReplyEndpoint, coreError, output);
}

ObjectDirectoryServiceFailure
ObjectDirectoryServiceCore::Dispatch(const Kernel::Ipc::MessageEnvelope& message,
                                     const Kernel::Ipc::EndpointReceiveMetadata& metadata,
                                     ObjectDirectoryServiceDispatchOutcome& output) noexcept
{
    if (!IsOutcomePristine(output))
    {
        return MakeFailure(ObjectDirectoryServiceError::OutputOccupied, ObjectDirectoryProtocolError::None,
                           DirectoryCoreError::None);
    }

    CollectReceivedCapabilities(message, output);
    if (m_expectedSealedObjectSelector == 0U)
    {
        return MakeFailure(ObjectDirectoryServiceError::InvalidConfiguration, ObjectDirectoryProtocolError::None,
                           DirectoryCoreError::None);
    }
    if ((metadata.SenderEndpointSealedObjectSelector != m_expectedSealedObjectSelector) ||
        !HasRight(metadata.SenderEndpointRights, Kernel::Object::CapabilityRight::Invoke))
    {
        return MakeFailure(ObjectDirectoryServiceError::InvocationRejected, ObjectDirectoryProtocolError::None,
                           DirectoryCoreError::None);
    }

    const bool hasManage = HasRight(metadata.SenderEndpointRights, Kernel::Object::CapabilityRight::Manage);
    switch (message.MethodIdentifier)
    {
    case ObjectDirectoryProtocol::ResolveMethodIdentifier:
        return DispatchResolve(message, output);

    case ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier:
        return DispatchBindIfAbsent(message, hasManage, output);

    case ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier:
        return DispatchReplaceIfVersion(message, hasManage, output);

    case ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier:
        return DispatchRemoveIfVersion(message, hasManage, output);

    default:
        return MakeFailure(ObjectDirectoryServiceError::ProtocolFailure, ObjectDirectoryProtocolError::InvalidMethod,
                           DirectoryCoreError::None);
    }
}

DirectoryCoreError ObjectDirectoryServiceCore::BeginQuiesce() noexcept
{
    return m_core.BeginQuiesce();
}

DirectoryCoreError ObjectDirectoryServiceCore::TakeNextOwnedEndpoint(Kernel::Object::CapabilityHandle& output) noexcept
{
    return m_core.TakeNextOwnedEndpoint(output);
}

DirectoryCoreSnapshot ObjectDirectoryServiceCore::GetSnapshot() const noexcept
{
    return m_core.GetSnapshot();
}
} // namespace Asteria::Executive::ObjectDirectory
