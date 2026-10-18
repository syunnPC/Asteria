// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/MessageCodec.hpp"
#include "Kernel/Object/CapabilityWireTypes.hpp"

namespace Asteria::Kernel::Ipc
{
namespace
{
constexpr Foundation::Size MagicOffset{0U};
constexpr Foundation::Size WireVersionOffset{4U};
constexpr Foundation::Size HeaderSizeOffset{6U};
constexpr Foundation::Size TotalSizeOffset{8U};
constexpr Foundation::Size PayloadSizeOffset{12U};
constexpr Foundation::Size FixedPayloadSizeOffset{14U};
constexpr Foundation::Size InterfaceLowOffset{16U};
constexpr Foundation::Size InterfaceHighOffset{24U};
constexpr Foundation::Size MethodIdentifierOffset{32U};
constexpr Foundation::Size InterfaceVersionMajorOffset{36U};
constexpr Foundation::Size InterfaceVersionMinorOffset{38U};
constexpr Foundation::Size TransactionIdentifierOffset{40U};
constexpr Foundation::Size DeadlineOffset{48U};
constexpr Foundation::Size TraceIdentifierHighOffset{56U};
constexpr Foundation::Size TraceIdentifierLowOffset{64U};
constexpr Foundation::Size SpanIdentifierOffset{72U};
constexpr Foundation::Size VariablePayloadCountOffset{80U};
constexpr Foundation::Size AttachmentCountOffset{81U};
constexpr Foundation::Size MessageKindOffset{82U};
constexpr Foundation::Size CancellationStateOffset{83U};
constexpr Foundation::Size HeaderReservedOffset{84U};

constexpr Foundation::Size VariableOffsetOffset{0U};
constexpr Foundation::Size VariableSizeOffset{2U};
constexpr Foundation::Size VariableReservedOffset{4U};

constexpr Foundation::Size AttachmentSelectorOffset{0U};
constexpr Foundation::Size AttachmentAuthenticatorOffset{8U};
constexpr Foundation::Size AttachmentDeadlineOffset{16U};
constexpr Foundation::Size AttachmentRightsOffset{24U};
constexpr Foundation::Size AttachmentTransferModeOffset{26U};
constexpr Foundation::Size AttachmentObjectTypeOffset{27U};
constexpr Foundation::Size AttachmentReservedOffset{28U};

struct MessageHeaderFields
{
    InterfaceIdentifier Interface;
    Foundation::UInt64 TransactionIdentifier;
    Object::MonotonicTime Deadline;
    TraceContext Trace;
    Foundation::UInt32 MethodIdentifier;
    Foundation::UInt16 InterfaceVersionMajor;
    Foundation::UInt16 InterfaceVersionMinor;
    Foundation::UInt16 FixedPayloadSizeBytes;
    Foundation::UInt16 PayloadSizeBytes;
    MessageKind Kind;
    CancellationState Cancellation;
    Foundation::UInt8 VariablePayloadCount;
    Foundation::UInt8 AttachmentCount;
    Foundation::UInt32 Reserved;
};

[[nodiscard]] constexpr Foundation::UInt16 ReadLittleEndian16(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt16>(
        static_cast<Foundation::UInt16>(bytes[0]) |
        static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[1]) << 8U));
}

[[nodiscard]] constexpr Foundation::UInt32 ReadLittleEndian32(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt32 value{0U};
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        value |= static_cast<Foundation::UInt32>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

constexpr void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 2U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

constexpr void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

constexpr void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] bool RangesOverlap(const void* const first, const Foundation::Size firstSize, const void* const second,
                                 const Foundation::Size secondSize) noexcept
{
    if ((firstSize == 0U) || (secondSize == 0U))
    {
        return false;
    }

    const Foundation::UIntPointer firstStart = reinterpret_cast<Foundation::UIntPointer>(first);
    const Foundation::UIntPointer secondStart = reinterpret_cast<Foundation::UIntPointer>(second);
    const Foundation::UIntPointer maximum{~Foundation::UIntPointer{0U}};
    const Foundation::UIntPointer firstLength = static_cast<Foundation::UIntPointer>(firstSize - 1U);
    const Foundation::UIntPointer secondLength = static_cast<Foundation::UIntPointer>(secondSize - 1U);
    if ((firstStart > (maximum - firstLength)) || (secondStart > (maximum - secondLength)))
    {
        return true;
    }

    const Foundation::UIntPointer firstEnd = firstStart + firstLength;
    const Foundation::UIntPointer secondEnd = secondStart + secondLength;
    return (firstStart <= secondEnd) && (secondStart <= firstEnd);
}

