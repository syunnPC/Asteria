// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_FILE_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_FILE_STATUS_HPP_INCLUDED

#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility FileFacility{5U};

/// Distinct file and backing block-I/O outcomes from the current protocols.
enum class FileStatus : Foundation::UInt32
{
    NotFound = 1U,
    Corrupt = 2U,
    IoFailed = 3U,
    Cancelled = 4U,
};
inline constexpr Foundation::UInt32 LastFileStatus{4U};

[[nodiscard]] constexpr Facility FacilityOf(FileStatus) noexcept
{
    return FileFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(FileStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastFileStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(FileStatus value) noexcept;
} // namespace Asteria::User::Runtime

#endif
