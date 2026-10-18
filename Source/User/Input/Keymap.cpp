// SPDX-License-Identifier: Apache-2.0

#include "User/Input/Keymap.hpp"

namespace Asteria::User::Input
{
namespace
{
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt8;

/// Usages 0x04 (A) to 0x38, unshifted and shifted; zero marks a key that
/// types no character (Enter, Escape, Backspace, Tab, the non-US '#').
constexpr UInt16 FirstCharacterUsage{0x04U};
constexpr UInt16 LastCharacterUsage{0x38U};
constexpr char Plain[]{"abcdefghijklmnopqrstuvwxyz1234567890\0\0\0\0 -=[]\\\0;'`,./"};
constexpr char Shifted[]{"ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?"};
static_assert((sizeof(Plain) == (LastCharacterUsage - FirstCharacterUsage + 2U)) && (sizeof(Shifted) == sizeof(Plain)));
/// The keypad's operator keys, 0x54-0x57.
constexpr UInt16 FirstKeypadUsage{0x54U};
constexpr char Keypad[]{"/*-+"};

struct LayoutEntry
{
    UInt32 Plain;
    UInt32 Shifted;
};
constexpr Foundation::Size LayoutUsageCount{0x8CU};
struct LayoutTable
{
    LayoutEntry Entries[LayoutUsageCount];
};
[[nodiscard]] consteval LayoutTable MakeUsLayout() noexcept
{
    LayoutTable table{};
    for (UInt16 usage{FirstCharacterUsage}; usage <= LastCharacterUsage; ++usage)
    {
        table.Entries[usage] = {static_cast<UInt32>(Plain[usage - FirstCharacterUsage]),
                                static_cast<UInt32>(Shifted[usage - FirstCharacterUsage])};
    }
    for (UInt16 at{0U}; at < 4U; ++at)
    {
        table.Entries[FirstKeypadUsage + at] = {static_cast<UInt32>(Keypad[at]), static_cast<UInt32>(Keypad[at])};
    }
    return table;
}
inline constexpr LayoutTable UsLayout{MakeUsLayout()};
struct JisPosition
{
    UInt16 Usage;
    LayoutEntry Text;
};
/// HID Usage Tables v1.5 section 10, table 10.1 supplies physical usages;
/// Japanese 106/109-key legends below are from general knowledge. Yen's
/// U+00A5 and ro's U+005C remain distinct Unicode characters.
constexpr JisPosition JisPositions[]{
    {0x1FU, {'2', '"'}},  {0x23U, {'6', '&'}},    {0x24U, {'7', '\''}}, {0x25U, {'8', '('}}, {0x26U, {'9', ')'}},
    {0x27U, {'0', 0U}},   {0x2DU, {'-', '='}},    {0x2EU, {'^', '~'}},  {0x2FU, {'@', '`'}}, {0x30U, {'[', '{'}},
    {0x31U, {0U, 0U}},    {0x32U, {']', '}'}},    {0x33U, {';', '+'}},  {0x34U, {':', '*'}}, {0x35U, {0U, 0U}},
    {0x87U, {'\\', '_'}}, {0x89U, {0x00A5U, '|'}}};
[[nodiscard]] consteval LayoutTable MakeJisLayout() noexcept
{
    LayoutTable table{UsLayout};
    for (const auto position : JisPositions)
    {
        table.Entries[position.Usage] = position.Text;
    }
    return table;
}
inline constexpr LayoutTable JisLayout{MakeJisLayout()};
static_assert(sizeof(UsLayout.Entries) == sizeof(JisLayout.Entries));

constexpr KeyCharacter None{.Present = false, .CodePoint = 0U};
} // Namespace

KeyCharacter LayoutKeyCharacter(const KeyboardLayout layout, const UInt16 usage, const UInt8 modifiers) noexcept
{
    if ((layout != KeyboardLayout::Us) && (layout != KeyboardLayout::Jis))
    {
        return None;
    }
    const bool control{(modifiers & KeymapControl) != 0U};
    const bool shift{(modifiers & KeymapShift) != 0U};
    if (usage >= LayoutUsageCount)
    {
        return None;
    }
    const LayoutEntry text{layout == KeyboardLayout::Jis ? JisLayout.Entries[usage] : UsLayout.Entries[usage]};
    const UInt32 plain{text.Plain};
    const UInt32 shifted{text.Shifted};
    if (plain == 0U)
    {
        return None;
    }
    if (!control)
    {
        return (shift && (shifted == 0U)) ? None : KeyCharacter{true, shift ? shifted : plain};
    }
    if ((plain >= 'a') && (plain <= 'z'))
    {
        return KeyCharacter{.Present = true, .CodePoint = static_cast<UInt8>(plain - 'a' + 1)};
    }
    if ((layout == KeyboardLayout::Jis) && (usage == 0x87U) && shift)
    {
        return KeyCharacter{true, 0x1FU};
    }
    if ((plain >= '[') && (plain <= ']'))
    {
        return KeyCharacter{.Present = true, .CodePoint = static_cast<UInt8>(plain - '[' + 0x1B)};
    }
    if ((plain == 0x00A5U) || (plain == '\\'))
    {
        return KeyCharacter{true, 0x1CU};
    }
    if (plain == ' ')
    {
        return KeyCharacter{.Present = true, .CodePoint = 0U};
    }
    return None;
}
KeyCharacter UsKeyCharacter(const UInt16 usage, const UInt8 modifiers) noexcept
{
    return LayoutKeyCharacter(KeyboardLayout::Us, usage, modifiers);
}
} // Namespace Asteria::User::Input
