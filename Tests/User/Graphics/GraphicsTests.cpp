// SPDX-License-Identifier: Apache-2.0
#include "User/Graphics/BitmapFont.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Int32;
using Foundation::Size;
using Foundation::UInt32;
using User::Graphics::Color;
using User::Graphics::PixelFormat;
using User::Graphics::Point;
using User::Graphics::Rect;
using User::Graphics::Surface;

constexpr Color Red{.Red = 0xFFU, .Green = 0U, .Blue = 0U};
constexpr Color Cyan{.Red = 0x11U, .Green = 0x22U, .Blue = 0x33U};
constexpr Color Navy{.Red = 0x10U, .Green = 0x20U, .Blue = 0x40U};
constexpr Color White{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
constexpr UInt32 Sentinel{0xDEADBEEFU};

constexpr UInt32 MaximumPixels{64U * 64U};

[[nodiscard]] Surface MakeSurface(UInt32* const pixels, const UInt32 width, const UInt32 height, const UInt32 stride,
                                  const PixelFormat format = PixelFormat::Bgrx) noexcept
{
    for (UInt32 index{0U}; index < (stride * height); ++index)
    {
        pixels[index] = Sentinel;
    }
    return Surface{.Pixels = pixels, .Width = width, .Height = height, .Stride = stride, .Format = format};
}

[[nodiscard]] UInt32 At(const UInt32* const pixels, const UInt32 stride, const UInt32 x, const UInt32 y) noexcept
{
    return pixels[(y * stride) + x];
}

void TestPixelFormats(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "the format values match the boot protocol", static_cast<UInt32>(PixelFormat::Rgbx) == 1U,
                   static_cast<UInt32>(PixelFormat::Bgrx) == 2U);
    ASTERIA_EXPECT(runner, "RGBX stores red in the low byte (byte 0 in memory)",
                   User::Graphics::PackColor(PixelFormat::Rgbx, Cyan) == 0x00332211U,
                   User::Graphics::PackColor(PixelFormat::Rgbx, Red) == 0x000000FFU);
    ASTERIA_EXPECT(runner, "BGRX stores blue in the low byte",
                   User::Graphics::PackColor(PixelFormat::Bgrx, Cyan) == 0x00112233U,
                   User::Graphics::PackColor(PixelFormat::Bgrx, Red) == 0x00FF0000U);
    ASTERIA_EXPECT(runner, "an invalid format packs zero and is not valid",
                   User::Graphics::PackColor(PixelFormat::Invalid, White) == 0U,
                   User::Graphics::PackColor(static_cast<PixelFormat>(7U), White) == 0U,
                   !User::Graphics::IsValidPixelFormat(PixelFormat::Invalid),
                   User::Graphics::IsValidPixelFormat(PixelFormat::Rgbx),
                   User::Graphics::IsValidPixelFormat(PixelFormat::Bgrx));
    ASTERIA_EXPECT(runner, "the packed fourth byte is zero",
                   (User::Graphics::PackColor(PixelFormat::Rgbx, White) >> 24U) == 0U,
                   (User::Graphics::PackColor(PixelFormat::Bgrx, White) >> 24U) == 0U);
}

