// SPDX-License-Identifier: Apache-2.0
#include "User/Network/NetworkStack.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Network/PacketFixtures.hpp"

namespace Asteria::Test::Network
{
namespace
{
using TestSupport::TestContext;

// Model peers are kept newest-first, unlike the production table's age ranks.
// Rules and port owners form independent tables; no production codecs are used.
struct Model
{
    Net::NetworkRule Rules[8U];
    UInt64 Owners[4U];
    bool Bound[4U];
    UInt8 Peers[4U];
    UInt8 MacTails[16U];
    Size RuleCount;
    Size PeerCount;
};

Model g_model;
Net::NetworkStack g_stack;
Net::ReceiveOutput g_received;
Net::TransmitFrame g_sent;
UInt8 g_input[60U];

[[nodiscard]] bool SameRule(const Net::NetworkRule& left, const Net::NetworkRule& right) noexcept
{
    return (left.Sender == right.Sender) && (left.LocalPort == right.LocalPort) &&
           (left.RemoteAddress == right.RemoteAddress) && (left.RemotePort == right.RemotePort);
}

[[nodiscard]] bool ModelInstall(const Net::NetworkRule& rule) noexcept
{
    for (Size index{0U}; index < g_model.RuleCount; ++index)
    {
        if (SameRule(rule, g_model.Rules[index]))
        {
            return true;
        }
    }
    if (g_model.RuleCount == 8U)
    {
        return false;
    }
    g_model.Rules[g_model.RuleCount] = rule;
    ++g_model.RuleCount;
    return true;
}

[[nodiscard]] bool Allowed(const UInt64 sender, const UInt16 port, const UInt32 address, const UInt16 remotePort,
                           const bool binding) noexcept
{
    bool allowed{false};
    for (Size index{0U}; index < g_model.RuleCount; ++index)
    {
        const Net::NetworkRule& rule = g_model.Rules[index];
        if ((rule.Sender == sender) && (rule.LocalPort == port))
        {
            allowed = allowed || binding ||
                      (((rule.RemoteAddress == 0U) || (rule.RemoteAddress == address)) &&
                       ((rule.RemotePort == 0U) || (rule.RemotePort == remotePort)));
        }
    }
    return allowed;
}

[[nodiscard]] bool HasPeer(const UInt8 peer) noexcept
{
    for (Size index{0U}; index < g_model.PeerCount; ++index)
    {
        if (g_model.Peers[index] == peer)
        {
            return true;
        }
    }
    return false;
}

void ModelLearn(const UInt8 peer, const UInt8 mac, const bool targeted) noexcept
{
    Size at{0U};
    while ((at < g_model.PeerCount) && (g_model.Peers[at] != peer))
    {
        ++at;
    }
    if ((at == g_model.PeerCount) && !targeted)
    {
        return;
    }
    if (at == g_model.PeerCount)
    {
        if (g_model.PeerCount < 4U)
        {
            ++g_model.PeerCount;
        }
        at = g_model.PeerCount - 1U;
    }
    while (at > 0U)
    {
        g_model.Peers[at] = g_model.Peers[at - 1U];
        --at;
    }
    g_model.Peers[0U] = peer;
    g_model.MacTails[peer] = mac;
}

[[nodiscard]] bool SnapshotMatches() noexcept
{
    Size bound{0U};
    for (const bool present : g_model.Bound)
    {
        bound += present ? 1U : 0U;
    }
    if ((g_stack.GetRuleCount() != g_model.RuleCount) || (g_stack.GetBindingCount() != bound) ||
        (g_stack.GetArpEntryCount() != g_model.PeerCount))
    {
        return false;
    }
    for (Size slot{0U}; slot < 4U; ++slot)
    {
        for (UInt64 sender{0xFFFF000000000000U}; sender < 0xFFFF000000000003U; ++sender)
        {
            if (g_stack.IsBound(sender, static_cast<UInt16>(7U + slot)) !=
                (g_model.Bound[slot] && (g_model.Owners[slot] == sender)))
            {
                return false;
            }
        }
    }
    for (UInt8 peer{1U}; peer <= 12U; ++peer)
    {
        Net::MacAddress mac{};
        const bool found = g_stack.LookupPeer(0x0A000200U + peer, mac);
        if ((found != HasPeer(peer)) || (found && (mac.Octets[5U] != g_model.MacTails[peer])))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool Step(TestSupport::Random& random, const Size operation) noexcept
{
    const UInt64 sender{0xFFFF000000000000U + random.Below(3U)};
    const Size slot = static_cast<Size>(random.Below(4U));
    const UInt16 port = static_cast<UInt16>(7U + slot);
    const UInt8 peer = static_cast<UInt8>(1U + random.Below(12U));
    const UInt32 address{0x0A000200U + peer};
    const UInt16 remotePort = static_cast<UInt16>(9000U + random.Below(2U));
    if (operation == 0U)
    {
        const Net::NetworkRule rule{.Sender = sender,
                                    .LocalPort = port,
                                    .RemoteAddress = random.Chance(1U, 2U) ? 0U : address,
                                    .RemotePort = random.Chance(1U, 2U) ? UInt16{0U} : remotePort};
        return g_stack.InstallRule(rule) == ModelInstall(rule);
    }
    if (operation == 1U)
    {
        bool expected = Allowed(sender, port, 0U, 0U, true);
        if (expected && g_model.Bound[slot])
        {
            expected = g_model.Owners[slot] == sender;
        }
        if (expected)
        {
            g_model.Bound[slot] = true;
            g_model.Owners[slot] = sender;
        }
        return g_stack.Bind(sender, port) == expected;
    }
    if ((operation == 2U) || (operation == 3U))
    {
        const bool targeted = random.Chance(3U, 4U);
        const UInt8 tail = static_cast<UInt8>(random.Below(256U));
        std::memcpy(g_input, ArpRequest, 60U);
        g_input[11U] = tail;
        g_input[27U] = tail;
        Put32(g_input + 28U, address);
        Put32(g_input + 38U, targeted ? OwnAddress : OwnAddress + 1U);
        g_input[21U] = (operation == 2U) ? 1U : 2U;
        ModelLearn(peer, tail, targeted);
        g_stack.Receive(g_input, 60U, g_received);
        const bool reply = targeted && (operation == 2U);
        return (g_received.Frame.Length == (reply ? 60U : 0U)) && !g_received.Datagram.Present &&
               (!reply || ((Get16(g_received.Frame.Bytes + 20U) == 2U) && (g_received.Frame.Bytes[5U] == tail)));
    }
    if (operation == 4U)
    {
        const bool allowed = Allowed(sender, port, address, remotePort, false);
        const Net::SendStatus expected =
            !allowed ? Net::SendStatus::Refused : (HasPeer(peer) ? Net::SendStatus::Sent : Net::SendStatus::Pending);
        const Net::SendStatus actual = g_stack.SendTo(sender, port, address, remotePort, nullptr, 0U, g_sent);
        return (actual == expected) &&
               ((expected == Net::SendStatus::Refused)
                    ? (g_sent.Length == 0U)
                    : ((expected == Net::SendStatus::Pending)
                           ? ((g_sent.Length == 60U) && (Get16(g_sent.Bytes + 12U) == 0x0806U) &&
                              (g_sent.Bytes[41U] == peer))
                           : (CheckIpv4Frame(g_sent, 17U) && (g_sent.Bytes[5U] == g_model.MacTails[peer]))));
    }
    if (operation == 5U)
    {
        // UdpIncoming's source port is 9000; the owner's rules for the port must allow the source.
        std::memcpy(g_input, UdpIncoming, 60U);
        Put32(g_input + 26U, address);
        Put16(g_input + 36U, port);
        Put16(g_input + 40U, 0U);
        FixIpChecksum(g_input);
        ModelLearn(peer, 2U, true);
        const bool bound = g_model.Bound[slot];
        const bool delivered = bound && Allowed(g_model.Owners[slot], port, address, 9000U, false);
        const UInt64 unauthorized =
            g_stack.GetDropCount(Net::DropReason::Unauthorized) + ((bound && !delivered) ? 1U : 0U);
        g_stack.Receive(g_input, 60U, g_received);
        return (g_received.Frame.Length == 0U) && (g_received.Datagram.Present == delivered) &&
               (g_stack.GetDropCount(Net::DropReason::Unauthorized) == unauthorized) &&
               (!delivered ||
                ((g_received.Datagram.Owner == g_model.Owners[slot]) && (g_received.Datagram.Payload.Length == 5U)));
    }
    g_model = {};
    return g_stack.Configure(OwnMac, OwnAddress, Netmask);
}

void TestModel(TestContext& context) noexcept
{
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(1000U, 32U);
    for (UInt64 seed{seeds.First}; seed - seeds.First < seeds.Count; ++seed)
    {
        TestSupport::Random random{seed};
        g_model = {};
        ASTERIA_EXPECT(context, "model initial configure", g_stack.Configure(OwnMac, OwnAddress, Netmask));
        for (Size step{0U}; step < 2000U; ++step)
        {
            // Infrequent resets let rules/peers fill and replace many times.
            const Size operation = random.Chance(1U, 100U) ? 6U : static_cast<Size>(random.Below(6U));
            const bool same = Step(random, operation) && SnapshotMatches();
            if (!same)
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu operation=%zu\n", seed, step, operation));
            }
            ASTERIA_EXPECT(context, "model result, owners, rule count and peer snapshot agree", same);
            if (!same)
            {
                return;
            }
        }
    }
}
} // Namespace
} // Namespace Asteria::Test::Network

int main() noexcept
{
    Asteria::TestSupport::TestContext context{"NetworkStackModelTests"};
    Asteria::Test::Network::TestModel(context);
    return context.Finish();
}
