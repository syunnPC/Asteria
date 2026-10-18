// SPDX-License-Identifier: Apache-2.0
#include "User/Terminal/TerminalKeys.hpp"

#include <cstdio>
#include <cstring>

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// The bytes each key press sends (HID keyboard usage and modifiers, US
// layout), including what sends nothing and the bounded output.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt8;
using User::Terminal::EncodeTerminalKey;

constexpr UInt8 LeftControl{0x01U};
constexpr UInt8 LeftShift{0x02U};
constexpr UInt8 LeftAlt{0x04U};
constexpr UInt8 LeftGui{0x08U};
constexpr UInt8 RightControl{0x10U};
constexpr UInt8 RightShift{0x20U};
constexpr UInt8 RightAlt{0x40U};
constexpr UInt8 RightGui{0x80U};

struct KeyCase
{
    const char* Name;
    UInt16 Usage;
    UInt8 Modifiers;
    bool Application;
    const char* Expected;
    Size ExpectedLength;
};

#define ASTERIA_KEY(name, usage, modifiers, application, expected)                                                     \
    KeyCase{name, usage, modifiers, application, expected, sizeof(expected) - 1U}

constexpr KeyCase KeyCases[]{
    ASTERIA_KEY("a", 0x04U, 0U, false, "a"),
    ASTERIA_KEY("z", 0x1DU, 0U, false, "z"),
    ASTERIA_KEY("Shift a", 0x04U, LeftShift, false, "A"),
    ASTERIA_KEY("right Shift z", 0x1DU, RightShift, false, "Z"),
    ASTERIA_KEY("1", 0x1EU, 0U, false, "1"),
    ASTERIA_KEY("Shift 1", 0x1EU, LeftShift, false, "!"),
    ASTERIA_KEY("0", 0x27U, 0U, false, "0"),
    ASTERIA_KEY("Shift 0", 0x27U, LeftShift, false, ")"),
    ASTERIA_KEY("Shift 2", 0x1FU, LeftShift, false, "@"),
    ASTERIA_KEY("Space", 0x2CU, 0U, false, " "),
    ASTERIA_KEY("Shift Space", 0x2CU, LeftShift, false, " "),
    ASTERIA_KEY("minus", 0x2DU, 0U, false, "-"),
    ASTERIA_KEY("Shift minus", 0x2DU, LeftShift, false, "_"),
    ASTERIA_KEY("equals", 0x2EU, 0U, false, "="),
    ASTERIA_KEY("Shift equals", 0x2EU, LeftShift, false, "+"),
    ASTERIA_KEY("left bracket", 0x2FU, 0U, false, "["),
    ASTERIA_KEY("Shift right bracket", 0x30U, LeftShift, false, "}"),
    ASTERIA_KEY("backslash", 0x31U, 0U, false, "\\"),
    ASTERIA_KEY("Shift backslash", 0x31U, LeftShift, false, "|"),
    ASTERIA_KEY("semicolon", 0x33U, 0U, false, ";"),
    ASTERIA_KEY("Shift quote", 0x34U, LeftShift, false, "\""),
    ASTERIA_KEY("grave", 0x35U, 0U, false, "`"),
    ASTERIA_KEY("Shift grave", 0x35U, LeftShift, false, "~"),
    ASTERIA_KEY("Shift comma", 0x36U, LeftShift, false, "<"),
    ASTERIA_KEY("period", 0x37U, 0U, false, "."),
    ASTERIA_KEY("Shift slash", 0x38U, LeftShift, false, "?"),
    ASTERIA_KEY("Enter", 0x28U, 0U, false, "\r"),
    ASTERIA_KEY("keypad Enter", 0x58U, 0U, false, "\r"),
    ASTERIA_KEY("Escape", 0x29U, 0U, false, "\x1b"),
    ASTERIA_KEY("Backspace sends DEL", 0x2AU, 0U, false, "\x7f"),
    ASTERIA_KEY("Tab", 0x2BU, 0U, false, "\t"),
    ASTERIA_KEY("Shift Tab", 0x2BU, LeftShift, false, "\t"),
    ASTERIA_KEY("keypad slash", 0x54U, 0U, false, "/"),
    ASTERIA_KEY("keypad star", 0x55U, 0U, false, "*"),
    ASTERIA_KEY("keypad minus", 0x56U, 0U, false, "-"),
    ASTERIA_KEY("keypad plus", 0x57U, 0U, false, "+"),
    ASTERIA_KEY("Up", 0x52U, 0U, false, "\x1b[A"),
    ASTERIA_KEY("Down", 0x51U, 0U, false, "\x1b[B"),
    ASTERIA_KEY("Right", 0x4FU, 0U, false, "\x1b[C"),
    ASTERIA_KEY("Left", 0x50U, 0U, false, "\x1b[D"),
    ASTERIA_KEY("application Up", 0x52U, 0U, true, "\x1bOA"),
    ASTERIA_KEY("application Down", 0x51U, 0U, true, "\x1bOB"),
    ASTERIA_KEY("application Right", 0x4FU, 0U, true, "\x1bOC"),
    ASTERIA_KEY("application Left", 0x50U, 0U, true, "\x1bOD"),
    ASTERIA_KEY("application mode does not change letters", 0x04U, 0U, true, "a"),
    ASTERIA_KEY("Home", 0x4AU, 0U, false, "\x1b[1~"),
    ASTERIA_KEY("Insert", 0x49U, 0U, false, "\x1b[2~"),
    ASTERIA_KEY("Delete", 0x4CU, 0U, false, "\x1b[3~"),
    ASTERIA_KEY("End", 0x4DU, 0U, false, "\x1b[4~"),
    ASTERIA_KEY("Page Up", 0x4BU, 0U, false, "\x1b[5~"),
    ASTERIA_KEY("Page Down", 0x4EU, 0U, false, "\x1b[6~"),
    ASTERIA_KEY("F1", 0x3AU, 0U, false, "\x1bOP"),
    ASTERIA_KEY("F2", 0x3BU, 0U, false, "\x1bOQ"),
    ASTERIA_KEY("F3", 0x3CU, 0U, false, "\x1bOR"),
    ASTERIA_KEY("F4", 0x3DU, 0U, false, "\x1bOS"),
    ASTERIA_KEY("F5", 0x3EU, 0U, false, "\x1b[15~"),
    ASTERIA_KEY("F6", 0x3FU, 0U, false, "\x1b[17~"),
    ASTERIA_KEY("F7", 0x40U, 0U, false, "\x1b[18~"),
    ASTERIA_KEY("F8", 0x41U, 0U, false, "\x1b[19~"),
    ASTERIA_KEY("F9", 0x42U, 0U, false, "\x1b[20~"),
    ASTERIA_KEY("F10", 0x43U, 0U, false, "\x1b[21~"),
    ASTERIA_KEY("F11", 0x44U, 0U, false, "\x1b[23~"),
    ASTERIA_KEY("F12", 0x45U, 0U, false, "\x1b[24~"),
    ASTERIA_KEY("Ctrl a", 0x04U, LeftControl, false, "\x01"),
    ASTERIA_KEY("right Ctrl z", 0x1DU, RightControl, false, "\x1a"),
    ASTERIA_KEY("Ctrl Shift c", 0x06U, LeftControl | LeftShift, false, "\x03"),
    ASTERIA_KEY("Ctrl left bracket", 0x2FU, LeftControl, false, "\x1b"),
    ASTERIA_KEY("Ctrl backslash", 0x31U, LeftControl, false, "\x1c"),
    ASTERIA_KEY("Ctrl right bracket", 0x30U, LeftControl, false, "\x1d"),
    ASTERIA_KEY("Ctrl Space", 0x2CU, LeftControl, false, "\0"),
    ASTERIA_KEY("Ctrl Up is Up", 0x52U, LeftControl, false, "\x1b[A"),
    ASTERIA_KEY("Ctrl Enter is CR", 0x28U, LeftControl, false, "\r"),
    ASTERIA_KEY("Alt a", 0x04U, LeftAlt, false,
                "\x1b"
                "a"),
    ASTERIA_KEY("right Alt Shift a", 0x04U, RightAlt | LeftShift, false,
                "\x1b"
                "A"),
    ASTERIA_KEY("Alt Ctrl a", 0x04U, LeftAlt | LeftControl, false, "\x1b\x01"),
    ASTERIA_KEY("Alt Up", 0x52U, LeftAlt, false, "\x1b\x1b[A"),
    ASTERIA_KEY("Alt F12, the longest", 0x45U, LeftAlt, false, "\x1b\x1b[24~"),
    ASTERIA_KEY("Alt Backspace", 0x2AU, LeftAlt, false, "\x1b\x7f"),
    // Keys that send nothing.
    ASTERIA_KEY("usage 0", 0x00U, 0U, false, ""),
    ASTERIA_KEY("usage 3", 0x03U, 0U, false, ""),
    ASTERIA_KEY("non-US #", 0x32U, 0U, false, ""),
    ASTERIA_KEY("Caps Lock", 0x39U, 0U, false, ""),
    ASTERIA_KEY("Print Screen", 0x46U, 0U, false, ""),
    ASTERIA_KEY("Num Lock", 0x53U, 0U, false, ""),
    ASTERIA_KEY("keypad 1", 0x59U, 0U, false, ""),
    ASTERIA_KEY("Left Control itself", 0xE0U, LeftControl, false, ""),
    ASTERIA_KEY("Right GUI itself", 0xE7U, RightGui, false, ""),
    ASTERIA_KEY("a large usage", 0xFFFFU, 0U, false, ""),
    ASTERIA_KEY("Ctrl 1", 0x1EU, LeftControl, false, ""),
    ASTERIA_KEY("Ctrl period", 0x37U, LeftControl, false, ""),
    ASTERIA_KEY("Alt Ctrl 1", 0x1EU, LeftAlt | LeftControl, false, ""),
    ASTERIA_KEY("GUI a", 0x04U, LeftGui, false, ""),
    ASTERIA_KEY("right GUI Enter", 0x28U, RightGui, false, ""),
};

