// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "Foundation/Types.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::EndpointReceiveMetadata;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::IpcDeliveryCodec;
using Kernel::Ipc::IpcDeliveryCodecError;
using Kernel::Ipc::IpcDeliveryCodecFailure;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Ipc::WritableByteView;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::IUserMemoryAccess;
using Kernel::Memory::ReadOnlyKernelBuffer;
using Kernel::Memory::UserMemoryAccessError;
using Kernel::Memory::UserMemoryRange;
using Kernel::Memory::UserMemoryRangeError;
using Kernel::Memory::UserMemoryWriteReservation;
using Kernel::Memory::WritableKernelBuffer;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Platform::Synchronization::SpinLock;

constexpr Foundation::Size MagicOffset{0U};
constexpr Foundation::Size VersionOffset{4U};
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
constexpr Foundation::Size MessageMagicOffset{IpcDeliveryCodec::WireHeaderSizeBytes};
constexpr Foundation::Size MessageAttachmentCountOffset{IpcDeliveryCodec::WireHeaderSizeBytes + 81U};

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

void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 2U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void FillBytes(Foundation::UInt8* const bytes, const Foundation::Size sizeBytes, const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = value;
    }
}

[[nodiscard]] bool BytesMatch(const void* const first, const void* const second,
                              const Foundation::Size sizeBytes) noexcept
{
    return std::memcmp(first, second, sizeBytes) == 0;
}

[[nodiscard]] constexpr CapabilityRights EffectiveRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read);
}

[[nodiscard]] MessageEnvelope MakeMessage() noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = 0x8877'6655'4433'2211ULL;
    message.Deadline = MonotonicTime::Infinite();
    message.MethodIdentifier = 0x1020'3040U;
    message.InterfaceVersionMajor = 1U;
    message.InterfaceVersionMinor = 2U;
    message.FixedPayloadSizeBytes = 4U;
    message.PayloadSizeBytes = 4U;
    message.Kind = MessageKind::SynchronousCall;
    message.AttachmentCount = 1U;
    message.Attachments[0] = CapabilityAttachment{
        .Handle = CapabilityHandle{0x0101U, 0x1020'3040'5060'7080ULL},
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Invoke},
        .ExpectedType = ObjectType::Endpoint,
        .TransferMode = CapabilityTransferMode::Copy,
        .Reserved = 0U,
    };
    message.Payload[0] = 0x10U;
    message.Payload[1] = 0x20U;
    message.Payload[2] = 0x30U;
    message.Payload[3] = 0x40U;
    return message;
}

