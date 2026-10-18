// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Ps2Keyboard.hpp"

#include <cstdio>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Input/ProtocolFixtures.hpp"

namespace
{
namespace In = Asteria::User::Input;
using In::UInt8;
struct Mapping
{
    UInt8 Code;
    UInt8 Usage;
    bool Extended;
};
// Microsoft Keyboard Scan Code Specification rev 1.3a, Appendix A
// physical rows/notes joined to Appendix C HID usages (USB HUT section 10).
constexpr Mapping Mappings[]{
    {0x0EU, 0x35U, false}, {0x16U, 0x1EU, false}, {0x1EU, 0x1FU, false}, {0x26U, 0x20U, false}, {0x25U, 0x21U, false},
    {0x2EU, 0x22U, false}, {0x36U, 0x23U, false}, {0x3DU, 0x24U, false}, {0x3EU, 0x25U, false}, {0x46U, 0x26U, false},
    {0x45U, 0x27U, false}, {0x4EU, 0x2DU, false}, {0x55U, 0x2EU, false}, {0x66U, 0x2AU, false}, {0x0DU, 0x2BU, false},
    {0x15U, 0x14U, false}, {0x1DU, 0x1AU, false}, {0x24U, 0x08U, false}, {0x2DU, 0x15U, false}, {0x2CU, 0x17U, false},
    {0x35U, 0x1CU, false}, {0x3CU, 0x18U, false}, {0x43U, 0x0CU, false}, {0x44U, 0x12U, false}, {0x4DU, 0x13U, false},
    {0x54U, 0x2FU, false}, {0x5BU, 0x30U, false}, {0x5DU, 0x31U, false}, {0x58U, 0x39U, false}, {0x1CU, 0x04U, false},
    {0x1BU, 0x16U, false}, {0x23U, 0x07U, false}, {0x2BU, 0x09U, false}, {0x34U, 0x0AU, false}, {0x33U, 0x0BU, false},
    {0x3BU, 0x0DU, false}, {0x42U, 0x0EU, false}, {0x4BU, 0x0FU, false}, {0x4CU, 0x33U, false}, {0x52U, 0x34U, false},
    {0x5AU, 0x28U, false}, {0x12U, 0xE1U, false}, {0x61U, 0x64U, false}, {0x1AU, 0x1DU, false}, {0x22U, 0x1BU, false},
    {0x21U, 0x06U, false}, {0x2AU, 0x19U, false}, {0x32U, 0x05U, false}, {0x31U, 0x11U, false}, {0x3AU, 0x10U, false},
    {0x41U, 0x36U, false}, {0x49U, 0x37U, false}, {0x4AU, 0x38U, false}, {0x51U, 0x87U, false}, {0x59U, 0xE5U, false},
    {0x14U, 0xE0U, false}, {0x11U, 0xE2U, false}, {0x29U, 0x2CU, false}, {0x11U, 0xE6U, true},  {0x14U, 0xE4U, true},
    {0x70U, 0x49U, true},  {0x71U, 0x4CU, true},  {0x6BU, 0x50U, true},  {0x6CU, 0x4AU, true},  {0x69U, 0x4DU, true},
    {0x75U, 0x52U, true},  {0x72U, 0x51U, true},  {0x7DU, 0x4BU, true},  {0x7AU, 0x4EU, true},  {0x74U, 0x4FU, true},
    {0x77U, 0x53U, false}, {0x6CU, 0x5FU, false}, {0x6BU, 0x5CU, false}, {0x69U, 0x59U, false}, {0x4AU, 0x54U, true},
    {0x75U, 0x60U, false}, {0x73U, 0x5DU, false}, {0x72U, 0x5AU, false}, {0x70U, 0x62U, false}, {0x7CU, 0x55U, false},
    {0x7DU, 0x61U, false}, {0x74U, 0x5EU, false}, {0x7AU, 0x5BU, false}, {0x71U, 0x63U, false}, {0x7BU, 0x56U, false},
    {0x79U, 0x57U, false}, {0x6DU, 0x85U, false}, {0x5AU, 0x58U, true},  {0x76U, 0x29U, false}, {0x05U, 0x3AU, false},
    {0x06U, 0x3BU, false}, {0x04U, 0x3CU, false}, {0x0CU, 0x3DU, false}, {0x03U, 0x3EU, false}, {0x0BU, 0x3FU, false},
    {0x83U, 0x40U, false}, {0x0AU, 0x41U, false}, {0x01U, 0x42U, false}, {0x09U, 0x43U, false}, {0x78U, 0x44U, false},
    {0x07U, 0x45U, false}, {0x7CU, 0x46U, true},  {0x84U, 0x46U, false}, {0x7EU, 0x47U, false}, {0x7EU, 0x48U, true},
    {0x1FU, 0xE3U, true},  {0x27U, 0xE7U, true},  {0x2FU, 0x65U, true},  {0x13U, 0x88U, false}, {0x64U, 0x8AU, false},
    {0x67U, 0x8BU, false},
};
[[nodiscard]] In::ScanOutput Send(In::Ps2Keyboard& keyboard, const Mapping key, const bool release) noexcept
{
    if (key.Extended)
    {
        static_cast<void>(keyboard.Feed(0xE0U));
    }
    if (release)
    {
        static_cast<void>(keyboard.Feed(0xF0U));
    }
    return keyboard.Feed(key.Code);
}
template <In::Size N>
void Sequence(Asteria::TestSupport::TestContext& context, In::Ps2Keyboard& keyboard, const UInt8 (&bytes)[N],
              const In::EventKind kind, const UInt8 usage)
{
    In::Size events = 0U;
    for (const UInt8 byte : bytes)
    {
        const In::ScanOutput result = keyboard.Feed(byte);
        if (result.Result == In::ScanResult::Event)
        {
            ++events;
            ASTERIA_EXPECT(context, "special sequence usage", result.Event.Kind == kind, result.Event.Usage == usage,
                           result.Event.Modifiers == 0U);
        }
        else
        {
            ASTERIA_EXPECT(context, "special prefix", result.Result == In::ScanResult::None);
        }
    }
    ASTERIA_EXPECT(context, "one special event", events == 1U, keyboard.GetModifiers() == 0U);
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"Ps2Keyboard"};
    In::Ps2Keyboard keyboard{42U};
    for (const Mapping key : Mappings)
    {
        keyboard.Reset();
        const In::ScanOutput make = Send(keyboard, key, false);
        const In::ScanOutput repeat = Send(keyboard, key, false);
        const In::ScanOutput release = Send(keyboard, key, true);
        ASTERIA_EXPECT(context, "Microsoft Appendix A/C mapping", make.Result == In::ScanResult::Event,
                       make.Event.Usage == key.Usage, make.Event.Kind == In::EventKind::KeyDown,
                       make.Event.Source == 42U, repeat.Result == In::ScanResult::None,
                       release.Result == In::ScanResult::Event, release.Event.Kind == In::EventKind::KeyUp,
                       !keyboard.IsPressed(key.Usage));
    }
    keyboard.Reset();
    Sequence(context, keyboard, Asteria::Test::Input::PrintMake, In::EventKind::KeyDown, 0x46U);
    Sequence(context, keyboard, Asteria::Test::Input::PrintBreak, In::EventKind::KeyUp, 0x46U);
    Sequence(context, keyboard, Asteria::Test::Input::PauseMake, In::EventKind::KeyDown, 0x48U);
    Sequence(context, keyboard, Asteria::Test::Input::PauseMake, In::EventKind::KeyDown, 0x48U);
    ASTERIA_EXPECT(context, "Pause not retained", !keyboard.IsPressed(0x48U));
    constexpr UInt8 responses[]{0U, 0xFFU, 0xAAU, 0xFAU, 0xFEU, 0xEEU};
    constexpr In::ScanResult results[]{In::ScanResult::Overrun, In::ScanResult::Overrun, In::ScanResult::SelfTestPassed,
                                       In::ScanResult::Ack,     In::ScanResult::Resend,  In::ScanResult::Echo};
    for (In::Size i = 0U; i < 6U; ++i)
    {
        static_cast<void>(keyboard.Feed(0xE0U));
        ASTERIA_EXPECT(context, "controller bytes reset prefix", keyboard.Feed(responses[i]).Result == results[i],
                       keyboard.Feed(0x1CU).Event.Usage == 4U);
        keyboard.Reset();
    }
    ASTERIA_EXPECT(context, "unknown extended is not a base key", keyboard.Feed(0xE0U).Result == In::ScanResult::None,
                   keyboard.Feed(0x1CU).Result == In::ScanResult::Unknown,
                   keyboard.Feed(0x02U).Result == In::ScanResult::Unknown);
    static_cast<void>(keyboard.Feed(0xE1U));
    ASTERIA_EXPECT(context, "malformed Pause is consumed", keyboard.Feed(0x1CU).Result == In::ScanResult::Unknown,
                   keyboard.GetModifiers() == 0U);
    In::Ps2Keyboard iso{0U, In::KeyboardGeometry::Iso};
    ASTERIA_EXPECT(context, "ISO hash and Non-US backslash", iso.Feed(0x5DU).Event.Usage == 0x32U,
                   iso.Feed(0x61U).Event.Usage == 0x64U);
    // Exhaust the scan byte space: no unnamed Appendix A row becomes a key.
    for (unsigned int extended = 0U; extended < 2U; ++extended)
    {
        for (unsigned int code = 1U; code < 256U; ++code)
        {
            if ((code == 0xE0U) || (code == 0xE1U) || (code == 0xF0U) || (code == 0xFFU) || (code == 0xAAU) ||
                (code == 0xFAU) || (code == 0xFEU) || (code == 0xEEU) ||
                ((extended != 0U) && ((code == 0x12U) || (code == 0x59U))))
            {
                continue;
            }
            UInt8 expected = 0U;
            for (const Mapping row : Mappings)
            {
                if ((row.Code == code) && (row.Extended == (extended != 0U)))
                {
                    expected = row.Usage;
                }
            }
            keyboard.Reset();
            const auto result = Send(keyboard, {static_cast<UInt8>(code), expected, extended != 0U}, false);
            ASTERIA_EXPECT(context, "only specified rows map", result.Event.Usage == expected,
                           result.Result == ((expected == 0U) ? In::ScanResult::Unknown : In::ScanResult::Event));
        }
    }
    // Independent pressed-set model: random makes, breaks, repeats, resets.
    const auto seeds = Asteria::TestSupport::GetSeedRange(230U, 8U);
    for (In::Size seedAt = 0U; seedAt < seeds.Count; ++seedAt)
    {
        const auto seed = seeds.First + seedAt;
        Asteria::TestSupport::Random random{seed};
        bool pressed[256U]{};
        keyboard.Reset();
        for (In::Size step = 0U; step < 4000U; ++step)
        {
            const Mapping key = Mappings[random.Below(sizeof(Mappings) / sizeof(Mappings[0U]))];
            const bool release = random.Chance(1U, 2U);
            const bool event = pressed[key.Usage] != !release;
            pressed[key.Usage] = !release;
            UInt8 modifiers = 0U;
            for (UInt8 bit = 0U; bit < 8U; ++bit)
            {
                if (pressed[0xE0U + bit])
                {
                    modifiers = static_cast<UInt8>(modifiers | (1U << bit));
                }
            }
            const auto result = Send(keyboard, key, release);
            bool good = (result.Result == (event ? In::ScanResult::Event : In::ScanResult::None)) &&
                        (keyboard.GetModifiers() == modifiers);
            if (event)
            {
                good = good && (result.Event.Usage == key.Usage) && (result.Event.Modifiers == modifiers);
            }
            for (In::Size i = 0U; i < 256U; ++i)
            {
                good = good && (keyboard.IsPressed(static_cast<UInt8>(i)) == pressed[i]);
            }
            if (!good)
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu code=%u release=%u\n",
                                               static_cast<unsigned long long>(seed), step, key.Code,
                                               release ? 1U : 0U));
                ASTERIA_FAIL(context, "pressed-set model");
                break;
            }
            if (random.Chance(1U, 40U))
            {
                keyboard.Reset();
                for (bool& down : pressed)
                {
                    down = false;
                }
            }
        }
    }
    return context.Finish();
}
