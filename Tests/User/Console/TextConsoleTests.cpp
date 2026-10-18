// SPDX-License-Identifier: Apache-2.0
#include "User/Console/TextConsole.hpp"

#include <cstring>

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;
using User::Console::ConsoleCell;
using User::Console::TextConsole;
using User::Graphics::PixelFormat;
using User::Graphics::Point;
using User::Graphics::Surface;

constexpr UInt32 Sentinel{0xDEADBEEFU};

// The console holds ConsoleMaximumCells bytes: static, as it is meant to be.
TextConsole g_console;

void Put(TextConsole& console, const char* const text) noexcept
{
    console.Write(reinterpret_cast<const UInt8*>(text), std::strlen(text));
}

void Start(const UInt32 columns, const UInt32 rows) noexcept
{
    g_console.Configure(columns * 8U, rows * 16U, User::Graphics::Bold8x16);
}

[[nodiscard]] bool RowIs(const TextConsole& console, const UInt32 row, const char* const text) noexcept
{
    const Size length{std::strlen(text)};
    for (UInt32 column{0U}; column < console.Columns(); ++column)
    {
        const UInt8 expected{column < length ? static_cast<UInt8>(text[column]) : UInt8{0x20U}};
        if (console.CellAt(row, column).Code != expected)
        {
            return false;
        }
    }
    return true;
}

void TestConfigure(TestContext& runner) noexcept
{
    g_console.Configure(1280U, 800U, User::Graphics::Bold8x16);
    ASTERIA_EXPECT(runner, "a 1280x800 screen gives 160x50 cells", g_console.Columns() == 160U,
                   g_console.Rows() == 50U);
    g_console.Configure(1920U, 1080U, User::Graphics::Bold8x16);
    ASTERIA_EXPECT(runner, "a 1920x1080 screen is filled", g_console.Columns() == 240U, g_console.Rows() == 67U);
    g_console.Configure(3840U, 2160U, User::Graphics::Bold8x16);
    ASTERIA_EXPECT(runner, "a 3840x2160 screen is filled", g_console.Columns() == 480U, g_console.Rows() == 135U);
    g_console.Configure(5120U, 1440U, User::Graphics::Bold8x16);
    ASTERIA_EXPECT(runner, "a 5120x1440 screen is filled", g_console.Columns() == 640U, g_console.Rows() == 90U);
    g_console.Configure(7680U, 4320U, User::Graphics::Bold8x16);
    ASTERIA_EXPECT(runner, "larger screens clamp to the columns, then to the cell budget",
                   g_console.Columns() == User::Console::ConsoleMaximumColumns,
                   g_console.Rows() == (User::Console::ConsoleMaximumCells / User::Console::ConsoleMaximumColumns));
    g_console.Configure(8U, 16U * 1000U, User::Graphics::Bold8x16);
    ASTERIA_EXPECT(runner, "a tall narrow screen clamps to the maximum rows", g_console.Columns() == 1U,
                   g_console.Rows() == User::Console::ConsoleMaximumRows);
    g_console.Configure(645U, 47U, User::Graphics::Bold8x16);
    ASTERIA_EXPECT(runner, "partial cells are dropped", g_console.Columns() == 80U, g_console.Rows() == 2U);
    ASTERIA_EXPECT(runner, "configuring damages every row and homes the cursor", g_console.Damage().First == 0U,
                   g_console.Damage().End == 2U, g_console.Row() == 0U, g_console.Column() == 0U);
    g_console.Configure(7U, 100U, User::Graphics::Bold8x16);
    Put(g_console, "ignored\n");
    ASTERIA_EXPECT(runner, "a surface smaller than a cell leaves an inert console", g_console.Columns() == 0U,
                   g_console.Rows() == 0U, !g_console.HasDamage(), g_console.Column() == 0U);
    const TextConsole fresh{};
    ASTERIA_EXPECT(runner, "an unconfigured console is inert", fresh.Columns() == 0U, !fresh.HasDamage());
    ASTERIA_EXPECT(runner, "the colors are the documented ones", User::Console::ConsoleForeground.Red == 0xFFU,
                   User::Console::ConsoleBackground.Blue == 0x40U);
}

