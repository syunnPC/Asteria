// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the socket protocol's codec (SocketFrame.hpp).
//
// Input layout: a canonical frame, which the harness places behind a 48-byte
// delivery header as the receiving service or client sees it.
//
// Properties checked for every input:
// - Decoding never reads outside the delivery (sanitizers report violations).
// - A rejected frame leaves the message byte-for-byte unchanged.
// - An accepted frame's method and fields are in range: a request's words in
//   their fields, a reply's first word its transaction and its kind known;
//   its inline length is what its words give, and the inline area is zero
//   past it.
// - Rebuilding an accepted frame with WriteSocketFrame yields a frame of the
//   input's size whose payload bytes equal the input's and which decodes to
//   the same message.

#include "User/Component/SocketFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
namespace Component = Asteria::User::Component;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

void Require(const bool condition, const char* const property)
{
    if (!condition)
    {
        Violation(property);
    }
}

constexpr Size Header{Component::EchoDeliveryHeaderBytes};
constexpr Size MaximumInput{Component::SocketMaximumFrameBytes + 8U};

UInt8 g_delivery[Header + MaximumInput];
UInt8 g_rebuilt[Component::SocketMaximumFrameBytes];
UInt8 g_seed[Component::SocketMaximumFrameBytes];
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size > MaximumInput)
    {
        return 0;
    }
    std::memset(g_delivery, 0, sizeof(g_delivery));
    std::memcpy(g_delivery + Header, data, size);

    static Component::SocketMessage message;
    static Component::SocketMessage before;
    std::memset(&message, 0xA5, sizeof(message));
    std::memcpy(&before, &message, sizeof(message));
    if (!Component::ReadSocketFrame(g_delivery, Header + size, message))
    {
        Require(std::memcmp(&message, &before, sizeof(message)) == 0, "a rejected frame leaves the message unchanged");
        return 0;
    }

    const bool reply = message.Method == Component::SocketReplyMethod;
    Require((message.Method >= Component::SocketRuleMethod) && (message.Method <= Component::SocketReplyMethod),
            "an accepted frame has a known method");
    Require(message.InlineLength <= Component::SocketInlineBytes, "an accepted inline length fits the area");
    for (Size index{message.InlineLength}; index < Component::SocketInlineBytes; ++index)
    {
        Require(message.Inline[index] == 0U, "an accepted inline area is zero past its length");
    }
    if (reply)
    {
        Require((message.Words[0U] == message.Transaction) && (message.Words[1U] >= Component::SocketDone) &&
                    (message.Words[1U] <= Component::SocketRestarted),
                "an accepted reply answers its transaction with a known kind");
        Require((message.Reply.Selector == 0U) && (message.Reply.Authenticator == 0U),
                "an accepted reply carries no capability");
        Require(message.InlineLength == Component::SocketInlineBytes, "an accepted reply is read with its whole area");
    }
    else if (message.Method == Component::SocketSendMethod)
    {
        Require((message.Words[3U] >= 1U) && (message.Words[3U] <= Component::SocketInlineBytes) &&
                    (message.InlineLength == message.Words[3U]) && (message.Words[1U] <= 0xFFFF'FFFFU) &&
                    (message.Words[2U] <= 0xFFFF'FFFFU),
                "an accepted Send's fields are in range and its length is its datagram's");
    }
    else
    {
        Require(message.InlineLength == 0U, "an accepted request other than a Send carries no inline bytes");
    }

    const Size built = Component::WriteSocketFrame(g_rebuilt, static_cast<Asteria::Foundation::UInt32>(message.Method),
                                                   message.Transaction, message.Words, message.Inline,
                                                   message.InlineLength, reply ? nullptr : &message.Reply);
    Require(built == size, "an accepted frame rebuilds to its size");
    const Size payloadAt = Component::EchoHeaderBytes + (reply ? 0U : Component::EchoAttachmentBytes);
    Require(std::memcmp(g_rebuilt + payloadAt, data + payloadAt, size - payloadAt) == 0,
            "an accepted frame's payload is its rebuild's");
    static UInt8 delivery[Header + Component::SocketMaximumFrameBytes];
    std::memcpy(delivery + Header, g_rebuilt, built);
    static Component::SocketMessage again;
    Require(Component::ReadSocketFrame(delivery, Header + built, again), "a rebuilt frame decodes");
    Require(std::memcmp(&again, &message, sizeof(message)) == 0, "a rebuilt frame decodes to the same message");
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    namespace C = Component;
    const Asteria::User::System::CapabilityHandle reply{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
    const UInt8 hello[5U]{'h', 'e', 'l', 'l', 'o'};
    UInt8 counts[C::SocketQueryInlineBytes]{};
    counts[0U] = 1U;
    counts[C::SocketQueryInlineBytes - 8U] = 2U;
    const UInt64 source{0x0A00'0202U | (UInt64{40000U} << 32U)};
    const UInt64 ruleWords[4U]{3U, 0U, 7U, 0x0A00'0202U};
    const UInt64 bindWords[4U]{3U, 7U, 0U, 0U};
    const UInt64 sendWords[4U]{3U, 7U | (UInt64{40000U} << 16U), 0x0A00'0202U, 5U};
    const UInt64 queryWords[4U]{3U, 0U, 0U, 0U};
    const UInt64 receivedWords[4U]{9U, C::SocketDone, 5U, source};
    const UInt64 queriedWords[4U]{9U, C::SocketDone, 0x017E'A500'5452ULL, 0x0A00'020FU | (UInt64{24U} << 32U)};
    const UInt64 refusedWords[4U]{9U, C::SocketRefused, C::SocketRefusedNoRule, 0U};
    const auto put = [&](const Asteria::Foundation::UInt32 method, const UInt64(&words)[4U], const UInt8* const bytes,
                         const Size length)
    {
        const Size written = C::WriteSocketFrame(g_seed, method, 9U, words, bytes, length,
                                                 (method == C::SocketReplyMethod) ? nullptr : &reply);
        emit(g_seed, written);
    };
    put(C::SocketRuleMethod, ruleWords, nullptr, 0U);
    put(C::SocketBindMethod, bindWords, nullptr, 0U);
    put(C::SocketSendMethod, sendWords, hello, 5U);
    put(C::SocketReceiveMethod, bindWords, nullptr, 0U);
    put(C::SocketQueryMethod, queryWords, nullptr, 0U);
    put(C::SocketReplyMethod, receivedWords, hello, 5U);
    put(C::SocketReplyMethod, queriedWords, counts, C::SocketQueryInlineBytes);
    put(C::SocketReplyMethod, refusedWords, nullptr, 0U);
    // A Send with a byte past its datagram, to start mutations at a refusal.
    const Size bytes = C::WriteSocketFrame(g_seed, C::SocketSendMethod, 9U, sendWords, hello, 5U, &reply);
    g_seed[C::EchoHeaderBytes + C::EchoAttachmentBytes + C::SocketPayloadBytes + 5U] = 'x';
    emit(g_seed, bytes);
}
