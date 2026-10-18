// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_DATAGRAM_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_DATAGRAM_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility DatagramFacility{6U};

/// The closed SocketRefused reasons and SocketFailed outcome, without diagnostic details.
enum class DatagramStatus : Foundation::UInt32
{
    NoRule = 1U,
    NotBound = 2U,
    IdentityRefused = 3U,
    Malformed = 4U,
    IoFailed = 5U,
};
inline constexpr Foundation::UInt32 LastDatagramStatus{5U};

[[nodiscard]] constexpr Facility FacilityOf(DatagramStatus) noexcept
{
    return DatagramFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(DatagramStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastDatagramStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(DatagramStatus value) noexcept;
} // namespace Asteria::User::Runtime

#endif
