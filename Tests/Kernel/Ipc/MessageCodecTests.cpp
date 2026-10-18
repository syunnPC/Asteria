// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Ipc::VariablePayloadDescriptor;
using Kernel::Ipc::WritableByteView;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectType;

constexpr Foundation::Size PayloadSizeOffset{12U};
constexpr Foundation::Size FixedPayloadSizeOffset{14U};
constexpr Foundation::Size InterfaceLowOffset{16U};
constexpr Foundation::Size InterfaceHighOffset{24U};
constexpr Foundation::Size MethodIdentifierOffset{32U};
constexpr Foundation::Size InterfaceVersionMajorOffset{36U};
constexpr Foundation::Size TransactionIdentifierOffset{40U};
constexpr Foundation::Size TraceIdentifierHighOffset{56U};
constexpr Foundation::Size TraceIdentifierLowOffset{64U};
constexpr Foundation::Size SpanIdentifierOffset{72U};
constexpr Foundation::Size VariablePayloadCountOffset{80U};
constexpr Foundation::Size AttachmentCountOffset{81U};
constexpr Foundation::Size MessageKindOffset{82U};
constexpr Foundation::Size CancellationStateOffset{83U};
constexpr Foundation::Size HeaderReservedOffset{84U};

[[nodiscard]] constexpr CapabilityRights InvokeAndRead() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read);
}

[[nodiscard]] constexpr CapabilityRights WriteAndMap() noexcept
{
    return CapabilityRights{CapabilityRight::Write}.With(CapabilityRight::Map);
}

[[nodiscard]] MessageEnvelope MakeValidMessage() noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = 0x8877'6655'4433'2211ULL;
    message.Deadline = MonotonicTime{0x1020'3040'5060'7080ULL};
    message.Trace = {
        .TraceIdentifierHigh = 0x1111'2222'3333'4444ULL,
        .TraceIdentifierLow = 0x5555'6666'7777'8888ULL,
        .SpanIdentifier = 0x9999'AAAA'BBBB'CCCCULL,
    };
    message.MethodIdentifier = 0x1020'3040U;
    message.InterfaceVersionMajor = 1U;
    message.InterfaceVersionMinor = 2U;
    message.FixedPayloadSizeBytes = 4U;
    message.PayloadSizeBytes = 12U;
    message.Kind = MessageKind::SynchronousCall;
    message.Cancellation = CancellationState::Cancellable;
    message.VariablePayloadCount = 2U;
    message.AttachmentCount = 2U;
    message.VariablePayloads[0] = VariablePayloadDescriptor{
        .OffsetBytes = 4U,
        .SizeBytes = 3U,
        .Reserved = 0U,
    };
    message.VariablePayloads[1] = VariablePayloadDescriptor{
        .OffsetBytes = 7U,
        .SizeBytes = 5U,
        .Reserved = 0U,
    };
    message.Attachments[0] = CapabilityAttachment{
        .Handle = CapabilityHandle{0x0101U, 0U},
        .RequestedDeadline = MonotonicTime{1'000U},
        .RequestedRights = InvokeAndRead(),
        .ExpectedType = ObjectType::Endpoint,
        .TransferMode = CapabilityTransferMode::Move,
        .Reserved = 0U,
    };
    message.Attachments[1] = CapabilityAttachment{
        .Handle = CapabilityHandle{0x0202U, 0xDEAD'BEEF'1234'5678ULL},
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = WriteAndMap(),
        .ExpectedType = ObjectType::MemoryObject,
        .TransferMode = CapabilityTransferMode::Copy,
        .Reserved = 0U,
    };
    for (Foundation::UInt16 index{0U}; index < message.PayloadSizeBytes; ++index)
    {
        message.Payload[index] = static_cast<Foundation::UInt8>(0xA0U + index);
    }
    return message;
}

[[nodiscard]] MessageEnvelope MakeMinimalMessage() noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{1U, 0U};
    message.TransactionIdentifier = 1U;
    message.Deadline = MonotonicTime::Infinite();
    message.MethodIdentifier = 1U;
    message.InterfaceVersionMajor = 1U;
    message.Kind = MessageKind::SynchronousCall;
    message.Cancellation = CancellationState::None;
    return message;
}

[[nodiscard]] MessageEnvelope MakeMaximumMessage() noexcept
{
    MessageEnvelope message = MakeMinimalMessage();
    message.PayloadSizeBytes = MessageEnvelope::MaximumPayloadBytes;
    message.Kind = MessageKind::Asynchronous;
    message.VariablePayloadCount = MessageEnvelope::MaximumVariablePayloads;
    message.AttachmentCount = MessageEnvelope::MaximumCapabilityAttachments;
    for (Foundation::Size index{0U}; index < MessageEnvelope::MaximumVariablePayloads; ++index)
    {
        message.VariablePayloads[index] = VariablePayloadDescriptor{
            .OffsetBytes = static_cast<Foundation::UInt16>(index * 64U),
            .SizeBytes = 64U,
            .Reserved = 0U,
        };
        message.Attachments[index] = CapabilityAttachment{
            .Handle = CapabilityHandle{index + 1U, index},
            .RequestedDeadline = MonotonicTime::Infinite(),
            .RequestedRights = CapabilityRights{CapabilityRight::Invoke},
            .ExpectedType = ObjectType::Endpoint,
            .TransferMode = (index & 1U) == 0U ? CapabilityTransferMode::Move : CapabilityTransferMode::Copy,
            .Reserved = 0U,
        };
        for (Foundation::Size byte{0U}; byte < 64U; ++byte)
        {
            message.Payload[(index * 64U) + byte] = static_cast<Foundation::UInt8>(index + byte);
        }
    }
    return message;
}

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

void FillBytes(Foundation::UInt8* const bytes, const Foundation::Size size, const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        bytes[index] = value;
    }
}

[[nodiscard]] bool AreBytesEqual(const void* const first, const void* const second,
                                 const Foundation::Size size) noexcept
{
    return std::memcmp(first, second, size) == 0;
}

