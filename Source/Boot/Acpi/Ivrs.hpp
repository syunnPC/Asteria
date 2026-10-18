// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_IVRS_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_IVRS_HPP_INCLUDED

#include "Boot/BootDmaExclusions.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Acpi
{
inline constexpr Foundation::Size MaximumIvrsDevices{64U};
inline constexpr Foundation::Size MaximumIvrsMemory{8U};

struct IvrsDevice
{
    Foundation::UInt32 ExtendedFlags;
    Foundation::UInt16 First;
    Foundation::UInt16 Last;
    Foundation::UInt16 Alias;
    Foundation::UInt8 Flags;
    Foundation::UInt8 Type;
    Foundation::UInt8 Handle;
    Foundation::UInt8 Variety;
    Foundation::UInt16 Reserved;
};

struct IvrsMemory
{
    Foundation::UInt64 Base;
    Foundation::UInt64 Limit;
    Foundation::UInt16 First;
    Foundation::UInt16 Last;
    Foundation::UInt16 Segment;
    Foundation::UInt8 Flags;
    Foundation::UInt8 Type;
};

/// Highest supported IVHD description of the single supported unit. Firmware
/// often repeats the same unit as 10h and 11h/40h; these are alternate images,
/// not multiple units. Counts exceeding storage or malformed input refuse DMA.
struct IvrsTopology
{
    Foundation::UInt64 PhysicalAddress;
    Foundation::UInt64 RegisterBase;
    Foundation::UInt64 ExtendedFeatures;
    Foundation::UInt64 ExtendedFeatures2;
    Foundation::UInt32 Information;
    Foundation::UInt32 UnitCount;
    Foundation::UInt32 DeviceCount;
    Foundation::UInt32 MemoryCount;
    Foundation::UInt32 Malformed;
    Foundation::UInt32 SkippedHid;
    Foundation::UInt16 Segment;
    Foundation::UInt16 IommuDevice;
    Foundation::UInt16 CapabilityOffset;
    Foundation::UInt16 IommuInformation;
    Foundation::UInt8 Type;
    Foundation::UInt8 Flags;
    Foundation::UInt8 Reserved[6];
    IvrsDevice Devices[MaximumIvrsDevices];
    IvrsMemory Memory[MaximumIvrsMemory];
};

/// Bounded byte decoder of one complete checksummed IVRS SDT, at most 64 KiB.
/// Invalid framing returns false and leaves output empty. Independently bad
/// records are skipped and counted; callers must refuse an incomplete topology.
/// Normal boot/host context, no allocation or callbacks.
[[nodiscard]] bool ParseIvrs(const Foundation::UInt8* bytes, Foundation::Size length, IvrsTopology& output) noexcept;

/// Appends every IVMD block (types 20h-22h) of a framed IVRS to output in table order, as ParseIvrs
/// rounds it to the enclosing pages, while room is left, and counts each in output.DeclaredCount. A
/// block whose range is empty or wraps is counted, not listed; one ParseIvrs would drop or ignore
/// is listed as malformed. An IVRS whose blocks stop framing contributes the blocks before the
/// fault, as the DMAR's RMRRs do; one too short or too long for a table contributes nothing.
void CollectIvrsExclusions(const Foundation::UInt8* bytes, Foundation::Size length, BootDmaExclusions& output) noexcept;

static_assert(sizeof(IvrsDevice) == 16U);
static_assert(sizeof(IvrsMemory) == 24U);
static_assert(sizeof(IvrsTopology) == 1288U);
} // Namespace Asteria::Boot::Acpi

#endif // ASTERIA_BOOT_ACPI_IVRS_HPP_INCLUDED
