// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/ConsoleStatus.hpp"

#include "User/Runtime/Include/AsConsoleStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsConsoleStatus) == sizeof(ConsoleStatus));
static_assert(ConsoleFacility.Number() == AsConsoleFacilityNumber);
static_assert(Status{ConsoleStatus::Refused}.Raw() == AsConsoleRefused);

const char* Name(const ConsoleStatus value) noexcept
{
    switch (value)
    {
    case ConsoleStatus::Refused:
        return "Refused";
    }
    return "Unknown";
}
} // namespace Asteria::User::Runtime
