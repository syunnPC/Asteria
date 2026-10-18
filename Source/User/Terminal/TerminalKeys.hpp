// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TERMINAL_TERMINAL_KEYS_HPP_INCLUDED
#define ASTERIA_USER_TERMINAL_TERMINAL_KEYS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Input/Keymap.hpp"

namespace Asteria::User::Terminal
{
/// The most bytes one key produces (Alt and "ESC [ 2 4 ~" are six).
inline constexpr Foundation::Size TerminalKeyMaximumBytes{8U};

/// The bytes the terminal sends for a key press, from its HID keyboard-page (0x07) usage and the
/// modifier byte, written to out; returns their count, or zero when the key sends nothing or
/// capacity cannot hold them all (nothing is written then). - Characters are UTF-8 from the
/// caller's per-keyboard layout (User/Input/Keymap.hpp): letters, digits, space and punctuation
/// with Shift, the keypad's / * - +, and with Ctrl the control codes it lists (Ctrl+letter
/// 0x01-0x1A). - Enter (and keypad Enter) CR; Backspace DEL (0x7F); Tab HT; Escape ESC. - Up, Down,
/// Right, Left: ESC [ A, B, C, D, or ESC O A-D when application cursor keys (DEC mode 1) are on. -
/// Home, Insert, Delete, End, Page Up, Page Down: ESC [ 1 ~, 2 ~, 3 ~, 4 ~, 5 ~, 6 ~. F1-F4: ESC O
/// P-S. F5-F12: ESC [ 15 ~, 17 ~, 18 ~, 19 ~, 20 ~, 21 ~, 23 ~, 24 ~ (DEC's numbering, with its
/// gaps). - Ctrl does not change these non-character keys. - Alt sends ESC before what the key
/// sends otherwise. - A key with the GUI modifier sends nothing (no Super shortcut); modifier keys,
/// locks and every other key send nothing.
[[nodiscard]] Foundation::Size EncodeTerminalKey(Foundation::UInt16 usage, Foundation::UInt8 modifiers,
                                                 bool applicationCursorKeys, Foundation::UInt8* out,
                                                 Foundation::Size capacity,
                                                 Input::KeyboardLayout layout = Input::KeyboardLayout::Us) noexcept;
} // Namespace Asteria::User::Terminal

#endif // ASTERIA_USER_TERMINAL_TERMINAL_KEYS_HPP_INCLUDED
