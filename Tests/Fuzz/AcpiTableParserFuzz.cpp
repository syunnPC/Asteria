// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the ACPI table parser (AcpiTableParser::Parse).
//
// Firmware memory lives at a fixed address so inputs can carry absolute table
// addresses: one AcpiReclaimable page, an unmapped hole page, one
// AcpiNonVolatile page, and an unmapped guard page. The parser accepts
// tables anywhere outside reusable memory, so the memory map lists every
// other address, the hole included, as conventional memory, which is never
// mapped: any dereference outside the two ACPI pages faults.
//
// Input: flags (1 byte), reclaimable-part length (2 bytes, LE), RSDP offset
// (2 bytes, LE), then the reclaimable page contents followed by the
// non-volatile page contents; missing bytes read as zero.
//   flags bit 0: parse for IA-64 instead of AMD64
//   flags bit 1: repair the RSDP and SDT checksums before parsing
//   flags bit 2: the RSDP lies in the non-volatile page
//   flags bit 3: inject the invalid argument selected by bits 4-7
//
// Properties checked for every input:
// - Parse dereferences nothing outside the two ACPI pages (such reads fault).
// - A rejected input leaves the output byte-for-byte unchanged.
// - Parsing into bootInfo.Acpi, the documented aliasing case, gives the same
//   result as parsing into an independent output (so parsing is deterministic).
// - Invalid arguments and BootInfo headers are rejected with their error.
// - An accepted topology is canonical: every published address lies in an
//   ACPI region, processors and I/O controllers match the architecture and
//   are unique, overrides are well formed and unique, MCFG regions are
//   aligned, sorted and disjoint, and unused slots are zero.
// - The retained processors are the MADT's first four enabled processors
//   plus, in the slots none of them needs, its first online-capable ones, in
//   MADT order; every other usable processor is counted as ignored.

#include "Boot/Acpi/AcpiTableParser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"

