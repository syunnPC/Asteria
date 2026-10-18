// SPDX-License-Identifier: Apache-2.0

#include "User/Terminal/TerminalScreen.hpp"

namespace Asteria::User::Terminal
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;

constexpr TerminalRendition DefaultRendition{
    .Attributes = 0U, .Foreground = TerminalDefaultColor, .Background = TerminalDefaultColor};

[[nodiscard]] constexpr UInt32 Clamp(const UInt32 value, const UInt32 minimum, const UInt32 maximum) noexcept
{
    if (value < minimum)
    {
        return minimum;
    }
    return (value > maximum) ? maximum : value;
}

[[nodiscard]] constexpr UInt32 Minimum(const UInt32 first, const UInt32 second) noexcept
{
    return (first < second) ? first : second;
}
} // namespace

void TerminalScreen::Configure(const UInt32 columns, const UInt32 rows) noexcept
{
    m_columns = Clamp(columns, 1U, TerminalMaximumColumns);
    m_rows = Clamp(rows, 1U, TerminalMaximumRows);
    Reset();
}

void TerminalScreen::Reset() noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    m_rendition = DefaultRendition;
    m_savedRendition = DefaultRendition;
    for (UInt32 row{0U}; row < m_rows; ++row)
    {
        BlankCells(row, 0U, m_columns);
    }
    m_row = 0U;
    m_column = 0U;
    m_savedRow = 0U;
    m_savedColumn = 0U;
    m_top = 0U;
    m_bottom = m_rows - 1U;
    m_wrapPending = false;
    m_autowrap = true;
    m_cursorVisible = true;
    m_applicationCursorKeys = false;
    DamageRows(0U, m_rows - 1U);
}

void TerminalScreen::Resize(const UInt32 columns, const UInt32 rows) noexcept
{
    if (m_columns == 0U)
    {
        Configure(columns, rows);
        return;
    }
    const UInt32 newColumns{Clamp(columns, 1U, TerminalMaximumColumns)};
    const UInt32 newRows{Clamp(rows, 1U, TerminalMaximumRows)};
    // The grid's stride is the maximum width, so kept cells stay in place;
    // only the cells that become visible are blanked, in the default colours.
    const TerminalCellView blank{.CodePoint = TerminalBlankCode,
                                 .Attributes = 0U,
                                 .Foreground = TerminalDefaultColor,
                                 .Background = TerminalDefaultColor};
    for (UInt32 row{0U}; row < newRows; ++row)
    {
        const UInt32 first{(row < m_rows) ? m_columns : 0U};
        for (UInt32 column{first}; column < newColumns; ++column)
        {
            StoreCell(Index(row, column), blank);
        }
    }
    m_columns = newColumns;
    m_rows = newRows;
    m_row = Minimum(m_row, m_rows - 1U);
    m_column = Minimum(m_column, m_columns - 1U);
    m_savedRow = Minimum(m_savedRow, m_rows - 1U);
    m_savedColumn = Minimum(m_savedColumn, m_columns - 1U);
    m_top = 0U;
    m_bottom = m_rows - 1U;
    m_wrapPending = false;
    DamageRows(0U, m_rows - 1U);
}

void TerminalScreen::Print(const UInt32 codePoint) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    if (m_wrapPending)
    {
        m_wrapPending = false;
        if (m_autowrap)
        {
            m_column = 0U;
            LineFeed();
        }
    }
    StoreCell(Index(m_row, m_column),
              TerminalCellView{.CodePoint = Text::IsScalar(codePoint) ? codePoint : Text::ReplacementCodePoint,
                               .Attributes = m_rendition.Attributes,
                               .Foreground = m_rendition.Foreground,
                               .Background = m_rendition.Background});
    DamageRow(m_row);
    if ((m_column + 1U) < m_columns)
    {
        ++m_column;
    }
    else
    {
        m_wrapPending = m_autowrap;
    }
}

void TerminalScreen::Backspace() noexcept
{
    m_wrapPending = false;
    if (m_column != 0U)
    {
        --m_column;
    }
}

void TerminalScreen::HorizontalTab() noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    m_wrapPending = false;
    const UInt32 next{(m_column & ~(TerminalTabWidth - 1U)) + TerminalTabWidth};
    m_column = Minimum(next, m_columns - 1U);
}

