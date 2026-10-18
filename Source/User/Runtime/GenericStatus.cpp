// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/GenericStatus.hpp"

#include "User/Runtime/Include/AsGenericStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsGenericStatus) == sizeof(GenericStatus));
static_assert(GenericFacility.Number() == AsGenericFacilityNumber);
static_assert(Status{GenericStatus::Restarted}.Raw() == AsGenericRestarted);
static_assert(Status{GenericStatus::Pending}.Raw() == AsGenericPending);
static_assert(Status{GenericStatus::RefusedForCredit}.Raw() == AsGenericRefusedForCredit);
static_assert(Status{GenericStatus::StaleObject}.Raw() == AsGenericStaleObject);
static_assert(Status{GenericStatus::StateLost}.Raw() == AsGenericStateLost);
static_assert(Status{GenericStatus::RebindRequired}.Raw() == AsGenericRebindRequired);
static_assert(Status{GenericStatus::CommitStatusUnknown}.Raw() == AsGenericCommitStatusUnknown);
static_assert(Status{GenericStatus::TooLate}.Raw() == AsGenericTooLate);
static_assert(Status{GenericStatus::NotSupported}.Raw() == AsGenericNotSupported);
static_assert(Status{GenericStatus::InvalidArgument}.Raw() == AsGenericInvalidArgument);
static_assert(Status{GenericStatus::OutOfResources}.Raw() == AsGenericOutOfResources);
static_assert(Status{GenericStatus::AccessDenied}.Raw() == AsGenericAccessDenied);

const char* Name(const GenericStatus value) noexcept
{
    switch (value)
    {
    case GenericStatus::Restarted:
        return "Restarted";
    case GenericStatus::Pending:
        return "Pending";
    case GenericStatus::RefusedForCredit:
        return "RefusedForCredit";
    case GenericStatus::StaleObject:
        return "StaleObject";
    case GenericStatus::StateLost:
        return "StateLost";
    case GenericStatus::RebindRequired:
        return "RebindRequired";
    case GenericStatus::CommitStatusUnknown:
        return "CommitStatusUnknown";
    case GenericStatus::TooLate:
        return "TooLate";
    case GenericStatus::NotSupported:
        return "NotSupported";
    case GenericStatus::InvalidArgument:
        return "InvalidArgument";
    case GenericStatus::OutOfResources:
        return "OutOfResources";
    case GenericStatus::AccessDenied:
        return "AccessDenied";
    }
    return "Unknown";
}
} // namespace Asteria::User::Runtime