void TestManyRows(TestContext& runner) noexcept
{
    // Rows from 64 on keep their wrap marks in a second word.
    Start(4U, 130U);
    for (UInt32 line{0U}; line < 63U; ++line)
    {
        Put(g_console, "\n");
    }
    Put(g_console, "abcdefgh\b \b\b \b\b \b\b \b\b \b");
    ASTERIA_EXPECT(runner, "an erase crosses a wrap into row 63 from row 64", RowIs(g_console, 63U, "abc"),
                   RowIs(g_console, 64U, ""), g_console.Row() == 63U, g_console.Column() == 3U);
    // Scrolling carries a wrap mark from one word into the next lower one.
    Start(4U, 66U);
    for (UInt32 line{0U}; line < 65U; ++line)
    {
        Put(g_console, "\n");
    }
    Put(g_console, "abcdefghijklmnop");
    ASTERIA_EXPECT(runner, "three wraps on the last row scroll three times", RowIs(g_console, 62U, "abcd"),
                   RowIs(g_console, 63U, "efgh"), RowIs(g_console, 64U, "ijkl"), RowIs(g_console, 65U, "mnop"));
    for (UInt32 erase{0U}; erase < 16U; ++erase)
    {
        Put(g_console, "\b \b");
    }
    ASTERIA_EXPECT(runner, "after the scrolls an erase crosses the mark carried below row 64",
                   RowIs(g_console, 62U, ""), RowIs(g_console, 63U, ""), g_console.Row() == 62U,
                   g_console.Column() == 0U);
}

void TestWriting(TestContext& runner) noexcept
{
    Start(10U, 4U);
    Put(g_console, "Hi\x01\xFE");
    ASTERIA_EXPECT(runner, "text lands in the cells as CP437 codes", g_console.CellAt(0U, 0U).Code == 'H',
                   g_console.CellAt(0U, 1U).Code == 'i', g_console.CellAt(0U, 2U).Code == 0x01U,
                   g_console.CellAt(0U, 3U).Code == 0xFEU, g_console.CellAt(0U, 4U).Code == 0x20U,
                   g_console.CellAt(0U, 0U).Attribute == 0U, g_console.Column() == 4U, g_console.Row() == 0U);
    Put(g_console, "\n");
    ASTERIA_EXPECT(runner, "a new line goes to column 0 of the next row", g_console.Column() == 0U,
                   g_console.Row() == 1U);
    Put(g_console, "abc\rX");
    ASTERIA_EXPECT(runner, "a carriage return overwrites from column 0", RowIs(g_console, 1U, "Xbc"),
                   g_console.Row() == 1U, g_console.Column() == 1U);
    Put(g_console, "\tY");
    ASTERIA_EXPECT(runner, "a tab goes to the next multiple of 8 without erasing", g_console.CellAt(1U, 1U).Code == 'b',
                   g_console.CellAt(1U, 8U).Code == 'Y', g_console.Column() == 9U);
    Put(g_console, "\t");
    ASTERIA_EXPECT(runner, "a tab at the end of the line stops at the line end", g_console.Column() == 10U,
                   g_console.Row() == 1U);
    Put(g_console, "\t");
    ASTERIA_EXPECT(runner, "a further tab stays", g_console.Column() == 10U);
    Put(g_console, "Z");
    ASTERIA_EXPECT(runner, "the next code after the end wraps", g_console.Row() == 2U,
                   g_console.CellAt(2U, 0U).Code == 'Z', g_console.Column() == 1U);
    Put(g_console, "\b\bQ");
    ASTERIA_EXPECT(runner, "backspace goes back one column without erasing, then into the row it wrapped from",
                   g_console.CellAt(2U, 0U).Code == 'Z', g_console.CellAt(1U, 9U).Code == 'Q', g_console.Row() == 1U,
                   g_console.Column() == 10U);
    Put(g_console, "\nS\b\b\bT");
    ASTERIA_EXPECT(runner, "backspace never goes before column 0 of a row a new line began",
                   g_console.CellAt(2U, 0U).Code == 'T', g_console.Row() == 2U, g_console.Column() == 1U);
}