[[nodiscard]] EndpointReceiveMetadata MakeMetadata() noexcept
{
    return EndpointReceiveMetadata{
        .Sequence = 0x0102'0304'0506'0708ULL,
        .Sender = ResourceDomainId{0x1112'1314'1516'1718ULL},
        .SenderEndpointSealedObjectSelector = 0x2122'2324'2526'2728ULL,
        .SenderEndpointRights = EffectiveRights(),
        .FrameSizeBytes = 124U,
        .AttachmentCount = 1U,
        .SendClass = EndpointSendClass::Normal,
    };
}

[[nodiscard]] bool MetadataMatches(const EndpointReceiveMetadata& first, const EndpointReceiveMetadata& second) noexcept
{
    return (first.Sequence == second.Sequence) && (first.Sender.GetValue() == second.Sender.GetValue()) &&
           (first.SenderEndpointSealedObjectSelector == second.SenderEndpointSealedObjectSelector) &&
           first.SenderEndpointRights.IsEqualTo(second.SenderEndpointRights) &&
           (first.FrameSizeBytes == second.FrameSizeBytes) && (first.AttachmentCount == second.AttachmentCount) &&
           (first.SendClass == second.SendClass);
}

void CopyBytes(Foundation::UInt8* const destination, const Foundation::UInt8* const source,
               const Foundation::Size sizeBytes) noexcept
{
    static_cast<void>(std::memcpy(destination, source, sizeBytes));
}

void CheckDecodeFailure(TestContext& runner, const Foundation::UInt8* const wire, const Foundation::Size sizeBytes,
                        const IpcDeliveryCodecError expected, const char* const description,
                        const MessageCodecError frameError = MessageCodecError::None) noexcept
{
    EndpointReceiveMetadata metadata = MakeMetadata();
    metadata.Sequence ^= 0xFFFFU;
    MessageEnvelope message = MakeMessage();
    message.MethodIdentifier ^= 0xFFFF'0000U;
    const EndpointReceiveMetadata metadataBefore = metadata;
    const MessageEnvelope messageBefore = message;
    const IpcDeliveryCodecFailure failure =
        IpcDeliveryCodec::Decode(ReadOnlyByteView{.Data = wire, .SizeBytes = sizeBytes}, metadata, message);
    ASTERIA_EXPECT(runner, description, failure.Error == expected, failure.Frame == frameError,
                   MetadataMatches(metadata, metadataBefore), BytesMatch(&message, &messageBefore, sizeof(message)));
}

void CheckEncodeFailure(TestContext& runner, const EndpointReceiveMetadata& metadata, const MessageEnvelope& message,
                        const IpcDeliveryCodecError expected, const char* const description,
                        const MessageCodecError frameError = MessageCodecError::None) noexcept
{
    Foundation::UInt8 output[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    Foundation::UInt8 before[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    FillBytes(output, sizeof(output), 0x5AU);
    CopyBytes(before, output, sizeof(output));
    const auto result =
        IpcDeliveryCodec::Encode(metadata, message, WritableByteView{.Data = output, .SizeBytes = sizeof(output)});
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError().Error == expected,
                   result.GetError().Frame == frameError, BytesMatch(output, before, sizeof(output)));
}

class TestUserMemoryAccess final : public IUserMemoryAccess
{
public:
    explicit TestUserMemoryAccess(const AddressSpaceId addressSpace) noexcept : m_addressSpace{addressSpace}
    {
    }

    [[nodiscard]] AddressSpaceId GetAddressSpaceId() const noexcept override
    {
        return m_addressSpace;
    }

    [[nodiscard]] bool IsReady() const noexcept override
    {
        return m_isReady;
    }

    void SetReady(const bool ready) noexcept
    {
        m_isReady = ready;
    }

    void FailNextCopyFrom() noexcept
    {
        m_failNextCopyFrom = true;
    }

    void FailNextPrepare() noexcept
    {
        m_failNextPrepare = true;
    }

    void FailNextPrepareWithNoError() noexcept
    {
        m_failNextPrepareWithNoError = true;
    }

    void SetUserByte(const Foundation::Size index, const Foundation::UInt8 value) noexcept
    {
        m_userBytes[index] = value;
    }

    [[nodiscard]] Foundation::UInt8 GetUserByte(const Foundation::Size index) const noexcept
    {
        return m_userBytes[index];
    }

protected:
    [[nodiscard]] UserMemoryAccessError CopyFromUserValidated(const UserMemoryRange source,
                                                              const WritableKernelBuffer destination) noexcept override
    {
        if (m_failNextCopyFrom)
        {
            m_failNextCopyFrom = false;
            return UserMemoryAccessError::Fault;
        }
        for (Foundation::Size index{0U}; index < source.GetSizeBytes(); ++index)
        {
            destination.Data[index] = m_userBytes[index];
        }
        return UserMemoryAccessError::None;
    }

    [[nodiscard]] Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
    PrepareCopyToUserValidated(const UserMemoryRange destination) noexcept override
    {
        using ResultType = Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>;

        if (m_failNextPrepare)
        {
            m_failNextPrepare = false;
            return ResultType::Failure(UserMemoryAccessError::PinFailed);
        }
        if (m_failNextPrepareWithNoError)
        {
            m_failNextPrepareWithNoError = false;
            return ResultType::Failure(UserMemoryAccessError::None);
        }
        if (m_hasActiveReservation)
        {
            return ResultType::Failure(UserMemoryAccessError::ReservationExhausted);
        }
        m_hasActiveReservation = true;
        m_activeRange = destination;
        m_activeGeneration = m_nextGeneration;
        ++m_nextGeneration;
        return ResultType::Success(MakeWriteReservation(destination, m_activeGeneration));
    }

    [[nodiscard]] UserMemoryAccessError
    CancelCopyToUserValidated(const UserMemoryWriteReservation reservation) noexcept override
    {
        if (!IsCurrent(reservation))
        {
            return UserMemoryAccessError::StaleReservation;
        }
        ClearActiveReservation();
        return UserMemoryAccessError::None;
    }

    [[nodiscard]] UserMemoryAccessError CommitCopyToUserValidated(const UserMemoryWriteReservation reservation,
                                                                  const ReadOnlyKernelBuffer source) noexcept override
    {
        if (!IsCurrent(reservation))
        {
            return UserMemoryAccessError::StaleReservation;
        }
        for (Foundation::Size index{0U}; index < source.SizeBytes; ++index)
        {
            m_userBytes[index] = source.Data[index];
        }
        ClearActiveReservation();
        return UserMemoryAccessError::None;
    }

private:
    [[nodiscard]] bool IsCurrent(const UserMemoryWriteReservation reservation) const noexcept
    {
        return m_hasActiveReservation && (reservation.GetGeneration() == m_activeGeneration) &&
               reservation.GetRange().IsEqualTo(m_activeRange);
    }

    void ClearActiveReservation() noexcept
    {
        m_hasActiveReservation = false;
        m_activeRange = UserMemoryRange{};
        m_activeGeneration = 0U;
    }

    AddressSpaceId m_addressSpace{};
    bool m_isReady{true};
    bool m_failNextCopyFrom{false};
    bool m_failNextPrepare{false};
    bool m_failNextPrepareWithNoError{false};
    bool m_hasActiveReservation{false};
    UserMemoryRange m_activeRange{};
    Foundation::UInt64 m_nextGeneration{1U};
    Foundation::UInt64 m_activeGeneration{0U};
    Foundation::UInt8 m_userBytes[UserMemoryRange::MaximumSizeBytes]{};
};

void TestExactDeliveryEncoding(TestContext& runner) noexcept
{
    const MessageEnvelope message = MakeMessage();
    const EndpointReceiveMetadata metadata = MakeMetadata();
    Foundation::UInt8 first[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    Foundation::UInt8 second[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    FillBytes(first, sizeof(first), 0xA5U);
    FillBytes(second, sizeof(second), 0x5AU);
    const auto firstResult =
        IpcDeliveryCodec::Encode(metadata, message, WritableByteView{.Data = first, .SizeBytes = sizeof(first)});
    const auto secondResult =
        IpcDeliveryCodec::Encode(metadata, message, WritableByteView{.Data = second, .SizeBytes = sizeof(second)});
    constexpr Foundation::Size expectedSizeBytes{172U};
    ASTERIA_EXPECT(runner, "canonical delivery has the exact fixed-header plus frame size", firstResult.HasValue(),
                   secondResult.HasValue(), firstResult.GetValue() == expectedSizeBytes,
                   secondResult.GetValue() == expectedSizeBytes);
    ASTERIA_EXPECT(runner, "delivery framing prefix is explicit little-endian AIPD version 1", first[0] == 'A',
                   first[1] == 'I', first[2] == 'P', first[3] == 'D',
                   ReadLittleEndian16(first + VersionOffset) == IpcDeliveryCodec::WireVersion,
                   ReadLittleEndian16(first + HeaderSizeOffset) == IpcDeliveryCodec::WireHeaderSizeBytes,
                   ReadLittleEndian32(first + TotalSizeOffset) == expectedSizeBytes,
                   ReadLittleEndian16(first + FrameSizeOffset) == 124U);
    ASTERIA_EXPECT(runner, "trusted delivery metadata occupies every documented golden offset",
                   ReadLittleEndian16(first + EffectiveRightsOffset) == EffectiveRights().GetRawValue(),
                   ReadLittleEndian64(first + SequenceOffset) == metadata.Sequence,
                   ReadLittleEndian64(first + SenderOffset) == metadata.Sender.GetValue(),
                   ReadLittleEndian64(first + SealedObjectSelectorOffset) ==
                       metadata.SenderEndpointSealedObjectSelector,
                   first[AttachmentCountOffset] == metadata.AttachmentCount,
                   first[SendClassOffset] == static_cast<Foundation::UInt8>(metadata.SendClass));
    bool reservedIsZero{true};
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        reservedIsZero = reservedIsZero && (first[ReservedOffset + index] == 0U);
    }
    ASTERIA_EXPECT(runner, "reserved delivery bytes are zero before the canonical MessageCodec frame", reservedIsZero,
                   first[MessageMagicOffset] == 'A', first[MessageMagicOffset + 1U] == 'I',
                   first[MessageMagicOffset + 2U] == 'P', first[MessageMagicOffset + 3U] == 'C');
    ASTERIA_EXPECT(runner, "delivery encoding is byte-for-byte deterministic",
                   BytesMatch(first, second, expectedSizeBytes));

    bool tailPreserved{true};
    for (Foundation::Size index{expectedSizeBytes}; index < sizeof(first); ++index)
    {
        tailPreserved = tailPreserved && (first[index] == 0xA5U);
    }
    ASTERIA_EXPECT(runner, "successful encoding leaves unused output capacity unchanged", tailPreserved);

    EndpointReceiveMetadata decodedMetadata{};
    MessageEnvelope decodedMessage{};
    const IpcDeliveryCodecFailure decoded = IpcDeliveryCodec::Decode(
        ReadOnlyByteView{.Data = first, .SizeBytes = expectedSizeBytes}, decodedMetadata, decodedMessage);
    ASTERIA_EXPECT(runner, "canonical delivery round-trips without a structure overlay",
                   decoded.Error == IpcDeliveryCodecError::None, decoded.Frame == MessageCodecError::None,
                   MetadataMatches(decodedMetadata, metadata), BytesMatch(&decodedMessage, &message, sizeof(message)));
}

void TestDeliveryShapeAndOverlap(TestContext& runner) noexcept
{
    MessageEnvelope message = MakeMessage();
    const EndpointReceiveMetadata metadata = MakeMetadata();
    Foundation::UInt8 wire[IpcDeliveryCodec::MaximumWireSizeBytes + 1U]{};
    const auto encoded =
        IpcDeliveryCodec::Encode(metadata, message, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    CheckDecodeFailure(runner, nullptr, 1U, IpcDeliveryCodecError::NullBuffer,
                       "nonempty null delivery input is rejected atomically");
    CheckDecodeFailure(runner, nullptr, 0U, IpcDeliveryCodecError::HeaderTooSmall,
                       "canonical empty delivery input is a short header");
    CheckDecodeFailure(runner, wire, IpcDeliveryCodec::WireHeaderSizeBytes - 1U, IpcDeliveryCodecError::HeaderTooSmall,
                       "truncated delivery header is rejected atomically");
    CheckDecodeFailure(runner, wire, IpcDeliveryCodec::MaximumWireSizeBytes + 1U,
                       IpcDeliveryCodecError::DeliveryTooLarge,
                       "delivery above the fixed maximum is rejected before parsing");

    EndpointReceiveMetadata overlapMetadata = metadata;
    MessageEnvelope overlapMessage{};
    const IpcDeliveryCodecFailure decodeOverlap = IpcDeliveryCodec::Decode(
        ReadOnlyByteView{
            .Data = reinterpret_cast<const Foundation::UInt8*>(&overlapMetadata),
            .SizeBytes = IpcDeliveryCodec::WireHeaderSizeBytes,
        },
        overlapMetadata, overlapMessage);
    ASTERIA_EXPECT(runner, "decode rejects overlap with either destination before parsing",
                   decodeOverlap.Error == IpcDeliveryCodecError::BufferOverlap);

    const auto encodeOverlap = IpcDeliveryCodec::Encode(metadata, message,
                                                        WritableByteView{
                                                            .Data = reinterpret_cast<Foundation::UInt8*>(&message),
                                                            .SizeBytes = IpcDeliveryCodec::MaximumWireSizeBytes,
                                                        });
    ASTERIA_EXPECT(runner, "encode rejects overlap with either stable input object", !encodeOverlap.HasValue(),
                   encodeOverlap.GetError().Error == IpcDeliveryCodecError::BufferOverlap);

    Foundation::UInt8 tooSmall[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    Foundation::UInt8 before[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    FillBytes(tooSmall, sizeof(tooSmall), 0xC3U);
    CopyBytes(before, tooSmall, sizeof(before));
    const auto shortEncode = IpcDeliveryCodec::Encode(metadata, message,
                                                      WritableByteView{
                                                          .Data = tooSmall,
                                                          .SizeBytes = encoded.GetValue() - 1U,
                                                      });
    ASTERIA_EXPECT(runner, "short encode output is rejected without modifying any byte", !shortEncode.HasValue(),
                   shortEncode.GetError().Error == IpcDeliveryCodecError::OutputTooSmall,
                   BytesMatch(tooSmall, before, sizeof(tooSmall)));
}

void TestDeliveryCanonicalityAndForgery(TestContext& runner) noexcept
{
    const MessageEnvelope message = MakeMessage();
    const EndpointReceiveMetadata metadata = MakeMetadata();
    Foundation::UInt8 wire[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    const auto encoded =
        IpcDeliveryCodec::Encode(metadata, message, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    Foundation::UInt8 mutated[IpcDeliveryCodec::MaximumWireSizeBytes]{};

    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[MagicOffset] ^= 1U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::InvalidMagic,
                       "forged delivery magic is rejected atomically");
    CopyBytes(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + VersionOffset, 2U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::UnsupportedWireVersion,
                       "forged delivery version is rejected atomically");
    CopyBytes(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + HeaderSizeOffset, 40U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::InvalidHeaderSize,
                       "noncanonical delivery header size is rejected");
    CopyBytes(mutated, wire, encoded.GetValue());
    WriteLittleEndian32(mutated + TotalSizeOffset, static_cast<Foundation::UInt32>(encoded.GetValue() + 1U));
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::SizeMismatch,
                       "declared delivery extent must equal the supplied extent");
    CopyBytes(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + FrameSizeOffset, 87U);
    WriteLittleEndian32(mutated + TotalSizeOffset, 135U);
    CheckDecodeFailure(runner, mutated, 135U, IpcDeliveryCodecError::InvalidFrameSize,
                       "outer frame extent cannot be smaller than a MessageCodec header");
    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[ReservedOffset + 5U] = 1U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::ReservedFieldNonzero,
                       "every reserved delivery byte must be zero");

    CopyBytes(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + SequenceOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::InvalidSequence,
                       "forged zero receive sequence is rejected");
    CopyBytes(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + SenderOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::InvalidSender,
                       "forged zero sender domain is rejected");
    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[AttachmentCountOffset] = 5U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::TooManyAttachments,
                       "forged outer attachment count is bounded");
    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[AttachmentCountOffset] = 0U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::MetadataMismatch,
                       "outer attachment metadata cannot forge the canonical frame count");
    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[MessageAttachmentCountOffset] = 0U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::MetadataMismatch,
                       "inner frame count cannot forge trusted delivery metadata");
    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[SendClassOffset] = 0U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::InvalidSendClass,
                       "forged send class is rejected");
    CopyBytes(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + EffectiveRightsOffset, 0x8000U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::InvalidEffectiveRights,
                       "unknown forged effective-rights bits are rejected");
    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[MessageMagicOffset] ^= 1U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::FrameRejected,
                       "invalid nested frame is reported without changing either output",
                       MessageCodecError::InvalidMagic);
    CopyBytes(mutated, wire, encoded.GetValue());
    mutated[MessageMagicOffset + 84U] = 1U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), IpcDeliveryCodecError::FrameRejected,
                       "nonzero nested reserved field remains noncanonical", MessageCodecError::ReservedFieldNonzero);
}

