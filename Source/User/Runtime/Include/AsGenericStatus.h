// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_GENERIC_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_GENERIC_STATUS_H_INCLUDED

#include "AsStatus.h"

/* Complete AsStatus encodings, usable directly with the C ABI. */
enum AsGenericStatus : uint32_t
{
    AsGenericRestarted = 0x80000001U,
    AsGenericPending = 0x80000002U,
    AsGenericRefusedForCredit = 0x80000003U,
    AsGenericStaleObject = 0x80000004U,
    AsGenericStateLost = 0x80000005U,
    AsGenericRebindRequired = 0x80000006U,
    AsGenericCommitStatusUnknown = 0x80000007U,
    AsGenericTooLate = 0x80000008U,
    AsGenericNotSupported = 0x80000009U,
    AsGenericInvalidArgument = 0x8000000AU,
    AsGenericOutOfResources = 0x8000000BU,
    AsGenericAccessDenied = 0x8000000CU,
};
enum AsGenericFacility : uint32_t
{
    AsGenericFacilityNumber = 0U,
};

#endif
