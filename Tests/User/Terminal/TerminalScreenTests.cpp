// SPDX-License-Identifier: Apache-2.0
#include "User/Terminal/Terminal.hpp"
#include "User/Terminal/TerminalScreen.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// The screen's size limits, Resize, row damage and the cursor's part in it,
// and the operations' behaviour before Configure.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;
using User::Terminal::Terminal;
using User::Terminal::TerminalCellView;
using User::Terminal::TerminalDefaultColor;
using User::Terminal::TerminalScreen;

Terminal g_terminal;
TerminalScreen g_screen;

void Write(const char* const text, const Size length) noexcept
{
    g_terminal.Write(reinterpret_cast<const UInt8*>(text), length);
}

#define ASTERIA_WRITE(text) Write(text, sizeof(text) - 1U)

[[nodiscard]] UInt32 DamagedRows(const TerminalScreen& screen) noexcept
{
    UInt32 mask{0U};
    for (UInt32 row{0U}; row < screen.Rows(); ++row)
    {
        if (screen.IsRowDamaged(row))
        {
            mask |= 1U << row;
        }
    }
    return mask;
}

void TestSizes(TestContext& runner) noexcept
{
    g_terminal.Configure(0U, 0U);
    ASTERIA_EXPECT(runner, "a zero size is one cell", g_terminal.Screen().Columns() == 1U,
                   g_terminal.Screen().Rows() == 1U);
    g_terminal.Configure(100'000U, 100'000U);
    ASTERIA_EXPECT(runner, "a large size is clamped to the maximum",
                   g_terminal.Screen().Columns() == User::Terminal::TerminalMaximumColumns,
                   g_terminal.Screen().Rows() == User::Terminal::TerminalMaximumRows);
    ASTERIA_EXPECT(runner, "the defaults and maximum", User::Terminal::TerminalDefaultColumns == 80U,
                   User::Terminal::TerminalDefaultRows == 25U, User::Terminal::TerminalMaximumColumns == 320U,
                   User::Terminal::TerminalMaximumRows == 90U, sizeof(User::Terminal::TerminalCell) == 4U);
    ASTERIA_WRITE("\x1b[90;320Hx");
    ASTERIA_EXPECT(runner, "the last cell of the largest grid", g_terminal.Screen().CellAt(89U, 319U).CodePoint == 'x',
                   g_terminal.Screen().WrapPending());
    g_terminal.Configure(1U, 1U);
    ASTERIA_WRITE("ab\r\nc\x1b[5;5Hd");
    ASTERIA_EXPECT(runner, "a one-cell terminal", g_terminal.Screen().CellAt(0U, 0U).CodePoint == 'd',
                   g_terminal.Screen().CursorRow() == 0U, g_terminal.Screen().CursorColumn() == 0U,
                   g_terminal.Screen().WrapPending());
    ASTERIA_EXPECT(runner, "outside the grid reads blank", g_terminal.Screen().CellAt(1U, 0U).CodePoint == 0x20U,
                   g_terminal.Screen().CellAt(0U, 1U).Foreground == TerminalDefaultColor,
                   !g_terminal.Screen().IsRowDamaged(1U));
}

