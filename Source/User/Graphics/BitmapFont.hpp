// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_BITMAP_FONT_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_BITMAP_FONT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/Bold8x16Font.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"

namespace Asteria::User::Graphics
{
/// A fixed-cell bitmap font. Each glyph is GlyphHeight bytes, one per row, the
/// most significant bit leftmost, so GlyphWidth is at most 8. Glyph code C
/// (FirstCode <= C < FirstCode + Count) starts at Data[(C - FirstCode) * GlyphHeight].
struct BitmapFont
{
    Foundation::UInt32 GlyphWidth;
    Foundation::UInt32 GlyphHeight;
    Foundation::UInt32 FirstCode;
    Foundation::UInt32 Count;
    const Foundation::UInt8* Data;
};

/// The IBM code page 437 bold 8x16 font (codes 0x01-0xFE; 0x00 and 0xFF draw blank).
inline constexpr BitmapFont Bold8x16{
    .GlyphWidth = Bold8x16Width,
    .GlyphHeight = Bold8x16Height,
    .FirstCode = Bold8x16FirstCode,
    .Count = Bold8x16Count,
    .Data = Bold8x16Glyphs,
};

/// Draws one glyph cell with its top-left pixel at (x, y): every pixel of the
/// cell is written (foreground where the glyph bit is set, background
/// elsewhere), clipped to the surface. A code outside the font draws a blank
/// (all background) cell.
template <typename TPixel>
inline void DrawGlyph(const PixelSurface<TPixel>& surface, const Foundation::Int32 x, const Foundation::Int32 y,
                      const BitmapFont& font, const Foundation::UInt32 code, const Color foreground,
                      const Color background) noexcept
{
    if (!IsUsable(surface) || (font.GlyphWidth == 0U) || (font.GlyphWidth > 8U) || (font.GlyphHeight == 0U))
    {
        return;
    }
    const Rect area{ClipToSurface(surface, Rect{.X = x, .Y = y, .Width = font.GlyphWidth, .Height = font.GlyphHeight})};
    if (IsEmpty(area))
    {
        return;
    }
    const bool present{(code >= font.FirstCode) && ((code - font.FirstCode) < font.Count) && (font.Data != nullptr)};
    const Foundation::UInt8* const glyph{
        present ? (font.Data + (static_cast<Foundation::Size>(code - font.FirstCode) * font.GlyphHeight)) : nullptr};
    const Foundation::UInt32 foregroundWord{PackColor(surface.Format, foreground)};
    const Foundation::UInt32 backgroundWord{PackColor(surface.Format, background)};
    const Foundation::UInt32 firstRow{static_cast<Foundation::UInt32>(area.Y - y)};
    const Foundation::UInt32 firstColumn{static_cast<Foundation::UInt32>(area.X - x)};
    for (Foundation::UInt32 row{0U}; row < area.Height; ++row)
    {
        const Foundation::UInt32 bits{present ? glyph[firstRow + row] : 0U};
        TPixel* const line{
            surface.Pixels +
            (static_cast<Foundation::Size>(static_cast<Foundation::UInt32>(area.Y) + row) * surface.Stride) +
            static_cast<Foundation::UInt32>(area.X)};
        for (Foundation::UInt32 column{0U}; column < area.Width; ++column)
        {
            const bool set{((bits >> (7U - (firstColumn + column))) & 1U) != 0U};
            line[column] = set ? foregroundWord : backgroundWord;
        }
    }
}
} // namespace Asteria::User::Graphics

#endif
