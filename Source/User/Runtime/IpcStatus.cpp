// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/IpcStatus.hpp"

#include "User/Runtime/Include/AsIpcStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsIpcStatus) == sizeof(IpcStatus));
static_assert(IpcFacility.Number() == AsIpcFacilityNumber);

const char* Name(const IpcStatus value) noexcept
{
    switch (value) {}
    return "Unknown";
}
} // namespace Asteria::User::Runtime
