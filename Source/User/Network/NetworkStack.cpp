// SPDX-License-Identifier: Apache-2.0

#include "User/Network/NetworkStack.hpp"

namespace Asteria::User::Network
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

constexpr MacAddress Broadcast{.Octets = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}};
constexpr MacAddress ZeroMac{.Octets = {0U, 0U, 0U, 0U, 0U, 0U}};

[[nodiscard]] UInt16 Read16(const UInt8* const bytes) noexcept
{
    return static_cast<UInt16>((static_cast<UInt32>(bytes[0U]) << 8U) | bytes[1U]);
}

[[nodiscard]] UInt32 Read32(const UInt8* const bytes) noexcept
{
    return (static_cast<UInt32>(Read16(bytes)) << 16U) | Read16(bytes + 2U);
}

void Put16(UInt8* const bytes, const UInt16 value) noexcept
{
    bytes[0U] = static_cast<UInt8>(value >> 8U);
    bytes[1U] = static_cast<UInt8>(value & 0xFFU);
}

void Put32(UInt8* const bytes, const UInt32 value) noexcept
{
    Put16(bytes, static_cast<UInt16>(value >> 16U));
    Put16(bytes + 2U, static_cast<UInt16>(value & 0xFFFFU));
}

void Copy(UInt8* const destination, const UInt8* const source, const Size length) noexcept
{
    for (Size index{0U}; index < length; ++index)
    {
        destination[index] = source[index];
    }
}

[[nodiscard]] MacAddress ReadMac(const UInt8* const bytes) noexcept
{
    MacAddress mac{};
    Copy(mac.Octets, bytes, 6U);
    return mac;
}

[[nodiscard]] bool UnicastMac(const MacAddress& mac) noexcept
{
    return ((mac.Octets[0U] & 1U) == 0U) && !EqualMac(mac, ZeroMac);
}

[[nodiscard]] bool UnicastIp(const Ipv4Address address) noexcept
{
    return (address != 0U) && ((address >> 24U) != 0U) && ((address >> 24U) != 127U) && ((address >> 28U) < 14U);
}

[[nodiscard]] UInt32 PseudoSum(const Ipv4Address source, const Ipv4Address destination, const UInt16 length) noexcept
{
    return (source >> 16U) + (source & 0xFFFFU) + (destination >> 16U) + (destination & 0xFFFFU) + 17U + length;
}

[[nodiscard]] bool Fits(const MutableByteSpan bytes, const ByteSpan payload, const Size header,
                        const Size maximum) noexcept
{
    return (bytes.Data != nullptr) && ((payload.Data != nullptr) || (payload.Length == 0U)) &&
           (payload.Length <= maximum - header) && (bytes.Length >= header) &&
           (payload.Length <= bytes.Length - header);
}
} // namespace

bool EqualMac(const MacAddress& left, const MacAddress& right) noexcept
{
    for (Size index{0U}; index < 6U; ++index)
    {
        if (left.Octets[index] != right.Octets[index])
        {
            return false;
        }
    }
    return true;
}

