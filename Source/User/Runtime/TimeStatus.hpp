// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_TIME_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_TIME_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility TimeFacility{3U};

/// No time-specific service outcomes; preserve WaitSetWait kernel errors.
enum class TimeStatus : Foundation::UInt32
{
};
inline constexpr Foundation::UInt32 LastTimeStatus{0U};

[[nodiscard]] constexpr Facility FacilityOf(TimeStatus) noexcept
{
    return TimeFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(TimeStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastTimeStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(TimeStatus value) noexcept;
} // namespace Asteria::User::Runtime

#endif
