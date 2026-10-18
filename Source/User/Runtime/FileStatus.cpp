// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/FileStatus.hpp"

#include "User/Runtime/Include/AsFileStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsFileStatus) == sizeof(FileStatus));
static_assert(FileFacility.Number() == AsFileFacilityNumber);
static_assert(Status{FileStatus::NotFound}.Raw() == AsFileNotFound);
static_assert(Status{FileStatus::Corrupt}.Raw() == AsFileCorrupt);
static_assert(Status{FileStatus::IoFailed}.Raw() == AsFileIoFailed);
static_assert(Status{FileStatus::Cancelled}.Raw() == AsFileCancelled);

const char* Name(const FileStatus value) noexcept
{
    switch (value)
    {
    case FileStatus::NotFound:
        return "NotFound";
    case FileStatus::Corrupt:
        return "Corrupt";
    case FileStatus::IoFailed:
        return "IoFailed";
    case FileStatus::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}
} // namespace Asteria::User::Runtime
