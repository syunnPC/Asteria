// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_NET_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_NET_FRAME_HPP_INCLUDED

#include "Abi/IpcCanonicalFrame.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The frame protocol between a client (the network stack) and the NIC driver: asynchronous
/// canonical frames of NicInterface built by hand, as the block protocol's are. Every frame is its
/// 88-byte header, its attachments of 32 bytes, then a payload of four 64-bit words; the header's
/// two payload size fields carry the payload's size. Every request carries first a copy of a send
/// capability on the client's reply Endpoint (Invoke right), and its payload's first word is the
/// generation the client addressed (the driver instance's tag): a request for another generation
/// reached an instance that is not the one the client knows, and is answered NicDriverRestarted
/// without being executed. - Send (NicSendMethod): {generation, length NicMinimumFrameBytes to
/// NicMaximumFrameBytes, 0, 0}, then a copy of the client's data Frame with the Map and Write
/// rights (NicMapRight), its first length bytes the Ethernet frame (destination address first, no
/// CRC: the device adds it, and pads a frame shorter than 60 bytes). The driver copies the frame
/// into its own pages and answers once the device has sent it. - ReceiveCredit (NicReceiveMethod):
/// {generation, 0, 0, 0} with the data Frame as for a Send: the driver answers it when a frame
/// arrives, the frame copied to the Frame's offset 0 (at most NicMaximumReceiveBytes; the device
/// has stripped the CRC). Credits are filled in the order they arrived, across clients; a frame
/// that arrives with no credit outstanding is dropped and counted. - Query (NicQueryMethod):
/// {generation, 0, 0, 0} and no data Frame. A reply (NicReplyMethod) carries no attachment and the
/// payload {transaction answered, kind, detail, extra}; its header's transaction is the request's
/// too, so a client matches either. Exactly one reply answers each request: - NicDone: a Send:
/// detail its length; a ReceiveCredit: detail the received frame's length; a Query: detail the
/// device's MAC address, its first byte (the one sent first) in bits 0-7 and its sixth in bits
/// 40-47, extra the link state in bit 0 (1 up) and the frames dropped for want of a credit in bits
/// 32-63 (saturating). Extra is zero otherwise. - NicFailed: the driver cannot execute the request;
/// detail NicNoDevice (no device it may drive: no IOMMU to confine it) or NicMapRefused with the
/// kernel's error in the low byte (the data Frame is not one page the driver may map). -
/// NicRefused: beyond the client's credit (NicMaximumSends Sends and Queries,
/// NicMaximumReceiveCredits ReceiveCredits outstanding), a transaction the client already has
/// outstanding, or a full table; detail the credit of the request's class. Not executed. -
/// NicCancelled: reserved for a credit the driver gives back unfilled; the v0 driver sends none. -
/// NicDriverRestarted: the generation addressed is not the running instance's; not executed. Both
/// attachments are copies, so the client's handles need the Duplicate right as well.
inline constexpr Foundation::UInt64 NicInterfaceLow{0x4153'5452'4E49'4301ULL};  // "ASTRNIC", version 1
inline constexpr Foundation::UInt64 NicInterfaceHigh{0x4554'4845'524E'4504ULL}; // "ETHERNE", 4
inline constexpr Foundation::UInt32 NicSendMethod{1U};
inline constexpr Foundation::UInt32 NicReceiveMethod{2U};
inline constexpr Foundation::UInt32 NicQueryMethod{3U};
inline constexpr Foundation::UInt32 NicReplyMethod{4U};
/// A reply's kind.
inline constexpr Foundation::UInt64 NicDone{1U};
inline constexpr Foundation::UInt64 NicFailed{2U};
inline constexpr Foundation::UInt64 NicRefused{3U};
inline constexpr Foundation::UInt64 NicCancelled{4U};
inline constexpr Foundation::UInt64 NicDriverRestarted{5U};
/// A NicFailed reply's detail.
inline constexpr Foundation::UInt64 NicNoDevice{1U};
inline constexpr Foundation::UInt64 NicMapRefused{0x100U};
/// The credit of a client: Sends and Queries outstanding, and
/// ReceiveCredits outstanding.
inline constexpr Foundation::Size NicMaximumSends{4U};
inline constexpr Foundation::Size NicMaximumReceiveCredits{4U};
/// The per-sender capacity of a client's reply Endpoint: every reply a client within its credit may
/// have outstanding. An Endpoint queues at most eight messages (Endpoint::MaximumQueueEntries), so
/// there is no room for a refusal besides them: the reply Endpoint is created with this many
/// messages per sender and none reserved for control, and a client that breaks its credit may lose
/// the refusal, which the driver drops and counts.
inline constexpr Foundation::Size NicReplyCapacity{NicMaximumSends + NicMaximumReceiveCredits};
/// An Ethernet frame without its CRC: a header of two addresses and a type,
/// at most a 1500-byte payload; a received one may carry a VLAN tag besides.
inline constexpr Foundation::Size NicMinimumFrameBytes{14U};
inline constexpr Foundation::Size NicMaximumFrameBytes{1514U};
inline constexpr Foundation::Size NicMaximumReceiveBytes{2048U};
inline constexpr Foundation::Size NicPayloadWords{4U};
inline constexpr Foundation::Size NicPayloadBytes{NicPayloadWords << 3U};
/// Send and ReceiveCredit: header, reply and data attachments, four words.
inline constexpr Foundation::Size NicDataRequestBytes{EchoHeaderBytes + (2U * EchoAttachmentBytes) + NicPayloadBytes};
/// Query: header, reply attachment, four words.
inline constexpr Foundation::Size NicQueryBytes{EchoHeaderBytes + EchoAttachmentBytes + NicPayloadBytes};
inline constexpr Foundation::Size NicReplyBytes{EchoHeaderBytes + NicPayloadBytes};
/// The Map and Write rights of the data Frame's attachment.
// The driver CPU-maps client packets writable.
inline constexpr Foundation::UInt16 NicMapRight{0x0018U};
/// The sealed selector of the driver's Endpoint and the tags of that Endpoint and of the driver's
/// Interrupt (its device's MSI) in the driver's WaitSet.
inline constexpr Foundation::UInt64 NicSealedSelector{0x4E49'4353'4541'4C45ULL};
inline constexpr Foundation::UInt64 NicServiceWaitTag{0x4E49'4353U};
inline constexpr Foundation::UInt64 NicInterruptTag{0x4E49'4349U};

/// One decoded NIC frame.
struct NicMessage
{
    Foundation::UInt64 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[NicPayloadWords];
    /// A request's reply capability, as the receiver's space holds it.
    System::CapabilityHandle Reply;
    /// A Send's or ReceiveCredit's data Frame, as the receiver's space holds
    /// It.
    System::CapabilityHandle Data;
};

namespace Detail
{
inline void StoreNicWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] inline Foundation::UInt64 LoadNicWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}

/// The attachments a method carries: two for a Send or ReceiveCredit, one
/// for a Query, none for a reply; 3 for an unknown method.
[[nodiscard]] inline Foundation::Size CountNicAttachments(const Foundation::UInt64 method) noexcept
{
    if ((method == NicSendMethod) || (method == NicReceiveMethod))
    {
        return 2U;
    }
    if (method == NicQueryMethod)
    {
        return 1U;
    }
    return (method == NicReplyMethod) ? 0U : 3U;
}
} // Namespace Detail

/// Writes a NIC frame of method into frame (at most NicDataRequestBytes): a
/// Send or ReceiveCredit with its reply Endpoint attachment and data Frame
/// attachment (the Frame's rights are NicMapRight, copied), a Query with its
/// reply attachment alone (data is ignored), a reply with neither. Returns
/// its size, or zero, writing nothing, for an unknown method or a method
/// whose attachments are not given.
inline Foundation::Size WriteNicFrame(Foundation::UInt8* const frame, const Foundation::UInt32 method,
                                      const Foundation::UInt64 transaction,
                                      const Foundation::UInt64 (&words)[NicPayloadWords],
                                      const System::CapabilityHandle* const reply,
                                      const System::CapabilityHandle* const data) noexcept
{
    const Foundation::Size attachments{Detail::CountNicAttachments(method)};
    if ((attachments > 2U) || ((attachments >= 1U) && (reply == nullptr)) || ((attachments == 2U) && (data == nullptr)))
    {
        return 0U;
    }
    EchoAttachment records[2]{};
    if (attachments >= 1U)
    {
        records[0U] =
            EchoAttachment{.Handle = *reply, .Rights = EchoInvokeRight, .Mode = EchoCopy, .Type = EchoEndpointType};
    }
    if (attachments == 2U)
    {
        records[1U] = EchoAttachment{.Handle = *data, .Rights = NicMapRight, .Mode = EchoCopy, .Type = EchoFrameType};
    }
    WriteEchoFrame(frame, records, attachments);
    const Foundation::Size total = EchoHeaderBytes + (attachments << 5U) + NicPayloadBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(total >> (index << 3U));
    }
    frame[12U] = static_cast<Foundation::UInt8>(NicPayloadBytes);
    frame[13U] = 0U;
    frame[14U] = static_cast<Foundation::UInt8>(NicPayloadBytes);
    frame[15U] = 0U;
    Detail::StoreNicWord(frame + 16U, NicInterfaceLow);
    Detail::StoreNicWord(frame + 24U, NicInterfaceHigh);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[32U + index] = static_cast<Foundation::UInt8>(method >> (index << 3U));
    }
    Detail::StoreNicWord(frame + 40U, transaction);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    for (Foundation::Size word{0U}; word < NicPayloadWords; ++word)
    {
        Detail::StoreNicWord(payload + (word << 3U), words[word]);
    }
    return total;
}

/// Decodes the frame of a delivery of deliveryBytes (its 48-byte header
/// first): false, leaving message unchanged, unless it is a NIC frame of its
/// method's exact size and attachment count: a Send with an Endpoint then a
/// Frame attachment, a length of NicMinimumFrameBytes to
/// NicMaximumFrameBytes and two zero words; a ReceiveCredit with the same
/// attachments and three zero words; a Query with an Endpoint attachment and
/// three zero words; or a reply with none and a known kind (its extra word is
/// free).
[[nodiscard]] inline bool ReadNicFrame(const Foundation::UInt8* const delivery, const Foundation::Size deliveryBytes,
                                       NicMessage& message) noexcept
{
    if (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::UInt64 method = frame[32U] | (static_cast<Foundation::UInt64>(frame[33U]) << 8U);
    const Foundation::Size expected = Detail::CountNicAttachments(method);
    const Foundation::Size attachments = frame[81U];
    if ((Detail::LoadNicWord(frame + 16U) != NicInterfaceLow) ||
        (Detail::LoadNicWord(frame + 24U) != NicInterfaceHigh) || (frame[34U] != 0U) || (frame[35U] != 0U) ||
        (expected > 2U) || (frame[80U] != 0U) || (attachments != expected) || (frame[12U] != NicPayloadBytes) ||
        (frame[13U] != 0U) ||
        (deliveryBytes != (EchoDeliveryHeaderBytes + EchoHeaderBytes + (attachments << 5U) + NicPayloadBytes)) ||
        ((attachments >= 1U) && (frame[EchoHeaderBytes + 27U] != EchoEndpointType)) ||
        ((attachments == 2U) && (frame[EchoHeaderBytes + EchoAttachmentBytes + 27U] != EchoFrameType)))
    {
        return false;
    }
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    Foundation::UInt64 words[NicPayloadWords]{};
    for (Foundation::Size word{0U}; word < NicPayloadWords; ++word)
    {
        words[word] = Detail::LoadNicWord(payload + (word << 3U));
    }
    bool valid{false};
    if (method == NicSendMethod)
    {
        valid = (words[1U] >= NicMinimumFrameBytes) && (words[1U] <= NicMaximumFrameBytes) && (words[2U] == 0U) &&
                (words[3U] == 0U);
    }
    else if (attachments != 0U)
    {
        valid = (words[1U] == 0U) && (words[2U] == 0U) && (words[3U] == 0U);
    }
    else
    {
        valid = (words[1U] >= NicDone) && (words[1U] <= NicDriverRestarted);
    }
    if (!valid)
    {
        return false;
    }
    message.Method = method;
    message.Transaction = Detail::LoadNicWord(frame + 40U);
    for (Foundation::Size word{0U}; word < NicPayloadWords; ++word)
    {
        message.Words[word] = words[word];
    }
    message.Reply = (attachments >= 1U) ? System::CapabilityHandle{Detail::LoadNicWord(frame + EchoHeaderBytes),
                                                                   Detail::LoadNicWord(frame + EchoHeaderBytes + 8U)}
                                        : System::CapabilityHandle{};
    message.Data =
        (attachments == 2U)
            ? System::CapabilityHandle{Detail::LoadNicWord(frame + EchoHeaderBytes + EchoAttachmentBytes),
                                       Detail::LoadNicWord(frame + EchoHeaderBytes + EchoAttachmentBytes + 8U)}
            : System::CapabilityHandle{};
    return true;
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_NET_FRAME_HPP_INCLUDED
