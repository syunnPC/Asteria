// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_EDID_FIXTURE_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_EDID_FIXTURE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::TestSupport
{
/// Synthetic project-owned EDID fields; no firmware data is retained.
inline void FixEdidChecksum(Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt32 sum{0U};
    for (Foundation::Size index{0U}; index < 127U; ++index)
    {
        sum += bytes[index];
    }
    bytes[127U] = static_cast<Foundation::UInt8>((256U - (sum & 255U)) & 255U);
}

inline void MakeEdid(Foundation::UInt8* const bytes, const Foundation::UInt32 width = 1366U,
                     const Foundation::UInt32 height = 768U) noexcept
{
    for (Foundation::Size index{0U}; index < 128U; ++index)
    {
        bytes[index] = 0U;
    }
    for (Foundation::Size index{1U}; index < 7U; ++index)
    {
        bytes[index] = 255U;
    }
    bytes[8U] = 0x06U; // AST compressed manufacturer identifier.
    bytes[9U] = 0x74U;
    bytes[10U] = 0x34U;
    bytes[11U] = 0x12U;
    bytes[18U] = 1U;
    bytes[19U] = 4U;
    bytes[24U] = 2U;
    bytes[54U] = 0xD4U; // 84.04 MHz in 10 kHz units.
    bytes[55U] = 0x20U;
    bytes[56U] = static_cast<Foundation::UInt8>(width & 255U);
    bytes[58U] = static_cast<Foundation::UInt8>(((width >> 8U) & 15U) << 4U);
    bytes[59U] = static_cast<Foundation::UInt8>(height & 255U);
    bytes[61U] = static_cast<Foundation::UInt8>(((height >> 8U) & 15U) << 4U);
    FixEdidChecksum(bytes);
}
} // namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_EDID_FIXTURE_HPP_INCLUDED
