// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_PIXEL_FORMAT_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_PIXEL_FORMAT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Graphics
{
/// A 32-bit pixel format of the scanout. The values are the boot protocol's
/// (BootDisplayFormatRgbx 1 and BootDisplayFormatBgrx 2 in Boot/BootInfo.hpp),
/// redefined here because user space does not include the boot header. The
/// names give the byte order in memory: Rgbx stores red in byte 0, so as a
/// little-endian 32-bit word red is bits 0-7; Bgrx stores blue in byte 0. The
/// fourth byte is unused and packed as zero.
enum class PixelFormat : Foundation::UInt32
{
    Invalid = 0U,
    Rgbx = 1U,
    Bgrx = 2U,
};

struct Color
{
    Foundation::UInt8 Red;
    Foundation::UInt8 Green;
    Foundation::UInt8 Blue;
};

[[nodiscard]] constexpr bool IsValidPixelFormat(const PixelFormat format) noexcept
{
    return (format == PixelFormat::Rgbx) || (format == PixelFormat::Bgrx);
}

/// The pixel word a format stores for a color; zero for an invalid format.
[[nodiscard]] constexpr Foundation::UInt32 PackColor(const PixelFormat format, const Color color) noexcept
{
    const Foundation::UInt32 red{color.Red};
    const Foundation::UInt32 green{color.Green};
    const Foundation::UInt32 blue{color.Blue};
    switch (format)
    {
    case PixelFormat::Rgbx:
        return red | (green << 8U) | (blue << 16U);
    case PixelFormat::Bgrx:
        return blue | (green << 8U) | (red << 16U);
    case PixelFormat::Invalid:
    default:
        return 0U;
    }
}
} // namespace Asteria::User::Graphics

#endif
