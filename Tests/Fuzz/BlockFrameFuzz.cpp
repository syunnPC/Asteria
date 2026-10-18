// SPDX-License-Identifier: Apache-2.0
// Block protocol properties: bounded decoding, failure atomicity and canonical
// round trips, including Describe and every reply kind.
#include "User/Component/BlockFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
namespace Component = Asteria::User::Component;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
constexpr Size Header{Component::EchoDeliveryHeaderBytes};
UInt8 g_delivery[Header + Component::BlockIoRequestBytes + 8U]{};
UInt8 g_rebuilt[Header + Component::BlockIoRequestBytes]{};
UInt8 g_seed[Component::BlockIoRequestBytes]{};

void Require(const bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size > Component::BlockIoRequestBytes + 8U)
    {
        return 0;
    }
    std::memcpy(g_delivery + Header, data, size);
    Component::BlockMessage message{};
    message.Transaction = 0xA5U;
    const Component::BlockMessage before = message;
    if (!Component::ReadBlockFrame(g_delivery, Header + size, message))
    {
        Require(std::memcmp(&message, &before, sizeof(message)) == 0);
        return 0;
    }
    Require((message.Method >= Component::BlockReadMethod) && (message.Method <= Component::BlockDescribeReplyMethod));
    const UInt32 method = static_cast<UInt32>(message.Method);
    const bool io = (method == Component::BlockReadMethod) || (method == Component::BlockWriteMethod);
    const UInt64 words[4U]{message.Words[0U], message.Words[1U], message.Words[2U], message.Words[3U]};
    const Size built = io ? Component::WriteBlockFrame(g_rebuilt + Header, method, message.Transaction, message.Words,
                                                       &message.Reply, &message.Data)
                          : Component::WriteBlockFrame(g_rebuilt + Header, method, message.Transaction, words,
                                                       &message.Reply, nullptr);
    Require(built == size);
    const Size attachments = Component::Detail::CountBlockAttachments(method);
    const Size payload = Component::EchoHeaderBytes + (attachments << 5U);
    Require(std::memcmp(g_rebuilt + Header + payload, data + payload, size - payload) == 0);
    Component::BlockMessage again{};
    Require(Component::ReadBlockFrame(g_rebuilt, Header + built, again));
    Require(std::memcmp(&again, &message, sizeof(message)) == 0);
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    const Asteria::User::System::CapabilityHandle reply{0x123U, 0x456U};
    const Asteria::User::System::CapabilityHandle data{0x789U, 0xABCU};
    const UInt64 io[5U]{3U, 8U, 1U, 0U, 9U};
    emit(g_seed, Component::WriteBlockFrame(g_seed, Component::BlockReadMethod, 1U, io, &reply, &data));
    emit(g_seed, Component::WriteBlockFrame(g_seed, Component::BlockWriteMethod, 2U, io, &reply, &data));
    const UInt64 cancel[4U]{3U, 1U, 0U, 0U};
    emit(g_seed, Component::WriteBlockFrame(g_seed, Component::BlockCancelMethod, 3U, cancel, &reply, nullptr));
    const UInt64 describe[4U]{3U, 0U, 0U, 0U};
    emit(g_seed, Component::WriteBlockFrame(g_seed, Component::BlockDescribeMethod, 4U, describe, &reply, nullptr));
    const UInt64 description[4U]{512U, 4096U, Component::BlockMediumPresent, 9U};
    emit(g_seed,
         Component::WriteBlockFrame(g_seed, Component::BlockDescribeReplyMethod, 4U, description, nullptr, nullptr));
    for (UInt64 kind{Component::BlockDone}; kind <= Component::BlockMediumChanged; ++kind)
    {
        const UInt64 words[4U]{5U, kind, 0U, 0U};
        emit(g_seed, Component::WriteBlockFrame(g_seed, Component::BlockReplyMethod, 5U, words, nullptr, nullptr));
    }
}
