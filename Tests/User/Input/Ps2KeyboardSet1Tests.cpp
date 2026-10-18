// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Ps2Keyboard.hpp"

#include <cstdio>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace In = Asteria::User::Input;
using In::UInt8;
// Microsoft Keyboard Scan Code Specification rev 1.3a, Appendix A: one row
// per key position, with its scan code set 1 and set 2 make codes, joined
// to the Appendix C HID usage (USB HUT 1.7, page 0x07). Typed from the
// specification's rows, not from the decoder's tables; a set-1 make code
// is its row's set-2 code as the controller's translation delivers it.
// The specification gives the Far East rows (International1/2/4/5) no
// usage; theirs (0x87, 0x88, 0x8A, 0x8B) come from the HID Usage Tables.
struct Row
{
    UInt8 Set1;
    UInt8 Set2;
    bool Extended;
    UInt8 Usage;
};
constexpr Row Rows[]{
    {0x29U, 0x0EU, false, 0x35U}, // 1 ` ~
    {0x02U, 0x16U, false, 0x1EU}, // 2 1 !
    {0x03U, 0x1EU, false, 0x1FU}, // 3 2 @
    {0x04U, 0x26U, false, 0x20U}, // 4 3 #
    {0x05U, 0x25U, false, 0x21U}, // 5 4 $
    {0x06U, 0x2EU, false, 0x22U}, // 6 5 %
    {0x07U, 0x36U, false, 0x23U}, // 7 6 ^
    {0x08U, 0x3DU, false, 0x24U}, // 8 7 &
    {0x09U, 0x3EU, false, 0x25U}, // 9 8 *
    {0x0AU, 0x46U, false, 0x26U}, // 10 9 (
    {0x0BU, 0x45U, false, 0x27U}, // 11 0 )
    {0x0CU, 0x4EU, false, 0x2DU}, // 12 - _
    {0x0DU, 0x55U, false, 0x2EU}, // 13 = +
    {0x0EU, 0x66U, false, 0x2AU}, // 15 Backspace
    {0x0FU, 0x0DU, false, 0x2BU}, // 16 Tab
    {0x10U, 0x15U, false, 0x14U}, // 17 Q
    {0x11U, 0x1DU, false, 0x1AU}, // 18 W
    {0x12U, 0x24U, false, 0x08U}, // 19 E
    {0x13U, 0x2DU, false, 0x15U}, // 20 R
    {0x14U, 0x2CU, false, 0x17U}, // 21 T
    {0x15U, 0x35U, false, 0x1CU}, // 22 Y
    {0x16U, 0x3CU, false, 0x18U}, // 23 U
    {0x17U, 0x43U, false, 0x0CU}, // 24 I
    {0x18U, 0x44U, false, 0x12U}, // 25 O
    {0x19U, 0x4DU, false, 0x13U}, // 26 P
    {0x1AU, 0x54U, false, 0x2FU}, // 27 [ {
    {0x1BU, 0x5BU, false, 0x30U}, // 28 ] }
    {0x2BU, 0x5DU, false, 0x31U}, // 29 \ | (ANSI layout)
    {0x3AU, 0x58U, false, 0x39U}, // 30 Caps Lock
    {0x1EU, 0x1CU, false, 0x04U}, // 31 A
    {0x1FU, 0x1BU, false, 0x16U}, // 32 S
    {0x20U, 0x23U, false, 0x07U}, // 33 D
    {0x21U, 0x2BU, false, 0x09U}, // 34 F
    {0x22U, 0x34U, false, 0x0AU}, // 35 G
    {0x23U, 0x33U, false, 0x0BU}, // 36 H
    {0x24U, 0x3BU, false, 0x0DU}, // 37 J
    {0x25U, 0x42U, false, 0x0EU}, // 38 K
    {0x26U, 0x4BU, false, 0x0FU}, // 39 L
    {0x27U, 0x4CU, false, 0x33U}, // 40;
    {0x28U, 0x52U, false, 0x34U}, // 41 ' "
    {0x1CU, 0x5AU, false, 0x28U}, // 43 Enter
    {0x2AU, 0x12U, false, 0xE1U}, // 44 Left Shift
    {0x56U, 0x61U, false, 0x64U}, // 45 Non-US \ |
    {0x2CU, 0x1AU, false, 0x1DU}, // 46 Z
    {0x2DU, 0x22U, false, 0x1BU}, // 47 X
    {0x2EU, 0x21U, false, 0x06U}, // 48 C
    {0x2FU, 0x2AU, false, 0x19U}, // 49 V
    {0x30U, 0x32U, false, 0x05U}, // 50 B
    {0x31U, 0x31U, false, 0x11U}, // 51 N
    {0x32U, 0x3AU, false, 0x10U}, // 52 M
    {0x33U, 0x41U, false, 0x36U}, // 53, <
    {0x34U, 0x49U, false, 0x37U}, // 54. >
    {0x35U, 0x4AU, false, 0x38U}, // 55 / ?
    {0x73U, 0x51U, false, 0x87U}, // 56 International1
    {0x36U, 0x59U, false, 0xE5U}, // 57 Right Shift
    {0x1DU, 0x14U, false, 0xE0U}, // 58 Left Control
    {0x38U, 0x11U, false, 0xE2U}, // 60 Left Alt
    {0x39U, 0x29U, false, 0x2CU}, // 61 Space
    {0x38U, 0x11U, true, 0xE6U},  // 62 Right Alt
    {0x1DU, 0x14U, true, 0xE4U},  // 64 Right Control
    {0x52U, 0x70U, true, 0x49U},  // 75 Insert
    {0x53U, 0x71U, true, 0x4CU},  // 76 Delete
    {0x4BU, 0x6BU, true, 0x50U},  // 79 Left Arrow
    {0x47U, 0x6CU, true, 0x4AU},  // 80 Home
    {0x4FU, 0x69U, true, 0x4DU},  // 81 End
    {0x48U, 0x75U, true, 0x52U},  // 83 Up Arrow
    {0x50U, 0x72U, true, 0x51U},  // 84 Down Arrow
    {0x49U, 0x7DU, true, 0x4BU},  // 85 Page Up
    {0x51U, 0x7AU, true, 0x4EU},  // 86 Page Down
    {0x4DU, 0x74U, true, 0x4FU},  // 89 Right Arrow
    {0x45U, 0x77U, false, 0x53U}, // 90 Num Lock
    {0x47U, 0x6CU, false, 0x5FU}, // 91 Keypad 7
    {0x4BU, 0x6BU, false, 0x5CU}, // 92 Keypad 4
    {0x4FU, 0x69U, false, 0x59U}, // 93 Keypad 1
    {0x35U, 0x4AU, true, 0x54U},  // 95 Keypad /
    {0x48U, 0x75U, false, 0x60U}, // 96 Keypad 8
    {0x4CU, 0x73U, false, 0x5DU}, // 97 Keypad 5
    {0x50U, 0x72U, false, 0x5AU}, // 98 Keypad 2
    {0x52U, 0x70U, false, 0x62U}, // 99 Keypad 0
    {0x37U, 0x7CU, false, 0x55U}, // 100 Keypad *
    {0x49U, 0x7DU, false, 0x61U}, // 101 Keypad 9
    {0x4DU, 0x74U, false, 0x5EU}, // 102 Keypad 6
    {0x51U, 0x7AU, false, 0x5BU}, // 103 Keypad 3
    {0x53U, 0x71U, false, 0x63U}, // 104 Keypad.
    {0x4AU, 0x7BU, false, 0x56U}, // 105 Keypad -
    {0x4EU, 0x79U, false, 0x57U}, // 106 Keypad +
    {0x1CU, 0x5AU, true, 0x58U},  // 108 Keypad Enter
    {0x01U, 0x76U, false, 0x29U}, // 110 Escape
    {0x3BU, 0x05U, false, 0x3AU}, // 112 F1
    {0x3CU, 0x06U, false, 0x3BU}, // 113 F2
    {0x3DU, 0x04U, false, 0x3CU}, // 114 F3
    {0x3EU, 0x0CU, false, 0x3DU}, // 115 F4
    {0x3FU, 0x03U, false, 0x3EU}, // 116 F5
    {0x40U, 0x0BU, false, 0x3FU}, // 117 F6
    {0x41U, 0x83U, false, 0x40U}, // 118 F7
    {0x42U, 0x0AU, false, 0x41U}, // 119 F8
    {0x43U, 0x01U, false, 0x42U}, // 120 F9
    {0x44U, 0x09U, false, 0x43U}, // 121 F10
    {0x57U, 0x78U, false, 0x44U}, // 122 F11
    {0x58U, 0x07U, false, 0x45U}, // 123 F12
    {0x37U, 0x7CU, true, 0x46U},  // 124 Print Screen (note 4, base code)
    {0x54U, 0x84U, false, 0x46U}, // 124 Print Screen with Alt (note 4)
    {0x46U, 0x7EU, false, 0x47U}, // 125 Scroll Lock
    {0x46U, 0x7EU, true, 0x48U},  // 126 Pause with Control (note 5)
    {0x5BU, 0x1FU, true, 0xE3U},  // 127 Left GUI
    {0x5CU, 0x27U, true, 0xE7U},  // 128 Right GUI
    {0x5DU, 0x2FU, true, 0x65U},  // 129 Application
    {0x70U, 0x13U, false, 0x88U}, // Far East row (Katakana/Hiragana): International2
    {0x79U, 0x64U, false, 0x8AU}, // Convert row: International4
    {0x7BU, 0x67U, false, 0x8BU}, // Nonconvert row: International5
};
constexpr In::Size RowCount{sizeof(Rows) / sizeof(Rows[0U])};

