// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/TimeStatus.hpp"

#include "User/Runtime/Include/AsTimeStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsTimeStatus) == sizeof(TimeStatus));
static_assert(TimeFacility.Number() == AsTimeFacilityNumber);

const char* Name(const TimeStatus value) noexcept
{
    switch (value) {}
    return "Unknown";
}
} // namespace Asteria::User::Runtime
