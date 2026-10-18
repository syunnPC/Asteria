// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_KEYMAP_HPP_INCLUDED
#define ASTERIA_USER_INPUT_KEYMAP_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Input
{
/// The HID modifier byte's bits, left and right together, as InputEvent::Modifiers carries them.
inline constexpr Foundation::UInt8 KeymapControl{0x11U};
inline constexpr Foundation::UInt8 KeymapShift{0x22U};
inline constexpr Foundation::UInt8 KeymapAlt{0x44U};
inline constexpr Foundation::UInt8 KeymapGui{0x88U};

/// The character a key types, when it types one.
struct KeyCharacter
{
    bool Present;
    Foundation::UInt32 CodePoint;
};

/// Caller-owned, per-device layout selection. An unknown id produces no character.
enum class KeyboardLayout : Foundation::UInt8
{
    Us = 0U,
    Jis = 1U
};

/// Unicode from keyboard-page usages and modifiers. USB HID Usage Tables
/// v1.5 (2024-01-26), section 10, table 10.1 identifies the physical keys;
/// Maps US and Japanese 106/109-key layouts.
/// Letters, digits and punctuation use Shift; Caps Lock and kana conversion
/// are caller policy. Keypad operators always type; Ctrl maps letters to
/// 1-26, [, backslash/yen and ] to ESC/FS/GS, Space to NUL, JIS ro to US.
/// Alt and GUI do not change text; no shortcut is assigned to GUI/Super.
[[nodiscard]] KeyCharacter LayoutKeyCharacter(KeyboardLayout layout, Foundation::UInt16 usage,
                                              Foundation::UInt8 modifiers) noexcept;
/// Explicit US convenience, with no mutable global keyboard layout.
[[nodiscard]] KeyCharacter UsKeyCharacter(Foundation::UInt16 usage, Foundation::UInt8 modifiers) noexcept;
} // Namespace Asteria::User::Input

#endif // ASTERIA_USER_INPUT_KEYMAP_HPP_INCLUDED