void TestResize(TestContext& runner) noexcept
{
    g_terminal.Configure(10U, 4U);
    ASTERIA_WRITE("0123456789\r\nabcdefghij\r\nABCDEFGHIJ\x1b[2;3r\x1b[4;10H\x1b"
                  "7\x1b[44m");
    g_terminal.Resize(5U, 2U);
    const TerminalScreen& screen = g_terminal.Screen();
    ASTERIA_EXPECT(runner, "a smaller size keeps the top-left cells", screen.Columns() == 5U, screen.Rows() == 2U,
                   screen.CellAt(0U, 4U).CodePoint == '4', screen.CellAt(1U, 0U).CodePoint == 'a',
                   screen.CellAt(1U, 4U).CodePoint == 'e');
    ASTERIA_EXPECT(runner, "the cursor and region are clamped", screen.CursorRow() == 1U, screen.CursorColumn() == 4U,
                   screen.MarginTop() == 0U, screen.MarginBottom() == 1U, DamagedRows(screen) == 3U);
    g_terminal.ClearDamage();
    g_terminal.Resize(10U, 4U);
    ASTERIA_EXPECT(runner, "a larger size blanks what it exposes", screen.CellAt(0U, 4U).CodePoint == '4',
                   screen.CellAt(0U, 5U).CodePoint == 0x20U, screen.CellAt(1U, 9U).CodePoint == 0x20U,
                   screen.CellAt(2U, 0U).CodePoint == 0x20U, screen.CellAt(2U, 0U).Background == TerminalDefaultColor,
                   DamagedRows(screen) == 15U);
    ASTERIA_WRITE("\x1b"
                  "8x");
    ASTERIA_EXPECT(runner, "the saved cursor was clamped by the smaller size", screen.CellAt(1U, 4U).CodePoint == 'x');
    ASTERIA_WRITE("\x1b[5");
    g_terminal.Resize(12U, 5U);
    ASTERIA_WRITE("Cy");
    ASTERIA_EXPECT(runner, "Resize keeps the parser's state", screen.CellAt(1U, 10U).CodePoint == 'y');
    ASTERIA_WRITE("\x1b[1;12Hz");
    const bool pending{screen.WrapPending()};
    g_terminal.Resize(12U, 5U);
    ASTERIA_EXPECT(runner, "Resize cancels a pending wrap", pending, !screen.WrapPending());

    static TerminalScreen fresh;
    fresh.Resize(3U, 2U);
    ASTERIA_EXPECT(runner, "resizing an unconfigured screen configures it", fresh.Columns() == 3U, fresh.Rows() == 2U,
                   fresh.CellAt(1U, 2U).CodePoint == 0x20U);
}

void TestDamage(TestContext& runner) noexcept
{
    g_terminal.Configure(10U, 6U);
    const TerminalScreen& screen = g_terminal.Screen();
    ASTERIA_EXPECT(runner, "Configure damages every row", DamagedRows(screen) == 0x3FU, screen.HasDamage());
    g_terminal.ClearDamage();
    ASTERIA_EXPECT(runner, "ClearDamage forgets it", DamagedRows(screen) == 0U, !screen.HasDamage());
    ASTERIA_WRITE("ab");
    ASTERIA_EXPECT(runner, "text damages its row", DamagedRows(screen) == 1U);
    g_terminal.ClearDamage();
    ASTERIA_WRITE("\x1b[4;1H");
    ASTERIA_EXPECT(runner, "a cursor move damages the old and new rows", DamagedRows(screen) == 9U);
    g_terminal.ClearDamage();
    ASTERIA_WRITE("\x1b[4;5H");
    ASTERIA_EXPECT(runner, "a move within a row damages it", DamagedRows(screen) == 8U);
    g_terminal.ClearDamage();
    ASTERIA_WRITE("\x1b[?25l");
    ASTERIA_EXPECT(runner, "hiding the cursor damages its row", DamagedRows(screen) == 8U);
    g_terminal.ClearDamage();
    ASTERIA_WRITE("\x1b[2;1H");
    ASTERIA_EXPECT(runner, "a hidden cursor's moves damage nothing", DamagedRows(screen) == 0U);
    ASTERIA_WRITE("\x1b[?25h");
    ASTERIA_EXPECT(runner, "showing the cursor damages its row", DamagedRows(screen) == 2U);
    g_terminal.ClearDamage();
    ASTERIA_WRITE("\x1b[2;4r\x1b[S");
    ASTERIA_EXPECT(runner, "scrolling damages the region and the cursor's rows", DamagedRows(screen) == 0x0FU);
    g_terminal.ClearDamage();
    ASTERIA_WRITE("\x1b[31m\x1b[?1h\x1b[6n");
    ASTERIA_EXPECT(runner, "SGR, modes and reports damage nothing", DamagedRows(screen) == 0U);
    ASTERIA_WRITE("\x1b[6;1H\x1b[K");
    ASTERIA_EXPECT(runner, "an erase damages its row", DamagedRows(screen) == 0x21U);
}

