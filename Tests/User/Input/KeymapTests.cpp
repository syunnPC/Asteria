// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Keymap.hpp"
#include "User/Input/Ps2Keyboard.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// The shared US layout: every character key with and without Shift, the control codes, and the keys
// that type nothing.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt8;
using User::Input::KeyCharacter;
using User::Input::UsKeyCharacter;

constexpr UInt8 LeftControl{0x01U};
constexpr UInt8 LeftShift{0x02U};
constexpr UInt8 LeftAlt{0x04U};
constexpr UInt8 LeftGui{0x08U};
constexpr UInt8 RightControl{0x10U};
constexpr UInt8 RightShift{0x20U};
constexpr UInt8 RightAlt{0x40U};

void TestCharacterKeys(TestContext& runner) noexcept
{
    // Usages 0x04-0x38 in order, '\0' where the key types nothing.
    constexpr char Plain[]{"abcdefghijklmnopqrstuvwxyz1234567890\0\0\0\0 -=[]\\\0;'`,./"};
    constexpr char Shifted[]{"ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?"};
    bool plain{true};
    bool shifted{true};
    bool right{true};
    bool alt{true};
    for (UInt16 usage{0x04U}; usage <= 0x38U; ++usage)
    {
        const char expected{Plain[usage - 0x04U]};
        const KeyCharacter none{UsKeyCharacter(usage, 0U)};
        const KeyCharacter shift{UsKeyCharacter(usage, LeftShift)};
        const KeyCharacter other{UsKeyCharacter(usage, RightShift)};
        const KeyCharacter modified{UsKeyCharacter(usage, LeftAlt | RightAlt | LeftGui)};
        plain = plain && (none.Present == (expected != '\0')) &&
                (!none.Present || (none.CodePoint == static_cast<UInt8>(expected)));
        shifted = shifted && (shift.Present == (expected != '\0')) &&
                  (!shift.Present || (shift.CodePoint == static_cast<UInt8>(Shifted[usage - 0x04U])));
        right = right && (other.Present == shift.Present) && (other.CodePoint == shift.CodePoint);
        alt = alt && (modified.Present == none.Present) && (modified.CodePoint == none.CodePoint);
    }
    ASTERIA_EXPECT(runner, "every character key types its US character", plain);
    ASTERIA_EXPECT(runner, "Shift selects the shifted character", shifted);
    ASTERIA_EXPECT(runner, "either Shift", right);
    ASTERIA_EXPECT(runner, "Alt and GUI do not change the character", alt);
    ASTERIA_EXPECT(runner, "a few characters by name", UsKeyCharacter(0x04U, 0U).CodePoint == 'a',
                   UsKeyCharacter(0x1EU, LeftShift).CodePoint == '!', UsKeyCharacter(0x31U, 0U).CodePoint == '\\',
                   UsKeyCharacter(0x34U, LeftShift).CodePoint == '"',
                   UsKeyCharacter(0x2CU, LeftShift).CodePoint == ' ');
}

struct ControlCase
{
    const char* Name;
    UInt16 Usage;
    UInt8 Modifiers;
    bool Present;
    UInt8 CodePoint;
};

constexpr ControlCase ControlCases[]{
    {"Ctrl a", 0x04U, LeftControl, true, 0x01U},
    {"Ctrl z", 0x1DU, LeftControl, true, 0x1AU},
    {"right Ctrl m", 0x10U, RightControl, true, 0x0DU},
    {"Ctrl Shift c", 0x06U, LeftControl | LeftShift, true, 0x03U},
    {"Ctrl Alt d", 0x07U, LeftControl | LeftAlt, true, 0x04U},
    {"Ctrl [", 0x2FU, LeftControl, true, 0x1BU},
    {"Ctrl \\", 0x31U, LeftControl, true, 0x1CU},
    {"Ctrl ]", 0x30U, LeftControl, true, 0x1DU},
    {"Ctrl Space", 0x2CU, LeftControl, true, 0x00U},
    {"Ctrl 1", 0x1EU, LeftControl, false, 0U},
    {"Ctrl minus", 0x2DU, LeftControl, false, 0U},
    {"Ctrl slash", 0x38U, LeftControl, false, 0U},
    {"Ctrl keypad plus", 0x57U, LeftControl, false, 0U},
    {"keypad slash", 0x54U, 0U, true, '/'},
    {"keypad star", 0x55U, LeftShift, true, '*'},
    {"keypad minus", 0x56U, 0U, true, '-'},
    {"keypad plus", 0x57U, 0U, true, '+'},
    {"Enter", 0x28U, 0U, false, 0U},
    {"Escape", 0x29U, 0U, false, 0U},
    {"Backspace", 0x2AU, 0U, false, 0U},
    {"Tab", 0x2BU, 0U, false, 0U},
    {"non-US #", 0x32U, 0U, false, 0U},
    {"usage 3", 0x03U, 0U, false, 0U},
    {"usage 0x39 (Caps Lock)", 0x39U, LeftShift, false, 0U},
    {"usage 0x53 (Num Lock)", 0x53U, 0U, false, 0U},
    {"usage 0x58 (keypad Enter)", 0x58U, 0U, false, 0U},
    {"usage 0x59 (keypad 1)", 0x59U, 0U, false, 0U},
    {"usage 0xE1 (Left Shift)", 0xE1U, LeftShift, false, 0U},
    {"a large usage", 0xFFFFU, 0U, false, 0U},
};

