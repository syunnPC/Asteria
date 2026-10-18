// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_PROCESS_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_PROCESS_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility ProcessFacility{8U};

/// No process-specific service outcomes; preserve kernel errors.
enum class ProcessStatus : Foundation::UInt32
{
};
inline constexpr Foundation::UInt32 LastProcessStatus{0U};

[[nodiscard]] constexpr Facility FacilityOf(ProcessStatus) noexcept
{
    return ProcessFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(ProcessStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastProcessStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(ProcessStatus value) noexcept;
} // namespace Asteria::User::Runtime

#endif
