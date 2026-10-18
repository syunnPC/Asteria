// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_INFO_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_INFO_HPP_INCLUDED

#include "Abi/DeviceInventory.hpp"
#include "Boot/Acpi/AcpiTopology.hpp"
#include "Boot/Acpi/PowerRegisters.hpp"
#include "Boot/BootDebugPort.hpp"
#include "Boot/BootDmaExclusions.hpp"
#include "Boot/BootEntropy.hpp"
#include "Boot/BootFiles.hpp"
#include "Boot/BootKernelInput.hpp"
#include "Boot/BootTrust.hpp"
#include "Foundation/Types.hpp"
#include "Abi/Watchdog.hpp"

namespace Asteria::Boot
{
constexpr Foundation::UInt64 BootInfoSignature{0x4F46'4E49'5254'5341ULL};
constexpr Foundation::UInt32 BootInfoVersion{22U};
constexpr Foundation::Size MaximumMemoryRegions{2048U};

enum class PhysicalMemoryKind : Foundation::UInt32
{
    Reserved,
    Loader,
    BootServices,
    RuntimeServices,
    Conventional,
    Unusable,
    AcpiReclaimable,
    AcpiNonVolatile,
    MemoryMappedIo,
    PalCode,
    Persistent,
    Unaccepted,
};

struct PhysicalMemoryRegion
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 PageCount;
    Foundation::UInt64 Attributes;
    Foundation::UInt32 FirmwareType;
    PhysicalMemoryKind Kind;
};

/// The firmware's graphics mode as the boot left it: a linear framebuffer of Width x Height pixels,
/// PixelsPerScanLine apart, four bytes each in Format (BootDisplayFormat*), which the System
/// Display Broker scans out. A zero FrameBufferBase names none (no graphics output, a mode without
/// a linear framebuffer or with another pixel layout).
struct BootDisplay
{
    Foundation::UInt64 FrameBufferBase;
    Foundation::UInt64 FrameBufferBytes;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
    Foundation::UInt32 PixelsPerScanLine;
    Foundation::UInt32 Format;
};

/// A pixel's four bytes in memory order: red, green, blue, reserved; or blue,
/// green, red, reserved (UEFI's PixelRedGreenBlueReserved8BitPerColor and
/// PixelBlueGreenRedReserved8BitPerColor).
constexpr Foundation::UInt32 BootDisplayFormatRgbx{1U};
constexpr Foundation::UInt32 BootDisplayFormatBgrx{2U};

struct BootInventoryLocation
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 Pages;
};

struct BootInfo
{
    Foundation::UInt64 Signature;
    Foundation::UInt32 Version;
    Foundation::UInt32 SizeBytes;
    Foundation::UInt32 MemoryRegionCount;
    Foundation::UInt32 FirmwareDescriptorVersion;
    Foundation::UInt32 FirmwareDescriptorSize;
    Foundation::UInt32 Reserved;
    Foundation::UInt64 FirmwareMemoryMapSizeBytes;
    PhysicalMemoryRegion MemoryRegions[MaximumMemoryRegions];
    Acpi::AcpiTopology Acpi;
    BootEntropy Entropy;
    BootDisplay Display;
    BootInventoryLocation Inventory;
    BootFileRegion Files;
    Foundation::UInt32 Crc32c;
    Foundation::UInt32 ReservedAbi;
    BootKernelInput Kernel;
    Abi::DeviceInventory InventoryRecord;
    /// The diagnostic UART the loader found in the firmware's tables (IA-64;
    /// AMD64 leaves it zero and uses Kernel.Diagnostics).
    BootDebugPort DebugPort;
    /// The verified trusted-driver allowlist (BootTrust.hpp); Count zero for none.
    BootTrustPolicy Trust;
    /// Every firmware DMA reservation the tables declare (BootDmaExclusions.hpp).
    BootDmaExclusions DmaExclusions;
    /// BOOT.CFG's `lowmemory`: the low-memory watermark in free 2 MiB blocks below 4 GiB, 1 to
    /// BootLowMemoryMaximumBlocks; zero when the key is absent and the kernel's default applies.
    Foundation::UInt64 LowMemoryBlocks;
    /// BOOT.CFG watchdog seconds, zero for off (Architecture/Watchdog.md).
    Foundation::UInt64 WatchdogSeconds;
    Acpi::PowerRegisters Power;
    /// Console=0, desktop=1.
    Foundation::UInt64 SessionMode;
    /// BOOT.CFG login off=0, on=1.
    Foundation::UInt64 LoginEnabled;
};

/// The 2 MiB blocks below 4 GiB: the most `lowmemory` can name.
constexpr Foundation::UInt64 BootLowMemoryMaximumBlocks{2048U};

static_assert(sizeof(PhysicalMemoryKind) == 4U);
static_assert(sizeof(PhysicalMemoryRegion) == 40U);
static_assert(alignof(PhysicalMemoryRegion) == 8U);
static_assert(__builtin_offsetof(BootInfo, MemoryRegions) == 40U);
static_assert(__builtin_offsetof(BootInfo, Acpi) == 81'960U);
static_assert(__builtin_offsetof(BootInfo, Entropy) == 87'976U);
static_assert(sizeof(BootDisplay) == 32U);
static_assert(__builtin_offsetof(BootInfo, Display) == 88'016U);
static_assert(__builtin_offsetof(BootInfo, Inventory) == 88'048U);
static_assert(__builtin_offsetof(BootInfo, Files) == 88'064U);
static_assert(__builtin_offsetof(BootInfo, Crc32c) == 89'128U);
static_assert(__builtin_offsetof(BootInfo, Kernel) == 89'136U);
static_assert(__builtin_offsetof(BootInfo, InventoryRecord) == 89'712U);
static_assert(sizeof(Abi::DeviceInventory) == 14'808U);
static_assert(__builtin_offsetof(BootInfo, DebugPort) == 104'520U);
static_assert(__builtin_offsetof(BootInfo, Trust) == 104'536U);
static_assert(__builtin_offsetof(BootInfo, DmaExclusions) == 104'928U);
static_assert(__builtin_offsetof(BootInfo, LowMemoryBlocks) == 106'472U);
static_assert(__builtin_offsetof(BootInfo, WatchdogSeconds) == 106'480U);
static_assert(__builtin_offsetof(BootInfo, Power) == 106'488U);
static_assert(__builtin_offsetof(BootInfo, SessionMode) == 106'552U);
static_assert(__builtin_offsetof(BootInfo, LoginEnabled) == 106'560U);
static_assert(sizeof(BootInfo) == 106'568U);
static_assert(__is_trivial(BootInfo)); // No firmware static initialization code.
static_assert(alignof(BootInfo) == 8U);
} // Namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_INFO_HPP_INCLUDED
