// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_DRAW_HPP_INCLUDED
#define ASTERIA_USER_FONT_DRAW_HPP_INCLUDED
#include "User/Font/Raster.hpp"
#include "User/Graphics/Surface.hpp"
namespace Asteria::User::Font
{
/// Blend linear coverage into Graphics RGBX/BGRX surfaces. baseline is integral pixels; fractional
/// positioning belongs in RasterOptions.
template <typename TPixel>
[[nodiscard]] Error DrawMask(const Graphics::PixelSurface<TPixel>& surface, const Mask& mask,
                             const Graphics::Point baseline, const Graphics::Color color,
                             const UInt8 opacity = 255U) noexcept
{
    if (!Graphics::IsUsable(surface) || mask.Width > MaximumMaskAxis || mask.Height > MaximumMaskAxis ||
        mask.Stride < mask.Width || mask.Stride > MaximumMaskAxis ||
        (static_cast<UInt64>(surface.Stride) * surface.Height > (~UInt64{0U}) / sizeof(UInt32)))
    {
        return Error::InvalidArgument;
    }
    if (mask.Width == 0U || mask.Height == 0U)
    {
        return Error::None;
    }
    if (mask.Pixels == nullptr || static_cast<Size>(mask.Height - 1U) * mask.Stride + mask.Width > mask.Length)
    {
        return Error::Truncated;
    }
    const Int64 x{static_cast<Int64>(baseline.X) + mask.Left};
    const Int64 y{static_cast<Int64>(baseline.Y) + mask.Top};
    const UInt32 foreground{Graphics::PackColor(surface.Format, color)};
    for (UInt32 row{}; row < mask.Height; ++row)
    {
        const Int64 py{y + row};
        if (py < 0 || py >= surface.Height)
        {
            continue;
        }
        for (UInt32 col{}; col < mask.Width; ++col)
        {
            const Int64 px{x + col};
            if (px < 0 || px >= surface.Width)
            {
                continue;
            }
            const UInt32 alpha{(mask.Pixels[static_cast<Size>(row) * mask.Stride + col] * opacity + 127U) / 255U};
            if (alpha == 0U)
            {
                continue;
            }
            TPixel& pixel = surface.Pixels[static_cast<Size>(py) * surface.Stride + static_cast<Size>(px)];
            const UInt32 background{pixel};
            UInt32 blended{};
            for (UInt32 shift{}; shift < 24U; shift += 8U)
            {
                const UInt32 c{
                    (((foreground >> shift) & 255U) * alpha + ((background >> shift) & 255U) * (255U - alpha) + 127U) /
                    255U};
                blended |= c << shift;
            }
            pixel = blended;
        }
    }
    return Error::None;
}
} // Namespace Asteria::User::Font
#endif
