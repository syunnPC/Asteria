// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_TIME_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_TIME_STATUS_H_INCLUDED

#include "AsStatus.h"

/* No facility-specific outcomes are returned; the fixed representation is reserved. */
enum AsTimeStatus : uint32_t;
enum AsTimeFacility : uint32_t
{
    AsTimeFacilityNumber = 3U,
};

#endif
