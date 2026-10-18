// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the canonical IPC wire codec (MessageCodec).
//
// Properties checked for every input:
// - Decode never reads outside the input or writes outside the envelope
//   (sanitizers report violations).
// - A rejected frame leaves the output envelope byte-for-byte unchanged.
// - An accepted frame is canonical: re-encoding the decoded envelope succeeds
//   and reproduces exactly the input bytes.

#include "Kernel/Ipc/MessageCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt8;
using Asteria::Kernel::Ipc::CancellationState;
using Asteria::Kernel::Ipc::CapabilityAttachment;
using Asteria::Kernel::Ipc::CapabilityTransferMode;
using Asteria::Kernel::Ipc::InterfaceIdentifier;
using Asteria::Kernel::Ipc::MessageCodec;
using Asteria::Kernel::Ipc::MessageCodecError;
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

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
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

[[nodiscard]] MessageEnvelope MakeRichMessage(const bool maximum) noexcept
{
    MessageEnvelope message = MakeMinimalMessage();
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = 0x8877'6655'4433'2211ULL;
    message.Deadline = MonotonicTime{0x1020'3040'5060'7080ULL};
    message.Trace = {
        .TraceIdentifierHigh = 0x1111'2222'3333'4444ULL,
        .TraceIdentifierLow = 0x5555'6666'7777'8888ULL,
        .SpanIdentifier = 0x9999'AAAA'BBBB'CCCCULL,
    };
    message.InterfaceVersionMinor = 2U;
    message.Kind = maximum ? MessageKind::Asynchronous : MessageKind::SynchronousCall;
    message.Cancellation = maximum ? CancellationState::None : CancellationState::Cancellable;
    const std::size_t variableCount = maximum ? MessageEnvelope::MaximumVariablePayloads : 2U;
    const std::size_t attachmentCount = maximum ? MessageEnvelope::MaximumCapabilityAttachments : 2U;
    const UInt16 slice = maximum ? UInt16{64U} : UInt16{4U};
    message.FixedPayloadSizeBytes = maximum ? UInt16{0U} : UInt16{4U};
    message.PayloadSizeBytes = static_cast<UInt16>(message.FixedPayloadSizeBytes + (variableCount * slice));
    message.VariablePayloadCount = static_cast<UInt8>(variableCount);
    message.AttachmentCount = static_cast<UInt8>(attachmentCount);
    for (std::size_t index{0U}; index < variableCount; ++index)
    {
        message.VariablePayloads[index] = VariablePayloadDescriptor{
            .OffsetBytes = static_cast<UInt16>(message.FixedPayloadSizeBytes + (index * slice)),
            .SizeBytes = slice,
            .Reserved = 0U,
        };
    }
    for (std::size_t index{0U}; index < attachmentCount; ++index)
    {
        message.Attachments[index] = CapabilityAttachment{
            .Handle = CapabilityHandle{0x0101U + index, index * 0x1111U},
            .RequestedDeadline = (index == 0U) ? MonotonicTime{1'000U} : MonotonicTime::Infinite(),
            .RequestedRights = CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read),
            .ExpectedType = (index & 1U) == 0U ? ObjectType::Endpoint : ObjectType::MemoryObject,
            .TransferMode = (index & 1U) == 0U ? CapabilityTransferMode::Move : CapabilityTransferMode::Copy,
            .Reserved = 0U,
        };
    }
    for (std::size_t index{0U}; index < message.PayloadSizeBytes; ++index)
    {
        message.Payload[index] = static_cast<UInt8>(0xA0U + index);
    }
    return message;
}

void EmitEncoded(const MessageEnvelope& message, void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    UInt8 frame[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(message, WritableByteView{.Data = frame, .SizeBytes = sizeof(frame)});
    if (!encoded.HasValue())
    {
        Violation("seed message encodes");
    }
    emit(frame, encoded.GetValue());
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    EmitEncoded(MakeMinimalMessage(), emit);
    EmitEncoded(MakeRichMessage(false), emit);
    EmitEncoded(MakeRichMessage(true), emit);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    // MessageEnvelope has no padding (its layout is pinned by static_assert),
    // so comparing object representations detects any write on failure.
    MessageEnvelope before = MakeRichMessage(true);
    before.TransactionIdentifier = 0xA5A5'A5A5'A5A5'A5A5ULL;
    MessageEnvelope decoded = before;

    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{.Data = data, .SizeBytes = size}, decoded);
    if (error != MessageCodecError::None)
    {
        if (std::memcmp(&before, &decoded, sizeof(decoded)) != 0)
        {
            Violation("a rejected frame leaves the output unchanged");
        }
        return 0;
    }

    UInt8 frame[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(decoded, WritableByteView{.Data = frame, .SizeBytes = sizeof(frame)});
    if (!encoded.HasValue())
    {
        Violation("an accepted frame re-encodes");
    }
    if ((encoded.GetValue() != size) || (std::memcmp(frame, data, size) != 0))
    {
        Violation("an accepted frame is canonical");
    }
    return 0;
}