void TestClipping(TestContext& runner) noexcept
{
    UInt32 pixels[16U * 8U];
    const Surface surface{MakeSurface(pixels, 16U, 8U, 16U)};
    const Rect inside{User::Graphics::ClipToSurface(surface, Rect{.X = 2, .Y = 3, .Width = 4U, .Height = 2U})};
    ASTERIA_EXPECT(runner, "a rectangle inside is unchanged", inside.X == 2, inside.Y == 3, inside.Width == 4U,
                   inside.Height == 2U);
    const Rect corner{User::Graphics::ClipToSurface(surface, Rect{.X = -3, .Y = -2, .Width = 5U, .Height = 4U})};
    ASTERIA_EXPECT(runner, "the top-left overhang is cut", corner.X == 0, corner.Y == 0, corner.Width == 2U,
                   corner.Height == 2U);
    const Rect far{User::Graphics::ClipToSurface(surface, Rect{.X = 14, .Y = 6, .Width = 100U, .Height = 100U})};
    ASTERIA_EXPECT(runner, "the bottom-right overhang is cut", far.X == 14, far.Y == 6, far.Width == 2U,
                   far.Height == 2U);
    ASTERIA_EXPECT(runner, "disjoint rectangles clip to empty",
                   User::Graphics::IsEmpty(
                       User::Graphics::ClipToSurface(surface, Rect{.X = 16, .Y = 0, .Width = 4U, .Height = 4U})),
                   User::Graphics::IsEmpty(
                       User::Graphics::ClipToSurface(surface, Rect{.X = -4, .Y = 0, .Width = 4U, .Height = 4U})),
                   User::Graphics::IsEmpty(
                       User::Graphics::ClipToSurface(surface, Rect{.X = 0, .Y = 8, .Width = 4U, .Height = 4U})),
                   User::Graphics::IsEmpty(
                       User::Graphics::ClipToSurface(surface, Rect{.X = 0, .Y = 0, .Width = 0U, .Height = 4U})));
    const Rect huge{User::Graphics::Intersect(Rect{.X = -2147483647, .Y = 0, .Width = 0xFFFFFFFFU, .Height = 3U},
                                              Rect{.X = 2147483647, .Y = 1, .Width = 0xFFFFFFFFU, .Height = 10U})};
    ASTERIA_EXPECT(runner, "extreme extents do not overflow", huge.X == 2147483647, huge.Y == 1, huge.Width == 1U,
                   huge.Height == 2U);
    Surface bad{surface};
    bad.Stride = 8U;
    Surface noFormat{surface};
    noFormat.Format = PixelFormat::Invalid;
    Surface noPixels{surface};
    noPixels.Pixels = nullptr;
    ASTERIA_EXPECT(runner, "unusable surfaces are recognised", User::Graphics::IsUsable(surface),
                   !User::Graphics::IsUsable(bad), !User::Graphics::IsUsable(noFormat),
                   !User::Graphics::IsUsable(noPixels));
}

void TestFill(TestContext& runner) noexcept
{
    UInt32 pixels[16U * 8U];
    const Surface surface{MakeSurface(pixels, 12U, 8U, 16U, PixelFormat::Rgbx)};
    User::Graphics::FillRect(surface, Rect{.X = 2, .Y = 1, .Width = 3U, .Height = 2U}, Red);
    bool exact{true};
    for (UInt32 y{0U}; y < 8U; ++y)
    {
        for (UInt32 x{0U}; x < 16U; ++x)
        {
            const bool in{(x >= 2U) && (x < 5U) && (y >= 1U) && (y < 3U)};
            exact = exact && (At(pixels, 16U, x, y) == (in ? 0xFFU : Sentinel));
        }
    }
    ASTERIA_EXPECT(runner, "a fill writes exactly its rectangle", exact);

    // Clipped at every edge; the stride padding (columns 12-15) is never written.
    User::Graphics::FillRect(surface, Rect{.X = -5, .Y = -5, .Width = 1000U, .Height = 1000U}, Cyan);
    bool all{true};
    for (UInt32 y{0U}; y < 8U; ++y)
    {
        for (UInt32 x{0U}; x < 16U; ++x)
        {
            all = all && (At(pixels, 16U, x, y) == ((x < 12U) ? 0x00332211U : Sentinel));
        }
    }
    ASTERIA_EXPECT(runner, "an oversize fill covers the surface and not the stride padding", all);

    UInt32 other[4U];
    const Surface unusable{MakeSurface(other, 2U, 2U, 1U)};
    User::Graphics::FillRect(unusable, Rect{.X = 0, .Y = 0, .Width = 2U, .Height = 2U}, White);
    ASTERIA_EXPECT(runner, "an unusable surface is untouched", other[0U] == Sentinel, other[1U] == Sentinel);
}

