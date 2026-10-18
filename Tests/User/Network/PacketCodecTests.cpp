// SPDX-License-Identifier: Apache-2.0
#include "User/Network/Packet.hpp"

#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Network/PacketFixtures.hpp"

namespace Asteria::Test::Network
{
namespace
{
using TestSupport::TestContext;
UInt8 g_bytes[Net::MaximumFrameBytes];
UInt8 g_saved[Net::MaximumFrameBytes];

void TestChecksums(TestContext& context) noexcept
{
    // RFC 1071 section 3: sum 0xDDF2, complement 0x220D.
    static constexpr UInt8 Vector[]{0x00U, 0x01U, 0xF2U, 0x03U, 0xF4U, 0xF5U, 0xF6U, 0xF7U};
    ASTERIA_EXPECT(context, "RFC 1071 even, odd and split sums",
                   (Net::InternetChecksum({.Data = Vector, .Length = 8U}) == 0x220DU),
                   (Net::InternetChecksum({.Data = Vector, .Length = 7U}) == 0x2304U),
                   (Net::InternetChecksum({.Data = Vector + 2U, .Length = 6U}, 1U) == 0x220DU),
                   (Net::InternetChecksum({.Data = nullptr, .Length = 0U}) == 0xFFFFU),
                   (Net::InternetChecksum({.Data = nullptr, .Length = 0U}, 0xFFFFFFFFU) == 0U));
    for (Size index{0U}; index < sizeof(g_bytes); ++index)
    {
        g_bytes[index] = static_cast<UInt8>((index * 97U) & 0xFFU);
    }
    for (Size length{0U}; length <= sizeof(g_bytes); ++length)
    {
        ASTERIA_EXPECT(
            context, "checksum agrees with independent modulo checker at every length",
            (Net::InternetChecksum({.Data = g_bytes, .Length = length}) == ReferenceChecksum(g_bytes, length)));
    }
    Net::EthernetPacket ethernet{};
    Net::Ipv4Packet ip{};
    Net::UdpPacket udp{};
    ASTERIA_EXPECT(context, "synthetic UDP request checksums and lengths decode",
                   (Net::ReadEthernet({.Data = SyntheticUdp, .Length = sizeof(SyntheticUdp)}, ethernet) ==
                    Net::PacketStatus::Done),
                   (Net::ReadIpv4(ethernet.Payload, ip) == Net::PacketStatus::Done), ip.Header.Source == 0xC000020AU,
                   ip.Header.Destination == 0xC0000214U, ip.Header.Ttl == 64U, ip.Header.Identification == 0x1357U,
                   (Net::ReadUdp(ip.Payload, ip.Header.Source, ip.Header.Destination, udp) == Net::PacketStatus::Done),
                   udp.SourcePort == 40000U, udp.DestinationPort == 7U, udp.Payload.Length == 12U);
    std::memcpy(g_bytes, SyntheticUdp, sizeof(SyntheticUdp));
    g_bytes[24U] = 0U;
    g_bytes[25U] = 0U;
    g_bytes[40U] = 0U;
    g_bytes[41U] = 0U;
    ASTERIA_EXPECT(context, "synthetic checksums independently recompute to their stored values",
                   (Net::InternetChecksum({.Data = g_bytes + 14U, .Length = 20U}) == 0xE34FU),
                   (Net::InternetChecksum({.Data = g_bytes + 34U, .Length = 20U},
                                          0xC000U + 0x020AU + 0xC000U + 0x0214U + 17U + 20U) == 0x4E16U));
}

void TestCodecs(TestContext& context) noexcept
{
    Net::EthernetPacket ethernet{};
    Net::ArpPacket arp{};
    ASTERIA_EXPECT(
        context, "Ethernet II and ARP decode padded literal request",
        (Net::ReadEthernet({.Data = ArpRequest, .Length = sizeof(ArpRequest)}, ethernet) == Net::PacketStatus::Done),
        ethernet.Header.EtherType == 0x0806U, Net::EqualMac(ethernet.Header.Source, PeerMac),
        (Net::ReadArp(ethernet.Payload, arp) == Net::PacketStatus::Done), arp.Operation == 1U,
        arp.SenderAddress == PeerAddress, arp.TargetAddress == OwnAddress);
    ASTERIA_EXPECT(context, "ARP encoder reproduces the literal ARP bytes",
                   (Net::WriteArp({.Data = g_bytes, .Length = sizeof(g_bytes)}, arp) == 28U),
                   std::memcmp(g_bytes, ArpRequest + 14U, 28U) == 0);
    ASTERIA_EXPECT(context, "Ethernet encoder supplies zero padding to 60",
                   (Net::WriteEthernet({.Data = g_bytes, .Length = sizeof(g_bytes)}, ethernet.Header,
                                       {.Data = ArpRequest + 14U, .Length = 28U}) == 60U),
                   std::memcmp(g_bytes, ArpRequest, 60U) == 0);
    Net::Ipv4Packet ip{};
    ASTERIA_EXPECT(context, "IPv4 and ICMP consume header length and ignore Ethernet padding",
                   (Net::ReadIpv4({.Data = IcmpRequest + 14U, .Length = 46U}, ip) == Net::PacketStatus::Done),
                   ip.Payload.Length == 13U, ip.Header.Ttl == 64U,
                   Net::ReadIcmpEcho(ip.Payload) == Net::PacketStatus::Done,
                   (Net::WriteIcmpEchoReply({.Data = g_bytes, .Length = sizeof(g_bytes)}, ip.Payload) == 13U),
                   std::memcmp(g_bytes, IcmpReply + 34U, 13U) == 0);
    ASTERIA_EXPECT(context, "IPv4 encoder reproduces literal header and odd payload",
                   (Net::WriteIpv4({.Data = g_bytes, .Length = sizeof(g_bytes)}, ip.Header, ip.Payload) == 33U),
                   std::memcmp(g_bytes, IcmpRequest + 14U, 33U) == 0);
    Net::UdpPacket udp{};
    ASTERIA_EXPECT(context, "UDP decodes odd payload and encodes its pseudo-header checksum",
                   (Net::ReadUdp({.Data = UdpIncoming + 34U, .Length = 26U}, PeerAddress, OwnAddress, udp) ==
                    Net::PacketStatus::Done),
                   udp.Payload.Length == 5U, udp.SourcePort == 9000U,
                   (Net::WriteUdp({.Data = g_bytes, .Length = sizeof(g_bytes)}, PeerAddress, OwnAddress, udp.SourcePort,
                                  udp.DestinationPort, udp.Payload) == 13U),
                   std::memcmp(g_bytes, UdpIncoming + 34U, 13U) == 0);
    // A payload word chosen so that the computed UDP checksum is zero.
    static constexpr UInt8 ZeroChecksumPayload[]{0xC4U, 0x9AU};
    ASTERIA_EXPECT(context, "computed UDP checksum zero is transmitted as FFFF",
                   (Net::WriteUdp({.Data = g_bytes, .Length = sizeof(g_bytes)}, OwnAddress, PeerAddress, 7U, 9000U,
                                  {.Data = ZeroChecksumPayload, .Length = 2U}) == 10U),
                   Get16(g_bytes + 6U) == 0xFFFFU,
                   ReferenceChecksum(g_bytes, 10U, 0x0A00U + 0x020FU + 0x0A00U + 0x0202U + 17U + 10U) == 0U);
}

void TestFailureAtomicity(TestContext& context) noexcept
{
    std::memset(g_bytes, 0xA5, sizeof(g_bytes));
    std::memcpy(g_saved, g_bytes, sizeof(g_bytes));
    const Net::EthernetHeader ethernet{.Destination = OwnMac, .Source = PeerMac, .EtherType = 0x0800U};
    const Net::Ipv4Header ip{
        .Source = PeerAddress, .Destination = OwnAddress, .Identification = 0U, .Ttl = 0U, .Protocol = 17U};
    Net::ArpPacket arp{};
    ASTERIA_EXPECT(
        context, "short buffers, null payload, overlarge payload and invalid fields write nothing",
        (Net::WriteEthernet({.Data = g_bytes, .Length = 59U}, ethernet, {.Data = nullptr, .Length = 0U}) == 0U),
        (Net::WriteIpv4({.Data = g_bytes, .Length = sizeof(g_bytes)}, ip, {.Data = nullptr, .Length = 0U}) == 0U),
        (Net::WriteArp({.Data = g_bytes, .Length = sizeof(g_bytes)}, arp) == 0U),
        (Net::WriteUdp({.Data = g_bytes, .Length = sizeof(g_bytes)}, OwnAddress, PeerAddress, 7U, 9U,
                       {.Data = nullptr, .Length = 1U}) == 0U),
        (Net::WriteUdp({.Data = g_bytes, .Length = 8U}, OwnAddress, PeerAddress, 7U, 9U,
                       {.Data = UdpIncoming, .Length = 1U}) == 0U),
        (Net::WriteUdp({.Data = g_bytes, .Length = sizeof(g_bytes)}, OwnAddress, PeerAddress, 7U, 9U,
                       {.Data = UdpIncoming, .Length = ~Size{0U}}) == 0U),
        (Net::WriteIcmpEchoReply({.Data = g_bytes, .Length = 8U}, {.Data = IcmpRequest + 34U, .Length = 13U}) == 0U),
        std::memcmp(g_bytes, g_saved, sizeof(g_bytes)) == 0);
    const Net::ByteSpan empty{.Data = nullptr, .Length = 0U};
    Net::EthernetPacket decodedEthernet{.Header = ethernet, .Payload = empty};
    Net::Ipv4Packet decodedIp{.Header = ip, .Payload = empty};
    Net::UdpPacket decodedUdp{.SourcePort = 99U, .DestinationPort = 88U, .Payload = empty};
    arp.Operation = 99U;
    ASTERIA_EXPECT(context, "truncated decodes preserve every output",
                   Net::ReadEthernet(empty, decodedEthernet) == Net::PacketStatus::Truncated,
                   decodedEthernet.Header.EtherType == 0x0800U,
                   Net::ReadArp(empty, arp) == Net::PacketStatus::Truncated, arp.Operation == 99U,
                   Net::ReadIpv4(empty, decodedIp) == Net::PacketStatus::Truncated,
                   decodedIp.Header.Source == PeerAddress,
                   Net::ReadUdp(empty, OwnAddress, PeerAddress, decodedUdp) == Net::PacketStatus::Truncated,
                   decodedUdp.SourcePort == 99U, Net::ReadIcmpEcho(empty) == Net::PacketStatus::Truncated);
}
} // namespace
} // namespace Asteria::Test::Network

int main() noexcept
{
    Asteria::TestSupport::TestContext context{"PacketCodecTests"};
    Asteria::Test::Network::TestChecksums(context);
    Asteria::Test::Network::TestCodecs(context);
    Asteria::Test::Network::TestFailureAtomicity(context);
    return context.Finish();
}
