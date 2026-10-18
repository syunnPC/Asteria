// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_ACPI_TOPOLOGY_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_ACPI_TOPOLOGY_HPP_INCLUDED

#include "Boot/Acpi/AcpiPciTopology.hpp"
#include "Boot/Acpi/Ivrs.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Acpi
{
constexpr Foundation::Size MaximumAcpiProcessors{4U};
constexpr Foundation::Size MaximumAcpiIoControllers{8U};
constexpr Foundation::Size MaximumAcpiInterruptOverrides{16U};
constexpr Foundation::Size MaximumAcpiDmaRemappingUnits{4U};
constexpr Foundation::Size MaximumAcpiDmaDeviceScopes{32U};
constexpr Foundation::Size MaximumAcpiDmaScopePath{8U};
constexpr Foundation::Size MaximumAcpiReservedMemoryRegions{8U};
/// DMAR device-scope types: a PCI endpoint and a PCI bridge (whose
/// subordinate buses the unit serves).
constexpr Foundation::UInt8 AcpiDmaScopeEndpoint{1U};
constexpr Foundation::UInt8 AcpiDmaScopeBridge{2U};
/// The DMAR DRHD flag of a unit that serves every PCI device of its segment
/// no other unit lists.
constexpr Foundation::UInt8 AcpiDmaRemappingIncludeAllFlag{1U};

enum class AcpiRootTableKind : Foundation::UInt32
{
    None,
    Rsdt,
    Xsdt,
};

enum class AcpiProcessorControllerKind : Foundation::UInt32
{
    LocalApic,
    LocalX2Apic,
    LocalSapic,
};

enum class AcpiIoControllerKind : Foundation::UInt32
{
    IoApic,
    IoSapic,
};

struct AcpiProcessorTopology
{
    Foundation::UInt32 AcpiUid;
    Foundation::UInt32 HardwareId;
    Foundation::UInt32 HardwareEid;
    Foundation::UInt32 Flags;
    AcpiProcessorControllerKind ControllerKind;
    Foundation::UInt32 Reserved;
};

struct AcpiIoControllerTopology
{
    Foundation::UInt64 PhysicalAddress;
    Foundation::UInt32 GlobalSystemInterruptBase;
    Foundation::UInt32 ControllerId;
    AcpiIoControllerKind ControllerKind;
    Foundation::UInt32 Reserved;
};

struct AcpiInterruptOverride
{
    Foundation::UInt32 GlobalSystemInterrupt;
    Foundation::UInt16 Flags;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Source;
};

/// One DMAR DMA-remapping hardware unit (DRHD): its register page, segment,
/// flags and register-set size field. Scopes are retained separately.
struct AcpiDmaRemappingUnit
{
    Foundation::UInt64 RegisterBaseAddress;
    Foundation::UInt16 Segment;
    Foundation::UInt8 Flags;
    Foundation::UInt8 Size;
    Foundation::UInt32 Reserved;
};

/// One device scope of a retained DRHD: the unit's index, the scope type, the
/// start bus and the number of (device, function) hops of its path, of which
/// the first is kept in Device/Function and the remaining hops in Path;
/// a one-hop path names the device on the start bus.
struct AcpiDmaDeviceScope
{
    Foundation::UInt8 Unit;
    Foundation::UInt8 Type;
    Foundation::UInt8 StartBus;
    Foundation::UInt8 PathEntries;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    Foundation::UInt16 Reserved;
    Foundation::UInt8 Path[(MaximumAcpiDmaScopePath - 1U) * 2U];
    Foundation::UInt16 Padding;
};

/// Inclusive, page-aligned firmware-owned DMA range. Scope.Unit indexes
/// ReservedMemoryRegions when the scope is in ReservedMemoryScopes.
struct AcpiReservedMemoryRegion
{
    Foundation::UInt64 Base;
    Foundation::UInt64 Limit;
    Foundation::UInt16 Segment;
    Foundation::UInt8 Reserved[6];
};

inline constexpr Foundation::Size MaximumAcpiIommuHandoffs{8U};
struct AcpiIommuHandoff
{
    Foundation::UInt64 RegisterBase;
    Foundation::UInt32 Kind; // 1 VT-d, 2 AMD-Vi; retained even on topology refusal.
    Foundation::UInt32 Reserved;
};

constexpr Foundation::Size MaximumAcpiRootTableEntries{1024U};
constexpr Foundation::Size MaximumAcpiSsdts{64U};

/// Firmware definition-block metadata. ChecksumValid is 0 or 1; zero length
/// means absent. Bytes remain firmware-owned and are never interpreted here.
struct AcpiTableRecord
{
    Foundation::UInt64 PhysicalAddress;
    Foundation::UInt32 Length;
    char Signature[4U];
    char OemTableId[8U];
    Foundation::UInt8 Revision;
    Foundation::UInt8 ChecksumValid;
    Foundation::UInt8 Reserved[6U];
};

/// Fixed, pointer-free hardware topology retained after firmware exit.
struct AcpiTopology
{
    Foundation::UInt64 RsdpPhysicalAddress;
    Foundation::UInt64 RootTablePhysicalAddress;
    Foundation::UInt64 MadtPhysicalAddress;
    Foundation::UInt64 LocalControllerPhysicalAddress;
    AcpiRootTableKind RootKind;
    Foundation::UInt32 RsdpRevision;
    Foundation::UInt32 MadtRevision;
    Foundation::UInt32 MadtFlags;
    Foundation::UInt32 ProcessorCount;
    Foundation::UInt32 IoControllerCount;
    Foundation::UInt32 InterruptOverrideCount;
    /// AML declaration flags, zero means no namespace evidence.
    Foundation::UInt32 Reserved;
    AcpiProcessorTopology Processors[MaximumAcpiProcessors];
    AcpiIoControllerTopology IoControllers[MaximumAcpiIoControllers];
    AcpiInterruptOverride InterruptOverrides[MaximumAcpiInterruptOverrides];
    Foundation::UInt64 McfgPhysicalAddress;
    Foundation::UInt32 McfgRevision;
    Foundation::UInt32 PciConfigurationRegionCount;
    AcpiPciConfigurationRegion PciConfigurationRegions[MaximumAcpiPciConfigurationRegions];
    /// Usable MADT processors (enabled or online-capable) beyond the four the
    /// record retains; a diagnostic fact, zero on a machine within capacity.
    Foundation::UInt32 IgnoredProcessorCount;
    /// Qualifying keyboard PNP numeric ID (reason code).
    Foundation::UInt32 TrailingReserved;
    /// The optional DMAR: the host address width in bits (zero without a
    /// DMAR), its flags, every DRHD and RMRR counted with bounded records
    /// retained in firmware order. Scopes are counted separately for units
    /// and reserved ranges; complete paths are retained up to eight hops.
    /// Counts/path lengths beyond capacity force a topology refusal.
    Foundation::UInt64 DmarPhysicalAddress;
    Foundation::UInt8 DmarHostAddressWidth;
    Foundation::UInt8 DmarFlags;
    Foundation::UInt16 DmarReserved;
    Foundation::UInt32 DmaRemappingUnitCount;
    Foundation::UInt32 ReservedMemoryRegionCount;
    /// Independently malformed RMRRs skipped without dropping valid DRHDs.
    Foundation::UInt32 MalformedRmrrCount;
    AcpiDmaRemappingUnit DmaRemappingUnits[MaximumAcpiDmaRemappingUnits];
    Foundation::UInt32 DmaDeviceScopeCount;
    Foundation::UInt32 DmaDeviceScopeReserved;
    AcpiDmaDeviceScope DmaDeviceScopes[MaximumAcpiDmaDeviceScopes];
    /// Optional FADT revision >= 3, IAPC_BOOT_ARCH bit 1 indicates an 8042.
    Foundation::UInt16 IapcBootArchitecture;
    Foundation::UInt8 FadtRevision;
    /// Bound DSDT checksum byte and length; I8042Evidence pins the offsets.
    Foundation::UInt8 FadtReserved;
    Foundation::UInt32 FadtPadding;
    Foundation::UInt32 ReservedMemoryScopeCount;
    Foundation::UInt32 ReservedMemoryScopeReserved;
    AcpiReservedMemoryRegion ReservedMemoryRegions[MaximumAcpiReservedMemoryRegions];
    AcpiDmaDeviceScope ReservedMemoryScopes[MaximumAcpiDmaDeviceScopes];
    /// Optional calibration counters. PM space is 0 (memory) or 1 (I/O),
    /// width is 24 or 32; zero addresses mean absent or unusable tables.
    Foundation::UInt64 PmTimerAddress;
    Foundation::UInt64 HpetAddress;
    Foundation::UInt8 PmTimerSpace;
    Foundation::UInt8 PmTimerBits;
    /// First three bytes: high bound DSDT address bytes; remaining bytes zero.
    Foundation::UInt8 TimerReserved[6U];
    IvrsTopology Ivrs;
    Foundation::UInt32 IommuHandoffCount;
    Foundation::UInt32 IommuHandoffUnsafe;
    AcpiIommuHandoff IommuHandoffs[MaximumAcpiIommuHandoffs];
    /// Raw fields of the unique checksummed FADT, independent of diagnostic masking. Short tables leave unavailable fields zero.
    Foundation::UInt16 RawIapcBootArchitecture;
    Foundation::UInt8 RawFadtRevision;
    /// Low five bytes of the bound DSDT address, little-endian.
    Foundation::UInt8 DefinitionReserved[5U];
    AcpiTableRecord Dsdt;
    Foundation::UInt32 SsdtCount;
    Foundation::UInt32 SsdtOverflowCount;
    AcpiTableRecord Ssdts[MaximumAcpiSsdts];
};

static_assert(sizeof(AcpiTableRecord) == 32U);
static_assert(sizeof(AcpiRootTableKind) == 4U);
static_assert(sizeof(AcpiProcessorControllerKind) == 4U);
static_assert(sizeof(AcpiIoControllerKind) == 4U);
static_assert(sizeof(AcpiProcessorTopology) == 24U);
static_assert(alignof(AcpiProcessorTopology) == 4U);
static_assert(sizeof(AcpiIoControllerTopology) == 24U);
static_assert(alignof(AcpiIoControllerTopology) == 8U);
static_assert(sizeof(AcpiInterruptOverride) == 8U);
static_assert(alignof(AcpiInterruptOverride) == 4U);
static_assert(__builtin_offsetof(AcpiTopology, McfgPhysicalAddress) == 480U);
static_assert(__builtin_offsetof(AcpiTopology, IgnoredProcessorCount) == 624U);
static_assert(sizeof(AcpiDmaRemappingUnit) == 16U);
static_assert(alignof(AcpiDmaRemappingUnit) == 8U);
static_assert(__builtin_offsetof(AcpiTopology, DmarPhysicalAddress) == 632U);
static_assert(__builtin_offsetof(AcpiTopology, DmaRemappingUnits) == 656U);
static_assert(sizeof(AcpiDmaDeviceScope) == 24U);
static_assert(__builtin_offsetof(AcpiTopology, DmaDeviceScopes) == 728U);
static_assert(__builtin_offsetof(AcpiTopology, IapcBootArchitecture) == 1496U);
static_assert(__builtin_offsetof(AcpiTopology, ReservedMemoryRegions) == 1512U);
static_assert(sizeof(AcpiReservedMemoryRegion) == 24U);
static_assert(__builtin_offsetof(AcpiTopology, PmTimerAddress) == 2472U);
static_assert(__builtin_offsetof(AcpiTopology, Ivrs) == 2496U);
static_assert(sizeof(AcpiTopology) == 6016U);
static_assert(alignof(AcpiTopology) == 8U);
} // Namespace Asteria::Boot::Acpi

#endif // ASTERIA_BOOT_ACPI_ACPI_TOPOLOGY_HPP_INCLUDED
