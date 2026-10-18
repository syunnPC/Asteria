// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_STATUS_H_INCLUDED

#include <stdint.h>

typedef uint32_t AsStatus;
enum AsStatusEncoding : uint32_t
{
    AsStatusSuccess = 0U,
    AsStatusFailureBit = 0x80000000U,
    AsStatusCodeMask = 0x00FFFFFFU,
    AsStatusFacilityMask = 0x7F000000U,
    AsStatusFacilityShift = 24U,
};

#endif