void TestReverseWrap(TestContext& runner) noexcept
{
    Start(4U, 3U);
    Put(g_console, "abcdef");
    Put(g_console, "\b \b\b \b\b \b");
    ASTERIA_EXPECT(runner, "a line editor's erase crosses a wrap into the row above", RowIs(g_console, 0U, "abc"),
                   RowIs(g_console, 1U, ""), g_console.Row() == 0U, g_console.Column() == 3U);
    Put(g_console, "\nxy\b\b\b");
    ASTERIA_EXPECT(runner, "an erase never crosses a new line", RowIs(g_console, 1U, "xy"), g_console.Row() == 1U,
                   g_console.Column() == 0U);
    Start(4U, 3U);
    Put(g_console, "abcdefghijklmn");
    ASTERIA_EXPECT(runner, "a wrapped line scrolls with its continuation", RowIs(g_console, 0U, "efgh"),
                   RowIs(g_console, 1U, "ijkl"), RowIs(g_console, 2U, "mn"), g_console.Row() == 2U,
                   g_console.Column() == 2U);
    Put(g_console, "\b \b\b \b\b \b\b \b\b \b");
    ASTERIA_EXPECT(runner, "after a scroll an erase still crosses the wraps that remain on screen",
                   RowIs(g_console, 0U, "efgh"), RowIs(g_console, 1U, "i"), RowIs(g_console, 2U, ""),
                   g_console.Row() == 1U, g_console.Column() == 1U);
    Put(g_console, "\b \b\b \b\b \b\b \b\b \b\b \b");
    ASTERIA_EXPECT(runner, "the top row continues nothing once the row it wrapped from scrolled away",
                   RowIs(g_console, 0U, ""), RowIs(g_console, 1U, ""), g_console.Row() == 0U, g_console.Column() == 0U);
}

void TestWrapAndScroll(TestContext& runner) noexcept
{
    Start(4U, 3U);
    Put(g_console, "abcd");
    ASTERIA_EXPECT(runner, "filling a row defers the wrap", g_console.Row() == 0U, g_console.Column() == 4U);
    Put(g_console, "e");
    ASTERIA_EXPECT(runner, "the next code wraps to the next row", g_console.Row() == 1U, g_console.Column() == 1U,
                   RowIs(g_console, 0U, "abcd"), RowIs(g_console, 1U, "e"));
    Put(g_console, "fghijk");
    ASTERIA_EXPECT(runner, "a full grid does not scroll until another code arrives", g_console.Row() == 2U,
                   g_console.Column() == 3U, RowIs(g_console, 0U, "abcd"), RowIs(g_console, 1U, "efgh"),
                   RowIs(g_console, 2U, "ijk"));
    Put(g_console, "lm");
    ASTERIA_EXPECT(runner, "a code after the last cell scrolls the grid up one row", RowIs(g_console, 0U, "efgh"),
                   RowIs(g_console, 1U, "ijkl"), RowIs(g_console, 2U, "m"), g_console.Row() == 2U,
                   g_console.Column() == 1U);
    ASTERIA_EXPECT(runner, "scrolling damages every row", g_console.Damage().First == 0U, g_console.Damage().End == 3U);
    Put(g_console, "\n\n");
    ASTERIA_EXPECT(runner, "new lines at the last row scroll and clear the last row", RowIs(g_console, 0U, "m"),
                   RowIs(g_console, 1U, ""), RowIs(g_console, 2U, ""), g_console.Row() == 2U, g_console.Column() == 0U);
    g_console.Clear();
    ASTERIA_EXPECT(runner, "Clear blanks the grid, homes the cursor and damages all", RowIs(g_console, 0U, ""),
                   RowIs(g_console, 1U, ""), RowIs(g_console, 2U, ""), g_console.Row() == 0U, g_console.Column() == 0U,
                   g_console.Damage().First == 0U, g_console.Damage().End == 3U);
}

/// The expected pixel of cell (row, column) at pixel (x, y) within it.
[[nodiscard]] UInt32 ExpectedPixel(const PixelFormat format, const UInt32 code, const UInt32 x, const UInt32 y) noexcept
{
    bool set{false};
    if ((code >= 1U) && (code <= 0xFEU))
    {
        set = (User::Graphics::Bold8x16Glyphs[((code - 1U) * 16U) + y] & (0x80U >> x)) != 0U;
    }
    return User::Graphics::PackColor(format, set ? User::Console::ConsoleForeground : User::Console::ConsoleBackground);
}

