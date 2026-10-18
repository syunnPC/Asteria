// SPDX-License-Identifier: Apache-2.0
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Session/InputV3Frame.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
#include <cstdio>
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace S = User::Session;
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt32;
using Foundation::UInt8;
using TestSupport::TestContext;
UInt8 frame[504U];
UInt8 scratch[504U];
UInt8 delivery[552U];
UInt64 words[32U];
S::Wire::Message message;
S::Wire::Message before;
const User::System::CapabilityHandle handles[4U]{{1U, 2U}, {3U, 4U}, {5U, 6U}, {7U, 8U}};
template <class Codec> void Check(TestContext& t, UInt32 method, Size attachments)
{
    const Size size = S::Wire::Write<Codec>(frame, method, 9U, S::Wire::ArrayWords{words}, handles, attachments);
    std::memset(delivery, 0, sizeof(delivery));
    std::memcpy(delivery + 48U, frame, size);
    ASTERIA_EXPECT(t, "broker method canonical round-trip", size != 0U, size <= 504U,
                   S::Wire::Read<Codec>(delivery, size + 48U, message), message.Method == method,
                   message.WordCount == Codec::Count(method));
    // Every header bit: some belong to unconstrained transaction and epoch,
    // the rest are either rejected or accepted with an exact canonical rebuild.
    for (Size byte{0U}; byte < size; ++byte)
    {
        for (UInt8 bit{0U}; bit < 8U; ++bit)
        {
            delivery[48U + byte] ^= static_cast<UInt8>(1U << bit);
            std::memset(&message, 0xA5, sizeof(message));
            before = message;
            const bool accepted = S::Wire::Read<Codec>(delivery, size + 48U, message);
            bool good = std::memcmp(&before, &message, sizeof(message)) == 0;
            if (accepted)
            {
                const Size rebuilt = S::Wire::Write<Codec>(scratch, message.Method, message.Transaction,
                                                           S::Wire::ArrayWords{message.Words}, message.Attachments,
                                                           message.AttachmentCount);
                good = (rebuilt == size) && (std::memcmp(scratch, delivery + 48U, size) == 0);
            }
            if (!good)
            {
                std::fprintf(stderr, "method=%u byte=%zu bit=%u\n", method, byte, bit);
            }
            ASTERIA_EXPECT(t, "every wire mutation is atomic or canonical", good);
            delivery[48U + byte] ^= static_cast<UInt8>(1U << bit);
        }
    }
    // Negative cases at every size boundary, not merely the reported size.
    for (Size length{0U}; length < size + 48U; ++length)
    {
        ASTERIA_EXPECT(t, "every truncated frame fails", !S::Wire::Read<Codec>(delivery, length, message));
    }
    // Every invalid bounded word is refused by encoder and decoder, atomically.
    for (Size i{0U}; i < Codec::Count(method); ++i)
    {
        const UInt64 original = words[i];
        constexpr UInt64 invalids[]{0U, 1U, 8U, 17U, 513U, 8193U, 32769U, 0x1'0000'0000ULL, ~UInt64{0U}};
        for (UInt64 value : invalids)
        {
            words[i] = value;
            if (!Codec::Valid(method, S::Wire::ArrayWords{words}, attachments, 9U))
            {
                std::memset(scratch, 0xA5, sizeof(scratch));
                ASTERIA_EXPECT(
                    t, "invalid words rejected before output write",
                    S::Wire::Write<Codec>(scratch, method, 9U, S::Wire::ArrayWords{words}, handles, attachments) == 0U,
                    scratch[0U] == 0xA5U, scratch[503U] == 0xA5U);
                User::System::StoreWord(delivery + 48U + 88U + attachments * 32U + i * 8U, value);
                std::memset(&message, 0xA5, sizeof(message));
                before = message;
                ASTERIA_EXPECT(t, "invalid word decoder atomic", !S::Wire::Read<Codec>(delivery, size + 48U, message),
                               std::memcmp(&before, &message, sizeof(message)) == 0);
            }
        }
        words[i] = original;
        User::System::StoreWord(delivery + 48U + 88U + attachments * 32U + i * 8U, original);
    }
}
void Face(TestContext& t)
{
    for (UInt32 method{3U}; method <= S::FaceFence; ++method)
    {
        if (S::SessionFaceCodec::Count(method) == 0U)
        {
            continue;
        }
        std::memset(words, 0, sizeof(words));
        words[0U] = 1U;
        Size attachments{0U};
        switch (method)
        {
        case 3U:
            words[0U] = 9U;
            words[1U] = 1U;
            words[2U] = 2U;
            words[3U] = 16U;
            words[4U] = S::PackSessionPair(640U, 400U);
            words[5U] = S::PackSessionPair(80U, 25U);
            words[6U] = 1U;
            attachments = 2U;
            break;
        case 5U:
            words[1U] = 250U;
            words[2U] = 1U;
            words[4U] = S::PackSessionPair(640U, 400U);
            words[5U] = 640U;
            words[6U] = 2U;
            words[8U] = 250U;
            attachments = 1U;
            break;
        case 6U:
            words[1U] = 2U;
            words[2U] = 1U;
            words[4U] = S::PackSessionDamage(0U, 0U, 640U, 400U);
            attachments = 1U;
            break;
        case 10U:
            words[1U] = 12U;
            attachments = 4U;
            break;
        case 11U:
            words[1U] = 7U;
            attachments = 1U;
            break;
        case 12U:
        case S::FaceFence:
            attachments = 1U;
            break;
        case 14U:
            words[0U] = 0U;
            break;
        case 16U:
            words[1U] = 1U;
            words[4U] = 2U;
            words[5U] = S::PackSessionPair(100U, 200U);
            words[6U] = 7U | (0xFFFF'FFFFULL << 32U);
            words[7U] = 100U;
            break;
        default:
            break;
        }
        Check<S::SessionFaceCodec>(t, method, attachments);
    }
}
void Input(TestContext& t)
{
    for (UInt32 method{1U}; method <= Asteria::User::Component::InputBaselineMethod; ++method)
    {
        std::memset(words, 0, sizeof(words));
        words[0U] = 1U;
        Size attachments = method <= 8U ? 1U : 0U;
        if (method == 2U)
        {
            words[1U] = 1U;
        }
        if ((method == 3U) || (method == 4U))
        {
            words[1U] = 1U;
        }
        if (method == 6U)
        {
            words[1U] = 2U;
            words[2U] = 1U;
        }
        if (method == 9U)
        {
            words[1U] = 1U;
            words[2U] = 2U;
            words[3U] = 3U;
            attachments = 1U;
        }
        if (method == 10U)
        {
            words[1U] = 2U;
            words[2U] = 1U;
            words[3U] = 99U;
        }
        if ((method == 3U) || (method == 10U))
        {
            words[4U] = 1U | (4ULL << 16U) | (1ULL << 32U);
        }
        if ((method == 11U) || (method == 12U))
        {
            words[1U] = 1U;
        }
        if (method == 13U)
        {
            attachments = 2U;
            words[1U] = 2U;
            words[2U] = 3U;
        }
        if (method == 14U)
        {
            attachments = 1U;
            words[1U] = 1U;
        }
        if (method == 15U)
        {
            attachments = 1U;
        }
        if (method == 16U)
        {
            words[1U] = 0x100000000ULL;
            words[2U] = 0x200000000ULL;
            words[3U] = 7U;
        }
        Check<S::InputV3Codec>(t, method, attachments);
    }
    std::memset(words, 0, sizeof(words));
    words[0U] = 1U;
    words[1U] = 2U;
    words[2U] = 1U;
    words[3U] = S::PackSessionPosition(-99, 40);
    words[4U] = 7U;
    words[5U] = 3U;
    words[6U] = 0xFFFF'FFFFU;
    words[7U] = 999U;
    Check<S::InputV3PointerCodec>(t, 9U, 0U);
}
int main()
{
    TestContext t{"BrokerCodecTests"};
    Face(t);
    Input(t);
    return t.Finish();
}