/// Fills a 16x8 surface with distinct values: x + 100 * y.
void Number(UInt32* const pixels) noexcept
{
    for (UInt32 y{0U}; y < 8U; ++y)
    {
        for (UInt32 x{0U}; x < 16U; ++x)
        {
            pixels[(y * 16U) + x] = x + (100U * y);
        }
    }
}

[[nodiscard]] bool OverlapMatches(const Int32 fromX, const Int32 fromY, const Int32 toX, const Int32 toY) noexcept
{
    UInt32 pixels[16U * 8U];
    UInt32 expected[16U * 8U];
    Number(pixels);
    Number(expected);
    const Surface surface{.Pixels = pixels, .Width = 16U, .Height = 8U, .Stride = 16U, .Format = PixelFormat::Bgrx};
    constexpr UInt32 Width{7U};
    constexpr UInt32 Height{5U};
    // Reference: snapshot the source rectangle, then store it at the target.
    UInt32 snapshot[Width * Height];
    for (UInt32 y{0U}; y < Height; ++y)
    {
        for (UInt32 x{0U}; x < Width; ++x)
        {
            snapshot[(y * Width) + x] =
                pixels[((static_cast<UInt32>(fromY) + y) * 16U) + static_cast<UInt32>(fromX) + x];
        }
    }
    for (UInt32 y{0U}; y < Height; ++y)
    {
        for (UInt32 x{0U}; x < Width; ++x)
        {
            expected[((static_cast<UInt32>(toY) + y) * 16U) + static_cast<UInt32>(toX) + x] = snapshot[(y * Width) + x];
        }
    }
    const bool ok{User::Graphics::CopyRect(
        surface, surface, Rect{.X = fromX, .Y = fromY, .Width = Width, .Height = Height}, Point{.X = toX, .Y = toY})};
    for (UInt32 index{0U}; index < (16U * 8U); ++index)
    {
        if (pixels[index] != expected[index])
        {
            return false;
        }
    }
    return ok;
}

