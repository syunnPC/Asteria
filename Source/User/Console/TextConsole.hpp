// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_CONSOLE_TEXT_CONSOLE_HPP_INCLUDED
#define ASTERIA_USER_CONSOLE_TEXT_CONSOLE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/BitmapFont.hpp"
#include "User/Graphics/Composition/Divide.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"

namespace Asteria::User::Console
{
/// The console's colors, in one place so a runner can read them by regex.
inline constexpr Graphics::Color ConsoleForeground{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
inline constexpr Graphics::Color ConsoleBackground{.Red = 0x10U, .Green = 0x20U, .Blue = 0x40U};

#if defined(__ia64__)
/// Only `display` carries the grid. The unchanged 227-page grant permits 12,288 cells; full
/// 65,536-cell parity waits for room.
inline constexpr Foundation::UInt32 ConsoleMaximumColumns{640U};
inline constexpr Foundation::UInt32 ConsoleMaximumRows{256U};
inline constexpr Foundation::UInt32 ConsoleMaximumCells{12'288U};
#else
/// The largest grid elsewhere: up to 640 columns and 256 rows within 65,536
/// one-byte cells, so a 3840x2160 (480 x 135 cells) or 5120x1440 (640 x 90)
/// screen is filled entirely; a larger screen keeps a margin.
inline constexpr Foundation::UInt32 ConsoleMaximumColumns{640U};
inline constexpr Foundation::UInt32 ConsoleMaximumRows{256U};
inline constexpr Foundation::UInt32 ConsoleMaximumCells{65'536U};
#endif
static_assert(ConsoleMaximumCells <= ConsoleMaximumColumns * ConsoleMaximumRows);

/// One character cell. Code is an IBM code page 437 code (the font's encoding);
/// Attribute is stored but unused in v0.
struct ConsoleCell
{
    Foundation::UInt8 Code;
    Foundation::UInt8 Attribute;
};

/// The row range a console has changed since its last Render: rows First up to
/// but excluding End; empty when First == End.
struct ConsoleDamage
{
    Foundation::UInt32 First;
    Foundation::UInt32 End;
};

/// A text console model over a cell grid it owns (ConsoleMaximumCells bytes, laid out row after row
/// at the configured width), meant to live in static storage: fixed arrays, no heap, no globals,
/// small locals. All zero bytes is the state before Configure, in which Write and Render do
/// nothing. It never reads pixels: scrolling shifts the cell grid and marks every row damaged, and
/// Render redraws only damaged rows by DrawGlyph. Write interprets '\n' as a new line (column 0 of
/// the next row, scrolling at the last row), '\r' as column 0, '\t' as the next multiple of 8
/// columns (clamped to the line end, no erase), '\b' as one column back (no erase), and every other
/// byte as a CP437 code placed at the cursor. The cursor wraps when a code is placed after the last
/// column of a row (a deferred wrap: Column equals Columns after the last cell is filled, and the
/// line break happens when the next code arrives). A row entered by such a wrap continues the row
/// above it: '\b' at its column 0 moves to the last column of that row, so a line editor's "\b \b"
/// erases across the wrap. '\b' never crosses a '\n' break or the top row; scrolling carries each
/// row's continuation mark with it, and the top row loses the row it continued. The cursor is not
/// drawn.
class TextConsole final
{
public:
    /// Sets the grid to the surface size in cells of the font, clamped to the
    /// maximum columns, then to the maximum rows and the rows the cell budget
    /// holds at that width; clears it, homes the cursor and damages every row. A surface
    /// smaller than one cell leaves an inert console (zero columns or rows).
    void Configure(const Foundation::UInt32 surfaceWidth, const Foundation::UInt32 surfaceHeight,
                   const Graphics::BitmapFont& font) noexcept
    {
        m_font = font;
        m_columns = 0U;
        m_rows = 0U;
        if ((font.GlyphWidth != 0U) && (font.GlyphHeight != 0U))
        {
            const Foundation::UInt32 columns{Graphics::Composition::Divide(surfaceWidth, font.GlyphWidth)};
            const Foundation::UInt32 rows{Graphics::Composition::Divide(surfaceHeight, font.GlyphHeight)};
            if ((columns != 0U) && (rows != 0U))
            {
                m_columns = columns < ConsoleMaximumColumns ? columns : ConsoleMaximumColumns;
                const Foundation::UInt32 budget{Graphics::Composition::Divide(ConsoleMaximumCells, m_columns)};
                const Foundation::UInt32 limit{budget < ConsoleMaximumRows ? budget : ConsoleMaximumRows};
                m_rows = rows < limit ? rows : limit;
            }
        }
        Clear();
    }

    /// Blanks every cell, homes the cursor and damages every row.
    void Clear() noexcept
    {
        for (Foundation::UInt32 cell{0U}; cell < ConsoleMaximumCells; ++cell)
        {
            m_cells[cell] = BlankCode;
        }
        for (Foundation::UInt64& word : m_continues)
        {
            word = 0U;
        }
        m_cursorColumn = 0U;
        m_cursorRow = 0U;
        DamageAll();
    }

    void Write(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
    {
        if ((m_columns == 0U) || (bytes == nullptr))
        {
            return;
        }
        for (Foundation::Size index{0U}; index < count; ++index)
        {
            Put(bytes[index]);
        }
    }

    /// Draws the cells of the damaged rows with the cell grid's top-left pixel
    /// at origin, then clears the damage. An unusable surface changes nothing.
    void Render(const Graphics::Surface& surface, const Graphics::Point origin) noexcept
    {
        if ((m_columns == 0U) || !Graphics::IsUsable(surface))
        {
            return;
        }
        for (Foundation::UInt32 row{m_damageFirst}; row < m_damageEnd; ++row)
        {
            const Foundation::Int32 y{origin.Y + static_cast<Foundation::Int32>(row * m_font.GlyphHeight)};
            for (Foundation::UInt32 column{0U}; column < m_columns; ++column)
            {
                const Foundation::Int32 x{origin.X + static_cast<Foundation::Int32>(column * m_font.GlyphWidth)};
                Graphics::DrawGlyph(surface, x, y, m_font, m_cells[Index(row, column)], ConsoleForeground,
                                    ConsoleBackground);
            }
        }
        m_damageFirst = 0U;
        m_damageEnd = 0U;
    }

    /// Renders a physical rectangle into a tile, including the margin outside
    /// the grid. origin is the output's logical desktop origin; scale is positive.
    /// Does not consume damage. Reads cells and font only, never the scanout.
    void RenderRegion(const Graphics::MemorySurface& target, const Graphics::Rect rectangle,
                      const Graphics::Point origin, const Foundation::UInt32 scale = 1U) const noexcept
    {
        if (!Graphics::IsUsable(target) || (scale == 0U))
        {
            return;
        }
        const Foundation::UInt32 foreground = Graphics::PackColor(target.Format, ConsoleForeground);
        const Foundation::UInt32 background = Graphics::PackColor(target.Format, ConsoleBackground);
        const bool validFont = (m_font.Data != nullptr) && (m_font.GlyphWidth != 0U) && (m_font.GlyphWidth <= 8U) &&
                               (m_font.GlyphHeight != 0U);
        const Foundation::Int64 gridWidth = static_cast<Foundation::Int64>(m_columns) * m_font.GlyphWidth;
        const Foundation::Int64 gridHeight = static_cast<Foundation::Int64>(m_rows) * m_font.GlyphHeight;
        const Foundation::UInt32 firstPhysicalX = static_cast<Foundation::UInt32>(rectangle.X);
        const Foundation::UInt32 firstLogicalX = Graphics::Composition::Divide(firstPhysicalX, scale);
        for (Foundation::UInt32 y{0U}; y < target.Height; ++y)
        {
            Foundation::UInt32* const line = target.Pixels + static_cast<Foundation::Size>(y) * target.Stride;
            const Foundation::Int64 gridY = origin.Y + static_cast<Foundation::Int64>(Graphics::Composition::Divide(
                                                           static_cast<Foundation::UInt32>(rectangle.Y) + y, scale));
            if (!validFont || (gridY < 0) || (gridY >= gridHeight))
            {
                for (Foundation::UInt32 x{0U}; x < target.Width; ++x)
                {
                    line[x] = background;
                }
                continue;
            }
            const Foundation::UInt32 pixelY = static_cast<Foundation::UInt32>(gridY);
            const Foundation::UInt32 row = Graphics::Composition::Divide(pixelY, m_font.GlyphHeight);
            const Foundation::UInt32 glyphY = pixelY - row * m_font.GlyphHeight;
            Foundation::Int64 gridX = origin.X + static_cast<Foundation::Int64>(firstLogicalX);
            Foundation::UInt32 repeat = scale - (firstPhysicalX - firstLogicalX * scale);
            Foundation::UInt32 x{0U};
            while (x < target.Width)
            {
                // Resolve bounds, the cell and its bitmap once per glyph run.
                Foundation::UInt32 bits{0U};
                Foundation::UInt32 glyphX{0U};
                Foundation::UInt32 logicalRun{1U};
                if ((gridX >= 0) && (gridX < gridWidth))
                {
                    const Foundation::UInt32 pixelX = static_cast<Foundation::UInt32>(gridX);
                    const Foundation::UInt32 column = Graphics::Composition::Divide(pixelX, m_font.GlyphWidth);
                    glyphX = pixelX - column * m_font.GlyphWidth;
                    logicalRun = m_font.GlyphWidth - glyphX;
                    bits = GlyphRow(row, column, glyphY);
                }
                for (Foundation::UInt32 logical{0U}; (logical < logicalRun) && (x < target.Width); ++logical)
                {
                    const Foundation::UInt32 word = (bits & (0x80U >> glyphX)) != 0U ? foreground : background;
                    const Foundation::UInt32 left = target.Width - x;
                    const Foundation::UInt32 run = repeat < left ? repeat : left;
                    const Foundation::UInt32 end = x + run;
                    while (x < end)
                    {
                        line[x++] = word;
                    }
                    ++glyphX;
                    ++gridX;
                    repeat = scale;
                }
            }
        }
    }

    [[nodiscard]] Foundation::UInt32 Columns() const noexcept
    {
        return m_columns;
    }

    [[nodiscard]] Foundation::UInt32 Rows() const noexcept
    {
        return m_rows;
    }

    /// The cursor column, in [0, Columns]; Columns is the deferred wrap.
    [[nodiscard]] Foundation::UInt32 Column() const noexcept
    {
        return m_cursorColumn;
    }

    [[nodiscard]] Foundation::UInt32 Row() const noexcept
    {
        return m_cursorRow;
    }

    /// A cell of the grid; a blank (space, attribute zero) cell outside it.
    [[nodiscard]] ConsoleCell CellAt(const Foundation::UInt32 row, const Foundation::UInt32 column) const noexcept
    {
        if ((row >= m_rows) || (column >= m_columns))
        {
            return ConsoleCell{.Code = BlankCode, .Attribute = 0U};
        }
        return ConsoleCell{.Code = m_cells[Index(row, column)], .Attribute = 0U};
    }

    /// Forgets the damage of rows the caller already drew: a blank grid over a
    /// surface filled with ConsoleBackground looks as Render would draw it.
    void MarkDrawn() noexcept
    {
        m_damageFirst = 0U;
        m_damageEnd = 0U;
    }

    [[nodiscard]] ConsoleDamage Damage() const noexcept
    {
        return ConsoleDamage{.First = m_damageFirst, .End = m_damageEnd};
    }

    [[nodiscard]] bool HasDamage() const noexcept
    {
        return m_damageEnd > m_damageFirst;
    }

private:
    /// Caller checked grid/font bounds once for the row and glyph run.
    [[nodiscard]] Foundation::UInt32 GlyphRow(const Foundation::UInt32 row, const Foundation::UInt32 column,
                                              const Foundation::UInt32 glyphY) const noexcept
    {
        const Foundation::UInt32 code = m_cells[Index(row, column)];
        if ((code < m_font.FirstCode) || (code - m_font.FirstCode >= m_font.Count))
        {
            return 0U;
        }
        return m_font.Data[static_cast<Foundation::Size>(code - m_font.FirstCode) * m_font.GlyphHeight + glyphY];
    }

    static constexpr Foundation::UInt8 BlankCode{0x20U};
    static constexpr Foundation::UInt32 TabWidth{8U};
    static constexpr Foundation::UInt32 ContinuationWords{(ConsoleMaximumRows + 63U) / 64U};

    /// The cell of a row and column inside the configured grid.
    [[nodiscard]] Foundation::Size Index(const Foundation::UInt32 row, const Foundation::UInt32 column) const noexcept
    {
        return static_cast<Foundation::Size>(row) * m_columns + column;
    }

    void ClearRow(const Foundation::UInt32 row) noexcept
    {
        for (Foundation::UInt32 column{0U}; column < m_columns; ++column)
        {
            m_cells[Index(row, column)] = BlankCode;
        }
    }

    [[nodiscard]] bool Continues(const Foundation::UInt32 row) const noexcept
    {
        return ((m_continues[row / 64U] >> (row % 64U)) & 1U) != 0U;
    }

    void MarkContinues(const Foundation::UInt32 row, const bool continues) noexcept
    {
        const Foundation::UInt64 bit{Foundation::UInt64{1U} << (row % 64U)};
        m_continues[row / 64U] = continues ? (m_continues[row / 64U] | bit) : (m_continues[row / 64U] & ~bit);
    }

    void DamageRow(const Foundation::UInt32 row) noexcept
    {
        if (m_damageEnd == m_damageFirst)
        {
            m_damageFirst = row;
            m_damageEnd = row + 1U;
            return;
        }
        if (row < m_damageFirst)
        {
            m_damageFirst = row;
        }
        if (row >= m_damageEnd)
        {
            m_damageEnd = row + 1U;
        }
    }

    void DamageAll() noexcept
    {
        m_damageFirst = 0U;
        m_damageEnd = m_rows;
    }

    /// Shifts the grid up one row, blanks the last row, damages every row.
    void Scroll() noexcept
    {
        const Foundation::Size cells{static_cast<Foundation::Size>(m_rows) * m_columns};
        for (Foundation::Size cell{m_columns}; cell < cells; ++cell)
        {
            m_cells[cell - m_columns] = m_cells[cell];
        }
        // Each mark moves up with its row. The old top row is gone, so the new
        // top row continues nothing.
        for (Foundation::UInt32 word{0U}; word < ContinuationWords; ++word)
        {
            const Foundation::UInt64 carry{(word + 1U) < ContinuationWords ? (m_continues[word + 1U] << 63U) : 0U};
            m_continues[word] = (m_continues[word] >> 1U) | carry;
        }
        m_continues[0U] &= ~Foundation::UInt64{1U};
        ClearRow(m_rows - 1U);
        DamageAll();
    }

    /// Column 0 of the next row, scrolling at the last row; wrapped marks the
    /// new row as the continuation of the one the cursor leaves.
    void NewLine(const bool wrapped) noexcept
    {
        m_cursorColumn = 0U;
        if ((m_cursorRow + 1U) < m_rows)
        {
            ++m_cursorRow;
        }
        else
        {
            Scroll();
        }
        MarkContinues(m_cursorRow, wrapped && (m_rows > 1U));
    }

    void Put(const Foundation::UInt8 code) noexcept
    {
        switch (code)
        {
        case '\n':
            NewLine(false);
            break;
        case '\r':
            m_cursorColumn = 0U;
            break;
        case '\t':
        {
            const Foundation::UInt32 next{((m_cursorColumn / TabWidth) + 1U) * TabWidth};
            m_cursorColumn = next < m_columns ? next : m_columns;
            break;
        }
        case '\b':
            if (m_cursorColumn != 0U)
            {
                --m_cursorColumn;
            }
            else if ((m_cursorRow != 0U) && Continues(m_cursorRow))
            {
                --m_cursorRow;
                m_cursorColumn = m_columns - 1U;
            }
            break;
        default:
            if (m_cursorColumn >= m_columns)
            {
                NewLine(true);
            }
            m_cells[Index(m_cursorRow, m_cursorColumn)] = code;
            DamageRow(m_cursorRow);
            ++m_cursorColumn;
            break;
        }
    }

    // Attributes are zero in the public v0 cell contract; storing a second
    // zero byte per cell would consume two extra pages in every incarnation.
    Foundation::UInt8 m_cells[ConsoleMaximumCells]{};
    /// Bit r: row r was entered by a deferred wrap from the row above.
    Foundation::UInt64 m_continues[ContinuationWords]{};
    Graphics::BitmapFont m_font{};
    Foundation::UInt32 m_columns{0U};
    Foundation::UInt32 m_rows{0U};
    Foundation::UInt32 m_cursorColumn{0U};
    Foundation::UInt32 m_cursorRow{0U};
    Foundation::UInt32 m_damageFirst{0U};
    Foundation::UInt32 m_damageEnd{0U};
};
} // Namespace Asteria::User::Console

#endif
