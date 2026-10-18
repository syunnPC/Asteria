// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/MemoryStatus.hpp"

#include "User/Runtime/Include/AsMemoryStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsMemoryStatus) == sizeof(MemoryStatus));
static_assert(MemoryFacility.Number() == AsMemoryFacilityNumber);

const char* Name(const MemoryStatus value) noexcept
{
    switch (value) {}
    return "Unknown";
}
} // namespace Asteria::User::Runtime
