// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/SocketFrame.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
// The stack's address and the peer the runner plays (Source/User/Network/NetworkStack.hpp;
// Tools/Vm/AsteriaVm/Lan.py): on-link 10.0.2.0/24.
constexpr Foundation::UInt64 StackAddress{0x0A00'020FU};
constexpr Foundation::UInt64 StackPrefix{24U};
constexpr Foundation::UInt64 PeerAddress{0x0A00'0202U};
constexpr Foundation::UInt64 PeerPort{40'000U};
// A destination no rule allows: a send to it puts no frame on the wire.
constexpr Foundation::UInt64 RefusedAddress{0x0A00'0203U};
constexpr Foundation::UInt64 RefusedPort{9U};
constexpr Foundation::UInt64 EchoPort{7U};
// The runner's NIC (Tools/Vm/AsteriaVm/Machine.py NIC_MAC 52:54:00:a5:7e:01,
// first octet in the low byte): elsewhere, on a real machine or another
// hypervisor, no peer plays the runner's part, so the exchange is skipped.
constexpr Foundation::UInt64 RunnerNicMac{0x01'7E'A5'00'54'52U};
constexpr char ResolveData[] = "asteria-resolve";
constexpr char RefusedData[] = "asteria-refused";
// How long the root service lets a service block again before it kills it.
constexpr Foundation::UInt64 KillPauseMicroseconds{100'000U};
// A Pending send is resent after a pause: the stack's ARP request and the
// peer's reply take a few milliseconds.
constexpr Foundation::UInt64 PendingPauseMicroseconds{100'000U};
constexpr Foundation::Size PendingAttempts{50U};
// Each AwaitMessage lasts at most 2 s: a reply comes at once, the peer's
// Datagram within a minute.
constexpr Foundation::Size ReplyWaits{4U};
constexpr Foundation::Size DatagramWaits{30U};

/// What the root service holds as a client of the stack: its reply Endpoint
/// and the next transaction.
struct SocketClient
{
    System::CapabilityHandle Reply;
    Foundation::UInt64 Transaction;
};

/// One reply: its kind, detail and extra, and the inline bytes in the
/// context's delivery buffer; zero kind for none.
struct SocketReply
{
    Foundation::UInt64 Kind;
    Foundation::UInt64 Detail;
    Foundation::UInt64 Extra;
};

constexpr SocketReply NoReply{.Kind = 0U, .Detail = 0U, .Extra = 0U};

// The decoded reply of the last await, its inline bytes included, and the
// datagram the echo sends back.
constinit Component::SocketMessage g_reply{};
alignas(8) constinit Foundation::UInt8 g_datagram[Component::SocketInlineBytes]{};

// The reply Endpoint: the default limits, but SocketReplyCapacity messages
// per sender and no attachments.
[[nodiscard, gnu::noinline]] bool CreateReplyEndpoint(ScenarioContext& context, SocketClient& client,
                                                      Abi::SystemCallError& systemCall) noexcept
{
    ObjectCalls& calls = *context.Calls;
    calls.ClearRequest();
    Foundation::UInt8(&request)[Abi::ObjectCreateEndpointRequestSizeBytes] =
        *reinterpret_cast<Foundation::UInt8(*)[Abi::ObjectCreateEndpointRequestSizeBytes]>(calls.GetRequest());
    System::BuildDefaultEndpointRequest(request);
    constexpr Foundation::UInt16 bytesPerSender{
        static_cast<Foundation::UInt16>(Component::SocketReplyCapacity * Component::EchoMaximumFrameBytes)};
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset] = static_cast<Foundation::UInt8>(bytesPerSender);
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset + 1U] = static_cast<Foundation::UInt8>(bytesPerSender >> 8U);
    request[Abi::ObjectCreateEndpointMessagesPerSenderOffset] =
        static_cast<Foundation::UInt8>(Component::SocketReplyCapacity);
    request[Abi::ObjectCreateEndpointAttachmentsPerSenderOffset] = 0U;
    return calls.Create(context.Management, Abi::ObjectTypeEndpoint, Abi::ObjectCreateEndpointRequestSizeBytes,
                        client.Reply, systemCall);
}

// Sends a request of method with the inline bytes; its transaction into
// sent. The stack's own creator capability carries Manage (a Rule needs it).
[[nodiscard, gnu::noinline]] bool SendSocket(ScenarioContext& context, const ComponentRecord& stack,
                                             SocketClient& client, const Foundation::UInt32 method,
                                             const Foundation::UInt64 (&words)[Component::SocketPayloadWords],
                                             const Foundation::UInt8* const bytes, const Foundation::Size length,
                                             Foundation::UInt64& sent, Abi::SystemCallError& systemCall) noexcept
{
    sent = ++client.Transaction;
    const Foundation::Size size =
        Component::WriteSocketFrame(context.Frame, method, sent, words, bytes, length, &client.Reply);
    return (size != 0U) && Check(context.Client->Send(stack.Service, AddressOf(context.Frame), size), systemCall);
}

// The reply to sent while the stack runs, into g_reply; replies to other
// transactions (a second Receive's datagram) are passed over. Zero kind if
// none came.
[[nodiscard, gnu::noinline]] SocketReply AwaitSocketReply(ScenarioContext& context, const ComponentRecord& stack,
                                                          const SocketClient& client, const Foundation::UInt64 sent,
                                                          const Foundation::Size waits,
                                                          Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::Size wait{0U}; (wait < waits) && (stack.State == ComponentState::Running); ++wait)
    {
        const Foundation::UInt64 received = AwaitMessage(context, stack, client.Reply, systemCall);
        if (received == 0U)
        {
            continue;
        }
        if (Component::ReadSocketFrame(context.Delivery, static_cast<Foundation::Size>(received), g_reply) &&
            (g_reply.Method == Component::SocketReplyMethod) && (g_reply.Words[0U] == sent))
        {
            return SocketReply{.Kind = g_reply.Words[1U], .Detail = g_reply.Words[2U], .Extra = g_reply.Words[3U]};
        }
    }
    return NoReply;
}

[[nodiscard]] SocketReply Request(ScenarioContext& context, const ComponentRecord& stack, SocketClient& client,
                                  const Foundation::UInt32 method,
                                  const Foundation::UInt64 (&words)[Component::SocketPayloadWords],
                                  Abi::SystemCallError& systemCall) noexcept
{
    Foundation::UInt64 sent{0U};
    return SendSocket(context, stack, client, method, words, nullptr, 0U, sent, systemCall)
               ? AwaitSocketReply(context, stack, client, sent, ReplyWaits, systemCall)
               : NoReply;
}

// A datagram from the echo port to address:port; a Pending answer (the
// stack is resolving the address) is resent after a pause, a bounded number
// of times.
[[nodiscard, gnu::noinline]] SocketReply
SendDatagram(ScenarioContext& context, const ComponentRecord& stack, SocketClient& client,
             const Foundation::UInt64 address, const Foundation::UInt64 port, const Foundation::UInt8* const bytes,
             const Foundation::Size length, Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 words[Component::SocketPayloadWords]{stack.Tag, EchoPort | (port << 16U), address, length};
    SocketReply reply = NoReply;
    for (Foundation::Size attempt{0U}; attempt < PendingAttempts; ++attempt)
    {
        Foundation::UInt64 sent{0U};
        reply = SendSocket(context, stack, client, Component::SocketSendMethod, words, bytes, length, sent, systemCall)
                    ? AwaitSocketReply(context, stack, client, sent, ReplyWaits, systemCall)
                    : NoReply;
        if ((reply.Kind != Component::SocketPending) ||
            (context.Supervisor->Pause(PendingPauseMicroseconds, systemCall) != PagerServeError::None))
        {
            break;
        }
    }
    return reply;
}

// Reports which reply was not the one expected: the step in bits 56-63, the
// reply's kind in bits 48-55 and its detail's low 48 bits, in place of an
// exit code, as the block demonstration does.
[[nodiscard]] PagerServeError Fail(const ScenarioContext& context, const ComponentRecord& component,
                                   const Foundation::UInt64 step, const SocketReply& reply) noexcept
{
    ReportUnexpectedEnd(context, component.Tag,
                        (component.ExitCode != 0U)
                            ? component.ExitCode
                            : ((step << 56U) | ((reply.Kind & 0xFFU) << 48U) | (reply.Detail & 0xFFFF'FFFF'FFFFULL)),
                        false);
    return PagerServeError::ManagementFailed;
}

// A service ends only by a kill; its ResourceDomain is then drained, and the NIC reset by the
// Device Manager.
[[nodiscard, gnu::noinline]] PagerServeError Stop(ScenarioContext& context, ComponentRecord& component,
                                                  Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    const PagerServeError paused = supervisor.Pause(KillPauseMicroseconds, systemCall);
    const PagerServeError served = (paused != PagerServeError::None) ? paused : supervisor.Stop(component, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    if (component.ExitCode != Abi::KilledExitCode)
    {
        ReportUnexpectedEnd(context, component.Tag, component.ExitCode, false);
        return PagerServeError::ManagementFailed;
    }
    return supervisor.Teardown(component, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed;
}

/// A line under construction, at most 96 bytes.
struct Line
{
    char Text[96];
    Foundation::Size Length;

    void Append(const char* const text) noexcept
    {
        for (Foundation::Size index{0U}; (text[index] != '\0') && (Length < sizeof(Text)); ++index)
        {
            Text[Length++] = text[index];
        }
    }

    void Decimal(const Foundation::UInt64 value) noexcept
    {
        char digits[20]{};
        Foundation::Size count{0U};
        Foundation::UInt64 rest = value;
        do
        {
            digits[count++] = static_cast<char>('0' + (rest % 10U));
            rest /= 10U;
        } while ((rest != 0U) && (count < sizeof(digits)));
        while ((count != 0U) && (Length < sizeof(Text)))
        {
            Text[Length++] = digits[--count];
        }
    }

    void Address(const Foundation::UInt64 address) noexcept
    {
        for (Foundation::UInt64 shift{32U}; shift != 0U; shift -= 8U)
        {
            Decimal((address >> (shift - 8U)) & 0xFFU);
            if (shift != 8U)
            {
                Append(".");
            }
        }
    }

    void Mac(const Foundation::UInt64 mac) noexcept
    {
        constexpr char Digits[] = "0123456789abcdef";
        for (Foundation::Size octet{0U}; (octet < 6U) && (Length + 3U <= sizeof(Text)); ++octet)
        {
            const Foundation::UInt64 value = (mac >> (octet << 3U)) & 0xFFU;
            Text[Length++] = Digits[value >> 4U];
            Text[Length++] = Digits[value & 0xFU];
            if (octet != 5U)
            {
                Text[Length++] = ':';
            }
        }
    }

    void Write(const ScenarioContext& context) noexcept
    {
        Append("\n");
        WriteLine(context, Text, Length);
    }
};

[[nodiscard]] Foundation::UInt64 Count(const Foundation::Size index) noexcept
{
    return LoadWord(g_reply.Inline + (index << 3U));
}

// "# net: nic <MAC>, <address>/<prefix>", from the stack's Query.
[[gnu::noinline]] void WriteNicLine(const ScenarioContext& context, const SocketReply& queried) noexcept
{
    Line line{.Text = {}, .Length = 0U};
    line.Append("# net: nic ");
    line.Mac(queried.Detail);
    line.Append(", ");
    line.Address(queried.Extra & 0xFFFF'FFFFU);
    line.Append("/");
    line.Decimal((queried.Extra >> 32U) & 0xFFU);
    line.Write(context);
}

// prefix, an address, then suffix.
[[gnu::noinline]] void WriteAddressLine(const ScenarioContext& context, const char* const prefix,
                                        const Foundation::UInt64 address, const char* const suffix) noexcept
{
    Line line{.Text = {}, .Length = 0U};
    line.Append(prefix);
    line.Address(address);
    line.Append(suffix);
    line.Write(context);
}

// The stack on the driver, with its address and prefix.
[[nodiscard, gnu::noinline]] ComponentRecord* StartStack(ScenarioContext& context, const ComponentRecord& driver,
                                                         Abi::SystemCallError& systemCall) noexcept
{
    context.DriverService = driver.Service;
    context.DriverGeneration = driver.Tag;
    context.ServiceFlags = StackAddress | (StackPrefix << 32U);
    ComponentRecord* const stack =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorNetwork, systemCall);
    context.DriverService = {};
    context.DriverGeneration = 0U;
    context.ServiceFlags = 0U;
    return stack;
}

// The rule, the binding and the stack's identity, then the send that the
// Stack must resolve before any of the peer's traffic.
[[nodiscard, gnu::noinline]] PagerServeError Configure(ScenarioContext& context, const ComponentRecord& stack,
                                                       SocketClient& client, bool& foreign,
                                                       Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 rule[Component::SocketPayloadWords]{stack.Tag, 0U, EchoPort, PeerAddress};
    const SocketReply ruled = Request(context, stack, client, Component::SocketRuleMethod, rule, systemCall);
    if (ruled.Kind != Component::SocketDone)
    {
        return Fail(context, stack, 1U, ruled);
    }
    const Foundation::UInt64 bind[Component::SocketPayloadWords]{stack.Tag, EchoPort, 0U, 0U};
    const SocketReply bound = Request(context, stack, client, Component::SocketBindMethod, bind, systemCall);
    if (bound.Kind != Component::SocketDone)
    {
        return Fail(context, stack, 2U, bound);
    }
    const Foundation::UInt64 query[Component::SocketPayloadWords]{stack.Tag, 0U, 0U, 0U};
    const SocketReply queried = Request(context, stack, client, Component::SocketQueryMethod, query, systemCall);
    if (queried.Kind != Component::SocketDone)
    {
        return Fail(context, stack, 3U, queried);
    }
    WriteNicLine(context, queried);
    if (queried.Detail != RunnerNicMac)
    {
        foreign = true;
        return PagerServeError::None;
    }
    const SocketReply resolved =
        SendDatagram(context, stack, client, PeerAddress, PeerPort,
                     reinterpret_cast<const Foundation::UInt8*>(ResolveData), sizeof(ResolveData) - 1U, systemCall);
    if (resolved.Kind != Component::SocketDone)
    {
        return Fail(context, stack, 4U, resolved);
    }
    WriteAddressLine(context, "# net: resolved ", PeerAddress, "");
    return PagerServeError::None;
}

// Both receive credits before the listening line, a datagram, and its echo
// to its source.
[[nodiscard, gnu::noinline]] PagerServeError Echo(ScenarioContext& context, const ComponentRecord& stack,
                                                  SocketClient& client, Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 receive[Component::SocketPayloadWords]{stack.Tag, EchoPort, 0U, 0U};
    Foundation::UInt64 first{0U};
    Foundation::UInt64 second{0U};
    if (!SendSocket(context, stack, client, Component::SocketReceiveMethod, receive, nullptr, 0U, first, systemCall) ||
        !SendSocket(context, stack, client, Component::SocketReceiveMethod, receive, nullptr, 0U, second, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    Line listening{.Text = {}, .Length = 0U};
    listening.Append("# net: listening on udp ");
    listening.Decimal(EchoPort);
    listening.Write(context);
    const SocketReply datagram = AwaitSocketReply(context, stack, client, first, DatagramWaits, systemCall);
    if ((datagram.Kind != Component::SocketDone) || (datagram.Detail == 0U) ||
        (datagram.Detail > Component::SocketInlineBytes))
    {
        return Fail(context, stack, 5U, datagram);
    }
    // The datagram's bytes leave g_reply before the next await reuses it.
    const Foundation::Size length = static_cast<Foundation::Size>(datagram.Detail);
    for (Foundation::Size index{0U}; index < length; ++index)
    {
        g_datagram[index] = g_reply.Inline[index];
    }
    const Foundation::UInt64 source = datagram.Extra & 0xFFFF'FFFFU;
    const Foundation::UInt64 port = (datagram.Extra >> 32U) & 0xFFFFU;
    const SocketReply echoed = SendDatagram(context, stack, client, source, port, g_datagram, length, systemCall);
    if (echoed.Kind != Component::SocketDone)
    {
        return Fail(context, stack, 6U, echoed);
    }
    Line line{.Text = {}, .Length = 0U};
    line.Append("# net: echoed ");
    line.Decimal(length);
    line.Append(" bytes to ");
    line.Address(source);
    line.Append(":");
    line.Decimal(port);
    line.Write(context);
    return PagerServeError::None;
}

// A send the rule refuses (no frame), then the stack's drop counts: each
// Negative control of the runner is a frame the stack received and refused.
[[nodiscard, gnu::noinline]] PagerServeError Refuse(ScenarioContext& context, const ComponentRecord& stack,
                                                    SocketClient& client, Abi::SystemCallError& systemCall) noexcept
{
    const SocketReply refused =
        SendDatagram(context, stack, client, RefusedAddress, RefusedPort,
                     reinterpret_cast<const Foundation::UInt8*>(RefusedData), sizeof(RefusedData) - 1U, systemCall);
    if (refused.Kind != Component::SocketRefused)
    {
        return Fail(context, stack, 7U, refused);
    }
    WriteAddressLine(context, "# net: send to ", RefusedAddress, " refused");
    const Foundation::UInt64 query[Component::SocketPayloadWords]{stack.Tag, 0U, 0U, 0U};
    const SocketReply queried = Request(context, stack, client, Component::SocketQueryMethod, query, systemCall);
    if (queried.Kind != Component::SocketDone)
    {
        return Fail(context, stack, 8U, queried);
    }
    Line drops{.Text = {}, .Length = 0U};
    drops.Append("# net: drops not-for-us ");
    drops.Decimal(Count(Component::SocketDropNotForUs));
    drops.Append(", bad-icmp ");
    drops.Decimal(Count(Component::SocketDropBadIcmp));
    drops.Append(", unauthorized ");
    drops.Decimal(Count(Component::SocketDropUnauthorized));
    drops.Write(context);
    return PagerServeError::None;
}

[[nodiscard, gnu::noinline]] PagerServeError RunClient(ScenarioContext& context, SocketClient& client,
                                                       Abi::SystemCallError& systemCall) noexcept
{
    ComponentRecord* const driver =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorNicDriver, systemCall);
    if (driver == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    ComponentRecord* const stack = StartStack(context, *driver, systemCall);
    if ((stack == nullptr) || !CreateReplyEndpoint(context, client, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    bool foreign{false};
    PagerServeError result = Configure(context, *stack, client, foreign, systemCall);
    if ((result == PagerServeError::None) && foreign)
    {
        Line line{.Text = {}, .Length = 0U};
        line.Append("# net: not the runner's network, exchange skipped");
        line.Write(context);
    }
    result = ((result == PagerServeError::None) && !foreign) ? Echo(context, *stack, client, systemCall) : result;
    result = ((result == PagerServeError::None) && !foreign) ? Refuse(context, *stack, client, systemCall) : result;
    if (result != PagerServeError::None)
    {
        return result;
    }
    // The stack first (it is a client of the driver), then the
    // driver, whose device the Device Manager resets.
    result = Stop(context, *stack, systemCall);
    result = (result == PagerServeError::None) ? Stop(context, *driver, systemCall) : result;
    if (result == PagerServeError::None)
    {
        Line line{.Text = {}, .Length = 0U};
        line.Append("# net: stopped");
        line.Write(context);
    }
    return result;
}
} // Namespace

// The network demonstration: the root service starts the NIC driver and the stack on it, installs
// its own rule (port 7, the peer only) as the holder of Manage on the stack's Endpoint, binds the
// port, sends to the peer through the stack's ARP resolution, echoes the peer's datagram, is
// refused a send its rule does not allow, and reports the stack's drop counts before it stops both.
PagerServeError RunNetDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    SocketClient client{.Reply = {}, .Transaction = 0U};
    const PagerServeError result = RunClient(context, client, systemCall);
    Abi::SystemCallError released{Abi::SystemCallError::None};
    const bool clean =
        (client.Reply.Selector == 0U) || context.Calls->Destroy(client.Reply, Abi::ObjectTypeEndpoint, released);
    systemCall = (systemCall == Abi::SystemCallError::None) ? released : systemCall;
    return (result != PagerServeError::None) ? result
                                             : (clean ? PagerServeError::None : PagerServeError::ManagementFailed);
}
} // Namespace Asteria::User::Pager
