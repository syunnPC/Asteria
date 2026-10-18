// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_PALETTE_HPP_INCLUDED
#define ASTERIA_USER_SESSION_PALETTE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/PixelFormat.hpp"

namespace Asteria::User::Session
{
/// Desktop colours, shared with the runner's Session.py and Terminal.py models.
/// Their parsers require the `{.Red = 0x..U,.Green = 0x..U,.Blue = 0x..U}` form.
/// The Classic theme uses grey bevels and dark-blue focused title bars.
/// The desktop colour differs from the trusted console's 0x102040.

/// The uncovered desktop.
inline constexpr Graphics::Color Desktop{.Red = 0x30U, .Green = 0x68U, .Blue = 0x68U};
/// Frame rings 2-3, the separator, buttons, the switcher and content the
/// client's image does not reach.
inline constexpr Graphics::Color Face{.Red = 0xC6U, .Green = 0xC6U, .Blue = 0xC6U};
/// A button's ring 0 top and left; the frame's ring 1 top and left.
inline constexpr Graphics::Color Highlight{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
/// The frame's ring 0 top and left; a button's ring 1 top and left.
inline constexpr Graphics::Color Light{.Red = 0xE2U, .Green = 0xE2U, .Blue = 0xE2U};
/// Ring 1 bottom and right.
inline constexpr Graphics::Color Shadow{.Red = 0x86U, .Green = 0x86U, .Blue = 0x86U};
/// Ring 0 bottom and right, and the outline of an outline move.
inline constexpr Graphics::Color DarkShadow{.Red = 0x26U, .Green = 0x26U, .Blue = 0x26U};
/// The focused window's title bar, its trusted text and its subtitle.
inline constexpr Graphics::Color ActiveTitle{.Red = 0x16U, .Green = 0x3AU, .Blue = 0x86U};
inline constexpr Graphics::Color ActiveTitleText{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
inline constexpr Graphics::Color ActiveSubtitle{.Red = 0xB8U, .Green = 0xC8U, .Blue = 0xE8U};
/// Every other window's title bar, trusted text and subtitle.
inline constexpr Graphics::Color InactiveTitle{.Red = 0x8AU, .Green = 0x8AU, .Blue = 0x8AU};
inline constexpr Graphics::Color InactiveTitleText{.Red = 0xD8U, .Green = 0xD8U, .Blue = 0xD8U};
inline constexpr Graphics::Color InactiveSubtitle{.Red = 0xB4U, .Green = 0xB4U, .Blue = 0xB4U};
/// A window wanting attention (raised unfocused), drawn with the
/// active title's text colours.
inline constexpr Graphics::Color AttentionTitle{.Red = 0x8AU, .Green = 0x5AU, .Blue = 0x1EU};
/// Button glyphs and the switcher's text.
inline constexpr Graphics::Color Glyph{.Red = 0x10U, .Green = 0x10U, .Blue = 0x10U};

/// The terminal's sixteen SGR colours: 0-7 for SGR 30-37 and
/// 40-47 (black, red, green, yellow, blue, magenta, cyan, white), 8-15 their
/// bright forms for SGR 90-97 and 100-107. SGR 1 shows a normal foreground
/// colour as its bright one.
inline constexpr Graphics::Color TerminalColor0{.Red = 0x00U, .Green = 0x00U, .Blue = 0x00U};
inline constexpr Graphics::Color TerminalColor1{.Red = 0xB0U, .Green = 0x18U, .Blue = 0x18U};
inline constexpr Graphics::Color TerminalColor2{.Red = 0x18U, .Green = 0xA0U, .Blue = 0x18U};
inline constexpr Graphics::Color TerminalColor3{.Red = 0xA8U, .Green = 0x78U, .Blue = 0x18U};
inline constexpr Graphics::Color TerminalColor4{.Red = 0x18U, .Green = 0x30U, .Blue = 0xB0U};
inline constexpr Graphics::Color TerminalColor5{.Red = 0xA0U, .Green = 0x18U, .Blue = 0xA0U};
inline constexpr Graphics::Color TerminalColor6{.Red = 0x18U, .Green = 0x98U, .Blue = 0xA0U};
inline constexpr Graphics::Color TerminalColor7{.Red = 0xBEU, .Green = 0xBEU, .Blue = 0xBEU};
inline constexpr Graphics::Color TerminalColor8{.Red = 0x60U, .Green = 0x60U, .Blue = 0x60U};
inline constexpr Graphics::Color TerminalColor9{.Red = 0xF0U, .Green = 0x48U, .Blue = 0x48U};
inline constexpr Graphics::Color TerminalColor10{.Red = 0x48U, .Green = 0xE0U, .Blue = 0x48U};
inline constexpr Graphics::Color TerminalColor11{.Red = 0xF0U, .Green = 0xE0U, .Blue = 0x48U};
inline constexpr Graphics::Color TerminalColor12{.Red = 0x50U, .Green = 0x70U, .Blue = 0xF0U};
inline constexpr Graphics::Color TerminalColor13{.Red = 0xE0U, .Green = 0x58U, .Blue = 0xE0U};
inline constexpr Graphics::Color TerminalColor14{.Red = 0x48U, .Green = 0xE0U, .Blue = 0xE8U};
inline constexpr Graphics::Color TerminalColor15{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
inline constexpr Foundation::Size TerminalColorCount{16U};
inline constexpr Graphics::Color TerminalColors[TerminalColorCount]{
    TerminalColor0,  TerminalColor1,  TerminalColor2,  TerminalColor3,  TerminalColor4,  TerminalColor5,
    TerminalColor6,  TerminalColor7,  TerminalColor8,  TerminalColor9,  TerminalColor10, TerminalColor11,
    TerminalColor12, TerminalColor13, TerminalColor14, TerminalColor15,
};
/// The terminal's default pair (SGR 39 and 49), the console's white on
/// 0x102040 (TextConsole.hpp), so the terminal reads like the console; a
/// host test checks the two headers agree. Bold leaves the default
/// foreground as it is.
inline constexpr Graphics::Color TerminalDefaultForeground{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
inline constexpr Graphics::Color TerminalDefaultBackground{.Red = 0x10U, .Green = 0x20U, .Blue = 0x40U};
} // Namespace Asteria::User::Session

#endif // ASTERIA_USER_SESSION_PALETTE_HPP_INCLUDED
