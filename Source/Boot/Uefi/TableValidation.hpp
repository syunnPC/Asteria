// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_TABLE_VALIDATION_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_TABLE_VALIDATION_HPP_INCLUDED

#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Uefi
{
constexpr Foundation::UInt32 MinimumSupportedTableRevision{0x0001'000AU};
constexpr Foundation::Size MaximumTableHeaderBytes{512U};

/// Calculates the UEFI/ISO 3309 CRC32 of a byte range.
/// Preconditions:
/// - data is non-null when byteCount is nonzero.
/// Allocation:
/// - Does not allocate.
[[nodiscard]] Foundation::UInt32 ComputeCrc32(const void* data, Foundation::Size byteCount) noexcept;

/// Validates the fixed fields that make a firmware table safe to inspect further.
[[nodiscard]] bool IsTableHeaderValid(const TableHeader& header, Foundation::UInt64 expectedSignature,
                                      Foundation::Size minimumSize) noexcept;

/// Validates Header.Crc32 over Header.HeaderSize bytes without calling firmware code.
/// Preconditions:
/// - IsTableHeaderValid has accepted header.
/// - table points to at least Header.HeaderSize readable bytes.
[[nodiscard]] bool IsTableCrcValid(const void* table, const TableHeader& header) noexcept;

/// Performs an observable read of a retained BSS probe.
[[nodiscard]] bool IsZeroInitializedProbe(const volatile Foundation::UInt64& probe) noexcept;

/// BSP only, after successful ExitBootServices. The table is mutable firmware
/// storage whose header was validated before exit. Clears expired boot/console
/// fields and updates its IEEE CRC32 without firmware calls or allocation.
/// Runtime services and configuration tables remain valid and unchanged.
void ClearExpiredBootServices(SystemTable& table) noexcept;
} // Namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_TABLE_VALIDATION_HPP_INCLUDED
