// SPDX-License-Identifier: Apache-2.0
#include "User/Terminal/TerminalRenderer.hpp"

#include "User/Graphics/Bold8x16Font.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Terminal/Terminal.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Pixel-exact rendering: the colour rules (default colours, bold as bright,
// reverse, the cursor block), glyphs, the underline row, origins and
// clipping, the damaged-rows render and both pixel formats. Expected pixels
// are computed here from the font bitmap and literal colours.
namespace Asteria::Test
{
namespace
{
using Foundation::Int32;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;
using User::Graphics::Color;
using User::Graphics::MemorySurface;
using User::Graphics::PixelFormat;
using User::Graphics::Point;
using User::Terminal::ResolveCellColors;
using User::Terminal::Terminal;
using User::Terminal::TerminalCellView;
using User::Terminal::TerminalCellColors;
using User::Terminal::TerminalDefaultColor;

// The default pair and some palette entries.
constexpr Color White{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
constexpr Color Ink{.Red = 0x10U, .Green = 0x20U, .Blue = 0x40U};
constexpr Color Red{.Red = 0xB0U, .Green = 0x18U, .Blue = 0x18U};
constexpr Color BrightRed{.Red = 0xF0U, .Green = 0x48U, .Blue = 0x48U};
constexpr Color Green{.Red = 0x18U, .Green = 0xA0U, .Blue = 0x18U};
constexpr Color BrightGreen{.Red = 0x48U, .Green = 0xE0U, .Blue = 0x48U};
constexpr Color Blue{.Red = 0x18U, .Green = 0x30U, .Blue = 0xB0U};
constexpr Color BrightBlack{.Red = 0x60U, .Green = 0x60U, .Blue = 0x60U};
constexpr UInt32 Sentinel{0xDEAD'BEEFU};
constexpr UInt8 Default{TerminalDefaultColor};
constexpr UInt8 Bold{User::Terminal::TerminalBold};
constexpr UInt8 Underline{User::Terminal::TerminalUnderline};
constexpr UInt8 Reverse{User::Terminal::TerminalReverse};

[[nodiscard]] bool Same(const Color first, const Color second) noexcept
{
    return (first.Red == second.Red) && (first.Green == second.Green) && (first.Blue == second.Blue);
}

struct ColorCase
{
    const char* Name;
    TerminalCellView Cell;
    bool Cursor;
    Color Foreground;
    Color Background;
};

constexpr ColorCase ColorCases[]{
    {"the default pair", {'a', 0U, Default, Default}, false, White, Ink},
    {"bold leaves the default foreground", {'a', Bold, Default, Default}, false, White, Ink},
    {"bold brightens 0-7", {'a', Bold, 1U, 4U}, false, BrightRed, Blue},
    {"bold keeps 8-15", {'a', Bold, 8U, Default}, false, BrightBlack, Ink},
    {"bold keeps 9", {'a', Bold, 9U, Default}, false, BrightRed, Ink},
    {"palette indexes", {'a', 0U, 1U, 10U}, false, Red, BrightGreen},
    {"reverse swaps", {'a', Reverse, Default, Default}, false, Ink, White},
    {"the cursor swaps", {'a', 0U, 1U, 4U}, true, Blue, Red},
    {"the cursor on reverse swaps back", {'a', Reverse, 1U, 4U}, true, Red, Blue},
    {"reverse after bold", {'a', Bold | Reverse, 2U, Default}, false, Ink, BrightGreen},
    {"an index above 15 is the default", {'a', 0U, 0x20U, 0x30U}, false, White, Ink},
    {"underline does not change colours", {'a', Underline, 1U, 4U}, false, Red, Blue},
};

void TestColors(TestContext& runner) noexcept
{
    for (const ColorCase& test : ColorCases)
    {
        const TerminalCellColors colors{ResolveCellColors(test.Cell, test.Cursor)};
        ASTERIA_EXPECT(runner, test.Name, Same(colors.Foreground, test.Foreground),
                       Same(colors.Background, test.Background));
    }
    ASTERIA_EXPECT(runner, "the cell and underline geometry", User::Terminal::TerminalCellWidth == 8U,
                   User::Terminal::TerminalCellHeight == 16U, User::Terminal::TerminalUnderlineRow == 13U);
}

/// What one cell looks like: its code, colours and underline.
struct CellLook
{
    UInt8 Code;
    Color Foreground;
    Color Background;
    bool Underlined;
};

[[nodiscard]] UInt32 GlyphBits(const UInt8 code, const UInt32 row) noexcept
{
    if ((code < User::Graphics::Bold8x16FirstCode) ||
        (code >= (User::Graphics::Bold8x16FirstCode + User::Graphics::Bold8x16Count)))
    {
        return 0U;
    }
    return User::Graphics::Bold8x16Glyphs[((code - User::Graphics::Bold8x16FirstCode) * 16U) + row];
}

[[nodiscard]] UInt32 ExpectedPixel(const CellLook& look, const UInt32 x, const UInt32 y,
                                   const PixelFormat format) noexcept
{
    const bool set{(look.Underlined && (y == 13U)) || (((GlyphBits(look.Code, y) >> (7U - x)) & 1U) != 0U)};
    return User::Graphics::PackColor(format, set ? look.Foreground : look.Background);
}

constexpr UInt32 SurfaceWidth{48U};
constexpr UInt32 SurfaceHeight{48U};
constexpr UInt32 SurfaceStride{52U};
UInt32 g_pixels[SurfaceStride * SurfaceHeight];
UInt32 g_reference[SurfaceStride * SurfaceHeight];
Terminal g_terminal;

[[nodiscard]] MemorySurface Clear(UInt32* const pixels, const UInt32 width, const UInt32 height,
                                  const PixelFormat format) noexcept
{
    for (Size index{0U}; index < (SurfaceStride * SurfaceHeight); ++index)
    {
        pixels[index] = Sentinel;
    }
    return MemorySurface{.Pixels = pixels, .Width = width, .Height = height, .Stride = SurfaceStride, .Format = format};
}

void Write(const char* const text, const Size length) noexcept
{
    g_terminal.Write(reinterpret_cast<const UInt8*>(text), length);
}

#define ASTERIA_WRITE(text) Write(text, sizeof(text) - 1U)

/// Compares the cells at origin (0, 0) with looks (row-major, columns per
/// row) and checks everything beyond the grid is untouched.
[[nodiscard]] bool GridDrawn(const UInt32* const pixels, const CellLook* const looks, const UInt32 columns,
                             const UInt32 rows, const PixelFormat format) noexcept
{
    for (UInt32 y{0U}; y < SurfaceHeight; ++y)
    {
        for (UInt32 x{0U}; x < SurfaceStride; ++x)
        {
            const UInt32 actual{pixels[(y * SurfaceStride) + x]};
            const bool inside{(x < (columns * 8U)) && (y < (rows * 16U)) && (x < SurfaceWidth)};
            const UInt32 expected{
                inside ? ExpectedPixel(looks[((y >> 4U) * columns) + (x >> 3U)], x & 7U, y & 15U, format) : Sentinel};
            if (actual != expected)
            {
                return false;
            }
        }
    }
    return true;
}

void TestCells(TestContext& runner) noexcept
{
    g_terminal.Configure(4U, 2U);
    ASTERIA_WRITE("A\x1b[1;4;31;44mg\x1b[0;7m \x1b[0;32m\xe2\x95\x94\r\n\x1b[4m_");
    // SGR 32 still applies to the underscore; the cursor sits at (1, 1) after it.
    const CellLook looks[8U]{
        {'A', White, Ink, false}, {'g', BrightRed, Blue, true}, {' ', Ink, White, false}, {0xC9U, Green, Ink, false},
        {'_', Green, Ink, true},  {' ', Ink, White, false},     {' ', White, Ink, false}, {' ', White, Ink, false},
    };
    const MemorySurface bgrx{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), bgrx, Point{.X = 0, .Y = 0});
    ASTERIA_EXPECT(runner, "glyphs, colours, underline and the cursor block (BGRX)",
                   GridDrawn(g_pixels, looks, 4U, 2U, PixelFormat::Bgrx));
    const MemorySurface rgbx{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Rgbx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), rgbx, Point{.X = 0, .Y = 0});
    ASTERIA_EXPECT(runner, "the same in RGBX", GridDrawn(g_pixels, looks, 4U, 2U, PixelFormat::Rgbx),
                   g_pixels[(5U * SurfaceStride) + 9U] != Sentinel);

    ASTERIA_WRITE("\x1b[?25l");
    CellLook hidden[8U]{};
    for (Size index{0U}; index < 8U; ++index)
    {
        hidden[index] = looks[index];
    }
    hidden[5U] = CellLook{' ', White, Ink, false};
    const MemorySurface again{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), again, Point{.X = 0, .Y = 0});
    ASTERIA_EXPECT(runner, "a hidden cursor is not drawn", GridDrawn(g_pixels, hidden, 4U, 2U, PixelFormat::Bgrx));

