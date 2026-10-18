// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_CONSOLE_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_CONSOLE_STATUS_H_INCLUDED

#include "AsStatus.h"

/* Complete AsStatus encodings, usable directly with the C ABI. */
enum AsConsoleStatus : uint32_t
{
    AsConsoleRefused = 0x84000001U,
};
enum AsConsoleFacility : uint32_t
{
    AsConsoleFacilityNumber = 4U,
};

#endif