constexpr UInt32 SurfaceWidth{48U};
constexpr UInt32 SurfaceHeight{64U};
constexpr UInt32 OriginX{4U};
constexpr UInt32 OriginY{3U};

void TestRender(TestContext& runner) noexcept
{
    // 4 columns x 3 rows at (4, 3) of a 48x64 surface with a stride of 50.
    static UInt32 pixels[50U * SurfaceHeight];
    for (UInt32& pixel : pixels)
    {
        pixel = Sentinel;
    }
    const Surface surface{
        .Pixels = pixels, .Width = SurfaceWidth, .Height = SurfaceHeight, .Stride = 50U, .Format = PixelFormat::Rgbx};
    const Point origin{.X = static_cast<Foundation::Int32>(OriginX), .Y = static_cast<Foundation::Int32>(OriginY)};
    Start(4U, 3U);
    Put(g_console, "A\xB1z\n \x02");
    g_console.Render(surface, origin);
    ASTERIA_EXPECT(runner, "a render clears the damage", !g_console.HasDamage(), g_console.Damage().End == 0U);
    bool exact{true};
    for (UInt32 y{0U}; y < SurfaceHeight; ++y)
    {
        for (UInt32 x{0U}; x < 50U; ++x)
        {
            UInt32 expected{Sentinel};
            if ((x >= OriginX) && (x < (OriginX + 32U)) && (y >= OriginY) && (y < (OriginY + 48U)))
            {
                const UInt32 column{(x - OriginX) / 8U};
                const UInt32 row{(y - OriginY) / 16U};
                const UInt32 code{g_console.CellAt(row, column).Code};
                expected = ExpectedPixel(PixelFormat::Rgbx, code, (x - OriginX) % 8U, (y - OriginY) % 16U);
            }
            exact = exact && (pixels[(y * 50U) + x] == expected);
        }
    }
    ASTERIA_EXPECT(runner, "a render is pixel-exact against the glyph table and leaves the margins", exact,
                   g_console.CellAt(0U, 0U).Code == 'A', g_console.CellAt(1U, 1U).Code == 0x02U);

    // Only the damaged rows are redrawn: poison everything, write on row 1, render.
    for (UInt32& pixel : pixels)
    {
        pixel = Sentinel;
    }
    Put(g_console, "\b\bQ");
    const bool damagedOne{g_console.Damage().First == 1U && g_console.Damage().End == 2U};
    g_console.Render(surface, origin);
    bool onlyRowOne{true};
    for (UInt32 y{0U}; y < SurfaceHeight; ++y)
    {
        for (UInt32 x{0U}; x < 50U; ++x)
        {
            const bool inRowOne{(x >= OriginX) && (x < (OriginX + 32U)) && (y >= (OriginY + 16U)) &&
                                (y < (OriginY + 32U))};
            UInt32 expected{Sentinel};
            if (inRowOne)
            {
                expected = ExpectedPixel(PixelFormat::Rgbx, g_console.CellAt(1U, (x - OriginX) / 8U).Code,
                                         (x - OriginX) % 8U, (y - OriginY) % 16U);
            }
            onlyRowOne = onlyRowOne && (pixels[(y * 50U) + x] == expected);
        }
    }
    ASTERIA_EXPECT(runner, "only the damaged row is redrawn", damagedOne, onlyRowOne, !g_console.HasDamage());

    // A second render with no damage writes nothing.
    for (UInt32& pixel : pixels)
    {
        pixel = Sentinel;
    }
    g_console.Render(surface, origin);
    bool none{true};
    for (const UInt32 pixel : pixels)
    {
        none = none && (pixel == Sentinel);
    }
    ASTERIA_EXPECT(runner, "a render without damage draws nothing", none);

    // A surface too small for the grid is clipped by the glyph drawing.
    g_console.Clear();
    Put(g_console, "AB");
    static UInt32 small[12U * 20U];
    for (UInt32& pixel : small)
    {
        pixel = Sentinel;
    }
    const Surface little{.Pixels = small, .Width = 12U, .Height = 20U, .Stride = 12U, .Format = PixelFormat::Bgrx};
    g_console.Render(little, Point{.X = 0, .Y = 0});
    bool clipped{true};
    for (UInt32 y{0U}; y < 20U; ++y)
    {
        for (UInt32 x{0U}; x < 12U; ++x)
        {
            clipped =
                clipped && (small[(y * 12U) + x] ==
                            ExpectedPixel(PixelFormat::Bgrx, g_console.CellAt(y / 16U, x / 8U).Code, x % 8U, y % 16U));
        }
    }
    ASTERIA_EXPECT(runner, "a render onto a smaller surface is clipped exactly", clipped);
}

