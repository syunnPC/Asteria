// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_SOCKET_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_SOCKET_FRAME_HPP_INCLUDED

#include "Abi/IpcCanonicalFrame.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The socket protocol between a client and the network stack service: asynchronous canonical
/// frames of SocketInterface built by hand, as the file and NIC protocols' are. Every frame is its
/// 88-byte header, its attachments of 32 bytes, four 64-bit words, then an inline area of exactly
/// SocketInlineBytes bytes: the bytes the words give, then zeros, so each method has one exact
/// size. The header's two payload size fields carry the payload's size (224, one byte each). Each
/// frame has its own transaction identifier in its header, never zero (the kernel's codec refuses
/// it). Every request carries one attachment, a copy of a send capability on the client's reply
/// Endpoint (Invoke right; the client's handle needs Duplicate as well), and its first word is the
/// generation it addresses: the stack instance's tag (its incarnation). A request for another
/// generation reached an instance that is not the one the client knows and is answered
/// SocketRestarted only. A reply carries no attachment. Requests (ports in 16-bit fields; the
/// reader refuses a bit beyond them): - Rule (SocketRuleMethod): {generation, identity, local port
/// | remote port << 16, remote address}. Installs a rule for the identity, which in v0 must be
/// zero, the sender itself (the ResourceDomain identity the kernel stamps on the delivery); a
/// nonzero identity is reserved for the Network Broker's decision and refused. Accepted only when
/// the delivery's effective rights include Manage. A remote address or remote port of zero means
/// any; the local port must not be zero. - Bind (SocketBindMethod): {generation, port, 0, 0}: the
/// sender becomes the port's owner, under a rule of its own naming the port. One owner per port; a
/// repeated Bind by the owner is Done. - Send (SocketSendMethod): {generation, local port | remote
/// port << 16, remote address, length 1 to SocketInlineBytes}, then the datagram's bytes inline.
/// Answered once the frame left: Done (detail its length) after the driver's Done; Pending when the
/// destination's MAC address is unknown: nothing was sent, the stack broadcast an ARP request, and
/// the client resends the data later (bounded; each resend broadcasts ARP again); Refused when no
/// rule of the sender allows it (nothing is sent); Failed when the driver failed it. The stack
/// holds at most SocketMaximumRequests Sends; one that finds the stack's transmit page busy waits
/// for it, and is never refused for that. - Receive (SocketReceiveMethod): {generation, port, 0,
/// 0}: a credit for a port the sender bound, answered when a datagram for it arrives: Done (detail
/// its length, extra the source address | source port << 32) with the datagram inline. There is no
/// socket buffer: a datagram that finds no Receive of its port's owner outstanding, or that is
/// longer than SocketInlineBytes, is dropped and counted. Credits are filled oldest first. - Query
/// (SocketQueryMethod): {generation, 0, 0, 0}: Done (detail the MAC address, its first byte in bits
/// 0-7, extra the IPv4 address | prefix length << 32) with SocketDropCounts little-endian 64-bit
/// counts inline, at the SocketDrop* indices. A reply (SocketReplyMethod) is {transaction answered
/// (also its header's), kind, detail, extra}, kind SocketDone to SocketRestarted, then the inline
/// area. Which of its inline bytes count follows from the request it answers, which the client
/// knows by its transaction: a Receive's Done carries detail bytes of datagram, a Query's Done
/// SocketQueryInlineBytes of counts, every other reply none. The service zeroes the rest, but the
/// reader does not check a reply's padding (its words alone cannot tell a Query's answer from a
/// datagram's); it reads the whole area. - SocketDone: as above; Rule and Bind have detail and
/// extra zero, a Send's extra is zero. - SocketRefused: not executed; detail the reason
/// (SocketRefused*): beyond the client's credit, a transaction it has outstanding, or a full table
/// (credit); no rule of the sender allows the Send or Bind, or the port is another's (no rule); a
/// Receive on a port the sender does not own (not bound); a Rule without Manage or for another
/// identity (identity); a zero port where one is required (malformed). - SocketPending: a Send that
/// was not sent (above). - SocketFailed: a Send the driver failed: detail the driver's reply kind
/// (NetFrame.hpp), extra its detail; or one the kernel refused to pass to the driver: detail zero,
/// extra the kernel's error. - SocketRestarted: the generation addressed is not the running
/// instance's. Rules, bindings and outstanding Receives live and die with the stack instance; there
/// is no Unbind or Cancel in v0.
inline constexpr Foundation::UInt64 SocketInterfaceLow{0x4153'5452'534F'4301ULL};  // "ASTRSOC", version 1
inline constexpr Foundation::UInt64 SocketInterfaceHigh{0x5544'5049'5056'3405ULL}; // "UDPIPV4", 5
inline constexpr Foundation::UInt32 SocketRuleMethod{1U};
inline constexpr Foundation::UInt32 SocketBindMethod{2U};
inline constexpr Foundation::UInt32 SocketSendMethod{3U};
inline constexpr Foundation::UInt32 SocketReceiveMethod{4U};
inline constexpr Foundation::UInt32 SocketQueryMethod{5U};
inline constexpr Foundation::UInt32 SocketReplyMethod{6U};
/// A reply's kind.
inline constexpr Foundation::UInt64 SocketDone{1U};
inline constexpr Foundation::UInt64 SocketRefused{2U};
inline constexpr Foundation::UInt64 SocketPending{3U};
inline constexpr Foundation::UInt64 SocketFailed{4U};
inline constexpr Foundation::UInt64 SocketRestarted{5U};
/// A SocketRefused reply's detail.
inline constexpr Foundation::UInt64 SocketRefusedCredit{1U};
inline constexpr Foundation::UInt64 SocketRefusedNoRule{2U};
inline constexpr Foundation::UInt64 SocketRefusedNotBound{3U};
inline constexpr Foundation::UInt64 SocketRefusedIdentity{4U};
inline constexpr Foundation::UInt64 SocketRefusedMalformed{5U};
/// A Query's inline counts: datagrams and frames dropped, by reason. The
/// library's NotForUs, BadIcmpChecksum and Unauthorized drops; the
/// service's: a datagram with no Receive outstanding, a datagram longer than
/// SocketInlineBytes, a frame the stack generated (an ARP or echo reply, a
/// Pending Send's ARP request) that found its transmit page busy; then every
/// other reason of the library's, summed.
inline constexpr Foundation::Size SocketDropNotForUs{0U};
inline constexpr Foundation::Size SocketDropBadIcmp{1U};
inline constexpr Foundation::Size SocketDropUnauthorized{2U};
inline constexpr Foundation::Size SocketDropNoReceive{3U};
inline constexpr Foundation::Size SocketDropTooLong{4U};
inline constexpr Foundation::Size SocketDropNoTransmit{5U};
inline constexpr Foundation::Size SocketDropMalformed{6U};
inline constexpr Foundation::Size SocketDropCounts{7U};
inline constexpr Foundation::Size SocketPayloadWords{4U};
inline constexpr Foundation::Size SocketPayloadBytes{SocketPayloadWords << 3U};
inline constexpr Foundation::Size SocketInlineBytes{192U};
inline constexpr Foundation::Size SocketQueryInlineBytes{SocketDropCounts << 3U};
/// The credit of a client (two Receives and two other requests outstanding;
/// Rule, Bind and Query are answered at once) and the per-sender capacity of
/// its reply Endpoint: every answer its credit allows, and one refusal.
inline constexpr Foundation::Size SocketMaximumReceives{2U};
inline constexpr Foundation::Size SocketMaximumRequests{2U};
inline constexpr Foundation::Size SocketReplyCapacity{SocketMaximumReceives + SocketMaximumRequests + 1U};
/// The per-sender capacity of the stack's own driver-reply Endpoint: its two
/// receive credits, its two frames in flight and one more (the driver's
/// Endpoint admits four messages from it, NetFrame.hpp).
inline constexpr Foundation::Size SocketDriverReplyCapacity{5U};
/// Every frame's payload: four words and the inline area.
inline constexpr Foundation::Size SocketFramePayloadBytes{SocketPayloadBytes + SocketInlineBytes};
/// Every request: header, reply attachment, payload.
inline constexpr Foundation::Size SocketRequestBytes{EchoHeaderBytes + EchoAttachmentBytes + SocketFramePayloadBytes};
/// A reply: header and payload.
inline constexpr Foundation::Size SocketReplyBytes{EchoHeaderBytes + SocketFramePayloadBytes};
/// The largest frame of the protocol.
inline constexpr Foundation::Size SocketMaximumFrameBytes{SocketRequestBytes};
static_assert(SocketFramePayloadBytes <= 255U, "the payload size fits one byte of each size field");
static_assert(SocketQueryInlineBytes <= SocketInlineBytes);
static_assert(SocketMaximumFrameBytes <= EchoMaximumFrameBytes);
/// The sealed selector of the stack's Endpoint and the tags of that Endpoint and of its
/// driver-reply Endpoint in its WaitSet.
inline constexpr Foundation::UInt64 SocketSealedSelector{0x534F'434B'5345'414CULL};
inline constexpr Foundation::UInt64 SocketServiceWaitTag{0x534F'4353U};
inline constexpr Foundation::UInt64 SocketDriverReplyWaitTag{0x534F'434EU};

/// One decoded socket frame.
struct SocketMessage
{
    Foundation::UInt64 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[SocketPayloadWords];
    /// The inline area as it arrived. A request's InlineLength bytes are its
    /// Method's (a Send's datagram), the rest zero; a reply's InlineLength is the whole area, of
    /// which the request it answers decides what counts.
    Foundation::UInt8 Inline[SocketInlineBytes];
    Foundation::Size InlineLength;
    /// A request's reply capability, as the receiver's space holds it.
    System::CapabilityHandle Reply;
};

namespace Detail
{
inline void StoreSocketWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] inline Foundation::UInt64 LoadSocketWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}

[[nodiscard]] inline bool IsSocketMethod(const Foundation::UInt64 method) noexcept
{
    return (method >= SocketRuleMethod) && (method <= SocketReplyMethod);
}

/// The inline bytes the reader takes as a frame of method's: a Send's
/// length (checked by AreSocketWords), none for the other requests, and the
/// whole area for a reply, whose padding it does not check.
[[nodiscard]] inline Foundation::UInt64
SocketInlineLength(const Foundation::UInt64 method, const Foundation::UInt64 (&words)[SocketPayloadWords]) noexcept
{
    if (method == SocketSendMethod)
    {
        return words[3U];
    }
    return (method == SocketReplyMethod) ? SocketInlineBytes : 0U;
}

/// The words of a request of method are in their fields: no bit beyond a
/// port or an address, and zero reserved words. A port's value is the
/// service's to judge.
[[nodiscard]] inline bool AreSocketWords(const Foundation::UInt64 method,
                                         const Foundation::UInt64 (&words)[SocketPayloadWords]) noexcept
{
    constexpr Foundation::UInt64 address{0xFFFF'FFFFU};
    switch (method)
    {
    case SocketRuleMethod:
        return (words[2U] <= address) && (words[3U] <= address);
    case SocketBindMethod:
    case SocketReceiveMethod:
        return (words[1U] <= 0xFFFFU) && (words[2U] == 0U) && (words[3U] == 0U);
    case SocketSendMethod:
        return (words[1U] <= address) && (words[2U] <= address) && (words[3U] >= 1U) &&
               (words[3U] <= SocketInlineBytes);
    case SocketQueryMethod:
        return (words[1U] == 0U) && (words[2U] == 0U) && (words[3U] == 0U);
    default:
        break;
    }
    return false;
}
} // namespace Detail

/// Writes a socket frame of method into frame (SocketMaximumFrameBytes at
/// most): a request with its reply Endpoint attachment (Invoke, copied), a
/// reply with none (reply is ignored), then words and the inline area: the
/// first inlineLength bytes of inlineBytes, then zeros. The words and length
/// are written as given; the reader refuses a frame whose inline bytes go past
/// what its words give. Returns the frame's size, or zero, writing nothing,
/// For an unknown method, a request without reply, or inline bytes beyond SocketInlineBytes or not
/// given.
inline Foundation::Size
WriteSocketFrame(Foundation::UInt8* const frame, const Foundation::UInt32 method, const Foundation::UInt64 transaction,
                 const Foundation::UInt64 (&words)[SocketPayloadWords], const Foundation::UInt8* const inlineBytes,
                 const Foundation::Size inlineLength, const System::CapabilityHandle* const reply) noexcept
{
    const bool request = method != SocketReplyMethod;
    if (!Detail::IsSocketMethod(method) || (request && (reply == nullptr)) || (inlineLength > SocketInlineBytes) ||
        ((inlineBytes == nullptr) && (inlineLength != 0U)))
    {
        return 0U;
    }
    const Foundation::Size attachments = request ? 1U : 0U;
    EchoAttachment record{};
    if (request)
    {
        record =
            EchoAttachment{.Handle = *reply, .Rights = EchoInvokeRight, .Mode = EchoCopy, .Type = EchoEndpointType};
    }
    WriteEchoFrame(frame, &record, attachments);
    const Foundation::Size total = EchoHeaderBytes + (attachments << 5U) + SocketFramePayloadBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(total >> (index << 3U));
    }
    frame[12U] = static_cast<Foundation::UInt8>(SocketFramePayloadBytes);
    frame[13U] = 0U;
    frame[14U] = static_cast<Foundation::UInt8>(SocketFramePayloadBytes);
    frame[15U] = 0U;
    Detail::StoreSocketWord(frame + 16U, SocketInterfaceLow);
    Detail::StoreSocketWord(frame + 24U, SocketInterfaceHigh);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[32U + index] = static_cast<Foundation::UInt8>(method >> (index << 3U));
    }
    Detail::StoreSocketWord(frame + 40U, transaction);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    for (Foundation::Size word{0U}; word < SocketPayloadWords; ++word)
    {
        Detail::StoreSocketWord(payload + (word << 3U), words[word]);
    }
    for (Foundation::Size index{0U}; index < SocketInlineBytes; ++index)
    {
        payload[SocketPayloadBytes + index] = (index < inlineLength) ? inlineBytes[index] : Foundation::UInt8{0U};
    }
    return total;
}

/// Decodes the frame of a delivery of deliveryBytes (its 48-byte header
/// first): false, leaving message unchanged, unless it is a socket frame of
/// its method's exact size and attachment count (an Endpoint attachment on a
/// request, none on a reply) whose words are in their fields (a Send's length
/// 1 to SocketInlineBytes, a reply's first word its header's transaction and
/// its kind SocketDone to SocketRestarted) and whose inline area is zero past
/// the bytes its words give. Attachment rights are the kernel's to enforce, as
/// in FsFrame.hpp.
[[nodiscard]] inline bool ReadSocketFrame(const Foundation::UInt8* const delivery, const Foundation::Size deliveryBytes,
                                          SocketMessage& message) noexcept
{
    if (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::UInt64 method = frame[32U] | (static_cast<Foundation::UInt64>(frame[33U]) << 8U);
    const Foundation::Size expected = (method == SocketReplyMethod) ? 0U : 1U;
    const Foundation::Size attachments = frame[81U];
    if ((Detail::LoadSocketWord(frame + 16U) != SocketInterfaceLow) ||
        (Detail::LoadSocketWord(frame + 24U) != SocketInterfaceHigh) || (frame[34U] != 0U) || (frame[35U] != 0U) ||
        !Detail::IsSocketMethod(method) || (frame[80U] != 0U) || (attachments != expected) ||
        (frame[12U] != SocketFramePayloadBytes) || (frame[13U] != 0U) ||
        (deliveryBytes !=
         (EchoDeliveryHeaderBytes + EchoHeaderBytes + (attachments << 5U) + SocketFramePayloadBytes)) ||
        ((attachments == 1U) && (frame[EchoHeaderBytes + 27U] != EchoEndpointType)))
    {
        return false;
    }
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    Foundation::UInt64 words[SocketPayloadWords]{};
    for (Foundation::Size word{0U}; word < SocketPayloadWords; ++word)
    {
        words[word] = Detail::LoadSocketWord(payload + (word << 3U));
    }
    const Foundation::UInt64 transaction = Detail::LoadSocketWord(frame + 40U);
    bool valid = (method == SocketReplyMethod)
                     ? ((words[0U] == transaction) && (words[1U] >= SocketDone) && (words[1U] <= SocketRestarted))
                     : Detail::AreSocketWords(method, words);
    const Foundation::UInt64 length = Detail::SocketInlineLength(method, words);
    const Foundation::UInt8* const area = payload + SocketPayloadBytes;
    for (Foundation::Size index{0U}; valid && (index < SocketInlineBytes); ++index)
    {
        valid = (index < length) || (area[index] == 0U);
    }
    if (!valid)
    {
        return false;
    }
    message.Method = method;
    message.Transaction = transaction;
    for (Foundation::Size word{0U}; word < SocketPayloadWords; ++word)
    {
        message.Words[word] = words[word];
    }
    for (Foundation::Size index{0U}; index < SocketInlineBytes; ++index)
    {
        message.Inline[index] = area[index];
    }
    message.InlineLength = static_cast<Foundation::Size>(length);
    message.Reply = (attachments == 1U) ? System::CapabilityHandle{Detail::LoadSocketWord(frame + EchoHeaderBytes),
                                                                   Detail::LoadSocketWord(frame + EchoHeaderBytes + 8U)}
                                        : System::CapabilityHandle{};
    return true;
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_SOCKET_FRAME_HPP_INCLUDED