[[nodiscard]] constexpr bool IsKnownMessageKind(const MessageKind kind) noexcept
{
    switch (kind)
    {
    case MessageKind::SynchronousCall:
    case MessageKind::Reply:
    case MessageKind::Asynchronous:
    case MessageKind::Cancellation:
        return true;

    case MessageKind::Invalid:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownCancellationState(const CancellationState state) noexcept
{
    switch (state)
    {
    case CancellationState::None:
    case CancellationState::Cancellable:
    case CancellationState::Requested:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownTransferMode(const CapabilityTransferMode mode) noexcept
{
    switch (mode)
    {
    case CapabilityTransferMode::Move:
    case CapabilityTransferMode::Copy:
        return true;

    case CapabilityTransferMode::Invalid:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr MessageCodecError ValidateHeaderFields(const MessageHeaderFields& fields) noexcept
{
    if (fields.Reserved != 0U)
    {
        return MessageCodecError::ReservedFieldNonzero;
    }
    if (!fields.Interface.IsValid())
    {
        return MessageCodecError::InvalidInterfaceIdentifier;
    }
    if (fields.MethodIdentifier == 0U)
    {
        return MessageCodecError::InvalidMethodIdentifier;
    }
    if (fields.InterfaceVersionMajor == 0U)
    {
        return MessageCodecError::InvalidInterfaceVersion;
    }
    if (fields.TransactionIdentifier == 0U)
    {
        return MessageCodecError::InvalidTransactionIdentifier;
    }

    const bool hasTraceIdentifier = (fields.Trace.TraceIdentifierHigh != 0U) || (fields.Trace.TraceIdentifierLow != 0U);
    if (hasTraceIdentifier != (fields.Trace.SpanIdentifier != 0U))
    {
        return MessageCodecError::InvalidTraceContext;
    }
    if (!IsKnownMessageKind(fields.Kind))
    {
        return MessageCodecError::InvalidMessageKind;
    }
    if (!IsKnownCancellationState(fields.Cancellation))
    {
        return MessageCodecError::InvalidCancellationState;
    }

    switch (fields.Kind)
    {
    case MessageKind::SynchronousCall:
    case MessageKind::Asynchronous:
        if (fields.Cancellation == CancellationState::Requested)
        {
            return MessageCodecError::InvalidMessageContract;
        }
        break;

    case MessageKind::Reply:
        if (fields.Cancellation != CancellationState::None)
        {
            return MessageCodecError::InvalidMessageContract;
        }
        break;

    case MessageKind::Cancellation:
        if ((fields.Cancellation != CancellationState::Requested) || (fields.FixedPayloadSizeBytes != 0U) ||
            (fields.PayloadSizeBytes != 0U) || (fields.VariablePayloadCount != 0U) || (fields.AttachmentCount != 0U))
        {
            return MessageCodecError::InvalidMessageContract;
        }
        break;

    case MessageKind::Invalid:
        return MessageCodecError::InvalidMessageKind;
    }
    return MessageCodecError::None;
}

[[nodiscard]] constexpr MessageCodecError ValidateAttachmentValues(const Object::CapabilityHandle handle,
                                                                   const Object::CapabilityRights requestedRights,
                                                                   const Object::ObjectType expectedType,
                                                                   const CapabilityTransferMode transferMode,
                                                                   const Foundation::UInt32 reserved) noexcept
{
    if (reserved != 0U)
    {
        return MessageCodecError::ReservedFieldNonzero;
    }
    if (!handle.IsValid())
    {
        return MessageCodecError::InvalidAttachmentHandle;
    }
    if (!requestedRights.IsValid() || (requestedRights.GetRawValue() == 0U))
    {
        return MessageCodecError::InvalidAttachmentRights;
    }
    if (!Object::IsKnownObjectType(expectedType))
    {
        return MessageCodecError::InvalidAttachmentObjectType;
    }
    if (!IsKnownTransferMode(transferMode))
    {
        return MessageCodecError::InvalidAttachmentTransferMode;
    }
    return MessageCodecError::None;
}

[[nodiscard]] MessageCodecError ValidateWireVariablePayloads(const Foundation::UInt8* const descriptors,
                                                             const MessageHeaderFields& fields) noexcept
{
    Foundation::UInt16 expectedOffset = fields.FixedPayloadSizeBytes;
    for (Foundation::UInt8 index{0U}; index < fields.VariablePayloadCount; ++index)
    {
        const Foundation::Size descriptorOffset =
            static_cast<Foundation::Size>(index) * MessageCodec::WireVariablePayloadDescriptorSizeBytes;
        const Foundation::UInt8* const descriptor = descriptors + descriptorOffset;
        const Foundation::UInt16 offset = ReadLittleEndian16(descriptor + VariableOffsetOffset);
        const Foundation::UInt16 size = ReadLittleEndian16(descriptor + VariableSizeOffset);
        if (ReadLittleEndian32(descriptor + VariableReservedOffset) != 0U)
        {
            return MessageCodecError::ReservedFieldNonzero;
        }
        if ((offset != expectedOffset) ||
            (size > static_cast<Foundation::UInt16>(fields.PayloadSizeBytes - expectedOffset)))
        {
            return MessageCodecError::InvalidVariablePayloadLayout;
        }
        expectedOffset = static_cast<Foundation::UInt16>(expectedOffset + size);
    }
    return expectedOffset == fields.PayloadSizeBytes ? MessageCodecError::None
                                                     : MessageCodecError::InvalidVariablePayloadLayout;
}

[[nodiscard]] MessageCodecError ValidateMessageVariablePayloads(const MessageEnvelope& message) noexcept
{
    Foundation::UInt16 expectedOffset = message.FixedPayloadSizeBytes;
    for (Foundation::Size index{0U}; index < MessageEnvelope::MaximumVariablePayloads; ++index)
    {
        const VariablePayloadDescriptor& descriptor = message.VariablePayloads[index];
        if (index >= message.VariablePayloadCount)
        {
            if ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U))
            {
                return MessageCodecError::InvalidVariablePayloadLayout;
            }
            continue;
        }

        if (descriptor.Reserved != 0U)
        {
            return MessageCodecError::ReservedFieldNonzero;
        }
        if ((descriptor.OffsetBytes != expectedOffset) ||
            (descriptor.SizeBytes > static_cast<Foundation::UInt16>(message.PayloadSizeBytes - expectedOffset)))
        {
            return MessageCodecError::InvalidVariablePayloadLayout;
        }
        expectedOffset = static_cast<Foundation::UInt16>(expectedOffset + descriptor.SizeBytes);
    }
    return expectedOffset == message.PayloadSizeBytes ? MessageCodecError::None
                                                      : MessageCodecError::InvalidVariablePayloadLayout;
}

[[nodiscard]] MessageCodecError ValidateWireAttachments(const Foundation::UInt8* const attachments,
                                                        const Foundation::UInt8 attachmentCount) noexcept
{
    for (Foundation::UInt8 index{0U}; index < attachmentCount; ++index)
    {
        const Foundation::Size attachmentOffset =
            static_cast<Foundation::Size>(index) * MessageCodec::WireCapabilityAttachmentSizeBytes;
        const Foundation::UInt8* const attachment = attachments + attachmentOffset;
        const Foundation::UInt64 selector = ReadLittleEndian64(attachment + AttachmentSelectorOffset);
        const Foundation::UInt64 authenticator = ReadLittleEndian64(attachment + AttachmentAuthenticatorOffset);
        const Foundation::UInt16 rightsBits = ReadLittleEndian16(attachment + AttachmentRightsOffset);
        Object::CapabilityRights rights{};
        if (!Object::TryDecodeCapabilityRights(rightsBits, rights))
        {
            return MessageCodecError::InvalidAttachmentRights;
        }
        Object::ObjectType type{Object::ObjectType::Invalid};
        if (!Object::TryDecodeObjectType(attachment[AttachmentObjectTypeOffset], type))
        {
            return MessageCodecError::InvalidAttachmentObjectType;
        }
        const auto transferMode = static_cast<CapabilityTransferMode>(attachment[AttachmentTransferModeOffset]);
        const MessageCodecError error =
            ValidateAttachmentValues(Object::CapabilityHandle{selector, authenticator}, rights, type, transferMode,
                                     ReadLittleEndian32(attachment + AttachmentReservedOffset));
        if (error != MessageCodecError::None)
        {
            return error;
        }

        for (Foundation::UInt8 previous{0U}; previous < index; ++previous)
        {
            const Foundation::Size previousOffset =
                static_cast<Foundation::Size>(previous) * MessageCodec::WireCapabilityAttachmentSizeBytes;
            if (ReadLittleEndian64(attachments + previousOffset + AttachmentSelectorOffset) == selector)
            {
                return MessageCodecError::DuplicateAttachment;
            }
        }
    }
    return MessageCodecError::None;
}

[[nodiscard]] MessageCodecError ValidateMessageAttachments(const MessageEnvelope& message) noexcept
{
    for (Foundation::Size index{0U}; index < MessageEnvelope::MaximumCapabilityAttachments; ++index)
    {
        const CapabilityAttachment& attachment = message.Attachments[index];
        if (index >= message.AttachmentCount)
        {
            if ((attachment.Handle.GetSelector() != 0U) || (attachment.Handle.GetAuthenticator() != 0U) ||
                (attachment.RequestedDeadline.GetTicks() != 0U) || (attachment.RequestedRights.GetRawValue() != 0U) ||
                (attachment.ExpectedType != Object::ObjectType::Invalid) ||
                (attachment.TransferMode != CapabilityTransferMode::Invalid) || (attachment.Reserved != 0U))
            {
                return MessageCodecError::InvalidMessageContract;
            }
            continue;
        }

        const MessageCodecError error =
            ValidateAttachmentValues(attachment.Handle, attachment.RequestedRights, attachment.ExpectedType,
                                     attachment.TransferMode, attachment.Reserved);
        if (error != MessageCodecError::None)
        {
            return error;
        }
        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            if (message.Attachments[previous].Handle.GetSelector() == attachment.Handle.GetSelector())
            {
                return MessageCodecError::DuplicateAttachment;
            }
        }
    }
    return MessageCodecError::None;
}

[[nodiscard]] constexpr Foundation::Size CalculateFrameSize(const Foundation::UInt8 variablePayloadCount,
                                                            const Foundation::UInt8 attachmentCount,
                                                            const Foundation::UInt16 payloadSizeBytes) noexcept
{
    return MessageCodec::WireHeaderSizeBytes +
           (static_cast<Foundation::Size>(variablePayloadCount) *
            MessageCodec::WireVariablePayloadDescriptorSizeBytes) +
           (static_cast<Foundation::Size>(attachmentCount) * MessageCodec::WireCapabilityAttachmentSizeBytes) +
           payloadSizeBytes;
}

[[nodiscard]] MessageCodecError ValidateMessage(const MessageEnvelope& message) noexcept
{
    if (message.PayloadSizeBytes > MessageEnvelope::MaximumPayloadBytes)
    {
        return MessageCodecError::PayloadTooLarge;
    }
    if (message.FixedPayloadSizeBytes > message.PayloadSizeBytes)
    {
        return MessageCodecError::InvalidFixedPayloadSize;
    }
    if (message.VariablePayloadCount > MessageEnvelope::MaximumVariablePayloads)
    {
        return MessageCodecError::TooManyVariablePayloads;
    }
    if (message.AttachmentCount > MessageEnvelope::MaximumCapabilityAttachments)
    {
        return MessageCodecError::TooManyCapabilityAttachments;
    }
    if ((message.Reserved != 0U) || (message.ReservedAlignment != 0U))
    {
        return MessageCodecError::ReservedFieldNonzero;
    }

    const MessageHeaderFields fields{
        .Interface = message.Interface,
        .TransactionIdentifier = message.TransactionIdentifier,
        .Deadline = message.Deadline,
        .Trace = message.Trace,
        .MethodIdentifier = message.MethodIdentifier,
        .InterfaceVersionMajor = message.InterfaceVersionMajor,
        .InterfaceVersionMinor = message.InterfaceVersionMinor,
        .FixedPayloadSizeBytes = message.FixedPayloadSizeBytes,
        .PayloadSizeBytes = message.PayloadSizeBytes,
        .Kind = message.Kind,
        .Cancellation = message.Cancellation,
        .VariablePayloadCount = message.VariablePayloadCount,
        .AttachmentCount = message.AttachmentCount,
        .Reserved = message.Reserved,
    };
    MessageCodecError error = ValidateHeaderFields(fields);
    if (error != MessageCodecError::None)
    {
        return error;
    }
    error = ValidateMessageVariablePayloads(message);
    if (error != MessageCodecError::None)
    {
        return error;
    }
    error = ValidateMessageAttachments(message);
    if (error != MessageCodecError::None)
    {
        return error;
    }
    for (Foundation::Size index{message.PayloadSizeBytes}; index < MessageEnvelope::MaximumPayloadBytes; ++index)
    {
        if (message.Payload[index] != 0U)
        {
            return MessageCodecError::InvalidMessageContract;
        }
    }
    return MessageCodecError::None;
}

void ClearAttachment(CapabilityAttachment& attachment) noexcept
{
    attachment.Handle = Object::CapabilityHandle{};
    attachment.RequestedDeadline = Object::MonotonicTime{};
    attachment.RequestedRights = Object::CapabilityRights{};
    attachment.ExpectedType = Object::ObjectType::Invalid;
    attachment.TransferMode = CapabilityTransferMode::Invalid;
    attachment.Reserved = 0U;
}
} // namespace

MessageCodecError MessageCodec::Decode(const ReadOnlyByteView input, MessageEnvelope& output) noexcept
{
    if ((input.Data == nullptr) && (input.SizeBytes != 0U))
    {
        return MessageCodecError::NullBuffer;
    }
    if (input.SizeBytes < WireHeaderSizeBytes)
    {
        return MessageCodecError::HeaderTooSmall;
    }
    if (input.SizeBytes > MaximumWireSizeBytes)
    {
        return MessageCodecError::FrameTooLarge;
    }
    if (RangesOverlap(input.Data, input.SizeBytes, &output, sizeof(output)))
    {
        return MessageCodecError::BufferOverlap;
    }
    if (ReadLittleEndian32(input.Data + MagicOffset) != WireMagic)
    {
        return MessageCodecError::InvalidMagic;
    }
    if (ReadLittleEndian16(input.Data + WireVersionOffset) != WireVersion)
    {
        return MessageCodecError::UnsupportedWireVersion;
    }
    if (ReadLittleEndian16(input.Data + HeaderSizeOffset) != WireHeaderSizeBytes)
    {
        return MessageCodecError::InvalidHeaderSize;
    }
    if (ReadLittleEndian32(input.Data + TotalSizeOffset) != input.SizeBytes)
    {
        return MessageCodecError::SizeMismatch;
    }

    const Foundation::UInt16 payloadSize = ReadLittleEndian16(input.Data + PayloadSizeOffset);
    const Foundation::UInt16 fixedPayloadSize = ReadLittleEndian16(input.Data + FixedPayloadSizeOffset);
    const Foundation::UInt8 variablePayloadCount = input.Data[VariablePayloadCountOffset];
    const Foundation::UInt8 attachmentCount = input.Data[AttachmentCountOffset];
    if (payloadSize > MessageEnvelope::MaximumPayloadBytes)
    {
        return MessageCodecError::PayloadTooLarge;
    }
    if (fixedPayloadSize > payloadSize)
    {
        return MessageCodecError::InvalidFixedPayloadSize;
    }
    if (variablePayloadCount > MessageEnvelope::MaximumVariablePayloads)
    {
        return MessageCodecError::TooManyVariablePayloads;
    }
    if (attachmentCount > MessageEnvelope::MaximumCapabilityAttachments)
    {
        return MessageCodecError::TooManyCapabilityAttachments;
    }
    const Foundation::Size expectedSize = CalculateFrameSize(variablePayloadCount, attachmentCount, payloadSize);
    if (expectedSize != input.SizeBytes)
    {
        return MessageCodecError::SizeMismatch;
    }

    const MessageHeaderFields fields{
        .Interface = InterfaceIdentifier{ReadLittleEndian64(input.Data + InterfaceLowOffset),
                                         ReadLittleEndian64(input.Data + InterfaceHighOffset)},
        .TransactionIdentifier = ReadLittleEndian64(input.Data + TransactionIdentifierOffset),
        .Deadline = Object::MonotonicTime{ReadLittleEndian64(input.Data + DeadlineOffset)},
        .Trace =
            TraceContext{
                .TraceIdentifierHigh = ReadLittleEndian64(input.Data + TraceIdentifierHighOffset),
                .TraceIdentifierLow = ReadLittleEndian64(input.Data + TraceIdentifierLowOffset),
                .SpanIdentifier = ReadLittleEndian64(input.Data + SpanIdentifierOffset),
            },
        .MethodIdentifier = ReadLittleEndian32(input.Data + MethodIdentifierOffset),
        .InterfaceVersionMajor = ReadLittleEndian16(input.Data + InterfaceVersionMajorOffset),
        .InterfaceVersionMinor = ReadLittleEndian16(input.Data + InterfaceVersionMinorOffset),
        .FixedPayloadSizeBytes = fixedPayloadSize,
        .PayloadSizeBytes = payloadSize,
        .Kind = static_cast<MessageKind>(input.Data[MessageKindOffset]),
        .Cancellation = static_cast<CancellationState>(input.Data[CancellationStateOffset]),
        .VariablePayloadCount = variablePayloadCount,
        .AttachmentCount = attachmentCount,
        .Reserved = ReadLittleEndian32(input.Data + HeaderReservedOffset),
    };
    MessageCodecError error = ValidateHeaderFields(fields);
    if (error != MessageCodecError::None)
    {
        return error;
    }

    const Foundation::UInt8* const variablePayloads = input.Data + WireHeaderSizeBytes;
    error = ValidateWireVariablePayloads(variablePayloads, fields);
    if (error != MessageCodecError::None)
    {
        return error;
    }
    const Foundation::Size attachmentsOffset =
        WireHeaderSizeBytes +
        (static_cast<Foundation::Size>(variablePayloadCount) * WireVariablePayloadDescriptorSizeBytes);
    const Foundation::UInt8* const attachments = input.Data + attachmentsOffset;
    error = ValidateWireAttachments(attachments, attachmentCount);
    if (error != MessageCodecError::None)
    {
        return error;
    }

    output.Interface = fields.Interface;
    output.TransactionIdentifier = fields.TransactionIdentifier;
    output.Deadline = fields.Deadline;
    output.Trace = fields.Trace;
    output.MethodIdentifier = fields.MethodIdentifier;
    output.InterfaceVersionMajor = fields.InterfaceVersionMajor;
    output.InterfaceVersionMinor = fields.InterfaceVersionMinor;
    output.FixedPayloadSizeBytes = fields.FixedPayloadSizeBytes;
    output.PayloadSizeBytes = fields.PayloadSizeBytes;
    output.Kind = fields.Kind;
    output.Cancellation = fields.Cancellation;
    output.VariablePayloadCount = fields.VariablePayloadCount;
    output.AttachmentCount = fields.AttachmentCount;
    output.Reserved = 0U;
    output.ReservedAlignment = 0U;

    for (Foundation::Size index{0U}; index < MessageEnvelope::MaximumCapabilityAttachments; ++index)
    {
        CapabilityAttachment& destination = output.Attachments[index];
        if (index >= attachmentCount)
        {
            ClearAttachment(destination);
            continue;
        }

        const Foundation::UInt8* const source = attachments + (index * WireCapabilityAttachmentSizeBytes);
        destination.Handle = Object::CapabilityHandle{ReadLittleEndian64(source + AttachmentSelectorOffset),
                                                      ReadLittleEndian64(source + AttachmentAuthenticatorOffset)};
        destination.RequestedDeadline = Object::MonotonicTime{ReadLittleEndian64(source + AttachmentDeadlineOffset)};
        static_cast<void>(Object::TryDecodeCapabilityRights(ReadLittleEndian16(source + AttachmentRightsOffset),
                                                            destination.RequestedRights));
        static_cast<void>(Object::TryDecodeObjectType(source[AttachmentObjectTypeOffset], destination.ExpectedType));
        destination.TransferMode = static_cast<CapabilityTransferMode>(source[AttachmentTransferModeOffset]);
        destination.Reserved = 0U;
    }

    for (Foundation::Size index{0U}; index < MessageEnvelope::MaximumVariablePayloads; ++index)
    {
        VariablePayloadDescriptor& destination = output.VariablePayloads[index];
        if (index >= variablePayloadCount)
        {
            destination = VariablePayloadDescriptor{};
            continue;
        }
        const Foundation::UInt8* const source = variablePayloads + (index * WireVariablePayloadDescriptorSizeBytes);
        destination.OffsetBytes = ReadLittleEndian16(source + VariableOffsetOffset);
        destination.SizeBytes = ReadLittleEndian16(source + VariableSizeOffset);
        destination.Reserved = 0U;
    }

    const Foundation::Size payloadOffset =
        attachmentsOffset + (static_cast<Foundation::Size>(attachmentCount) * WireCapabilityAttachmentSizeBytes);
    const Foundation::UInt8* const payload = input.Data + payloadOffset;
    for (Foundation::Size index{0U}; index < MessageEnvelope::MaximumPayloadBytes; ++index)
    {
        output.Payload[index] = index < payloadSize ? payload[index] : 0U;
    }
    return MessageCodecError::None;
}

Foundation::Result<Foundation::Size, MessageCodecError> MessageCodec::Encode(const MessageEnvelope& message,
                                                                             const WritableByteView output) noexcept
{
    const MessageCodecError validationError = ValidateMessage(message);
    if (validationError != MessageCodecError::None)
    {
        return Foundation::Result<Foundation::Size, MessageCodecError>::Failure(validationError);
    }
    if (output.Data == nullptr)
    {
        return Foundation::Result<Foundation::Size, MessageCodecError>::Failure(MessageCodecError::NullBuffer);
    }

    const Foundation::Size requiredSize =
        CalculateFrameSize(message.VariablePayloadCount, message.AttachmentCount, message.PayloadSizeBytes);
    if (output.SizeBytes < requiredSize)
    {
        return Foundation::Result<Foundation::Size, MessageCodecError>::Failure(MessageCodecError::OutputTooSmall);
    }
    if (RangesOverlap(output.Data, output.SizeBytes, &message, sizeof(message)))
    {
        return Foundation::Result<Foundation::Size, MessageCodecError>::Failure(MessageCodecError::BufferOverlap);
    }

    WriteLittleEndian32(output.Data + MagicOffset, WireMagic);
    WriteLittleEndian16(output.Data + WireVersionOffset, WireVersion);
    WriteLittleEndian16(output.Data + HeaderSizeOffset, static_cast<Foundation::UInt16>(WireHeaderSizeBytes));
    WriteLittleEndian32(output.Data + TotalSizeOffset, static_cast<Foundation::UInt32>(requiredSize));
    WriteLittleEndian16(output.Data + PayloadSizeOffset, message.PayloadSizeBytes);
    WriteLittleEndian16(output.Data + FixedPayloadSizeOffset, message.FixedPayloadSizeBytes);
    WriteLittleEndian64(output.Data + InterfaceLowOffset, message.Interface.GetLow());
    WriteLittleEndian64(output.Data + InterfaceHighOffset, message.Interface.GetHigh());
    WriteLittleEndian32(output.Data + MethodIdentifierOffset, message.MethodIdentifier);
    WriteLittleEndian16(output.Data + InterfaceVersionMajorOffset, message.InterfaceVersionMajor);
    WriteLittleEndian16(output.Data + InterfaceVersionMinorOffset, message.InterfaceVersionMinor);
    WriteLittleEndian64(output.Data + TransactionIdentifierOffset, message.TransactionIdentifier);
    WriteLittleEndian64(output.Data + DeadlineOffset, message.Deadline.GetTicks());
    WriteLittleEndian64(output.Data + TraceIdentifierHighOffset, message.Trace.TraceIdentifierHigh);
    WriteLittleEndian64(output.Data + TraceIdentifierLowOffset, message.Trace.TraceIdentifierLow);
    WriteLittleEndian64(output.Data + SpanIdentifierOffset, message.Trace.SpanIdentifier);
    output.Data[VariablePayloadCountOffset] = message.VariablePayloadCount;
    output.Data[AttachmentCountOffset] = message.AttachmentCount;
    output.Data[MessageKindOffset] = static_cast<Foundation::UInt8>(message.Kind);
    output.Data[CancellationStateOffset] = static_cast<Foundation::UInt8>(message.Cancellation);
    WriteLittleEndian32(output.Data + HeaderReservedOffset, 0U);

    Foundation::Size offset = WireHeaderSizeBytes;
    for (Foundation::Size index{0U}; index < message.VariablePayloadCount; ++index)
    {
        const VariablePayloadDescriptor& descriptor = message.VariablePayloads[index];
        WriteLittleEndian16(output.Data + offset + VariableOffsetOffset, descriptor.OffsetBytes);
        WriteLittleEndian16(output.Data + offset + VariableSizeOffset, descriptor.SizeBytes);
        WriteLittleEndian32(output.Data + offset + VariableReservedOffset, 0U);
        offset += WireVariablePayloadDescriptorSizeBytes;
    }
    for (Foundation::Size index{0U}; index < message.AttachmentCount; ++index)
    {
        const CapabilityAttachment& attachment = message.Attachments[index];
        WriteLittleEndian64(output.Data + offset + AttachmentSelectorOffset, attachment.Handle.GetSelector());
        WriteLittleEndian64(output.Data + offset + AttachmentAuthenticatorOffset, attachment.Handle.GetAuthenticator());
        WriteLittleEndian64(output.Data + offset + AttachmentDeadlineOffset, attachment.RequestedDeadline.GetTicks());
        WriteLittleEndian16(output.Data + offset + AttachmentRightsOffset, attachment.RequestedRights.GetRawValue());
        output.Data[offset + AttachmentTransferModeOffset] = static_cast<Foundation::UInt8>(attachment.TransferMode);
        output.Data[offset + AttachmentObjectTypeOffset] = Object::EncodeObjectType(attachment.ExpectedType);
        WriteLittleEndian32(output.Data + offset + AttachmentReservedOffset, 0U);
        offset += WireCapabilityAttachmentSizeBytes;
    }
    for (Foundation::Size index{0U}; index < message.PayloadSizeBytes; ++index)
    {
        output.Data[offset + index] = message.Payload[index];
    }

    return Foundation::Result<Foundation::Size, MessageCodecError>::Success(requiredSize);
}
} // namespace Asteria::Kernel::Ipc