void CopyWire(Foundation::UInt8* const destination, const Foundation::UInt8* const source,
              const Foundation::Size size) noexcept
{
    static_cast<void>(std::memcpy(destination, source, size));
}

void CheckDecodeFailure(TestContext& runner, const Foundation::UInt8* const wire, const Foundation::Size wireSize,
                        const MessageCodecError expected, const char* const description) noexcept
{
    MessageEnvelope output = MakeValidMessage();
    output.MethodIdentifier ^= 0xFFFF'0000U;
    const MessageEnvelope before = output;
    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{.Data = wire, .SizeBytes = wireSize}, output);
    ASTERIA_EXPECT(runner, description, error == expected, AreBytesEqual(&output, &before, sizeof(output)));
}

void CheckEncodeFailure(TestContext& runner, const MessageEnvelope& message, const MessageCodecError expected,
                        const char* const description) noexcept
{
    Foundation::UInt8 output[MessageCodec::MaximumWireSizeBytes]{};
    Foundation::UInt8 before[MessageCodec::MaximumWireSizeBytes]{};
    FillBytes(output, sizeof(output), 0x5AU);
    CopyWire(before, output, sizeof(output));
    const auto result = MessageCodec::Encode(message, WritableByteView{.Data = output, .SizeBytes = sizeof(output)});
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expected,
                   AreBytesEqual(output, before, sizeof(output)));
}

void TestExactEncodingAndRoundTrip(TestContext& runner) noexcept
{
    const MessageEnvelope message = MakeValidMessage();
    Foundation::UInt8 first[MessageCodec::MaximumWireSizeBytes]{};
    Foundation::UInt8 second[MessageCodec::MaximumWireSizeBytes]{};
    FillBytes(first, sizeof(first), 0xCCU);
    FillBytes(second, sizeof(second), 0x55U);
    const auto firstResult = MessageCodec::Encode(message, WritableByteView{.Data = first, .SizeBytes = sizeof(first)});
    const auto secondResult =
        MessageCodec::Encode(message, WritableByteView{.Data = second, .SizeBytes = sizeof(second)});
    constexpr Foundation::Size ExpectedSize{180U};
    ASTERIA_EXPECT(runner, "valid typed message encodes to the exact bounded frame size", firstResult.HasValue(),
                   secondResult.HasValue(), firstResult.GetValue() == ExpectedSize,
                   secondResult.GetValue() == ExpectedSize);
    ASTERIA_EXPECT(runner, "wire prefix is explicit little-endian AIPC version 1 framing", first[0] == 'A',
                   first[1] == 'I', first[2] == 'P', first[3] == 'C',
                   ReadLittleEndian16(first + 4U) == MessageCodec::WireVersion,
                   ReadLittleEndian16(first + 6U) == MessageCodec::WireHeaderSizeBytes,
                   ReadLittleEndian32(first + 8U) == ExpectedSize);
    ASTERIA_EXPECT(runner, "wire header fields use fixed offsets rather than a C++ object overlay",
                   ReadLittleEndian16(first + PayloadSizeOffset) == 12U,
                   ReadLittleEndian16(first + FixedPayloadSizeOffset) == 4U,
                   ReadLittleEndian64(first + InterfaceLowOffset) == message.Interface.GetLow(),
                   ReadLittleEndian64(first + InterfaceHighOffset) == message.Interface.GetHigh(),
                   ReadLittleEndian32(first + MethodIdentifierOffset) == message.MethodIdentifier,
                   ReadLittleEndian16(first + InterfaceVersionMajorOffset) == 1U,
                   ReadLittleEndian64(first + TransactionIdentifierOffset) == message.TransactionIdentifier);
    ASTERIA_EXPECT(runner, "deadline and every trace field occupy their documented golden offsets",
                   ReadLittleEndian16(first + 38U) == message.InterfaceVersionMinor,
                   ReadLittleEndian64(first + 48U) == message.Deadline.GetTicks(),
                   ReadLittleEndian64(first + TraceIdentifierHighOffset) == message.Trace.TraceIdentifierHigh,
                   ReadLittleEndian64(first + TraceIdentifierLowOffset) == message.Trace.TraceIdentifierLow,
                   ReadLittleEndian64(first + SpanIdentifierOffset) == message.Trace.SpanIdentifier);
    ASTERIA_EXPECT(runner, "counts, state, reserved field, and both variable descriptors are canonical",
                   first[VariablePayloadCountOffset] == 2U, first[AttachmentCountOffset] == 2U,
                   first[MessageKindOffset] == static_cast<Foundation::UInt8>(MessageKind::SynchronousCall),
                   first[CancellationStateOffset] == static_cast<Foundation::UInt8>(CancellationState::Cancellable),
                   ReadLittleEndian32(first + HeaderReservedOffset) == 0U,
                   ReadLittleEndian16(first + MessageCodec::WireHeaderSizeBytes) == 4U,
                   ReadLittleEndian16(first + MessageCodec::WireHeaderSizeBytes + 2U) == 3U,
                   ReadLittleEndian32(first + MessageCodec::WireHeaderSizeBytes + 4U) == 0U,
                   ReadLittleEndian16(first + MessageCodec::WireHeaderSizeBytes + 8U) == 7U,
                   ReadLittleEndian16(first + MessageCodec::WireHeaderSizeBytes + 10U) == 5U,
                   ReadLittleEndian32(first + MessageCodec::WireHeaderSizeBytes + 12U) == 0U);
    constexpr Foundation::Size FirstAttachmentOffset{104U};
    constexpr Foundation::Size PayloadOffset{168U};
    ASTERIA_EXPECT(runner, "capability attachment carries handle, attenuation, type, mode, and no padding",
                   ReadLittleEndian64(first + FirstAttachmentOffset) == 0x0101U,
                   ReadLittleEndian64(first + FirstAttachmentOffset + 8U) == 0U,
                   ReadLittleEndian64(first + FirstAttachmentOffset + 16U) == 1'000U,
                   ReadLittleEndian16(first + FirstAttachmentOffset + 24U) == InvokeAndRead().GetRawValue(),
                   first[FirstAttachmentOffset + 26U] == static_cast<Foundation::UInt8>(CapabilityTransferMode::Move),
                   first[FirstAttachmentOffset + 27U] == 9U,
                   ReadLittleEndian32(first + FirstAttachmentOffset + 28U) == 0U);
    constexpr Foundation::Size SecondAttachmentOffset{FirstAttachmentOffset +
                                                      MessageCodec::WireCapabilityAttachmentSizeBytes};
    ASTERIA_EXPECT(runner, "second capability attachment has an independent complete golden vector",
                   ReadLittleEndian64(first + SecondAttachmentOffset) == 0x0202U,
                   ReadLittleEndian64(first + SecondAttachmentOffset + 8U) == 0xDEAD'BEEF'1234'5678ULL,
                   ReadLittleEndian64(first + SecondAttachmentOffset + 16U) == MonotonicTime::Infinite().GetTicks(),
                   ReadLittleEndian16(first + SecondAttachmentOffset + 24U) == WriteAndMap().GetRawValue(),
                   first[SecondAttachmentOffset + 26U] == static_cast<Foundation::UInt8>(CapabilityTransferMode::Copy),
                   first[SecondAttachmentOffset + 27U] == 7U,
                   ReadLittleEndian32(first + SecondAttachmentOffset + 28U) == 0U);
    bool payloadMatches{true};
    for (Foundation::Size index{0U}; index < message.PayloadSizeBytes; ++index)
    {
        if (first[PayloadOffset + index] != message.Payload[index])
        {
            payloadMatches = false;
        }
    }
    bool tailPreserved{true};
    for (Foundation::Size index{ExpectedSize}; index < sizeof(first); ++index)
    {
        if (first[index] != 0xCCU)
        {
            tailPreserved = false;
        }
    }
    ASTERIA_EXPECT(runner, "encoder writes the exact payload and does not touch unused output capacity", payloadMatches,
                   tailPreserved);
    ASTERIA_EXPECT(runner, "repeated encoding is byte-for-byte deterministic",
                   AreBytesEqual(first, second, ExpectedSize));

    MessageEnvelope decoded{};
    FillBytes(reinterpret_cast<Foundation::UInt8*>(&decoded), sizeof(decoded), 0xA5U);
    const MessageCodecError decodeError =
        MessageCodec::Decode(ReadOnlyByteView{.Data = first, .SizeBytes = ExpectedSize}, decoded);
    ASTERIA_EXPECT(runner, "canonical frame round-trips to the byte-canonical internal envelope",
                   decodeError == MessageCodecError::None, AreBytesEqual(&decoded, &message, sizeof(message)));
}