[[nodiscard]] In::ScanOutput Set1(In::Ps2Keyboard& keyboard, const UInt8 make, const bool extended,
                                  const bool release) noexcept
{
    if (extended)
    {
        static_cast<void>(keyboard.Feed(0xE0U));
    }
    return keyboard.Feed(static_cast<UInt8>(release ? (make | 0x80U) : make));
}

[[nodiscard]] In::ScanOutput Set2(In::Ps2Keyboard& keyboard, const Row& row) noexcept
{
    if (row.Extended)
    {
        static_cast<void>(keyboard.Feed(0xE0U));
    }
    return keyboard.Feed(row.Set2);
}

template <In::Size N>
[[nodiscard]] In::Size Events(In::Ps2Keyboard& keyboard, const UInt8 (&bytes)[N], const In::EventKind kind,
                              const UInt8 usage, bool& clean) noexcept
{
    In::Size events{0U};
    for (const UInt8 byte : bytes)
    {
        const In::ScanOutput result = keyboard.Feed(byte);
        if (result.Result == In::ScanResult::Event)
        {
            ++events;
            clean = clean && (result.Event.Kind == kind) && (result.Event.Usage == usage);
        }
        else
        {
            clean = clean && (result.Result == In::ScanResult::None);
        }
    }
    return events;
}
} // Namespace

