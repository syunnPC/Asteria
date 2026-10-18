// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/Status.hpp"

#include "User/Runtime/GenericStatus.hpp"
#include "User/Runtime/Include/AsStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsStatus) == sizeof(Status));
static_assert(alignof(AsStatus) == alignof(Status));
static_assert(AsStatusFacilityMask == 0x7F00'0000U);
static_assert(AsStatusFacilityShift == 24U);
static_assert(AsStatusFailureBit == 0x8000'0000U);
static_assert(AsStatusCodeMask == 0x00FF'FFFFU);
static_assert(AsStatusSuccess == 0U);

Foundation::Result<Status, Status> Status::FromRaw(const Foundation::UInt32 raw) noexcept
{
    if ((raw != 0U) && (((raw & 0x8000'0000U) == 0U) || ((raw & 0x00FF'FFFFU) == 0U)))
    {
        return Foundation::Result<Status, Status>::Failure(GenericStatus::InvalidArgument);
    }
    Status status{};
    status.m_raw = raw;
    return Foundation::Result<Status, Status>::Success(status);
}
} // namespace Asteria::User::Runtime
