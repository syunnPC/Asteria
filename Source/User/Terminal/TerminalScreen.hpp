// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TERMINAL_TERMINAL_SCREEN_HPP_INCLUDED
#define ASTERIA_USER_TERMINAL_TERMINAL_SCREEN_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Text/Utf8.hpp"

namespace Asteria::User::Terminal
{
/// The terminal's grid: 80 x 25 cells by default, at most 320 x 90, which a maximized window on a
/// 2560x1440 output needs (320 columns of 8 pixels, 88 rows of 16). The grid always occupies the
/// static maximum: TerminalMaximumCells cells of four bytes (115,200 bytes).
inline constexpr Foundation::UInt32 TerminalDefaultColumns{80U};
inline constexpr Foundation::UInt32 TerminalDefaultRows{25U};
inline constexpr Foundation::UInt32 TerminalMaximumColumns{320U};
inline constexpr Foundation::UInt32 TerminalMaximumRows{90U};
inline constexpr Foundation::Size TerminalMaximumCells{TerminalMaximumColumns * TerminalMaximumRows};
/// Horizontal tab stops: every TerminalTabWidth columns.
inline constexpr Foundation::UInt32 TerminalTabWidth{8U};

/// A cell's colour that is not a palette index: the default foreground or background (SGR 39 and
/// 49), resolved when the cell is drawn to the default pair. It differs from every index 0-15, so
/// bold, which brightens a normal colour 0-7, leaves the default alone.
inline constexpr Foundation::UInt8 TerminalDefaultColor{0xFFU};
/// The blank a cell holds after an erase.
inline constexpr Foundation::UInt8 TerminalBlankCode{0x20U};

/// A cell's Attributes bits (SGR 1, 4 and 7).
inline constexpr Foundation::UInt8 TerminalBold{0x01U};
inline constexpr Foundation::UInt8 TerminalUnderline{0x02U};
inline constexpr Foundation::UInt8 TerminalReverse{0x04U};

/// The grid stores Unicode in bits 0-20, foreground in 21-24, background in 25-28, flags in 29-31.
/// Explicit shifts avoid ABI-dependent bitfield layout.
struct TerminalCell
{
    Foundation::UInt32 Bits;
    [[nodiscard]] constexpr Foundation::UInt32 CodePoint() const noexcept
    {
        return Bits & 0x1FFFFFU;
    }
    [[nodiscard]] constexpr Foundation::UInt8 Foreground() const noexcept
    {
        return static_cast<Foundation::UInt8>((Bits >> 21U) & 0xFU);
    }
    [[nodiscard]] constexpr Foundation::UInt8 Background() const noexcept
    {
        return static_cast<Foundation::UInt8>((Bits >> 25U) & 0xFU);
    }
    [[nodiscard]] constexpr Foundation::UInt8 Attributes() const noexcept
    {
        return static_cast<Foundation::UInt8>(Bits >> 29U);
    }
};
static_assert(sizeof(TerminalCell) == 4U);
static_assert((21U + 4U + 4U + 3U) == (sizeof(TerminalCell) * 8U));
/// An unpacked view, returned by value, resolves the two bounded default
/// colour bitmaps back to TerminalDefaultColor for the renderer/caller.
/// The bitmaps cost 7,200 bytes and preserve all sixteen SGR indices plus
/// the distinct default colour pair; they follow every cell move/erase.
struct TerminalCellView
{
    Foundation::UInt32 CodePoint;
    Foundation::UInt8 Attributes;
    Foundation::UInt8 Foreground;
    Foundation::UInt8 Background;
};
inline constexpr Foundation::Size TerminalDefaultBitmapWords{TerminalMaximumCells / 64U};
static_assert((TerminalMaximumCells % 64U) == 0U);

/// The graphic rendition new cells are written with (SGR state).
struct TerminalRendition
{
    Foundation::UInt8 Attributes;
    Foundation::UInt8 Foreground;
    Foundation::UInt8 Background;
};

/// The cell grid, the cursor, the scrolling region, the DEC modes the subset keeps and per-row
/// damage. Meant for static storage (it holds the maximum grid); no allocation, every operation
/// bounded by the grid. The parser (TerminalParser) maps control functions onto the operations
/// below, whose comments state the behaviour this terminal implements: - The cursor has a deferred
/// wrap: writing a graphic into the last column leaves the cursor there with the wrap pending (when
/// autowrap, DEC mode 7, is on); the next graphic first moves to column 0 of the next line
/// (scrolling at the region's bottom margin). Every operation that sets the cursor's position (the
/// C0 movements, IND, RI, NEL, the CSI cursor functions, IL, DL, DECSTBM, DECRC, RIS, Resize) and
/// ICH and DCH cancel a pending wrap; erasing (ED, EL, ECH), SU, SD, SGR, modes and reports leave
/// it, so a line filled to its end and then cleared to its end still wraps. With autowrap off the
/// last column is overwritten. - Erasing (ED, EL, ECH, the blanks of IL, DL, ICH, DCH, SU, SD and
/// of scrolling) writes TerminalBlankCode with no attributes, the default foreground and the
/// current background colour ("background colour erase"), so a coloured background fills cleared
/// lines. - The scrolling region (DECSTBM) bounds LF, IND, RI, IL, DL, SU and SD: a line feed
/// scrolls only when the cursor is on the bottom margin, a reverse index only on the top margin;
/// outside the region they move the cursor without scrolling and stop at the screen's edge. - The
/// cursor is drawn (a block, the cell in reversed final colours) when DEC mode 25 is on; it
/// occupies the last column while a wrap is pending.
class TerminalScreen final
{
public:
    /// Sets the size (each clamped to 1 up to the maximum) and resets
    /// everything as Reset does.
    void Configure(Foundation::UInt32 columns, Foundation::UInt32 rows) noexcept;

