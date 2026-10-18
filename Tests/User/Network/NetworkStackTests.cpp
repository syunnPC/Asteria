// SPDX-License-Identifier: Apache-2.0
#include "User/Network/NetworkStack.hpp"

#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Network/PacketFixtures.hpp"

namespace Asteria::Test::Network
{
namespace
{
using TestSupport::TestContext;
Net::NetworkStack g_stack;
Net::ReceiveOutput g_received;
Net::TransmitFrame g_sent;
UInt8 g_input[Net::MaximumFrameBytes + 1U];
UInt8 g_payload[Net::MaximumUdpPayloadBytes + 1U];

void Reset(TestContext& context) noexcept
{
    ASTERIA_EXPECT(context, "valid static configuration", g_stack.Configure(OwnMac, OwnAddress, Netmask));
}

void ExpectDrop(TestContext& context, const Net::DropReason reason, const Size length = 60U) noexcept
{
    const UInt64 before = g_stack.GetDropCount(reason);
    UInt64 totalBefore{0U};
    for (Size index{0U}; index < static_cast<Size>(Net::DropReason::Count); ++index)
    {
        totalBefore += g_stack.GetDropCount(static_cast<Net::DropReason>(index));
    }
    g_received.Frame.Length = 123U;
    g_received.Datagram.Present = true;
    g_stack.Receive(g_input, length, g_received);
    UInt64 totalAfter{0U};
    for (Size index{0U}; index < static_cast<Size>(Net::DropReason::Count); ++index)
    {
        totalAfter += g_stack.GetDropCount(static_cast<Net::DropReason>(index));
    }
    ASTERIA_EXPECT(context, "rejection emits/delivers nothing and increments exactly the specified reason",
                   g_received.Frame.Length == 0U, !g_received.Datagram.Present,
                   g_stack.GetDropCount(reason) == before + 1U, totalAfter == totalBefore + 1U);
}

void TestConfigurationAndCapacity(TestContext& context) noexcept
{
    const Net::MacAddress zero{};
    Net::MacAddress multicast{OwnMac};
    multicast.Octets[0U] = 3U;
    ASTERIA_EXPECT(context, "invalid configuration leaves no authority or traffic",
                   !g_stack.Configure(zero, OwnAddress, Netmask), !g_stack.InstallRule(EchoRule),
                   !g_stack.Bind(17U, 7U),
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, nullptr, 0U, g_sent) == Net::SendStatus::Refused,
                   g_sent.Length == 0U);
    g_stack.Receive(ArpRequest, 60U, g_received);
    ASTERIA_EXPECT(context, "unconfigured receive is counted", g_received.Frame.Length == 0U,
                   g_stack.GetDropCount(Net::DropReason::Unconfigured) == 1U,
                   !g_stack.Configure(multicast, OwnAddress, Netmask), !g_stack.Configure(OwnMac, 0U, Netmask),
                   !g_stack.Configure(OwnMac, 0xE000020FU, Netmask), !g_stack.Configure(OwnMac, OwnAddress, 0U),
                   !g_stack.Configure(OwnMac, OwnAddress, 0xFF00FF00U),
                   !g_stack.Configure(OwnMac, 0x0A000200U, Netmask), !g_stack.Configure(OwnMac, 0x0A0002FFU, Netmask));
    ASTERIA_EXPECT(context, "/31 and /32 are valid configurations", g_stack.Configure(OwnMac, OwnAddress, 0xFFFFFFFEU),
                   g_stack.Configure(OwnMac, OwnAddress, 0xFFFFFFFFU));
    Reset(context);
    const Net::NetworkRule invalid{.Sender = 17U, .LocalPort = 0U, .RemoteAddress = 0U, .RemotePort = 0U};
    ASTERIA_EXPECT(context, "zero local port is invalid", !g_stack.InstallRule(invalid));
    for (UInt16 port{1U}; port <= 8U; ++port)
    {
        const Net::NetworkRule rule{
            .Sender = 0xFFFF000000000011U, .LocalPort = port, .RemoteAddress = 0U, .RemotePort = 0U};
        ASTERIA_EXPECT(context, "all eight distinct rules and binds fit", g_stack.InstallRule(rule),
                       g_stack.Bind(rule.Sender, port), !g_stack.Bind(17U, port));
    }
    const Net::NetworkRule ninth{.Sender = 17U, .LocalPort = 9U, .RemoteAddress = 0U, .RemotePort = 0U};
    ASTERIA_EXPECT(context, "ninth rule refused with full authority tables", !g_stack.InstallRule(ninth),
                   g_stack.GetRuleCount() == 8U, g_stack.GetBindingCount() == 8U);
    static constexpr UInt32 RefusedAddresses[]{0U,          OwnAddress,  0x0B000202U, 0x0A000200U,
                                               0x0A0002FFU, 0xFFFFFFFFU, 0xE0000202U};
    for (const UInt32 address : RefusedAddresses)
    {
        ASTERIA_EXPECT(context, "wildcard rule still refuses off-link/non-unicast destinations",
                       g_stack.SendTo(0xFFFF000000000011U, 7U, address, 9000U, nullptr, 0U, g_sent) ==
                           Net::SendStatus::Refused,
                       g_sent.Length == 0U);
    }
    ASTERIA_EXPECT(context, "wildcard rule permits arbitrary peer and remote port",
                   g_stack.SendTo(0xFFFF000000000011U, 7U, PeerAddress, 9001U, nullptr, 0U, g_sent) ==
                       Net::SendStatus::Pending);
    Reset(context);
    const Net::NetworkRule other{.Sender = 18U, .LocalPort = 7U, .RemoteAddress = 0U, .RemotePort = 0U};
    ASTERIA_EXPECT(context, "two authorized senders cannot both own one receive port", g_stack.InstallRule(EchoRule),
                   g_stack.InstallRule(other), g_stack.Bind(17U, 7U), !g_stack.Bind(18U, 7U),
                   g_stack.GetBindingCount() == 1U);
    g_stack.Receive(ArpRequest, 60U, g_received);
    ASTERIA_EXPECT(context, "Configure clears old rules, bindings, peers and counters",
                   g_stack.Configure(OwnMac, OwnAddress, Netmask), g_stack.GetRuleCount() == 0U,
                   g_stack.GetBindingCount() == 0U, g_stack.GetArpEntryCount() == 0U,
                   g_stack.GetDropCount(Net::DropReason::Unconfigured) == 0U,
                   g_stack.GetDropCount(Net::DropReason::Count) == 0U);
}

void TestTraffic(TestContext& context) noexcept
{
    Reset(context);
    ASTERIA_EXPECT(context, "absent rule refuses sending without emitting even ARP",
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, UdpIncoming + 42U, 5U, g_sent) ==
                       Net::SendStatus::Refused,
                   g_sent.Length == 0U, !g_stack.Bind(17U, 7U));
    ASTERIA_EXPECT(context, "rule names exact sender; wrong sender cannot bind", g_stack.InstallRule(EchoRule),
                   !g_stack.Bind(18U, 7U), g_stack.Bind(17U, 7U), g_stack.Bind(17U, 7U),
                   g_stack.GetBindingCount() == 1U, g_stack.InstallRule(EchoRule), g_stack.GetRuleCount() == 1U,
                   g_stack.IsBound(17U, 7U), !g_stack.IsBound(18U, 7U), !g_stack.IsBound(17U, 8U),
                   !g_stack.IsBound(17U, 0U));
    ASTERIA_EXPECT(context, "unknown peer produces byte-exact padded ARP request and Pending",
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, UdpIncoming + 42U, 5U, g_sent) ==
                       Net::SendStatus::Pending,
                   g_sent.Length == sizeof(ResolutionRequest), std::memcmp(g_sent.Bytes, ResolutionRequest, 60U) == 0);
    g_stack.Receive(PeerArpReply, sizeof(PeerArpReply), g_received);
    Net::MacAddress mac{};
    ASTERIA_EXPECT(context, "ARP reply learns peer and emits nothing", g_received.Frame.Length == 0U,
                   !g_received.Datagram.Present, g_stack.GetArpEntryCount() == 1U, g_stack.LookupPeer(PeerAddress, mac),
                   Net::EqualMac(mac, PeerMac));
    ASTERIA_EXPECT(context, "retry emits byte-exact UDP frame",
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, UdpIncoming + 42U, 5U, g_sent) == Net::SendStatus::Sent,
                   g_sent.Length == sizeof(UdpOutgoing), std::memcmp(g_sent.Bytes, UdpOutgoing, 60U) == 0,
                   CheckIpv4Frame(g_sent, 17U));
    g_stack.Receive(ArpRequest, sizeof(ArpRequest), g_received);
    ASTERIA_EXPECT(context, "targeted ARP request receives exact reply including zero padding",
                   g_received.Frame.Length == sizeof(ArpReply), std::memcmp(g_received.Frame.Bytes, ArpReply, 60U) == 0,
                   !g_received.Datagram.Present);
    g_stack.Receive(IcmpRequest, sizeof(IcmpRequest), g_received);
    ASTERIA_EXPECT(context, "echo returns exact identifier, sequence and odd body, with fresh checksums",
                   g_received.Frame.Length == sizeof(IcmpReply),
                   std::memcmp(g_received.Frame.Bytes, IcmpReply, 60U) == 0, CheckIpv4Frame(g_received.Frame, 1U),
                   !g_received.Datagram.Present);
    g_stack.Receive(UdpIncoming, sizeof(UdpIncoming), g_received);
    ASTERIA_EXPECT(context, "UDP delivers borrowed header-sized payload to bound owner", g_received.Frame.Length == 0U,
                   g_received.Datagram.Present, g_received.Datagram.Owner == 17U,
                   g_received.Datagram.SourceAddress == PeerAddress, g_received.Datagram.SourcePort == 9000U,
                   g_received.Datagram.LocalPort == 7U, g_received.Datagram.Payload.Length == 5U,
                   g_received.Datagram.Payload.Data == UdpIncoming + 42U,
                   std::memcmp(g_received.Datagram.Payload.Data, "hello", 5U) == 0);
    // A rule filters inbound datagrams too: the owner's wildcard rule admits any peer and port,
    // another sender's or another port's rule admits nothing.
    Reset(context);
    const Net::NetworkRule wildcard{.Sender = 17U, .LocalPort = 7U, .RemoteAddress = 0U, .RemotePort = 0U};
    const Net::NetworkRule foreign{.Sender = 18U, .LocalPort = 7U, .RemoteAddress = 0U, .RemotePort = 0U};
    const Net::NetworkRule otherPort{.Sender = 17U, .LocalPort = 8U, .RemoteAddress = 0U, .RemotePort = 0U};
    ASTERIA_EXPECT(context, "rules for the inbound cases", g_stack.InstallRule(EchoRule), g_stack.InstallRule(foreign),
                   g_stack.InstallRule(otherPort), g_stack.Bind(17U, 7U));
    std::memcpy(g_input, UdpIncoming, 60U);
    Put16(g_input + 40U, 0U);
    Put16(g_input + 34U, 9001U);
    ExpectDrop(context, Net::DropReason::Unauthorized);
    ASTERIA_EXPECT(context, "an unauthorized datagram still teaches its sender's address",
                   g_stack.LookupPeer(PeerAddress, mac));
    ASTERIA_EXPECT(context, "the wildcard rule admits another source port", g_stack.InstallRule(wildcard));
    g_stack.Receive(g_input, 60U, g_received);
    ASTERIA_EXPECT(context, "the owner's wildcard rule delivers the datagram", g_received.Datagram.Present,
                   g_received.Datagram.Owner == 17U, g_received.Datagram.SourcePort == 9001U,
                   g_stack.GetDropCount(Net::DropReason::Unauthorized) == 1U);
    Reset(context);
    ASTERIA_EXPECT(context, "the exact rule alone", g_stack.InstallRule(EchoRule), g_stack.Bind(17U, 7U));
    std::memcpy(g_input, UdpIncoming, 60U);
    Put16(g_input + 40U, 0U);
    Put32(g_input + 26U, PeerAddress + 1U);
    FixIpChecksum(g_input);
    ExpectDrop(context, Net::DropReason::Unauthorized);
    Reset(context);
    ASTERIA_EXPECT(context, "install permits sends without binding", g_stack.InstallRule(EchoRule));
    g_stack.Receive(IcmpRequest, sizeof(IcmpRequest), g_received);
    ASTERIA_EXPECT(context, "receiving IPv4 learns peer even without prior ARP", g_stack.LookupPeer(PeerAddress, mac),
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, nullptr, 0U, g_sent) == Net::SendStatus::Sent,
                   CheckIpv4Frame(g_sent, 17U), Get16(g_sent.Bytes + 38U) == 8U);
}

