// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_ECHO_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_ECHO_FRAME_HPP_INCLUDED

#include "Abi/IpcCanonicalFrame.hpp"
#include "Foundation/Types.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The echo exchange of the root service's fourth incarnation: the root service sends the canonical
/// frame with two capability attachments, a copy of a send capability on its report Endpoint for
/// the reply and a moved Frame; the echo component replies on the first with the second moved back.
/// Both sides build the frames from the canonical one, with no codec: the attachments follow the
/// 88-byte header, since the canonical frame has no variable payload.
inline constexpr Foundation::UInt64 EchoWaitTag{0x4543'484FU};
/// The sealed selector of the echo's service Endpoint, which every request's delivery reports.
inline constexpr Foundation::UInt64 EchoSealedSelector{0x4543'484F'5345'414CULL};
/// Its place in a delivery's header (IpcDeliveryCodec).
inline constexpr Foundation::Size EchoDeliverySealedSelectorOffset{32U};
inline constexpr Foundation::Size EchoHeaderBytes{sizeof(Abi::CanonicalAsynchronousFrame)};
inline constexpr Foundation::Size EchoAttachmentBytes{32U};
inline constexpr Foundation::Size EchoDescriptorBytes{8U};
inline constexpr Foundation::Size EchoRequestBytes{EchoHeaderBytes + (2U * EchoAttachmentBytes)};
inline constexpr Foundation::Size EchoReplyBytes{EchoHeaderBytes + EchoAttachmentBytes};
/// A delivery's header before the frame (IpcDeliveryCodec).
inline constexpr Foundation::Size EchoDeliveryHeaderBytes{48U};

/// Capability rights (CapabilityTypes.hpp), transfer modes and object types
/// (IpcWireFormat.md) the exchange uses.
inline constexpr Foundation::UInt16 EchoInspectRight{0x0001U};
inline constexpr Foundation::UInt16 EchoInvokeRight{0x0002U};
inline constexpr Foundation::UInt16 EchoTransferRight{0x0080U};
inline constexpr Foundation::UInt16 EchoManageRight{0x0800U};
inline constexpr Foundation::UInt8 EchoMove{1U};
inline constexpr Foundation::UInt8 EchoCopy{2U};
inline constexpr Foundation::UInt8 EchoFrameType{8U};
inline constexpr Foundation::UInt8 EchoEndpointType{9U};
/// The moved Frame's rights each way: its holder can move it on, query it
/// and destroy it.
inline constexpr Foundation::UInt16 EchoFrameRights{EchoTransferRight | EchoManageRight | EchoInspectRight};

/// One capability attachment of an echo frame.
struct EchoAttachment
{
    System::CapabilityHandle Handle;
    Foundation::UInt16 Rights;
    Foundation::UInt8 Mode;
    Foundation::UInt8 Type;
};

/// Writes the canonical frame with count attachments (at most two) into
/// frame, EchoHeaderBytes plus EchoAttachmentBytes each; every lease
/// deadline is the source's, infinite.
inline void WriteEchoFrame(Foundation::UInt8* const frame, const EchoAttachment* const attachments,
                           const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < EchoHeaderBytes; ++index)
    {
        frame[index] = Abi::CanonicalAsynchronousFrame[index];
    }
    const Foundation::Size total = EchoHeaderBytes + (count << 5U);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(total >> (index << 3U));
    }
    frame[81U] = static_cast<Foundation::UInt8>(count);
    for (Foundation::Size attachment{0U}; attachment < count; ++attachment)
    {
        Foundation::UInt8* const record = frame + EchoHeaderBytes + (attachment << 5U);
        const EchoAttachment& source = attachments[attachment];
        for (Foundation::Size index{0U}; index < 8U; ++index)
        {
            record[index] = static_cast<Foundation::UInt8>(source.Handle.Selector >> (index << 3U));
            record[8U + index] = static_cast<Foundation::UInt8>(source.Handle.Authenticator >> (index << 3U));
            record[16U + index] = 0xFFU;
        }
        record[24U] = static_cast<Foundation::UInt8>(source.Rights);
        record[25U] = static_cast<Foundation::UInt8>(source.Rights >> 8U);
        record[26U] = source.Mode;
        record[27U] = source.Type;
        for (Foundation::Size index{28U}; index < EchoAttachmentBytes; ++index)
        {
            record[index] = 0U;
        }
    }
}

/// The sealed selector of the capability a delivery's sender invoked.
[[nodiscard]] inline Foundation::UInt64 ReadEchoSealedSelector(const Foundation::UInt8* const delivery) noexcept
{
    Foundation::UInt64 selector{0U};
    for (Foundation::Size byte{0U}; byte < 8U; ++byte)
    {
        selector |= static_cast<Foundation::UInt64>(delivery[EchoDeliverySealedSelectorOffset + byte]) << (byte << 3U);
    }
    return selector;
}

