// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_IPC_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_IPC_STATUS_H_INCLUDED

#include "AsStatus.h"

/* No facility-specific outcomes are returned; the fixed representation is reserved. */
enum AsIpcStatus : uint32_t;
enum AsIpcFacility : uint32_t
{
    AsIpcFacilityNumber = 7U,
};

#endif
