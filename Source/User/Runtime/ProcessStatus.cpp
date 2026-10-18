// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/ProcessStatus.hpp"

#include "User/Runtime/Include/AsProcessStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsProcessStatus) == sizeof(ProcessStatus));
static_assert(ProcessFacility.Number() == AsProcessFacilityNumber);

const char* Name(const ProcessStatus value) noexcept
{
    switch (value) {}
    return "Unknown";
}
} // namespace Asteria::User::Runtime