void TestKeys(TestContext& runner) noexcept
{
    for (const KeyCase& test : KeyCases)
    {
        UInt8 out[User::Terminal::TerminalKeyMaximumBytes + 4U]{};
        for (UInt8& byte : out)
        {
            byte = 0xA5U;
        }
        const Size count{EncodeTerminalKey(test.Usage, test.Modifiers, test.Application, out, sizeof(out))};
        bool tail{true};
        for (Size index{count}; index < sizeof(out); ++index)
        {
            tail = tail && (out[index] == 0xA5U);
        }
        if (!ASTERIA_EXPECT(runner, test.Name, count == test.ExpectedLength,
                            std::memcmp(out, test.Expected, test.ExpectedLength) == 0, tail,
                            count <= User::Terminal::TerminalKeyMaximumBytes))
        {
            static_cast<void>(std::fprintf(stderr, "  %zu bytes\n", count));
        }
    }
}

void TestBounds(TestContext& runner) noexcept
{
    UInt8 out[8U]{0xA5U, 0xA5U, 0xA5U, 0xA5U, 0xA5U, 0xA5U, 0xA5U, 0xA5U};
    ASTERIA_EXPECT(runner, "a buffer too small takes nothing", EncodeTerminalKey(0x45U, LeftAlt, false, out, 5U) == 0U,
                   out[0U] == 0xA5U, EncodeTerminalKey(0x04U, LeftAlt, false, out, 1U) == 0U, out[0U] == 0xA5U,
                   EncodeTerminalKey(0x04U, 0U, false, out, 0U) == 0U, out[0U] == 0xA5U);
    ASTERIA_EXPECT(runner, "an exact buffer is enough", EncodeTerminalKey(0x45U, LeftAlt, false, out, 6U) == 6U,
                   out[6U] == 0xA5U, EncodeTerminalKey(0x04U, 0U, false, out, 1U) == 1U);
    ASTERIA_EXPECT(runner, "a null buffer takes nothing", EncodeTerminalKey(0x04U, 0U, false, nullptr, 8U) == 0U);
    // Every usage and modifier combination stays within the bound.
    bool bounded{true};
    for (UInt16 usage{0U}; usage < 0x100U; ++usage)
    {
        for (UInt16 modifiers{0U}; modifiers < 0x100U; ++modifiers)
        {
            UInt8 any[User::Terminal::TerminalKeyMaximumBytes]{};
            const Size count{
                EncodeTerminalKey(usage, static_cast<UInt8>(modifiers), (usage & 1U) != 0U, any, sizeof(any))};
            bounded = bounded && (count <= User::Terminal::TerminalKeyMaximumBytes) &&
                      (((modifiers & 0x88U) == 0U) || (count == 0U));
        }
    }
    ASTERIA_EXPECT(runner, "every key is bounded and GUI sends nothing", bounded);
}
void TestJis(TestContext& runner) noexcept
{
    using User::Input::KeyboardLayout;
    UInt8 out[8U]{};
    ASTERIA_EXPECT(runner, "JIS yen emits UTF-8",
                   EncodeTerminalKey(0x89U, 0U, false, out, sizeof(out), KeyboardLayout::Jis) == 2U, out[0U] == 0xC2U,
                   out[1U] == 0xA5U);
    ASTERIA_EXPECT(runner, "JIS Alt yen prepends ESC before UTF-8",
                   EncodeTerminalKey(0x89U, LeftAlt, false, out, sizeof(out), KeyboardLayout::Jis) == 3U,
                   out[0U] == 0x1BU, out[1U] == 0xC2U, out[2U] == 0xA5U);
    out[0U] = 0xA5U;
    ASTERIA_EXPECT(runner, "UTF-8 capacity failure writes nothing",
                   EncodeTerminalKey(0x89U, 0U, false, out, 1U, KeyboardLayout::Jis) == 0U, out[0U] == 0xA5U);
    ASTERIA_EXPECT(runner, "JIS ro and shifted ro",
                   EncodeTerminalKey(0x87U, 0U, false, out, sizeof(out), KeyboardLayout::Jis) == 1U, out[0U] == '\\',
                   EncodeTerminalKey(0x87U, LeftShift, false, out, sizeof(out), KeyboardLayout::Jis) == 1U,
                   out[0U] == '_');
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TerminalKeysTests"};
    Asteria::Test::TestKeys(runner);
    Asteria::Test::TestBounds(runner);
    Asteria::Test::TestJis(runner);
    return runner.Finish();
}
