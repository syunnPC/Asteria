// SPDX-License-Identifier: Apache-2.0

#include "User/Component/NetworkService.hpp"

#include "Abi/SystemCallAbi.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/NetFrame.hpp"
#include "User/Component/ServiceCapabilities.hpp"
#include "User/Component/SocketFrame.hpp"
#include "User/Network/NetworkStack.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Component
{
namespace
{
using Abi::SystemCallError;
using System::CapabilityHandle;

constexpr Foundation::UInt64 PageBytes{4096U};
// The window (Source/User/Network/NetworkStack.hpp stack): two receive pages, the transmit page of
// the frames the stack generates, the transmit page of clients' Sends.
constexpr Foundation::Size WindowPages{4U};
constexpr Foundation::Size ReceivePages{2U};
constexpr Foundation::Size GeneratedPage{2U};
constexpr Foundation::Size ClientPage{3U};
// A delivery's header (IpcDeliveryCodec): the effective rights of the
// capability the sender invoked, and the sender's ResourceDomainId, which
// the kernel writes and the sender cannot choose.
constexpr Foundation::Size DeliveryRightsOffset{14U};
constexpr Foundation::Size DeliverySenderOffset{24U};
// How long a wait lasts while a request the driver's Endpoint refused with
// WouldBlock waits to be retried, or while the bring-up awaits the driver's
// Query; and how many such timeouts the bring-up takes (a minute).
constexpr Foundation::UInt64 RetryMicroseconds{10'000U};
constexpr Foundation::UInt64 BringUpTimeouts{6'000U};
// The Receives the stack holds: two for each of four clients.
constexpr Foundation::Size ReceiveCapacity{8U};
constexpr Foundation::Size SendCapacity{SocketMaximumRequests};
constexpr Foundation::UInt64 PortMask{0xFFFFU};

// A window page's request to the driver: none, built but not yet accepted
// by the kernel (the driver's Endpoint refused it with WouldBlock), or
// accepted and awaiting the driver's reply.
enum class PageState : Foundation::UInt8
{
    Free,
    Unsent,
    Posted,
};

struct WindowPage
{
    CapabilityHandle Frame;
    Foundation::UInt64 Transaction;
    // A transmit page's frame length.
    Foundation::UInt64 Length;
    PageState State;
};

// A client's Send: held (its bytes kept) until the client page frees, or
// sending (its frame on that page, awaiting the driver).
enum class SendState : Foundation::UInt8
{
    Free,
    Held,
    Sending,
};

struct ClientSend
{
    Foundation::UInt64 Sender;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Arrival;
    CapabilityHandle Reply;
    Foundation::UInt64 RemoteAddress;
    Foundation::UInt64 Length;
    Foundation::UInt16 LocalPort;
    Foundation::UInt16 RemotePort;
    SendState State;
    Foundation::UInt8 Bytes[SocketInlineBytes];
};

struct ClientReceive
{
    Foundation::UInt64 Sender;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Arrival;
    CapabilityHandle Reply;
    Foundation::UInt16 Port;
    bool Present;
};

// The instance: what its start block and image gave it, its pages, its
// counters and its end.
struct ServiceState
{
    const System::SystemCallClient* Client;
    NetworkPlatform Platform;
    CapabilityHandle Endpoint;
    CapabilityHandle WaitSet;
    CapabilityHandle Domain;
    CapabilityHandle Space;
    CapabilityHandle Driver;
    CapabilityHandle DriverReplies;
    Foundation::UInt64 Incarnation;
    Foundation::UInt64 DriverGeneration;
    // Word 37: the address in bits 0-31, the prefix length in bits 32-39.
    Foundation::UInt64 Configuration;
    // The driver's MAC address, its first byte in bits 0-7.
    Foundation::UInt64 Mac;
    // The last driver transaction used, zero before the first (the kernel's
    // codec refuses zero); an instance does not send 2^64 requests.
    Foundation::UInt64 Transaction;
    // Orders the clients' requests (64 bits do not wrap).
    Foundation::UInt64 Arrival;
    // The service's drops (SocketFrame.hpp): a datagram with no Receive, one too long, a generated
    // frame with no transmit page; and replies the kernel refused (a client that broke its credit),
    // dropped.
    Foundation::UInt64 NoReceive;
    Foundation::UInt64 TooLong;
    Foundation::UInt64 NoTransmit;
    Foundation::UInt64 DroppedReplies;
    // Nonzero once the instance must end.
    Foundation::UInt64 Exit;
    WindowPage Pages[WindowPages];
    // The kernel's error for the last request to the driver it refused other
    // than with WouldBlock.
    SystemCallError Refusal;
};

// The instance's state, its stack and the buffers it receives into and
// replies from: static, not on its stack, zero-initialized (the image's
// data stays empty) and inside its image's own mappings, as the kernel's
// record-backed access requires.
constinit ServiceState g_state{};
constinit ClientSend g_sends[SendCapacity]{};
constinit ClientReceive g_receives[ReceiveCapacity]{};
constinit Network::NetworkStack g_stack{};
constinit Network::ReceiveOutput g_output{};
constinit Network::TransmitFrame g_transmit{};
constinit SocketMessage g_message{};
alignas(8) constinit Foundation::UInt8 g_request[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};
alignas(8) constinit Foundation::UInt8 g_driverReply[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};
alignas(8) constinit Foundation::UInt8 g_reply[SocketReplyBytes]{};

[[nodiscard]] constexpr Foundation::UInt64 NetworkFailure(const Foundation::UInt64 step,
                                                          const SystemCallError error) noexcept
{
    return Abi::ComponentNetworkFailedExitCode | (step << 32U) | (static_cast<Foundation::UInt64>(error) << 40U);
}

// Records the instance's first failure; later ones keep it.
void End(const Foundation::UInt64 exit) noexcept
{
    g_state.Exit = (g_state.Exit == 0U) ? exit : g_state.Exit;
}

// A send to the driver that names an Endpoint no longer there: the driver
// is gone.
[[nodiscard]] constexpr bool IsDriverGone(const SystemCallError error) noexcept
{
    return (error == SystemCallError::ObjectNotFound) || (error == SystemCallError::InvalidHandle) ||
           (error == SystemCallError::Revoked);
}

void Count(Foundation::UInt64& count) noexcept
{
    // Diagnostic counts saturate, as the library's do.
    count = (count == ~Foundation::UInt64{0U}) ? count : (count + 1U);
}

[[nodiscard]] Foundation::UInt8* PageBytesOf(const Foundation::Size page) noexcept
{
    return reinterpret_cast<Foundation::UInt8*>(
        static_cast<Foundation::UIntPointer>(g_state.Platform.Window + (page * PageBytes)));
}

void CopyBytes(Foundation::UInt8* const target, const Foundation::UInt8* const source,
               const Foundation::Size bytes) noexcept
{
    for (Foundation::Size index{0U}; index < bytes; ++index)
    {
        target[index] = source[index];
    }
}

// Sends the reply of kind, detail and extra, with length inline bytes,
// answering transaction on reply, then closes reply. A reply the kernel
// refuses is dropped and counted; a Close that fails other than on a stale
// handle ends the instance.
[[gnu::noinline]] void Respond(const CapabilityHandle reply, const Foundation::UInt64 transaction,
                               const Foundation::UInt64 kind, const Foundation::UInt64 detail,
                               const Foundation::UInt64 extra, const Foundation::UInt8* const bytes = nullptr,
                               const Foundation::Size length = 0U) noexcept
{
    const System::SystemCallClient& client = *g_state.Client;
    const Foundation::UInt64 words[SocketPayloadWords]{transaction, kind, detail, extra};
    const Foundation::Size size =
        WriteSocketFrame(g_reply, SocketReplyMethod, transaction, words, bytes, length, nullptr);
    if (client.Send(reply, System::AddressOf(g_reply), size).Error != SystemCallError::None)
    {
        Count(g_state.DroppedReplies);
    }
    if (!Closed(client, reply))
    {
        End(NetworkFailure(Abi::ComponentNetworkCloseStep, SystemCallError::None));
    }
}

// What becomes of a request to the driver: accepted, refused with
// WouldBlock (kept, to be retried), or refused otherwise (the error in
// g_state.Refusal; the instance has ended if the driver is gone).
enum class Posting : Foundation::UInt8
{
    Accepted,
    Blocked,
    Refused,
};

// Sends page's request to the driver: a receive page's ReceiveCredit, a
// transmit page's Send of its Length bytes; with a copy of the driver-reply
// Endpoint (Invoke) and a Map-only copy of the page's Frame, under the
// page's transaction.
[[nodiscard, gnu::noinline]] Posting Post(const Foundation::Size page) noexcept
{
    WindowPage& entry = g_state.Pages[page];
    const bool receive = page < ReceivePages;
    alignas(8) Foundation::UInt8 request[NicDataRequestBytes]{};
    const Foundation::UInt64 words[NicPayloadWords]{g_state.DriverGeneration, receive ? 0U : entry.Length, 0U, 0U};
    const Foundation::Size bytes = WriteNicFrame(request, receive ? NicReceiveMethod : NicSendMethod, entry.Transaction,
                                                 words, &g_state.DriverReplies, &entry.Frame);
    const SystemCallError error = g_state.Client->Send(g_state.Driver, System::AddressOf(request), bytes).Error;
    if (error == SystemCallError::None)
    {
        entry.State = PageState::Posted;
        return Posting::Accepted;
    }
    if (error == SystemCallError::WouldBlock)
    {
        return Posting::Blocked;
    }
    g_state.Refusal = error;
    if (IsDriverGone(error))
    {
        End(NetworkFailure(Abi::ComponentNetworkDriverStep, error));
    }
    return Posting::Refused;
}

// The oldest Send in state, or SendCapacity.
[[nodiscard]] Foundation::Size OldestSend(const SendState state) noexcept
{
    Foundation::Size oldest{SendCapacity};
    for (Foundation::Size index{0U}; index < SendCapacity; ++index)
    {
        if ((g_sends[index].State == state) &&
            ((oldest == SendCapacity) || (g_sends[index].Arrival < g_sends[oldest].Arrival)))
        {
            oldest = index;
        }
    }
    return oldest;
}

// The client Send on the client page is over: answered Done with its length,
// or Failed with the driver's kind and detail, or with the kernel's error.
void FinishSend(const Foundation::UInt64 kind, const Foundation::UInt64 detail) noexcept
{
    g_state.Pages[ClientPage].State = PageState::Free;
    const Foundation::Size index = OldestSend(SendState::Sending);
    if (index == SendCapacity)
    {
        return;
    }
    ClientSend& send = g_sends[index];
    send.State = SendState::Free;
    if (kind == NicDone)
    {
        Respond(send.Reply, send.Transaction, SocketDone, send.Length, 0U);
        return;
    }
    Respond(send.Reply, send.Transaction, SocketFailed, kind, detail);
}

// Sends page's request to the driver and settles a refusal other than
// WouldBlock: a receive page's ends the instance, a generated frame is
// dropped and counted, a client's Send is answered Failed with the error.
void Submit(const Foundation::Size page) noexcept
{
    if (Post(page) != Posting::Refused)
    {
        return;
    }
    if (page < ReceivePages)
    {
        End(NetworkFailure(Abi::ComponentNetworkDriverStep, g_state.Refusal));
        return;
    }
    if (page == GeneratedPage)
    {
        g_state.Pages[GeneratedPage].State = PageState::Free;
        Count(g_state.NoTransmit);
        return;
    }
    FinishSend(0U, static_cast<Foundation::UInt64>(g_state.Refusal));
}

// Posts a receive page's credit again under a new transaction.
void Credit(const Foundation::Size page) noexcept
{
    g_state.Pages[page].Transaction = ++g_state.Transaction;
    g_state.Pages[page].State = PageState::Unsent;
    Submit(page);
}

// A frame the stack generated (an ARP or echo reply, a Pending Send's ARP
// request) through the generated page, or dropped and counted while that
// page is busy.
[[gnu::noinline]] void Transmit(const Network::TransmitFrame& frame) noexcept
{
    if (frame.Length == 0U)
    {
        return;
    }
    WindowPage& page = g_state.Pages[GeneratedPage];
    if (page.State != PageState::Free)
    {
        Count(g_state.NoTransmit);
        return;
    }
    CopyBytes(PageBytesOf(GeneratedPage), frame.Bytes, frame.Length);
    page.Length = frame.Length;
    page.Transaction = ++g_state.Transaction;
    page.State = PageState::Unsent;
    Submit(GeneratedPage);
}

// Starts the held Sends, oldest first, while the client page is free: the
// stack builds each frame now (a held Send kept its bytes only). Sent takes
// the page until the driver's reply; a peer that left the ARP table
// meanwhile makes it Pending, its ARP request sent through the generated
// page and the client answered at once (Refused cannot follow a Send that
// was sendable: rules are never removed, but it is answered alike).
[[gnu::noinline]] void StartSends() noexcept
{
    while ((g_state.Exit == 0U) && (g_state.Pages[ClientPage].State == PageState::Free))
    {
        const Foundation::Size index = OldestSend(SendState::Held);
        if (index == SendCapacity)
        {
            return;
        }
        ClientSend& send = g_sends[index];
        const Network::SendStatus status =
            g_stack.SendTo(send.Sender, send.LocalPort, static_cast<Network::Ipv4Address>(send.RemoteAddress),
                           send.RemotePort, send.Bytes, static_cast<Foundation::Size>(send.Length), g_transmit);
        if (status == Network::SendStatus::Sent)
        {
            WindowPage& page = g_state.Pages[ClientPage];
            CopyBytes(PageBytesOf(ClientPage), g_transmit.Bytes, g_transmit.Length);
            page.Length = g_transmit.Length;
            page.Transaction = ++g_state.Transaction;
            page.State = PageState::Unsent;
            send.State = SendState::Sending;
            Submit(ClientPage);
            continue;
        }
        send.State = SendState::Free;
        if (status == Network::SendStatus::Pending)
        {
            Transmit(g_transmit);
            Respond(send.Reply, send.Transaction, SocketPending, 0U, 0U);
            continue;
        }
        Respond(send.Reply, send.Transaction, SocketRefused, SocketRefusedNoRule, 0U);
    }
}

// A datagram the stack delivered: copied into the reply to the oldest
// Receive its owner has outstanding for its port. There is no socket buffer:
// one with no Receive, or longer than the inline area, is dropped and
// counted.
[[gnu::noinline]] void Deliver(const Network::DeliveredDatagram& datagram) noexcept
{
    if (!datagram.Present)
    {
        return;
    }
    if (datagram.Payload.Length > SocketInlineBytes)
    {
        Count(g_state.TooLong);
        return;
    }
    Foundation::Size oldest{ReceiveCapacity};
    for (Foundation::Size index{0U}; index < ReceiveCapacity; ++index)
    {
        const ClientReceive& entry = g_receives[index];
        if (entry.Present && (entry.Sender == datagram.Owner) && (entry.Port == datagram.LocalPort) &&
            ((oldest == ReceiveCapacity) || (entry.Arrival < g_receives[oldest].Arrival)))
        {
            oldest = index;
        }
    }
    if (oldest == ReceiveCapacity)
    {
        Count(g_state.NoReceive);
        return;
    }
    ClientReceive& receive = g_receives[oldest];
    receive.Present = false;
    const Foundation::UInt64 source = static_cast<Foundation::UInt64>(datagram.SourceAddress) |
                                      (static_cast<Foundation::UInt64>(datagram.SourcePort) << 32U);
    Respond(receive.Reply, receive.Transaction, SocketDone, datagram.Payload.Length, source, datagram.Payload.Data,
            datagram.Payload.Length);
}

// The driver's reply for a receive page: a frame received goes through the
// stack (its output frame transmitted, its datagram delivered, before the
// page is lent again), then the credit is posted again. A credit given back
// unfilled is posted again; one the driver failed or refused is not (its
// Frame or device is unusable, and posting it again would only repeat that).
[[gnu::noinline]] void Received(const Foundation::Size page, const Foundation::UInt64 kind,
                                const Foundation::UInt64 detail) noexcept
{
    g_state.Pages[page].State = PageState::Free;
    if (kind == NicDone)
    {
        const Foundation::Size length = static_cast<Foundation::Size>((detail > PageBytes) ? PageBytes : detail);
        g_stack.Receive(PageBytesOf(page), length, g_output);
        Transmit(g_output.Frame);
        Deliver(g_output.Datagram);
    }
    if (((kind == NicDone) || (kind == NicCancelled)) && (g_state.Exit == 0U))
    {
        Credit(page);
    }
}

// One delivery on the driver-reply Endpoint: a NIC reply answers the page
// whose request carried its transaction (a stale one is dropped); anything
// else is dropped with its capabilities closed. A NicDriverRestarted reply
// ends the instance.
[[gnu::noinline]] void ServeDriverReply(const Foundation::Size deliveryBytes) noexcept
{
    NicMessage reply{};
    if (!ReadNicFrame(g_driverReply, deliveryBytes, reply) || (reply.Method != NicReplyMethod))
    {
        if (!CloseAttachments(*g_state.Client, g_driverReply, 0U))
        {
            End(NetworkFailure(Abi::ComponentNetworkCloseStep, SystemCallError::None));
        }
        return;
    }
    const Foundation::UInt64 kind = reply.Words[1U];
    if (kind == NicDriverRestarted)
    {
        End(NetworkFailure(Abi::ComponentNetworkDriverStep, SystemCallError::None));
        return;
    }
    for (Foundation::Size page{0U}; page < WindowPages; ++page)
    {
        if ((g_state.Pages[page].State != PageState::Posted) || (g_state.Pages[page].Transaction != reply.Words[0U]))
        {
            continue;
        }
        if (page < ReceivePages)
        {
            Received(page, kind, reply.Words[2U]);
        }
        else if (page == GeneratedPage)
        {
            g_state.Pages[GeneratedPage].State = PageState::Free;
        }
        else
        {
            FinishSend(kind, reply.Words[2U]);
            StartSends();
        }
        return;
    }
}

// What a sender has outstanding: its Receives and Sends, whether one has
// transaction, and a free entry of each table (or its capacity).
struct Outstanding
{
    Foundation::Size Receives;
    Foundation::Size Sends;
    Foundation::Size FreeReceive;
    Foundation::Size FreeSend;
    bool Duplicate;
};

[[nodiscard]] Outstanding Inspect(const Foundation::UInt64 sender, const Foundation::UInt64 transaction) noexcept
{
    Outstanding held{
        .Receives = 0U, .Sends = 0U, .FreeReceive = ReceiveCapacity, .FreeSend = SendCapacity, .Duplicate = false};
    for (Foundation::Size index{0U}; index < ReceiveCapacity; ++index)
    {
        const ClientReceive& entry = g_receives[index];
        if (!entry.Present)
        {
            held.FreeReceive = index;
        }
        else if (entry.Sender == sender)
        {
            ++held.Receives;
            held.Duplicate = held.Duplicate || (entry.Transaction == transaction);
        }
    }
    for (Foundation::Size index{0U}; index < SendCapacity; ++index)
    {
        const ClientSend& entry = g_sends[index];
        if (entry.State == SendState::Free)
        {
            held.FreeSend = index;
        }
        else if (entry.Sender == sender)
        {
            ++held.Sends;
            held.Duplicate = held.Duplicate || (entry.Transaction == transaction);
        }
    }
    return held;
}

void Refuse(const SocketMessage& message, const Foundation::UInt64 reason) noexcept
{
    Respond(message.Reply, message.Transaction, SocketRefused, reason, 0U);
}

void Done(const SocketMessage& message) noexcept
{
    Respond(message.Reply, message.Transaction, SocketDone, 0U, 0U);
}

// A rule for the sender itself (identity zero), from a holder of Manage on the stack's Endpoint.
void ServeRule(const SocketMessage& message, const Foundation::UInt64 sender, const Foundation::UInt16 rights) noexcept
{
    if (((rights & EchoManageRight) == 0U) || (message.Words[1U] != 0U))
    {
        Refuse(message, SocketRefusedIdentity);
        return;
    }
    const Network::NetworkRule rule{.Sender = sender,
                                    .LocalPort = static_cast<Foundation::UInt16>(message.Words[2U] & PortMask),
                                    .RemoteAddress = static_cast<Network::Ipv4Address>(message.Words[3U]),
                                    .RemotePort =
                                        static_cast<Foundation::UInt16>((message.Words[2U] >> 16U) & PortMask)};
    if (rule.LocalPort == 0U)
    {
        Refuse(message, SocketRefusedMalformed);
        return;
    }
    if (!g_stack.InstallRule(rule))
    {
        Refuse(message, SocketRefusedCredit);
        return;
    }
    Done(message);
}

void ServeBind(const SocketMessage& message, const Foundation::UInt64 sender) noexcept
{
    const Foundation::UInt16 port = static_cast<Foundation::UInt16>(message.Words[1U]);
    if (port == 0U)
    {
        Refuse(message, SocketRefusedMalformed);
        return;
    }
    if (!g_stack.Bind(sender, port))
    {
        Refuse(message, SocketRefusedNoRule);
        return;
    }
    Done(message);
}

// A Send within the sender's credit: one no rule of the sender allows is
// refused, one to a peer whose MAC address is unknown is Pending after the
// stack's ARP request, both at once; one the stack can send is held, its
// bytes copied, and started at once when the client page is free (the
// stack builds its frame again then: SendTo has no effect but its output).
[[gnu::noinline]] void ServeSend(const SocketMessage& message, const Foundation::UInt64 sender) noexcept
{
    const Foundation::UInt16 localPort = static_cast<Foundation::UInt16>(message.Words[1U] & PortMask);
    const Foundation::UInt16 remotePort = static_cast<Foundation::UInt16>((message.Words[1U] >> 16U) & PortMask);
    if ((localPort == 0U) || (remotePort == 0U))
    {
        Refuse(message, SocketRefusedMalformed);
        return;
    }
    const Outstanding held = Inspect(sender, message.Transaction);
    if (held.Duplicate || (held.Sends >= SocketMaximumRequests) || (held.FreeSend == SendCapacity))
    {
        Refuse(message, SocketRefusedCredit);
        return;
    }
    const Network::SendStatus status =
        g_stack.SendTo(sender, localPort, static_cast<Network::Ipv4Address>(message.Words[2U]), remotePort,
                       message.Inline, message.InlineLength, g_transmit);
    if (status == Network::SendStatus::Refused)
    {
        Refuse(message, SocketRefusedNoRule);
        return;
    }
    if (status == Network::SendStatus::Pending)
    {
        Transmit(g_transmit);
        Respond(message.Reply, message.Transaction, SocketPending, 0U, 0U);
        return;
    }
    ClientSend& send = g_sends[held.FreeSend];
    send.Sender = sender;
    send.Transaction = message.Transaction;
    send.Arrival = ++g_state.Arrival;
    send.Reply = message.Reply;
    send.RemoteAddress = message.Words[2U];
    send.Length = message.Words[3U];
    send.LocalPort = localPort;
    send.RemotePort = remotePort;
    send.State = SendState::Held;
    CopyBytes(send.Bytes, message.Inline, message.InlineLength);
    StartSends();
}

void ServeReceive(const SocketMessage& message, const Foundation::UInt64 sender) noexcept
{
    const Foundation::UInt16 port = static_cast<Foundation::UInt16>(message.Words[1U]);
    if (port == 0U)
    {
        Refuse(message, SocketRefusedMalformed);
        return;
    }
    if (!g_stack.IsBound(sender, port))
    {
        Refuse(message, SocketRefusedNotBound);
        return;
    }
    const Outstanding held = Inspect(sender, message.Transaction);
    if (held.Duplicate || (held.Receives >= SocketMaximumReceives) || (held.FreeReceive == ReceiveCapacity))
    {
        Refuse(message, SocketRefusedCredit);
        return;
    }
    g_receives[held.FreeReceive] = ClientReceive{.Sender = sender,
                                                 .Transaction = message.Transaction,
                                                 .Arrival = ++g_state.Arrival,
                                                 .Reply = message.Reply,
                                                 .Port = port,
                                                 .Present = true};
}

[[nodiscard]] Foundation::UInt64 Saturate(const Foundation::UInt64 sum, const Foundation::UInt64 count) noexcept
{
    return ((sum + count) < sum) ? ~Foundation::UInt64{0U} : (sum + count);
}

// Done with the MAC address, the configuration and the drop counts at their
// protocol indices (SocketFrame.hpp).
[[gnu::noinline]] void ServeQuery(const SocketMessage& message) noexcept
{
    Foundation::UInt64 counts[SocketDropCounts]{};
    counts[SocketDropNotForUs] = g_stack.GetDropCount(Network::DropReason::NotForUs);
    counts[SocketDropBadIcmp] = g_stack.GetDropCount(Network::DropReason::BadIcmpChecksum);
    counts[SocketDropUnauthorized] = g_stack.GetDropCount(Network::DropReason::Unauthorized);
    counts[SocketDropNoReceive] = g_state.NoReceive;
    counts[SocketDropTooLong] = g_state.TooLong;
    counts[SocketDropNoTransmit] = g_state.NoTransmit;
    for (Foundation::Size reason{0U}; reason < static_cast<Foundation::Size>(Network::DropReason::Count); ++reason)
    {
        const Network::DropReason named = static_cast<Network::DropReason>(reason);
        if ((named != Network::DropReason::NotForUs) && (named != Network::DropReason::BadIcmpChecksum) &&
            (named != Network::DropReason::Unauthorized))
        {
            counts[SocketDropMalformed] = Saturate(counts[SocketDropMalformed], g_stack.GetDropCount(named));
        }
    }
    Foundation::UInt8 bytes[SocketQueryInlineBytes]{};
    for (Foundation::Size index{0U}; index < SocketDropCounts; ++index)
    {
        System::StoreWord(bytes + (index << 3U), counts[index]);
    }
    Respond(message.Reply, message.Transaction, SocketDone, g_state.Mac, g_state.Configuration, bytes,
            SocketQueryInlineBytes);
}

// One delivery on the service Endpoint. A frame that is no request of the
// socket protocol through the sealed Endpoint is dropped, every capability
// it carried closed: its reply capability cannot be trusted.
[[gnu::noinline]] void ServeRequest(const Foundation::Size deliveryBytes) noexcept
{
    SocketMessage& message = g_message;
    if (!ReadSocketFrame(g_request, deliveryBytes, message) || (message.Method == SocketReplyMethod) ||
        (ReadEchoSealedSelector(g_request) != SocketSealedSelector))
    {
        if (!CloseAttachments(*g_state.Client, g_request, 0U))
        {
            End(NetworkFailure(Abi::ComponentNetworkCloseStep, SystemCallError::None));
        }
        return;
    }
    if (message.Words[0U] != g_state.Incarnation)
    {
        Respond(message.Reply, message.Transaction, SocketRestarted, 0U, 0U);
        return;
    }
    const Foundation::UInt64 sender = System::LoadWord(g_request + DeliverySenderOffset);
    const Foundation::UInt16 rights =
        static_cast<Foundation::UInt16>(g_request[DeliveryRightsOffset] |
                                        (static_cast<Foundation::UInt16>(g_request[DeliveryRightsOffset + 1U]) << 8U));
    // Rule, Bind and Query are answered at once and hold nothing, but a
    // transaction the sender has outstanding is still one too many.
    if ((message.Method != SocketSendMethod) && (message.Method != SocketReceiveMethod) &&
        Inspect(sender, message.Transaction).Duplicate)
    {
        Refuse(message, SocketRefusedCredit);
        return;
    }
    switch (message.Method)
    {
    case SocketRuleMethod:
        ServeRule(message, sender, rights);
        return;
    case SocketBindMethod:
        ServeBind(message, sender);
        return;
    case SocketSendMethod:
        ServeSend(message, sender);
        return;
    case SocketReceiveMethod:
        ServeReceive(message, sender);
        return;
    default:
        break;
    }
    ServeQuery(message);
}

// Receives and serves every delivery endpoint holds, in order, into buffer
// with serve; stops at WouldBlock or the instance's end.
void Drain(const CapabilityHandle endpoint, Foundation::UInt8* const buffer, const Foundation::Size capacity,
           void (*const serve)(Foundation::Size) noexcept) noexcept
{
    while (g_state.Exit == 0U)
    {
        const Abi::SystemCallResponse received = g_state.Client->Receive(endpoint, System::AddressOf(buffer), capacity);
        if (received.Error == SystemCallError::WouldBlock)
        {
            return;
        }
        if (received.Error != SystemCallError::None)
        {
            End(NetworkFailure(Abi::ComponentNetworkReceiveStep, received.Error));
            return;
        }
        serve(static_cast<Foundation::Size>(received.Value1));
    }
}

// Retries every request the driver's Endpoint refused with WouldBlock, in
// page order, until it refuses one again, then starts the held Sends a
// refusal let through; true while a request is still kept.
[[nodiscard, gnu::noinline]] bool Retry() noexcept
{
    for (Foundation::Size page{0U}; (page < WindowPages) && (g_state.Exit == 0U); ++page)
    {
        if (g_state.Pages[page].State != PageState::Unsent)
        {
            continue;
        }
        Submit(page);
        if (g_state.Pages[page].State == PageState::Unsent)
        {
            break;
        }
    }
    StartSends();
    bool kept{false};
    for (const WindowPage& page : g_state.Pages)
    {
        kept = kept || (page.State == PageState::Unsent);
    }
    return kept;
}

// The window (four pages): four one-page Frames of the instance's own
// domain, each mapped through a MemoryObject of its own (the driver maps one
// page per request, NetFrame.hpp). Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 PrepareWindow() noexcept
{
    const System::SystemCallClient& client = *g_state.Client;
    for (Foundation::Size page{0U}; page < WindowPages; ++page)
    {
        const Abi::SystemCallResponse frame = System::CreateMemory(client, g_state.Domain, Abi::ObjectTypeFrame, 1U);
        if (frame.Error != SystemCallError::None)
        {
            return NetworkFailure(Abi::ComponentNetworkFrameStep, frame.Error);
        }
        g_state.Pages[page].Frame = CapabilityHandle{frame.Value0, frame.Value1};
        const Abi::SystemCallResponse memory =
            System::CreateMemory(client, g_state.Domain, Abi::ObjectTypeMemoryObject, 1U);
        if (memory.Error != SystemCallError::None)
        {
            return NetworkFailure(Abi::ComponentNetworkMemoryObjectStep, memory.Error);
        }
        const SystemCallError mapped =
            System::MapWindow(client, g_state.Space, CapabilityHandle{memory.Value0, memory.Value1},
                              g_state.Pages[page].Frame, g_state.Platform.Window + (page * PageBytes), 1U);
        if (mapped != SystemCallError::None)
        {
            return NetworkFailure(Abi::ComponentNetworkMapStep, mapped);
        }
    }
    return 0U;
}

// Sends the driver's Query. Zero, WouldBlock's marker (one), or the
// failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 SendQuery() noexcept
{
    alignas(8) Foundation::UInt8 request[NicQueryBytes]{};
    const Foundation::UInt64 words[NicPayloadWords]{g_state.DriverGeneration, 0U, 0U, 0U};
    const Foundation::Size bytes =
        WriteNicFrame(request, NicQueryMethod, g_state.Transaction, words, &g_state.DriverReplies, nullptr);
    const SystemCallError error = g_state.Client->Send(g_state.Driver, System::AddressOf(request), bytes).Error;
    if (error == SystemCallError::None)
    {
        return 0U;
    }
    if (error == SystemCallError::WouldBlock)
    {
        return 1U;
    }
    return NetworkFailure(IsDriverGone(error) ? Abi::ComponentNetworkDriverStep : Abi::ComponentNetworkQueryStep,
                          error);
}

// The driver's answer to the Query: its MAC address in g_state.Mac. The
// bring-up polls only the driver-reply Endpoint, with timed waits: a wake
// for the service Endpoint is left to the loop, which drains that Endpoint
// before its first wait. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 QueryDriver() noexcept
{
    const System::SystemCallClient& client = *g_state.Client;
    g_state.Transaction = 1U;
    bool sent{false};
    Foundation::UInt64 timeouts{0U};
    for (;;)
    {
        if (!sent)
        {
            const Foundation::UInt64 queried = SendQuery();
            if (queried > 1U)
            {
                return queried;
            }
            sent = queried == 0U;
        }
        const Abi::SystemCallResponse received =
            sent ? client.Receive(g_state.DriverReplies, System::AddressOf(g_driverReply), sizeof(g_driverReply))
                 : Abi::SystemCallResponse{.Error = SystemCallError::WouldBlock, .Value0 = 0U, .Value1 = 0U};
        if (received.Error == SystemCallError::WouldBlock)
        {
            const SystemCallError waited = client.Wait(g_state.WaitSet, RetryMicroseconds).Error;
            if ((waited != SystemCallError::None) && (waited != SystemCallError::TimedOut))
            {
                return NetworkFailure(Abi::ComponentNetworkWaitStep, waited);
            }
            timeouts += (waited == SystemCallError::TimedOut) ? 1U : 0U;
            if (timeouts >= BringUpTimeouts)
            {
                return NetworkFailure(Abi::ComponentNetworkQueryStep, SystemCallError::TimedOut);
            }
            continue;
        }
        if (received.Error != SystemCallError::None)
        {
            return NetworkFailure(Abi::ComponentNetworkReceiveStep, received.Error);
        }
        NicMessage reply{};
        if (!ReadNicFrame(g_driverReply, static_cast<Foundation::Size>(received.Value1), reply) ||
            (reply.Method != NicReplyMethod))
        {
            if (!CloseAttachments(client, g_driverReply, 0U))
            {
                return NetworkFailure(Abi::ComponentNetworkCloseStep, SystemCallError::None);
            }
            continue;
        }
        if (reply.Words[0U] != g_state.Transaction)
        {
            continue;
        }
        if (reply.Words[1U] == NicDriverRestarted)
        {
            return NetworkFailure(Abi::ComponentNetworkDriverStep, SystemCallError::None);
        }
        if (reply.Words[1U] != NicDone)
        {
            return NetworkFailure(Abi::ComponentNetworkQueryStep, SystemCallError::None);
        }
        g_state.Mac = reply.Words[2U];
        return 0U;
    }
}

// The stack's configuration: the driver's MAC address and word 37's address
// and prefix length. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 Configure() noexcept
{
    const Foundation::UInt64 prefix = g_state.Configuration >> 32U;
    Network::MacAddress mac{};
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        mac.Octets[index] = static_cast<Foundation::UInt8>(g_state.Mac >> (index << 3U));
    }
    const Network::Ipv4Address netmask =
        static_cast<Network::Ipv4Address>((0xFFFF'FFFFULL << (32U - prefix)) & 0xFFFF'FFFFULL);
    if ((g_state.Mac > 0xFFFF'FFFF'FFFFULL) ||
        !g_stack.Configure(mac, static_cast<Network::Ipv4Address>(g_state.Configuration), netmask))
    {
        return NetworkFailure(Abi::ComponentNetworkConfigurationStep, SystemCallError::None);
    }
    return 0U;
}

// Start-block checks, the window, the driver's Query, the stack's
// configuration and both receive credits. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 BringUp(const Foundation::UInt64 windowPages) noexcept
{
    const Foundation::UInt64 prefix = g_state.Configuration >> 32U;
    if (windowPages < WindowPages)
    {
        return NetworkFailure(Abi::ComponentNetworkWindowStep, SystemCallError::None);
    }
    if ((prefix == 0U) || (prefix > 32U))
    {
        return NetworkFailure(Abi::ComponentNetworkConfigurationStep, SystemCallError::None);
    }
    Foundation::UInt64 failure = PrepareWindow();
    failure = (failure != 0U) ? failure : QueryDriver();
    failure = (failure != 0U) ? failure : Configure();
    if (failure != 0U)
    {
        return failure;
    }
    for (Foundation::Size page{0U}; page < ReceivePages; ++page)
    {
        Credit(page);
    }
    return g_state.Exit;
}
} // Namespace

Foundation::UInt64 ServeNetwork(const System::SystemCallClient& client,
                                const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                const NetworkPlatform& platform) noexcept
{
    g_state = ServiceState{
        .Client = &client,
        .Platform = platform,
        .Endpoint = {startBlock[Abi::ComponentReceiveWord], startBlock[Abi::ComponentReceiveWord + 1U]},
        .WaitSet = {startBlock[Abi::ComponentWaitSetWord], startBlock[Abi::ComponentWaitSetWord + 1U]},
        .Domain = {startBlock[Abi::ComponentAccountWord], startBlock[Abi::ComponentAccountWord + 1U]},
        .Space = {startBlock[Abi::ComponentAccountSpaceWord], startBlock[Abi::ComponentAccountSpaceWord + 1U]},
        .Driver = {startBlock[Abi::ComponentBlockServiceWord], startBlock[Abi::ComponentBlockServiceWord + 1U]},
        .DriverReplies = {startBlock[Abi::ComponentBlockReplyWord], startBlock[Abi::ComponentBlockReplyWord + 1U]},
        .Incarnation = startBlock[Abi::ComponentIncarnationWord],
        .DriverGeneration = startBlock[Abi::ComponentBlockGenerationWord],
        .Configuration = startBlock[Abi::ComponentNetworkConfigurationWord],
        .Mac = 0U,
        .Transaction = 0U,
        .Arrival = 0U,
        .NoReceive = 0U,
        .TooLong = 0U,
        .NoTransmit = 0U,
        .DroppedReplies = 0U,
        .Exit = 0U,
        .Pages = {},
        .Refusal = SystemCallError::None};
    for (ClientSend& send : g_sends)
    {
        send.State = SendState::Free;
    }
    for (ClientReceive& receive : g_receives)
    {
        receive.Present = false;
    }
    const Foundation::UInt64 started = BringUp(startBlock[Abi::ComponentWindowPagesWord]);
    if (started != 0U)
    {
        return started;
    }
    // Every message queued is taken before the instance waits: a readiness a wait consumed (the
    // bring-up's, or one for the other Endpoint) is not signalled again.
    for (;;)
    {
        Drain(g_state.DriverReplies, g_driverReply, sizeof(g_driverReply), &ServeDriverReply);
        Drain(g_state.Endpoint, g_request, sizeof(g_request), &ServeRequest);
        const bool kept = (g_state.Exit == 0U) && Retry();
        if (g_state.Exit != 0U)
        {
            return g_state.Exit;
        }
        const Foundation::UInt64 timeout = kept ? RetryMicroseconds : 0U;
        const SystemCallError waited = client.Wait(g_state.WaitSet, timeout).Error;
        if ((waited != SystemCallError::None) && !((timeout != 0U) && (waited == SystemCallError::TimedOut)))
        {
            return NetworkFailure(Abi::ComponentNetworkWaitStep, waited);
        }
    }
}
} // Namespace Asteria::User::Component
