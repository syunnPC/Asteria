// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the kernel-to-user IPC delivery codec (IpcDeliveryCodec).
//
// Properties checked for every input:
// - Decode never reads outside the input or writes outside its outputs
//   (sanitizers report violations).
// - A rejected delivery leaves both outputs byte-for-byte unchanged.
// - An accepted delivery is self-consistent: a nonzero sequence and sender, a
//   known send class, valid rights, an attachment count equal to the
//   message's, and a frame size that fills the delivery after the header.
// - An accepted delivery is canonical: encoding the decoded metadata and
//   message succeeds and reproduces exactly the input bytes.
// - Encoding the same delivery into a buffer one byte too small fails with
//   OutputTooSmall and leaves every byte of that buffer unchanged.

#include "Kernel/Ipc/IpcDeliveryCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt8;
using Asteria::Kernel::Ipc::CancellationState;
using Asteria::Kernel::Ipc::CapabilityAttachment;
using Asteria::Kernel::Ipc::CapabilityTransferMode;
using Asteria::Kernel::Ipc::EndpointReceiveMetadata;
using Asteria::Kernel::Ipc::EndpointSendClass;
using Asteria::Kernel::Ipc::InterfaceIdentifier;
using Asteria::Kernel::Ipc::IpcDeliveryCodec;
using Asteria::Kernel::Ipc::IpcDeliveryCodecError;
using Asteria::Kernel::Ipc::MessageCodec;
using Asteria::Kernel::Ipc::MessageEnvelope;
using Asteria::Kernel::Ipc::MessageKind;
using Asteria::Kernel::Ipc::ReadOnlyByteView;
using Asteria::Kernel::Ipc::VariablePayloadDescriptor;
using Asteria::Kernel::Ipc::WritableByteView;
using Asteria::Kernel::Object::CapabilityHandle;
using Asteria::Kernel::Object::CapabilityRight;
using Asteria::Kernel::Object::CapabilityRights;
using Asteria::Kernel::Object::MonotonicTime;
using Asteria::Kernel::Object::ObjectType;
using Asteria::Kernel::Object::ResourceDomainId;

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

void Require(const bool condition, const char* const property)
{
    if (!condition)
    {
        Violation(property);
    }
}