/// The largest frame an echo answers (IpcWireFormat.md).
inline constexpr Foundation::Size EchoMaximumFrameBytes{504U};

/// Attachment records follow every variable descriptor. These readers consume kernel-decoded
/// deliveries; the kernel has already checked the frame length and the descriptor/attachment
/// counts.
[[nodiscard]] inline Foundation::Size EchoAttachmentOffset(const Foundation::UInt8* const frame,
                                                           const Foundation::Size index = 0U) noexcept
{
    return EchoHeaderBytes + static_cast<Foundation::Size>(frame[80U]) * EchoDescriptorBytes +
           index * EchoAttachmentBytes;
}

/// Writes the echo's answer to a delivery of deliveryBytes into reply
/// (EchoMaximumFrameBytes): the request's frame, its header, variable
/// payload descriptors and payload unchanged (so its transaction identifier
/// matches it to its request), without its first attachment, the reply
/// capability the answer goes to; every other attachment is moved back with
/// the rights it came with. Returns the answer's size, or zero for a
/// delivery that is no frame with a reply attachment.
[[nodiscard]] inline Foundation::Size WriteEchoReply(const Foundation::UInt8* const delivery,
                                                     const Foundation::Size deliveryBytes,
                                                     Foundation::UInt8* const reply) noexcept
{
    if (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return 0U;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::Size attachmentStart = EchoAttachmentOffset(frame);
    const Foundation::Size attachments = frame[81U];
    const Foundation::Size payload =
        static_cast<Foundation::Size>(frame[12U]) | (static_cast<Foundation::Size>(frame[13U]) << 8U);
    const Foundation::Size frameBytes = EchoAttachmentOffset(frame, attachments) + payload;
    if ((attachments == 0U) || ((EchoDeliveryHeaderBytes + frameBytes) != deliveryBytes) ||
        (frameBytes > EchoMaximumFrameBytes) || (frame[attachmentStart + 27U] != EchoEndpointType))
    {
        return 0U;
    }
    // Header and descriptors, the attachments after the first, the payload.
    Foundation::Size out{0U};
    for (Foundation::Size index{0U}; index < attachmentStart; ++index)
    {
        reply[out++] = frame[index];
    }
    for (Foundation::Size index{EchoAttachmentOffset(frame, 1U)}; index < EchoAttachmentOffset(frame, attachments);
         ++index)
    {
        reply[out++] = frame[index];
    }
    for (Foundation::Size record{1U}; record < attachments; ++record)
    {
        reply[EchoAttachmentOffset(frame, record - 1U) + 26U] = EchoMove;
    }
    for (Foundation::Size index{frameBytes - payload}; index < frameBytes; ++index)
    {
        reply[out++] = frame[index];
    }
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        reply[8U + index] = static_cast<Foundation::UInt8>(out >> (index << 3U));
    }
    reply[81U] = static_cast<Foundation::UInt8>(attachments - 1U);
    return out;
}

/// The attachment count of a delivery of an echo frame.
[[nodiscard]] inline Foundation::UInt8 GetEchoAttachmentCount(const Foundation::UInt8* const delivery) noexcept
{
    return delivery[EchoDeliveryHeaderBytes + 81U];
}

/// Attachment index's handle in the receiver's space, as the kernel wrote it
/// into the delivery, and its object type.
[[nodiscard]] inline System::CapabilityHandle ReadEchoHandle(const Foundation::UInt8* const delivery,
                                                             const Foundation::Size index) noexcept
{
    const auto* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::UInt8* const record = frame + EchoAttachmentOffset(frame, index);
    Foundation::UInt64 selector{0U};
    Foundation::UInt64 authenticator{0U};
    for (Foundation::Size byte{0U}; byte < 8U; ++byte)
    {
        selector |= static_cast<Foundation::UInt64>(record[byte]) << (byte << 3U);
        authenticator |= static_cast<Foundation::UInt64>(record[8U + byte]) << (byte << 3U);
    }
    return System::CapabilityHandle{selector, authenticator};
}

[[nodiscard]] inline Foundation::UInt8 ReadEchoType(const Foundation::UInt8* const delivery,
                                                    const Foundation::Size index) noexcept
{
    const auto* const frame = delivery + EchoDeliveryHeaderBytes;
    return frame[EchoAttachmentOffset(frame, index) + 27U];
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_ECHO_FRAME_HPP_INCLUDED