void TestUnconfiguredScreen(TestContext& runner) noexcept
{
    // Every operation of a screen that was never configured does nothing.
    g_screen.Print('a');
    g_screen.Backspace();
    g_screen.HorizontalTab();
    g_screen.LineFeed();
    g_screen.ReverseIndex();
    g_screen.CarriageReturn();
    g_screen.CursorUp(3U);
    g_screen.CursorDown(3U);
    g_screen.CursorForward(3U);
    g_screen.CursorBackward(3U);
    g_screen.SetCursor(2U, 2U);
    g_screen.SetColumn(2U);
    g_screen.SetRow(2U);
    g_screen.EraseInDisplay(2U);
    g_screen.EraseInLine(2U);
    g_screen.EraseCharacters(2U);
    g_screen.InsertCharacters(2U);
    g_screen.DeleteCharacters(2U);
    g_screen.InsertLines(2U);
    g_screen.DeleteLines(2U);
    g_screen.ScrollUp(2U);
    g_screen.ScrollDown(2U);
    g_screen.SetMargins(0U, 1U);
    g_screen.SaveCursor();
    g_screen.RestoreCursor();
    g_screen.Reset();
    ASTERIA_EXPECT(runner, "an unconfigured screen stays empty", g_screen.Columns() == 0U, g_screen.Rows() == 0U,
                   g_screen.CursorRow() == 0U, g_screen.CursorColumn() == 0U, !g_screen.HasDamage(),
                   g_screen.CellAt(0U, 0U).CodePoint == 0x20U);
}

void TestDirectOperations(TestContext& runner) noexcept
{
    g_screen.Configure(6U, 3U);
    g_screen.SetCursor(1U, 2U);
    g_screen.InsertCharacters(0U);
    g_screen.DeleteCharacters(0U);
    g_screen.EraseCharacters(0U);
    g_screen.InsertLines(0U);
    g_screen.ScrollUp(0U);
    g_screen.CursorUp(0U);
    ASTERIA_EXPECT(runner, "zero counts change nothing", g_screen.CursorRow() == 1U, g_screen.CursorColumn() == 2U);
    g_screen.SetCursor(0U, 5U);
    g_screen.InsertCharacters(1U);
    g_screen.Print('z');
    ASTERIA_EXPECT(runner, "ICH at the last column blanks it", g_screen.CellAt(0U, 5U).CodePoint == 'z',
                   g_screen.WrapPending());
    g_screen.SetMargins(1U, 1U);
    g_screen.SetMargins(2U, 3U);
    ASTERIA_EXPECT(runner, "invalid margins are refused", g_screen.MarginTop() == 0U, g_screen.MarginBottom() == 2U);
}
void TestUnicodeCells(TestContext& runner) noexcept
{
    using User::Terminal::TerminalCell;
    constexpr TerminalCell packed{0x10FFFFU | (9U << 21U) | (4U << 25U) | (7U << 29U)};
    ASTERIA_EXPECT(runner, "packed cell fields do not overlap", packed.CodePoint() == 0x10FFFFU,
                   packed.Foreground() == 9U, packed.Background() == 4U, packed.Attributes() == 7U,
                   sizeof(packed) == 4U);
    g_screen.Configure(80U, 3U);
    g_screen.SetRendition({7U, 9U, 4U});
    g_screen.SetCursor(0U, 63U);
    g_screen.Print(0x4E2DU);
    g_screen.Print(0x10FFFFU);
    g_screen.SetRendition({0U, TerminalDefaultColor, TerminalDefaultColor});
    g_screen.Print(0x2500U);
    g_screen.SetCursor(0U, 63U);
    g_screen.InsertCharacters(1U);
    const auto kept{g_screen.CellAt(0U, 64U)};
    const auto defaults{g_screen.CellAt(0U, 66U)};
    ASTERIA_EXPECT(runner, "Unicode and default bitmaps follow insertion across a bitmap word",
                   kept.CodePoint == 0x4E2DU, kept.Attributes == 7U, kept.Foreground == 9U, kept.Background == 4U,
                   defaults.CodePoint == 0x2500U, defaults.Foreground == TerminalDefaultColor,
                   defaults.Background == TerminalDefaultColor);
    g_screen.SetCursor(1U, 0U);
    g_screen.Print(0xD800U);
    g_screen.Print(0x110000U);
    ASTERIA_EXPECT(runner, "direct invalid scalars store Unicode replacement",
                   g_screen.CellAt(1U, 0U).CodePoint == User::Text::ReplacementCodePoint,
                   g_screen.CellAt(1U, 1U).CodePoint == User::Text::ReplacementCodePoint);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TerminalScreenTests"};
    Asteria::Test::TestSizes(runner);
    Asteria::Test::TestResize(runner);
    Asteria::Test::TestDamage(runner);
    Asteria::Test::TestUnconfiguredScreen(runner);
    Asteria::Test::TestDirectOperations(runner);
    Asteria::Test::TestUnicodeCells(runner);
    return runner.Finish();
}
