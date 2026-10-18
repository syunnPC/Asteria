// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_ACPI_TABLE_PARSER_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_ACPI_TABLE_PARSER_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Acpi
{
enum class AcpiTargetArchitecture : Foundation::UInt32
{
    Amd64,
    Ia64,
};

enum class AcpiTableParseError : Foundation::UInt32
{
    None,
    InvalidArgument,
    InvalidBootInfo,
    /// A table or the RSDP overlaps memory the kernel or the firmware may
    /// still reuse (conventional, loader, boot-services, persistent or
    /// unaccepted memory).
    TableInReusableMemory,
    InvalidRsdp,
    InvalidRootTable,
    MadtNotFound,
    DuplicateMadt,
    InvalidMadt,
    InvalidControllerEntry,
    UnsupportedInterruptModel,
    IoControllerCapacityExceeded,
    InterruptOverrideCapacityExceeded,
    DuplicateProcessor,
    DuplicateIoController,
    DuplicateInterruptOverride,
    MissingEnabledProcessor,
    MissingIoController,
};

/// Synchronous, allocation-free table consumer; borrows firmware bytes only
/// during Visit. Never retained by the parser or called after firmware exit.
class IAcpiTableVisitor
{
public:
    virtual void Visit(const AcpiTableRecord& record, const Foundation::UInt8* bytes) noexcept = 0;

protected:
    constexpr IAcpiTableVisitor() noexcept = default;
    ~IAcpiTableVisitor() noexcept = default;
};

/// Parses the UEFI-provided RSDP, MADT, and optional MCFG, DMAR, FADT and HPET into a
/// pointer-free topology.
/// Preconditions:
/// - rsdpPointer is the physical identity-mapped address published by UEFI.
/// - ACPI table bytes remain immutable for the duration of this call.
/// Placement:
/// - every byte of the RSDP and of each table read lies outside the memory
/// map's reusable regions (conventional, loader, boot-services, persistent,
/// unaccepted); ACPI reclaim and NVS, reserved, runtime-services, MMIO,
/// unusable and PAL memory, and addresses the map does not list, are
/// accepted, as real firmware places tables there (the legacy BIOS area,
/// EfiReservedMemoryType). Every address outside the reusable regions must
/// therefore be readable while Parse runs: the firmware's identity map
/// before ExitBootServices provides that.
/// - output does not overlap the firmware tables.
/// - output is either independent of bootInfo or is exactly bootInfo.Acpi;
/// the parser reads all required BootInfo fields before publishing output.
/// Processors:
/// - every processor entry is validated; the output retains at most
/// MaximumAcpiProcessors usable ones in firmware order: the first enabled
/// processors, and online-capable ones only in slots no enabled processor
/// needs. On AMD64, bspApicId reserves a slot for the enabled BSP,
/// replacing the last retained processor if necessary. The default omits
/// this preference; IA-64 ignores it. A type-9 ID below 255 duplicating
/// a type-0 ID is ignored, regardless of entry order. Further usable processors are counted in IgnoredProcessorCount
/// instead of rejecting the topology; identities are unique among the
/// retained processors and no ignored one repeats a retained identity.
/// Optional input:
/// - absent, duplicate, or malformed MCFG input publishes zero MCFG fields
/// without rejecting an otherwise valid mandatory topology; so does DMAR
/// input for the DMAR fields.
/// - a unique checksummed FADT of revision >= 3 publishes IAPC_BOOT_ARCH;
/// absent, old, short, duplicate, or malformed FADTs publish zero.
/// - a unique checksummed FADT publishes a usable extended PM timer GAS
/// (memory or I/O), or its legacy I/O timer; hardware-reduced tables omit it.
/// A unique checksummed HPET publishes its aligned memory GAS. Malformed,
/// duplicate or absent timer tables publish zero addresses.
/// - AMD64 skips unusable IOAPIC entries and malformed interrupt source
/// overrides; it accepts no IOAPIC. MADT framing and CPU topology remain
/// mandatory. IA64 still requires its IOSAPIC model.
/// Failure atomicity:
/// - output is unchanged on failure.
/// Allocation:
/// - Does not allocate.
class AcpiTableParser final
{
public:
    /// Visits the root, each structurally bounded root entry and each DSDT
    /// selected by a checksummed FADT. Requires a successful Parse and immutable
    /// firmware bytes. RSDP and FACS are excluded (neither is an SDT).
    /// Checksum failures are passed as metadata, never as validated bytes.
    /// Does not allocate; callback runs synchronously before firmware exit.
    static void VisitTables(const BootInfo& bootInfo, const AcpiTopology& topology,
                            IAcpiTableVisitor& visitor) noexcept;

    /// Lists every RMRR of topology's DMAR and every IVMD of its IVRS in output: table order, up to
    /// BootDmaExclusionMaximumCount, an RMRR whose base exceeds its limit counted but not listed,
    /// one the parser drops listed as malformed, and DeclaredCount counting every structure. Tables
    /// whose structures do not frame contribute what precedes the fault. Requires a successful
    /// Parse; output starts empty. Does not allocate.
    static void CollectDmaExclusions(const BootInfo& bootInfo, const AcpiTopology& topology,
                                     BootDmaExclusions& output) noexcept;

    [[nodiscard]] static AcpiTableParseError Parse(const BootInfo& bootInfo, const void* rsdpPointer,
                                                   AcpiTargetArchitecture targetArchitecture, AcpiTopology& output,
                                                   Foundation::UInt32 bspApicId = 0xFFFF'FFFFU,
                                                   PowerRegisters* power = nullptr) noexcept;
};

static_assert(sizeof(AcpiTargetArchitecture) == 4U);
static_assert(sizeof(AcpiTableParseError) == 4U);
} // Namespace Asteria::Boot::Acpi

#endif // ASTERIA_BOOT_ACPI_ACPI_TABLE_PARSER_HPP_INCLUDED