void TestBufferAndFramingFailures(TestContext& runner) noexcept
{
    const MessageEnvelope message = MakeValidMessage();
    Foundation::UInt8 wire[MessageCodec::MaximumWireSizeBytes + 1U]{};
    const auto result = MessageCodec::Encode(message, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    ASTERIA_EXPECT(runner, "framing fixture encodes successfully", result.HasValue());

    MessageEnvelope output{};
    ASTERIA_EXPECT(runner, "nonempty null decode input is rejected",
                   MessageCodec::Decode(ReadOnlyByteView{.Data = nullptr, .SizeBytes = 1U}, output) ==
                       MessageCodecError::NullBuffer);
    ASTERIA_EXPECT(runner, "empty decode input is a short header, not a dereference",
                   MessageCodec::Decode(ReadOnlyByteView{.Data = nullptr, .SizeBytes = 0U}, output) ==
                       MessageCodecError::HeaderTooSmall);
    CheckDecodeFailure(runner, wire, MessageCodec::WireHeaderSizeBytes - 1U, MessageCodecError::HeaderTooSmall,
                       "truncated header is rejected without changing output");
    CheckDecodeFailure(runner, wire, MessageCodec::MaximumWireSizeBytes + 1U, MessageCodecError::FrameTooLarge,
                       "oversized frame is rejected before parsing");
    ASTERIA_EXPECT(runner, "decode rejects overlap between untrusted frame and destination object",
                   MessageCodec::Decode(
                       ReadOnlyByteView{
                           .Data = reinterpret_cast<const Foundation::UInt8*>(&output),
                           .SizeBytes = MessageCodec::WireHeaderSizeBytes,
                       },
                       output) == MessageCodecError::BufferOverlap);

    Foundation::UInt8 mutated[MessageCodec::MaximumWireSizeBytes]{};
    CopyWire(mutated, wire, result.GetValue());
    mutated[0] ^= 1U;
    CheckDecodeFailure(runner, mutated, result.GetValue(), MessageCodecError::InvalidMagic,
                       "wrong wire magic is rejected atomically");
    CopyWire(mutated, wire, result.GetValue());
    WriteLittleEndian16(mutated + 4U, 2U);
    CheckDecodeFailure(runner, mutated, result.GetValue(), MessageCodecError::UnsupportedWireVersion,
                       "unsupported wire version is rejected atomically");
    CopyWire(mutated, wire, result.GetValue());
    WriteLittleEndian16(mutated + 6U, 80U);
    CheckDecodeFailure(runner, mutated, result.GetValue(), MessageCodecError::InvalidHeaderSize,
                       "noncanonical header size is rejected atomically");
    CopyWire(mutated, wire, result.GetValue());
    WriteLittleEndian32(mutated + 8U, static_cast<Foundation::UInt32>(result.GetValue() + 1U));
    CheckDecodeFailure(runner, mutated, result.GetValue(), MessageCodecError::SizeMismatch,
                       "declared total size must equal the supplied frame extent");
    CopyWire(mutated, wire, result.GetValue());
    mutated[VariablePayloadCountOffset] = 3U;
    CheckDecodeFailure(runner, mutated, result.GetValue(), MessageCodecError::SizeMismatch,
                       "section counts must reconstruct the exact frame size");
}

void TestHeaderAndContractFailures(TestContext& runner) noexcept
{
    const MessageEnvelope message = MakeValidMessage();
    Foundation::UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(message, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    Foundation::UInt8 mutated[MessageCodec::MaximumWireSizeBytes]{};

    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + PayloadSizeOffset, 257U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::PayloadTooLarge,
                       "payload above the fixed inline bound is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + FixedPayloadSizeOffset, 13U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidFixedPayloadSize,
                       "fixed payload cannot extend past total payload");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[VariablePayloadCountOffset] = 5U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::TooManyVariablePayloads,
                       "variable descriptor count is bounded");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[AttachmentCountOffset] = 5U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::TooManyCapabilityAttachments,
                       "capability attachment count is bounded");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian32(mutated + HeaderReservedOffset, 1U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::ReservedFieldNonzero,
                       "nonzero reserved header data is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + InterfaceLowOffset, 0U);
    WriteLittleEndian64(mutated + InterfaceHighOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidInterfaceIdentifier,
                       "zero interface identifier is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian32(mutated + MethodIdentifierOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidMethodIdentifier,
                       "zero method identifier is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + InterfaceVersionMajorOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidInterfaceVersion,
                       "zero interface major version is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + TransactionIdentifierOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidTransactionIdentifier,
                       "zero transaction identifier is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + SpanIdentifierOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidTraceContext,
                       "nonzero trace identifier requires a nonzero span identifier");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + TraceIdentifierHighOffset, 0U);
    WriteLittleEndian64(mutated + TraceIdentifierLowOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidTraceContext,
                       "zero trace identifier cannot carry a span identifier");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[MessageKindOffset] = 0U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidMessageKind,
                       "invalid message kind is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[CancellationStateOffset] = 3U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidCancellationState,
                       "unknown cancellation state is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[CancellationStateOffset] = static_cast<Foundation::UInt8>(CancellationState::Requested);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidMessageContract,
                       "ordinary call cannot masquerade as a cancellation request");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[MessageKindOffset] = static_cast<Foundation::UInt8>(MessageKind::Reply);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidMessageContract,
                       "reply cannot advertise cancellable state");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[MessageKindOffset] = static_cast<Foundation::UInt8>(MessageKind::Cancellation);
    mutated[CancellationStateOffset] = static_cast<Foundation::UInt8>(CancellationState::Requested);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidMessageContract,
                       "cancellation frame cannot carry payload or capabilities");
}