    // A pending wrap keeps the cursor block on the last column.
    g_terminal.Configure(2U, 1U);
    ASTERIA_WRITE("ab");
    const CellLook wrapped[2U]{{'a', White, Ink, false}, {'b', Ink, White, false}};
    const MemorySurface last{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), last, Point{.X = 0, .Y = 0});
    ASTERIA_EXPECT(runner, "the cursor block on a pending wrap",
                   GridDrawn(g_pixels, wrapped, 2U, 1U, PixelFormat::Bgrx));
}

void TestOrigins(TestContext& runner) noexcept
{
    g_terminal.Configure(5U, 3U);
    ASTERIA_WRITE("Hello\x1b[1;33mWorld\x1b[0;7mabc");
    const MemorySurface reference{Clear(g_reference, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), reference, Point{.X = 0, .Y = 0});

    const MemorySurface shifted{Clear(g_pixels, 30U, 30U, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), shifted, Point{.X = -5, .Y = -9});
    bool match{true};
    for (UInt32 y{0U}; y < SurfaceHeight; ++y)
    {
        for (UInt32 x{0U}; x < SurfaceStride; ++x)
        {
            const bool inside{(x < 30U) && (y < 30U) && ((x + 5U) < 40U) && ((y + 9U) < 48U)};
            const UInt32 expected{inside ? g_reference[((y + 9U) * SurfaceStride) + x + 5U] : Sentinel};
            match = match && (g_pixels[(y * SurfaceStride) + x] == expected);
        }
    }
    ASTERIA_EXPECT(runner, "a negative origin is clipped", match);

    const MemorySurface offset{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), offset, Point{.X = 13, .Y = 7});
    match = true;
    for (UInt32 y{0U}; y < SurfaceHeight; ++y)
    {
        for (UInt32 x{0U}; x < SurfaceStride; ++x)
        {
            const bool inside{(x >= 13U) && (x < SurfaceWidth) && (y >= 7U) && ((x - 13U) < 40U)};
            const UInt32 expected{inside ? g_reference[((y - 7U) * SurfaceStride) + (x - 13U)] : Sentinel};
            match = match && (g_pixels[(y * SurfaceStride) + x] == expected);
        }
    }
    ASTERIA_EXPECT(runner, "a positive origin is clipped at the far edges", match);

    bool untouched{true};
    const MemorySurface far{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminal(g_terminal.Screen(), far, Point{.X = 48, .Y = 0});
    User::Terminal::RenderTerminal(g_terminal.Screen(), far, Point{.X = 0, .Y = 48});
    User::Terminal::RenderTerminal(g_terminal.Screen(), far, Point{.X = -40, .Y = 0});
    User::Terminal::RenderTerminal(g_terminal.Screen(), far, Point{.X = 0, .Y = -48});
    User::Terminal::RenderTerminal(g_terminal.Screen(), far, Point{.X = -0x7FFF'FFFF, .Y = 0x7FFF'FFFF});
    const MemorySurface unusable{
        .Pixels = g_pixels, .Width = 8U, .Height = 8U, .Stride = 4U, .Format = PixelFormat::Bgrx};
    User::Terminal::RenderTerminal(g_terminal.Screen(), unusable, Point{.X = 0, .Y = 0});
    User::Terminal::RenderTerminalDamage(g_terminal.Screen(), unusable, Point{.X = 0, .Y = 0});
    for (const UInt32 pixel : g_pixels)
    {
        untouched = untouched && (pixel == Sentinel);
    }
    ASTERIA_EXPECT(runner, "a grid entirely outside, or an unusable surface, draws nothing", untouched);
}

