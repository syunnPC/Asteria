// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_CONSOLE_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_CONSOLE_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility ConsoleFacility{4U};

/// The display broker answers this refusal for its current console query face.
enum class ConsoleStatus : Foundation::UInt32
{
    Refused = 1U,
};
inline constexpr Foundation::UInt32 LastConsoleStatus{1U};

[[nodiscard]] constexpr Facility FacilityOf(ConsoleStatus) noexcept
{
    return ConsoleFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(ConsoleStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastConsoleStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(ConsoleStatus value) noexcept;
} // namespace Asteria::User::Runtime

#endif