[[nodiscard]] MessageEnvelope MakeMessage(const Size variableCount, const Size attachmentCount) noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = 0x8877'6655'4433'2211ULL;
    message.Deadline = (attachmentCount == 0U) ? MonotonicTime::Infinite() : MonotonicTime{0x1020'3040U};
    message.MethodIdentifier = 7U;
    message.InterfaceVersionMajor = 1U;
    message.InterfaceVersionMinor = 2U;
    message.Kind = (variableCount == MessageEnvelope::MaximumVariablePayloads) ? MessageKind::Asynchronous
                                                                               : MessageKind::SynchronousCall;
    message.Cancellation = CancellationState::None;
    const UInt16 slice = (variableCount == MessageEnvelope::MaximumVariablePayloads) ? UInt16{40U} : UInt16{4U};
    message.FixedPayloadSizeBytes = 8U;
    message.PayloadSizeBytes = static_cast<UInt16>(message.FixedPayloadSizeBytes + (variableCount * slice));
    message.VariablePayloadCount = static_cast<UInt8>(variableCount);
    message.AttachmentCount = static_cast<UInt8>(attachmentCount);
    for (Size index{0U}; index < variableCount; ++index)
    {
        message.VariablePayloads[index] = VariablePayloadDescriptor{
            .OffsetBytes = static_cast<UInt16>(message.FixedPayloadSizeBytes + (index * slice)),
            .SizeBytes = slice,
            .Reserved = 0U,
        };
    }
    for (Size index{0U}; index < attachmentCount; ++index)
    {
        message.Attachments[index] = CapabilityAttachment{
            .Handle = CapabilityHandle{0x0201U + index, 0x5A5A'0000U + index},
            .RequestedDeadline = (index == 0U) ? MonotonicTime{1'000U} : MonotonicTime::Infinite(),
            .RequestedRights = CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Transfer),
            .ExpectedType = ((index & 1U) == 0U) ? ObjectType::Endpoint : ObjectType::MemoryObject,
            .TransferMode = ((index & 1U) == 0U) ? CapabilityTransferMode::Move : CapabilityTransferMode::Copy,
            .Reserved = 0U,
        };
    }
    for (Size index{0U}; index < message.PayloadSizeBytes; ++index)
    {
        message.Payload[index] = static_cast<UInt8>(0x30U + index);
    }
    return message;
}

[[nodiscard]] EndpointReceiveMetadata MakeMetadata(const MessageEnvelope& message, const EndpointSendClass sendClass,
                                                   const CapabilityRights rights) noexcept
{
    const Size frameSize = MessageCodec::WireHeaderSizeBytes +
                           (message.VariablePayloadCount * MessageCodec::WireVariablePayloadDescriptorSizeBytes) +
                           (message.AttachmentCount * MessageCodec::WireCapabilityAttachmentSizeBytes) +
                           message.PayloadSizeBytes;
    return EndpointReceiveMetadata{
        .Sequence = 0x0102'0304'0506'0708ULL + message.AttachmentCount,
        .Sender = ResourceDomainId{0x44U},
        .SenderEndpointSealedObjectSelector = message.AttachmentCount * 0x1111U,
        .SenderEndpointRights = rights,
        .FrameSizeBytes = static_cast<UInt16>(frameSize),
        .AttachmentCount = message.AttachmentCount,
        .SendClass = sendClass,
    };
}

UInt8 g_seed[IpcDeliveryCodec::MaximumWireSizeBytes];

// Encodes one delivery into g_seed, applies an optional one-byte patch, and
// emits it.
void EmitDelivery(void (*const emit)(const std::uint8_t* data, std::size_t size), const MessageEnvelope& message,
                  const EndpointReceiveMetadata& metadata, const Size patchOffset, const UInt8 patchValue)
{
    const auto encoded =
        IpcDeliveryCodec::Encode(metadata, message, WritableByteView{.Data = g_seed, .SizeBytes = sizeof(g_seed)});
    Require(encoded.HasValue(), "seed delivery encodes");
    if ((patchOffset < encoded.GetValue()) && (patchOffset < sizeof(g_seed)))
    {
        g_seed[patchOffset] = patchValue;
    }
    emit(g_seed, encoded.GetValue());
}

// Output sentinels live in static storage, so their padding bytes are zero
// and byte comparisons are meaningful.
EndpointReceiveMetadata g_metadataBefore{};
MessageEnvelope g_messageBefore{};
EndpointReceiveMetadata g_metadata{};
MessageEnvelope g_message{};
UInt8 g_frame[IpcDeliveryCodec::MaximumWireSizeBytes];
UInt8 g_short[IpcDeliveryCodec::MaximumWireSizeBytes];
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    constexpr Size noPatch{~Size{0U}};
    const CapabilityRights invoke{CapabilityRight::Invoke};
    const MessageEnvelope minimal = MakeMessage(0U, 0U);
    const MessageEnvelope rich = MakeMessage(2U, 2U);
    const MessageEnvelope maximum =
        MakeMessage(MessageEnvelope::MaximumVariablePayloads, MessageEnvelope::MaximumCapabilityAttachments);
    const EndpointReceiveMetadata minimalMetadata = MakeMetadata(minimal, EndpointSendClass::Normal, invoke);
    EmitDelivery(emit, minimal, minimalMetadata, noPatch, 0U);
    EmitDelivery(emit, rich, MakeMetadata(rich, EndpointSendClass::ReservedControl, CapabilityRights::All()), noPatch,
                 0U);
    EmitDelivery(emit, maximum, MakeMetadata(maximum, EndpointSendClass::Normal, invoke), noPatch, 0U);
    // Near misses: a nonzero reserved byte, an attachment-count mismatch, an
    // unknown rights bit, a wrong total size, an unknown send class, and a
    // frame whose own header is damaged.
    EmitDelivery(emit, minimal, minimalMetadata, 42U, 1U);
    EmitDelivery(emit, minimal, minimalMetadata, 40U, 1U);
    EmitDelivery(emit, minimal, minimalMetadata, 15U, 0x80U);
    EmitDelivery(emit, minimal, minimalMetadata, 8U, 0U);
    EmitDelivery(emit, minimal, minimalMetadata, 41U, 3U);
    EmitDelivery(emit, rich, MakeMetadata(rich, EndpointSendClass::Normal, invoke), 48U, 0U);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    static bool initialized{false};
    if (!initialized)
    {
        g_messageBefore = MakeMessage(1U, 1U);
        g_messageBefore.TransactionIdentifier = 0xA5A5'A5A5'A5A5'A5A5ULL;
        g_metadataBefore = MakeMetadata(g_messageBefore, EndpointSendClass::ReservedControl, CapabilityRights::All());
        initialized = true;
    }
    std::memcpy(&g_metadata, &g_metadataBefore, sizeof(g_metadata));
    std::memcpy(&g_message, &g_messageBefore, sizeof(g_message));

    const auto failure =
        IpcDeliveryCodec::Decode(ReadOnlyByteView{.Data = data, .SizeBytes = size}, g_metadata, g_message);
    if (failure.Error != IpcDeliveryCodecError::None)
    {
        Require((std::memcmp(&g_metadata, &g_metadataBefore, sizeof(g_metadata)) == 0) &&
                    (std::memcmp(&g_message, &g_messageBefore, sizeof(g_message)) == 0),
                "a rejected delivery leaves both outputs unchanged");
        return 0;
    }

    Require((g_metadata.Sequence != 0U) && g_metadata.Sender.IsValid() && g_metadata.SenderEndpointRights.IsValid() &&
                ((g_metadata.SendClass == EndpointSendClass::Normal) ||
                 (g_metadata.SendClass == EndpointSendClass::ReservedControl)),
            "accepted metadata is valid");
    Require((g_metadata.AttachmentCount == g_message.AttachmentCount) &&
                ((IpcDeliveryCodec::WireHeaderSizeBytes + g_metadata.FrameSizeBytes) == size),
            "accepted metadata matches the message and the delivery size");

    const auto encoded = IpcDeliveryCodec::Encode(g_metadata, g_message,
                                                  WritableByteView{.Data = g_frame, .SizeBytes = sizeof(g_frame)});
    Require(encoded.HasValue(), "an accepted delivery re-encodes");
    Require((encoded.GetValue() == size) && (std::memcmp(g_frame, data, size) == 0),
            "an accepted delivery is canonical");

    std::memset(g_short, 0xA5, sizeof(g_short));
    const auto tooSmall =
        IpcDeliveryCodec::Encode(g_metadata, g_message, WritableByteView{.Data = g_short, .SizeBytes = size - 1U});
    bool untouched{true};
    for (const UInt8 value : g_short)
    {
        untouched = untouched && (value == 0xA5U);
    }
    Require(!tooSmall.HasValue() && (tooSmall.GetError().Error == IpcDeliveryCodecError::OutputTooSmall) && untouched,
            "a short output is rejected without being written");
    return 0;
}
