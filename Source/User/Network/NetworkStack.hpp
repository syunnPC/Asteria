// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_NETWORK_NETWORK_STACK_HPP_INCLUDED
#define ASTERIA_USER_NETWORK_NETWORK_STACK_HPP_INCLUDED

#include "User/Network/Packet.hpp"

namespace Asteria::User::Network
{
inline constexpr Foundation::Size MaximumRules{8U};
inline constexpr Foundation::Size MaximumArpEntries{4U};

struct NetworkRule
{
    Foundation::UInt64 Sender;
    Foundation::UInt16 LocalPort;
    /// Zero means any remote address/port; local port zero is invalid.
    Ipv4Address RemoteAddress;
    Foundation::UInt16 RemotePort;
};

struct TransmitFrame
{
    Foundation::UInt8 Bytes[MaximumFrameBytes];
    Foundation::Size Length;
};

struct DeliveredDatagram
{
    Foundation::UInt64 Owner;
    Ipv4Address SourceAddress;
    Foundation::UInt16 SourcePort;
    Foundation::UInt16 LocalPort;
    ByteSpan Payload;
    /// Also distinguishes an empty datagram from no delivery.
    bool Present;
};

struct ReceiveOutput
{
    TransmitFrame Frame;
    DeliveredDatagram Datagram;
};

enum class SendStatus : Foundation::UInt8
{
    Sent,
    Pending,
    Refused,
};

enum class DropReason : Foundation::UInt8
{
    Unconfigured,
    Runt,
    Oversize,
    NotForUs,
    Unsupported,
    Malformed,
    BadIpChecksum,
    BadIcmpChecksum,
    BadUdpChecksum,
    IpOptions,
    Fragment,
    TtlExpired,
    OffLink,
    UnboundPort,
    /// A datagram to a bound port from a source no rule of the port's owner allows
    /// (Source/User/Network/NetworkStack.hpp: a rule filters both directions).
    Unauthorized,
    Count,
};

/// Network service contract, pure service core. All operations are bounded, nonblocking,
/// allocation-free, and require serialized access (not ISR shared). Keep this and
/// ReceiveOutput/TransmitFrame in static storage in a component: zero initialization needs no.data
/// or dynamic initialization. Configure resets peers, rules, bindings and counters. Before
/// Configure the stack refuses traffic. No timeouts or queued sends: Pending emits ARP and the
/// caller retries SendTo.
class NetworkStack final
{
public:
    constexpr NetworkStack() noexcept = default;
    NetworkStack(const NetworkStack&) = delete;
    NetworkStack& operator=(const NetworkStack&) = delete;

    /// Rejects zero/multicast MAC/IP, noncontiguous/zero netmasks, and subnet
    /// network/broadcast addresses (except /31 and /32). Invalid configuration
    /// resets the stack into its unconfigured state.
    [[nodiscard]] bool Configure(const MacAddress& mac, Ipv4Address address, Ipv4Address netmask) noexcept;

    /// Trusted management interface: callers must authenticate
    /// Manage and derive Sender from the kernel delivery identity. Exact duplicates
    /// are idempotent; other rules consume one of eight slots. No replacement.
    [[nodiscard]] bool InstallRule(const NetworkRule& rule) noexcept;
    /// One owner per port; repeat binding by that owner is idempotent.
    [[nodiscard]] bool Bind(Foundation::UInt64 sender, Foundation::UInt16 port) noexcept;

    /// Clears output metadata on every call; input and output must not overlap.
    /// Datagram payload borrows frame; copy it before reusing the input storage.
    /// A valid targeted ARP request emits one reply; echo emits one reply; UDP
    /// delivers one datagram to its port's owner when a rule of the owner for
    /// that port allows the source address and port (as SendTo checks the
    /// destination), else drops it as Unauthorized. Targeted ARP replies only
    /// learn the sender. Rejected traffic increments one drop reason.
    void Receive(const Foundation::UInt8* frame, Foundation::Size length, ReceiveOutput& output) noexcept;

    /// Does not require Bind: rules authorize sending, Bind selects receive owner.
    /// Refused sets output.Length to zero; Pending emits one broadcast ARP request.
    /// Only on-link unicast peers are accepted; no gateway or fragmentation.
    /// Payload/output follow the codecs' overlap contract (UDP offset 42).
    [[nodiscard]] SendStatus SendTo(Foundation::UInt64 sender, Foundation::UInt16 localPort, Ipv4Address remoteAddress,
                                    Foundation::UInt16 remotePort, const Foundation::UInt8* payload,
                                    Foundation::Size length, TransmitFrame& output) noexcept;

    [[nodiscard]] Foundation::UInt64 GetDropCount(DropReason reason) const noexcept;
    [[nodiscard]] Foundation::Size GetArpEntryCount() const noexcept;
    [[nodiscard]] Foundation::Size GetRuleCount() const noexcept;
    [[nodiscard]] Foundation::Size GetBindingCount() const noexcept;
    /// Whether owner holds port's binding.
    [[nodiscard]] bool IsBound(Foundation::UInt64 owner, Foundation::UInt16 port) const noexcept;
    [[nodiscard]] bool LookupPeer(Ipv4Address address, MacAddress& mac) const noexcept;

private:
    struct Peer
    {
        Ipv4Address Address;
        MacAddress Mac;
        Foundation::UInt8 Age;
        bool Present;
    };

    struct Binding
    {
        Foundation::UInt64 Owner;
        Foundation::UInt16 Port;
    };

    [[nodiscard]] bool IsPeer(Ipv4Address address) const noexcept;
    [[nodiscard]] bool Allows(Foundation::UInt64 sender, Foundation::UInt16 port, Ipv4Address remoteAddress,
                              Foundation::UInt16 remotePort, bool binding) const noexcept;
    void Learn(Ipv4Address address, const MacAddress& mac, bool insert) noexcept;
    void Drop(DropReason reason) noexcept;
    void DropPacket(PacketStatus status, DropReason checksum) noexcept;
    void ReceiveArp(const EthernetPacket& ethernet, ReceiveOutput& output) noexcept;
    void ReceiveIpv4(const EthernetPacket& ethernet, ReceiveOutput& output) noexcept;
    void EmitArp(const MacAddress& destination, const ArpPacket& packet, TransmitFrame& output) const noexcept;
    void EmitIpv4(const MacAddress& destination, Foundation::UInt8 protocol, Ipv4Address remoteAddress,
                  Foundation::Size length, TransmitFrame& output) const noexcept;

    MacAddress m_mac{};
    Ipv4Address m_address{};
    Ipv4Address m_netmask{};
    Peer m_peers[MaximumArpEntries]{};
    NetworkRule m_rules[MaximumRules]{};
    Binding m_bindings[MaximumRules]{};
    Foundation::UInt64 m_drops[static_cast<Foundation::Size>(DropReason::Count)]{};
    Foundation::Size m_ruleCount{};
    Foundation::Size m_bindingCount{};
    bool m_configured{};
};
} // Namespace Asteria::User::Network

#endif // ASTERIA_USER_NETWORK_NETWORK_STACK_HPP_INCLUDED
