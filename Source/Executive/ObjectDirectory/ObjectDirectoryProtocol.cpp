// SPDX-License-Identifier: Apache-2.0

#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"

namespace Asteria::Executive::ObjectDirectory
{
namespace
{
constexpr Foundation::Size InterfaceLowOffset{0U};
constexpr Foundation::Size InterfaceHighOffset{8U};
constexpr Foundation::Size ResolveDeadlineOffset{16U};
constexpr Foundation::Size ResolveMajorOffset{24U};
constexpr Foundation::Size ResolveMinimumMinorOffset{26U};
constexpr Foundation::Size ResolveMaximumMinorOffset{28U};
constexpr Foundation::Size ResolveRightsOffset{30U};

constexpr Foundation::Size BindServiceGenerationOffset{16U};
constexpr Foundation::Size BindMajorOffset{24U};
constexpr Foundation::Size BindMinimumMinorOffset{26U};
constexpr Foundation::Size BindMaximumMinorOffset{28U};
constexpr Foundation::Size BindRightsOffset{30U};
constexpr Foundation::Size BindFlagsOffset{32U};
constexpr Foundation::Size BindReservedOffset{36U};

constexpr Foundation::Size ExpectedGenerationOffset{0U};
constexpr Foundation::Size ExpectedRevisionOffset{8U};
constexpr Foundation::Size ReplaceInterfaceLowOffset{16U};
constexpr Foundation::Size ReplaceInterfaceHighOffset{24U};
constexpr Foundation::Size ReplaceServiceGenerationOffset{32U};
constexpr Foundation::Size ReplaceMajorOffset{40U};
constexpr Foundation::Size ReplaceMinimumMinorOffset{42U};
constexpr Foundation::Size ReplaceMaximumMinorOffset{44U};
constexpr Foundation::Size ReplaceRightsOffset{46U};
constexpr Foundation::Size ReplaceFlagsOffset{48U};
constexpr Foundation::Size ReplaceReservedOffset{52U};

constexpr Foundation::Size RemoveFlagsOffset{16U};
constexpr Foundation::Size RemoveReservedOffset{20U};

constexpr Foundation::Size ReplyStatusOffset{0U};
constexpr Foundation::Size ReplyReservedOffset{2U};
constexpr Foundation::Size ReplyReservedSizeBytes{6U};
constexpr Foundation::Size ReplyGenerationOffset{8U};
constexpr Foundation::Size ReplyRevisionOffset{16U};

constexpr Foundation::Size ResolveReplyInterfaceLowOffset{24U};
constexpr Foundation::Size ResolveReplyInterfaceHighOffset{32U};
constexpr Foundation::Size ResolveReplyServiceGenerationOffset{40U};
constexpr Foundation::Size ResolveReplyDeadlineOffset{48U};
constexpr Foundation::Size ResolveReplyMajorOffset{56U};
constexpr Foundation::Size ResolveReplySelectedMinorOffset{58U};
constexpr Foundation::Size ResolveReplyRightsOffset{60U};
constexpr Foundation::Size ResolveReplyReservedOffset{62U};

[[nodiscard]] constexpr Foundation::UInt16 ReadLittleEndian16(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[0U]) |
                                           (static_cast<Foundation::UInt16>(bytes[1U]) << 8U));
}

[[nodiscard]] constexpr Foundation::UInt32 ReadLittleEndian32(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt32 value{0U};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        value |= static_cast<Foundation::UInt32>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

constexpr void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    for (Foundation::Size index{0U}; index < 2U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

constexpr void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

constexpr void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] constexpr bool TraceIsValid(const Kernel::Ipc::TraceContext& trace) noexcept
{
    const bool hasTraceIdentifier = (trace.TraceIdentifierHigh != 0U) || (trace.TraceIdentifierLow != 0U);
    return hasTraceIdentifier == (trace.SpanIdentifier != 0U);
}

[[nodiscard]] constexpr bool InterfaceIsDirectory(const Kernel::Ipc::InterfaceIdentifier interface) noexcept
{
    return (interface.GetLow() == ObjectDirectoryProtocol::Interface.GetLow()) &&
           (interface.GetHigh() == ObjectDirectoryProtocol::Interface.GetHigh());
}

[[nodiscard]] constexpr Kernel::Object::CapabilityRights InvokeRights() noexcept
{
    return Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke};
}

[[nodiscard]] constexpr bool RightsAreValidAndInvocable(const Kernel::Object::CapabilityRights rights) noexcept
{
    return rights.IsValid() && (rights.GetRawValue() != 0U) && rights.ContainsAll(InvokeRights());
}

[[nodiscard]] constexpr bool RawRightsAreValidAndInvocable(const Foundation::UInt16 rawRights) noexcept
{
    const Foundation::UInt16 knownRights = Kernel::Object::CapabilityRights::All().GetRawValue();
    const Foundation::UInt16 invoke = static_cast<Foundation::UInt16>(Kernel::Object::CapabilityRight::Invoke);
    return (rawRights != 0U) && ((rawRights & static_cast<Foundation::UInt16>(~knownRights)) == 0U) &&
           ((rawRights & invoke) != 0U);
}

[[nodiscard]] constexpr Kernel::Object::CapabilityRights RightsFromRaw(const Foundation::UInt16 rawRights) noexcept
{
    Kernel::Object::CapabilityRights rights{};
    for (Foundation::UInt16 index{0U}; index < 13U; ++index)
    {
        const Foundation::UInt16 bit = static_cast<Foundation::UInt16>(Foundation::UInt16{1U} << index);
        if ((rawRights & bit) != 0U)
        {
            rights = rights.With(static_cast<Kernel::Object::CapabilityRight>(bit));
        }
    }
    return rights;
}

[[nodiscard]] constexpr bool VersionRangeIsValid(const InterfaceVersionRange& version) noexcept
{
    return (version.Major != 0U) && (version.MinimumMinor <= version.MaximumMinor) && (version.Reserved == 0U);
}

[[nodiscard]] constexpr bool BindingVersionIsValid(const BindingVersion& version) noexcept
{
    return (version.DirectoryGeneration != 0U) && (version.Revision != 0U);
}

[[nodiscard]] constexpr bool BindingVersionIsZero(const BindingVersion& version) noexcept
{
    return (version.DirectoryGeneration == 0U) && (version.Revision == 0U);
}

