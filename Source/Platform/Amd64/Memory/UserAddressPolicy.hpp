// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_USER_ADDRESS_POLICY_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_USER_ADDRESS_POLICY_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Memory
{
// User code and mappings share the four-level lower canonical half above page zero. Fault records
// may still describe an access outside it; those addresses are delivered to the supervisor, never
// mapped.
inline constexpr Foundation::UInt64 UserAddressStart{4096U};
inline constexpr Foundation::UInt64 UserAddressEnd{Foundation::UInt64{1U} << 47U};

[[nodiscard]] constexpr bool IsUserAddress(const Foundation::UInt64 address) noexcept
{
    return address >= UserAddressStart && address < UserAddressEnd;
}

[[nodiscard]] constexpr bool IsUserAddressRange(const Foundation::UInt64 start, const Foundation::UInt64 end) noexcept
{
    return IsUserAddress(start) && end > start && end <= UserAddressEnd;
}
} // Namespace Asteria::Platform::Amd64::Memory

#endif
