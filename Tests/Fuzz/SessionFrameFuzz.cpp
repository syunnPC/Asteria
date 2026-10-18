// SPDX-License-Identifier: Apache-2.0
#include "User/Session/SessionFrame.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
namespace
{
namespace S = Asteria::User::Session;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
UInt8 delivery[560U];
UInt8 frame[504U];
S::SessionMessage message;
S::SessionMessage before;
const Asteria::User::System::CapabilityHandle handles[4U]{{1U, 2U}, {3U, 4U}, {5U, 6U}, {7U, 8U}};
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > 512U)
    {
        return 0;
    }
    std::memcpy(delivery + 48U, data, size);
    std::memset(&message, 0xA5, sizeof(message));
    before = message;
    if (!S::ReadSessionFrame(delivery, size + 48U, message))
    {
        if (std::memcmp(&before, &message, sizeof(message)) != 0)
        {
            std::abort();
        }
        return 0;
    }
    const Size rebuilt = S::WriteSessionFrame(frame, message.Method, message.Transaction, message.Words,
                                              message.Attachments, message.AttachmentCount);
    if ((rebuilt != size) || (std::memcmp(frame, data, size) != 0))
    {
        std::abort();
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    for (Asteria::Foundation::UInt32 method{1U}; method <= 20U; ++method)
    {
        UInt64 w[32U]{};
        Size attachments{0U};
        w[0U] = 1U;
        switch (method)
        {
        case 1U:
            attachments = 1U;
            w[1U] = 7U;
            break;
        case 2U:
            attachments = 1U;
            w[1U] = 1U;
            w[2U] = 1U;
            w[4U] = 1U;
            break;
        case 3U:
            attachments = 4U;
            w[1U] = 12U;
            break;
        case 5U:
            w[1U] = 1U;
            w[3U] = S::PackSessionPair(1U, 1U);
            w[4U] = 1U;
            w[5U] = 1U;
            break;
        case 7U:
            w[1U] = 1U;
            break;
        case 9U:
            w[2U] = 1U;
            break;
        case 12U:
            w[1U] = (1ULL << 56U) | 17U;
            w[4U] = 1U;
            w[5U] = 1U;
            w[6U] = 4U;
            break;
        case 13U:
            w[1U] = S::PackSessionPair(640U, 400U);
            w[2U] = 1U;
            break;
        case 14U:
            attachments = 1U;
            w[1U] = 1U;
            w[2U] = 1U;
            w[4U] = 'x';
            break;
        case 15U:
            attachments = 1U;
            w[0U] = 257U;
            break;
        case 17U:
            attachments = 1U;
            break;
        case 18U:
            w[1U] = 1U;
            w[2U] = 257U;
            attachments = 1U;
            break;
        default:
            break;
        }
        const Size size = S::WriteSessionFrame(frame, method, 1U, w, handles, attachments);
        if (size == 0U)
        {
            std::abort();
        }
        emit(frame, size);
    }
    for (UInt8 kind{1U}; kind <= S::SessionLastEventKind; ++kind)
    {
        S::SessionEvent event{static_cast<S::SessionEventKind>(kind), {1U, 0U, 0U, 0U}};
        switch (kind)
        {
        case 1U:
            event.Words[2U] = 1U;
            event.Words[3U] = S::PackSessionPair(640U, 400U);
            break;
        case 3U:
        case 7U:
            event.Words[1U] = 1U;
            break;
        case 8U:
            event.Words[1U] = 1U;
            break;
        case 9U:
            event.Words[1U] = 1U;
            event.Words[2U] = S::PackSessionPair(1U, 1U);
            break;
        case 16U:
            event.Words[1U] = 1U;
            break;
        case 17U:
            event.Words[1U] = 1U;
            event.Words[2U] = 4U;
            break;
        case 18U:
            event.Words[1U] = 1U;
            event.Words[2U] = 1U;
            break;
        default:
            break;
        }
        const Size size = S::WriteSessionEvents(frame, 1U, 1U, &event, 1U, 1U, 65U);
        if (size == 0U)
        {
            std::abort();
        }
        emit(frame, size);
    }
}