void TestDamageRender(TestContext& runner) noexcept
{
    g_terminal.Configure(4U, 3U);
    ASTERIA_WRITE("ab\r\ncd\r\nef");
    const MemorySurface reference{Clear(g_reference, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    g_terminal.ClearDamage();
    ASTERIA_WRITE("\x1b[1;1HX");
    User::Terminal::RenderTerminal(g_terminal.Screen(), reference, Point{.X = 0, .Y = 0});
    const MemorySurface damaged{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminalDamage(g_terminal.Screen(), damaged, Point{.X = 0, .Y = 0});
    bool match{true};
    for (UInt32 y{0U}; y < SurfaceHeight; ++y)
    {
        for (UInt32 x{0U}; x < SurfaceStride; ++x)
        {
            // Rows 0 (the X) and 2 (the cursor left it) are drawn; row 1 not.
            const bool drawn{(x < 32U) && ((y < 16U) || ((y >= 32U) && (y < 48U)))};
            const UInt32 expected{drawn ? g_reference[(y * SurfaceStride) + x] : Sentinel};
            match = match && (g_pixels[(y * SurfaceStride) + x] == expected);
        }
    }
    ASTERIA_EXPECT(runner, "the damaged-rows render draws exactly the damaged rows", match,
                   g_terminal.Screen().IsRowDamaged(0U), !g_terminal.Screen().IsRowDamaged(1U),
                   g_terminal.Screen().IsRowDamaged(2U));
    g_terminal.ClearDamage();
    const MemorySurface clean{Clear(g_pixels, SurfaceWidth, SurfaceHeight, PixelFormat::Bgrx)};
    User::Terminal::RenderTerminalDamage(g_terminal.Screen(), clean, Point{.X = 0, .Y = 0});
    bool untouched{true};
    for (const UInt32 pixel : g_pixels)
    {
        untouched = untouched && (pixel == Sentinel);
    }
    ASTERIA_EXPECT(runner, "no damage draws nothing", untouched);
}
void TestGridForOutput(TestContext& runner) noexcept
{
    using User::Terminal::TerminalGridForOutput;
    struct GridCase
    {
        UInt32 Width;
        UInt32 Height;
        UInt32 Columns;
        UInt32 Rows;
    };
    // Desktop session: the frame is the content plus 8 by 29.
    constexpr GridCase Cases[]{
        {640U, 400U, 79U, 23U}, {1920U, 1080U, 80U, 25U}, {648U, 429U, 80U, 25U},
        {647U, 428U, 79U, 24U}, {655U, 444U, 80U, 25U},   {800U, 600U, 80U, 25U},
        {320U, 200U, 39U, 10U}, {16U, 45U, 1U, 1U},       {15U, 44U, 1U, 1U},
        {0U, 0U, 1U, 1U},       {8U, 29U, 1U, 1U},        {0xFFFF'FFFFU, 0xFFFF'FFFFU, 80U, 25U},
    };
    for (const GridCase& test : Cases)
    {
        const User::Terminal::TerminalGrid grid{TerminalGridForOutput(test.Width, test.Height)};
        ASTERIA_EXPECT(runner, "the grid for an output", grid.Columns == test.Columns, grid.Rows == test.Rows);
    }
    static_assert(TerminalGridForOutput(640U, 400U).Columns == 79U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"TerminalRendererTests"};
    Asteria::Test::TestColors(runner);
    Asteria::Test::TestCells(runner);
    Asteria::Test::TestOrigins(runner);
    Asteria::Test::TestDamageRender(runner);
    Asteria::Test::TestGridForOutput(runner);
    return runner.Finish();
}
