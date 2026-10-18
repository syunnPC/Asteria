// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/DatagramStatus.hpp"

#include "User/Runtime/Include/AsDatagramStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsDatagramStatus) == sizeof(DatagramStatus));
static_assert(DatagramFacility.Number() == AsDatagramFacilityNumber);
static_assert(Status{DatagramStatus::NoRule}.Raw() == AsDatagramNoRule);
static_assert(Status{DatagramStatus::NotBound}.Raw() == AsDatagramNotBound);
static_assert(Status{DatagramStatus::IdentityRefused}.Raw() == AsDatagramIdentityRefused);
static_assert(Status{DatagramStatus::Malformed}.Raw() == AsDatagramMalformed);
static_assert(Status{DatagramStatus::IoFailed}.Raw() == AsDatagramIoFailed);

const char* Name(const DatagramStatus value) noexcept
{
    switch (value)
    {
    case DatagramStatus::NoRule:
        return "NoRule";
    case DatagramStatus::NotBound:
        return "NotBound";
    case DatagramStatus::IdentityRefused:
        return "IdentityRefused";
    case DatagramStatus::Malformed:
        return "Malformed";
    case DatagramStatus::IoFailed:
        return "IoFailed";
    }
    return "Unknown";
}
} // namespace Asteria::User::Runtime
