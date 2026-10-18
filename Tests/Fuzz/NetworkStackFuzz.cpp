// SPDX-License-Identifier: Apache-2.0
#include "User/Network/NetworkStack.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "User/Network/PacketFixtures.hpp"

namespace
{
namespace Fixtures = Asteria::Test::Network;
namespace Net = Asteria::User::Network;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;

Net::NetworkStack g_stack;
Net::ReceiveOutput g_output;
UInt8 g_seed[Net::MaximumFrameBytes + 1U];

void Require(const bool condition) noexcept
{
    if (!condition)
    {
        std::abort();
    }
}

[[nodiscard]] UInt64 Drops() noexcept
{
    UInt64 total{0U};
    for (Size index{0U}; index < static_cast<Size>(Net::DropReason::Count); ++index)
    {
        total += g_stack.GetDropCount(static_cast<Net::DropReason>(index));
    }
    return total;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    Require(g_stack.Configure(Fixtures::OwnMac, Fixtures::OwnAddress, Fixtures::Netmask));
    Require(g_stack.InstallRule(Fixtures::EchoRule));
    Require(g_stack.Bind(17U, 7U));
    // Repeated arbitrary receives exercise both insertion and RFC 826 merge.
    for (Size iteration{0U}; iteration < 2U; ++iteration)
    {
        const UInt64 before = Drops();
        g_stack.Receive(data, size, g_output);
        Require(g_output.Frame.Length <= Net::MaximumFrameBytes);
        Require((g_output.Frame.Length == 0U) || (g_output.Frame.Length >= 60U));
        Require((g_output.Frame.Length == 0U) || !g_output.Datagram.Present);
        Require(g_stack.GetArpEntryCount() <= 4U);
        Require(Drops() <= before + 1U);
        if (g_output.Frame.Length != 0U)
        {
            const auto type = Fixtures::Get16(g_output.Frame.Bytes + 12U);
            Require((type == 0x0806U) || ((type == 0x0800U) && Fixtures::CheckIpv4Frame(g_output.Frame, 1U)));
            Require(Drops() == before);
        }
        if (g_output.Datagram.Present)
        {
            Require(g_output.Datagram.Owner == 17U);
            Require(g_output.Datagram.LocalPort == 7U);
            // Only a source the owner's rule allows is delivered.
            Require(g_output.Datagram.SourceAddress == Fixtures::EchoRule.RemoteAddress);
            Require(g_output.Datagram.SourcePort == Fixtures::EchoRule.RemotePort);
            Require(g_output.Datagram.Payload.Length <= Net::MaximumUdpPayloadBytes);
            Require(size >= 42U);
            Require(g_output.Datagram.Payload.Data == data + 42U);
            Require(g_output.Datagram.Payload.Length <= size - 42U);
            Require(Drops() == before);
        }
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size)) noexcept
{
    emit(Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    emit(Fixtures::PeerArpReply, sizeof(Fixtures::PeerArpReply));
    emit(Fixtures::IcmpRequest, sizeof(Fixtures::IcmpRequest));
    emit(Fixtures::UdpIncoming, sizeof(Fixtures::UdpIncoming));
    emit(Fixtures::SyntheticUdp, sizeof(Fixtures::SyntheticUdp));
    emit(Fixtures::UdpIncoming, 13U);
    emit(Fixtures::UdpIncoming, 0U);
    std::memcpy(g_seed, Fixtures::UdpIncoming, 60U);
    g_seed[40U] = 0U;
    g_seed[41U] = 0U;
    emit(g_seed, 60U);
    // A source port the rule does not allow: an Unauthorized drop.
    g_seed[35U] = 0x29U;
    emit(g_seed, 60U);
    std::memset(g_seed, 0, sizeof(g_seed));
    emit(g_seed, sizeof(g_seed));
}
