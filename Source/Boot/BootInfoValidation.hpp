// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_INFO_VALIDATION_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_INFO_VALIDATION_HPP_INCLUDED

#include "Boot/BootInfo.hpp"

namespace Asteria::Boot
{
/// Computes the CRC with Crc32c treated as zero. Does not allocate or retain
/// pointers; the complete aligned record must be readable and stable.
[[nodiscard]] Foundation::UInt32 ComputeBootInfoCrc32c(const BootInfo& info) noexcept;

/// Checks the version, exact size and CRC before an entry imports the record.
/// Does not dereference any physical address carried by the record.
[[nodiscard]] bool ValidateBootInfo(const BootInfo& info) noexcept;

/// Checks whole-file extents before forming views: required init/user files,
/// optional spinner, bounded modules and a non-wrapping LoaderData allocation
/// in the final map. No allocation or dereference of carried addresses.
[[nodiscard]] bool ValidateBootFileRegion(const BootInfo& info) noexcept;

/// Seals a fully initialized record, including unused array entries.
void SealBootInfo(BootInfo& info) noexcept;
} // namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_INFO_VALIDATION_HPP_INCLUDED
