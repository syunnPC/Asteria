// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Ps2Mouse.hpp"

#include <cstdio>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Input/ProtocolFixtures.hpp"

int main()
{
    namespace In = Asteria::User::Input;
    Asteria::TestSupport::TestContext context{"Ps2Mouse"};
    In::Ps2Mouse mouse{9U};
    In::EventBatch output{};
    ASTERIA_EXPECT(context, "sync bit", mouse.Feed(0U, output) == In::MouseResult::Unsynchronised, output.Count == 0U);
    for (const In::UInt8 byte : Asteria::Test::Input::Ps2Motion)
    {
        static_cast<void>(mouse.Feed(byte, output));
    }
    ASTERIA_EXPECT(context, "synthetic packet", output.Count == 2U, output.Events[0U].Dx == 5,
                   output.Events[0U].Dy == -3, output.Events[1U].Buttons == 1U, output.Events[0U].Source == 9U);
    static_cast<void>(mouse.Feed(0x39U, output));
    static_cast<void>(mouse.Feed(0U, output));
    ASTERIA_EXPECT(context, "9-bit extremes", mouse.Feed(0U, output) == In::MouseResult::Packet, output.Count == 1U,
                   output.Events[0U].Dx == -256, output.Events[0U].Dy == 256);
    static_cast<void>(mouse.Feed(0xC8U, output));
    static_cast<void>(mouse.Feed(0xFFU, output));
    ASTERIA_EXPECT(context, "overflow keeps button release", mouse.Feed(0xFFU, output) == In::MouseResult::Overflow,
                   output.Count == 1U, output.Events[0U].Kind == In::EventKind::Buttons,
                   output.Events[0U].Buttons == 0U);
    mouse.Reset(In::MouseMode::IntelliMouse);
    static_cast<void>(mouse.Feed(8U, output));
    static_cast<void>(mouse.Feed(0U, output));
    ASTERIA_EXPECT(context, "four-byte mode waits", mouse.Feed(0U, output) == In::MouseResult::Pending,
                   output.Count == 0U);
    ASTERIA_EXPECT(context, "ID3 signed full wheel byte", mouse.Feed(0x80U, output) == In::MouseResult::Packet,
                   output.Count == 1U, output.Events[0U].Wheel == 128);
    mouse.Reset(In::MouseMode::IntelliMouse);
    static_cast<void>(mouse.Feed(8U, output));
    static_cast<void>(mouse.Feed(0U, output));
    static_cast<void>(mouse.Feed(0U, output));
    ASTERIA_EXPECT(context, "synthetic PS/2 wheel up normalised", mouse.Feed(0xFFU, output) == In::MouseResult::Packet,
                   output.Count == 1U, output.Events[0U].Wheel == 1);
    const auto seeds = Asteria::TestSupport::GetSeedRange(250U, 8U);
    for (In::Size seedAt = 0U; seedAt < seeds.Count; ++seedAt)
    {
        const auto seed = seeds.First + seedAt;
        Asteria::TestSupport::Random random{seed};
        mouse.Reset(In::MouseMode::IntelliMouse);
        In::UInt8 previous = 0U;
        for (In::Size step = 0U; step < 2000U; ++step)
        {
            const auto flags = static_cast<In::UInt8>(random.Below(256U) | 8U);
            const auto x = static_cast<In::UInt8>(random.Below(256U));
            const auto y = static_cast<In::UInt8>(random.Below(256U));
            const auto wheel = static_cast<In::UInt8>(random.Below(256U));
            const int dx = (flags & 0xC0U) != 0U ? 0 : x - ((flags & 16U) != 0U ? 256 : 0);
            const int dy = (flags & 0xC0U) != 0U ? 0 : -y + ((flags & 32U) != 0U ? 256 : 0);
            const int delta = wheel < 128U ? -wheel : 256 - wheel;
            const auto buttons = static_cast<In::UInt8>(flags & 7U);
            static_cast<void>(mouse.Feed(flags, output));
            static_cast<void>(mouse.Feed(x, output));
            static_cast<void>(mouse.Feed(y, output));
            const auto result = mouse.Feed(wheel, output);
            In::Size at = 0U;
            bool good = result == ((flags & 0xC0U) != 0U ? In::MouseResult::Overflow : In::MouseResult::Packet);
            if ((dx != 0) || (dy != 0))
            {
                good = good && (at < output.Count) && (output.Events[at].Kind == In::EventKind::Motion) &&
                       (output.Events[at].Dx == dx) && (output.Events[at].Dy == dy);
                ++at;
            }
            if (buttons != previous)
            {
                good = good && (at < output.Count) && (output.Events[at].Kind == In::EventKind::Buttons) &&
                       (output.Events[at].Buttons == buttons);
                ++at;
            }
            if (delta != 0)
            {
                good = good && (at < output.Count) && (output.Events[at].Kind == In::EventKind::Wheel) &&
                       (output.Events[at].Wheel == delta);
                ++at;
            }
            previous = buttons;
            if (!good || (at != output.Count))
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu flags=%u\n",
                                               static_cast<unsigned long long>(seed), step, flags));
                ASTERIA_FAIL(context, "mouse packet model");
                break;
            }
        }
    }
    return context.Finish();
}