void TestDescriptorAndAttachmentFailures(TestContext& runner) noexcept
{
    const MessageEnvelope message = MakeValidMessage();
    Foundation::UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(message, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    Foundation::UInt8 mutated[MessageCodec::MaximumWireSizeBytes]{};
    constexpr Foundation::Size DescriptorOffset{MessageCodec::WireHeaderSizeBytes};
    constexpr Foundation::Size AttachmentOffset{104U};

    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian32(mutated + DescriptorOffset + 4U, 1U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::ReservedFieldNonzero,
                       "variable descriptor reserved bits are rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + DescriptorOffset, 5U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidVariablePayloadLayout,
                       "variable descriptors cannot leave a gap after fixed payload");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + DescriptorOffset + 2U, 9U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidVariablePayloadLayout,
                       "variable descriptor cannot extend past payload");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + DescriptorOffset + 8U + 2U, 4U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidVariablePayloadLayout,
                       "variable descriptors must consume the complete variable payload");

    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + AttachmentOffset, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentHandle,
                       "attachment selector zero is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + AttachmentOffset + 24U, 0U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentRights,
                       "attachment with no requested rights is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian16(mutated + AttachmentOffset + 24U, 0x4000U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentRights,
                       "unknown capability right bits are rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[AttachmentOffset + 27U] = 0U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentObjectType,
                       "invalid attachment object type is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[AttachmentOffset + 27U] = 0xFFU;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentObjectType,
                       "unknown attachment object type is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[AttachmentOffset + 27U] = 24U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentObjectType,
                       "object type immediately above the fixed wire registry is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[AttachmentOffset + 26U] = 0U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentTransferMode,
                       "invalid capability transfer mode is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    mutated[AttachmentOffset + 26U] = 3U;
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::InvalidAttachmentTransferMode,
                       "unknown capability transfer mode is rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian32(mutated + AttachmentOffset + 28U, 1U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::ReservedFieldNonzero,
                       "attachment reserved bits are rejected");
    CopyWire(mutated, wire, encoded.GetValue());
    WriteLittleEndian64(mutated + AttachmentOffset + 32U, 0x0101U);
    CheckDecodeFailure(runner, mutated, encoded.GetValue(), MessageCodecError::DuplicateAttachment,
                       "same source selector cannot be transferred twice in one message");
}