int main()
{
    Asteria::TestSupport::TestContext context{"Ps2Keyboard set 1"};
    In::Ps2Keyboard keyboard{7U, In::KeyboardGeometry::Ansi, In::ScanCodeSet::Set1};
    In::Ps2Keyboard reference{7U};
    for (const Row& row : Rows)
    {
        keyboard.Reset();
        reference.Reset();
        const In::ScanOutput make = Set1(keyboard, row.Set1, row.Extended, false);
        const In::ScanOutput repeat = Set1(keyboard, row.Set1, row.Extended, false);
        const In::ScanOutput release = Set1(keyboard, row.Set1, row.Extended, true);
        const In::ScanOutput translated = Set2(reference, row);
        ASTERIA_EXPECT(context, "Appendix A set-1 row", make.Result == In::ScanResult::Event,
                       make.Event.Usage == row.Usage, make.Event.Kind == In::EventKind::KeyDown,
                       make.Event.Source == 7U, repeat.Result == In::ScanResult::None,
                       release.Result == In::ScanResult::Event, release.Event.Kind == In::EventKind::KeyUp,
                       release.Event.Usage == row.Usage, !keyboard.IsPressed(row.Usage));
        ASTERIA_EXPECT(context, "set 1 and set 2 name the same key", translated.Result == In::ScanResult::Event,
                       translated.Event.Usage == make.Event.Usage);
    }
    // Every other make code, plain or E0-prefixed, is unnamed; E0 2A and
    // E0 36 (notes 2-4) are shift compensation and produce nothing.
    for (unsigned int extended{0U}; extended < 2U; ++extended)
    {
        for (unsigned int code{1U}; code < 0x80U; ++code)
        {
            UInt8 expected{0U};
            for (const Row& row : Rows)
            {
                if ((row.Set1 == code) && (row.Extended == (extended != 0U)))
                {
                    expected = row.Usage;
                }
            }
            keyboard.Reset();
            const bool shift = (extended != 0U) && ((code == 0x2AU) || (code == 0x36U));
            const In::ScanOutput result = Set1(keyboard, static_cast<UInt8>(code), extended != 0U, false);
            In::ScanResult wanted{(expected == 0U) ? In::ScanResult::Unknown : In::ScanResult::Event};
            if (shift)
            {
                wanted = In::ScanResult::None;
            }
            ASTERIA_EXPECT(context, "only specified set-1 rows map", result.Result == wanted,
                           (expected == 0U) || (result.Event.Usage == expected));
        }
    }
    keyboard.Reset();
    ASTERIA_EXPECT(context, "Keypad Comma's break would be RESEND: unmapped",
                   keyboard.Feed(0x7EU).Result == In::ScanResult::Unknown,
                   keyboard.Feed(0xFEU).Result == In::ScanResult::Resend);
    constexpr UInt8 answers[]{0x00U, 0xFFU, 0xFAU, 0xFEU, 0xEEU};
    constexpr In::ScanResult results[]{In::ScanResult::Overrun, In::ScanResult::Overrun, In::ScanResult::Ack,
                                       In::ScanResult::Resend, In::ScanResult::Echo};
    for (In::Size index{0U}; index < 5U; ++index)
    {
        keyboard.Reset();
        static_cast<void>(keyboard.Feed(0xE0U));
        ASTERIA_EXPECT(context, "set-1 answers reset the prefix",
                       keyboard.Feed(answers[index]).Result == results[index],
                       keyboard.Feed(0x1DU).Event.Usage == 0xE0U);
    }
    keyboard.Reset();
    const In::ScanOutput shiftDown = keyboard.Feed(0x2AU);
    const In::ScanOutput shiftUp = keyboard.Feed(0xAAU);
    ASTERIA_EXPECT(context, "0xAA is Left Shift's break in set 1", shiftDown.Event.Usage == 0xE1U,
                   shiftUp.Result == In::ScanResult::Event, shiftUp.Event.Kind == In::EventKind::KeyUp,
                   shiftUp.Event.Usage == 0xE1U, keyboard.GetModifiers() == 0U);
    // Appendix A note 4: Print Screen alone is E0 2A E0 37, its break
    // E0 B7 E0 AA. Note 5: Pause is E1 1D 45 E1 9D C5 with no break.
    keyboard.Reset();
    bool clean{true};
    constexpr UInt8 printMake[]{0xE0U, 0x2AU, 0xE0U, 0x37U};
    constexpr UInt8 printBreak[]{0xE0U, 0xB7U, 0xE0U, 0xAAU};
    constexpr UInt8 pause[]{0xE1U, 0x1DU, 0x45U, 0xE1U, 0x9DU, 0xC5U};
    const In::Size printDown = Events(keyboard, printMake, In::EventKind::KeyDown, 0x46U, clean);
    const In::Size printUp = Events(keyboard, printBreak, In::EventKind::KeyUp, 0x46U, clean);
    const In::Size pauses = Events(keyboard, pause, In::EventKind::KeyDown, 0x48U, clean) +
                            Events(keyboard, pause, In::EventKind::KeyDown, 0x48U, clean);
    ASTERIA_EXPECT(context, "Print Screen and Pause sequences", clean, printDown == 1U, printUp == 1U, pauses == 2U,
                   !keyboard.IsPressed(0x48U), keyboard.GetModifiers() == 0U);
    static_cast<void>(keyboard.Feed(0xE1U));
    ASTERIA_EXPECT(context, "malformed set-1 Pause is consumed", keyboard.Feed(0x1EU).Result == In::ScanResult::Unknown,
                   keyboard.Feed(0x1EU).Event.Usage == 0x04U);
    In::Ps2Keyboard iso{0U, In::KeyboardGeometry::Iso, In::ScanCodeSet::Set1};
    ASTERIA_EXPECT(context, "set-1 ISO hash", iso.Feed(0x2BU).Event.Usage == 0x32U);
    In::Ps2Keyboard switched{0U};
    static_cast<void>(switched.Feed(0x1CU));
    switched.SetScanCodeSet(In::ScanCodeSet::Set1);
    ASTERIA_EXPECT(context, "switching the set resets the decoder", !switched.IsPressed(0x04U),
                   switched.GetScanCodeSet() == In::ScanCodeSet::Set1, switched.Feed(0x1EU).Event.Usage == 0x04U);
    // Pressed-set model over random set-1 makes and breaks.
    const auto seeds = Asteria::TestSupport::GetSeedRange(231U, 4U);
    for (In::Size seedAt{0U}; seedAt < seeds.Count; ++seedAt)
    {
        const auto seed = seeds.First + seedAt;
        Asteria::TestSupport::Random random{seed};
        bool pressed[256U]{};
        keyboard.Reset();
        for (In::Size step{0U}; step < 2000U; ++step)
        {
            const Row& row = Rows[random.Below(RowCount)];
            const bool release = random.Chance(1U, 2U);
            const bool event = pressed[row.Usage] != !release;
            pressed[row.Usage] = !release;
            UInt8 modifiers{0U};
            for (UInt8 bit{0U}; bit < 8U; ++bit)
            {
                if (pressed[0xE0U + bit])
                {
                    modifiers = static_cast<UInt8>(modifiers | (1U << bit));
                }
            }
            const In::ScanOutput result = Set1(keyboard, row.Set1, row.Extended, release);
            bool good = (result.Result == (event ? In::ScanResult::Event : In::ScanResult::None)) &&
                        (keyboard.GetModifiers() == modifiers);
            if (event)
            {
                good = good && (result.Event.Usage == row.Usage) && (result.Event.Modifiers == modifiers);
            }
            for (In::Size usage{0U}; usage < 256U; ++usage)
            {
                good = good && (keyboard.IsPressed(static_cast<UInt8>(usage)) == pressed[usage]);
            }
            if (!good)
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu code=%u release=%u\n",
                                               static_cast<unsigned long long>(seed), step, row.Set1,
                                               release ? 1U : 0U));
                ASTERIA_FAIL(context, "set-1 pressed-set model");
                break;
            }
        }
    }
    return context.Finish();
}