UInt16 InternetChecksum(const ByteSpan bytes, const UInt32 initialSum) noexcept
{
    UInt32 sum{(initialSum & 0xFFFFU) + (initialSum >> 16U)};
    Size index{0U};
    while (bytes.Length - index >= 2U)
    {
        sum += Read16(bytes.Data + index);
        sum = (sum & 0xFFFFU) + (sum >> 16U);
        index += 2U;
    }
    if (index < bytes.Length)
    {
        sum += static_cast<UInt32>(bytes.Data[index]) << 8U;
    }
    while ((sum >> 16U) != 0U)
    {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return static_cast<UInt16>((~sum) & 0xFFFFU);
}

PacketStatus ReadEthernet(const ByteSpan bytes, EthernetPacket& packet) noexcept
{
    if (bytes.Length > MaximumFrameBytes)
    {
        return PacketStatus::Oversize;
    }
    if ((bytes.Data == nullptr) || (bytes.Length < 14U))
    {
        return PacketStatus::Truncated;
    }
    const UInt16 type = Read16(bytes.Data + 12U);
    if (type < 0x0600U)
    {
        return PacketStatus::Unsupported;
    }
    packet = {.Header = {.Destination = ReadMac(bytes.Data), .Source = ReadMac(bytes.Data + 6U), .EtherType = type},
              .Payload = {.Data = bytes.Data + 14U, .Length = bytes.Length - 14U}};
    return PacketStatus::Done;
}

PacketStatus ReadArp(const ByteSpan bytes, ArpPacket& packet) noexcept
{
    if ((bytes.Data == nullptr) || (bytes.Length < 28U))
    {
        return PacketStatus::Truncated;
    }
    if ((Read16(bytes.Data) != 1U) || (Read16(bytes.Data + 2U) != Ipv4EtherType) || (bytes.Data[4U] != 6U) ||
        (bytes.Data[5U] != 4U))
    {
        return PacketStatus::Unsupported;
    }
    const UInt16 operation = Read16(bytes.Data + 6U);
    if ((operation != 1U) && (operation != 2U))
    {
        return PacketStatus::Unsupported;
    }
    packet = {.Operation = operation,
              .SenderMac = ReadMac(bytes.Data + 8U),
              .SenderAddress = Read32(bytes.Data + 14U),
              .TargetMac = ReadMac(bytes.Data + 18U),
              .TargetAddress = Read32(bytes.Data + 24U)};
    return PacketStatus::Done;
}

PacketStatus ReadIpv4(const ByteSpan bytes, Ipv4Packet& packet) noexcept
{
    if ((bytes.Data == nullptr) || (bytes.Length < 20U))
    {
        return PacketStatus::Truncated;
    }
    if ((bytes.Data[0U] >> 4U) != 4U)
    {
        return PacketStatus::Unsupported;
    }
    if ((bytes.Data[0U] & 0x0FU) != 5U)
    {
        return PacketStatus::Options;
    }
    const UInt16 length = Read16(bytes.Data + 2U);
    if ((length < 20U) || (length > 1500U))
    {
        return PacketStatus::Malformed;
    }
    if (length > bytes.Length)
    {
        return PacketStatus::Truncated;
    }
    if (InternetChecksum({.Data = bytes.Data, .Length = 20U}) != 0U)
    {
        return PacketStatus::BadChecksum;
    }
    const UInt16 fragment = Read16(bytes.Data + 6U);
    if ((fragment & 0x8000U) != 0U)
    {
        return PacketStatus::Malformed;
    }
    if ((fragment & 0x3FFFU) != 0U)
    {
        return PacketStatus::Fragmented;
    }
    if (bytes.Data[8U] == 0U)
    {
        return PacketStatus::Expired;
    }
    packet = {.Header = {.Source = Read32(bytes.Data + 12U),
                         .Destination = Read32(bytes.Data + 16U),
                         .Identification = Read16(bytes.Data + 4U),
                         .Ttl = bytes.Data[8U],
                         .Protocol = bytes.Data[9U]},
              .Payload = {.Data = bytes.Data + 20U, .Length = static_cast<Size>(length) - 20U}};
    return PacketStatus::Done;
}

PacketStatus ReadIcmpEcho(const ByteSpan bytes) noexcept
{
    if ((bytes.Data == nullptr) || (bytes.Length < 8U))
    {
        return PacketStatus::Truncated;
    }
    if (InternetChecksum(bytes) != 0U)
    {
        return PacketStatus::BadChecksum;
    }
    if ((bytes.Data[0U] != 8U) || (bytes.Data[1U] != 0U))
    {
        return PacketStatus::Unsupported;
    }
    return PacketStatus::Done;
}

PacketStatus ReadUdp(const ByteSpan bytes, const Ipv4Address source, const Ipv4Address destination,
                     UdpPacket& packet) noexcept
{
    if ((bytes.Data == nullptr) || (bytes.Length < 8U))
    {
        return PacketStatus::Truncated;
    }
    const UInt16 length = Read16(bytes.Data + 4U);
    if ((length < 8U) || (length > 1480U))
    {
        return PacketStatus::Malformed;
    }
    if (length > bytes.Length)
    {
        return PacketStatus::Truncated;
    }
    if ((Read16(bytes.Data + 6U) != 0U) &&
        (InternetChecksum({.Data = bytes.Data, .Length = length}, PseudoSum(source, destination, length)) != 0U))
    {
        return PacketStatus::BadChecksum;
    }
    packet = {.SourcePort = Read16(bytes.Data),
              .DestinationPort = Read16(bytes.Data + 2U),
              .Payload = {.Data = bytes.Data + 8U, .Length = static_cast<Size>(length) - 8U}};
    return PacketStatus::Done;
}

Size WriteEthernet(const MutableByteSpan bytes, const EthernetHeader& header, const ByteSpan payload) noexcept
{
    if (!Fits(bytes, payload, 14U, MaximumFrameBytes) || (bytes.Length < MinimumTransmitFrameBytes) ||
        (header.EtherType < 0x0600U))
    {
        return 0U;
    }
    Copy(bytes.Data + 14U, payload.Data, payload.Length);
    Copy(bytes.Data, header.Destination.Octets, 6U);
    Copy(bytes.Data + 6U, header.Source.Octets, 6U);
    Put16(bytes.Data + 12U, header.EtherType);
    Size length{14U + payload.Length};
    while (length < MinimumTransmitFrameBytes)
    {
        bytes.Data[length] = 0U;
        ++length;
    }
    return length;
}

Size WriteArp(const MutableByteSpan bytes, const ArpPacket& packet) noexcept
{
    if ((bytes.Data == nullptr) || (bytes.Length < 28U) || ((packet.Operation != 1U) && (packet.Operation != 2U)))
    {
        return 0U;
    }
    Put16(bytes.Data, 1U);
    Put16(bytes.Data + 2U, Ipv4EtherType);
    bytes.Data[4U] = 6U;
    bytes.Data[5U] = 4U;
    Put16(bytes.Data + 6U, packet.Operation);
    Copy(bytes.Data + 8U, packet.SenderMac.Octets, 6U);
    Put32(bytes.Data + 14U, packet.SenderAddress);
    Copy(bytes.Data + 18U, packet.TargetMac.Octets, 6U);
    Put32(bytes.Data + 24U, packet.TargetAddress);
    return 28U;
}

Size WriteIpv4(const MutableByteSpan bytes, const Ipv4Header& header, const ByteSpan payload) noexcept
{
    if (!Fits(bytes, payload, 20U, 1500U) || (header.Ttl == 0U))
    {
        return 0U;
    }
    Copy(bytes.Data + 20U, payload.Data, payload.Length);
    bytes.Data[0U] = 0x45U;
    bytes.Data[1U] = 0U;
    Put16(bytes.Data + 2U, static_cast<UInt16>(20U + payload.Length));
    Put16(bytes.Data + 4U, header.Identification);
    Put16(bytes.Data + 6U, 0U);
    bytes.Data[8U] = header.Ttl;
    bytes.Data[9U] = header.Protocol;
    Put16(bytes.Data + 10U, 0U);
    Put32(bytes.Data + 12U, header.Source);
    Put32(bytes.Data + 16U, header.Destination);
    Put16(bytes.Data + 10U, InternetChecksum({.Data = bytes.Data, .Length = 20U}));
    return 20U + payload.Length;
}

Size WriteIcmpEchoReply(const MutableByteSpan bytes, const ByteSpan request) noexcept
{
    if ((request.Length > 1480U) || (ReadIcmpEcho(request) != PacketStatus::Done) || (bytes.Data == nullptr) ||
        (bytes.Length < request.Length))
    {
        return 0U;
    }
    Copy(bytes.Data, request.Data, request.Length);
    bytes.Data[0U] = 0U;
    Put16(bytes.Data + 2U, 0U);
    Put16(bytes.Data + 2U, InternetChecksum({.Data = bytes.Data, .Length = request.Length}));
    return request.Length;
}

Size WriteUdp(const MutableByteSpan bytes, const Ipv4Address source, const Ipv4Address destination,
              const UInt16 sourcePort, const UInt16 destinationPort, const ByteSpan payload) noexcept
{
    if (!Fits(bytes, payload, 8U, 1480U))
    {
        return 0U;
    }
    const UInt16 length = static_cast<UInt16>(8U + payload.Length);
    Copy(bytes.Data + 8U, payload.Data, payload.Length);
    Put16(bytes.Data, sourcePort);
    Put16(bytes.Data + 2U, destinationPort);
    Put16(bytes.Data + 4U, length);
    Put16(bytes.Data + 6U, 0U);
    const UInt16 checksum =
        InternetChecksum({.Data = bytes.Data, .Length = length}, PseudoSum(source, destination, length));
    Put16(bytes.Data + 6U, (checksum == 0U) ? UInt16{0xFFFFU} : checksum);
    return length;
}

bool NetworkStack::Configure(const MacAddress& mac, const Ipv4Address address, const Ipv4Address netmask) noexcept
{
    m_configured = false;
    m_mac = mac;
    m_address = address;
    m_netmask = netmask;
    m_ruleCount = 0U;
    m_bindingCount = 0U;
    for (Peer& peer : m_peers)
    {
        peer = {};
    }
    for (NetworkRule& rule : m_rules)
    {
        rule = {};
    }
    for (Binding& binding : m_bindings)
    {
        binding = {};
    }
    for (UInt64& count : m_drops)
    {
        count = 0U;
    }
    const UInt32 hostMask{~netmask};
    // hostMask + 1 wraps only for /0, which the preceding check rejects.
    if (!UnicastMac(mac) || !UnicastIp(address) || (netmask == 0U) || ((hostMask & (hostMask + 1U)) != 0U) ||
        ((hostMask > 1U) && (((address & hostMask) == 0U) || ((address & hostMask) == hostMask))))
    {
        return false;
    }
    m_configured = true;
    return true;
}

bool NetworkStack::InstallRule(const NetworkRule& rule) noexcept
{
    if (!m_configured || (rule.LocalPort == 0U))
    {
        return false;
    }
    for (Size index{0U}; index < m_ruleCount; ++index)
    {
        const NetworkRule& installed = m_rules[index];
        if ((installed.Sender == rule.Sender) && (installed.LocalPort == rule.LocalPort) &&
            (installed.RemoteAddress == rule.RemoteAddress) && (installed.RemotePort == rule.RemotePort))
        {
            return true;
        }
    }
    if (m_ruleCount == MaximumRules)
    {
        return false;
    }
    m_rules[m_ruleCount] = rule;
    ++m_ruleCount;
    return true;
}

bool NetworkStack::Allows(const UInt64 sender, const UInt16 port, const Ipv4Address remoteAddress,
                          const UInt16 remotePort, const bool binding) const noexcept
{
    for (Size index{0U}; index < m_ruleCount; ++index)
    {
        const NetworkRule& rule = m_rules[index];
        if ((rule.Sender == sender) && (rule.LocalPort == port) &&
            (binding || (((rule.RemoteAddress == 0U) || (rule.RemoteAddress == remoteAddress)) &&
                         ((rule.RemotePort == 0U) || (rule.RemotePort == remotePort)))))
        {
            return true;
        }
    }
    return false;
}

bool NetworkStack::Bind(const UInt64 sender, const UInt16 port) noexcept
{
    if (!m_configured || !Allows(sender, port, 0U, 0U, true))
    {
        return false;
    }
    for (Size index{0U}; index < m_bindingCount; ++index)
    {
        if (m_bindings[index].Port == port)
        {
            return m_bindings[index].Owner == sender;
        }
    }
    if (m_bindingCount == MaximumRules)
    {
        return false;
    }
    m_bindings[m_bindingCount] = {.Owner = sender, .Port = port};
    ++m_bindingCount;
    return true;
}

bool NetworkStack::IsPeer(const Ipv4Address address) const noexcept
{
    const UInt32 hostMask{~m_netmask};
    return UnicastIp(address) && (address != m_address) && ((address & m_netmask) == (m_address & m_netmask)) &&
           ((hostMask <= 1U) || (((address & hostMask) != 0U) && ((address & hostMask) != hostMask)));
}

void NetworkStack::Learn(const Ipv4Address address, const MacAddress& mac, const bool insert) noexcept
{
    if (!IsPeer(address) || !UnicastMac(mac))
    {
        return;
    }
    Size slot{MaximumArpEntries};
    for (Size index{0U}; index < MaximumArpEntries; ++index)
    {
        if (m_peers[index].Present && (m_peers[index].Address == address))
        {
            slot = index;
            break;
        }
    }
    if (slot == MaximumArpEntries)
    {
        if (!insert)
        {
            return;
        }
        slot = 0U;
        for (Size index{0U}; index < MaximumArpEntries; ++index)
        {
            if (!m_peers[index].Present)
            {
                slot = index;
                break;
            }
            if (m_peers[index].Age > m_peers[slot].Age)
            {
                slot = index;
            }
        }
    }
    // Bounded recency ranks avoid a wrapping clock/sequence counter.
    const UInt8 age = m_peers[slot].Present ? m_peers[slot].Age : UInt8{4U};
    for (Peer& peer : m_peers)
    {
        if (peer.Present && (peer.Age < age))
        {
            ++peer.Age;
        }
    }
    m_peers[slot] = {.Address = address, .Mac = mac, .Age = 0U, .Present = true};
}

bool NetworkStack::LookupPeer(const Ipv4Address address, MacAddress& mac) const noexcept
{
    for (const Peer& peer : m_peers)
    {
        if (peer.Present && (peer.Address == address))
        {
            mac = peer.Mac;
            return true;
        }
    }
    return false;
}

void NetworkStack::Drop(const DropReason reason) noexcept
{
    UInt64& count = m_drops[static_cast<Size>(reason)];
    // Diagnostic counts saturate rather than wrapping back to zero.
    if (count != ~UInt64{0U})
    {
        ++count;
    }
}

void NetworkStack::DropPacket(const PacketStatus status, const DropReason checksum) noexcept
{
    switch (status)
    {
    case PacketStatus::Done:
        break;
    case PacketStatus::Truncated:
        Drop(DropReason::Runt);
        break;
    case PacketStatus::Oversize:
        Drop(DropReason::Oversize);
        break;
    case PacketStatus::Malformed:
        Drop(DropReason::Malformed);
        break;
    case PacketStatus::Unsupported:
        Drop(DropReason::Unsupported);
        break;
    case PacketStatus::Options:
        Drop(DropReason::IpOptions);
        break;
    case PacketStatus::Fragmented:
        Drop(DropReason::Fragment);
        break;
    case PacketStatus::BadChecksum:
        Drop(checksum);
        break;
    case PacketStatus::Expired:
        Drop(DropReason::TtlExpired);
        break;
    }
}

void NetworkStack::EmitArp(const MacAddress& destination, const ArpPacket& packet, TransmitFrame& output) const noexcept
{
    const Size length = WriteArp({.Data = output.Bytes + 14U, .Length = MaximumFrameBytes - 14U}, packet);
    const EthernetHeader header{.Destination = destination, .Source = m_mac, .EtherType = ArpEtherType};
    output.Length = WriteEthernet({.Data = output.Bytes, .Length = MaximumFrameBytes}, header,
                                  {.Data = output.Bytes + 14U, .Length = length});
}

void NetworkStack::EmitIpv4(const MacAddress& destination, const UInt8 protocol, const Ipv4Address remoteAddress,
                            const Size length, TransmitFrame& output) const noexcept
{
    const Ipv4Header ip{
        .Source = m_address, .Destination = remoteAddress, .Identification = 0U, .Ttl = 64U, .Protocol = protocol};
    const Size ipLength = WriteIpv4({.Data = output.Bytes + 14U, .Length = MaximumFrameBytes - 14U}, ip,
                                    {.Data = output.Bytes + 34U, .Length = length});
    const EthernetHeader ethernet{.Destination = destination, .Source = m_mac, .EtherType = Ipv4EtherType};
    output.Length = WriteEthernet({.Data = output.Bytes, .Length = MaximumFrameBytes}, ethernet,
                                  {.Data = output.Bytes + 14U, .Length = ipLength});
}

void NetworkStack::ReceiveArp(const EthernetPacket& ethernet, ReceiveOutput& output) noexcept
{
    ArpPacket arp{};
    const PacketStatus status = ReadArp(ethernet.Payload, arp);
    if (status != PacketStatus::Done)
    {
        DropPacket(status, DropReason::Malformed);
        return;
    }
    if (!EqualMac(arp.SenderMac, ethernet.Header.Source) || !UnicastMac(arp.SenderMac))
    {
        Drop(DropReason::Malformed);
        return;
    }
    // RFC 826 merge: update a known sender even when the target is someone else;
    // insert an unknown sender only when the protocol target is our own address.
    Learn(arp.SenderAddress, arp.SenderMac, arp.TargetAddress == m_address);
    if (arp.TargetAddress != m_address)
    {
        Drop(DropReason::NotForUs);
        return;
    }
    if (arp.Operation == 1U)
    {
        const ArpPacket reply{.Operation = 2U,
                              .SenderMac = m_mac,
                              .SenderAddress = m_address,
                              .TargetMac = arp.SenderMac,
                              .TargetAddress = arp.SenderAddress};
        EmitArp(arp.SenderMac, reply, output.Frame);
    }
}

void NetworkStack::ReceiveIpv4(const EthernetPacket& ethernet, ReceiveOutput& output) noexcept
{
    Ipv4Packet ip{};
    const PacketStatus status = ReadIpv4(ethernet.Payload, ip);
    if (status != PacketStatus::Done)
    {
        DropPacket(status, DropReason::BadIpChecksum);
        return;
    }
    if ((ip.Header.Destination != m_address) || !EqualMac(ethernet.Header.Destination, m_mac))
    {
        Drop(DropReason::NotForUs);
        return;
    }
    if (!IsPeer(ip.Header.Source))
    {
        Drop(DropReason::OffLink);
        return;
    }
    if (!UnicastMac(ethernet.Header.Source))
    {
        Drop(DropReason::Malformed);
        return;
    }
    Learn(ip.Header.Source, ethernet.Header.Source, true);
    if (ip.Header.Protocol == 1U)
    {
        const PacketStatus echo = ReadIcmpEcho(ip.Payload);
        if (echo != PacketStatus::Done)
        {
            DropPacket(echo, DropReason::BadIcmpChecksum);
            return;
        }
        const Size length =
            WriteIcmpEchoReply({.Data = output.Frame.Bytes + 34U, .Length = MaximumFrameBytes - 34U}, ip.Payload);
        EmitIpv4(ethernet.Header.Source, 1U, ip.Header.Source, length, output.Frame);
        return;
    }
    if (ip.Header.Protocol == 17U)
    {
        UdpPacket udp{};
        const PacketStatus datagram = ReadUdp(ip.Payload, ip.Header.Source, ip.Header.Destination, udp);
        if (datagram != PacketStatus::Done)
        {
            DropPacket(datagram, DropReason::BadUdpChecksum);
            return;
        }
        for (Size index{0U}; index < m_bindingCount; ++index)
        {
            if (m_bindings[index].Port == udp.DestinationPort)
            {
                const UInt64 owner = m_bindings[index].Owner;
                if (!Allows(owner, udp.DestinationPort, ip.Header.Source, udp.SourcePort, false))
                {
                    Drop(DropReason::Unauthorized);
                    return;
                }
                output.Datagram = {.Owner = owner,
                                   .SourceAddress = ip.Header.Source,
                                   .SourcePort = udp.SourcePort,
                                   .LocalPort = udp.DestinationPort,
                                   .Payload = udp.Payload,
                                   .Present = true};
                return;
            }
        }
        Drop(DropReason::UnboundPort);
        return;
    }
    Drop(DropReason::Unsupported);
}

void NetworkStack::Receive(const UInt8* const frame, const Size length, ReceiveOutput& output) noexcept
{
    output.Frame.Length = 0U;
    output.Datagram = {};
    if (!m_configured)
    {
        Drop(DropReason::Unconfigured);
        return;
    }
    EthernetPacket ethernet{};
    const PacketStatus status = ReadEthernet({.Data = frame, .Length = length}, ethernet);
    if (status != PacketStatus::Done)
    {
        DropPacket(status, DropReason::Malformed);
        return;
    }
    if (!EqualMac(ethernet.Header.Destination, m_mac) && !EqualMac(ethernet.Header.Destination, Broadcast))
    {
        Drop(DropReason::NotForUs);
        return;
    }
    if (ethernet.Header.EtherType == ArpEtherType)
    {
        ReceiveArp(ethernet, output);
    }
    else if (ethernet.Header.EtherType == Ipv4EtherType)
    {
        ReceiveIpv4(ethernet, output);
    }
    else
    {
        Drop(DropReason::Unsupported);
    }
}

SendStatus NetworkStack::SendTo(const UInt64 sender, const UInt16 localPort, const Ipv4Address remoteAddress,
                                const UInt16 remotePort, const UInt8* const payload, const Size length,
                                TransmitFrame& output) noexcept
{
    output.Length = 0U;
    if (!m_configured || (remotePort == 0U) || (length > MaximumUdpPayloadBytes) ||
        ((payload == nullptr) && (length != 0U)) || !IsPeer(remoteAddress) ||
        !Allows(sender, localPort, remoteAddress, remotePort, false))
    {
        return SendStatus::Refused;
    }
    MacAddress destination{};
    if (!LookupPeer(remoteAddress, destination))
    {
        const ArpPacket request{.Operation = 1U,
                                .SenderMac = m_mac,
                                .SenderAddress = m_address,
                                .TargetMac = ZeroMac,
                                .TargetAddress = remoteAddress};
        EmitArp(Broadcast, request, output);
        return SendStatus::Pending;
    }
    const Size udpLength = WriteUdp({.Data = output.Bytes + 34U, .Length = MaximumFrameBytes - 34U}, m_address,
                                    remoteAddress, localPort, remotePort, {.Data = payload, .Length = length});
    EmitIpv4(destination, 17U, remoteAddress, udpLength, output);
    return SendStatus::Sent;
}

UInt64 NetworkStack::GetDropCount(const DropReason reason) const noexcept
{
    const Size index = static_cast<Size>(reason);
    return (index < static_cast<Size>(DropReason::Count)) ? m_drops[index] : 0U;
}

Size NetworkStack::GetArpEntryCount() const noexcept
{
    Size count{0U};
    for (const Peer& peer : m_peers)
    {
        if (peer.Present)
        {
            ++count;
        }
    }
    return count;
}

Size NetworkStack::GetRuleCount() const noexcept
{
    return m_ruleCount;
}

Size NetworkStack::GetBindingCount() const noexcept
{
    return m_bindingCount;
}

bool NetworkStack::IsBound(const UInt64 owner, const UInt16 port) const noexcept
{
    for (Size index{0U}; index < m_bindingCount; ++index)
    {
        if (m_bindings[index].Port == port)
        {
            return m_bindings[index].Owner == owner;
        }
    }
    return false;
}
} // namespace Asteria::User::Network
