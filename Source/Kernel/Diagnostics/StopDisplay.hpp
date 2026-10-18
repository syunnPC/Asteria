// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_STOP_DISPLAY_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_STOP_DISPLAY_HPP_INCLUDED

#include "Boot/BootInfo.hpp"

namespace Asteria::Kernel::Diagnostics
{
/// Full pitched mode extent, or zero for an unusable boot display. Capacity is
/// the platform's pre-reserved stop aperture; no allocation occurs at stop.
[[nodiscard]] constexpr Foundation::UInt64 StopDisplayBytes(const Boot::BootDisplay& display,
                                                            const Foundation::UInt64 capacityBytes) noexcept
{
    constexpr Foundation::UInt64 maximum{~Foundation::UInt64{0U}};
    const Foundation::UInt64 pixels = Foundation::UInt64{display.PixelsPerScanLine} * display.Height;
    if (display.FrameBufferBase == 0U || (display.FrameBufferBase & 4095U) != 0U || display.Width == 0U ||
        display.Height == 0U || display.PixelsPerScanLine < display.Width ||
        (display.Format != Boot::BootDisplayFormatRgbx && display.Format != Boot::BootDisplayFormatBgrx) ||
        pixels > maximum / 4U)
    {
        return 0U;
    }
    const Foundation::UInt64 bytes = pixels * 4U;
    if (bytes > display.FrameBufferBytes || bytes > capacityBytes || display.FrameBufferBase > maximum - bytes)
    {
        return 0U;
    }
    return bytes;
}
} // namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_STOP_DISPLAY_HPP_INCLUDED