void TerminalScreen::LineFeed() noexcept
{
    if (m_rows == 0U)
    {
        return;
    }
    m_wrapPending = false;
    if (m_row == m_bottom)
    {
        ShiftRowsUp(m_top, m_bottom, 1U);
    }
    else if ((m_row + 1U) < m_rows)
    {
        ++m_row;
    }
}

void TerminalScreen::ReverseIndex() noexcept
{
    if (m_rows == 0U)
    {
        return;
    }
    m_wrapPending = false;
    if (m_row == m_top)
    {
        ShiftRowsDown(m_top, m_bottom, 1U);
    }
    else if (m_row != 0U)
    {
        --m_row;
    }
}

void TerminalScreen::CarriageReturn() noexcept
{
    m_wrapPending = false;
    m_column = 0U;
}

void TerminalScreen::CursorUp(const UInt32 count) noexcept
{
    m_wrapPending = false;
    const UInt32 limit{(m_row >= m_top) ? m_top : 0U};
    m_row = ((m_row - limit) > count) ? (m_row - count) : limit;
}

void TerminalScreen::CursorDown(const UInt32 count) noexcept
{
    if (m_rows == 0U)
    {
        return;
    }
    m_wrapPending = false;
    const UInt32 limit{(m_row <= m_bottom) ? m_bottom : (m_rows - 1U)};
    m_row = ((limit - m_row) > count) ? (m_row + count) : limit;
}

void TerminalScreen::CursorForward(const UInt32 count) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    m_wrapPending = false;
    const UInt32 limit{m_columns - 1U};
    m_column = ((limit - m_column) > count) ? (m_column + count) : limit;
}

void TerminalScreen::CursorBackward(const UInt32 count) noexcept
{
    m_wrapPending = false;
    m_column = (m_column > count) ? (m_column - count) : 0U;
}

void TerminalScreen::SetCursor(const UInt32 row, const UInt32 column) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    m_wrapPending = false;
    m_row = Minimum(row, m_rows - 1U);
    m_column = Minimum(column, m_columns - 1U);
}

void TerminalScreen::SetColumn(const UInt32 column) noexcept
{
    SetCursor(m_row, column);
}

void TerminalScreen::SetRow(const UInt32 row) noexcept
{
    SetCursor(row, m_column);
}

void TerminalScreen::EraseInDisplay(const UInt32 selector) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    if (selector == 0U)
    {
        BlankCells(m_row, m_column, m_columns);
        for (UInt32 row{m_row + 1U}; row < m_rows; ++row)
        {
            BlankCells(row, 0U, m_columns);
        }
    }
    else if (selector == 1U)
    {
        for (UInt32 row{0U}; row < m_row; ++row)
        {
            BlankCells(row, 0U, m_columns);
        }
        BlankCells(m_row, 0U, m_column + 1U);
    }
    else if (selector == 2U)
    {
        for (UInt32 row{0U}; row < m_rows; ++row)
        {
            BlankCells(row, 0U, m_columns);
        }
    }
}

void TerminalScreen::EraseInLine(const UInt32 selector) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    if (selector == 0U)
    {
        BlankCells(m_row, m_column, m_columns);
    }
    else if (selector == 1U)
    {
        BlankCells(m_row, 0U, m_column + 1U);
    }
    else if (selector == 2U)
    {
        BlankCells(m_row, 0U, m_columns);
    }
}

void TerminalScreen::EraseCharacters(const UInt32 count) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    const UInt32 room{m_columns - m_column};
    BlankCells(m_row, m_column, m_column + Minimum(count, room));
}

void TerminalScreen::InsertCharacters(const UInt32 count) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    m_wrapPending = false;
    const UInt32 shift{Minimum(count, m_columns - m_column)};
    if (shift == 0U)
    {
        return;
    }
    for (UInt32 column{m_columns - 1U}; column >= (m_column + shift); --column)
    {
        StoreCell(Index(m_row, column), ReadCell(Index(m_row, column - shift)));
    }
    BlankCells(m_row, m_column, m_column + shift);
}

void TerminalScreen::DeleteCharacters(const UInt32 count) noexcept
{
    if (m_columns == 0U)
    {
        return;
    }
    m_wrapPending = false;
    const UInt32 shift{Minimum(count, m_columns - m_column)};
    if (shift == 0U)
    {
        return;
    }
    for (UInt32 column{m_column}; (column + shift) < m_columns; ++column)
    {
        StoreCell(Index(m_row, column), ReadCell(Index(m_row, column + shift)));
    }
    BlankCells(m_row, m_columns - shift, m_columns);
}