void TestFixedObjectTypeAndRightsRegistry(TestContext& runner) noexcept
{
    constexpr ObjectType ExpectedTypes[]{
        ObjectType::ProtectionDomain,
        ObjectType::AddressSpace,
        ObjectType::CapabilitySpace,
        ObjectType::Thread,
        ObjectType::SchedulingContext,
        ObjectType::ResourceDomain,
        ObjectType::MemoryObject,
        ObjectType::Frame,
        ObjectType::Endpoint,
        ObjectType::ReplyToken,
        ObjectType::Notification,
        ObjectType::WaitSet,
        ObjectType::Timer,
        ObjectType::Interrupt,
        ObjectType::DeviceRegion,
        ObjectType::IommuDomain,
        ObjectType::FaultChannel,
        ObjectType::CpuSet,
        ObjectType::Vcpu,
        ObjectType::FaultToken,
        ObjectType::PciFunction,
        ObjectType::DiagnosticChannel,
        ObjectType::InterruptLine,
    };
    const MessageEnvelope message = MakeValidMessage();
    Foundation::UInt8 baseline[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded =
        MessageCodec::Encode(message, WritableByteView{.Data = baseline, .SizeBytes = sizeof(baseline)});
    constexpr Foundation::Size AttachmentOffset{104U};

    bool exactRegistry{encoded.HasValue()};
    for (Foundation::UInt8 encodedType{1U}; encodedType <= 23U; ++encodedType)
    {
        Foundation::UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
        CopyWire(wire, baseline, encoded.GetValue());
        wire[AttachmentOffset + 27U] = encodedType;
        MessageEnvelope decoded{};
        if ((MessageCodec::Decode(
                 ReadOnlyByteView{
                     .Data = wire,
                     .SizeBytes = encoded.GetValue(),
                 },
                 decoded) != MessageCodecError::None) ||
            (decoded.Attachments[0].ExpectedType != ExpectedTypes[encodedType - 1U]))
        {
            exactRegistry = false;
            continue;
        }
        Foundation::UInt8 reencoded[MessageCodec::MaximumWireSizeBytes]{};
        const auto result =
            MessageCodec::Encode(decoded, WritableByteView{.Data = reencoded, .SizeBytes = sizeof(reencoded)});
        if (!result.HasValue() || (reencoded[AttachmentOffset + 27U] != encodedType))
        {
            exactRegistry = false;
        }
    }
    ASTERIA_EXPECT(runner, "all twenty-one object types use their explicit fixed wire values", exactRegistry);

    Foundation::UInt8 allRights[MessageCodec::MaximumWireSizeBytes]{};
    CopyWire(allRights, baseline, encoded.GetValue());
    WriteLittleEndian16(allRights + AttachmentOffset + 24U, 0x3FFFU);
    MessageEnvelope decoded{};
    Foundation::UInt8 reencoded[MessageCodec::MaximumWireSizeBytes]{};
    const MessageCodecError decodeError = MessageCodec::Decode(
        ReadOnlyByteView{
            .Data = allRights,
            .SizeBytes = encoded.GetValue(),
        },
        decoded);
    const auto encodeResult =
        MessageCodec::Encode(decoded, WritableByteView{.Data = reencoded, .SizeBytes = sizeof(reencoded)});
    ASTERIA_EXPECT(runner, "all thirteen defined capability-right bits round-trip exactly",
                   decodeError == MessageCodecError::None,
                   decoded.Attachments[0].RequestedRights.GetRawValue() == 0x3FFFU, encodeResult.HasValue(),
                   ReadLittleEndian16(reencoded + AttachmentOffset + 24U) == 0x3FFFU);
}

void TestEncodeAtomicityAndCanonicalStorage(TestContext& runner) noexcept
{
    MessageEnvelope message = MakeValidMessage();
    message.ReservedAlignment = 1U;
    CheckEncodeFailure(runner, message, MessageCodecError::ReservedFieldNonzero,
                       "encoder rejects nonzero internal reserved alignment atomically");
    message = MakeValidMessage();
    message.VariablePayloads[3].SizeBytes = 1U;
    CheckEncodeFailure(runner, message, MessageCodecError::InvalidVariablePayloadLayout,
                       "encoder rejects hidden unused variable descriptor data");
    message = MakeValidMessage();
    message.Attachments[3].Handle = CapabilityHandle{0x404U, 0U};
    CheckEncodeFailure(runner, message, MessageCodecError::InvalidMessageContract,
                       "encoder rejects hidden unused capability attachment data");
    message = MakeValidMessage();
    message.Payload[MessageEnvelope::MaximumPayloadBytes - 1U] = 1U;
    CheckEncodeFailure(runner, message, MessageCodecError::InvalidMessageContract,
                       "encoder rejects hidden bytes beyond declared payload");
    message = MakeValidMessage();
    message.Attachments[0].RequestedRights = CapabilityRights{};
    CheckEncodeFailure(runner, message, MessageCodecError::InvalidAttachmentRights,
                       "encoder rejects attachment with no rights");
    message = MakeValidMessage();
    message.Attachments[0].ExpectedType = ObjectType::Invalid;
    CheckEncodeFailure(runner, message, MessageCodecError::InvalidAttachmentObjectType,
                       "encoder rejects the invalid object type atomically");
    message = MakeValidMessage();
    message.Attachments[0].ExpectedType = static_cast<ObjectType>(24U);
    CheckEncodeFailure(runner, message, MessageCodecError::InvalidAttachmentObjectType,
                       "encoder rejects an unknown object type atomically");
    message = MakeValidMessage();
    message.Attachments[1].Handle = CapabilityHandle{0x0101U, 0x9999U};
    CheckEncodeFailure(runner, message, MessageCodecError::DuplicateAttachment,
                       "encoder rejects duplicate source selector despite authenticator mismatch");

    message = MakeValidMessage();
    Foundation::UInt8 output[MessageCodec::MaximumWireSizeBytes]{};
    ASTERIA_EXPECT(
        runner, "encoder rejects null output",
        MessageCodec::Encode(message, WritableByteView{.Data = nullptr, .SizeBytes = sizeof(output)}).GetError() ==
            MessageCodecError::NullBuffer);
    FillBytes(output, sizeof(output), 0x6CU);
    Foundation::UInt8 before[MessageCodec::MaximumWireSizeBytes]{};
    CopyWire(before, output, sizeof(output));
    const auto shortResult = MessageCodec::Encode(message, WritableByteView{.Data = output, .SizeBytes = 179U});
    ASTERIA_EXPECT(runner, "short output buffer is rejected without a partial frame", !shortResult.HasValue(),
                   shortResult.GetError() == MessageCodecError::OutputTooSmall,
                   AreBytesEqual(output, before, sizeof(output)));
    ASTERIA_EXPECT(runner, "encoder rejects overlap with the source envelope",
                   MessageCodec::Encode(message,
                                        WritableByteView{
                                            .Data = reinterpret_cast<Foundation::UInt8*>(&message),
                                            .SizeBytes = sizeof(message),
                                        })
                           .GetError() == MessageCodecError::BufferOverlap);

    struct CapacityTailOverlap
    {
        Foundation::UInt8 Output[MessageCodec::WireHeaderSizeBytes];
        MessageEnvelope Message;
    };
    static_assert(offsetof(CapacityTailOverlap, Message) == MessageCodec::WireHeaderSizeBytes);
    CapacityTailOverlap capacityTailOverlap{
        .Output = {},
        .Message = MakeMinimalMessage(),
    };
    FillBytes(capacityTailOverlap.Output, sizeof(capacityTailOverlap.Output), 0x5AU);
    Foundation::UInt8 capacityTailBefore[MessageCodec::WireHeaderSizeBytes]{};
    CopyWire(capacityTailBefore, capacityTailOverlap.Output, sizeof(capacityTailBefore));
    const MessageEnvelope capacityTailMessageBefore = capacityTailOverlap.Message;
    const auto capacityTailResult =
        MessageCodec::Encode(capacityTailOverlap.Message, WritableByteView{
                                                              .Data = capacityTailOverlap.Output,
                                                              .SizeBytes = sizeof(capacityTailOverlap.Output) + 1U,
                                                          });
    ASTERIA_EXPECT(
        runner, "encoder rejects overlap in the unused tail of writable view capacity", !capacityTailResult.HasValue(),
        capacityTailResult.GetError() == MessageCodecError::BufferOverlap,
        AreBytesEqual(capacityTailOverlap.Output, capacityTailBefore, sizeof(capacityTailBefore)),
        AreBytesEqual(&capacityTailOverlap.Message, &capacityTailMessageBefore, sizeof(capacityTailMessageBefore)));
}

void TestKindStateMatrixAndCancellationIsolation(TestContext& runner) noexcept
{
    bool matrixMatches{true};
    for (Foundation::UInt8 kindValue{1U}; kindValue <= 4U; ++kindValue)
    {
        const MessageKind kind = static_cast<MessageKind>(kindValue);
        for (Foundation::UInt8 stateValue{0U}; stateValue <= 2U; ++stateValue)
        {
            const CancellationState state = static_cast<CancellationState>(stateValue);
            MessageEnvelope message = MakeMinimalMessage();
            message.Kind = kind;
            message.Cancellation = state;

            const bool expectedSuccess = ((kind == MessageKind::SynchronousCall) || (kind == MessageKind::Asynchronous))
                                             ? state != CancellationState::Requested
                                             : ((kind == MessageKind::Reply) ? state == CancellationState::None
                                                                             : state == CancellationState::Requested);
            Foundation::UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
            const auto encoded =
                MessageCodec::Encode(message, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
            if (encoded.HasValue() != expectedSuccess)
            {
                matrixMatches = false;
                continue;
            }
            if (!expectedSuccess)
            {
                if (encoded.GetError() != MessageCodecError::InvalidMessageContract)
                {
                    matrixMatches = false;
                }
                continue;
            }

            MessageEnvelope decoded{};
            FillBytes(reinterpret_cast<Foundation::UInt8*>(&decoded), sizeof(decoded), 0xA5U);
            if ((MessageCodec::Decode(
                     ReadOnlyByteView{
                         .Data = wire,
                         .SizeBytes = encoded.GetValue(),
                     },
                     decoded) != MessageCodecError::None) ||
                !AreBytesEqual(&decoded, &message, sizeof(message)))
            {
                matrixMatches = false;
            }
        }
    }
    ASTERIA_EXPECT(runner, "all twelve message-kind and cancellation-state combinations are exact", matrixMatches);

    MessageEnvelope cancellation = MakeMinimalMessage();
    cancellation.Kind = MessageKind::Cancellation;
    cancellation.Cancellation = CancellationState::Requested;
    cancellation.FixedPayloadSizeBytes = 1U;
    cancellation.PayloadSizeBytes = 1U;
    cancellation.Payload[0] = 0xA5U;
    CheckEncodeFailure(runner, cancellation, MessageCodecError::InvalidMessageContract,
                       "cancellation independently rejects a fixed payload");

    cancellation = MakeMinimalMessage();
    cancellation.Kind = MessageKind::Cancellation;
    cancellation.Cancellation = CancellationState::Requested;
    cancellation.VariablePayloadCount = 1U;
    cancellation.VariablePayloads[0] = VariablePayloadDescriptor{};
    CheckEncodeFailure(runner, cancellation, MessageCodecError::InvalidMessageContract,
                       "cancellation independently rejects even a zero-length variable field");

    cancellation = MakeMinimalMessage();
    cancellation.Kind = MessageKind::Cancellation;
    cancellation.Cancellation = CancellationState::Requested;
    cancellation.AttachmentCount = 1U;
    cancellation.Attachments[0] = CapabilityAttachment{
        .Handle = CapabilityHandle{1U, 0U},
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Invoke},
        .ExpectedType = ObjectType::Endpoint,
        .TransferMode = CapabilityTransferMode::Move,
        .Reserved = 0U,
    };
    CheckEncodeFailure(runner, cancellation, MessageCodecError::InvalidMessageContract,
                       "cancellation independently rejects a capability attachment");
}

void TestBoundaryFramesAndKinds(TestContext& runner) noexcept
{
    const MessageEnvelope maximum = MakeMaximumMessage();

    Foundation::UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
    const auto maximumResult = MessageCodec::Encode(maximum, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    MessageEnvelope decoded{};
    ASTERIA_EXPECT(runner, "maximum 504-byte frame with four variables and four capabilities round-trips",
                   maximumResult.HasValue(), maximumResult.GetValue() == MessageCodec::MaximumWireSizeBytes,
                   MessageCodec::Decode(
                       ReadOnlyByteView{
                           .Data = wire,
                           .SizeBytes = maximumResult.GetValue(),
                       },
                       decoded) == MessageCodecError::None,
                   AreBytesEqual(&maximum, &decoded, sizeof(maximum)));

    MessageEnvelope cancellation{};
    cancellation.Interface = InterfaceIdentifier{0U, 1U};
    cancellation.TransactionIdentifier = 9U;
    cancellation.Deadline = MonotonicTime{10U};
    cancellation.MethodIdentifier = 7U;
    cancellation.InterfaceVersionMajor = 2U;
    cancellation.Kind = MessageKind::Cancellation;
    cancellation.Cancellation = CancellationState::Requested;
    const auto cancellationResult =
        MessageCodec::Encode(cancellation, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    ASTERIA_EXPECT(runner, "payload-free cancellation request has one canonical 88-byte representation",
                   cancellationResult.HasValue(), cancellationResult.GetValue() == MessageCodec::WireHeaderSizeBytes,
                   MessageCodec::Decode(
                       ReadOnlyByteView{
                           .Data = wire,
                           .SizeBytes = cancellationResult.GetValue(),
                       },
                       decoded) == MessageCodecError::None,
                   decoded.Kind == MessageKind::Cancellation, decoded.Cancellation == CancellationState::Requested);

    MessageEnvelope reply = MakeValidMessage();
    reply.Kind = MessageKind::Reply;
    reply.Cancellation = CancellationState::None;
    ASTERIA_EXPECT(runner, "reply with explicit noncancellable state is accepted",
                   MessageCodec::Encode(reply, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)}).HasValue());
    MessageEnvelope noTrace = MakeValidMessage();
    noTrace.Trace = {};
    ASTERIA_EXPECT(runner, "fully absent trace context is accepted without partial identifiers",
                   MessageCodec::Encode(noTrace, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)}).HasValue());

    MessageEnvelope zeroLengthVariables = MakeValidMessage();
    zeroLengthVariables.FixedPayloadSizeBytes = 4U;
    zeroLengthVariables.PayloadSizeBytes = 4U;
    zeroLengthVariables.VariablePayloads[0] = {
        .OffsetBytes = 4U,
        .SizeBytes = 0U,
        .Reserved = 0U,
    };
    zeroLengthVariables.VariablePayloads[1] = {
        .OffsetBytes = 4U,
        .SizeBytes = 0U,
        .Reserved = 0U,
    };
    for (Foundation::Size index{4U}; index < MessageEnvelope::MaximumPayloadBytes; ++index)
    {
        zeroLengthVariables.Payload[index] = 0U;
    }
    ASTERIA_EXPECT(runner, "ordered zero-length variable fields retain a unique descriptor sequence",
                   MessageCodec::Encode(zeroLengthVariables, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)})
                       .HasValue());
}

