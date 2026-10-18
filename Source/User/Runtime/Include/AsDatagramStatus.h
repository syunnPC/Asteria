// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_DATAGRAM_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_DATAGRAM_STATUS_H_INCLUDED

#include "AsStatus.h"

/* Complete AsStatus encodings, usable directly with the C ABI. */
enum AsDatagramStatus : uint32_t
{
    AsDatagramNoRule = 0x86000001U,
    AsDatagramNotBound = 0x86000002U,
    AsDatagramIdentityRefused = 0x86000003U,
    AsDatagramMalformed = 0x86000004U,
    AsDatagramIoFailed = 0x86000005U,
};
enum AsDatagramFacility : uint32_t
{
    AsDatagramFacilityNumber = 6U,
};

#endif