[[nodiscard]] constexpr bool ResolveFailureFieldsAreZero(const ResolveReply& reply) noexcept
{
    return BindingVersionIsZero(reply.Version) && !reply.ServiceInterface.IsValid() &&
           (reply.ServiceGeneration == 0U) && (reply.GrantedDeadline.GetTicks() == 0U) && (reply.Major == 0U) &&
           (reply.SelectedMinor == 0U) && (reply.GrantedRights.GetRawValue() == 0U) &&
           (reply.TargetEndpoint.GetSelector() == 0U) && (reply.TargetEndpoint.GetAuthenticator() == 0U);
}

[[nodiscard]] constexpr bool StatusIsKnown(const DirectoryStatus status) noexcept
{
    return static_cast<Foundation::UInt16>(status) <= static_cast<Foundation::UInt16>(DirectoryStatus::InternalFailure);
}

[[nodiscard]] bool NameIsValid(const DirectoryName& name) noexcept
{
    const Foundation::Result<DirectoryName, DirectoryNameError> result = DirectoryName::Create(DirectoryNameView{
        .Data = name.GetData(),
        .SizeBytes = name.GetSizeBytes(),
    });
    return result.HasValue() && result.GetValue().IsEqualTo(name);
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AttachmentIsZero(const Kernel::Ipc::CapabilityAttachment& attachment) noexcept
{
    return !attachment.Handle.IsValid() && (attachment.Handle.GetAuthenticator() == 0U) &&
           (attachment.RequestedDeadline.GetTicks() == 0U) && (attachment.RequestedRights.GetRawValue() == 0U) &&
           (attachment.ExpectedType == Kernel::Object::ObjectType::Invalid) &&
           (attachment.TransferMode == Kernel::Ipc::CapabilityTransferMode::Invalid) && (attachment.Reserved == 0U);
}

[[nodiscard]] bool UnusedAttachmentsAreZero(const Kernel::Ipc::MessageEnvelope& message,
                                            const Foundation::Size firstUnused) noexcept
{
    for (Foundation::Size index{firstUnused}; index < Kernel::Ipc::MessageEnvelope::MaximumCapabilityAttachments;
         ++index)
    {
        if (!AttachmentIsZero(message.Attachments[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AllVariableDescriptorsAreZero(const Kernel::Ipc::MessageEnvelope& message) noexcept
{
    for (const Kernel::Ipc::VariablePayloadDescriptor& descriptor : message.VariablePayloads)
    {
        if ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool UnusedVariableDescriptorsAreZero(const Kernel::Ipc::MessageEnvelope& message) noexcept
{
    for (Foundation::Size index{1U}; index < Kernel::Ipc::MessageEnvelope::MaximumVariablePayloads; ++index)
    {
        const Kernel::Ipc::VariablePayloadDescriptor& descriptor = message.VariablePayloads[index];
        if ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool UnusedPayloadIsZero(const Kernel::Ipc::MessageEnvelope& message) noexcept
{
    if (message.PayloadSizeBytes > Kernel::Ipc::MessageEnvelope::MaximumPayloadBytes)
    {
        return false;
    }
    for (Foundation::Size index{message.PayloadSizeBytes}; index < Kernel::Ipc::MessageEnvelope::MaximumPayloadBytes;
         ++index)
    {
        if (message.Payload[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ObjectDirectoryProtocolError ValidateContext(const DirectoryMessageContext& context) noexcept
{
    if (context.TransactionIdentifier == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidTransactionIdentifier;
    }
    if (!TraceIsValid(context.Trace))
    {
        return ObjectDirectoryProtocolError::InvalidTraceContext;
    }
    return ObjectDirectoryProtocolError::None;
}

[[nodiscard]] ObjectDirectoryProtocolError ValidateCommonHeader(const Kernel::Ipc::MessageEnvelope& message,
                                                                const Foundation::UInt32 expectedMethod) noexcept
{
    if (!InterfaceIsDirectory(message.Interface))
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    if ((message.InterfaceVersionMajor != ObjectDirectoryProtocol::VersionMajor) ||
        (message.InterfaceVersionMinor != ObjectDirectoryProtocol::VersionMinor))
    {
        return ObjectDirectoryProtocolError::UnsupportedVersion;
    }
    if (message.MethodIdentifier != expectedMethod)
    {
        return ObjectDirectoryProtocolError::InvalidMethod;
    }
    if (message.TransactionIdentifier == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidTransactionIdentifier;
    }
    if (!TraceIsValid(message.Trace))
    {
        return ObjectDirectoryProtocolError::InvalidTraceContext;
    }
    if ((message.Reserved != 0U) || (message.ReservedAlignment != 0U))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }
    return ObjectDirectoryProtocolError::None;
}

void InitializeMessage(Kernel::Ipc::MessageEnvelope& message, const DirectoryMessageContext& context,
                       const Foundation::UInt32 method, const Foundation::UInt16 fixedPayloadSize,
                       const Foundation::UInt16 payloadSize, const Kernel::Ipc::MessageKind kind) noexcept
{
    message.Interface = Kernel::Ipc::InterfaceIdentifier{};
    message.TransactionIdentifier = 0U;
    message.Deadline = Kernel::Object::MonotonicTime{};
    message.Trace = Kernel::Ipc::TraceContext{};
    message.MethodIdentifier = 0U;
    message.InterfaceVersionMajor = 0U;
    message.InterfaceVersionMinor = 0U;
    message.FixedPayloadSizeBytes = 0U;
    message.PayloadSizeBytes = 0U;
    message.Kind = Kernel::Ipc::MessageKind::Invalid;
    message.Cancellation = Kernel::Ipc::CancellationState::None;
    message.VariablePayloadCount = 0U;
    message.AttachmentCount = 0U;
    message.Reserved = 0U;
    message.ReservedAlignment = 0U;
    for (Kernel::Ipc::CapabilityAttachment& attachment : message.Attachments)
    {
        attachment = Kernel::Ipc::CapabilityAttachment{};
    }
    for (Kernel::Ipc::VariablePayloadDescriptor& descriptor : message.VariablePayloads)
    {
        descriptor = Kernel::Ipc::VariablePayloadDescriptor{};
    }
    for (Foundation::UInt8& value : message.Payload)
    {
        value = 0U;
    }
    message.Interface = ObjectDirectoryProtocol::Interface;
    message.TransactionIdentifier = context.TransactionIdentifier;
    message.Deadline = context.Deadline;
    message.Trace = context.Trace;
    message.MethodIdentifier = method;
    message.InterfaceVersionMajor = ObjectDirectoryProtocol::VersionMajor;
    message.InterfaceVersionMinor = ObjectDirectoryProtocol::VersionMinor;
    message.FixedPayloadSizeBytes = fixedPayloadSize;
    message.PayloadSizeBytes = payloadSize;
    message.Kind = kind;
    message.Cancellation = Kernel::Ipc::CancellationState::None;
}

void AddName(Kernel::Ipc::MessageEnvelope& message, const DirectoryName& name,
             const Foundation::UInt16 fixedPayloadSize) noexcept
{
    const Foundation::UInt16 nameSize = static_cast<Foundation::UInt16>(name.GetSizeBytes());
    message.PayloadSizeBytes = static_cast<Foundation::UInt16>(fixedPayloadSize + nameSize);
    message.VariablePayloadCount = 1U;
    message.VariablePayloads[0U] = Kernel::Ipc::VariablePayloadDescriptor{
        .OffsetBytes = fixedPayloadSize,
        .SizeBytes = nameSize,
        .Reserved = 0U,
    };
    for (Foundation::Size index{0U}; index < name.GetSizeBytes(); ++index)
    {
        message.Payload[fixedPayloadSize + index] = name.GetData()[index];
    }
}

void AddReplyAttachment(Kernel::Ipc::MessageEnvelope& message,
                        const Kernel::Object::CapabilityHandle replyEndpoint) noexcept
{
    message.Attachments[0U] = Kernel::Ipc::CapabilityAttachment{
        .Handle = replyEndpoint,
        .RequestedDeadline = message.Deadline,
        .RequestedRights = InvokeRights(),
        .ExpectedType = Kernel::Object::ObjectType::Endpoint,
        .TransferMode = Kernel::Ipc::CapabilityTransferMode::Copy,
        .Reserved = 0U,
    };
}

void AddTargetAttachment(Kernel::Ipc::MessageEnvelope& message, const Kernel::Object::CapabilityHandle targetEndpoint,
                         const Kernel::Object::MonotonicTime targetDeadline,
                         const Kernel::Object::CapabilityRights publishedRights) noexcept
{
    message.Attachments[1U] = Kernel::Ipc::CapabilityAttachment{
        .Handle = targetEndpoint,
        .RequestedDeadline = targetDeadline,
        .RequestedRights = publishedRights.With(Kernel::Object::CapabilityRight::Duplicate),
        .ExpectedType = Kernel::Object::ObjectType::Endpoint,
        .TransferMode = Kernel::Ipc::CapabilityTransferMode::Copy,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool ReplyAttachmentIsValid(const Kernel::Ipc::CapabilityAttachment& attachment,
                                          const Kernel::Object::MonotonicTime deadline) noexcept
{
    return attachment.Handle.IsValid() && (attachment.RequestedDeadline.GetTicks() == deadline.GetTicks()) &&
           attachment.RequestedRights.IsEqualTo(InvokeRights()) &&
           (attachment.ExpectedType == Kernel::Object::ObjectType::Endpoint) &&
           (attachment.TransferMode == Kernel::Ipc::CapabilityTransferMode::Copy) && (attachment.Reserved == 0U);
}

[[nodiscard]] bool TargetAttachmentIsValid(const Kernel::Ipc::CapabilityAttachment& attachment,
                                           const Kernel::Object::CapabilityRights publishedRights) noexcept
{
    return attachment.Handle.IsValid() &&
           attachment.RequestedRights.IsEqualTo(publishedRights.With(Kernel::Object::CapabilityRight::Duplicate)) &&
           (attachment.ExpectedType == Kernel::Object::ObjectType::Endpoint) &&
           (attachment.TransferMode == Kernel::Ipc::CapabilityTransferMode::Copy) && (attachment.Reserved == 0U);
}

[[nodiscard]] ObjectDirectoryProtocolError ValidateRequestShape(const Kernel::Ipc::MessageEnvelope& message,
                                                                const Foundation::UInt16 fixedPayloadSize,
                                                                const Foundation::UInt8 attachmentCount) noexcept
{
    const Foundation::Size minimumPayloadSize = static_cast<Foundation::Size>(fixedPayloadSize) + 1U;
    const Foundation::Size maximumPayloadSize =
        static_cast<Foundation::Size>(fixedPayloadSize) + DirectoryName::MaximumSizeBytes;
    if ((message.Kind != Kernel::Ipc::MessageKind::Asynchronous) ||
        (message.Cancellation != Kernel::Ipc::CancellationState::None) ||
        (message.FixedPayloadSizeBytes != fixedPayloadSize) || (message.PayloadSizeBytes < minimumPayloadSize) ||
        (message.PayloadSizeBytes > maximumPayloadSize) || (message.VariablePayloadCount != 1U) ||
        (message.AttachmentCount != attachmentCount))
    {
        return ObjectDirectoryProtocolError::InvalidMessageContract;
    }
    const Kernel::Ipc::VariablePayloadDescriptor& nameDescriptor = message.VariablePayloads[0U];
    const Foundation::UInt16 expectedNameSize =
        static_cast<Foundation::UInt16>(message.PayloadSizeBytes - fixedPayloadSize);
    if ((nameDescriptor.OffsetBytes != fixedPayloadSize) || (nameDescriptor.SizeBytes != expectedNameSize) ||
        (nameDescriptor.Reserved != 0U))
    {
        return ObjectDirectoryProtocolError::InvalidMessageContract;
    }
    if (!UnusedVariableDescriptorsAreZero(message) || !UnusedAttachmentsAreZero(message, attachmentCount) ||
        !UnusedPayloadIsZero(message))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }
    return ObjectDirectoryProtocolError::None;
}

[[nodiscard]] ObjectDirectoryProtocolError ValidateReplyShape(const Kernel::Ipc::MessageEnvelope& message,
                                                              const Foundation::UInt16 payloadSize,
                                                              const Foundation::UInt8 attachmentCount) noexcept
{
    if ((message.Kind != Kernel::Ipc::MessageKind::Reply) ||
        (message.Cancellation != Kernel::Ipc::CancellationState::None) ||
        (message.FixedPayloadSizeBytes != payloadSize) || (message.PayloadSizeBytes != payloadSize) ||
        (message.VariablePayloadCount != 0U) || (message.AttachmentCount != attachmentCount))
    {
        return ObjectDirectoryProtocolError::InvalidMessageContract;
    }
    if (!AllVariableDescriptorsAreZero(message) || !UnusedAttachmentsAreZero(message, attachmentCount) ||
        !UnusedPayloadIsZero(message))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }
    return ObjectDirectoryProtocolError::None;
}

[[nodiscard]] ObjectDirectoryProtocolError DecodeName(const Kernel::Ipc::MessageEnvelope& message,
                                                      const Foundation::UInt16 fixedPayloadSize,
                                                      DirectoryName& output) noexcept
{
    const Foundation::Result<DirectoryName, DirectoryNameError> result = DirectoryName::Create(DirectoryNameView{
        .Data = message.Payload + fixedPayloadSize,
        .SizeBytes = message.VariablePayloads[0U].SizeBytes,
    });
    if (!result.HasValue())
    {
        return ObjectDirectoryProtocolError::InvalidName;
    }
    output = result.GetValue();
    return ObjectDirectoryProtocolError::None;
}

[[nodiscard]] DirectoryMessageContext DecodeContext(const Kernel::Ipc::MessageEnvelope& message) noexcept
{
    return DirectoryMessageContext{
        .Trace = message.Trace,
        .Deadline = message.Deadline,
        .TransactionIdentifier = message.TransactionIdentifier,
    };
}

[[nodiscard]] ObjectDirectoryProtocolError
ValidateBuildRequestCommon(const DirectoryName& name, const Kernel::Object::CapabilityHandle replyEndpoint,
                           const DirectoryMessageContext& context) noexcept
{
    const ObjectDirectoryProtocolError contextError = ValidateContext(context);
    if (contextError != ObjectDirectoryProtocolError::None)
    {
        return contextError;
    }
    if (!NameIsValid(name))
    {
        return ObjectDirectoryProtocolError::InvalidName;
    }
    if (!replyEndpoint.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidReplyCapability;
    }
    return ObjectDirectoryProtocolError::None;
}

[[nodiscard]] ObjectDirectoryProtocolError
ValidateRequestCommonAndShape(const Kernel::Ipc::MessageEnvelope& message, const Foundation::UInt32 method,
                              const Foundation::UInt16 fixedPayloadSize,
                              const Foundation::UInt8 attachmentCount) noexcept
{
    const ObjectDirectoryProtocolError headerError = ValidateCommonHeader(message, method);
    if (headerError != ObjectDirectoryProtocolError::None)
    {
        return headerError;
    }
    return ValidateRequestShape(message, fixedPayloadSize, attachmentCount);
}

void EncodeBindingVersion(const BindingVersion& version, Foundation::UInt8* const payload) noexcept
{
    WriteLittleEndian64(payload + ReplyGenerationOffset, version.DirectoryGeneration);
    WriteLittleEndian64(payload + ReplyRevisionOffset, version.Revision);
}

[[nodiscard]] BindingVersion DecodeBindingVersion(const Foundation::UInt8* const payload) noexcept
{
    return BindingVersion{
        .DirectoryGeneration = ReadLittleEndian64(payload + ReplyGenerationOffset),
        .Revision = ReadLittleEndian64(payload + ReplyRevisionOffset),
    };
}

[[nodiscard]] ObjectDirectoryProtocolError BuildMutationReply(const MutationReply& reply,
                                                              const Foundation::UInt32 method,
                                                              Kernel::Ipc::MessageEnvelope& output) noexcept
{
    const ObjectDirectoryProtocolError contextError = ValidateContext(reply.Context);
    if (contextError != ObjectDirectoryProtocolError::None)
    {
        return contextError;
    }
    if (!StatusIsKnown(reply.Status))
    {
        return ObjectDirectoryProtocolError::InvalidStatus;
    }
    if ((reply.Status == DirectoryStatus::Success) ? !BindingVersionIsValid(reply.Version)
                                                   : !BindingVersionIsZero(reply.Version))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }

    InitializeMessage(output, reply.Context, method, ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes,
                      ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes, Kernel::Ipc::MessageKind::Reply);
    WriteLittleEndian16(output.Payload + ReplyStatusOffset, static_cast<Foundation::UInt16>(reply.Status));
    EncodeBindingVersion(reply.Version, output.Payload);
    return ObjectDirectoryProtocolError::None;
}

[[nodiscard]] ObjectDirectoryProtocolError ParseMutationReply(const Kernel::Ipc::MessageEnvelope& message,
                                                              const Foundation::UInt32 method,
                                                              MutationReply& output) noexcept
{
    const ObjectDirectoryProtocolError headerError = ValidateCommonHeader(message, method);
    if (headerError != ObjectDirectoryProtocolError::None)
    {
        return headerError;
    }
    const ObjectDirectoryProtocolError shapeError =
        ValidateReplyShape(message, ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes, 0U);
    if (shapeError != ObjectDirectoryProtocolError::None)
    {
        return shapeError;
    }
    if (!BytesAreZero(message.Payload + ReplyReservedOffset, ReplyReservedSizeBytes))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }
    const DirectoryStatus status =
        static_cast<DirectoryStatus>(ReadLittleEndian16(message.Payload + ReplyStatusOffset));
    if (!StatusIsKnown(status))
    {
        return ObjectDirectoryProtocolError::InvalidStatus;
    }
    const BindingVersion version = DecodeBindingVersion(message.Payload);
    if ((status == DirectoryStatus::Success) ? !BindingVersionIsValid(version) : !BindingVersionIsZero(version))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }

    const MutationReply candidate{
        .Status = status,
        .Version = version,
        .Context = DecodeContext(message),
    };
    output = candidate;
    return ObjectDirectoryProtocolError::None;
}
} // namespace

ObjectDirectoryProtocolError ObjectDirectoryProtocol::BuildResolveRequest(const ResolveRequest& request,
                                                                          Kernel::Ipc::MessageEnvelope& output) noexcept
{
    ObjectDirectoryProtocolError error =
        ValidateBuildRequestCommon(request.Name, request.ReplyEndpoint, request.Context);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!request.ServiceInterface.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    if (!VersionRangeIsValid(request.RequiredVersion))
    {
        return ObjectDirectoryProtocolError::InvalidVersionRange;
    }
    if (!RightsAreValidAndInvocable(request.RequiredRights))
    {
        return ObjectDirectoryProtocolError::InvalidRights;
    }

    InitializeMessage(output, request.Context, ResolveMethodIdentifier, ResolveRequestPayloadSizeBytes,
                      ResolveRequestPayloadSizeBytes, Kernel::Ipc::MessageKind::Asynchronous);
    AddName(output, request.Name, ResolveRequestPayloadSizeBytes);
    output.AttachmentCount = 1U;
    AddReplyAttachment(output, request.ReplyEndpoint);
    WriteLittleEndian64(output.Payload + InterfaceLowOffset, request.ServiceInterface.GetLow());
    WriteLittleEndian64(output.Payload + InterfaceHighOffset, request.ServiceInterface.GetHigh());
    WriteLittleEndian64(output.Payload + ResolveDeadlineOffset, request.RequestedDeadline.GetTicks());
    WriteLittleEndian16(output.Payload + ResolveMajorOffset, request.RequiredVersion.Major);
    WriteLittleEndian16(output.Payload + ResolveMinimumMinorOffset, request.RequiredVersion.MinimumMinor);
    WriteLittleEndian16(output.Payload + ResolveMaximumMinorOffset, request.RequiredVersion.MaximumMinor);
    WriteLittleEndian16(output.Payload + ResolveRightsOffset, request.RequiredRights.GetRawValue());
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError ObjectDirectoryProtocol::ParseResolveRequest(const Kernel::Ipc::MessageEnvelope& message,
                                                                          ResolveRequest& output) noexcept
{
    ObjectDirectoryProtocolError error =
        ValidateRequestCommonAndShape(message, ResolveMethodIdentifier, ResolveRequestPayloadSizeBytes, 1U);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!ReplyAttachmentIsValid(message.Attachments[0U], message.Deadline))
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }

    DirectoryName name{};
    error = DecodeName(message, ResolveRequestPayloadSizeBytes, name);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    const Kernel::Ipc::InterfaceIdentifier serviceInterface{ReadLittleEndian64(message.Payload + InterfaceLowOffset),
                                                            ReadLittleEndian64(message.Payload + InterfaceHighOffset)};
    if (!serviceInterface.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    const InterfaceVersionRange requiredVersion{
        .Major = ReadLittleEndian16(message.Payload + ResolveMajorOffset),
        .MinimumMinor = ReadLittleEndian16(message.Payload + ResolveMinimumMinorOffset),
        .MaximumMinor = ReadLittleEndian16(message.Payload + ResolveMaximumMinorOffset),
        .Reserved = 0U,
    };
    if (!VersionRangeIsValid(requiredVersion))
    {
        return ObjectDirectoryProtocolError::InvalidVersionRange;
    }
    const Foundation::UInt16 rawRights = ReadLittleEndian16(message.Payload + ResolveRightsOffset);
    if (!RawRightsAreValidAndInvocable(rawRights))
    {
        return ObjectDirectoryProtocolError::InvalidRights;
    }

    const ResolveRequest candidate{
        .Name = name,
        .ServiceInterface = serviceInterface,
        .RequestedDeadline = Kernel::Object::MonotonicTime{ReadLittleEndian64(message.Payload + ResolveDeadlineOffset)},
        .RequiredVersion = requiredVersion,
        .RequiredRights = RightsFromRaw(rawRights),
        .ReplyEndpoint = message.Attachments[0U].Handle,
        .Context = DecodeContext(message),
    };
    output = candidate;
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::BuildBindIfAbsentRequest(const BindIfAbsentRequest& request,
                                                  Kernel::Ipc::MessageEnvelope& output) noexcept
{
    ObjectDirectoryProtocolError error =
        ValidateBuildRequestCommon(request.Name, request.ReplyEndpoint, request.Context);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!request.ServiceInterface.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    if (request.ServiceGeneration == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidServiceGeneration;
    }
    if (!VersionRangeIsValid(request.Version))
    {
        return ObjectDirectoryProtocolError::InvalidVersionRange;
    }
    if (!RightsAreValidAndInvocable(request.PublishedRights))
    {
        return ObjectDirectoryProtocolError::InvalidRights;
    }
    if (!request.TargetEndpoint.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidTargetCapability;
    }
    if (request.ReplyEndpoint.GetSelector() == request.TargetEndpoint.GetSelector())
    {
        return ObjectDirectoryProtocolError::InvalidTargetCapability;
    }

    InitializeMessage(output, request.Context, BindIfAbsentMethodIdentifier, BindIfAbsentRequestPayloadSizeBytes,
                      BindIfAbsentRequestPayloadSizeBytes, Kernel::Ipc::MessageKind::Asynchronous);
    AddName(output, request.Name, BindIfAbsentRequestPayloadSizeBytes);
    output.AttachmentCount = 2U;
    AddReplyAttachment(output, request.ReplyEndpoint);
    AddTargetAttachment(output, request.TargetEndpoint, request.TargetDeadline, request.PublishedRights);
    WriteLittleEndian64(output.Payload + InterfaceLowOffset, request.ServiceInterface.GetLow());
    WriteLittleEndian64(output.Payload + InterfaceHighOffset, request.ServiceInterface.GetHigh());
    WriteLittleEndian64(output.Payload + BindServiceGenerationOffset, request.ServiceGeneration);
    WriteLittleEndian16(output.Payload + BindMajorOffset, request.Version.Major);
    WriteLittleEndian16(output.Payload + BindMinimumMinorOffset, request.Version.MinimumMinor);
    WriteLittleEndian16(output.Payload + BindMaximumMinorOffset, request.Version.MaximumMinor);
    WriteLittleEndian16(output.Payload + BindRightsOffset, request.PublishedRights.GetRawValue());
    WriteLittleEndian32(output.Payload + BindFlagsOffset, 0U);
    WriteLittleEndian32(output.Payload + BindReservedOffset, 0U);
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::ParseBindIfAbsentRequest(const Kernel::Ipc::MessageEnvelope& message,
                                                  BindIfAbsentRequest& output) noexcept
{
    ObjectDirectoryProtocolError error =
        ValidateRequestCommonAndShape(message, BindIfAbsentMethodIdentifier, BindIfAbsentRequestPayloadSizeBytes, 2U);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!ReplyAttachmentIsValid(message.Attachments[0U], message.Deadline))
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }
    if ((ReadLittleEndian32(message.Payload + BindFlagsOffset) != 0U) ||
        (ReadLittleEndian32(message.Payload + BindReservedOffset) != 0U))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }

    DirectoryName name{};
    error = DecodeName(message, BindIfAbsentRequestPayloadSizeBytes, name);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    const Kernel::Ipc::InterfaceIdentifier serviceInterface{ReadLittleEndian64(message.Payload + InterfaceLowOffset),
                                                            ReadLittleEndian64(message.Payload + InterfaceHighOffset)};
    if (!serviceInterface.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    const Foundation::UInt64 serviceGeneration = ReadLittleEndian64(message.Payload + BindServiceGenerationOffset);
    if (serviceGeneration == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidServiceGeneration;
    }
    const InterfaceVersionRange version{
        .Major = ReadLittleEndian16(message.Payload + BindMajorOffset),
        .MinimumMinor = ReadLittleEndian16(message.Payload + BindMinimumMinorOffset),
        .MaximumMinor = ReadLittleEndian16(message.Payload + BindMaximumMinorOffset),
        .Reserved = 0U,
    };
    if (!VersionRangeIsValid(version))
    {
        return ObjectDirectoryProtocolError::InvalidVersionRange;
    }
    const Foundation::UInt16 rawRights = ReadLittleEndian16(message.Payload + BindRightsOffset);
    if (!RawRightsAreValidAndInvocable(rawRights))
    {
        return ObjectDirectoryProtocolError::InvalidRights;
    }
    const Kernel::Object::CapabilityRights publishedRights = RightsFromRaw(rawRights);
    if (!TargetAttachmentIsValid(message.Attachments[1U], publishedRights))
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }
    if (message.Attachments[0U].Handle.GetSelector() == message.Attachments[1U].Handle.GetSelector())
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }

    const BindIfAbsentRequest candidate{
        .Name = name,
        .ServiceInterface = serviceInterface,
        .ServiceGeneration = serviceGeneration,
        .Version = version,
        .PublishedRights = publishedRights,
        .TargetEndpoint = message.Attachments[1U].Handle,
        .TargetDeadline = message.Attachments[1U].RequestedDeadline,
        .ReplyEndpoint = message.Attachments[0U].Handle,
        .Context = DecodeContext(message),
    };
    output = candidate;
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(const ReplaceIfVersionRequest& request,
                                                      Kernel::Ipc::MessageEnvelope& output) noexcept
{
    ObjectDirectoryProtocolError error =
        ValidateBuildRequestCommon(request.Name, request.ReplyEndpoint, request.Context);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!BindingVersionIsValid(request.ExpectedVersion))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }
    if (!request.ServiceInterface.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    if (request.ServiceGeneration == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidServiceGeneration;
    }
    if (!VersionRangeIsValid(request.Version))
    {
        return ObjectDirectoryProtocolError::InvalidVersionRange;
    }
    if (!RightsAreValidAndInvocable(request.PublishedRights))
    {
        return ObjectDirectoryProtocolError::InvalidRights;
    }
    if (!request.TargetEndpoint.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidTargetCapability;
    }
    if (request.ReplyEndpoint.GetSelector() == request.TargetEndpoint.GetSelector())
    {
        return ObjectDirectoryProtocolError::InvalidTargetCapability;
    }

    InitializeMessage(output, request.Context, ReplaceIfVersionMethodIdentifier,
                      ReplaceIfVersionRequestPayloadSizeBytes, ReplaceIfVersionRequestPayloadSizeBytes,
                      Kernel::Ipc::MessageKind::Asynchronous);
    AddName(output, request.Name, ReplaceIfVersionRequestPayloadSizeBytes);
    output.AttachmentCount = 2U;
    AddReplyAttachment(output, request.ReplyEndpoint);
    AddTargetAttachment(output, request.TargetEndpoint, request.TargetDeadline, request.PublishedRights);
    WriteLittleEndian64(output.Payload + ExpectedGenerationOffset, request.ExpectedVersion.DirectoryGeneration);
    WriteLittleEndian64(output.Payload + ExpectedRevisionOffset, request.ExpectedVersion.Revision);
    WriteLittleEndian64(output.Payload + ReplaceInterfaceLowOffset, request.ServiceInterface.GetLow());
    WriteLittleEndian64(output.Payload + ReplaceInterfaceHighOffset, request.ServiceInterface.GetHigh());
    WriteLittleEndian64(output.Payload + ReplaceServiceGenerationOffset, request.ServiceGeneration);
    WriteLittleEndian16(output.Payload + ReplaceMajorOffset, request.Version.Major);
    WriteLittleEndian16(output.Payload + ReplaceMinimumMinorOffset, request.Version.MinimumMinor);
    WriteLittleEndian16(output.Payload + ReplaceMaximumMinorOffset, request.Version.MaximumMinor);
    WriteLittleEndian16(output.Payload + ReplaceRightsOffset, request.PublishedRights.GetRawValue());
    WriteLittleEndian32(output.Payload + ReplaceFlagsOffset, 0U);
    WriteLittleEndian32(output.Payload + ReplaceReservedOffset, 0U);
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(const Kernel::Ipc::MessageEnvelope& message,
                                                      ReplaceIfVersionRequest& output) noexcept
{
    ObjectDirectoryProtocolError error = ValidateRequestCommonAndShape(message, ReplaceIfVersionMethodIdentifier,
                                                                       ReplaceIfVersionRequestPayloadSizeBytes, 2U);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!ReplyAttachmentIsValid(message.Attachments[0U], message.Deadline))
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }
    if ((ReadLittleEndian32(message.Payload + ReplaceFlagsOffset) != 0U) ||
        (ReadLittleEndian32(message.Payload + ReplaceReservedOffset) != 0U))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }

    DirectoryName name{};
    error = DecodeName(message, ReplaceIfVersionRequestPayloadSizeBytes, name);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    const Kernel::Ipc::InterfaceIdentifier serviceInterface{
        ReadLittleEndian64(message.Payload + ReplaceInterfaceLowOffset),
        ReadLittleEndian64(message.Payload + ReplaceInterfaceHighOffset)};
    if (!serviceInterface.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    const Foundation::UInt64 serviceGeneration = ReadLittleEndian64(message.Payload + ReplaceServiceGenerationOffset);
    if (serviceGeneration == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidServiceGeneration;
    }
    const InterfaceVersionRange version{
        .Major = ReadLittleEndian16(message.Payload + ReplaceMajorOffset),
        .MinimumMinor = ReadLittleEndian16(message.Payload + ReplaceMinimumMinorOffset),
        .MaximumMinor = ReadLittleEndian16(message.Payload + ReplaceMaximumMinorOffset),
        .Reserved = 0U,
    };
    if (!VersionRangeIsValid(version))
    {
        return ObjectDirectoryProtocolError::InvalidVersionRange;
    }
    const Foundation::UInt16 rawRights = ReadLittleEndian16(message.Payload + ReplaceRightsOffset);
    if (!RawRightsAreValidAndInvocable(rawRights))
    {
        return ObjectDirectoryProtocolError::InvalidRights;
    }
    const Kernel::Object::CapabilityRights publishedRights = RightsFromRaw(rawRights);
    if (!TargetAttachmentIsValid(message.Attachments[1U], publishedRights))
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }
    if (message.Attachments[0U].Handle.GetSelector() == message.Attachments[1U].Handle.GetSelector())
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }
    const BindingVersion expectedVersion{
        .DirectoryGeneration = ReadLittleEndian64(message.Payload + ExpectedGenerationOffset),
        .Revision = ReadLittleEndian64(message.Payload + ExpectedRevisionOffset),
    };
    if (!BindingVersionIsValid(expectedVersion))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }

    const ReplaceIfVersionRequest candidate{
        .Name = name,
        .ExpectedVersion = expectedVersion,
        .ServiceInterface = serviceInterface,
        .ServiceGeneration = serviceGeneration,
        .Version = version,
        .PublishedRights = publishedRights,
        .TargetEndpoint = message.Attachments[1U].Handle,
        .TargetDeadline = message.Attachments[1U].RequestedDeadline,
        .ReplyEndpoint = message.Attachments[0U].Handle,
        .Context = DecodeContext(message),
    };
    output = candidate;
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(const RemoveIfVersionRequest& request,
                                                     Kernel::Ipc::MessageEnvelope& output) noexcept
{
    const ObjectDirectoryProtocolError error =
        ValidateBuildRequestCommon(request.Name, request.ReplyEndpoint, request.Context);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!BindingVersionIsValid(request.ExpectedVersion))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }

    InitializeMessage(output, request.Context, RemoveIfVersionMethodIdentifier, RemoveIfVersionRequestPayloadSizeBytes,
                      RemoveIfVersionRequestPayloadSizeBytes, Kernel::Ipc::MessageKind::Asynchronous);
    AddName(output, request.Name, RemoveIfVersionRequestPayloadSizeBytes);
    output.AttachmentCount = 1U;
    AddReplyAttachment(output, request.ReplyEndpoint);
    WriteLittleEndian64(output.Payload + ExpectedGenerationOffset, request.ExpectedVersion.DirectoryGeneration);
    WriteLittleEndian64(output.Payload + ExpectedRevisionOffset, request.ExpectedVersion.Revision);
    WriteLittleEndian32(output.Payload + RemoveFlagsOffset, 0U);
    WriteLittleEndian32(output.Payload + RemoveReservedOffset, 0U);
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::ParseRemoveIfVersionRequest(const Kernel::Ipc::MessageEnvelope& message,
                                                     RemoveIfVersionRequest& output) noexcept
{
    ObjectDirectoryProtocolError error = ValidateRequestCommonAndShape(message, RemoveIfVersionMethodIdentifier,
                                                                       RemoveIfVersionRequestPayloadSizeBytes, 1U);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if (!ReplyAttachmentIsValid(message.Attachments[0U], message.Deadline))
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }
    if ((ReadLittleEndian32(message.Payload + RemoveFlagsOffset) != 0U) ||
        (ReadLittleEndian32(message.Payload + RemoveReservedOffset) != 0U))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }

    DirectoryName name{};
    error = DecodeName(message, RemoveIfVersionRequestPayloadSizeBytes, name);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    const BindingVersion expectedVersion{
        .DirectoryGeneration = ReadLittleEndian64(message.Payload + ExpectedGenerationOffset),
        .Revision = ReadLittleEndian64(message.Payload + ExpectedRevisionOffset),
    };
    if (!BindingVersionIsValid(expectedVersion))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }
    const RemoveIfVersionRequest candidate{
        .Name = name,
        .ExpectedVersion = expectedVersion,
        .ReplyEndpoint = message.Attachments[0U].Handle,
        .Context = DecodeContext(message),
    };
    output = candidate;
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError ObjectDirectoryProtocol::BuildResolveReply(const ResolveReply& reply,
                                                                        Kernel::Ipc::MessageEnvelope& output) noexcept
{
    const ObjectDirectoryProtocolError contextError = ValidateContext(reply.Context);
    if (contextError != ObjectDirectoryProtocolError::None)
    {
        return contextError;
    }
    if (!StatusIsKnown(reply.Status))
    {
        return ObjectDirectoryProtocolError::InvalidStatus;
    }
    if (reply.Status == DirectoryStatus::Success)
    {
        if (!BindingVersionIsValid(reply.Version))
        {
            return ObjectDirectoryProtocolError::InvalidPayload;
        }
        if (!reply.ServiceInterface.IsValid())
        {
            return ObjectDirectoryProtocolError::InvalidInterface;
        }
        if (reply.ServiceGeneration == 0U)
        {
            return ObjectDirectoryProtocolError::InvalidServiceGeneration;
        }
        if (reply.Major == 0U)
        {
            return ObjectDirectoryProtocolError::InvalidVersionRange;
        }
        if (!RightsAreValidAndInvocable(reply.GrantedRights))
        {
            return ObjectDirectoryProtocolError::InvalidRights;
        }
        if (!reply.TargetEndpoint.IsValid())
        {
            return ObjectDirectoryProtocolError::InvalidTargetCapability;
        }
    }
    else if (!ResolveFailureFieldsAreZero(reply))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }

    InitializeMessage(output, reply.Context, ResolveMethodIdentifier, ResolveReplyPayloadSizeBytes,
                      ResolveReplyPayloadSizeBytes, Kernel::Ipc::MessageKind::Reply);
    WriteLittleEndian16(output.Payload + ReplyStatusOffset, static_cast<Foundation::UInt16>(reply.Status));
    if (reply.Status == DirectoryStatus::Success)
    {
        EncodeBindingVersion(reply.Version, output.Payload);
        WriteLittleEndian64(output.Payload + ResolveReplyInterfaceLowOffset, reply.ServiceInterface.GetLow());
        WriteLittleEndian64(output.Payload + ResolveReplyInterfaceHighOffset, reply.ServiceInterface.GetHigh());
        WriteLittleEndian64(output.Payload + ResolveReplyServiceGenerationOffset, reply.ServiceGeneration);
        WriteLittleEndian64(output.Payload + ResolveReplyDeadlineOffset, reply.GrantedDeadline.GetTicks());
        WriteLittleEndian16(output.Payload + ResolveReplyMajorOffset, reply.Major);
        WriteLittleEndian16(output.Payload + ResolveReplySelectedMinorOffset, reply.SelectedMinor);
        WriteLittleEndian16(output.Payload + ResolveReplyRightsOffset, reply.GrantedRights.GetRawValue());
        WriteLittleEndian16(output.Payload + ResolveReplyReservedOffset, 0U);
        output.AttachmentCount = 1U;
        output.Attachments[0U] = Kernel::Ipc::CapabilityAttachment{
            .Handle = reply.TargetEndpoint,
            .RequestedDeadline = reply.GrantedDeadline,
            .RequestedRights = reply.GrantedRights,
            .ExpectedType = Kernel::Object::ObjectType::Endpoint,
            .TransferMode = Kernel::Ipc::CapabilityTransferMode::Copy,
            .Reserved = 0U,
        };
    }
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError ObjectDirectoryProtocol::ParseResolveReply(const Kernel::Ipc::MessageEnvelope& message,
                                                                        ResolveReply& output) noexcept
{
    ObjectDirectoryProtocolError error = ValidateCommonHeader(message, ResolveMethodIdentifier);
    if (error != ObjectDirectoryProtocolError::None)
    {
        return error;
    }
    if ((message.Kind != Kernel::Ipc::MessageKind::Reply) ||
        (message.Cancellation != Kernel::Ipc::CancellationState::None) ||
        (message.FixedPayloadSizeBytes != ResolveReplyPayloadSizeBytes) ||
        (message.PayloadSizeBytes != ResolveReplyPayloadSizeBytes) || (message.VariablePayloadCount != 0U) ||
        (message.AttachmentCount > 1U))
    {
        return ObjectDirectoryProtocolError::InvalidMessageContract;
    }
    if (!AllVariableDescriptorsAreZero(message) || !UnusedAttachmentsAreZero(message, message.AttachmentCount) ||
        !UnusedPayloadIsZero(message))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }
    if (!BytesAreZero(message.Payload + ReplyReservedOffset, ReplyReservedSizeBytes))
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }
    const DirectoryStatus status =
        static_cast<DirectoryStatus>(ReadLittleEndian16(message.Payload + ReplyStatusOffset));
    if (!StatusIsKnown(status))
    {
        return ObjectDirectoryProtocolError::InvalidStatus;
    }

    if (status != DirectoryStatus::Success)
    {
        if (message.AttachmentCount != 0U)
        {
            return ObjectDirectoryProtocolError::InvalidMessageContract;
        }
        if (!BytesAreZero(message.Payload + ReplyGenerationOffset,
                          ResolveReplyPayloadSizeBytes - ReplyGenerationOffset))
        {
            return ObjectDirectoryProtocolError::NoncanonicalMessage;
        }
        const ResolveReply candidate{
            .Status = status,
            .Version = {},
            .ServiceInterface = {},
            .ServiceGeneration = 0U,
            .GrantedDeadline = {},
            .Major = 0U,
            .SelectedMinor = 0U,
            .GrantedRights = {},
            .TargetEndpoint = {},
            .Context = DecodeContext(message),
        };
        output = candidate;
        return ObjectDirectoryProtocolError::None;
    }

    if (message.AttachmentCount != 1U)
    {
        return ObjectDirectoryProtocolError::InvalidMessageContract;
    }
    if (ReadLittleEndian16(message.Payload + ResolveReplyReservedOffset) != 0U)
    {
        return ObjectDirectoryProtocolError::NoncanonicalMessage;
    }
    const BindingVersion version = DecodeBindingVersion(message.Payload);
    if (!BindingVersionIsValid(version))
    {
        return ObjectDirectoryProtocolError::InvalidPayload;
    }
    const Kernel::Ipc::InterfaceIdentifier serviceInterface{
        ReadLittleEndian64(message.Payload + ResolveReplyInterfaceLowOffset),
        ReadLittleEndian64(message.Payload + ResolveReplyInterfaceHighOffset)};
    if (!serviceInterface.IsValid())
    {
        return ObjectDirectoryProtocolError::InvalidInterface;
    }
    const Foundation::UInt64 serviceGeneration =
        ReadLittleEndian64(message.Payload + ResolveReplyServiceGenerationOffset);
    if (serviceGeneration == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidServiceGeneration;
    }
    const Foundation::UInt16 major = ReadLittleEndian16(message.Payload + ResolveReplyMajorOffset);
    if (major == 0U)
    {
        return ObjectDirectoryProtocolError::InvalidVersionRange;
    }
    const Foundation::UInt16 rawRights = ReadLittleEndian16(message.Payload + ResolveReplyRightsOffset);
    if (!RawRightsAreValidAndInvocable(rawRights))
    {
        return ObjectDirectoryProtocolError::InvalidRights;
    }
    const Kernel::Object::CapabilityRights grantedRights = RightsFromRaw(rawRights);
    const Kernel::Object::MonotonicTime grantedDeadline{
        ReadLittleEndian64(message.Payload + ResolveReplyDeadlineOffset)};
    const Kernel::Ipc::CapabilityAttachment& attachment = message.Attachments[0U];
    if (!attachment.Handle.IsValid() || (attachment.RequestedDeadline.GetTicks() != grantedDeadline.GetTicks()) ||
        !attachment.RequestedRights.IsEqualTo(grantedRights) ||
        (attachment.ExpectedType != Kernel::Object::ObjectType::Endpoint) ||
        (attachment.TransferMode != Kernel::Ipc::CapabilityTransferMode::Copy) || (attachment.Reserved != 0U))
    {
        return ObjectDirectoryProtocolError::InvalidAttachment;
    }

    const ResolveReply candidate{
        .Status = status,
        .Version = version,
        .ServiceInterface = serviceInterface,
        .ServiceGeneration = serviceGeneration,
        .GrantedDeadline = grantedDeadline,
        .Major = major,
        .SelectedMinor = ReadLittleEndian16(message.Payload + ResolveReplySelectedMinorOffset),
        .GrantedRights = grantedRights,
        .TargetEndpoint = attachment.Handle,
        .Context = DecodeContext(message),
    };
    output = candidate;
    return ObjectDirectoryProtocolError::None;
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::BuildBindIfAbsentReply(const MutationReply& reply,
                                                Kernel::Ipc::MessageEnvelope& output) noexcept
{
    return BuildMutationReply(reply, BindIfAbsentMethodIdentifier, output);
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::ParseBindIfAbsentReply(const Kernel::Ipc::MessageEnvelope& message,
                                                MutationReply& output) noexcept
{
    return ParseMutationReply(message, BindIfAbsentMethodIdentifier, output);
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::BuildReplaceIfVersionReply(const MutationReply& reply,
                                                    Kernel::Ipc::MessageEnvelope& output) noexcept
{
    return BuildMutationReply(reply, ReplaceIfVersionMethodIdentifier, output);
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::ParseReplaceIfVersionReply(const Kernel::Ipc::MessageEnvelope& message,
                                                    MutationReply& output) noexcept
{
    return ParseMutationReply(message, ReplaceIfVersionMethodIdentifier, output);
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::BuildRemoveIfVersionReply(const MutationReply& reply,
                                                   Kernel::Ipc::MessageEnvelope& output) noexcept
{
    return BuildMutationReply(reply, RemoveIfVersionMethodIdentifier, output);
}

ObjectDirectoryProtocolError
ObjectDirectoryProtocol::ParseRemoveIfVersionReply(const Kernel::Ipc::MessageEnvelope& message,
                                                   MutationReply& output) noexcept
{
    return ParseMutationReply(message, RemoveIfVersionMethodIdentifier, output);
}
} // namespace Asteria::Executive::ObjectDirectory
