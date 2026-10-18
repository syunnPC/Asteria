// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_DEVICE_INVENTORY_HPP_INCLUDED
#define ASTERIA_ABI_DEVICE_INVENTORY_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Abi
{
inline constexpr Foundation::UInt32 DeviceInventoryVersion{3U};
inline constexpr Foundation::Size MaximumInventoryPciFunctions{256U};
inline constexpr Foundation::Size MaximumInventoryDisks{8U};
inline constexpr Foundation::Size MaximumInventoryTrust{8U};
inline constexpr Foundation::UInt64 DeviceInventoryPages{4U};

struct InventoryPciFunction
{
    Foundation::UInt16 Segment;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    Foundation::UInt8 HeaderType;
    Foundation::UInt16 Vendor;
    Foundation::UInt16 Product;
    Foundation::UInt8 Class;
    Foundation::UInt8 Subclass;
    Foundation::UInt8 Interface;
    Foundation::UInt8 InterruptPin;
    Foundation::UInt8 InterruptLine;
    Foundation::UInt8 Reserved;
    Foundation::UInt32 Bars[6U];
};

enum class InventoryDriver : Foundation::UInt8
{
    None,
    Ahci,
    E1000e,
    Edu,
    Display,
    EduPending,
    Xhci,
    /// A USB 1.1/2.0 host controller published to the USB host group driver.
    UsbHost,
    /// IA-64's e1000e before the kernel's ECAM capture, as EduPending.
    E1000ePending,
};

/// How a published driver's function masters the bus: Trusted names a function the kernel lets DMA
/// without isolation under a BOOT.CFG trust entry.
enum class InventoryDma : Foundation::UInt8
{
    Unknown,
    Trusted,
};

struct InventoryPciEntry
{
    InventoryPciFunction Function;
    InventoryDriver Driver;
    /// One-based index into the published xHCI bootstrap table (Driver Xhci)
    /// or USB host table (Driver UsbHost); zero if skipped.
    Foundation::UInt8 XhciIndex;
    /// Publication outcome: zero success/unknown, 1 capture, 2 publication, 3 capacity,
    /// 4 IOMMU handoff refused without a trust entry, 5 firmware DMA reservation (USB host).
    Foundation::UInt8 PublicationStep;
    Foundation::UInt8 PublicationError;
    Foundation::UInt16 XhciScratchpads;
    InventoryDma Dma;
    /// One-based index into DeviceInventoryTrust::Entries of the entry that applies; zero for none.
    Foundation::UInt8 TrustEntry;
    /// Authoritative decoded firmware BAR0 extent of an xHCI, EHCI or OHCI,
    /// without destructive probing.
    Foundation::UInt64 XhciBarBytes;
};

/// What became of one BOOT.CFG trust entry (BootTrust.hpp): Pending until the kernel decided.
enum class InventoryTrustOutcome : Foundation::UInt8
{
    Pending,
    /// The function is driven without
    Applied,
    /// An IOMMU confines the function; the entry is unused.
    IommuConfines,
    /// No published function the entry's driver serves matches it.
    NoFunction,
    /// The module's SHA-256 digest differs from the entry's.
    DigestMismatch,
    /// The matching function is served by another driver than the entry names.
    DriverMismatch,
    /// The function matches more than one entry; none applies.
    Ambiguous,
    /// The function is one nothing may assign (bridge, display, firmware DMA).
    NotAssignable,
};

struct InventoryTrust
{
    Foundation::UInt16 Segment;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    /// BootTrustSelects* bits.
    Foundation::UInt8 Selector;
    Foundation::UInt16 Vendor;
    Foundation::UInt16 Product;
    /// BootTrustDriver value.
    Foundation::UInt8 Driver;
    InventoryTrustOutcome Outcome;
    Foundation::UInt8 Reserved[4U];
    char Module[16U];
    Foundation::UInt8 Digest[32U];
};

struct InventoryDisk
{
    Foundation::UInt64 Sectors;
    Foundation::UInt8 Port;
    Foundation::UInt8 Reserved[7U];
    char Model[40U];
};

/// Immutable after bootstrap diagnostics and before the pager is admitted.
/// The kernel carries this ABI record without interpreting its contents.
struct DeviceInventory
{
    Foundation::UInt32 Version;
    Foundation::UInt32 PciCount;
    Foundation::UInt32 DiskCount;
    /// 0: unknown, 1: unpublished, 2: FADT rule, 3: AML rule (status gate passed).
    Foundation::UInt32 I8042;
    Foundation::UInt32 Truncated;
    Foundation::UInt32 Reserved;
    InventoryPciEntry Pci[MaximumInventoryPciFunctions];
    InventoryDisk Disks[MaximumInventoryDisks];
};

/// Version 3's trusted-driver block: the kernel's copy of the BOOT.CFG trust
/// entries and each one's outcome. It follows the record in the inventory's
/// pages (DeviceInventoryFrame), outside the record BootInfo carries, and only
/// the kernel writes it; the loader's pages leave it zero.
struct DeviceInventoryTrust
{
    Foundation::UInt32 Count;
    Foundation::UInt32 Reserved;
    InventoryTrust Entries[MaximumInventoryTrust];
};

/// The inventory's pages as the kernel publishes them read-only to the root
/// service: the record, then the trust block.
struct DeviceInventoryFrame
{
    DeviceInventory Record;
    DeviceInventoryTrust Trust;
};

static_assert(sizeof(InventoryPciFunction) == 40U);
static_assert(sizeof(InventoryPciEntry) == 56U);
static_assert(sizeof(InventoryDisk) == 56U);
static_assert(sizeof(InventoryTrust) == 64U);
static_assert(sizeof(DeviceInventoryTrust) == 520U);
static_assert(__builtin_offsetof(DeviceInventoryFrame, Trust) == sizeof(DeviceInventory));
static_assert(sizeof(DeviceInventoryFrame) <= DeviceInventoryPages * 4096U);
static_assert(sizeof(DeviceInventory) <= DeviceInventoryPages * 4096U);
} // Namespace Asteria::Abi

#endif // ASTERIA_ABI_DEVICE_INVENTORY_HPP_INCLUDED
