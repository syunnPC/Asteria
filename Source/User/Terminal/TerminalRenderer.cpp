// SPDX-License-Identifier: Apache-2.0

#include "User/Terminal/TerminalRenderer.hpp"

#include "User/Graphics/BitmapFont.hpp"
#include "User/Terminal/Cp437.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/Session/Palette.hpp"

namespace Asteria::User::Terminal
{
namespace
{
using Foundation::Int32;
using Foundation::Int64;
using Foundation::UInt32;

void RenderRow(const TerminalScreen& screen, const Graphics::MemorySurface& target, const Graphics::Point origin,
               const UInt32 row) noexcept
{
    const Int64 top{static_cast<Int64>(origin.Y) + (static_cast<Int64>(row) * TerminalCellHeight)};
    if ((top >= static_cast<Int64>(target.Height)) || ((top + TerminalCellHeight) <= 0))
    {
        return;
    }
    const bool cursorRow{screen.CursorVisible() && (screen.CursorRow() == row)};
    for (UInt32 column{0U}; column < screen.Columns(); ++column)
    {
        const Int64 left{static_cast<Int64>(origin.X) + (static_cast<Int64>(column) * TerminalCellWidth)};
        if (left >= static_cast<Int64>(target.Width))
        {
            break;
        }
        if ((left + TerminalCellWidth) <= 0)
        {
            continue;
        }
        const TerminalCellView cell{screen.CellAt(row, column)};
        const TerminalCellColors colors{ResolveCellColors(cell, cursorRow && (screen.CursorColumn() == column))};
        const Int32 x{static_cast<Int32>(left)};
        const Int32 y{static_cast<Int32>(top)};
        Graphics::DrawGlyph(target, x, y, Graphics::Bold8x16, MapToCp437(cell.CodePoint), colors.Foreground,
                            colors.Background);
        if ((cell.Attributes & TerminalUnderline) != 0U)
        {
            Graphics::FillRect(target,
                               Graphics::Rect{.X = x,
                                              .Y = y + static_cast<Int32>(TerminalUnderlineRow),
                                              .Width = TerminalCellWidth,
                                              .Height = 1U},
                               colors.Foreground);
        }
    }
}
} // namespace

TerminalCellColors ResolveCellColors(const TerminalCellView cell, const bool cursor) noexcept
{
    UInt32 foreground{cell.Foreground};
    if (((cell.Attributes & TerminalBold) != 0U) && (foreground < 8U))
    {
        foreground += 8U;
    }
    TerminalCellColors colors{
        .Foreground = (foreground < Session::TerminalColorCount) ? Session::TerminalColors[foreground]
                                                                 : Session::TerminalDefaultForeground,
        .Background = (cell.Background < Session::TerminalColorCount) ? Session::TerminalColors[cell.Background]
                                                                      : Session::TerminalDefaultBackground,
    };
    const bool swap{((cell.Attributes & TerminalReverse) != 0U) != cursor};
    if (swap)
    {
        const Graphics::Color first{colors.Foreground};
        colors.Foreground = colors.Background;
        colors.Background = first;
    }
    return colors;
}

void RenderTerminal(const TerminalScreen& screen, const Graphics::MemorySurface& target,
                    const Graphics::Point origin) noexcept
{
    if (!Graphics::IsUsable(target))
    {
        return;
    }
    for (UInt32 row{0U}; row < screen.Rows(); ++row)
    {
        RenderRow(screen, target, origin, row);
    }
}

void RenderTerminalDamage(const TerminalScreen& screen, const Graphics::MemorySurface& target,
                          const Graphics::Point origin) noexcept
{
    if (!Graphics::IsUsable(target))
    {
        return;
    }
    for (UInt32 row{0U}; row < screen.Rows(); ++row)
    {
        if (screen.IsRowDamaged(row))
        {
            RenderRow(screen, target, origin, row);
        }
    }
}
} // namespace Asteria::User::Terminal
