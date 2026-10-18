// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_IPC_DELIVERY_CODEC_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_IPC_DELIVERY_CODEC_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/EndpointTypes.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"

namespace Asteria::Kernel::Ipc
{
enum class IpcDeliveryCodecError : Foundation::UInt8
{
    None,
    NullBuffer,
    BufferOverlap,
    HeaderTooSmall,
    DeliveryTooLarge,
    InvalidMagic,
    UnsupportedWireVersion,
    InvalidHeaderSize,
    SizeMismatch,
    InvalidFrameSize,
    InvalidSequence,
    InvalidSender,
    TooManyAttachments,
    InvalidSendClass,
    InvalidEffectiveRights,
    ReservedFieldNonzero,
    MetadataMismatch,
    FrameRejected,
    OutputTooSmall,
};

struct IpcDeliveryCodecFailure
{
    MessageCodecError Frame;
    IpcDeliveryCodecError Error;
    Foundation::UInt8 Reserved[6];
};

static_assert(sizeof(IpcDeliveryCodecFailure) == 8U);
static_assert(alignof(IpcDeliveryCodecFailure) == 1U);

/// Canonical kernel-to-user delivery framing. The fixed delivery header is
/// followed immediately by one complete canonical MessageCodec frame.
class IpcDeliveryCodec final
{
public:
    static constexpr Foundation::UInt32 WireMagic{0x4450'4941U};
    static constexpr Foundation::UInt16 WireVersion{1U};
    static constexpr Foundation::Size WireHeaderSizeBytes{48U};
    static constexpr Foundation::Size MaximumWireSizeBytes{WireHeaderSizeBytes + MessageCodec::MaximumWireSizeBytes};

    IpcDeliveryCodec() = delete;

    /// Encodes trusted receive metadata and its exact canonical message.
    /// Input objects and the entire writable view must not overlap. Failure
    /// leaves every output byte unchanged; success returns the exact extent.
    [[nodiscard]] static Foundation::Result<Foundation::Size, IpcDeliveryCodecFailure>
    Encode(const EndpointReceiveMetadata& metadata, const MessageEnvelope& message, WritableByteView output) noexcept;

    /// Decodes one complete canonical delivery. The input and both outputs
    /// must be pairwise disjoint. Every failure leaves both outputs unchanged.
    [[nodiscard]] static IpcDeliveryCodecFailure Decode(ReadOnlyByteView input, EndpointReceiveMetadata& metadata,
                                                        MessageEnvelope& message) noexcept;
};
} // namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_IPC_DELIVERY_CODEC_HPP_INCLUDED