void TerminalScreen::InsertLines(const UInt32 count) noexcept
{
    if ((count == 0U) || !InsideRegion())
    {
        return;
    }
    m_wrapPending = false;
    ShiftRowsDown(m_row, m_bottom, count);
    m_column = 0U;
}

void TerminalScreen::DeleteLines(const UInt32 count) noexcept
{
    if ((count == 0U) || !InsideRegion())
    {
        return;
    }
    m_wrapPending = false;
    ShiftRowsUp(m_row, m_bottom, count);
    m_column = 0U;
}

void TerminalScreen::ScrollUp(const UInt32 count) noexcept
{
    if (m_rows == 0U)
    {
        return;
    }
    ShiftRowsUp(m_top, m_bottom, count);
}

void TerminalScreen::ScrollDown(const UInt32 count) noexcept
{
    if (m_rows == 0U)
    {
        return;
    }
    ShiftRowsDown(m_top, m_bottom, count);
}

void TerminalScreen::SetMargins(const UInt32 top, const UInt32 bottom) noexcept
{
    if ((top >= bottom) || (bottom >= m_rows))
    {
        return;
    }
    m_top = top;
    m_bottom = bottom;
    SetCursor(0U, 0U);
}

void TerminalScreen::SaveCursor() noexcept
{
    m_savedRow = m_row;
    m_savedColumn = m_column;
    m_savedRendition = m_rendition;
}

void TerminalScreen::RestoreCursor() noexcept
{
    m_rendition = m_savedRendition;
    SetCursor(m_savedRow, m_savedColumn);
}

void TerminalScreen::SetRendition(const TerminalRendition rendition) noexcept
{
    m_rendition = rendition;
}

TerminalRendition TerminalScreen::Rendition() const noexcept
{
    return m_rendition;
}

void TerminalScreen::SetApplicationCursorKeys(const bool enabled) noexcept
{
    m_applicationCursorKeys = enabled;
}

void TerminalScreen::SetAutowrap(const bool enabled) noexcept
{
    m_autowrap = enabled;
}

void TerminalScreen::SetCursorVisible(const bool visible) noexcept
{
    m_cursorVisible = visible;
}

bool TerminalScreen::ApplicationCursorKeys() const noexcept
{
    return m_applicationCursorKeys;
}

bool TerminalScreen::Autowrap() const noexcept
{
    return m_autowrap;
}

bool TerminalScreen::CursorVisible() const noexcept
{
    return m_cursorVisible;
}

UInt32 TerminalScreen::Columns() const noexcept
{
    return m_columns;
}

UInt32 TerminalScreen::Rows() const noexcept
{
    return m_rows;
}

UInt32 TerminalScreen::CursorRow() const noexcept
{
    return m_row;
}

UInt32 TerminalScreen::CursorColumn() const noexcept
{
    return m_column;
}

bool TerminalScreen::WrapPending() const noexcept
{
    return m_wrapPending;
}

UInt32 TerminalScreen::MarginTop() const noexcept
{
    return m_top;
}

UInt32 TerminalScreen::MarginBottom() const noexcept
{
    return m_bottom;
}

TerminalCellView TerminalScreen::CellAt(const UInt32 row, const UInt32 column) const noexcept
{
    if ((row >= m_rows) || (column >= m_columns))
    {
        return TerminalCellView{.CodePoint = TerminalBlankCode,
                                .Attributes = 0U,
                                .Foreground = TerminalDefaultColor,
                                .Background = TerminalDefaultColor};
    }
    return ReadCell(Index(row, column));
}

bool TerminalScreen::IsRowDamaged(const UInt32 row) const noexcept
{
    if (row >= m_rows)
    {
        return false;
    }
    if (((m_damage[row >> 6U] >> (row & 63U)) & 1U) != 0U)
    {
        return true;
    }
    const bool moved{(m_drawnVisible != m_cursorVisible) ||
                     (m_cursorVisible && ((m_drawnRow != m_row) || (m_drawnColumn != m_column)))};
    return moved && ((m_drawnVisible && (row == m_drawnRow)) || (m_cursorVisible && (row == m_row)));
}

bool TerminalScreen::HasDamage() const noexcept
{
    for (UInt32 row{0U}; row < m_rows; ++row)
    {
        if (IsRowDamaged(row))
        {
            return true;
        }
    }
    return false;
}