/// A tiny reference model of the cell grid (a flat array, linear positions).
struct Reference
{
    UInt32 Columns;
    UInt32 Rows;
    UInt8 Codes[160U * 50U];
    UInt32 Position; // Row * Columns + column, column may equal Columns only via Pending
    bool Pending;
    bool Continues[50U]; // The row was entered by a wrap from the row above

    void Reset(const UInt32 columns, const UInt32 rows) noexcept
    {
        Columns = columns;
        Rows = rows;
        for (UInt8& code : Codes)
        {
            code = 0x20U;
        }
        Position = 0U;
        Pending = false;
        for (bool& continues : Continues)
        {
            continues = false;
        }
    }

    void LineFeed(const bool wrapped) noexcept
    {
        Pending = false;
        Position = (Position / Columns) * Columns;
        if ((Position / Columns) == (Rows - 1U))
        {
            for (UInt32 index{0U}; index < (Columns * (Rows - 1U)); ++index)
            {
                Codes[index] = Codes[index + Columns];
            }
            for (UInt32 row{1U}; row < Rows; ++row)
            {
                Continues[row - 1U] = Continues[row];
            }
            Continues[0U] = false;
            for (UInt32 index{Columns * (Rows - 1U)}; index < (Columns * Rows); ++index)
            {
                Codes[index] = 0x20U;
            }
        }
        else
        {
            Position += Columns;
        }
        Continues[Position / Columns] = wrapped;
    }

    [[nodiscard]] UInt32 Col() const noexcept
    {
        return (Position % Columns) + (Pending ? 1U : 0U);
    }

    void Feed(const UInt8 byte) noexcept
    {
        const UInt32 column{Position % Columns};
        if (byte == '\n')
        {
            LineFeed(false);
        }
        else if (byte == '\r')
        {
            Position -= column;
            Pending = false;
        }
        else if (byte == '\t')
        {
            const UInt32 now{Col()};
            const UInt32 target{(now - (now % 8U)) + 8U};
            if (target >= Columns)
            {
                Position += (Columns - 1U) - column;
                Pending = true;
            }
            else
            {
                Position += target - column;
                Pending = false;
            }
        }
        else if (byte == '\b')
        {
            if (Pending)
            {
                Pending = false;
            }
            else if (column != 0U)
            {
                --Position;
            }
            else if ((Position != 0U) && Continues[Position / Columns])
            {
                --Position; // The last column of the row this one wrapped from
            }
        }
        else
        {
            if (Pending)
            {
                LineFeed(true);
            }
            Codes[Position] = byte;
            if (((Position % Columns) + 1U) == Columns)
            {
                Pending = true;
            }
            else
            {
                ++Position;
            }
        }
    }
};

// Pending cursor representation: with Pending the console's Column is Columns,
// which the reference reports as the last column plus one. A backspace then
// returns the console to Columns - 1, the reference's last column.
static Reference g_reference;

