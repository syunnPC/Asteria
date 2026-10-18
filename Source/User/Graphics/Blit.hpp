// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_BLIT_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_BLIT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"

namespace Asteria::User::Graphics
{
/// Fills the part of a rectangle inside the surface. An unusable surface is
/// left alone.
template <typename TPixel>
inline void FillRect(const PixelSurface<TPixel>& surface, const Rect rect, const Color color) noexcept
{
    if (!IsUsable(surface))
    {
        return;
    }
    const Rect area{ClipToSurface(surface, rect)};
    if (IsEmpty(area))
    {
        return;
    }
    const Foundation::UInt32 word{PackColor(surface.Format, color)};
    for (Foundation::UInt32 row{0U}; row < area.Height; ++row)
    {
        TPixel* const line{
            surface.Pixels +
            (static_cast<Foundation::Size>(static_cast<Foundation::UInt32>(area.Y) + row) * surface.Stride) +
            static_cast<Foundation::UInt32>(area.X)};
        for (Foundation::UInt32 column{0U}; column < area.Width; ++column)
        {
            line[column] = word;
        }
    }
}

/// Copies the source rectangle to the destination with its top-left pixel at point, clipped to both
/// surfaces; pixels whose source or destination falls outside are skipped. The formats must match
/// (otherwise nothing is copied and false is returned). Overlapping memory spans with different
/// strides are refused before any write. With equal strides, rectangles may overlap: the copy runs
/// backwards (bottom row first, right to left) when the destination starts after the source in
/// memory, as memmove does, so the result is as if the source had been read first.
template <typename TDestinationPixel, typename TSourcePixel>
[[nodiscard]] inline bool CopyRect(const PixelSurface<TDestinationPixel>& destination,
                                   const PixelSurface<TSourcePixel>& source, const Rect rect,
                                   const Point point) noexcept
{
    if (!IsUsable(destination) || !IsUsable(source) || (destination.Format != source.Format))
    {
        return false;
    }
    // Clip against the source, move into destination coordinates, clip again.
    const Rect sourceArea{ClipToSurface(source, rect)};
    if (IsEmpty(sourceArea))
    {
        return true;
    }
    const Foundation::Int64 shiftX{static_cast<Foundation::Int64>(point.X) - rect.X};
    const Foundation::Int64 shiftY{static_cast<Foundation::Int64>(point.Y) - rect.Y};
    const Foundation::Int64 wantedX{sourceArea.X + shiftX};
    const Foundation::Int64 wantedY{sourceArea.Y + shiftY};
    const Foundation::Int64 left{wantedX > 0 ? wantedX : 0};
    const Foundation::Int64 top{wantedY > 0 ? wantedY : 0};
    Foundation::Int64 right{wantedX + sourceArea.Width};
    Foundation::Int64 bottom{wantedY + sourceArea.Height};
    if (right > destination.Width)
    {
        right = destination.Width;
    }
    if (bottom > destination.Height)
    {
        bottom = destination.Height;
    }
    if ((right <= left) || (bottom <= top))
    {
        return true;
    }
    const Foundation::UInt32 width{static_cast<Foundation::UInt32>(right - left)};
    const Foundation::UInt32 height{static_cast<Foundation::UInt32>(bottom - top)};
    const Foundation::Int64 sourceX{left - shiftX};
    const Foundation::Int64 sourceY{top - shiftY};
    TDestinationPixel* const destinationFirst{destination.Pixels +
                                              (static_cast<Foundation::Size>(top) * destination.Stride) +
                                              static_cast<Foundation::Size>(left)};
    TSourcePixel* const sourceFirst{source.Pixels + (static_cast<Foundation::Size>(sourceY) * source.Stride) +
                                    static_cast<Foundation::Size>(sourceX)};
    const Foundation::UIntPointer destinationStart{reinterpret_cast<Foundation::UIntPointer>(destinationFirst)};
    const Foundation::UIntPointer sourceStart{reinterpret_cast<Foundation::UIntPointer>(sourceFirst)};
    const Foundation::Size destinationSpan{(static_cast<Foundation::Size>(height - 1U) * destination.Stride + width) *
                                           sizeof(TDestinationPixel)};
    const Foundation::Size sourceSpan{(static_cast<Foundation::Size>(height - 1U) * source.Stride + width) *
                                      sizeof(TSourcePixel)};
    const bool overlaps{destinationStart <= sourceStart ? sourceStart - destinationStart < destinationSpan
                                                        : destinationStart - sourceStart < sourceSpan};
    if (overlaps && destination.Stride != source.Stride)
    {
        return false;
    }
    const bool backwards{reinterpret_cast<Foundation::UIntPointer>(destinationFirst) >
                         reinterpret_cast<Foundation::UIntPointer>(sourceFirst)};
    if (backwards)
    {
        for (Foundation::UInt32 step{0U}; step < height; ++step)
        {
            const Foundation::UInt32 row = height - 1U - step;
            TDestinationPixel* const to = destinationFirst + static_cast<Foundation::Size>(row) * destination.Stride;
            TSourcePixel* const from = sourceFirst + static_cast<Foundation::Size>(row) * source.Stride;
            for (Foundation::UInt32 index{width}; index != 0U; --index)
            {
                to[index - 1U] = from[index - 1U];
            }
        }
    }
    else
    {
        for (Foundation::UInt32 row{0U}; row < height; ++row)
        {
            TDestinationPixel* const to = destinationFirst + static_cast<Foundation::Size>(row) * destination.Stride;
            TSourcePixel* const from = sourceFirst + static_cast<Foundation::Size>(row) * source.Stride;
            for (Foundation::UInt32 column{0U}; column < width; ++column)
            {
                to[column] = from[column];
            }
        }
    }
    return true;
}
} // Namespace Asteria::User::Graphics

#endif