namespace
{
using Asteria::Boot::BootInfo;
using Asteria::Boot::PhysicalMemoryKind;
using Asteria::Boot::PhysicalMemoryRegion;
using Asteria::Boot::Acpi::AcpiIoControllerKind;
using Asteria::Boot::Acpi::AcpiPciConfigurationRegion;
using Asteria::Boot::Acpi::AcpiProcessorControllerKind;
using Asteria::Boot::Acpi::AcpiRootTableKind;
using Asteria::Boot::Acpi::AcpiTableParseError;
using Asteria::Boot::Acpi::AcpiTableParser;
using Asteria::Boot::Acpi::AcpiTargetArchitecture;
using Asteria::Boot::Acpi::AcpiTopology;
using Asteria::Boot::Acpi::MaximumAcpiInterruptOverrides;
using Asteria::Boot::Acpi::MaximumAcpiIoControllers;
using Asteria::Boot::Acpi::MaximumAcpiPciConfigurationRegions;
using Asteria::Boot::Acpi::MaximumAcpiProcessors;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UIntPointer;

constexpr Size PageBytes{4096U};
constexpr Size InputHeaderBytes{5U};
constexpr UIntPointer ArenaAddress{0x4150'0000U};
constexpr UIntPointer ReclaimableAddress{ArenaAddress};
constexpr UIntPointer NonVolatileAddress{ArenaAddress + (2U * PageBytes)};
constexpr UInt8 RepairChecksumsFlag{1U << 1U};
constexpr UInt8 Ia64Flag{1U << 0U};
constexpr UInt8 NonVolatileRsdpFlag{1U << 2U};
constexpr UInt8 InvalidArgumentFlag{1U << 3U};

constexpr UIntPointer AcpiPageAddresses[]{ReclaimableAddress, NonVolatileAddress};

constinit BootInfo g_bootInfo{};
constinit UInt8* g_arena{nullptr};

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

[[nodiscard]] UInt64 ReadLittleEndian(const UInt8* const bytes, const Size byteCount) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < byteCount; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void WriteLittleEndian(UInt8* const bytes, const UInt64 value, const Size byteCount) noexcept
{
    for (Size index{0U}; index < byteCount; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

void StoreChecksum(UInt8* const bytes, const Size byteCount, const Size checksumOffset) noexcept
{
    bytes[checksumOffset] = 0U;
    UInt8 sum{0U};
    for (Size index{0U}; index < byteCount; ++index)
    {
        sum = static_cast<UInt8>(sum + bytes[index]);
    }
    bytes[checksumOffset] = static_cast<UInt8>(0U - sum);
}

// Maps the arena once: the hole and guard pages are unmapped again, and no
// other mapping of the process is placed this low.
[[nodiscard]] UInt8* GetArena() noexcept
{
    if (g_arena == nullptr)
    {
        void* const hint = reinterpret_cast<void*>(ArenaAddress);
        void* const memory = mmap(hint, 4U * PageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory != hint)
        {
            static_cast<void>(std::fprintf(stderr, "FAIL: the ACPI arena is not at its fixed address\n"));
            std::abort();
        }
        g_arena = static_cast<UInt8*>(memory);
        if ((munmap(g_arena + PageBytes, PageBytes) != 0) || (munmap(g_arena + (3U * PageBytes), PageBytes) != 0))
        {
            static_cast<void>(std::fprintf(stderr, "FAIL: cannot unmap the arena guard pages\n"));
            std::abort();
        }
    }
    return g_arena;
}

// Returns the mapped bytes of [address, address + byteCount) when the range
// lies entirely inside one ACPI page of the arena, and nullptr otherwise.
[[nodiscard]] UInt8* ResolveArena(const UInt64 address, const Size byteCount) noexcept
{
    for (const UIntPointer pageAddress : AcpiPageAddresses)
    {
        if ((address >= pageAddress) && (byteCount <= PageBytes) &&
            ((address - pageAddress) <= (PageBytes - byteCount)))
        {
            return reinterpret_cast<UInt8*>(static_cast<UIntPointer>(address));
        }
    }
    return nullptr;
}

void RepairTable(const UInt64 address) noexcept
{
    const UInt8* const header = ResolveArena(address, 36U);
    if (header == nullptr)
    {
        return;
    }
    const Size length = static_cast<Size>(ReadLittleEndian(header + 4U, 4U));
    UInt8* const table = length >= 36U ? ResolveArena(address, length) : nullptr;
    if (table != nullptr)
    {
        StoreChecksum(table, length, 9U);
    }
}

// Makes every checksum that the parser verifies valid, from the leaf tables
// to the RSDP, without reading outside the arena pages.
void RepairChecksums(const UInt64 rsdpAddress) noexcept
{
    UInt8* const rsdp = ResolveArena(rsdpAddress, 20U);
    if (rsdp == nullptr)
    {
        return;
    }
    const bool hasExtendedRsdp = (rsdp[15U] != 0U) && (ResolveArena(rsdpAddress, 36U) != nullptr);
    const UInt64 xsdtAddress = hasExtendedRsdp ? ReadLittleEndian(rsdp + 24U, 8U) : 0U;
    const UInt64 rootAddress = xsdtAddress != 0U ? xsdtAddress : ReadLittleEndian(rsdp + 16U, 4U);
    const Size entryBytes = xsdtAddress != 0U ? 8U : 4U;
    const UInt8* const rootHeader = ResolveArena(rootAddress, 36U);
    const Size rootLength = rootHeader != nullptr ? static_cast<Size>(ReadLittleEndian(rootHeader + 4U, 4U)) : 0U;
    const UInt8* const root = rootLength >= 36U ? ResolveArena(rootAddress, rootLength) : nullptr;
    if (root != nullptr)
    {
        for (Size offset{36U}; (offset + entryBytes) <= rootLength; offset += entryBytes)
        {
            RepairTable(ReadLittleEndian(root + offset, entryBytes));
        }
        RepairTable(rootAddress);
    }
    StoreChecksum(rsdp, 20U, 8U);
    if (hasExtendedRsdp)
    {
        const Size rsdpLength = static_cast<Size>(ReadLittleEndian(rsdp + 20U, 4U));
        UInt8* const extended = rsdpLength >= 36U ? ResolveArena(rsdpAddress, rsdpLength) : nullptr;
        if (extended != nullptr)
        {
            StoreChecksum(extended, rsdpLength, 32U);
        }
    }
}

// A valid map: conventional memory, never mapped, everywhere but the two
// ACPI pages of the arena.
void ResetBootInfo() noexcept
{
    constexpr UInt64 GuardAddress{NonVolatileAddress + PageBytes};
    g_bootInfo.Signature = Asteria::Boot::BootInfoSignature;
    g_bootInfo.Version = Asteria::Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = sizeof(BootInfo);
    g_bootInfo.MemoryRegionCount = 5U;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = 48U;
    g_bootInfo.Reserved = 0U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 5U * 48U;
    g_bootInfo.MemoryRegions[0U] =
        PhysicalMemoryRegion{0U, 0U, ArenaAddress / PageBytes, 0U, 7U, PhysicalMemoryKind::Conventional};
    g_bootInfo.MemoryRegions[1U] =
        PhysicalMemoryRegion{ReclaimableAddress, 0U, 1U, 0U, 9U, PhysicalMemoryKind::AcpiReclaimable};
    g_bootInfo.MemoryRegions[2U] =
        PhysicalMemoryRegion{ReclaimableAddress + PageBytes, 0U, 1U, 0U, 7U, PhysicalMemoryKind::Conventional};
    g_bootInfo.MemoryRegions[3U] =
        PhysicalMemoryRegion{NonVolatileAddress, 0U, 1U, 0U, 10U, PhysicalMemoryKind::AcpiNonVolatile};
    g_bootInfo.MemoryRegions[4U] = PhysicalMemoryRegion{GuardAddress, 0U, (UInt64{0U} - GuardAddress) / PageBytes,
                                                        0U,           7U, PhysicalMemoryKind::Conventional};
}

// Applies one invalid argument; returns the error Parse must report.
[[nodiscard]] AcpiTableParseError InjectInvalidArgument(const UInt8 selector, const void*& rsdp,
                                                        AcpiTargetArchitecture& architecture) noexcept
{
    switch (selector % 13U)
    {
    case 0U:
        rsdp = nullptr;
        return AcpiTableParseError::InvalidArgument;
    case 1U:
        architecture = static_cast<AcpiTargetArchitecture>(2U);
        return AcpiTableParseError::InvalidArgument;
    case 2U:
        g_bootInfo.Signature ^= 1U;
        break;
    case 3U:
        ++g_bootInfo.Version;
        break;
    case 4U:
        g_bootInfo.SizeBytes -= 8U;
        break;
    case 5U:
        g_bootInfo.MemoryRegionCount = 0U;
        g_bootInfo.FirmwareMemoryMapSizeBytes = 0U;
        break;
    case 6U:
        g_bootInfo.FirmwareDescriptorVersion = 2U;
        break;
    case 7U:
        g_bootInfo.FirmwareDescriptorSize = 32U;
        g_bootInfo.FirmwareMemoryMapSizeBytes = 5U * 32U;
        break;
    case 8U:
        g_bootInfo.Reserved = 1U;
        break;
    case 9U:
        ++g_bootInfo.FirmwareMemoryMapSizeBytes;
        break;
    case 10U:
        g_bootInfo.MemoryRegions[1U].PhysicalStart = NonVolatileAddress;
        g_bootInfo.MemoryRegions[3U].PhysicalStart = ReclaimableAddress;
        break;
    case 11U:
        g_bootInfo.MemoryRegions[3U].FirmwareType = 9U;
        break;
    default:
        ++g_bootInfo.MemoryRegions[0U].PhysicalStart;
        break;
    }
    return AcpiTableParseError::InvalidBootInfo;
}

[[nodiscard]] AcpiTopology MakeSentinel() noexcept
{
    AcpiTopology topology{};
    auto* const bytes = reinterpret_cast<UInt8*>(&topology);
    for (Size index{0U}; index < sizeof(topology); ++index)
    {
        bytes[index] = 0xA5U;
    }
    return topology;
}

[[nodiscard]] bool IsZero(const void* const bytes, const Size byteCount) noexcept
{
    const auto* const data = static_cast<const UInt8*>(bytes);
    for (Size index{0U}; index < byteCount; ++index)
    {
        if (data[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool HasSignature(const UInt64 address, const char* const signature) noexcept
{
    const UInt8* const header = ResolveArena(address, 36U);
    return (header != nullptr) && (std::memcmp(header, signature, 4U) == 0);
}

void CheckProcessors(const AcpiTopology& topology, const bool isIa64) noexcept
{
    if ((topology.ProcessorCount == 0U) || (topology.ProcessorCount > MaximumAcpiProcessors))
    {
        Violation("an accepted topology has between one and four processors");
    }
    bool hasEnabledProcessor{false};
    for (Size index{0U}; index < topology.ProcessorCount; ++index)
    {
        const auto& processor = topology.Processors[index];
        const bool kindMatches = isIa64 ? (processor.ControllerKind == AcpiProcessorControllerKind::LocalSapic)
                                        : ((processor.ControllerKind == AcpiProcessorControllerKind::LocalApic) ||
                                           (processor.ControllerKind == AcpiProcessorControllerKind::LocalX2Apic));
        if (!kindMatches || ((processor.Flags != 1U) && (processor.Flags != 2U)) || (processor.Reserved != 0U) ||
            (!isIa64 && (processor.HardwareEid != 0U)))
        {
            Violation("processors carry the architecture's controller kind and exactly one of enabled/online-capable");
        }
        hasEnabledProcessor = hasEnabledProcessor || (processor.Flags == 1U);
        for (Size other{0U}; other < index; ++other)
        {
            if ((topology.Processors[other].AcpiUid == processor.AcpiUid) ||
                ((topology.Processors[other].HardwareId == processor.HardwareId) &&
                 (topology.Processors[other].HardwareEid == processor.HardwareEid)))
            {
                Violation("processor UIDs and hardware identifiers are unique");
            }
        }
    }
    if (!hasEnabledProcessor)
    {
        Violation("an accepted topology has an enabled processor");
    }
    if (!IsZero(&topology.Processors[topology.ProcessorCount],
                (MaximumAcpiProcessors - topology.ProcessorCount) * sizeof(topology.Processors[0U])))
    {
        Violation("unused processor slots are zero");
    }
}

struct MadtProcessor
{
    UInt32 AcpiUid;
    UInt32 HardwareId;
    UInt32 HardwareEid;
    UInt32 Flags;
};

// Decodes the processor entry at `entry` of the target's kinds, if it is one.
[[nodiscard]] bool DecodeProcessor(const UInt8* const entry, const bool isIa64, MadtProcessor& processor) noexcept
{
    const Size length = entry[1U];
    if (!isIa64 && (entry[0U] == 0U))
    {
        processor = MadtProcessor{entry[2U], entry[3U], 0U, static_cast<UInt32>(ReadLittleEndian(entry + 4U, 4U))};
        return true;
    }
    if (!isIa64 && (entry[0U] == 9U))
    {
        processor = MadtProcessor{static_cast<UInt32>(ReadLittleEndian(entry + 12U, 4U)),
                                  static_cast<UInt32>(ReadLittleEndian(entry + 4U, 4U)), 0U,
                                  static_cast<UInt32>(ReadLittleEndian(entry + 8U, 4U))};
        return true;
    }
    if (isIa64 && (entry[0U] == 7U))
    {
        processor = MadtProcessor{length == 12U ? entry[2U] : static_cast<UInt32>(ReadLittleEndian(entry + 12U, 4U)),
                                  entry[3U], entry[4U], static_cast<UInt32>(ReadLittleEndian(entry + 8U, 4U))};
        return true;
    }
    return false;
}

// An independent two-pass statement of the selection rule: count the enabled
// processors, then take the first min(enabled, 4) of them and the first
// online-capable ones for the remaining slots, in MADT order.
void CheckProcessorSelection(const AcpiTopology& topology, const bool isIa64) noexcept
{
    const UInt8* const header = ResolveArena(topology.MadtPhysicalAddress, 44U);
    const Size length = header == nullptr ? 0U : static_cast<Size>(ReadLittleEndian(header + 4U, 4U));
    const UInt8* const madt = ResolveArena(topology.MadtPhysicalAddress, length);
    if (madt == nullptr)
    {
        Violation("the published MADT lies in an ACPI region");
    }
    Size enabled{0U};
    for (Size offset{44U}; offset < length; offset += madt[offset + 1U])
    {
        MadtProcessor processor{};
        if (DecodeProcessor(madt + offset, isIa64, processor) && ((processor.Flags & 1U) != 0U))
        {
            ++enabled;
        }
    }
    const Size enabledSlots = enabled < MaximumAcpiProcessors ? enabled : MaximumAcpiProcessors;
    Size enabledTaken{0U};
    Size onlineCapableTaken{0U};
    Size retained{0U};
    Size ignored{0U};
    for (Size offset{44U}; offset < length; offset += madt[offset + 1U])
    {
        MadtProcessor processor{};
        if (!DecodeProcessor(madt + offset, isIa64, processor) || ((processor.Flags & 3U) == 0U))
        {
            continue;
        }
        const bool isEnabled = (processor.Flags & 1U) != 0U;
        const bool taken =
            isEnabled ? (enabledTaken < enabledSlots) : (onlineCapableTaken < (MaximumAcpiProcessors - enabledSlots));
        if (!taken)
        {
            ++ignored;
            continue;
        }
        enabledTaken += isEnabled ? 1U : 0U;
        onlineCapableTaken += isEnabled ? 0U : 1U;
        if (retained >= topology.ProcessorCount)
        {
            Violation("every processor the rule selects is retained");
        }
        const auto& published = topology.Processors[retained];
        if ((published.AcpiUid != processor.AcpiUid) || (published.HardwareId != processor.HardwareId) ||
            (published.HardwareEid != processor.HardwareEid) || (published.Flags != processor.Flags))
        {
            Violation("the retained processors are the first enabled ones and then online-capable ones");
        }
        ++retained;
    }
    if ((retained != topology.ProcessorCount) || (ignored != topology.IgnoredProcessorCount))
    {
        Violation("every usable processor is either retained or counted as ignored");
    }
}

void CheckControllers(const AcpiTopology& topology, const bool isIa64) noexcept
{
    if ((isIa64 && (topology.IoControllerCount == 0U)) || (topology.IoControllerCount > MaximumAcpiIoControllers))
    {
        Violation("an accepted topology has at most eight I/O controllers; IA64 needs one");
    }
    for (Size index{0U}; index < topology.IoControllerCount; ++index)
    {
        const auto& controller = topology.IoControllers[index];
        const AcpiIoControllerKind expectedKind = isIa64 ? AcpiIoControllerKind::IoSapic : AcpiIoControllerKind::IoApic;
        if ((controller.ControllerKind != expectedKind) || (controller.PhysicalAddress == 0U) ||
            (controller.ControllerId > 0xFFU) || (controller.Reserved != 0U))
        {
            Violation("I/O controllers carry the architecture's kind, an address and an 8-bit identifier");
        }
        for (Size other{0U}; other < index; ++other)
        {
            const auto& previous = topology.IoControllers[other];
            if ((previous.ControllerId == controller.ControllerId) ||
                (previous.PhysicalAddress == controller.PhysicalAddress) ||
                (previous.GlobalSystemInterruptBase == controller.GlobalSystemInterruptBase))
            {
                Violation("I/O controller identifiers, addresses and interrupt bases are unique");
            }
        }
    }
    if (!IsZero(&topology.IoControllers[topology.IoControllerCount],
                (MaximumAcpiIoControllers - topology.IoControllerCount) * sizeof(topology.IoControllers[0U])))
    {
        Violation("unused I/O controller slots are zero");
    }
    if (topology.InterruptOverrideCount > MaximumAcpiInterruptOverrides)
    {
        Violation("an accepted topology has at most sixteen interrupt overrides");
    }
    for (Size index{0U}; index < topology.InterruptOverrideCount; ++index)
    {
        const auto& interruptOverride = topology.InterruptOverrides[index];
        const UInt32 flags = interruptOverride.Flags;
        if ((interruptOverride.Bus != 0U) || (interruptOverride.Source > 15U) || ((flags & 0xFFF0U) != 0U) ||
            ((flags & 3U) == 2U) || (((flags >> 2U) & 3U) == 2U))
        {
            Violation("interrupt overrides are ISA-bus overrides with defined flags");
        }
        for (Size other{0U}; other < index; ++other)
        {
            if ((topology.InterruptOverrides[other].Source == interruptOverride.Source) ||
                (topology.InterruptOverrides[other].GlobalSystemInterrupt == interruptOverride.GlobalSystemInterrupt))
            {
                Violation("interrupt override sources and targets are unique");
            }
        }
    }
    const Size unusedOverrides = MaximumAcpiInterruptOverrides - topology.InterruptOverrideCount;
    if (!IsZero(&topology.InterruptOverrides[topology.InterruptOverrideCount],
                unusedOverrides * sizeof(topology.InterruptOverrides[0U])))
    {
        Violation("unused interrupt override slots are zero");
    }
}

[[nodiscard]] UInt64 LastAddress(const AcpiPciConfigurationRegion& region) noexcept
{
    return region.BaseAddress + ((static_cast<UInt64>(region.LastBus) + 1U) << 20U) - 1U;
}

void CheckPciRegions(const AcpiTopology& topology) noexcept
{
    const Size count = topology.PciConfigurationRegionCount;
    if (count == 0U)
    {
        if ((topology.McfgPhysicalAddress != 0U) || (topology.McfgRevision != 0U) ||
            !IsZero(topology.PciConfigurationRegions, sizeof(topology.PciConfigurationRegions)))
        {
            Violation("a topology without MCFG publishes zero MCFG fields");
        }
        return;
    }
    if ((count > MaximumAcpiPciConfigurationRegions) || (topology.McfgRevision == 0U) ||
        !HasSignature(topology.McfgPhysicalAddress, "MCFG"))
    {
        Violation("a published MCFG lies in an ACPI region and has a revision");
    }
    for (Size index{0U}; index < count; ++index)
    {
        const AcpiPciConfigurationRegion& region = topology.PciConfigurationRegions[index];
        if (((region.BaseAddress & 0xF'FFFFU) != 0U) || (region.FirstBus > region.LastBus) || (region.Reserved != 0U) ||
            (LastAddress(region) < region.BaseAddress))
        {
            Violation("MCFG regions are 1-MiB aligned, ordered bus ranges without overflow");
        }
        if (index == 0U)
        {
            continue;
        }
        const AcpiPciConfigurationRegion& previous = topology.PciConfigurationRegions[index - 1U];
        if ((previous.Segment > region.Segment) ||
            ((previous.Segment == region.Segment) && (previous.FirstBus >= region.FirstBus)))
        {
            Violation("MCFG regions are sorted by segment and bus");
        }
        for (Size other{0U}; other < index; ++other)
        {
            const AcpiPciConfigurationRegion& earlier = topology.PciConfigurationRegions[other];
            const UInt64 earlierFirst = earlier.BaseAddress + (static_cast<UInt64>(earlier.FirstBus) << 20U);
            const UInt64 regionFirst = region.BaseAddress + (static_cast<UInt64>(region.FirstBus) << 20U);
            if (((earlier.Segment == region.Segment) && (earlier.LastBus >= region.FirstBus)) ||
                ((earlierFirst <= LastAddress(region)) && (regionFirst <= LastAddress(earlier))))
            {
                Violation("MCFG regions are disjoint in bus numbers and addresses");
            }
        }
    }
    if (!IsZero(&topology.PciConfigurationRegions[count],
                (MaximumAcpiPciConfigurationRegions - count) * sizeof(topology.PciConfigurationRegions[0U])))
    {
        Violation("unused MCFG slots are zero");
    }
}

void CheckDmar(const AcpiTopology& topology) noexcept
{
    if (topology.DmarPhysicalAddress == 0U)
    {
        if ((topology.DmarHostAddressWidth != 0U) || (topology.DmaRemappingUnitCount != 0U) ||
            (topology.ReservedMemoryRegionCount != 0U) || (topology.ReservedMemoryScopeCount != 0U))
        {
            Violation("an invalid optional DMAR publishes no partial ranges");
        }
        return;
    }
    if (!HasSignature(topology.DmarPhysicalAddress, "DMAR") || (topology.DmarHostAddressWidth > 64U))
    {
        Violation("retained DMAR has a valid signature and bounded width");
    }
    constexpr Size Regions{Asteria::Boot::Acpi::MaximumAcpiReservedMemoryRegions};
    for (Size index{0U}; (index < topology.ReservedMemoryRegionCount) && (index < Regions); ++index)
    {
        const auto& region = topology.ReservedMemoryRegions[index];
        if ((region.Base > region.Limit) || ((region.Base & 0xFFFU) != 0U) || ((region.Limit & 0xFFFU) != 0xFFFU) ||
            (region.Limit == ~UInt64{0U}))
        {
            Violation("RMRRs retain inclusive whole-page ranges without overflow");
        }
    }
    constexpr Size Scopes{Asteria::Boot::Acpi::MaximumAcpiDmaDeviceScopes};
    for (Size index{0U}; (index < topology.ReservedMemoryScopeCount) && (index < Scopes); ++index)
    {
        const auto& scope = topology.ReservedMemoryScopes[index];
        if ((scope.Unit >= Regions) || (scope.Unit >= topology.ReservedMemoryRegionCount) ||
            (scope.PathEntries == 0U) || (scope.Device > 31U) || (scope.Function > 7U))
        {
            Violation("retained RMRR scopes index a retained range and valid PCI path");
        }
    }
}

void CheckDefinitionRecord(const Asteria::Boot::Acpi::AcpiTableRecord& record, const char* signature) noexcept
{
    if (record.Length == 0U)
    {
        if (!IsZero(&record, sizeof(record)))
        {
            Violation("absent definition blocks have zero metadata");
        }
        return;
    }
    const UInt8* const bytes = ResolveArena(record.PhysicalAddress, record.Length);
    if (bytes == nullptr || record.Length < 36U || record.Length > 1'048'576U ||
        std::memcmp(bytes, signature, 4U) != 0 || std::memcmp(record.Signature, signature, 4U) != 0 ||
        ReadLittleEndian(bytes + 4U, 4U) != record.Length || bytes[8U] != record.Revision ||
        std::memcmp(bytes + 16U, record.OemTableId, 8U) != 0 || !IsZero(record.Reserved, sizeof(record.Reserved)))
    {
        Violation("definition metadata matches the bounded firmware table");
    }
    UInt8 sum{0U};
    for (Size index{0U}; index < record.Length; ++index)
    {
        sum = static_cast<UInt8>(sum + bytes[index]);
    }
    if (record.ChecksumValid != (sum == 0U ? 1U : 0U))
    {
        Violation("definition checksum verdict matches the original bytes");
    }
}

void CheckTopology(const AcpiTopology& topology, const bool isIa64, const UInt64 rsdpAddress) noexcept
{
    const UInt8* const rsdp = ResolveArena(rsdpAddress, 20U);
    const bool isXsdt = topology.RootKind == AcpiRootTableKind::Xsdt;
    if ((rsdp == nullptr) || (topology.RsdpPhysicalAddress != rsdpAddress) || (topology.RsdpRevision != rsdp[15U]) ||
        (topology.RsdpRevision == 1U) || (!isXsdt && (topology.RootKind != AcpiRootTableKind::Rsdt)) ||
        (isXsdt && (topology.RsdpRevision == 0U)) || (topology.Reserved != 0U) || (topology.TrailingReserved != 0U))
    {
        Violation("the RSDP fields describe the parsed RSDP");
    }
    if (!HasSignature(topology.RootTablePhysicalAddress, isXsdt ? "XSDT" : "RSDT") ||
        !HasSignature(topology.MadtPhysicalAddress, "APIC"))
    {
        Violation("the root table and MADT lie in ACPI regions with their signatures");
    }
    const UInt8* const madt = ResolveArena(topology.MadtPhysicalAddress, 44U);
    if ((madt == nullptr) || (topology.MadtRevision == 0U) || (topology.MadtRevision != madt[8U]) ||
        (topology.MadtFlags > 1U) || (topology.LocalControllerPhysicalAddress == 0U))
    {
        Violation("the MADT header fields are published unchanged and valid");
    }
    CheckDefinitionRecord(topology.Dsdt, "DSDT");
    if (topology.SsdtCount > Asteria::Boot::Acpi::MaximumAcpiSsdts)
    {
        Violation("SSDT retention stays within its named capacity");
    }
    for (Size index{0U}; index < Asteria::Boot::Acpi::MaximumAcpiSsdts; ++index)
    {
        CheckDefinitionRecord(topology.Ssdts[index], "SSDT");
        if (index >= topology.SsdtCount && topology.Ssdts[index].Length != 0U)
        {
            Violation("unused SSDT records are zero");
        }
    }
    CheckProcessors(topology, isIa64);
    CheckProcessorSelection(topology, isIa64);
    CheckControllers(topology, isIa64);
    CheckPciRegions(topology);
    CheckDmar(topology);
    if ((topology.PmTimerAddress != 0U &&
         (topology.PmTimerSpace > 1U || (topology.PmTimerBits != 24U && topology.PmTimerBits != 32U) ||
          (topology.PmTimerSpace == 1U && topology.PmTimerAddress > 0xFFFCU) ||
          (topology.PmTimerSpace == 0U &&
           ((topology.PmTimerAddress & 3U) != 0U || topology.PmTimerAddress >= (UInt64{1U} << 47U) - 4U)))) ||
        (topology.HpetAddress != 0U &&
         ((topology.HpetAddress & 4095U) != 0U || topology.HpetAddress >= (UInt64{1U} << 47U) - 4096U)) ||
        !IsZero(topology.TimerReserved, sizeof(topology.TimerReserved)))
    {
        Violation("calibration counters have supported space/width/alignment and canonical identity addresses");
    }
}

// Seed construction: tables are written into images of the two arena pages.
struct ArenaImage
{
    UInt8 Pages[2U][PageBytes];
    Size Used[2U];
};

[[nodiscard]] UInt64 AddressOf(const Size page, const Size offset) noexcept
{
    return (page == 0U ? ReclaimableAddress : NonVolatileAddress) + offset;
}

UInt8* PlaceTable(ArenaImage& image, const Size page, const Size offset, const char* const signature,
                  const Size length) noexcept
{
    UInt8* const table = image.Pages[page] + offset;
    std::memcpy(table, signature, 4U);
    WriteLittleEndian(table + 4U, length, 4U);
    table[8U] = 1U;
    std::memcpy(table + 10U, "ASTERI", 6U);
    const Size end = offset + length < PageBytes ? offset + length : PageBytes;
    image.Used[page] = image.Used[page] > end ? image.Used[page] : end;
    return table;
}

void PlaceRoot(ArenaImage& image, const Size offset, const bool xsdt, const UInt64* const entries,
               const Size count) noexcept
{
    const Size entryBytes = xsdt ? 8U : 4U;
    UInt8* const root = PlaceTable(image, 0U, offset, xsdt ? "XSDT" : "RSDT", 36U + (count * entryBytes));
    for (Size index{0U}; index < count; ++index)
    {
        WriteLittleEndian(root + 36U + (index * entryBytes), entries[index], entryBytes);
    }
    StoreChecksum(root, 36U + (count * entryBytes), 9U);
}

void PlaceRsdp(ArenaImage& image, const Size page, const Size offset, const UInt8 revision,
               const UInt64 rootAddress) noexcept
{
    UInt8* const rsdp = image.Pages[page] + offset;
    std::memcpy(rsdp, "RSD PTR ", 8U);
    std::memcpy(rsdp + 9U, "ASTERI", 6U);
    rsdp[15U] = revision;
    WriteLittleEndian(rsdp + 16U, revision == 0U ? rootAddress : 0U, 4U);
    if (revision != 0U)
    {
        WriteLittleEndian(rsdp + 20U, 36U, 4U);
        WriteLittleEndian(rsdp + 24U, rootAddress, 8U);
    }
    StoreChecksum(rsdp, 20U, 8U);
    if (revision != 0U)
    {
        StoreChecksum(rsdp, 36U, 32U);
    }
    const Size end = offset + 36U;
    image.Used[page] = image.Used[page] > end ? image.Used[page] : end;
}

// Writes a MADT whose entries are given as concatenated raw bytes.
void PlaceMadt(ArenaImage& image, const Size page, const Size offset, const UInt32 flags, const UInt8* const entries,
               const Size entryBytes) noexcept
{
    UInt8* const madt = PlaceTable(image, page, offset, "APIC", 44U + entryBytes);
    madt[8U] = 5U;
    WriteLittleEndian(madt + 36U, 0xFEE0'0000U, 4U);
    WriteLittleEndian(madt + 40U, flags, 4U);
    std::memcpy(madt + 44U, entries, entryBytes);
    StoreChecksum(madt, 44U + entryBytes, 9U);
}

void PlaceMcfg(ArenaImage& image, const Size offset, const AcpiPciConfigurationRegion* const regions,
               const Size count) noexcept
{
    UInt8* const mcfg = PlaceTable(image, 0U, offset, "MCFG", 44U + (count * 16U));
    for (Size index{0U}; index < count; ++index)
    {
        UInt8* const allocation = mcfg + 44U + (index * 16U);
        WriteLittleEndian(allocation, regions[index].BaseAddress, 8U);
        WriteLittleEndian(allocation + 8U, regions[index].Segment, 2U);
        allocation[10U] = regions[index].FirstBus;
        allocation[11U] = regions[index].LastBus;
    }
    StoreChecksum(mcfg, 44U + (count * 16U), 9U);
}

void PlaceDmar(ArenaImage& image) noexcept
{
    constexpr Size Length{184U};
    UInt8* const dmar = PlaceTable(image, 0U, 768U, "DMAR", Length);
    dmar[36U] = 38U;
    WriteLittleEndian(dmar + 50U, 24U, 2U);
    WriteLittleEndian(dmar + 56U, 0xFED9'0000U, 8U);
    dmar[64U] = 1U;
    dmar[65U] = 8U;
    dmar[70U] = 2U;
    WriteLittleEndian(dmar + 74U, 16U, 2U);
    dmar[76U] = 1U;
    WriteLittleEndian(dmar + 80U, 0xFED9'1000U, 8U);
    constexpr UInt8 Devices[]{2U, 0x1AU, 0x1DU};
    for (Size index{0U}; index < 3U; ++index)
    {
        UInt8* const rmrr = dmar + 88U + (index * 32U);
        WriteLittleEndian(rmrr, 1U, 2U);
        WriteLittleEndian(rmrr + 2U, 32U, 2U);
        WriteLittleEndian(rmrr + 8U, 0x7C00'0000U, 8U);
        WriteLittleEndian(rmrr + 16U, 0x7C00'1FFFU, 8U);
        rmrr[24U] = 1U;
        rmrr[25U] = 8U;
        rmrr[30U] = Devices[index];
    }
    StoreChecksum(dmar, Length, 9U);
}

constexpr UInt8 Amd64Entries[]{
    0U, 8U,  0U, 2U, 1U,    0U,    0U,    0U,                                       // Local APIC, enabled
    9U, 16U, 0U, 0U, 0x01U, 0x01U, 0U,    0U,    2U,    0U, 0U, 0U, 1U, 0U, 0U, 0U, // x2APIC, online capable
    1U, 12U, 3U, 0U, 0x00U, 0x00U, 0xC0U, 0xFEU, 0U,    0U, 0U, 0U,                 // I/O APIC 3 at 0xFEC00000
    2U, 10U, 0U, 9U, 20U,   0U,    0U,    0U,    0x0FU, 0U,                         // ISA IRQ 9 -> GSI 20
    5U, 12U, 0U, 0U, 0x00U, 0x10U, 0xE0U, 0xFEU, 0U,    0U, 0U, 0U,                 // local APIC address override
};

constexpr UInt8 Ia64Entries[]{
    7U, 17U, 0U, 4U, 1U,    0U,    0U,    0U,    1U,    0U,    0U,    0U,    42U, 0U, 0U, 0U,
    0U,                                                                   // extended local SAPIC, UID 42
    7U, 12U, 7U, 5U, 0U,    0U,    0U,    0U,    1U,    0U,    0U,    0U, // legacy local SAPIC, UID 7
    1U, 12U, 8U, 0U, 0x00U, 0x00U, 0xC0U, 0xFEU, 32U,   0U,    0U,    0U, // I/O APIC 8, replaced below
    6U, 16U, 8U, 0U, 32U,   0U,    0U,    0U,    0x00U, 0x00U, 0xC1U, 0xFEU, 0U,  0U, 0U, 0U, // I/O SAPIC 8
    5U, 12U, 0U, 0U, 0x00U, 0x00U, 0xE1U, 0xFEU, 0U,    0U,    0U,    0U, // local SAPIC address override
};

// Listed out of order so that accepting them exercises the sort.
// The I/O SAPIC precedes the I/O APIC with its identifier, which is ignored.
constexpr UInt8 Ia64SapicFirstEntries[]{
    7U, 12U, 7U, 5U, 0U,    0U,    0U,    0U,    1U,    0U,    0U,    0U, // legacy local SAPIC, UID 7
    6U, 16U, 8U, 0U, 32U,   0U,    0U,    0U,    0x00U, 0x00U, 0xC1U, 0xFEU, 0U, 0U, 0U, 0U, // I/O SAPIC 8
    1U, 12U, 8U, 0U, 0x00U, 0x00U, 0xC0U, 0xFEU, 32U,   0U,    0U,    0U,                    // I/O APIC 8, ignored
};

// Five enabled local APICs, one more than the record retains; the fifth
// entry's flags byte is at FifthProcessorFlagsOffset.
constexpr UInt8 FiveProcessorEntries[]{
    0U, 8U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 8U, 1U, 1U, 1U, 0U, 0U, 0U,  0U, 8U, 2U,    2U,    1U,    0U,    0U, 0U, 0U, 8U,
    3U, 3U, 1U, 0U, 0U, 0U, 0U, 8U, 4U, 4U, 1U, 0U, 0U, 0U, 1U, 12U, 3U, 0U, 0x00U, 0x00U, 0xC0U, 0xFEU, 0U, 0U, 0U, 0U,
};
constexpr Size FifthProcessorFlagsOffset{44U + 32U + 4U};

constexpr AcpiPciConfigurationRegion TwoSegments[]{
    {0x9000'0000U, 1U, 0U, 15U, 0U},
    {0x8000'0000U, 0U, 0U, 255U, 0U},
};
constexpr AcpiPciConfigurationRegion OverlappingBuses[]{
    {0x8000'0000U, 0U, 0U, 127U, 0U},
    {0xA000'0000U, 0U, 64U, 255U, 0U},
};
constexpr AcpiPciConfigurationRegion TopOfAddressSpace[]{
    {0xFFFF'FFFF'FFF0'0000U, 0U, 0U, 1U, 0U},
};

constinit ArenaImage g_seedImage{};
constinit UInt8 g_seedInput[PageBytes]{};

void EmitImage(void (*const emit)(const std::uint8_t* data, std::size_t size), const ArenaImage& image,
               const UInt8 flags, const Size rsdpOffset) noexcept
{
    UInt8* const input = g_seedInput;
    constexpr Size Capacity{PageBytes - InputHeaderBytes};
    const Size reclaimable = image.Used[0U] < Capacity ? image.Used[0U] : Capacity;
    const Size nonVolatile = image.Used[1U] < (Capacity - reclaimable) ? image.Used[1U] : Capacity - reclaimable;
    input[0U] = flags;
    WriteLittleEndian(input + 1U, reclaimable, 2U);
    WriteLittleEndian(input + 3U, rsdpOffset, 2U);
    std::memcpy(input + InputHeaderBytes, image.Pages[0U], reclaimable);
    std::memcpy(input + InputHeaderBytes + reclaimable, image.Pages[1U], nonVolatile);
    emit(input, InputHeaderBytes + reclaimable + nonVolatile);
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    ArenaImage& image = g_seedImage;
    const UInt64 madt = AddressOf(0U, 128U);
    const UInt64 mcfg = AddressOf(0U, 512U);

    // AMD64 through the XSDT with every MADT entry kind and two MCFG segments.
    image = {};
    PlaceMadt(image, 0U, 128U, 1U, Amd64Entries, sizeof(Amd64Entries));
    PlaceMcfg(image, 512U, TwoSegments, 2U);
    const UInt64 amd64Tables[]{madt, mcfg};
    PlaceRoot(image, 64U, true, amd64Tables, 2U);
    PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
    EmitImage(emit, image, 0U, 0U);
    // Multi-unit laptop DMAR seeds exercise both successful retention and
    // optional-table rejection. Repair mode lets mutations reach the parser.
    PlaceDmar(image);
    const UInt64 dmarTables[]{madt, mcfg, AddressOf(0U, 768U)};
    PlaceRoot(image, 64U, true, dmarTables, 3U);
    EmitImage(emit, image, RepairChecksumsFlag, 0U);
    for (Size fault{0U}; fault < 4U; ++fault)
    {
        ArenaImage malformed = image;
        UInt8* const dmar = malformed.Pages[0U] + 768U;
        if (fault == 0U)
        {
            dmar[89U + 24U] = 7U;
        }
        if (fault == 1U)
        {
            dmar[90U] = 23U;
        }
        if (fault == 2U)
        {
            dmar[50U] = 2U;
        }
        if (fault == 3U)
        {
            dmar[70U] = 32U;
        }
        EmitImage(emit, malformed, RepairChecksumsFlag, 0U);
    }
    PlaceRoot(image, 64U, true, amd64Tables, 2U);
    // The same tables with every checksum cleared, parsed in repair mode.
    ArenaImage broken = image;
    broken.Pages[0U][8U] = 0U;
    broken.Pages[0U][32U] = 0U;
    broken.Pages[0U][64U + 9U] = 0U;
    broken.Pages[0U][128U + 9U] = 0U;
    broken.Pages[0U][512U + 9U] = 0U;
    EmitImage(emit, broken, RepairChecksumsFlag, 0U);
    EmitImage(emit, broken, 0U, 0U);
    // Invalid arguments and BootInfo headers on an otherwise valid input.
    constexpr UInt8 InvalidArgumentSelectors[]{0U, 1U, 7U, 10U, 12U};
    for (const UInt8 selector : InvalidArgumentSelectors)
    {
        EmitImage(emit, image, static_cast<UInt8>(InvalidArgumentFlag | (selector << 4U)), 0U);
    }
    // The IA-64 profile with legacy and extended local SAPICs and an I/O APIC
    // that the I/O SAPIC with the same identifier supersedes.
    image = {};
    PlaceMadt(image, 0U, 128U, 0U, Ia64Entries, sizeof(Ia64Entries));
    PlaceMcfg(image, 512U, TwoSegments, 1U);
    PlaceRoot(image, 64U, true, amd64Tables, 2U);
    PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
    EmitImage(emit, image, Ia64Flag, 0U);
    EmitImage(emit, image, 0U, 0U);
    // Revision 0 RSDP with an RSDT and no MCFG.
    image = {};
    PlaceMadt(image, 0U, 128U, 1U, Amd64Entries, sizeof(Amd64Entries));
    PlaceRoot(image, 64U, false, amd64Tables, 1U);
    PlaceRsdp(image, 0U, 0U, 0U, AddressOf(0U, 64U));
    EmitImage(emit, image, 0U, 0U);
    // MADT in the non-volatile region, a duplicated MCFG, and the RSDP moved
    // to the non-volatile page.
    image = {};
    PlaceMadt(image, 1U, 256U, 1U, Amd64Entries, sizeof(Amd64Entries));
    PlaceMcfg(image, 512U, TwoSegments, 2U);
    const UInt64 duplicateTables[]{AddressOf(1U, 256U), mcfg, mcfg};
    PlaceRoot(image, 64U, true, duplicateTables, 3U);
    PlaceRsdp(image, 1U, 64U, 2U, AddressOf(0U, 64U));
    EmitImage(emit, image, NonVolatileRsdpFlag, 64U);
    // Overlapping MCFG buses and an MCFG region past the top of the address
    // space: the MCFG is dropped, the topology accepted.
    image = {};
    PlaceMadt(image, 0U, 128U, 1U, Amd64Entries, sizeof(Amd64Entries));
    PlaceMcfg(image, 512U, OverlappingBuses, 2U);
    PlaceRoot(image, 64U, true, amd64Tables, 2U);
    PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
    EmitImage(emit, image, 0U, 0U);
    PlaceMcfg(image, 512U, TopOfAddressSpace, 1U);
    EmitImage(emit, image, RepairChecksumsFlag, 0U);
    // The IA-64 profile with the I/O SAPIC before the I/O APIC it supersedes.
    image = {};
    PlaceMadt(image, 0U, 128U, 0U, Ia64SapicFirstEntries, sizeof(Ia64SapicFirstEntries));
    PlaceRoot(image, 64U, true, amd64Tables, 1U);
    PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
    EmitImage(emit, image, Ia64Flag, 0U);
    // One enabled processor beyond the capacity, which is counted as ignored,
    // then the same MADT with the fifth processor unusable, which is not.
    image = {};
    PlaceMadt(image, 0U, 128U, 1U, FiveProcessorEntries, sizeof(FiveProcessorEntries));
    PlaceRoot(image, 64U, true, amd64Tables, 1U);
    PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
    EmitImage(emit, image, 0U, 0U);
    image.Pages[0U][128U + FifthProcessorFlagsOffset] = 0U;
    EmitImage(emit, image, RepairChecksumsFlag, 0U);
    // Optional counter tables: valid extended, legacy, malformed and duplicate
    // GAS metadata remain subject to atomicity and canonical topology checks.
    for (UInt8 variant{0U}; variant < 5U; ++variant)
    {
        image = {};
        PlaceMadt(image, 0U, 256U, 1U, Amd64Entries, sizeof(Amd64Entries));
        UInt8* const fadt = PlaceTable(image, 0U, 1024U, "FACP", 220U);
        fadt[8U] = 3U;
        WriteLittleEndian(fadt + 76U, 0x408U, 4U);
        fadt[91U] = 4U;
        fadt[208U] = variant == 1U ? 0U : 1U;
        fadt[209U] = 32U;
        fadt[210U] = variant == 2U ? 1U : 0U;
        WriteLittleEndian(fadt + 212U, variant == 1U ? 0xFED0'1000U : 0x508U, 8U);
        UInt8* const hpet = PlaceTable(image, 1U, 0U, "HPET", 56U);
        WriteLittleEndian(hpet + 44U, variant == 3U ? 1ULL << 47U : 0xFED0'0000U, 8U);
        const UInt64 timers[]{AddressOf(0U, 256U), AddressOf(0U, 1024U), AddressOf(1U, 0U), AddressOf(0U, 1024U)};
        PlaceRoot(image, 64U, true, timers, variant == 4U ? 4U : 3U);
        PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
        EmitImage(emit, image, RepairChecksumsFlag, 0U);
    }
    // FADT-selected DSDT and multiple SSDTs, including retained bad checksums.
    for (UInt8 variant{0U}; variant < 4U; ++variant)
    {
        image = {};
        PlaceMadt(image, 0U, 256U, 1U, Amd64Entries, sizeof(Amd64Entries));
        UInt8* const fadt = PlaceTable(image, 0U, 1024U, "FACP", 148U);
        fadt[8U] = 6U;
        WriteLittleEndian(fadt + 40U, AddressOf(0U, 1280U), 4U);
        WriteLittleEndian(fadt + 140U, variant == 0U ? 0U : AddressOf(1U, 128U), 8U);
        static_cast<void>(PlaceTable(image, 0U, 1280U, "DSDT", 40U));
        UInt8* const dsdt = PlaceTable(image, 1U, 128U, "DSDT", 40U);
        UInt8* const ssdt = PlaceTable(image, 1U, 256U, "SSDT", 40U);
        if (variant == 2U)
        {
            dsdt[9U] ^= 1U;
            ssdt[9U] ^= 1U;
        }
        if (variant == 3U)
        {
            WriteLittleEndian(dsdt + 4U, 1'048'577U, 4U);
        }
        StoreChecksum(fadt, 148U, 9U);
        const UInt64 tables[]{AddressOf(0U, 256U), AddressOf(0U, 1024U), AddressOf(1U, 256U), AddressOf(1U, 256U)};
        PlaceRoot(image, 64U, true, tables, 4U);
        PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
        EmitImage(emit, image, 0U, 0U);
    }
    // A MADT whose declared length runs past its page into the hole.
    image = {};
    PlaceMadt(image, 0U, 3968U, 1U, Amd64Entries, sizeof(Amd64Entries));
    image.Pages[0U][3968U + 4U] = 200U;
    const UInt64 edgeTables[]{AddressOf(0U, 3968U)};
    PlaceRoot(image, 64U, true, edgeTables, 1U);
    PlaceRsdp(image, 0U, 0U, 2U, AddressOf(0U, 64U));
    EmitImage(emit, image, RepairChecksumsFlag, 0U);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    UInt8* const arena = GetArena();
    UInt8* const reclaimable = arena;
    UInt8* const nonVolatile = arena + (2U * PageBytes);
    UInt8 header[InputHeaderBytes]{};
    if (size != 0U)
    {
        std::memcpy(header, data, size < InputHeaderBytes ? size : InputHeaderBytes);
    }
    const UInt8 flags = header[0U];
    const Size payloadBytes = size > InputHeaderBytes ? size - InputHeaderBytes : 0U;
    const UInt8* const payload = data + (size > InputHeaderBytes ? InputHeaderBytes : size);
    Size reclaimableBytes = static_cast<Size>(ReadLittleEndian(header + 1U, 2U));
    reclaimableBytes = reclaimableBytes < payloadBytes ? reclaimableBytes : payloadBytes;
    reclaimableBytes = reclaimableBytes < PageBytes ? reclaimableBytes : PageBytes;
    const Size remainingBytes = payloadBytes - reclaimableBytes;
    std::memset(reclaimable, 0, PageBytes);
    std::memset(nonVolatile, 0, PageBytes);
    std::memcpy(reclaimable, payload, reclaimableBytes);
    std::memcpy(nonVolatile, payload + reclaimableBytes, remainingBytes < PageBytes ? remainingBytes : PageBytes);

    const Size rsdpOffset = static_cast<Size>(ReadLittleEndian(header + 3U, 2U)) & (PageBytes - 1U);
    const UIntPointer rsdpPage = (flags & NonVolatileRsdpFlag) != 0U ? NonVolatileAddress : ReclaimableAddress;
    const UInt64 rsdpAddress = rsdpPage + rsdpOffset;
    if ((flags & RepairChecksumsFlag) != 0U)
    {
        RepairChecksums(rsdpAddress);
    }

    ResetBootInfo();
    const bool isIa64 = (flags & Ia64Flag) != 0U;
    AcpiTargetArchitecture architecture = isIa64 ? AcpiTargetArchitecture::Ia64 : AcpiTargetArchitecture::Amd64;
    const void* rsdp = reinterpret_cast<const void*>(static_cast<UIntPointer>(rsdpAddress));
    AcpiTableParseError expectedError{AcpiTableParseError::None};
    if ((flags & InvalidArgumentFlag) != 0U)
    {
        expectedError = InjectInvalidArgument(static_cast<UInt8>(flags >> 4U), rsdp, architecture);
    }

    // AcpiTopology has no padding (its layout is pinned by static_assert),
    // so byte comparisons detect any write.
    const AcpiTopology sentinel = MakeSentinel();
    AcpiTopology independent = sentinel;
    const AcpiTableParseError error = AcpiTableParser::Parse(g_bootInfo, rsdp, architecture, independent);
    g_bootInfo.Acpi = sentinel;
    const AcpiTableParseError aliasedError = AcpiTableParser::Parse(g_bootInfo, rsdp, architecture, g_bootInfo.Acpi);
    if ((aliasedError != error) || (std::memcmp(&g_bootInfo.Acpi, &independent, sizeof(independent)) != 0))
    {
        Violation("parsing into bootInfo.Acpi gives the same result as an independent output");
    }
    if ((expectedError != AcpiTableParseError::None) && (error != expectedError))
    {
        Violation("invalid arguments and BootInfo headers are rejected with their error");
    }
    if (error != AcpiTableParseError::None)
    {
        if (std::memcmp(&independent, &sentinel, sizeof(sentinel)) != 0)
        {
            Violation("a rejected input leaves the output unchanged");
        }
        return 0;
    }
    CheckTopology(independent, isIa64, rsdpAddress);
    return 0;
}
