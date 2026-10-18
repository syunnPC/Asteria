// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_MEMORY_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_MEMORY_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility MemoryFacility{2U};

/// No memory-specific service outcomes; preserve kernel errors.
enum class MemoryStatus : Foundation::UInt32
{
};
inline constexpr Foundation::UInt32 LastMemoryStatus{0U};

[[nodiscard]] constexpr Facility FacilityOf(MemoryStatus) noexcept
{
    return MemoryFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(MemoryStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastMemoryStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(MemoryStatus value) noexcept;
} // namespace Asteria::User::Runtime

#endif
