// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_SURFACE_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_SURFACE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/PixelFormat.hpp"

namespace Asteria::User::Graphics
{
/// A pixel position; may lie outside any surface.
struct Point
{
    Foundation::Int32 X;
    Foundation::Int32 Y;
};

/// A rectangle of Width x Height pixels whose top-left pixel is (X, Y). Empty
/// when either extent is zero.
struct Rect
{
    Foundation::Int32 X;
    Foundation::Int32 Y;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
};

/// A borrowed 32-bit pixel view. Surface is the device/MMIO view; MemorySurface
/// is ordinary private storage, which permits normal compiler optimization.
template <typename TPixel> struct PixelSurface
{
    TPixel* Pixels;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
    Foundation::UInt32 Stride;
    PixelFormat Format;
};
using Surface = PixelSurface<volatile Foundation::UInt32>;
using MemorySurface = PixelSurface<Foundation::UInt32>;

[[nodiscard]] constexpr bool IsEmpty(const Rect rect) noexcept
{
    return (rect.Width == 0U) || (rect.Height == 0U);
}

/// Usable for drawing: pixels present, a valid format, a stride that holds a row.
template <typename TPixel> [[nodiscard]] constexpr bool IsUsable(const PixelSurface<TPixel>& surface) noexcept
{
    return (surface.Pixels != nullptr) && (surface.Width != 0U) && (surface.Height != 0U) &&
           (surface.Stride >= surface.Width) && IsValidPixelFormat(surface.Format);
}

template <typename TPixel> [[nodiscard]] constexpr Rect Bounds(const PixelSurface<TPixel>& surface) noexcept
{
    return Rect{.X = 0, .Y = 0, .Width = surface.Width, .Height = surface.Height};
}

/// The common area of two rectangles; the result is empty (extents zero) when
/// they do not meet.
[[nodiscard]] constexpr Rect Intersect(const Rect first, const Rect second) noexcept
{
    const Foundation::Int64 left{first.X > second.X ? first.X : second.X};
    const Foundation::Int64 top{first.Y > second.Y ? first.Y : second.Y};
    const Foundation::Int64 firstRight{static_cast<Foundation::Int64>(first.X) + first.Width};
    const Foundation::Int64 secondRight{static_cast<Foundation::Int64>(second.X) + second.Width};
    const Foundation::Int64 firstBottom{static_cast<Foundation::Int64>(first.Y) + first.Height};
    const Foundation::Int64 secondBottom{static_cast<Foundation::Int64>(second.Y) + second.Height};
    const Foundation::Int64 right{firstRight < secondRight ? firstRight : secondRight};
    const Foundation::Int64 bottom{firstBottom < secondBottom ? firstBottom : secondBottom};
    if ((right <= left) || (bottom <= top))
    {
        return Rect{.X = 0, .Y = 0, .Width = 0U, .Height = 0U};
    }
    return Rect{
        .X = static_cast<Foundation::Int32>(left),
        .Y = static_cast<Foundation::Int32>(top),
        .Width = static_cast<Foundation::UInt32>(right - left),
        .Height = static_cast<Foundation::UInt32>(bottom - top),
    };
}

/// The part of a rectangle inside the surface.
template <typename TPixel>
[[nodiscard]] constexpr Rect ClipToSurface(const PixelSurface<TPixel>& surface, const Rect rect) noexcept
{
    return Intersect(rect, Bounds(surface));
}
} // namespace Asteria::User::Graphics

#endif
