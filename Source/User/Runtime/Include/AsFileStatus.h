// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_FILE_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_FILE_STATUS_H_INCLUDED

#include "AsStatus.h"

/* Complete AsStatus encodings, usable directly with the C ABI. */
enum AsFileStatus : uint32_t
{
    AsFileNotFound = 0x85000001U,
    AsFileCorrupt = 0x85000002U,
    AsFileIoFailed = 0x85000003U,
    AsFileCancelled = 0x85000004U,
};
enum AsFileFacility : uint32_t
{
    AsFileFacilityNumber = 5U,
};

#endif