void TestRejections(TestContext& context) noexcept
{
    Reset(context);
    std::memcpy(g_input, ArpRequest, 60U);
    g_input[41U] = 3U;
    ExpectDrop(context, Net::DropReason::NotForUs);
    ASTERIA_EXPECT(context, "foreign ARP target does not insert unknown sender", g_stack.GetArpEntryCount() == 0U);
    std::memcpy(g_input, UdpIncoming, 60U);
    ExpectDrop(context, Net::DropReason::UnboundPort);
    ASTERIA_EXPECT(context, "binding for negative UDP cases", g_stack.InstallRule(EchoRule), g_stack.Bind(17U, 7U));
    struct Mutation
    {
        Size Offset;
        UInt8 Value;
        Size SecondOffset;
        UInt8 SecondValue;
        bool FixIp;
        Net::DropReason Reason;
    };
    static constexpr Mutation Mutations[]{
        {24U, 0U, 25U, 0U, false, Net::DropReason::BadIpChecksum},
        {40U, 1U, 41U, 1U, false, Net::DropReason::BadUdpChecksum},
        {14U, 0x46U, 14U, 0x46U, false, Net::DropReason::IpOptions},
        {14U, 0x44U, 14U, 0x44U, false, Net::DropReason::IpOptions},
        {14U, 0x65U, 14U, 0x65U, false, Net::DropReason::Unsupported},
        {29U, 3U, 29U, 3U, true, Net::DropReason::BadUdpChecksum},
        {20U, 0x20U, 21U, 0U, true, Net::DropReason::Fragment},
        {20U, 0U, 21U, 1U, true, Net::DropReason::Fragment},
        {20U, 0x80U, 21U, 0U, true, Net::DropReason::Malformed},
        {22U, 0U, 22U, 0U, true, Net::DropReason::TtlExpired},
        {16U, 0U, 17U, 19U, true, Net::DropReason::Malformed},
        {16U, 0U, 17U, 80U, true, Net::DropReason::Runt},
        {38U, 0U, 39U, 7U, false, Net::DropReason::Malformed},
        {38U, 0U, 39U, 14U, false, Net::DropReason::Runt},
        {33U, 99U, 33U, 99U, true, Net::DropReason::NotForUs},
        {26U, 11U, 26U, 11U, true, Net::DropReason::OffLink},
        {23U, 6U, 23U, 6U, true, Net::DropReason::Unsupported},
        {12U, 0x86U, 13U, 0xDDU, false, Net::DropReason::Unsupported},
        {12U, 0x88U, 13U, 0xB5U, false, Net::DropReason::Unsupported},
        {12U, 0U, 13U, 46U, false, Net::DropReason::Unsupported},
        {5U, 99U, 5U, 99U, false, Net::DropReason::NotForUs},
        {6U, 3U, 6U, 3U, false, Net::DropReason::Malformed},
    };
    for (const Mutation& mutation : Mutations)
    {
        std::memcpy(g_input, UdpIncoming, 60U);
        g_input[mutation.Offset] = mutation.Value;
        g_input[mutation.SecondOffset] = mutation.SecondValue;
        if (mutation.FixIp)
        {
            FixIpChecksum(g_input);
        }
        ExpectDrop(context, mutation.Reason);
    }
    std::memcpy(g_input, IcmpRequest, 60U);
    g_input[36U] ^= 1U;
    ExpectDrop(context, Net::DropReason::BadIcmpChecksum);
    std::memcpy(g_input, ArpRequest, 60U);
    g_input[21U] = 3U;
    ExpectDrop(context, Net::DropReason::Unsupported);
    std::memcpy(g_input, ArpRequest, 60U);
    g_input[27U] = 99U;
    ExpectDrop(context, Net::DropReason::Malformed);
    std::memcpy(g_input, UdpIncoming, 60U);
    ExpectDrop(context, Net::DropReason::Runt, 13U);
    ExpectDrop(context, Net::DropReason::Runt, 0U);
    ExpectDrop(context, Net::DropReason::Oversize, sizeof(g_input));
    g_stack.Receive(nullptr, 60U, g_received);
    ASTERIA_EXPECT(context, "null frame emits nothing", g_received.Frame.Length == 0U, !g_received.Datagram.Present);
    static constexpr UInt16 RefusedPorts[]{8999U, 0U};
    for (const UInt16 remotePort : RefusedPorts)
    {
        ASTERIA_EXPECT(context, "rule rejects mismatched remote port and zero port",
                       g_stack.SendTo(17U, 7U, PeerAddress, remotePort, nullptr, 0U, g_sent) ==
                           Net::SendStatus::Refused,
                       g_sent.Length == 0U);
    }
    ASTERIA_EXPECT(context, "sender, local port, destination and payload bounds refuse without frame",
                   g_stack.SendTo(18U, 7U, PeerAddress, 9000U, nullptr, 0U, g_sent) == Net::SendStatus::Refused,
                   g_stack.SendTo(17U, 8U, PeerAddress, 9000U, nullptr, 0U, g_sent) == Net::SendStatus::Refused,
                   g_stack.SendTo(17U, 7U, PeerAddress + 1U, 9000U, nullptr, 0U, g_sent) == Net::SendStatus::Refused,
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, nullptr, 1U, g_sent) == Net::SendStatus::Refused,
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, g_payload, sizeof(g_payload), g_sent) ==
                       Net::SendStatus::Refused,
                   g_sent.Length == 0U);
}

