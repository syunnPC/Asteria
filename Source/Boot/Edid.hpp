// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_EDID_HPP_INCLUDED
#define ASTERIA_BOOT_EDID_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
enum class EdidError : Foundation::UInt32
{
    None,
    Absent,
    Truncated,
    Header,
    Checksum,
    Version,
    Timing,
};

struct EdidTiming
{
    char Manufacturer[4];
    Foundation::UInt16 Product;
    Foundation::UInt8 Revision;
    bool PreferredTimingFlag;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
    Foundation::UInt32 PixelClockKhz;
};

/// Reads only the 128-byte base block and its first detailed timing, field by
/// field. Extensions are ignored. No allocation; at most 128 checksum steps.
/// Output is unchanged on rejection. Not interrupt-safe: firmware boot use.
[[nodiscard]] EdidError ParseEdid(const Foundation::UInt8* bytes, Foundation::Size size, EdidTiming& output) noexcept;
[[nodiscard]] const char* EdidReason(EdidError error) noexcept;
} // namespace Asteria::Boot

#endif // ASTERIA_BOOT_EDID_HPP_INCLUDED
