// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/IpcDeliveryCodec.hpp"

#include "Kernel/Object/CapabilityWireTypes.hpp"

namespace Asteria::Kernel::Ipc
{
namespace
{
constexpr Foundation::Size MagicOffset{0U};
constexpr Foundation::Size WireVersionOffset{4U};
constexpr Foundation::Size HeaderSizeOffset{6U};
constexpr Foundation::Size TotalSizeOffset{8U};
constexpr Foundation::Size FrameSizeOffset{12U};
constexpr Foundation::Size EffectiveRightsOffset{14U};
constexpr Foundation::Size SequenceOffset{16U};
constexpr Foundation::Size SenderOffset{24U};
constexpr Foundation::Size SealedObjectSelectorOffset{32U};
constexpr Foundation::Size AttachmentCountOffset{40U};
constexpr Foundation::Size SendClassOffset{41U};
constexpr Foundation::Size ReservedOffset{42U};
constexpr Foundation::Size ReservedSizeBytes{6U};
constexpr Foundation::Size MessageAttachmentCountOffset{81U};

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

[[nodiscard]] constexpr IpcDeliveryCodecFailure
Failure(const IpcDeliveryCodecError error, const MessageCodecError frame = MessageCodecError::None) noexcept
{
    return IpcDeliveryCodecFailure{
        .Frame = frame,
        .Error = error,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

[[nodiscard]] constexpr bool IsKnownSendClass(const EndpointSendClass sendClass) noexcept
{
    switch (sendClass)
    {
    case EndpointSendClass::Normal:
    case EndpointSendClass::ReservedControl:
        return true;

    case EndpointSendClass::Invalid:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr Foundation::Size CalculateFrameSize(const MessageEnvelope& message) noexcept
{
    return MessageCodec::WireHeaderSizeBytes +
           (static_cast<Foundation::Size>(message.VariablePayloadCount) *
            MessageCodec::WireVariablePayloadDescriptorSizeBytes) +
           (static_cast<Foundation::Size>(message.AttachmentCount) * MessageCodec::WireCapabilityAttachmentSizeBytes) +
           message.PayloadSizeBytes;
}

[[nodiscard]] constexpr IpcDeliveryCodecError ValidateMetadata(const EndpointReceiveMetadata& metadata,
                                                               const MessageEnvelope& message) noexcept
{
    if (metadata.Sequence == 0U)
    {
        return IpcDeliveryCodecError::InvalidSequence;
    }
    if (!metadata.Sender.IsValid())
    {
        return IpcDeliveryCodecError::InvalidSender;
    }
    if (metadata.AttachmentCount > MessageEnvelope::MaximumCapabilityAttachments)
    {
        return IpcDeliveryCodecError::TooManyAttachments;
    }
    if (!IsKnownSendClass(metadata.SendClass))
    {
        return IpcDeliveryCodecError::InvalidSendClass;
    }
    if (!metadata.SenderEndpointRights.IsValid())
    {
        return IpcDeliveryCodecError::InvalidEffectiveRights;
    }
    if (metadata.AttachmentCount != message.AttachmentCount)
    {
        return IpcDeliveryCodecError::MetadataMismatch;
    }

    const Foundation::Size frameSize = CalculateFrameSize(message);
    if ((frameSize < MessageCodec::WireHeaderSizeBytes) || (frameSize > MessageCodec::MaximumWireSizeBytes) ||
        (metadata.FrameSizeBytes != frameSize))
    {
        return IpcDeliveryCodecError::InvalidFrameSize;
    }
    return IpcDeliveryCodecError::None;
}
} // namespace

Foundation::Result<Foundation::Size, IpcDeliveryCodecFailure>
IpcDeliveryCodec::Encode(const EndpointReceiveMetadata& metadata, const MessageEnvelope& message,
                         const WritableByteView output) noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, IpcDeliveryCodecFailure>;

    const IpcDeliveryCodecError metadataError = ValidateMetadata(metadata, message);
    if (metadataError != IpcDeliveryCodecError::None)
    {
        return ResultType::Failure(Failure(metadataError));
    }
    if (output.Data == nullptr)
    {
        return ResultType::Failure(Failure(IpcDeliveryCodecError::NullBuffer));
    }

    const Foundation::Size frameSize = CalculateFrameSize(message);
    const Foundation::Size totalSize = WireHeaderSizeBytes + frameSize;
    if (output.SizeBytes < totalSize)
    {
        return ResultType::Failure(Failure(IpcDeliveryCodecError::OutputTooSmall));
    }
    if (RangesOverlap(output.Data, output.SizeBytes, &metadata, sizeof(metadata)) ||
        RangesOverlap(output.Data, output.SizeBytes, &message, sizeof(message)))
    {
        return ResultType::Failure(Failure(IpcDeliveryCodecError::BufferOverlap));
    }

    const auto frameResult = MessageCodec::Encode(message, WritableByteView{
                                                               .Data = output.Data + WireHeaderSizeBytes,
                                                               .SizeBytes = output.SizeBytes - WireHeaderSizeBytes,
                                                           });
    if (!frameResult.HasValue())
    {
        return ResultType::Failure(Failure(IpcDeliveryCodecError::FrameRejected, frameResult.GetError()));
    }

    WriteLittleEndian32(output.Data + MagicOffset, WireMagic);
    WriteLittleEndian16(output.Data + WireVersionOffset, WireVersion);
    WriteLittleEndian16(output.Data + HeaderSizeOffset, static_cast<Foundation::UInt16>(WireHeaderSizeBytes));
    WriteLittleEndian32(output.Data + TotalSizeOffset, static_cast<Foundation::UInt32>(totalSize));
    WriteLittleEndian16(output.Data + FrameSizeOffset, metadata.FrameSizeBytes);
    WriteLittleEndian16(output.Data + EffectiveRightsOffset, metadata.SenderEndpointRights.GetRawValue());
    WriteLittleEndian64(output.Data + SequenceOffset, metadata.Sequence);
    WriteLittleEndian64(output.Data + SenderOffset, metadata.Sender.GetValue());
    WriteLittleEndian64(output.Data + SealedObjectSelectorOffset, metadata.SenderEndpointSealedObjectSelector);
    output.Data[AttachmentCountOffset] = metadata.AttachmentCount;
    output.Data[SendClassOffset] = static_cast<Foundation::UInt8>(metadata.SendClass);
    for (Foundation::Size index{0U}; index < ReservedSizeBytes; ++index)
    {
        output.Data[ReservedOffset + index] = 0U;
    }

    return ResultType::Success(totalSize);
}

IpcDeliveryCodecFailure IpcDeliveryCodec::Decode(const ReadOnlyByteView input, EndpointReceiveMetadata& metadata,
                                                 MessageEnvelope& message) noexcept
{
    if (input.Data == nullptr)
    {
        return input.SizeBytes == 0U ? Failure(IpcDeliveryCodecError::HeaderTooSmall)
                                     : Failure(IpcDeliveryCodecError::NullBuffer);
    }
    if (input.SizeBytes < WireHeaderSizeBytes)
    {
        return Failure(IpcDeliveryCodecError::HeaderTooSmall);
    }
    if (input.SizeBytes > MaximumWireSizeBytes)
    {
        return Failure(IpcDeliveryCodecError::DeliveryTooLarge);
    }
    if (RangesOverlap(input.Data, input.SizeBytes, &metadata, sizeof(metadata)) ||
        RangesOverlap(input.Data, input.SizeBytes, &message, sizeof(message)) ||
        RangesOverlap(&metadata, sizeof(metadata), &message, sizeof(message)))
    {
        return Failure(IpcDeliveryCodecError::BufferOverlap);
    }

    if (ReadLittleEndian32(input.Data + MagicOffset) != WireMagic)
    {
        return Failure(IpcDeliveryCodecError::InvalidMagic);
    }
    if (ReadLittleEndian16(input.Data + WireVersionOffset) != WireVersion)
    {
        return Failure(IpcDeliveryCodecError::UnsupportedWireVersion);
    }
    if (ReadLittleEndian16(input.Data + HeaderSizeOffset) != WireHeaderSizeBytes)
    {
        return Failure(IpcDeliveryCodecError::InvalidHeaderSize);
    }

    const Foundation::UInt32 totalSize = ReadLittleEndian32(input.Data + TotalSizeOffset);
    if (totalSize != input.SizeBytes)
    {
        return Failure(IpcDeliveryCodecError::SizeMismatch);
    }
    const Foundation::UInt16 frameSize = ReadLittleEndian16(input.Data + FrameSizeOffset);
    if ((frameSize < MessageCodec::WireHeaderSizeBytes) || (frameSize > MessageCodec::MaximumWireSizeBytes))
    {
        return Failure(IpcDeliveryCodecError::InvalidFrameSize);
    }
    if (totalSize != (WireHeaderSizeBytes + frameSize))
    {
        return Failure(IpcDeliveryCodecError::SizeMismatch);
    }
    for (Foundation::Size index{0U}; index < ReservedSizeBytes; ++index)
    {
        if (input.Data[ReservedOffset + index] != 0U)
        {
            return Failure(IpcDeliveryCodecError::ReservedFieldNonzero);
        }
    }

    const Foundation::UInt64 sequence = ReadLittleEndian64(input.Data + SequenceOffset);
    if (sequence == 0U)
    {
        return Failure(IpcDeliveryCodecError::InvalidSequence);
    }
    const Foundation::UInt64 sender = ReadLittleEndian64(input.Data + SenderOffset);
    if (sender == 0U)
    {
        return Failure(IpcDeliveryCodecError::InvalidSender);
    }
    const Foundation::UInt8 attachmentCount = input.Data[AttachmentCountOffset];
    if (attachmentCount > MessageEnvelope::MaximumCapabilityAttachments)
    {
        return Failure(IpcDeliveryCodecError::TooManyAttachments);
    }
    const EndpointSendClass sendClass = static_cast<EndpointSendClass>(input.Data[SendClassOffset]);
    if (!IsKnownSendClass(sendClass))
    {
        return Failure(IpcDeliveryCodecError::InvalidSendClass);
    }

    Object::CapabilityRights effectiveRights{};
    if (!Object::TryDecodeCapabilityRights(ReadLittleEndian16(input.Data + EffectiveRightsOffset), effectiveRights))
    {
        return Failure(IpcDeliveryCodecError::InvalidEffectiveRights);
    }
    if (input.Data[WireHeaderSizeBytes + MessageAttachmentCountOffset] != attachmentCount)
    {
        return Failure(IpcDeliveryCodecError::MetadataMismatch);
    }

    const MessageCodecError frameError = MessageCodec::Decode(
        ReadOnlyByteView{
            .Data = input.Data + WireHeaderSizeBytes,
            .SizeBytes = frameSize,
        },
        message);
    if (frameError != MessageCodecError::None)
    {
        return Failure(IpcDeliveryCodecError::FrameRejected, frameError);
    }

    metadata = EndpointReceiveMetadata{
        .Sequence = sequence,
        .Sender = Object::ResourceDomainId{sender},
        .SenderEndpointSealedObjectSelector = ReadLittleEndian64(input.Data + SealedObjectSelectorOffset),
        .SenderEndpointRights = effectiveRights,
        .FrameSizeBytes = frameSize,
        .AttachmentCount = attachmentCount,
        .SendClass = sendClass,
    };
    return Failure(IpcDeliveryCodecError::None);
}
} // namespace Asteria::Kernel::Ipc