    /// Full reset (RIS): every cell blank in the default colours, the cursor
    /// home, default rendition, the whole screen as the scrolling region,
    /// autowrap and cursor visible on, application cursor keys off, the saved
    /// cursor home with the default rendition, every row damaged.
    void Reset() noexcept;

    /// Changes the size without reflowing: the top-left cells common to both
    /// sizes are kept, new cells are blank in the default colours, the
    /// cursor and the saved cursor are clamped, the scrolling region becomes
    /// the whole screen, a pending wrap is cancelled and every row damaged.
    void Resize(Foundation::UInt32 columns, Foundation::UInt32 rows) noexcept;

    /// Writes a Unicode scalar at the cursor with the current
    /// rendition, wrapping first when a wrap is pending.
    void Print(Foundation::UInt32 codePoint) noexcept;

    /// BS: one column left, stopping at column 0; never erases.
    void Backspace() noexcept;
    /// HT: to the next multiple of TerminalTabWidth, at most the last column.
    void HorizontalTab() noexcept;
    /// LF, VT, FF and IND: one line down; scrolls the region up at its bottom
    /// margin; stops at the last line below the region. The column is kept.
    void LineFeed() noexcept;
    /// RI: one line up; scrolls the region down at its top margin; stops at
    /// the first line above the region.
    void ReverseIndex() noexcept;
    /// CR: column 0.
    void CarriageReturn() noexcept;

    /// CUU and CUD: count lines up or down, stopping at the top margin when
    /// the cursor starts at or below it (CUU), at the bottom margin when it
    /// starts at or above it (CUD), else at the screen's edge.
    void CursorUp(Foundation::UInt32 count) noexcept;
    void CursorDown(Foundation::UInt32 count) noexcept;
    /// CUF and CUB: count columns right or left, stopping at the edge.
    void CursorForward(Foundation::UInt32 count) noexcept;
    void CursorBackward(Foundation::UInt32 count) noexcept;
    /// CUP, HVP, CHA and VPA: zero-based positions, clamped to the screen.
    void SetCursor(Foundation::UInt32 row, Foundation::UInt32 column) noexcept;
    void SetColumn(Foundation::UInt32 column) noexcept;
    void SetRow(Foundation::UInt32 row) noexcept;

    /// ED and EL: 0 from the cursor to the end (inclusive), 1 from the start
    /// to the cursor (inclusive), 2 all; other selectors do nothing. The
    /// cursor stays.
    void EraseInDisplay(Foundation::UInt32 selector) noexcept;
    void EraseInLine(Foundation::UInt32 selector) noexcept;
    /// ECH: count cells from the cursor, at most to the line's end.
    void EraseCharacters(Foundation::UInt32 count) noexcept;
    /// ICH and DCH: inserts blanks at the cursor (cells pushed past the last
    /// column are lost) or deletes cells (blanks enter at the line's end).
    /// The cursor stays.
    void InsertCharacters(Foundation::UInt32 count) noexcept;
    void DeleteCharacters(Foundation::UInt32 count) noexcept;
    /// IL and DL: inserts or deletes count lines at the cursor's line inside
    /// the region (lines leave or enter at its bottom margin), then moves the
    /// cursor to column 0; nothing for a zero count or when the cursor is
    /// outside the region.
    void InsertLines(Foundation::UInt32 count) noexcept;
    void DeleteLines(Foundation::UInt32 count) noexcept;
    /// SU and SD: the region's content moves up (blank lines enter at the
    /// bottom margin) or down (at the top margin) by count lines. The cursor
    /// stays.
    void ScrollUp(Foundation::UInt32 count) noexcept;
    void ScrollDown(Foundation::UInt32 count) noexcept;
    /// DECSTBM: zero-based top and bottom margins; refused (nothing changes)
    /// unless top < bottom < Rows. Moves the cursor home.
    void SetMargins(Foundation::UInt32 top, Foundation::UInt32 bottom) noexcept;

