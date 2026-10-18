// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the file protocol's codec (FsFrame.hpp).
//
// Input layout: a canonical frame, which the harness places behind a 48-byte
// delivery header as the receiving service sees it.
//
// Properties checked for every input:
// - Decoding never reads outside the delivery (sanitizers report violations).
// - A rejected frame leaves the message byte-for-byte unchanged.
// - An accepted frame's method is known, its name (Open and Create) is a valid
//   Object Directory name with zero padding, its fields are in range, and
//   rebuilding it with WriteFsFrame yields a frame of the input's size whose
//   payload bytes equal the input's and which decodes to the same message.

#include "User/Component/FsFrame.hpp"

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
constexpr Size MaximumInput{Component::FsMaximumFrameBytes + 8U};

UInt8 g_delivery[Header + MaximumInput];
UInt8 g_rebuilt[Component::FsMaximumFrameBytes];
UInt8 g_seed[Component::FsMaximumFrameBytes];

constexpr Component::FsMessage Sentinel{};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size > MaximumInput)
    {
        return 0;
    }
    std::memset(g_delivery, 0, sizeof(g_delivery));
    std::memcpy(g_delivery + Header, data, size);

    static Component::FsMessage message;
    static Component::FsMessage before;
    std::memset(&message, 0xA5, sizeof(message));
    std::memcpy(&before, &message, sizeof(message));
    static_cast<void>(Sentinel);
    if (!Component::ReadFsFrame(g_delivery, Header + size, message))
    {
        Require(std::memcmp(&message, &before, sizeof(message)) == 0, "a rejected frame leaves the message unchanged");
        return 0;
    }

    Require(Component::Detail::CountFsAttachments(message.Method) <= 2U, "an accepted frame has a known method");
    const bool named = Component::Detail::HasFsName(message.Method);
    if (named)
    {
        Require(Component::Detail::IsFsName(message.Name, message.NameLength), "an accepted name is valid");
        for (Size index{message.NameLength}; index < Component::FsMaximumNameBytes; ++index)
        {
            Require(message.Name[index] == 0U, "an accepted name is zero-padded");
        }
    }
    else
    {
        Require(message.NameLength == 0U, "a frame with no name has no name length");
    }
    if ((message.Method == Component::FsReadMethod) || (message.Method == Component::FsWriteMethod))
    {
        Require(message.Words[3U] <= 0x1FFF'FFFFU, "an accepted block index is in range");
    }
    if (message.Method == Component::FsReplyMethod)
    {
        Require((message.Words[1U] >= Component::FsDone) && (message.Words[1U] <= Component::FsRightsDenied),
                "an accepted reply kind is known");
    }

    const bool data2 = Component::Detail::CountFsAttachments(message.Method) == 2U;
    const bool reply = message.Method != Component::FsReplyMethod;
    const Size built =
        Component::WriteFsFrame(g_rebuilt, static_cast<Asteria::Foundation::UInt32>(message.Method),
                                message.Transaction, message.Words, named ? message.Name : nullptr, message.NameLength,
                                reply ? &message.Reply : nullptr, data2 ? &message.Data : nullptr);
    Require(built == size, "an accepted frame rebuilds to its size");
    const Size payloadAt = Component::EchoHeaderBytes + ((reply ? (data2 ? 2U : 1U) : 0U) << 5U);
    Require(std::memcmp(g_rebuilt + payloadAt, data + payloadAt, size - payloadAt) == 0,
            "an accepted frame's payload is its rebuild's");
    static UInt8 delivery[Header + Component::FsMaximumFrameBytes];
    std::memcpy(delivery + Header, g_rebuilt, built);
    static Component::FsMessage again;
    Require(Component::ReadFsFrame(delivery, Header + built, again), "a rebuilt frame decodes");
    Require(std::memcmp(&again, &message, sizeof(message)) == 0, "a rebuilt frame decodes to the same message");
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    namespace C = Component;
    const Asteria::User::System::CapabilityHandle reply{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
    const Asteria::User::System::CapabilityHandle data{0x0456U, 0x1111'2222'3333'4444ULL};
    const Asteria::Foundation::UInt64 words[Component::FsPayloadWords]{5U, 6U, 0U, 0U, 1U, 0U, 1U, 0U};
    const Asteria::Foundation::UInt64 io[Component::FsPayloadWords]{5U, 6U, 0U, 0U, 1U, 0U, 0U, 0U};
    const Asteria::Foundation::UInt64 read[Component::FsPayloadWords]{5U, 6U, 0U, 0U, 1U, 0U, 0U, 1U};
    const Asteria::Foundation::UInt64 tag[Component::FsPayloadWords]{5U};
    const Asteria::Foundation::UInt64 flagWords[Component::FsPayloadWords]{5U, 9U, C::FsCrashBeforeSuperblock, 0U};
    const Asteria::Foundation::UInt64 queryWords[Component::FsPayloadWords]{5U, 9U, 0U, 0U};
    const Asteria::Foundation::UInt64 replyWords[Component::FsPayloadWords]{9U, C::FsCommitted, 3U, 0U};
    const UInt8 name[9U]{'a', '0', '-', 'b', '_', 'c', '.', 'd', '9'};
    const UInt8 longName[64U]{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                              'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                              'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                              'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    const auto put = [&](const Asteria::Foundation::UInt32 method,
                         const Asteria::Foundation::UInt64(&payload)[Component::FsPayloadWords],
                         const UInt8* const text, const Size length, const bool withReply, const bool withData)
    {
        const Size bytes = C::WriteFsFrame(g_seed, method, 0x8070'6050'4030'2010ULL, payload, text, length,
                                           withReply ? &reply : nullptr, withData ? &data : nullptr);
        std::memcpy(g_delivery + Header, g_seed, bytes);
        C::FsMessage decoded{};
        Require(C::ReadFsFrame(g_delivery, Header + bytes, decoded), "a canonical generated seed decodes");
        emit(g_seed, bytes);
    };
    put(C::FsOpenMethod, words, name, sizeof(name), true, false);
    put(C::FsCreateMethod, words, longName, sizeof(longName), true, false);
    put(C::FsReadMethod, read, nullptr, 0U, true, true);
    const Asteria::Foundation::UInt64 many[Component::FsPayloadWords]{5U, 6U, 7U, 0U,
                                                                      1U, 0U, 0U, C::FsReadPagesCapacity};
    put(C::FsReadMethod, many, nullptr, 0U, true, true);
    put(C::FsWriteMethod, io, nullptr, 0U, true, true);
    put(C::FsCommitMethod, flagWords, nullptr, 0U, true, false);
    put(C::FsQueryCommitMethod, queryWords, nullptr, 0U, true, false);
    put(C::FsReplyMethod, replyWords, nullptr, 0U, false, false);
    put(C::FsStatMethod, queryWords, nullptr, 0U, true, false);
    put(C::FsReadDirectoryMethod, io, nullptr, 0U, true, true);
    put(C::FsMakeDirectoryMethod, words, name, sizeof(name), true, false);
    put(C::FsCloseMethod, queryWords, nullptr, 0U, true, false);
    put(C::FsInstallViewMethod, io, nullptr, 0U, true, false);
    put(C::FsResetViewsMethod, tag, nullptr, 0U, true, false);
    // A frame with a flipped byte in its name, to start mutations at a refusal.
    const Size bytes = C::WriteFsFrame(g_seed, C::FsOpenMethod, 1U, words, name, sizeof(name), &reply, nullptr);
    g_seed[C::EchoHeaderBytes + C::EchoAttachmentBytes + C::FsPayloadBytes] = 'A';
    emit(g_seed, bytes);
}
