// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TERMINAL_TERMINAL_RENDERER_HPP_INCLUDED
#define ASTERIA_USER_TERMINAL_TERMINAL_RENDERER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Session/Decorations.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"
#include "User/Terminal/TerminalScreen.hpp"

namespace Asteria::User::Terminal
{
/// A cell's size in pixels: one Bold8x16 glyph.
inline constexpr Foundation::UInt32 TerminalCellWidth{8U};
inline constexpr Foundation::UInt32 TerminalCellHeight{16U};
/// The glyph row an underlined cell paints in its foreground colour (the
/// row of the font's underscore).
inline constexpr Foundation::UInt32 TerminalUnderlineRow{13U};

/// The window a terminal's grid sits in: its frame is the content plus these many pixels across and
/// down (two 4-pixel sides; 4 above, the 20-pixel title bar and the 1-pixel separator).
inline constexpr Foundation::UInt32 TerminalFrameExtraWidth{8U};
inline constexpr Foundation::UInt32 TerminalFrameExtraHeight{29U};
static_assert(TerminalFrameExtraWidth == Session::FrameExtraWidth);
static_assert(TerminalFrameExtraHeight == Session::FrameExtraHeight);

/// A grid size in cells.
struct TerminalGrid
{
    Foundation::UInt32 Columns;
    Foundation::UInt32 Rows;
};

/// A new terminal window's grid: 80x25, or the largest grid whose window frame fits an output of
/// the given size, at least 1x1. At 640x400 that is 79x23; from 648x429 on it is 80x25.
[[nodiscard]] constexpr TerminalGrid TerminalGridForOutput(const Foundation::UInt32 outputWidth,
                                                           const Foundation::UInt32 outputHeight) noexcept
{
    const Foundation::UInt32 width{(outputWidth > TerminalFrameExtraWidth) ? (outputWidth - TerminalFrameExtraWidth)
                                                                           : 0U};
    const Foundation::UInt32 height{
        (outputHeight > TerminalFrameExtraHeight) ? (outputHeight - TerminalFrameExtraHeight) : 0U};
    const Foundation::UInt32 columns{width >> 3U};
    const Foundation::UInt32 rows{height >> 4U};
    return TerminalGrid{
        .Columns = (columns == 0U) ? 1U : ((columns < TerminalDefaultColumns) ? columns : TerminalDefaultColumns),
        .Rows = (rows == 0U) ? 1U : ((rows < TerminalDefaultRows) ? rows : TerminalDefaultRows),
    };
}
static_assert((TerminalCellWidth == 8U) && (TerminalCellHeight == 16U));

/// The colours a cell is drawn with.
struct TerminalCellColors
{
    Graphics::Color Foreground;
    Graphics::Color Background;
};

/// A cell's final colours: a palette index 0-15 is Session/Palette.hpp's TerminalColors entry, any
/// other value the default pair (TerminalDefaultForeground and TerminalDefaultBackground, the
/// console's white on 0x102040); bold shows a normal foreground 0-7 as its bright counterpart
/// (index + 8) and leaves the default foreground as it is; reverse swaps the two; the cursor, a
/// steady block, swaps them once more.
[[nodiscard]] TerminalCellColors ResolveCellColors(TerminalCellView cell, bool cursor) noexcept;

/// Draws every cell of the screen with cell (row, column)'s top-left pixel at
/// origin + (column * TerminalCellWidth, row * TerminalCellHeight), clipped
/// to the target, and the cursor when it is visible. Reads the screen only.
void RenderTerminal(const TerminalScreen& screen, const Graphics::MemorySurface& target,
                    Graphics::Point origin) noexcept;

/// Draws only the rows TerminalScreen::IsRowDamaged reports; the caller then clears the damage
/// (Terminal::ClearDamage).
void RenderTerminalDamage(const TerminalScreen& screen, const Graphics::MemorySurface& target,
                          Graphics::Point origin) noexcept;
} // Namespace Asteria::User::Terminal

#endif // ASTERIA_USER_TERMINAL_TERMINAL_RENDERER_HPP_INCLUDED