void TerminalScreen::ClearDamage() noexcept
{
    for (UInt64& word : m_damage)
    {
        word = 0U;
    }
    m_drawnRow = m_row;
    m_drawnColumn = m_column;
    m_drawnVisible = m_cursorVisible;
}

Size TerminalScreen::Index(const UInt32 row, const UInt32 column) const noexcept
{
    return (static_cast<Size>(row) * TerminalMaximumColumns) + column;
}

TerminalCellView TerminalScreen::Blank() const noexcept
{
    return TerminalCellView{.CodePoint = TerminalBlankCode,
                            .Attributes = 0U,
                            .Foreground = TerminalDefaultColor,
                            .Background = m_rendition.Background};
}

void TerminalScreen::DamageRow(const UInt32 row) noexcept
{
    if (row < TerminalMaximumRows)
    {
        m_damage[row >> 6U] |= UInt64{1U} << (row & 63U);
    }
}

void TerminalScreen::DamageRows(const UInt32 first, const UInt32 last) noexcept
{
    for (UInt32 row{first}; (row <= last) && (row < m_rows); ++row)
    {
        DamageRow(row);
    }
}

void TerminalScreen::BlankCells(const UInt32 row, const UInt32 first, const UInt32 end) noexcept
{
    const TerminalCellView blank{Blank()};
    const UInt32 limit{Minimum(end, m_columns)};
    for (UInt32 column{first}; column < limit; ++column)
    {
        StoreCell(Index(row, column), blank);
    }
    DamageRow(row);
}

void TerminalScreen::StoreCell(const Size index, const TerminalCellView cell) noexcept
{
    m_cells[index].Bits = (cell.CodePoint & 0x1FFFFFU) | ((static_cast<UInt32>(cell.Foreground) & 0xFU) << 21U) |
                          ((static_cast<UInt32>(cell.Background) & 0xFU) << 25U) |
                          ((static_cast<UInt32>(cell.Attributes) & 0x7U) << 29U);
    const UInt64 mask{UInt64{1U} << (index & 63U)};
    const Size word{index >> 6U};
    m_defaultForeground[word] =
        (m_defaultForeground[word] & ~mask) | (cell.Foreground == TerminalDefaultColor ? mask : 0U);
    m_defaultBackground[word] =
        (m_defaultBackground[word] & ~mask) | (cell.Background == TerminalDefaultColor ? mask : 0U);
}

TerminalCellView TerminalScreen::ReadCell(const Size index) const noexcept
{
    const auto cell = m_cells[index];
    const UInt64 mask{UInt64{1U} << (index & 63U)};
    const Size word{index >> 6U};
    return TerminalCellView{cell.CodePoint(), cell.Attributes(),
                            (m_defaultForeground[word] & mask) != 0U ? TerminalDefaultColor : cell.Foreground(),
                            (m_defaultBackground[word] & mask) != 0U ? TerminalDefaultColor : cell.Background()};
}

void TerminalScreen::CopyRow(const UInt32 to, const UInt32 from) noexcept
{
    for (UInt32 column{0U}; column < m_columns; ++column)
    {
        StoreCell(Index(to, column), ReadCell(Index(from, column)));
    }
}

void TerminalScreen::ShiftRowsUp(const UInt32 first, const UInt32 last, const UInt32 count) noexcept
{
    if ((first > last) || (last >= m_rows) || (count == 0U))
    {
        return;
    }
    const UInt32 shift{Minimum(count, last - first + 1U)};
    for (UInt32 row{first}; (row + shift) <= last; ++row)
    {
        CopyRow(row, row + shift);
    }
    for (UInt32 row{last + 1U - shift}; row <= last; ++row)
    {
        BlankCells(row, 0U, m_columns);
    }
    DamageRows(first, last);
}

void TerminalScreen::ShiftRowsDown(const UInt32 first, const UInt32 last, const UInt32 count) noexcept
{
    if ((first > last) || (last >= m_rows) || (count == 0U))
    {
        return;
    }
    const UInt32 shift{Minimum(count, last - first + 1U)};
    for (UInt32 row{last}; row >= (first + shift); --row)
    {
        CopyRow(row, row - shift);
    }
    for (UInt32 row{first}; row < (first + shift); ++row)
    {
        BlankCells(row, 0U, m_columns);
    }
    DamageRows(first, last);
}

bool TerminalScreen::InsideRegion() const noexcept
{
    return (m_rows != 0U) && (m_row >= m_top) && (m_row <= m_bottom);
}
} // namespace Asteria::User::Terminal
