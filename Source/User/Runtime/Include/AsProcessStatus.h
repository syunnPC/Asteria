// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_PROCESS_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_PROCESS_STATUS_H_INCLUDED

#include "AsStatus.h"

/* No facility-specific outcomes are returned; the fixed representation is reserved. */
enum AsProcessStatus : uint32_t;
enum AsProcessFacility : uint32_t
{
    AsProcessFacilityNumber = 8U,
};

#endif