void TestLengths(TestContext& context) noexcept
{
    Reset(context);
    ASTERIA_EXPECT(context, "bind for length cases", g_stack.InstallRule(EchoRule), g_stack.Bind(17U, 7U));
    std::memcpy(g_input, UdpIncoming, 60U);
    g_input[40U] = 0U;
    g_input[41U] = 0U;
    g_input[59U] = 0xA5U;
    g_stack.Receive(g_input, 60U, g_received);
    ASTERIA_EXPECT(context, "zero UDP checksum accepted; nonzero Ethernet padding ignored", g_received.Datagram.Present,
                   g_received.Datagram.Payload.Length == 5U);
    // Three additional bytes inside IPv4 but outside the UDP header's length.
    g_input[17U] = 36U;
    g_input[47U] = 0xFFU;
    FixIpChecksum(g_input);
    g_stack.Receive(g_input, 60U, g_received);
    ASTERIA_EXPECT(context, "UDP header length trims longer IP payload", g_received.Datagram.Present,
                   g_received.Datagram.Payload.Length == 5U);
    std::memcpy(g_input, UdpIncoming, 60U);
    Put16(g_input + 38U, 8U);
    Put16(g_input + 40U, 0U);
    g_stack.Receive(g_input, 60U, g_received);
    ASTERIA_EXPECT(context, "empty received datagram still marks delivery", g_received.Datagram.Present,
                   g_received.Datagram.Payload.Length == 0U);
    for (Size index{0U}; index < sizeof(g_payload); ++index)
    {
        g_payload[index] = static_cast<UInt8>(index & 0xFFU);
    }
    ASTERIA_EXPECT(context, "maximum MTU UDP is 1514 bytes with correct checksum and no padding",
                   g_stack.SendTo(17U, 7U, PeerAddress, 9000U, g_payload, 1472U, g_sent) == Net::SendStatus::Sent,
                   g_sent.Length == 1514U, CheckIpv4Frame(g_sent, 17U), Get16(g_sent.Bytes + 38U) == 1480U,
                   std::memcmp(g_sent.Bytes + 42U, g_payload, 1472U) == 0);
    // Turn the independently checked outgoing packet into incoming, with checksum 0.
    std::memcpy(g_input, g_sent.Bytes, 1514U);
    std::memcpy(g_input, OwnMac.Octets, 6U);
    std::memcpy(g_input + 6U, PeerMac.Octets, 6U);
    Put32(g_input + 26U, PeerAddress);
    Put32(g_input + 30U, OwnAddress);
    Put16(g_input + 34U, 9000U);
    Put16(g_input + 36U, 7U);
    Put16(g_input + 40U, 0U);
    FixIpChecksum(g_input);
    g_stack.Receive(g_input, 1514U, g_received);
    ASTERIA_EXPECT(context, "maximum received frame delivers the whole MTU datagram", g_received.Datagram.Present,
                   g_received.Datagram.Payload.Length == 1472U,
                   std::memcmp(g_received.Datagram.Payload.Data, g_payload, 1472U) == 0);
}
} // Namespace
} // Namespace Asteria::Test::Network

int main() noexcept
{
    Asteria::TestSupport::TestContext context{"NetworkStackTests"};
    Asteria::Test::Network::TestConfigurationAndCapacity(context);
    Asteria::Test::Network::TestTraffic(context);
    Asteria::Test::Network::TestRejections(context);
    Asteria::Test::Network::TestLengths(context);
    return context.Finish();
}