    /// DECSC and DECRC: saves or restores the cursor position and the
    /// rendition; a restore clamps the position and cancels a pending wrap.
    void SaveCursor() noexcept;
    void RestoreCursor() noexcept;

    /// The rendition later graphics and erases use (SGR).
    void SetRendition(TerminalRendition rendition) noexcept;
    [[nodiscard]] TerminalRendition Rendition() const noexcept;

    /// DEC modes 1 (application cursor keys), 7 (autowrap), 25 (cursor
    /// visible).
    void SetApplicationCursorKeys(bool enabled) noexcept;
    void SetAutowrap(bool enabled) noexcept;
    void SetCursorVisible(bool visible) noexcept;
    [[nodiscard]] bool ApplicationCursorKeys() const noexcept;
    [[nodiscard]] bool Autowrap() const noexcept;
    [[nodiscard]] bool CursorVisible() const noexcept;

    [[nodiscard]] Foundation::UInt32 Columns() const noexcept;
    [[nodiscard]] Foundation::UInt32 Rows() const noexcept;
    [[nodiscard]] Foundation::UInt32 CursorRow() const noexcept;
    /// The cursor's column, at most Columns - 1 (the last column while a
    /// wrap is pending).
    [[nodiscard]] Foundation::UInt32 CursorColumn() const noexcept;
    [[nodiscard]] bool WrapPending() const noexcept;
    [[nodiscard]] Foundation::UInt32 MarginTop() const noexcept;
    [[nodiscard]] Foundation::UInt32 MarginBottom() const noexcept;

    /// A cell of the grid; a blank default cell outside it.
    [[nodiscard]] TerminalCellView CellAt(Foundation::UInt32 row, Foundation::UInt32 column) const noexcept;

    /// A row must be drawn again: one of its cells changed, or the drawn
    /// cursor (position or visibility) left or entered it, since ClearDamage.
    [[nodiscard]] bool IsRowDamaged(Foundation::UInt32 row) const noexcept;
    [[nodiscard]] bool HasDamage() const noexcept;
    /// Forgets the damage once the rows were drawn.
    void ClearDamage() noexcept;

private:
    [[nodiscard]] Foundation::Size Index(Foundation::UInt32 row, Foundation::UInt32 column) const noexcept;
    [[nodiscard]] TerminalCellView Blank() const noexcept;
    void DamageRow(Foundation::UInt32 row) noexcept;
    void DamageRows(Foundation::UInt32 first, Foundation::UInt32 last) noexcept;
    void BlankCells(Foundation::UInt32 row, Foundation::UInt32 first, Foundation::UInt32 end) noexcept;
    void StoreCell(Foundation::Size index, TerminalCellView cell) noexcept;
    [[nodiscard]] TerminalCellView ReadCell(Foundation::Size index) const noexcept;
    void CopyRow(Foundation::UInt32 to, Foundation::UInt32 from) noexcept;
    /// Moves rows [first, last] up (blanks enter at last) or down (at first)
    /// by count, which is clamped to the span.
    void ShiftRowsUp(Foundation::UInt32 first, Foundation::UInt32 last, Foundation::UInt32 count) noexcept;
    void ShiftRowsDown(Foundation::UInt32 first, Foundation::UInt32 last, Foundation::UInt32 count) noexcept;
    [[nodiscard]] bool InsideRegion() const noexcept;

    static constexpr Foundation::Size DamageWords{(TerminalMaximumRows + 63U) / 64U};

    TerminalCell m_cells[TerminalMaximumCells]{};
    Foundation::UInt64 m_defaultForeground[TerminalDefaultBitmapWords]{};
    Foundation::UInt64 m_defaultBackground[TerminalDefaultBitmapWords]{};
    Foundation::UInt64 m_damage[DamageWords]{};
    Foundation::UInt32 m_columns{0U};
    Foundation::UInt32 m_rows{0U};
    Foundation::UInt32 m_row{0U};
    Foundation::UInt32 m_column{0U};
    Foundation::UInt32 m_top{0U};
    Foundation::UInt32 m_bottom{0U};
    Foundation::UInt32 m_savedRow{0U};
    Foundation::UInt32 m_savedColumn{0U};
    TerminalRendition m_rendition{};
    TerminalRendition m_savedRendition{};
    bool m_wrapPending{false};
    bool m_autowrap{true};
    bool m_cursorVisible{true};
    bool m_applicationCursorKeys{false};
    /// Where the cursor was drawn last (as of ClearDamage).
    Foundation::UInt32 m_drawnRow{0U};
    Foundation::UInt32 m_drawnColumn{0U};
    bool m_drawnVisible{false};
};
} // Namespace Asteria::User::Terminal

#endif // ASTERIA_USER_TERMINAL_TERMINAL_SCREEN_HPP_INCLUDED