void TestCopy(TestContext& runner) noexcept
{
    UInt32 alias[32U]{};
    for (UInt32 i{0U}; i < 32U; ++i)
    {
        alias[i] = i;
    }
    const User::Graphics::MemorySurface narrow{alias, 2U, 3U, 2U, PixelFormat::Bgrx};
    const User::Graphics::MemorySurface wide{alias, 2U, 3U, 3U, PixelFormat::Bgrx};
    ASTERIA_EXPECT(runner, "overlapping unequal strides are refused before writing",
                   !User::Graphics::CopyRect(wide, narrow, {0, 0, 2U, 3U}, {0, 0}));
    bool unchanged{true};
    for (UInt32 i{0U}; i < 32U; ++i)
    {
        unchanged = unchanged && alias[i] == i;
    }
    ASTERIA_EXPECT(runner, "refused alias copy leaves all pixels intact", unchanged);
    ASTERIA_EXPECT(runner, "an overlapping copy moving down and right matches a snapshot copy",
                   OverlapMatches(1, 0, 3, 2), OverlapMatches(2, 1, 3, 1), OverlapMatches(2, 1, 2, 2));
    ASTERIA_EXPECT(runner, "an overlapping copy moving up and left matches a snapshot copy", OverlapMatches(3, 2, 1, 0),
                   OverlapMatches(3, 1, 2, 1), OverlapMatches(2, 2, 2, 1));
    ASTERIA_EXPECT(runner, "overlapping copies in mixed directions match a snapshot copy", OverlapMatches(1, 2, 3, 0),
                   OverlapMatches(3, 0, 1, 2), OverlapMatches(0, 0, 0, 0));

    UInt32 sourcePixels[16U * 8U];
    Number(sourcePixels);
    UInt32 destinationPixels[10U * 6U];
    const Surface source{
        .Pixels = sourcePixels, .Width = 16U, .Height = 8U, .Stride = 16U, .Format = PixelFormat::Bgrx};
    const Surface destination{MakeSurface(destinationPixels, 8U, 6U, 10U)};
    ASTERIA_EXPECT(runner, "a copy between surfaces of different strides succeeds",
                   User::Graphics::CopyRect(destination, source, Rect{.X = 4, .Y = 2, .Width = 3U, .Height = 2U},
                                            Point{.X = 1, .Y = 1}));
    bool exact{true};
    for (UInt32 y{0U}; y < 6U; ++y)
    {
        for (UInt32 x{0U}; x < 10U; ++x)
        {
            const bool in{(x >= 1U) && (x < 4U) && (y >= 1U) && (y < 3U)};
            exact = exact && (At(destinationPixels, 10U, x, y) == (in ? ((x + 3U) + (100U * (y + 1U))) : Sentinel));
        }
    }
    ASTERIA_EXPECT(runner, "the copied pixels land where asked and nothing else changes", exact);

    // Clipped on both sides: source (12..19, 6..9) shows 4x2 pixels; destination at (-2,-1).
    const Surface clipped{MakeSurface(destinationPixels, 8U, 6U, 10U)};
    ASTERIA_EXPECT(runner, "a copy clipped on both surfaces succeeds",
                   User::Graphics::CopyRect(clipped, source, Rect{.X = 12, .Y = 6, .Width = 8U, .Height = 4U},
                                            Point{.X = -2, .Y = -1}));
    bool clippedExact{true};
    for (UInt32 y{0U}; y < 6U; ++y)
    {
        for (UInt32 x{0U}; x < 10U; ++x)
        {
            // Destination (x, y) shows source (x + 14, y + 7) for x in 0..1, y in 0..0.
            const bool in{(x < 2U) && (y < 1U)};
            clippedExact =
                clippedExact && (At(destinationPixels, 10U, x, y) == (in ? ((x + 14U) + (100U * (y + 7U))) : Sentinel));
        }
    }
    ASTERIA_EXPECT(runner, "both clips shift the source with the destination", clippedExact);

    const Surface rgbx{MakeSurface(destinationPixels, 8U, 6U, 10U, PixelFormat::Rgbx)};
    ASTERIA_EXPECT(
        runner, "a copy between different formats is refused and writes nothing",
        !User::Graphics::CopyRect(rgbx, source, Rect{.X = 0, .Y = 0, .Width = 2U, .Height = 2U}, Point{.X = 0, .Y = 0}),
        destinationPixels[0U] == Sentinel);
    ASTERIA_EXPECT(runner, "a copy entirely outside succeeds and writes nothing",
                   User::Graphics::CopyRect(destination, source, Rect{.X = 0, .Y = 0, .Width = 2U, .Height = 2U},
                                            Point{.X = 20, .Y = 20}),
                   destinationPixels[0U] == Sentinel);
}

/// Glyph bit (column 0 is the leftmost) of a code, read straight from the table.
[[nodiscard]] bool GlyphBit(const UInt32 code, const UInt32 row, const UInt32 column) noexcept
{
    if ((code < 1U) || (code > 0xFEU))
    {
        return false;
    }
    const UInt32 byte{User::Graphics::Bold8x16Glyphs[((code - 1U) * 16U) + row]};
    return (byte & (0x80U >> column)) != 0U;
}