void TestRandomised(TestContext& runner) noexcept
{
    constexpr UInt32 Columns{23U};
    constexpr UInt32 Rows{7U};
    Start(Columns, Rows);
    g_reference.Reset(Columns, Rows);
    static UInt32 pixels[Columns * 8U * Rows * 16U];
    for (UInt32& pixel : pixels)
    {
        pixel = Sentinel;
    }
    const Surface surface{.Pixels = pixels,
                          .Width = Columns * 8U,
                          .Height = Rows * 16U,
                          .Stride = Columns * 8U,
                          .Format = PixelFormat::Bgrx};
    UInt32 seed{12345U};
    bool gridSame{true};
    bool cursorSame{true};
    bool damageCovers{true};
    bool renderSame{true};
    UInt32 scrolls{0U};
    UInt32 renders{0U};
    UInt32 oldRows[Rows]{};
    for (UInt32 round{0U}; round < 4000U; ++round)
    {
        seed = (seed * 1664525U) + 1013904223U;
        const UInt32 kind{(seed >> 24U) % 16U};
        UInt8 chunk[40U]{};
        UInt32 length{1U + ((seed >> 8U) % 39U)};
        if (kind == 0U)
        {
            length = 1U;
        }
        UInt32 inner{seed};
        for (UInt32 index{0U}; index < length; ++index)
        {
            inner = (inner * 1103515245U) + 12345U;
            const UInt32 pick{(inner >> 16U) % 40U};
            UInt8 byte{static_cast<UInt8>('a' + (pick % 26U))};
            if (pick == 26U)
            {
                byte = '\n';
            }
            else if (pick == 27U)
            {
                byte = '\r';
            }
            else if (pick == 28U)
            {
                byte = '\t';
            }
            else if (pick == 29U)
            {
                byte = '\b';
            }
            else if (pick == 30U)
            {
                byte = static_cast<UInt8>((inner >> 8U) & 0xFFU);
            }
            chunk[index] = byte;
        }
        // Snapshot rows to check the damage covers every row that changed.
        for (UInt32 row{0U}; row < Rows; ++row)
        {
            UInt32 sum{0U};
            for (UInt32 column{0U}; column < Columns; ++column)
            {
                sum = (sum * 31U) + g_console.CellAt(row, column).Code;
            }
            oldRows[row] = sum;
        }
        g_console.Write(chunk, length);
        for (UInt32 index{0U}; index < length; ++index)
        {
            g_reference.Feed(chunk[index]);
        }
        for (UInt32 row{0U}; row < Rows; ++row)
        {
            UInt32 sum{0U};
            for (UInt32 column{0U}; column < Columns; ++column)
            {
                const UInt32 code{g_console.CellAt(row, column).Code};
                sum = (sum * 31U) + code;
                gridSame = gridSame && (code == g_reference.Codes[(row * Columns) + column]);
            }
            if (sum != oldRows[row])
            {
                const auto damage{g_console.Damage()};
                damageCovers = damageCovers && (row >= damage.First) && (row < damage.End);
            }
        }
        cursorSame = cursorSame && (g_console.Row() == (g_reference.Position / Columns)) &&
                     (g_console.Column() == g_reference.Col());
        if ((round % 9U) == 0U)
        {
            g_console.Render(surface, Point{.X = 0, .Y = 0});
            ++renders;
            // The surface is never poisoned: each render must leave it equal
            // to the whole reference grid, so rows it skipped were still right.
            for (UInt32 y{0U}; y < (Rows * 16U); ++y)
            {
                for (UInt32 x{0U}; x < (Columns * 8U); ++x)
                {
                    renderSame = renderSame &&
                                 (pixels[(y * Columns * 8U) + x] ==
                                  ExpectedPixel(PixelFormat::Bgrx, g_reference.Codes[((y / 16U) * Columns) + (x / 8U)],
                                                x % 8U, y % 16U));
                }
            }
        }
        scrolls += (g_console.Row() == (Rows - 1U)) ? 1U : 0U;
    }
    ASTERIA_EXPECT(runner, "4000 random writes keep the cell grid equal to the reference model", gridSame,
                   scrolls > 100U);
    ASTERIA_EXPECT(runner, "the cursor follows the reference model", cursorSame);
    ASTERIA_EXPECT(runner, "every changed row lies in the damage range", damageCovers);
    ASTERIA_EXPECT(runner, "every incremental render is pixel-exact against the reference", renderSame, renders > 100U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"TextConsoleTests"};
    Asteria::Test::TestConfigure(runner);
    Asteria::Test::TestWriting(runner);
    Asteria::Test::TestWrapAndScroll(runner);
    Asteria::Test::TestReverseWrap(runner);
    Asteria::Test::TestRender(runner);
    Asteria::Test::TestRandomised(runner);
    Asteria::Test::TestManyRows(runner);
    return runner.Finish();
}