void TestDeliveryEncodeFailures(TestContext& runner) noexcept
{
    const MessageEnvelope message = MakeMessage();
    const EndpointReceiveMetadata metadata = MakeMetadata();

    EndpointReceiveMetadata invalid = metadata;
    invalid.Sequence = 0U;
    CheckEncodeFailure(runner, invalid, message, IpcDeliveryCodecError::InvalidSequence,
                       "encode rejects a zero trusted receive sequence atomically");
    invalid = metadata;
    invalid.Sender = ResourceDomainId{};
    CheckEncodeFailure(runner, invalid, message, IpcDeliveryCodecError::InvalidSender,
                       "encode rejects an invalid trusted sender atomically");
    invalid = metadata;
    invalid.FrameSizeBytes = 123U;
    CheckEncodeFailure(runner, invalid, message, IpcDeliveryCodecError::InvalidFrameSize,
                       "encode requires exact canonical MessageCodec frame size metadata");
    invalid = metadata;
    invalid.AttachmentCount = 0U;
    CheckEncodeFailure(runner, invalid, message, IpcDeliveryCodecError::MetadataMismatch,
                       "encode requires trusted and message attachment counts to match");
    invalid = metadata;
    invalid.SendClass = EndpointSendClass::Invalid;
    CheckEncodeFailure(runner, invalid, message, IpcDeliveryCodecError::InvalidSendClass,
                       "encode rejects an invalid trusted send class atomically");
    MessageEnvelope invalidMessage = message;
    invalidMessage.MethodIdentifier = 0U;
    CheckEncodeFailure(runner, metadata, invalidMessage, IpcDeliveryCodecError::FrameRejected,
                       "nested MessageCodec validation failure preserves delivery output",
                       MessageCodecError::InvalidMethodIdentifier);
}

