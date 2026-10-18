// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_NETWORK_PACKET_HPP_INCLUDED
#define ASTERIA_USER_NETWORK_PACKET_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Network
{
inline constexpr Foundation::Size MaximumFrameBytes{1514U};
inline constexpr Foundation::Size MinimumTransmitFrameBytes{60U};
inline constexpr Foundation::Size MaximumUdpPayloadBytes{1472U};
inline constexpr Foundation::UInt16 Ipv4EtherType{0x0800U};
inline constexpr Foundation::UInt16 ArpEtherType{0x0806U};

/// Numeric network order: 10.0.2.15 is 0x0A00020FU, independently of host endianness.
using Ipv4Address = Foundation::UInt32;

struct MacAddress
{
    Foundation::UInt8 Octets[6U];
};

struct ByteSpan
{
    const Foundation::UInt8* Data;
    Foundation::Size Length;
};

struct MutableByteSpan
{
    Foundation::UInt8* Data;
    Foundation::Size Length;
};

enum class PacketStatus : Foundation::UInt8
{
    Done,
    Truncated,
    Oversize,
    Malformed,
    Unsupported,
    Options,
    Fragmented,
    BadChecksum,
    Expired,
};

struct EthernetHeader
{
    MacAddress Destination;
    MacAddress Source;
    Foundation::UInt16 EtherType;
};

struct EthernetPacket
{
    EthernetHeader Header;
    ByteSpan Payload;
};

struct ArpPacket
{
    Foundation::UInt16 Operation;
    MacAddress SenderMac;
    Ipv4Address SenderAddress;
    MacAddress TargetMac;
    Ipv4Address TargetAddress;
};

struct Ipv4Header
{
    Ipv4Address Source;
    Ipv4Address Destination;
    Foundation::UInt16 Identification;
    Foundation::UInt8 Ttl;
    Foundation::UInt8 Protocol;
};

struct Ipv4Packet
{
    Ipv4Header Header;
    ByteSpan Payload;
};

struct UdpPacket
{
    Foundation::UInt16 SourcePort;
    Foundation::UInt16 DestinationPort;
    ByteSpan Payload;
};

/// Pure bounded codecs, no allocation or system calls. Spans must denote accessible
/// storage; nullptr is allowed only for an empty payload. Read failures leave output
/// unchanged. Successful decoded payloads borrow the input until the caller reuses it.
/// Writers return bytes written, or zero without modifying storage on invalid input.
/// Input/output storage must be disjoint, or payload already at its exact output offset.
/// Ethernet includes padding but never an FCS; IP/UDP lengths exclude trailing bytes.
[[nodiscard]] bool EqualMac(const MacAddress& left, const MacAddress& right) noexcept;

/// RFC 1071 one's-complement checksum, odd final octet in the high byte.
/// initialSum is an unfolded sum (e.g. UDP pseudo-header); valid messages yield zero.
/// The span must be readable (including when used independently of the codecs).
[[nodiscard]] Foundation::UInt16 InternetChecksum(ByteSpan bytes, Foundation::UInt32 initialSum = 0U) noexcept;

[[nodiscard]] PacketStatus ReadEthernet(ByteSpan bytes, EthernetPacket& packet) noexcept;
[[nodiscard]] PacketStatus ReadArp(ByteSpan bytes, ArpPacket& packet) noexcept;
[[nodiscard]] PacketStatus ReadIpv4(ByteSpan bytes, Ipv4Packet& packet) noexcept;
[[nodiscard]] PacketStatus ReadIcmpEcho(ByteSpan bytes) noexcept;
[[nodiscard]] PacketStatus ReadUdp(ByteSpan bytes, Ipv4Address source, Ipv4Address destination,
                                   UdpPacket& packet) noexcept;

[[nodiscard]] Foundation::Size WriteEthernet(MutableByteSpan bytes, const EthernetHeader& header,
                                             ByteSpan payload) noexcept;
[[nodiscard]] Foundation::Size WriteArp(MutableByteSpan bytes, const ArpPacket& packet) noexcept;
[[nodiscard]] Foundation::Size WriteIpv4(MutableByteSpan bytes, const Ipv4Header& header, ByteSpan payload) noexcept;
[[nodiscard]] Foundation::Size WriteIcmpEchoReply(MutableByteSpan bytes, ByteSpan request) noexcept;
[[nodiscard]] Foundation::Size WriteUdp(MutableByteSpan bytes, Ipv4Address source, Ipv4Address destination,
                                        Foundation::UInt16 sourcePort, Foundation::UInt16 destinationPort,
                                        ByteSpan payload) noexcept;
} // namespace Asteria::User::Network

#endif // ASTERIA_USER_NETWORK_PACKET_HPP_INCLUDED
