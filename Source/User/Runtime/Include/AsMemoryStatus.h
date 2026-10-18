// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_MEMORY_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_MEMORY_STATUS_H_INCLUDED

#include "AsStatus.h"

/* No facility-specific outcomes are returned; the fixed representation is reserved. */
enum AsMemoryStatus : uint32_t;
enum AsMemoryFacility : uint32_t
{
    AsMemoryFacilityNumber = 2U,
};

#endif
