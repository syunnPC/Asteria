// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_GENERIC_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_GENERIC_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility GenericFacility{0U};

/// Shared recovery and native preflight outcomes.
enum class GenericStatus : Foundation::UInt32
{
    Restarted = 1U,
    Pending = 2U,
    RefusedForCredit = 3U,
    StaleObject = 4U,
    StateLost = 5U,
    RebindRequired = 6U,
    CommitStatusUnknown = 7U,
    TooLate = 8U,
    NotSupported = 9U,
    InvalidArgument = 10U,
    OutOfResources = 11U,
    AccessDenied = 12U,
};
inline constexpr Foundation::UInt32 LastGenericStatus{12U};

[[nodiscard]] constexpr Facility FacilityOf(GenericStatus) noexcept
{
    return GenericFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(GenericStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastGenericStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(GenericStatus value) noexcept;
} // Namespace Asteria::User::Runtime

#endif
