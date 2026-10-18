// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_IPC_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_IPC_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility IpcFacility{7U};

/// No IPC-specific service outcomes; preserve endpoint and WaitSet kernel errors.
enum class IpcStatus : Foundation::UInt32
{
};
inline constexpr Foundation::UInt32 LastIpcStatus{0U};

[[nodiscard]] constexpr Facility FacilityOf(IpcStatus) noexcept
{
    return IpcFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(IpcStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastIpcStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(IpcStatus value) noexcept;
} // namespace Asteria::User::Runtime

#endif