void TestUserMemoryRanges(TestContext& runner) noexcept
{
    const auto empty = UserMemoryRange::Create(0U, 0U);
    const auto valid = UserMemoryRange::Create(0x1000U, 16U);
    const auto null = UserMemoryRange::Create(0U, 1U);
    const auto noncanonicalEmpty = UserMemoryRange::Create(0x1000U, 0U);
    const auto tooLarge = UserMemoryRange::Create(0x1000U, UserMemoryRange::MaximumSizeBytes + 1U);
    const auto overflow = UserMemoryRange::Create(~Foundation::UInt64{0U} - 1U, 2U);
    ASTERIA_EXPECT(runner, "user range accepts only the canonical empty null interval", empty.HasValue(),
                   empty.GetValue().IsEmpty(), empty.GetValue().IsValid(), empty.GetValue().GetEndAddress() == 0U);
    ASTERIA_EXPECT(runner, "user range preserves an exact bounded 64-bit interval", valid.HasValue(),
                   valid.GetValue().IsValid(), valid.GetValue().GetStartAddress() == 0x1000U,
                   valid.GetValue().GetEndAddress() == 0x1010U, valid.GetValue().GetSizeBytes() == 16U);
    ASTERIA_EXPECT(runner, "null and zero-size user range shapes are distinguished", !null.HasValue(),
                   null.GetError() == UserMemoryRangeError::NullAddress, !noncanonicalEmpty.HasValue(),
                   noncanonicalEmpty.GetError() == UserMemoryRangeError::NonCanonicalEmptyRange);
    ASTERIA_EXPECT(runner, "oversized and overflowing user ranges are rejected before access", !tooLarge.HasValue(),
                   tooLarge.GetError() == UserMemoryRangeError::RangeTooLarge, !overflow.HasValue(),
                   overflow.GetError() == UserMemoryRangeError::AddressOverflow);
}

