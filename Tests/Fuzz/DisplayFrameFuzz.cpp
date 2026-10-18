// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the display protocol's codec (DisplayFrame.hpp).
//
// Input layout: a canonical frame, which the harness places behind a 48-byte
// delivery header as the receiving service sees it.
//
// Properties checked for every input:
// - Decoding never reads outside the delivery (sanitizers report violations).
// - A rejected frame leaves the message byte-for-byte unchanged.
// - An accepted frame's method is ConsoleWrite, Query or reply, its fields are
//   in range, its text padding is zero, and rebuilding it with the writers
//   yields a frame of the input's size whose payload bytes equal the input's
//   and which decodes to the same message.

#include "User/Component/DisplayFrame.hpp"

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
constexpr Size MaximumInput{Component::DisplayMaximumFrameBytes + 8U};

UInt8 g_delivery[Header + MaximumInput];
UInt8 g_rebuilt[Component::DisplayMaximumFrameBytes];
UInt8 g_seed[Component::DisplayMaximumFrameBytes];
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size > MaximumInput)
    {
        return 0;
    }
    std::memset(g_delivery, 0, sizeof(g_delivery));
    std::memcpy(g_delivery + Header, data, size);

    static Component::DisplayMessage message;
    static Component::DisplayMessage before;
    std::memset(&message, 0xA5, sizeof(message));
    std::memcpy(&before, &message, sizeof(message));
    if (!Component::ReadDisplayFrame(g_delivery, Header + size, message))
    {
        Require(std::memcmp(&message, &before, sizeof(message)) == 0, "a rejected frame leaves the message unchanged");
        return 0;
    }

    Require(
        (message.Method >= Component::DisplayConsoleWriteMethod) &&
            ((message.Method <= Component::DisplayReplyMethod) || (message.Method == Component::DisplayPointerMethod)),
        "an accepted frame has a v0 method");
    const bool write = message.Method == Component::DisplayConsoleWriteMethod;
    const bool query = message.Method == Component::DisplayQueryMethod;
    Size built{0U};
    Size payloadAt{Component::EchoHeaderBytes};
    if (write)
    {
        Require((message.Count >= 1U) && (message.Count <= Component::DisplayMaximumWriteBytes),
                "an accepted count is in range");
        Require(message.Text == (g_delivery + Header + Component::EchoHeaderBytes + Component::DisplayPayloadBytes),
                "an accepted text points into the delivery's text area");
        for (Size index{message.Count}; index < Component::DisplayMaximumWriteBytes; ++index)
        {
            Require(message.Text[index] == 0U, "an accepted text is zero-padded");
        }
        built = Component::WriteDisplayConsoleWrite(g_rebuilt, message.Transaction, message.Text, message.Count);
    }
    else if (query)
    {
        payloadAt += Component::EchoAttachmentBytes;
        Require(message.Text == nullptr, "a query has no text");
        built = Component::WriteDisplayQuery(g_rebuilt, message.Transaction, message.Reply);
    }
    else if (message.Method == Component::DisplayPointerMethod)
    {
        built = Component::WriteDisplayPointer(g_rebuilt, message.Transaction, message.Words);
    }
    else
    {
        Require((message.Words[1U] == Component::DisplayDone) || (message.Words[1U] == Component::DisplayRefused),
                "an accepted reply kind is known");
        Require(message.Words[0U] == message.Transaction, "an accepted reply answers its transaction");
        const Component::DisplayInfo info = Component::UnpackDisplayInfo(message);
        built = Component::WriteDisplayReply(g_rebuilt, message.Transaction, message.Words[1U], info);
    }
    Require(built == size, "an accepted frame rebuilds to its size");
    Require(std::memcmp(g_rebuilt + payloadAt, data + payloadAt, size - payloadAt) == 0,
            "an accepted frame's payload is its rebuild's");
    static UInt8 delivery[Header + Component::DisplayMaximumFrameBytes];
    std::memcpy(delivery + Header, g_rebuilt, built);
    static Component::DisplayMessage again;
    Require(Component::ReadDisplayFrame(delivery, Header + built, again), "a rebuilt frame decodes");
    // The text pointers differ by construction: compare the rest, then the text.
    const bool sameText = write ? (std::memcmp(again.Text, message.Text, Component::DisplayMaximumWriteBytes) == 0)
                                : (again.Text == nullptr);
    again.Text = message.Text;
    Require(sameText && (std::memcmp(&again, &message, sizeof(message)) == 0),
            "a rebuilt frame decodes to the same message");
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    namespace C = Component;
    const Asteria::User::System::CapabilityHandle reply{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
    const UInt8 text[9U]{'a', '\n', 0x00U, 0xFFU, 'b', 0x80U, 'c', '\r', '9'};
    UInt8 full[C::DisplayMaximumWriteBytes]{};
    for (Size index{0U}; index < sizeof(full); ++index)
    {
        full[index] = static_cast<UInt8>(index + 1U);
    }
    emit(g_seed, C::WriteDisplayConsoleWrite(g_seed, 0x8070'6050'4030'2010ULL, text, sizeof(text)));
    emit(g_seed, C::WriteDisplayConsoleWrite(g_seed, 1U, full, sizeof(full)));
    emit(g_seed, C::WriteDisplayQuery(g_seed, 2U, reply));
    C::DisplayInfo info{.Version = C::DisplayProtocolVersion,
                        .Features = C::DisplayFeatureConsole,
                        .Width = 1280U,
                        .Height = 800U,
                        .Columns = 160U,
                        .Rows = 50U,
                        .Epoch = 3U};
    emit(g_seed, C::WriteDisplayReply(g_seed, 4U, C::DisplayDone, info));
    info.Features |= C::DisplayFeatureMetrics | C::DisplayFeatureTiming;
    info.ScanoutPolicy = C::DisplayScanoutWc;
    info.FillBytes = 1228800U;
    info.FillMicroseconds = 3200U;
    info.LatestPixels = 307200U;
    info.LatestMicroseconds = 41000U;
    info.SlowestPixels = 307200U;
    info.SlowestMicroseconds = 52000U;
    emit(g_seed, C::WriteDisplayReply(g_seed, 6U, C::DisplayDone, info));
    emit(g_seed, C::WriteDisplayReply(g_seed, 5U, C::DisplayRefused, C::DisplayInfo{}));
    for (Asteria::Foundation::UInt64 kind{1U}; kind <= 3U; ++kind)
    {
        const Asteria::Foundation::UInt64 pointer[8U]{3U, kind, kind, 0U, 1U, 1U, 0U, 0U};
        emit(g_seed, C::WriteDisplayPointer(g_seed, 20U + kind, pointer));
    }
    // A frame with a flipped padding byte, to start mutations at a refusal.
    const Size bytes = C::WriteDisplayConsoleWrite(g_seed, 1U, text, sizeof(text));
    g_seed[C::EchoHeaderBytes + C::DisplayPayloadBytes + sizeof(text)] = 'A';
    emit(g_seed, bytes);
}