void TestGlyph(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "the font descriptor describes the table", User::Graphics::Bold8x16.GlyphWidth == 8U,
                   User::Graphics::Bold8x16.GlyphHeight == 16U, User::Graphics::Bold8x16.FirstCode == 1U,
                   User::Graphics::Bold8x16.Count == 254U,
                   User::Graphics::Bold8x16.Data == User::Graphics::Bold8x16Glyphs);

    UInt32 pixels[32U * 24U];
    const Surface surface{MakeSurface(pixels, 32U, 24U, 32U, PixelFormat::Rgbx)};
    User::Graphics::DrawGlyph(surface, 5, 3, User::Graphics::Bold8x16, 'A', White, Navy);
    const UInt32 foreground{User::Graphics::PackColor(PixelFormat::Rgbx, White)};
    const UInt32 background{User::Graphics::PackColor(PixelFormat::Rgbx, Navy)};
    bool exact{true};
    bool anyForeground{false};
    for (UInt32 y{0U}; y < 24U; ++y)
    {
        for (UInt32 x{0U}; x < 32U; ++x)
        {
            const bool in{(x >= 5U) && (x < 13U) && (y >= 3U) && (y < 19U)};
            UInt32 expected{Sentinel};
            if (in)
            {
                const bool bit{GlyphBit('A', y - 3U, x - 5U)};
                anyForeground = anyForeground || bit;
                expected = bit ? foreground : background;
            }
            exact = exact && (At(pixels, 32U, x, y) == expected);
        }
    }
    ASTERIA_EXPECT(runner, "an A is drawn pixel-exact: every cell pixel written, nothing outside", exact,
                   anyForeground);

    // Clipped at the right and bottom edges, and at the left and top.
    const Surface edge{MakeSurface(pixels, 32U, 24U, 32U, PixelFormat::Bgrx)};
    User::Graphics::DrawGlyph(edge, 28, 20, User::Graphics::Bold8x16, 'g', White, Navy);
    User::Graphics::DrawGlyph(edge, -3, -5, User::Graphics::Bold8x16, 0xB1U, White, Navy);
    const UInt32 edgeForeground{User::Graphics::PackColor(PixelFormat::Bgrx, White)};
    const UInt32 edgeBackground{User::Graphics::PackColor(PixelFormat::Bgrx, Navy)};
    bool edgeExact{true};
    for (UInt32 y{0U}; y < 24U; ++y)
    {
        for (UInt32 x{0U}; x < 32U; ++x)
        {
            UInt32 expected{Sentinel};
            if ((x >= 28U) && (y >= 20U))
            {
                expected = GlyphBit('g', y - 20U, x - 28U) ? edgeForeground : edgeBackground;
            }
            if ((x < 5U) && (y < 11U))
            {
                expected = GlyphBit(0xB1U, y + 5U, x + 3U) ? edgeForeground : edgeBackground;
            }
            edgeExact = edgeExact && (At(pixels, 32U, x, y) == expected);
        }
    }
    ASTERIA_EXPECT(runner, "glyphs clipped at all four edges draw the visible part exactly", edgeExact);

    const Surface blank{MakeSurface(pixels, 32U, 24U, 32U)};
    User::Graphics::DrawGlyph(blank, 0, 0, User::Graphics::Bold8x16, 0x00U, White, Navy);
    User::Graphics::DrawGlyph(blank, 8, 0, User::Graphics::Bold8x16, 0xFFU, White, Navy);
    User::Graphics::DrawGlyph(blank, 16, 0, User::Graphics::Bold8x16, 0x1234U, White, Navy);
    const UInt32 blankBackground{User::Graphics::PackColor(PixelFormat::Bgrx, Navy)};
    bool blanks{true};
    for (UInt32 y{0U}; y < 16U; ++y)
    {
        for (UInt32 x{0U}; x < 24U; ++x)
        {
            blanks = blanks && (At(pixels, 32U, x, y) == blankBackground);
        }
    }
    ASTERIA_EXPECT(runner, "codes outside the font draw blank cells of background", blanks,
                   At(pixels, 32U, 24U, 0U) == Sentinel);

    const Surface off{MakeSurface(pixels, 32U, 24U, 32U)};
    User::Graphics::DrawGlyph(off, 32, 0, User::Graphics::Bold8x16, 'A', White, Navy);
    User::Graphics::DrawGlyph(off, 0, -16, User::Graphics::Bold8x16, 'A', White, Navy);
    ASTERIA_EXPECT(runner, "a glyph entirely off the surface writes nothing", pixels[0U] == Sentinel,
                   pixels[(23U * 32U) + 31U] == Sentinel);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"GraphicsTests"};
    Asteria::Test::TestPixelFormats(runner);
    Asteria::Test::TestClipping(runner);
    Asteria::Test::TestFill(runner);
    Asteria::Test::TestCopy(runner);
    Asteria::Test::TestGlyph(runner);
    return runner.Finish();
}
