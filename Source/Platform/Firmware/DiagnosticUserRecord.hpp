// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_USER_RECORD_HPP_INCLUDED
#define ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_USER_RECORD_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Platform::Firmware
{
// Caller holds the platform writer exclusion for the entire record. product and test boot profiles:
// preserve the user provenance on dropped output.
template <typename TWriter>
void WriteDiagnosticUserRecord(TWriter write, const Foundation::UInt8* bytes, const Foundation::Size sizeBytes) noexcept
{
    constexpr char prefix[]{"\r\nUSER:"};
    constexpr char suffix[]{"\r\n"};
    for (Foundation::Size i{}; i < sizeof(prefix) - 1U; ++i)
    {
        if (!write(static_cast<Foundation::UInt8>(prefix[i])))
        {
            return;
        }
    }
    for (Foundation::Size i{}; i < sizeBytes; ++i)
    {
        if (!write(bytes[i]))
        {
            return;
        }
    }
    for (Foundation::Size i{}; i < sizeof(suffix) - 1U; ++i)
    {
        if (!write(static_cast<Foundation::UInt8>(suffix[i])))
        {
            return;
        }
    }
}
} // Namespace Asteria::Platform::Firmware
#endif
