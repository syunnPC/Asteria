// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_COMPOSITION_CURSOR_IMAGES_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_COMPOSITION_CURSOR_IMAGES_HPP_INCLUDED

#include "User/Graphics/Surface.hpp"

namespace Asteria::User::Graphics::Composition
{
/// Borrowed straight-alpha ARGB words, row-major; at most 64x64 pixels.
/// Storage must outlive every render and backend use of the image.
struct CursorImage
{
    const Foundation::UInt32* Pixels;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
    Point HotSpot;
};

/// Original cursor artwork contributed to Asteria.
/// This project-owned mask is the source: MSB leftmost, rows top to bottom.
inline constexpr Foundation::UInt8 ArrowMask[16U]{
    0b10000000, 0b11000000, 0b11100000, 0b11110000, 0b11111000, 0b11111100, 0b11111110, 0b11111111,
    0b11111000, 0b11011100, 0b10011100, 0b00001110, 0b00001110, 0b00000111, 0b00000111, 0b00000011,
};
inline constexpr Foundation::UInt32 CursorWhite{0xFFFFFFFFU};
inline constexpr Foundation::UInt32 CursorOutline{0xFF000000U};
inline constexpr Foundation::UInt32 ArrowWidth{10U};
inline constexpr Foundation::UInt32 ArrowHeight{18U};

struct ArrowBitmap
{
    Foundation::UInt32 Pixels[ArrowWidth * ArrowHeight];
};

[[nodiscard]] constexpr bool ArrowBit(const Foundation::Int32 x, const Foundation::Int32 y) noexcept
{
    return (x >= 0) && (x < 8) && (y >= 0) && (y < 16) &&
           ((ArrowMask[y] & (0x80U >> static_cast<Foundation::UInt32>(x))) != 0U);
}

[[nodiscard]] constexpr ArrowBitmap MakeArrow() noexcept
{
    ArrowBitmap image{};
    for (Foundation::Int32 y{0}; y < 18; ++y)
    {
        for (Foundation::Int32 x{0}; x < 10; ++x)
        {
            bool touches{false};
            for (Foundation::Int32 dy{-1}; dy <= 1; ++dy)
            {
                for (Foundation::Int32 dx{-1}; dx <= 1; ++dx)
                {
                    touches = touches || ArrowBit(x - 1 + dx, y - 1 + dy);
                }
            }
            if (ArrowBit(x - 1, y - 1))
            {
                image.Pixels[y * 10 + x] = CursorWhite;
            }
            else if (touches)
            {
                image.Pixels[y * 10 + x] = CursorOutline;
            }
        }
    }
    return image;
}
inline constexpr ArrowBitmap ArrowPixels{MakeArrow()};
inline constexpr CursorImage Arrow{
    .Pixels = ArrowPixels.Pixels, .Width = ArrowWidth, .Height = ArrowHeight, .HotSpot = {.X = 1, .Y = 1}};
/// Project-owned alternate dot, opaque white centre with a dark border.
inline constexpr Foundation::UInt32 DotPixels[25U]{
    CursorOutline, CursorOutline, CursorOutline, CursorOutline, CursorOutline, CursorOutline, CursorWhite,
    CursorWhite,   CursorWhite,   CursorOutline, CursorOutline, CursorWhite,   CursorWhite,   CursorWhite,
    CursorOutline, CursorOutline, CursorWhite,   CursorWhite,   CursorWhite,   CursorOutline, CursorOutline,
    CursorOutline, CursorOutline, CursorOutline, CursorOutline,
};
inline constexpr CursorImage Dot{.Pixels = DotPixels, .Width = 5U, .Height = 5U, .HotSpot = {.X = 2, .Y = 2}};
} // namespace Asteria::User::Graphics::Composition
#endif // ASTERIA_USER_GRAPHICS_COMPOSITION_CURSOR_IMAGES_HPP_INCLUDED