void TestControlKeys(TestContext& runner) noexcept
{
    for (const ControlCase& test : ControlCases)
    {
        const KeyCharacter character{UsKeyCharacter(test.Usage, test.Modifiers)};
        ASTERIA_EXPECT(runner, test.Name, character.Present == test.Present,
                       !character.Present || (character.CodePoint == test.CodePoint),
                       character.Present || (character.CodePoint == 0U));
    }
    // Only the listed keys type anything, whatever the modifiers.
    Size typed{0U};
    for (UInt16 usage{0U}; usage < 0x100U; ++usage)
    {
        typed += UsKeyCharacter(usage, 0U).Present ? 1U : 0U;
    }
    ASTERIA_EXPECT(runner, "52 keys type a character (48 main, 4 keypad)", typed == 52U);
}
void TestLayouts(TestContext& runner) noexcept
{
    using User::Input::KeyboardLayout;
    using User::Input::LayoutKeyCharacter;
    struct Case
    {
        UInt16 Usage;
        Foundation::UInt32 Plain;
        Foundation::UInt32 Shifted;
    };
    constexpr Case cases[]{{0x1FU, '2', '"'}, {0x23U, '6', '&'},  {0x24U, '7', '\''},   {0x25U, '8', '('},
                           {0x26U, '9', ')'}, {0x27U, '0', 0U},   {0x2DU, '-', '='},    {0x2EU, '^', '~'},
                           {0x2FU, '@', '`'}, {0x30U, '[', '{'},  {0x32U, ']', '}'},    {0x33U, ';', '+'},
                           {0x34U, ':', '*'}, {0x87U, '\\', '_'}, {0x89U, 0x00A5U, '|'}};
    for (const auto& test : cases)
    {
        const auto plain = LayoutKeyCharacter(KeyboardLayout::Jis, test.Usage, 0U);
        const auto shift = LayoutKeyCharacter(KeyboardLayout::Jis, test.Usage, RightShift);
        ASTERIA_EXPECT(runner, "JIS physical positions produce Unicode", plain.Present, plain.CodePoint == test.Plain,
                       shift.Present == (test.Shifted != 0U), shift.CodePoint == test.Shifted);
    }
    ASTERIA_EXPECT(runner, "per-device layout selection has no global state",
                   LayoutKeyCharacter(KeyboardLayout::Jis, 0x2FU, 0U).CodePoint == '@',
                   LayoutKeyCharacter(KeyboardLayout::Us, 0x2FU, 0U).CodePoint == '[',
                   LayoutKeyCharacter(KeyboardLayout::Jis, 0x2FU, 0U).CodePoint == '@',
                   !LayoutKeyCharacter(static_cast<KeyboardLayout>(255U), 0x04U, 0U).Present);
    ASTERIA_EXPECT(runner, "JIS Ctrl follows the selected layout",
                   LayoutKeyCharacter(KeyboardLayout::Jis, 0x30U, LeftControl).CodePoint == 0x1BU,
                   LayoutKeyCharacter(KeyboardLayout::Jis, 0x32U, LeftControl).CodePoint == 0x1DU,
                   LayoutKeyCharacter(KeyboardLayout::Jis, 0x89U, LeftControl).CodePoint == 0x1CU,
                   LayoutKeyCharacter(KeyboardLayout::Jis, 0x87U, LeftControl | LeftShift).CodePoint == 0x1FU,
                   !LayoutKeyCharacter(KeyboardLayout::Jis, 0x88U, 0U).Present,
                   !LayoutKeyCharacter(KeyboardLayout::Jis, 0x8AU, 0U).Present,
                   !LayoutKeyCharacter(KeyboardLayout::Jis, 0x8BU, 0U).Present);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"KeymapTests"};
    Asteria::Test::TestCharacterKeys(runner);
    Asteria::Test::TestControlKeys(runner);
    Asteria::Test::TestLayouts(runner);
    return runner.Finish();
}
