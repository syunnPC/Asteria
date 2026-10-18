// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_MESSAGE_CODEC_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_MESSAGE_CODEC_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Kernel::Ipc
{
class InterfaceIdentifier final
{
public:
    constexpr InterfaceIdentifier() noexcept = default;

    constexpr InterfaceIdentifier(const Foundation::UInt64 low, const Foundation::UInt64 high) noexcept
        : m_low{low}, m_high{high}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetLow() const noexcept
    {
        return m_low;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetHigh() const noexcept
    {
        return m_high;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_low != 0U) || (m_high != 0U);
    }

private:
    Foundation::UInt64 m_low{0U};
    Foundation::UInt64 m_high{0U};
};

static_assert(sizeof(InterfaceIdentifier) == 16U);
static_assert(alignof(InterfaceIdentifier) == 8U);

enum class MessageKind : Foundation::UInt8
{
    Invalid = 0U,
    SynchronousCall = 1U,
    Reply = 2U,
    Asynchronous = 3U,
    Cancellation = 4U,
};

enum class CancellationState : Foundation::UInt8
{
    None = 0U,
    Cancellable = 1U,
    Requested = 2U,
};

enum class CapabilityTransferMode : Foundation::UInt8
{
    Invalid = 0U,
    Move = 1U,
    Copy = 2U,
};

struct TraceContext
{
    Foundation::UInt64 TraceIdentifierHigh;
    Foundation::UInt64 TraceIdentifierLow;
    Foundation::UInt64 SpanIdentifier;
};

static_assert(sizeof(TraceContext) == 24U);

struct VariablePayloadDescriptor
{
    Foundation::UInt16 OffsetBytes;
    Foundation::UInt16 SizeBytes;
    Foundation::UInt32 Reserved;
};

static_assert(sizeof(VariablePayloadDescriptor) == 8U);

struct CapabilityAttachment
{
    Object::CapabilityHandle Handle;
    Object::MonotonicTime RequestedDeadline;
    Object::CapabilityRights RequestedRights;
    Object::ObjectType ExpectedType;
    CapabilityTransferMode TransferMode;
    Foundation::UInt32 Reserved;
};

static_assert(sizeof(CapabilityAttachment) == 32U);
static_assert(alignof(CapabilityAttachment) == 8U);

struct MessageEnvelope
{
    static constexpr Foundation::Size MaximumPayloadBytes{256U};
    static constexpr Foundation::Size MaximumVariablePayloads{4U};
    static constexpr Foundation::Size MaximumCapabilityAttachments{4U};

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
    Foundation::UInt32 ReservedAlignment;
    CapabilityAttachment Attachments[MaximumCapabilityAttachments];
    VariablePayloadDescriptor VariablePayloads[MaximumVariablePayloads];
    Foundation::UInt8 Payload[MaximumPayloadBytes];
};

static_assert(sizeof(MessageEnvelope) == 496U);
static_assert(alignof(MessageEnvelope) == 8U);

struct ReadOnlyByteView
{
    const Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

static_assert(sizeof(ReadOnlyByteView) == 16U);
static_assert(alignof(ReadOnlyByteView) == 8U);

struct WritableByteView
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

static_assert(sizeof(WritableByteView) == 16U);
static_assert(alignof(WritableByteView) == 8U);

enum class MessageCodecError : Foundation::UInt8
{
    None,
    NullBuffer,
    BufferOverlap,
    HeaderTooSmall,
    FrameTooLarge,
    InvalidMagic,
    UnsupportedWireVersion,
    InvalidHeaderSize,
    SizeMismatch,
    PayloadTooLarge,
    InvalidFixedPayloadSize,
    TooManyVariablePayloads,
    TooManyCapabilityAttachments,
    ReservedFieldNonzero,
    InvalidInterfaceIdentifier,
    InvalidMethodIdentifier,
    InvalidInterfaceVersion,
    InvalidTransactionIdentifier,
    InvalidTraceContext,
    InvalidMessageKind,
    InvalidCancellationState,
    InvalidMessageContract,
    InvalidVariablePayloadLayout,
    InvalidAttachmentHandle,
    InvalidAttachmentRights,
    InvalidAttachmentObjectType,
    InvalidAttachmentTransferMode,
    DuplicateAttachment,
    OutputTooSmall,
};

class MessageCodec final
{
public:
    static constexpr Foundation::UInt32 WireMagic{0x4350'4941U};
    static constexpr Foundation::UInt16 WireVersion{1U};
    static constexpr Foundation::Size WireHeaderSizeBytes{88U};
    static constexpr Foundation::Size WireVariablePayloadDescriptorSizeBytes{8U};
    static constexpr Foundation::Size WireCapabilityAttachmentSizeBytes{32U};
    static constexpr Foundation::Size MaximumWireSizeBytes{504U};

    MessageCodec() = delete;

    /// Decodes one complete canonical wire frame.
    ///
    /// The input must remain readable and stable for the call. Input and output must not
    /// overlap. Every failure leaves output unchanged. The function is allocation-free and
    /// bounded by the fixed envelope capacities.
    [[nodiscard]] static MessageCodecError Decode(ReadOnlyByteView input, MessageEnvelope& output) noexcept;

    /// Encodes one canonical wire frame.
    ///
    /// The message must remain stable for the call. Message storage and the entire writable
    /// view must not overlap. Every failure leaves the output bytes unchanged. Success carries
    /// the exact frame size; failure carries a MessageCodecError.
    [[nodiscard]] static Foundation::Result<Foundation::Size, MessageCodecError>
    Encode(const MessageEnvelope& message, WritableByteView output) noexcept;
};
} // namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_MESSAGE_CODEC_HPP_INCLUDED
