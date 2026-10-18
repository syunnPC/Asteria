// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_CAPTURE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_CAPTURE_HPP_INCLUDED

#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableLocator.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
/// Bounds the amount of firmware-owned SAL storage copied before boot-services
/// exit. A larger declared table is rejected by the parser as truncated.
constexpr Foundation::Size MaximumSalSystemTableSnapshotBytes{64U * 1024U};

enum class SalSystemTableCaptureErrorKind : Foundation::UInt32
{
    None,
    InvalidSnapshotBuffer,
    SnapshotBufferTooSmall,
    InvalidMemoryMapBuffer,
    MissingGetMemoryMap,
    MemoryMapFailed,
    MemoryMapTooLarge,
    LocationFailed,
    ParseFailed,
    SnapshotScrubFailed,
};

struct SalSystemTableCaptureError
{
    SalSystemTableCaptureErrorKind Kind;
    SalSystemTableLocateError LocationError;
    SalSystemTableParseError ParseError;
};

/// Locates the physical SAL table while the UEFI identity mapping is active, copies a bounded
/// extent into caller-owned storage, and parses only that immutable copy. Success returns inert
/// values and retains no firmware pointer. Preconditions not representable by the value arguments:
/// - configurationTables, memoryMap, and the located SAL bytes remain stable and readable until
/// their consumed bytes have been copied; - snapshotBuffer designates snapshotCapacity writable
/// bytes; it may overlap an input range because capture does not reread inputs after the move; -
/// the caller owns snapshotBuffer and prevents concurrent access.
class SalSystemTableCapture final
{
public:
    [[nodiscard]] static Foundation::Result<SalSystemTableInformation, SalSystemTableCaptureError>
    Capture(const Boot::Uefi::ConfigurationTable* configurationTables, Foundation::Size configurationTableCount,
            const void* memoryMap, Foundation::Size memoryMapSize, Foundation::Size memoryDescriptorSize,
            Foundation::UInt32 memoryDescriptorVersion, Foundation::UInt8* snapshotBuffer,
            Foundation::Size snapshotCapacity) noexcept;

    /// Obtains a fresh UEFI memory map and immediately captures the SAL table
    /// without an intervening firmware callback. Every valid snapshot extent is
    /// scrubbed and verified before this function returns, on success or failure.
    /// The memory-map and snapshot buffers must not overlap each other.
    [[nodiscard]] static Foundation::Result<SalSystemTableInformation, SalSystemTableCaptureError>
    CaptureFromBootServices(Boot::Uefi::BootServices& bootServices,
                            const Boot::Uefi::ConfigurationTable* configurationTables,
                            Foundation::Size configurationTableCount, void* memoryMapBuffer,
                            Foundation::Size memoryMapCapacity, Foundation::UInt8* snapshotBuffer,
                            Foundation::Size snapshotCapacity) noexcept;

private:
    SalSystemTableCapture() = delete;
};

static_assert(sizeof(SalSystemTableCaptureErrorKind) == 4U);
static_assert(sizeof(SalSystemTableCaptureError) == 12U);
static_assert(alignof(SalSystemTableCaptureError) == 4U);
static_assert(sizeof(Foundation::Result<SalSystemTableInformation, SalSystemTableCaptureError>) == 96U);
static_assert(alignof(Foundation::Result<SalSystemTableInformation, SalSystemTableCaptureError>) == 8U);
} // Namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_CAPTURE_HPP_INCLUDED
