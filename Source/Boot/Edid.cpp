// SPDX-License-Identifier: Apache-2.0

#include "Boot/Edid.hpp"

namespace Asteria::Boot
{
EdidError ParseEdid(const Foundation::UInt8* const bytes, const Foundation::Size size, EdidTiming& output) noexcept
{
    // VESA E-EDID Release A, revision 2, sections 3.1, 3.4, 3.10 and 3.11:
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf
    if (bytes == nullptr || size == 0U)
    {
        return EdidError::Absent;
    }
    if (size < 128U)
    {
        return EdidError::Truncated;
    }
    constexpr Foundation::UInt8 header[]{0U, 255U, 255U, 255U, 255U, 255U, 255U, 0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (bytes[index] != header[index])
        {
            return EdidError::Header;
        }
    }
    Foundation::UInt32 sum{0U};
    for (Foundation::Size index{0U}; index < 128U; ++index)
    {
        sum += bytes[index];
    }
    if ((sum & 255U) != 0U)
    {
        return EdidError::Checksum;
    }
    if (bytes[18U] != 1U || bytes[19U] > 4U)
    {
        return EdidError::Version;
    }
    EdidTiming timing{};
    timing.Revision = bytes[19U];
    timing.PreferredTimingFlag = (bytes[24U] & 2U) != 0U;
    const Foundation::UInt32 manufacturer{(Foundation::UInt32{bytes[8U]} << 8U) | bytes[9U]};
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        const Foundation::UInt32 letter{(manufacturer >> ((2U - index) * 5U)) & 31U};
        timing.Manufacturer[index] = letter >= 1U && letter <= 26U ? static_cast<char>('A' + letter - 1U) : '?';
    }
    timing.Product =
        static_cast<Foundation::UInt16>(Foundation::UInt32{bytes[10U]} | (Foundation::UInt32{bytes[11U]} << 8U));
    timing.Width = bytes[56U] | ((Foundation::UInt32{bytes[58U]} & 0xF0U) << 4U);
    timing.Height = bytes[59U] | ((Foundation::UInt32{bytes[61U]} & 0xF0U) << 4U);
    timing.PixelClockKhz = (Foundation::UInt32{bytes[54U]} | (Foundation::UInt32{bytes[55U]} << 8U)) * 10U;
    // A zero clock denotes a display descriptor, not a timing. Interlaced
    // timings describe fields; the boot only selects progressive scanout.
    if (timing.PixelClockKhz == 0U || timing.Width == 0U || timing.Height == 0U || (bytes[71U] & 0x80U) != 0U)
    {
        return EdidError::Timing;
    }
    output = timing;
    return EdidError::None;
}

const char* EdidReason(const EdidError error) noexcept
{
    switch (error)
    {
    case EdidError::None:
        return "OK";
    case EdidError::Absent:
        return "absent";
    case EdidError::Truncated:
        return "truncated";
    case EdidError::Header:
        return "invalid-header";
    case EdidError::Checksum:
        return "invalid-checksum";
    case EdidError::Version:
        return "unsupported-version";
    case EdidError::Timing:
        return "no-progressive-preferred-timing";
    }
    return "invalid-error";
}
} // namespace Asteria::Boot
