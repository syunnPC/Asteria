// SPDX-License-Identifier: Apache-2.0

#include "User/Terminal/TerminalKeys.hpp"

#include "User/Input/Keymap.hpp"
#include "User/Text/Utf8.hpp"

namespace Asteria::User::Terminal
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt8;

constexpr UInt8 Escape{0x1BU};
constexpr UInt16 EnterUsage{0x28U};
constexpr UInt16 EscapeUsage{0x29U};
constexpr UInt16 BackspaceUsage{0x2AU};
constexpr UInt16 TabUsage{0x2BU};
constexpr UInt16 F1Usage{0x3AU};
constexpr UInt16 F12Usage{0x45U};
constexpr UInt16 InsertUsage{0x49U};
constexpr UInt16 HomeUsage{0x4AU};
constexpr UInt16 PageUpUsage{0x4BU};
constexpr UInt16 DeleteUsage{0x4CU};
constexpr UInt16 EndUsage{0x4DU};
constexpr UInt16 PageDownUsage{0x4EU};
constexpr UInt16 RightUsage{0x4FU};
constexpr UInt16 LeftUsage{0x50U};
constexpr UInt16 DownUsage{0x51U};
constexpr UInt16 UpUsage{0x52U};
constexpr UInt16 KeypadEnterUsage{0x58U};
/// F5-F12's numbers in ESC [ n ~.
constexpr UInt8 FunctionNumbers[8U]{15U, 17U, 18U, 19U, 20U, 21U, 23U, 24U};

/// What a key sends without Alt, into sequence; returns the count.
[[nodiscard]] Size Encode(const UInt16 usage, const UInt8 modifiers, const bool applicationCursorKeys,
                          UInt8 (&sequence)[TerminalKeyMaximumBytes], const Input::KeyboardLayout layout) noexcept
{
    Size count{0U};
    const Input::KeyCharacter character{Input::LayoutKeyCharacter(layout, usage, modifiers)};
    if (character.Present)
    {
        return Text::EncodeUtf8(character.CodePoint, sequence);
    }
    switch (usage)
    {
    case EnterUsage:
    case KeypadEnterUsage:
        sequence[count++] = '\r';
        return count;
    case EscapeUsage:
        sequence[count++] = Escape;
        return count;
    case BackspaceUsage:
        sequence[count++] = 0x7FU;
        return count;
    case TabUsage:
        sequence[count++] = '\t';
        return count;
    case UpUsage:
    case DownUsage:
    case RightUsage:
    case LeftUsage:
    {
        constexpr char Finals[4U]{'C', 'D', 'B', 'A'};
        sequence[count++] = Escape;
        sequence[count++] = applicationCursorKeys ? UInt8{'O'} : UInt8{'['};
        sequence[count++] = static_cast<UInt8>(Finals[usage - RightUsage]);
        return count;
    }
    case HomeUsage:
    case InsertUsage:
    case DeleteUsage:
    case EndUsage:
    case PageUpUsage:
    case PageDownUsage:
    {
        UInt8 number{'1'};
        if (usage == InsertUsage)
        {
            number = '2';
        }
        else if (usage == DeleteUsage)
        {
            number = '3';
        }
        else if (usage == EndUsage)
        {
            number = '4';
        }
        else if (usage == PageUpUsage)
        {
            number = '5';
        }
        else if (usage == PageDownUsage)
        {
            number = '6';
        }
        sequence[count++] = Escape;
        sequence[count++] = '[';
        sequence[count++] = number;
        sequence[count++] = '~';
        return count;
    }
    default:
        break;
    }
    if ((usage >= F1Usage) && (usage <= F12Usage))
    {
        const Size index{static_cast<Size>(usage) - F1Usage};
        sequence[count++] = Escape;
        if (index < 4U)
        {
            sequence[count++] = 'O';
            sequence[count++] = static_cast<UInt8>('P' + index);
            return count;
        }
        const Size functionIndex{index - 4U};
        // Keep the table bound explicit after the HID subtraction,
        // including in the O0 analyzer build.
        if (functionIndex >= (sizeof(FunctionNumbers) / sizeof(FunctionNumbers[0U])))
        {
            return 0U;
        }
        const UInt8 number{FunctionNumbers[functionIndex]};
        sequence[count++] = '[';
        sequence[count++] = (number >= 20U) ? UInt8{'2'} : UInt8{'1'};
        sequence[count++] = static_cast<UInt8>('0' + ((number >= 20U) ? (number - 20U) : (number - 10U)));
        sequence[count++] = '~';
        return count;
    }
    return 0U;
}
} // namespace

Size EncodeTerminalKey(const UInt16 usage, const UInt8 modifiers, const bool applicationCursorKeys, UInt8* const out,
                       const Size capacity, const Input::KeyboardLayout layout) noexcept
{
    if ((out == nullptr) || ((modifiers & Input::KeymapGui) != 0U))
    {
        return 0U;
    }
    UInt8 sequence[TerminalKeyMaximumBytes]{};
    const Size count{Encode(usage, modifiers, applicationCursorKeys, sequence, layout)};
    if (count == 0U)
    {
        return 0U;
    }
    const bool alt{(modifiers & Input::KeymapAlt) != 0U};
    const Size total{count + (alt ? 1U : 0U)};
    if (total > capacity)
    {
        return 0U;
    }
    Size at{0U};
    if (alt)
    {
        out[at++] = Escape;
    }
    for (Size index{0U}; index < count; ++index)
    {
        out[at++] = sequence[index];
    }
    return total;
}
} // namespace Asteria::User::Terminal