void TestUserMemoryAccessContract(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    const AddressSpaceId addressSpace{registry, ObjectId{9U}};
    TestUserMemoryAccess access{addressSpace};
    TestUserMemoryAccess foreign{addressSpace};
    const UserMemoryRange range = UserMemoryRange::Create(0x4000U, 4U).GetValue();
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        access.SetUserByte(index, static_cast<Foundation::UInt8>(0x20U + index));
    }

    ASTERIA_EXPECT(runner, "user-memory access exposes its exact ready AddressSpace generation", access.IsReady(),
                   access.GetAddressSpaceId().IsEqualTo(addressSpace));
    Foundation::UInt8 destination[4U]{0xA5U, 0xA5U, 0xA5U, 0xA5U};
    access.FailNextCopyFrom();
    ASTERIA_EXPECT(runner, "fault-contained copy-in leaves the full kernel destination unchanged",
                   access.CopyFromUser(range, WritableKernelBuffer{.Data = destination, .SizeBytes = 4U}) ==
                       UserMemoryAccessError::Fault,
                   destination[0] == 0xA5U, destination[1] == 0xA5U, destination[2] == 0xA5U, destination[3] == 0xA5U);
    ASTERIA_EXPECT(runner, "successful bounded copy-in publishes the exact bytes",
                   access.CopyFromUser(range, WritableKernelBuffer{.Data = destination, .SizeBytes = 4U}) ==
                       UserMemoryAccessError::None,
                   destination[0] == 0x20U, destination[3] == 0x23U);
    ASTERIA_EXPECT(runner, "copy-in rejects a null nonempty kernel destination",
                   access.CopyFromUser(range, WritableKernelBuffer{.Data = nullptr, .SizeBytes = 4U}) ==
                       UserMemoryAccessError::NullKernelBuffer);
    ASTERIA_EXPECT(runner, "copy-in requires an exact destination extent",
                   access.CopyFromUser(range, WritableKernelBuffer{.Data = destination, .SizeBytes = 3U}) ==
                       UserMemoryAccessError::SizeMismatch);
    ASTERIA_EXPECT(
        runner, "kernel-buffer address overflow is rejected before native access",
        access.CopyFromUser(range, WritableKernelBuffer{
                                       .Data = reinterpret_cast<Foundation::UInt8*>(~Foundation::UIntPointer{0U} - 1U),
                                       .SizeBytes = 4U,
                                   }) == UserMemoryAccessError::KernelBufferOverflow);

    access.SetReady(false);
    ASTERIA_EXPECT(runner, "unready user-memory access rejects copy-in",
                   access.CopyFromUser(range, WritableKernelBuffer{.Data = destination, .SizeBytes = 4U}) ==
                       UserMemoryAccessError::NotReady);
    access.SetReady(true);
    access.FailNextPrepare();
    const auto failedPrepare = access.PrepareCopyToUser(range);
    ASTERIA_EXPECT(runner, "fallible copy-out preparation publishes no user byte", !failedPrepare.HasValue(),
                   failedPrepare.GetError() == UserMemoryAccessError::PinFailed, access.GetUserByte(0U) == 0x20U);

    access.FailNextPrepareWithNoError();
    const auto malformedFailurePrepare = access.PrepareCopyToUser(range);
    const auto prepareAfterMalformedFailure = access.PrepareCopyToUser(range);
    ASTERIA_EXPECT(runner, "backend failure with no error becomes a side-effect-free protocol violation",
                   !malformedFailurePrepare.HasValue(),
                   malformedFailurePrepare.GetError() == UserMemoryAccessError::BackendProtocolViolation,
                   prepareAfterMalformedFailure.HasValue(),
                   access.CancelCopyToUser(prepareAfterMalformedFailure.GetValue()) == UserMemoryAccessError::None,
                   access.GetUserByte(0U) == 0x20U, access.GetUserByte(3U) == 0x23U);

    const auto prepared = access.PrepareCopyToUser(range);
    const UserMemoryWriteReservation reservation = prepared.GetValue();
    const Foundation::UInt8 source[4U]{0x91U, 0x92U, 0x93U, 0x94U};
    ASTERIA_EXPECT(runner, "prepared copy-out token is owner, exact-range, and generation bound", prepared.HasValue(),
                   reservation.IsValid(), reservation.IsFor(access), reservation.GetRange().IsEqualTo(range),
                   reservation.GetGeneration() != 0U);
    ASTERIA_EXPECT(runner, "foreign access object cannot commit another owner's reservation",
                   foreign.CommitCopyToUser(reservation, ReadOnlyKernelBuffer{.Data = source, .SizeBytes = 4U}) ==
                       UserMemoryAccessError::ForeignReservation);
    ASTERIA_EXPECT(runner, "copy-out rejects wrong-size input without consuming the reservation",
                   access.CommitCopyToUser(reservation, ReadOnlyKernelBuffer{.Data = source, .SizeBytes = 3U}) ==
                       UserMemoryAccessError::SizeMismatch);
    ASTERIA_EXPECT(runner, "cancelling a prepared copy-out releases it with no visible write",
                   access.CancelCopyToUser(reservation) == UserMemoryAccessError::None, access.GetUserByte(0U) == 0x20U,
                   access.GetUserByte(3U) == 0x23U);
    ASTERIA_EXPECT(runner, "cancelled generation is stale and cannot publish bytes",
                   access.CommitCopyToUser(reservation, ReadOnlyKernelBuffer{.Data = source, .SizeBytes = 4U}) ==
                       UserMemoryAccessError::StaleReservation);

    const auto committedPrepare = access.PrepareCopyToUser(range);
    ASTERIA_EXPECT(
        runner, "live prepared exact copy-out commits through an infallible publication path",
        committedPrepare.HasValue(),
        access.CommitCopyToUser(committedPrepare.GetValue(), ReadOnlyKernelBuffer{.Data = source, .SizeBytes = 4U}) ==
            UserMemoryAccessError::None,
        access.GetUserByte(0U) == 0x91U, access.GetUserByte(3U) == 0x94U);
    ASTERIA_EXPECT(
        runner, "committed copy-out token cannot be replayed",
        access.CommitCopyToUser(committedPrepare.GetValue(), ReadOnlyKernelBuffer{.Data = source, .SizeBytes = 4U}) ==
            UserMemoryAccessError::StaleReservation);

    const UserMemoryRange emptyRange = UserMemoryRange::Create(0U, 0U).GetValue();
    Foundation::UInt8 unused{0U};
    const auto emptyPrepare = access.PrepareCopyToUser(emptyRange);
    ASTERIA_EXPECT(
        runner, "empty copy-out accepts only the canonical null kernel-buffer shape", emptyPrepare.HasValue(),
        access.CommitCopyToUser(emptyPrepare.GetValue(), ReadOnlyKernelBuffer{.Data = &unused, .SizeBytes = 0U}) ==
            UserMemoryAccessError::NonCanonicalEmptyBuffer,
        access.CancelCopyToUser(emptyPrepare.GetValue()) == UserMemoryAccessError::None);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"IpcDeliveryCodecTests"};
    Asteria::Test::TestExactDeliveryEncoding(runner);
    Asteria::Test::TestDeliveryShapeAndOverlap(runner);
    Asteria::Test::TestDeliveryCanonicalityAndForgery(runner);
    Asteria::Test::TestDeliveryEncodeFailures(runner);
    Asteria::Test::TestUserMemoryRanges(runner);
    Asteria::Test::TestUserMemoryAccessContract(runner);
    return runner.Finish();
}
