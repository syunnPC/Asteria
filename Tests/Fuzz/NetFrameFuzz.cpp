// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the NIC driver's frame protocol codec (NetFrame.hpp).
//
// Input layout: a canonical frame, which the harness places behind a 48-byte
// delivery header as the receiving driver sees it.
//
// Properties checked for every input:
// - Decoding never reads outside the delivery (sanitizers report violations).
// - A rejected frame leaves the message byte-for-byte unchanged.
// - An accepted frame's method is known, a Send's length is within the
//   Ethernet bounds and a reply's kind is known, and rebuilding it with
//   WriteNicFrame yields a frame of the input's size whose payload bytes equal
//   the input's and which decodes to the same message.

#include "User/Component/NetFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
namespace Component = Asteria::User::Component;
using Asteria::Foundation::Size;
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
constexpr Size MaximumInput{Component::NicDataRequestBytes + 8U};

UInt8 g_delivery[Header + MaximumInput];
UInt8 g_rebuilt[Component::NicDataRequestBytes];
UInt8 g_seed[Component::NicDataRequestBytes];
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size > MaximumInput)
    {
        return 0;
    }
    std::memset(g_delivery, 0, sizeof(g_delivery));
    std::memcpy(g_delivery + Header, data, size);

    static Component::NicMessage message;
    static Component::NicMessage before;
    std::memset(&message, 0xA5, sizeof(message));
    std::memcpy(&before, &message, sizeof(message));
    if (!Component::ReadNicFrame(g_delivery, Header + size, message))
    {
        Require(std::memcmp(&message, &before, sizeof(message)) == 0, "a rejected frame leaves the message unchanged");
        return 0;
    }

    Require((message.Method >= Component::NicSendMethod) && (message.Method <= Component::NicReplyMethod),
            "an accepted frame has a known method");
    if (message.Method == Component::NicSendMethod)
    {
        Require((message.Words[1U] >= Component::NicMinimumFrameBytes) &&
                    (message.Words[1U] <= Component::NicMaximumFrameBytes),
                "an accepted Send's length is an Ethernet frame's");
    }
    if (message.Method == Component::NicReplyMethod)
    {
        Require((message.Words[1U] >= Component::NicDone) && (message.Words[1U] <= Component::NicDriverRestarted),
                "an accepted reply kind is known");
    }

    const bool data2 = (message.Method == Component::NicSendMethod) || (message.Method == Component::NicReceiveMethod);
    const bool reply = message.Method != Component::NicReplyMethod;
    const Size built = Component::WriteNicFrame(g_rebuilt, static_cast<Asteria::Foundation::UInt32>(message.Method),
                                                message.Transaction, message.Words, reply ? &message.Reply : nullptr,
                                                data2 ? &message.Data : nullptr);
    Require(built == size, "an accepted frame rebuilds to its size");
    const Size payloadAt = Component::EchoHeaderBytes + ((reply ? (data2 ? 2U : 1U) : 0U) << 5U);
    Require(std::memcmp(g_rebuilt + payloadAt, data + payloadAt, size - payloadAt) == 0,
            "an accepted frame's payload is its rebuild's");
    static UInt8 delivery[Header + Component::NicDataRequestBytes];
    std::memcpy(delivery + Header, g_rebuilt, built);
    static Component::NicMessage again;
    std::memset(&again, 0xA5, sizeof(again));
    Require(Component::ReadNicFrame(delivery, Header + built, again), "a rebuilt frame decodes");
    Require(std::memcmp(&again, &message, sizeof(message)) == 0, "a rebuilt frame decodes to the same message");
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    namespace C = Component;
    const Asteria::User::System::CapabilityHandle reply{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
    const Asteria::User::System::CapabilityHandle data{0x0456U, 0x1111'2222'3333'4444ULL};
    const Asteria::Foundation::UInt64 sendWords[4U]{5U, 1514U, 0U, 0U};
    const Asteria::Foundation::UInt64 shortWords[4U]{5U, 14U, 0U, 0U};
    const Asteria::Foundation::UInt64 creditWords[4U]{5U, 0U, 0U, 0U};
    const Asteria::Foundation::UInt64 replyWords[4U]{9U, C::NicDone, 0x5634'1200'5452ULL, 1U};
    const Asteria::Foundation::UInt64 restartWords[4U]{9U, C::NicDriverRestarted, 0U, 0U};
    const auto put = [&](const Asteria::Foundation::UInt32 method, const Asteria::Foundation::UInt64(&payload)[4U],
                         const bool withReply, const bool withData)
    {
        const Size bytes = C::WriteNicFrame(g_seed, method, 0x8070'6050'4030'2010ULL, payload,
                                            withReply ? &reply : nullptr, withData ? &data : nullptr);
        emit(g_seed, bytes);
    };
    put(C::NicSendMethod, sendWords, true, true);
    put(C::NicSendMethod, shortWords, true, true);
    put(C::NicReceiveMethod, creditWords, true, true);
    put(C::NicQueryMethod, creditWords, true, false);
    put(C::NicReplyMethod, replyWords, false, false);
    put(C::NicReplyMethod, restartWords, false, false);
    // A Send one byte too long, to start mutations at a refusal.
    const Asteria::Foundation::UInt64 longWords[4U]{5U, 1515U, 0U, 0U};
    put(C::NicSendMethod, longWords, true, true);
}