void TestBoundedFramingProperties(TestContext& runner) noexcept
{
    const MessageEnvelope maximum = MakeMaximumMessage();
    Foundation::UInt8 baseline[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded =
        MessageCodec::Encode(maximum, WritableByteView{.Data = baseline, .SizeBytes = sizeof(baseline)});
    ASTERIA_EXPECT(runner, "bounded property fixture is the exact maximum-size frame", encoded.HasValue(),
                   encoded.GetValue() == MessageCodec::MaximumWireSizeBytes);
    if (!encoded.HasValue())
    {
        return;
    }

    bool allTruncationsAreAtomic{true};
    for (Foundation::Size size{0U}; size < encoded.GetValue(); ++size)
    {
        MessageEnvelope output = MakeValidMessage();
        output.MethodIdentifier ^= 0xFFFF'0000U;
        const MessageEnvelope before = output;
        if ((MessageCodec::Decode(ReadOnlyByteView{.Data = baseline, .SizeBytes = size}, output) ==
             MessageCodecError::None) ||
            !AreBytesEqual(&output, &before, sizeof(output)))
        {
            allTruncationsAreAtomic = false;
            break;
        }
    }
    ASTERIA_EXPECT(runner, "every truncation of a maximum frame fails without changing the envelope",
                   allTruncationsAreAtomic);

    bool allShortCapacitiesAreAtomic{true};
    for (Foundation::Size capacity{0U}; capacity < encoded.GetValue(); ++capacity)
    {
        Foundation::UInt8 output[MessageCodec::MaximumWireSizeBytes]{};
        Foundation::UInt8 before[MessageCodec::MaximumWireSizeBytes]{};
        FillBytes(output, sizeof(output), 0x5AU);
        CopyWire(before, output, sizeof(output));
        const auto result = MessageCodec::Encode(maximum, WritableByteView{.Data = output, .SizeBytes = capacity});
        if (result.HasValue() || (result.GetError() != MessageCodecError::OutputTooSmall) ||
            !AreBytesEqual(output, before, sizeof(output)))
        {
            allShortCapacitiesAreAtomic = false;
            break;
        }
    }
    ASTERIA_EXPECT(runner, "every insufficient output capacity fails without a partial frame",
                   allShortCapacitiesAreAtomic);

    bool allSingleByteMutationsAreCanonical{true};
    Foundation::UInt8 mutated[MessageCodec::MaximumWireSizeBytes]{};
    Foundation::UInt8 reencoded[MessageCodec::MaximumWireSizeBytes]{};
    for (Foundation::Size offset{0U}; (offset < encoded.GetValue()) && allSingleByteMutationsAreCanonical; ++offset)
    {
        for (Foundation::UInt16 difference{1U}; difference <= 0xFFU; ++difference)
        {
            CopyWire(mutated, baseline, encoded.GetValue());
            mutated[offset] ^= static_cast<Foundation::UInt8>(difference);
            MessageEnvelope decoded{};
            FillBytes(reinterpret_cast<Foundation::UInt8*>(&decoded), sizeof(decoded), 0xA5U);
            const MessageEnvelope before = decoded;
            const MessageCodecError error = MessageCodec::Decode(
                ReadOnlyByteView{
                    .Data = mutated,
                    .SizeBytes = encoded.GetValue(),
                },
                decoded);
            if (error != MessageCodecError::None)
            {
                if (!AreBytesEqual(&decoded, &before, sizeof(decoded)))
                {
                    allSingleByteMutationsAreCanonical = false;
                    break;
                }
                continue;
            }

            FillBytes(reencoded, sizeof(reencoded), 0x5AU);
            const auto result = MessageCodec::Encode(decoded, WritableByteView{
                                                                  .Data = reencoded,
                                                                  .SizeBytes = sizeof(reencoded),
                                                              });
            if (!result.HasValue() || (result.GetValue() != encoded.GetValue()) ||
                !AreBytesEqual(mutated, reencoded, encoded.GetValue()))
            {
                allSingleByteMutationsAreCanonical = false;
                break;
            }
        }
    }
    ASTERIA_EXPECT(runner, "all 128520 single-byte mutations reject atomically or re-encode exactly",
                   allSingleByteMutationsAreCanonical);

    MessageEnvelope decodeOverlap = MakeValidMessage();
    const MessageEnvelope decodeOverlapBefore = decodeOverlap;
    const Foundation::UInt8* const lastDecodeByte =
        reinterpret_cast<const Foundation::UInt8*>(&decodeOverlap) + sizeof(decodeOverlap) - 1U;
    const MessageCodecError oneByteDecodeOverlap = MessageCodec::Decode(
        ReadOnlyByteView{
            .Data = lastDecodeByte,
            .SizeBytes = MessageCodec::WireHeaderSizeBytes,
        },
        decodeOverlap);

    MessageEnvelope encodeOverlap = MakeValidMessage();
    Foundation::UInt8* const lastEncodeByte =
        reinterpret_cast<Foundation::UInt8*>(&encodeOverlap) + sizeof(encodeOverlap) - 1U;
    const auto oneByteEncodeOverlap =
        MessageCodec::Encode(encodeOverlap, WritableByteView{
                                                .Data = lastEncodeByte,
                                                .SizeBytes = MessageCodec::MaximumWireSizeBytes,
                                            });

    const Foundation::UIntPointer maximumAddress{~Foundation::UIntPointer{0U}};
    MessageEnvelope wrapOutput = MakeValidMessage();
    const MessageEnvelope wrapOutputBefore = wrapOutput;
    const MessageCodecError decodeWrap = MessageCodec::Decode(
        ReadOnlyByteView{
            .Data = reinterpret_cast<const Foundation::UInt8*>(maximumAddress - 7U),
            .SizeBytes = MessageCodec::WireHeaderSizeBytes,
        },
        wrapOutput);
    const auto encodeWrap =
        MessageCodec::Encode(maximum, WritableByteView{
                                          .Data = reinterpret_cast<Foundation::UInt8*>(maximumAddress - 7U),
                                          .SizeBytes = MessageCodec::MaximumWireSizeBytes,
                                      });
    ASTERIA_EXPECT(runner, "one-byte overlap and pointer-end overflow fail closed before any access",
                   oneByteDecodeOverlap == MessageCodecError::BufferOverlap,
                   AreBytesEqual(&decodeOverlap, &decodeOverlapBefore, sizeof(decodeOverlap)),
                   !oneByteEncodeOverlap.HasValue(),
                   oneByteEncodeOverlap.GetError() == MessageCodecError::BufferOverlap,
                   decodeWrap == MessageCodecError::BufferOverlap,
                   AreBytesEqual(&wrapOutput, &wrapOutputBefore, sizeof(wrapOutput)), !encodeWrap.HasValue(),
                   encodeWrap.GetError() == MessageCodecError::BufferOverlap);

    struct EncodingAdjacency
    {
        MessageEnvelope Message;
        Foundation::UInt8 Output[MessageCodec::MaximumWireSizeBytes];
    };
    struct DecodingAdjacency
    {
        Foundation::UInt8 Input[MessageCodec::MaximumWireSizeBytes];
        MessageEnvelope Output;
    };
    static_assert(offsetof(EncodingAdjacency, Output) == sizeof(MessageEnvelope));
    static_assert(offsetof(DecodingAdjacency, Output) == MessageCodec::MaximumWireSizeBytes);

    EncodingAdjacency encodeAdjacent{
        .Message = maximum,
        .Output = {},
    };
    const auto adjacentEncoding =
        MessageCodec::Encode(encodeAdjacent.Message, WritableByteView{
                                                         .Data = encodeAdjacent.Output,
                                                         .SizeBytes = sizeof(encodeAdjacent.Output),
                                                     });
    DecodingAdjacency decodeAdjacent{};
    CopyWire(decodeAdjacent.Input, baseline, sizeof(decodeAdjacent.Input));
    FillBytes(reinterpret_cast<Foundation::UInt8*>(&decodeAdjacent.Output), sizeof(decodeAdjacent.Output), 0xA5U);
    const MessageCodecError adjacentDecoding = MessageCodec::Decode(
        ReadOnlyByteView{
            .Data = decodeAdjacent.Input,
            .SizeBytes = sizeof(decodeAdjacent.Input),
        },
        decodeAdjacent.Output);
    ASTERIA_EXPECT(
        runner, "exactly adjacent ranges are accepted without an overlap false positive", adjacentEncoding.HasValue(),
        AreBytesEqual(encodeAdjacent.Output, baseline, sizeof(encodeAdjacent.Output)),
        adjacentDecoding == MessageCodecError::None, AreBytesEqual(&decodeAdjacent.Output, &maximum, sizeof(maximum)));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"MessageCodecTests"};
    Asteria::Test::TestExactEncodingAndRoundTrip(runner);
    Asteria::Test::TestBufferAndFramingFailures(runner);
    Asteria::Test::TestHeaderAndContractFailures(runner);
    Asteria::Test::TestDescriptorAndAttachmentFailures(runner);
    Asteria::Test::TestFixedObjectTypeAndRightsRegistry(runner);
    Asteria::Test::TestEncodeAtomicityAndCanonicalStorage(runner);
    Asteria::Test::TestKindStateMatrixAndCancellationIsolation(runner);
    Asteria::Test::TestBoundaryFramesAndKinds(runner);
    Asteria::Test::TestBoundedFramingProperties(runner);
    return runner.Finish();
}
