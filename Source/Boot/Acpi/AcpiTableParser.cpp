// SPDX-License-Identifier: Apache-2.0

#include "Boot/Acpi/AcpiTableParser.hpp"

#include "Boot/Acpi/AcpiDefinitionBlocks.hpp"

namespace Asteria::Boot::Acpi
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 MaximumPageCount{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt32 ExpectedFirmwareDescriptorVersion{1U};
constexpr Foundation::Size RsdpV1SizeBytes{20U};
constexpr Foundation::Size RsdpV2MinimumSizeBytes{36U};
constexpr Foundation::Size MaximumRsdpSizeBytes{4096U};
constexpr Foundation::Size SdtHeaderSizeBytes{36U};
constexpr Foundation::Size MadtHeaderSizeBytes{44U};
constexpr Foundation::Size McfgHeaderSizeBytes{44U};
constexpr Foundation::Size McfgAllocationSizeBytes{16U};
constexpr Foundation::Size DmarHeaderSizeBytes{48U};
constexpr Foundation::Size DmarStructureHeaderSizeBytes{4U};
constexpr Foundation::Size DmarUnitMinimumSizeBytes{16U};
constexpr Foundation::Size DmarReservedMemoryMinimumSizeBytes{24U};
constexpr Foundation::Size DmarScopeHeaderSizeBytes{6U};
constexpr Foundation::Size MaximumDmarStructures{256U};
constexpr Foundation::UInt16 DmarUnitType{0U};
constexpr Foundation::UInt16 DmarReservedMemoryType{1U};
constexpr Foundation::Size MaximumAcpiTableSizeBytes{1024U * 1024U};
constexpr Foundation::Size MaximumMadtEntries{256U};
constexpr Foundation::UInt64 PciBusConfigurationSizeBytes{Foundation::UInt64{1U} << 20U};
constexpr Foundation::UInt32 MadtPcatCompatibleBit{1U};
constexpr Foundation::UInt32 ProcessorEnabledBit{1U};
constexpr Foundation::UInt32 ProcessorOnlineCapableBit{1U << 1U};
constexpr Foundation::UInt32 ProcessorKnownFlags{ProcessorEnabledBit | ProcessorOnlineCapableBit};

constexpr Foundation::UInt8 MadtLocalApicType{0U};
constexpr Foundation::UInt8 MadtIoApicType{1U};
constexpr Foundation::UInt8 MadtInterruptSourceOverrideType{2U};
constexpr Foundation::UInt8 MadtLocalApicAddressOverrideType{5U};
constexpr Foundation::UInt8 MadtIoSapicType{6U};
constexpr Foundation::UInt8 MadtLocalSapicType{7U};
constexpr Foundation::UInt8 MadtLocalX2ApicType{9U};

struct ValidationState
{
    const Foundation::UInt8* Madt;
    const Foundation::UInt8* Mcfg;
    const Foundation::UInt8* Dmar;
    Foundation::UInt16 IapcBootArchitecture;
    Foundation::UInt8 FadtRevision;
    Foundation::UInt64 PmTimerAddress;
    Foundation::UInt64 HpetAddress;
    Foundation::UInt8 PmTimerSpace;
    Foundation::UInt8 PmTimerBits;
    Foundation::Size MadtLength;
    Foundation::Size McfgLength;
    Foundation::Size DmarLength;
    Foundation::UInt64 RsdpPhysicalAddress;
    Foundation::UInt64 RootTablePhysicalAddress;
    Foundation::UInt64 MadtPhysicalAddress;
    Foundation::UInt64 McfgPhysicalAddress;
    Foundation::UInt64 DmarPhysicalAddress;
    Foundation::UInt64 IvrsPhysicalAddress;
    Foundation::UInt64 LocalControllerPhysicalAddress;
    AcpiRootTableKind RootKind;
    Foundation::UInt32 RsdpRevision;
    Foundation::UInt32 MadtRevision;
    Foundation::UInt32 MadtFlags;
    Foundation::UInt32 BspApicId;
    Foundation::UInt32 ProcessorCount;
    Foundation::UInt32 EnabledProcessorCount;
    Foundation::UInt32 IgnoredProcessorCount;
    Foundation::UInt32 IoControllerCount;
    Foundation::UInt32 InterruptOverrideCount;
    Foundation::UInt32 LocalControllerOverrideCount;
    // MADT offsets of the retained processor entries, in firmware order.
    Foundation::UInt32 ProcessorEntryOffsets[MaximumAcpiProcessors];
    Foundation::UInt64 IoControllerPhysicalAddresses[MaximumAcpiIoControllers];
    Foundation::UInt32 IoControllerGlobalInterruptBases[MaximumAcpiIoControllers];
    Foundation::UInt8 IoControllerIds[MaximumAcpiIoControllers];
    Foundation::UInt8 IoControllerKinds[MaximumAcpiIoControllers];
    Foundation::UInt32 InterruptOverrideGlobalInterrupts[MaximumAcpiInterruptOverrides];
    // Validated ISA source and polarity/trigger flags each fit one nibble.
    Foundation::UInt8 InterruptOverrideSourcesAndFlags[MaximumAcpiInterruptOverrides];
};

[[nodiscard]] Foundation::UInt16 ReadLittleEndian16(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt16>(bytes[0U]) |
           static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[1U]) << 8U);
}

[[nodiscard]] Foundation::UInt32 ReadLittleEndian32(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt32>(bytes[0U]) | (static_cast<Foundation::UInt32>(bytes[1U]) << 8U) |
           (static_cast<Foundation::UInt32>(bytes[2U]) << 16U) | (static_cast<Foundation::UInt32>(bytes[3U]) << 24U);
}

[[nodiscard]] Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool BytesEqual(const Foundation::UInt8* const bytes, const char* const expected,
                              const Foundation::Size byteCount) noexcept
{
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        if (bytes[index] != static_cast<Foundation::UInt8>(expected[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ChecksumIsZero(const Foundation::UInt8* const bytes, const Foundation::Size byteCount) noexcept
{
    Foundation::UInt8 sum{0U};
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        sum = static_cast<Foundation::UInt8>(sum + bytes[index]);
    }
    return sum == 0U;
}

[[nodiscard]] bool TryGetRegionEnd(const PhysicalMemoryRegion& region, Foundation::UInt64& end) noexcept
{
    if ((region.PageCount == 0U) || (region.PageCount > MaximumPageCount) ||
        ((region.PhysicalStart & (PageSizeBytes - 1U)) != 0U))
    {
        return false;
    }
    if (region.PageCount == MaximumPageCount)
    {
        if (region.PhysicalStart != 0U)
        {
            return false;
        }
        end = MaximumUInt64;
        return true;
    }
    const Foundation::UInt64 byteCount = region.PageCount << 12U;
    if (region.PhysicalStart > (MaximumUInt64 - (byteCount - 1U)))
    {
        return false;
    }
    end = region.PhysicalStart + byteCount - 1U;
    return true;
}

[[nodiscard]] bool TryGetPciConfigurationRegionBounds(const AcpiPciConfigurationRegion& configurationRegion,
                                                      Foundation::UInt64& firstAddress,
                                                      Foundation::UInt64& lastAddress) noexcept
{
    if (((configurationRegion.BaseAddress & (PciBusConfigurationSizeBytes - 1U)) != 0U) ||
        (configurationRegion.FirstBus > configurationRegion.LastBus) || (configurationRegion.Reserved != 0U))
    {
        return false;
    }

    const Foundation::UInt64 firstOffset = static_cast<Foundation::UInt64>(configurationRegion.FirstBus) << 20U;
    const Foundation::UInt64 lastOffset =
        ((static_cast<Foundation::UInt64>(configurationRegion.LastBus) + 1U) << 20U) - 1U;
    if ((configurationRegion.BaseAddress > (MaximumUInt64 - firstOffset)) ||
        (configurationRegion.BaseAddress > (MaximumUInt64 - lastOffset)))
    {
        return false;
    }
    firstAddress = configurationRegion.BaseAddress + firstOffset;
    lastAddress = configurationRegion.BaseAddress + lastOffset;
    return true;
}

[[nodiscard]] AcpiPciConfigurationRegion ReadPciConfigurationRegion(const Foundation::UInt8* const allocation) noexcept
{
    return AcpiPciConfigurationRegion{
        .BaseAddress = ReadLittleEndian64(allocation),
        .Segment = ReadLittleEndian16(allocation + 8U),
        .FirstBus = allocation[10U],
        .LastBus = allocation[11U],
        .Reserved = ReadLittleEndian32(allocation + 12U),
    };
}

[[nodiscard]] bool BootInfoIsValid(const BootInfo& bootInfo) noexcept
{
    if ((bootInfo.Signature != BootInfoSignature) || (bootInfo.Version != BootInfoVersion) ||
        (bootInfo.SizeBytes != sizeof(BootInfo)) || (bootInfo.MemoryRegionCount == 0U) ||
        (bootInfo.MemoryRegionCount > MaximumMemoryRegions) ||
        (bootInfo.FirmwareDescriptorVersion != ExpectedFirmwareDescriptorVersion) ||
        (bootInfo.FirmwareDescriptorSize < 40U) || (bootInfo.Reserved != 0U) ||
        (bootInfo.FirmwareMemoryMapSizeBytes !=
         (static_cast<Foundation::UInt64>(bootInfo.MemoryRegionCount) * bootInfo.FirmwareDescriptorSize)))
    {
        return false;
    }

    Foundation::UInt64 previousEnd{0U};
    for (Foundation::Size index{0U}; index < bootInfo.MemoryRegionCount; ++index)
    {
        Foundation::UInt64 regionEnd{0U};
        const PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        if (!TryGetRegionEnd(region, regionEnd) ||
            ((region.Kind == PhysicalMemoryKind::AcpiReclaimable) && (region.FirmwareType != 9U)) ||
            ((region.Kind == PhysicalMemoryKind::AcpiNonVolatile) && (region.FirmwareType != 10U)) ||
            ((index != 0U) && (region.PhysicalStart <= previousEnd)))
        {
            return false;
        }
        previousEnd = regionEnd;
    }
    return true;
}

// Whether the kernel's allocators or the firmware's may hand out or rewrite
// memory of this kind once the boot goes on: tables there could change while
// the parser or the boot still relies on them. An unknown kind counts as
// reusable.
[[nodiscard]] constexpr bool IsReusableMemory(const PhysicalMemoryKind kind) noexcept
{
    switch (kind)
    {
    case PhysicalMemoryKind::Conventional:
    case PhysicalMemoryKind::Loader:
    case PhysicalMemoryKind::BootServices:
    case PhysicalMemoryKind::Persistent:
    case PhysicalMemoryKind::Unaccepted:
        return true;
    case PhysicalMemoryKind::Reserved:
    case PhysicalMemoryKind::RuntimeServices:
    case PhysicalMemoryKind::Unusable:
    case PhysicalMemoryKind::AcpiReclaimable:
    case PhysicalMemoryKind::AcpiNonVolatile:
    case PhysicalMemoryKind::MemoryMappedIo:
    case PhysicalMemoryKind::PalCode:
        return false;
    }
    return true;
}

// The bytes of [physicalAddress, physicalAddress + byteCount) when no byte
// lies in a reusable region of the map (see the header's placement rule):
// firmware keeps tables in ACPI memory, but also in reserved or runtime
// memory and in ranges the map leaves out, such as the legacy BIOS area.
[[nodiscard]] bool TryResolvePhysicalRange(const BootInfo& bootInfo, const Foundation::UInt64 physicalAddress,
                                           const Foundation::Size byteCount, const Foundation::UInt8*& bytes) noexcept
{
    if ((physicalAddress == 0U) || (byteCount == 0U) ||
        (static_cast<Foundation::UInt64>(byteCount) > (MaximumUInt64 - physicalAddress + 1U)))
    {
        return false;
    }
    const Foundation::UInt64 lastAddress = physicalAddress + static_cast<Foundation::UInt64>(byteCount) - 1U;
    for (Foundation::Size index{0U}; index < bootInfo.MemoryRegionCount; ++index)
    {
        const PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        if (!IsReusableMemory(region.Kind))
        {
            continue;
        }
        Foundation::UInt64 regionEnd{0U};
        if (!TryGetRegionEnd(region, regionEnd) ||
            ((physicalAddress <= regionEnd) && (lastAddress >= region.PhysicalStart)))
        {
            return false;
        }
    }
    bytes = reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(physicalAddress));
    return true;
}

[[nodiscard]] bool TryResolveSdtBytes(const BootInfo& bootInfo, const Foundation::UInt64 physicalAddress,
                                      const Foundation::UInt8*& table, Foundation::Size& tableLength) noexcept
{
    const Foundation::UInt8* header{nullptr};
    if (!TryResolvePhysicalRange(bootInfo, physicalAddress, SdtHeaderSizeBytes, header))
    {
        return false;
    }
    const Foundation::UInt32 length = ReadLittleEndian32(header + 4U);
    if ((length < SdtHeaderSizeBytes) || (length > MaximumAcpiTableSizeBytes) ||
        !TryResolvePhysicalRange(bootInfo, physicalAddress, static_cast<Foundation::Size>(length), table))
    {
        return false;
    }
    tableLength = static_cast<Foundation::Size>(length);
    return true;
}

[[nodiscard]] bool TryResolveSdt(const BootInfo& info, const Foundation::UInt64 address,
                                 const Foundation::UInt8*& bytes, Foundation::Size& length) noexcept
{
    return TryResolveSdtBytes(info, address, bytes, length) && ChecksumIsZero(bytes, length);
}

[[nodiscard]] AcpiTableRecord TableRecord(const Foundation::UInt64 address, const Foundation::UInt8* bytes,
                                          const Foundation::Size length) noexcept
{
    AcpiTableRecord record{};
    record.PhysicalAddress = address;
    record.Length = static_cast<Foundation::UInt32>(length);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        record.Signature[index] = static_cast<char>(bytes[index]);
    }
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        record.OemTableId[index] = static_cast<char>(bytes[16U + index]);
    }
    record.Revision = bytes[8U];
    record.ChecksumValid = ChecksumIsZero(bytes, length) ? 1U : 0U;
    return record;
}

[[nodiscard]] Foundation::UInt64 DsdtAddress(const Foundation::UInt8* fadt, const Foundation::Size length) noexcept
{
    if (length >= 148U)
    {
        const Foundation::UInt64 extended = ReadLittleEndian64(fadt + 140U);
        if (extended != 0U)
        {
            return extended;
        }
    }
    return length >= 44U ? ReadLittleEndian32(fadt + 40U) : 0U;
}

[[nodiscard]] bool ProcessorFlagsAreValid(const Foundation::UInt32 flags) noexcept
{
    return ((flags & ~ProcessorKnownFlags) == 0U) &&
           (((flags & ProcessorEnabledBit) == 0U) || ((flags & ProcessorOnlineCapableBit) == 0U));
}

struct ProcessorEntry
{
    Foundation::UInt32 AcpiUid;
    Foundation::UInt32 HardwareId;
    Foundation::UInt32 HardwareEid;
    Foundation::UInt32 Flags;
    AcpiProcessorControllerKind ControllerKind;
};

// Decodes a processor entry whose type and length ValidateMadtEntry accepted.
[[nodiscard]] ProcessorEntry ReadProcessorEntry(const Foundation::UInt8* const entry) noexcept
{
    switch (entry[0U])
    {
    case MadtLocalApicType:
        return ProcessorEntry{
            .AcpiUid = entry[2U],
            .HardwareId = entry[3U],
            .HardwareEid = 0U,
            .Flags = ReadLittleEndian32(entry + 4U),
            .ControllerKind = AcpiProcessorControllerKind::LocalApic,
        };
    case MadtLocalSapicType:
        // The legacy 12-byte ACPI 2.0 form has no UID field; its
        // ProcessorId serves as the UID.
        return ProcessorEntry{
            .AcpiUid = entry[1U] == 12U ? entry[2U] : ReadLittleEndian32(entry + 12U),
            .HardwareId = entry[3U],
            .HardwareEid = entry[4U],
            .Flags = ReadLittleEndian32(entry + 8U),
            .ControllerKind = AcpiProcessorControllerKind::LocalSapic,
        };
    case MadtLocalX2ApicType:
        return ProcessorEntry{
            .AcpiUid = ReadLittleEndian32(entry + 12U),
            .HardwareId = ReadLittleEndian32(entry + 4U),
            .HardwareEid = 0U,
            .Flags = ReadLittleEndian32(entry + 8U),
            .ControllerKind = AcpiProcessorControllerKind::LocalX2Apic,
        };
    default:
        // Not a processor entry: no flags, so never usable.
        return ProcessorEntry{};
    }
}

[[nodiscard]] bool IsProcessorEnabled(const ValidationState& state, const Foundation::UInt32 entryOffset) noexcept
{
    return (ReadProcessorEntry(state.Madt + entryOffset).Flags & ProcessorEnabledBit) != 0U;
}

// Some firmware describes a low-ID CPU twice. Prefer its legacy entry even
// when it occurs later; every scan checks framing before reading an ID.
[[nodiscard]] bool HasLegacyApicEntry(const ValidationState& state, const Foundation::UInt32 id) noexcept
{
    Foundation::Size offset{MadtHeaderSizeBytes};
    for (Foundation::Size entries{0U}; entries < MaximumMadtEntries && offset < state.MadtLength; ++entries)
    {
        if (state.MadtLength - offset < 2U)
        {
            return false;
        }
        const Foundation::Size length = state.Madt[offset + 1U];
        if (length < 2U || length > state.MadtLength - offset)
        {
            return false;
        }
        if (state.Madt[offset] == MadtLocalApicType && length == 8U && state.Madt[offset + 3U] == id)
        {
            return true;
        }
        offset += length;
    }
    return false;
}

// Retains at most MaximumAcpiProcessors usable processors in firmware order:
// the first enabled processors, and online-capable ones only in slots no
// enabled processor needs. When the record is full, an enabled processor
// replaces the last retained online-capable one; any other usable processor
// is ignored and counted.
[[nodiscard]] AcpiTableParseError AddProcessor(ValidationState& state, const Foundation::Size entryOffset) noexcept
{
    const ProcessorEntry processor = ReadProcessorEntry(state.Madt + entryOffset);
    if (!ProcessorFlagsAreValid(processor.Flags))
    {
        return AcpiTableParseError::InvalidControllerEntry;
    }
    if (processor.ControllerKind == AcpiProcessorControllerKind::LocalX2Apic && processor.HardwareId < 255U &&
        HasLegacyApicEntry(state, processor.HardwareId))
    {
        return AcpiTableParseError::None;
    }
    if ((processor.Flags & ProcessorKnownFlags) == 0U)
    {
        return AcpiTableParseError::None;
    }
    for (Foundation::Size index{0U}; index < state.ProcessorCount; ++index)
    {
        const ProcessorEntry retained = ReadProcessorEntry(state.Madt + state.ProcessorEntryOffsets[index]);
        if ((retained.AcpiUid == processor.AcpiUid) ||
            ((retained.HardwareId == processor.HardwareId) && (retained.HardwareEid == processor.HardwareEid)))
        {
            return AcpiTableParseError::DuplicateProcessor;
        }
    }
    const bool enabled = (processor.Flags & ProcessorEnabledBit) != 0U;
    if (enabled)
    {
        ++state.EnabledProcessorCount;
    }
    const auto offset = static_cast<Foundation::UInt32>(entryOffset);
    if (state.ProcessorCount < MaximumAcpiProcessors)
    {
        state.ProcessorEntryOffsets[state.ProcessorCount] = offset;
        ++state.ProcessorCount;
        return AcpiTableParseError::None;
    }
    ++state.IgnoredProcessorCount;
    if (!enabled)
    {
        return AcpiTableParseError::None;
    }
    for (Foundation::Size index{MaximumAcpiProcessors}; index != 0U; --index)
    {
        const ProcessorEntry retained = ReadProcessorEntry(state.Madt + state.ProcessorEntryOffsets[index - 1U]);
        if (retained.HardwareId != state.BspApicId &&
            (!IsProcessorEnabled(state, state.ProcessorEntryOffsets[index - 1U]) ||
             processor.HardwareId == state.BspApicId))
        {
            for (Foundation::Size next{index}; next < MaximumAcpiProcessors; ++next)
            {
                state.ProcessorEntryOffsets[next - 1U] = state.ProcessorEntryOffsets[next];
            }
            state.ProcessorEntryOffsets[MaximumAcpiProcessors - 1U] = offset;
            return AcpiTableParseError::None;
        }
    }
    return AcpiTableParseError::None;
}

[[nodiscard]] Foundation::Size FindIoController(const ValidationState& state,
                                                const Foundation::UInt8 controllerId) noexcept
{
    for (Foundation::Size index{0U}; index < state.IoControllerCount; ++index)
    {
        if (state.IoControllerIds[index] == controllerId)
        {
            return index;
        }
    }
    return MaximumAcpiIoControllers;
}

[[nodiscard]] bool IoControllerConflicts(const ValidationState& state, const Foundation::Size ignoredIndex,
                                         const Foundation::UInt64 physicalAddress,
                                         const Foundation::UInt32 globalInterruptBase) noexcept
{
    for (Foundation::Size index{0U}; index < state.IoControllerCount; ++index)
    {
        if ((index != ignoredIndex) && ((state.IoControllerPhysicalAddresses[index] == physicalAddress) ||
                                        (state.IoControllerGlobalInterruptBases[index] == globalInterruptBase)))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] AcpiTableParseError AddIoController(ValidationState& state, const Foundation::UInt8 controllerId,
                                                  const Foundation::UInt64 physicalAddress,
                                                  const Foundation::UInt32 globalInterruptBase,
                                                  const AcpiIoControllerKind kind) noexcept
{
    if (physicalAddress == 0U)
    {
        return AcpiTableParseError::InvalidControllerEntry;
    }
    const Foundation::Size existing = FindIoController(state, controllerId);
    if (existing != MaximumAcpiIoControllers)
    {
        const AcpiIoControllerKind existingKind = static_cast<AcpiIoControllerKind>(state.IoControllerKinds[existing]);
        if ((existingKind == AcpiIoControllerKind::IoSapic) && (kind == AcpiIoControllerKind::IoApic))
        {
            return AcpiTableParseError::None;
        }
        if ((existingKind != AcpiIoControllerKind::IoApic) || (kind != AcpiIoControllerKind::IoSapic) ||
            IoControllerConflicts(state, existing, physicalAddress, globalInterruptBase))
        {
            return AcpiTableParseError::DuplicateIoController;
        }
        state.IoControllerPhysicalAddresses[existing] = physicalAddress;
        state.IoControllerGlobalInterruptBases[existing] = globalInterruptBase;
        state.IoControllerKinds[existing] = static_cast<Foundation::UInt8>(kind);
        return AcpiTableParseError::None;
    }
    if ((state.IoControllerCount >= MaximumAcpiIoControllers) ||
        IoControllerConflicts(state, MaximumAcpiIoControllers, physicalAddress, globalInterruptBase))
    {
        return state.IoControllerCount >= MaximumAcpiIoControllers ? AcpiTableParseError::IoControllerCapacityExceeded
                                                                   : AcpiTableParseError::DuplicateIoController;
    }
    const Foundation::Size outputIndex = state.IoControllerCount;
    state.IoControllerPhysicalAddresses[outputIndex] = physicalAddress;
    state.IoControllerGlobalInterruptBases[outputIndex] = globalInterruptBase;
    state.IoControllerIds[outputIndex] = controllerId;
    state.IoControllerKinds[outputIndex] = static_cast<Foundation::UInt8>(kind);
    ++state.IoControllerCount;
    return AcpiTableParseError::None;
}

[[nodiscard]] bool InterruptFlagsAreValid(const Foundation::UInt16 flags) noexcept
{
    const Foundation::UInt16 polarity = flags & 3U;
    const Foundation::UInt16 triggerMode = static_cast<Foundation::UInt16>((flags >> 2U) & 3U);
    return ((flags & 0xFFF0U) == 0U) && (polarity != 2U) && (triggerMode != 2U);
}

[[nodiscard]] AcpiTableParseError AddInterruptOverride(ValidationState& state, const Foundation::UInt8 bus,
                                                       const Foundation::UInt8 source,
                                                       const Foundation::UInt32 globalInterrupt,
                                                       const Foundation::UInt16 flags) noexcept
{
    if ((bus != 0U) || (source > 15U) || !InterruptFlagsAreValid(flags))
    {
        return AcpiTableParseError::InvalidControllerEntry;
    }
    if (state.InterruptOverrideCount >= MaximumAcpiInterruptOverrides)
    {
        return AcpiTableParseError::InterruptOverrideCapacityExceeded;
    }
    for (Foundation::Size index{0U}; index < state.InterruptOverrideCount; ++index)
    {
        if (((state.InterruptOverrideSourcesAndFlags[index] & 0xFU) == source) ||
            (state.InterruptOverrideGlobalInterrupts[index] == globalInterrupt))
        {
            return AcpiTableParseError::DuplicateInterruptOverride;
        }
    }
    const Foundation::Size outputIndex = state.InterruptOverrideCount;
    state.InterruptOverrideSourcesAndFlags[outputIndex] = static_cast<Foundation::UInt8>((flags << 4U) | source);
    state.InterruptOverrideGlobalInterrupts[outputIndex] = globalInterrupt;
    ++state.InterruptOverrideCount;
    return AcpiTableParseError::None;
}

[[nodiscard]] bool HasNullTerminator(const Foundation::UInt8* const bytes, const Foundation::Size byteCount) noexcept
{
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        if (bytes[index] == 0U)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] AcpiTableParseError ValidateMadtEntry(ValidationState& state, const Foundation::Size entryOffset,
                                                    const Foundation::Size length,
                                                    const AcpiTargetArchitecture targetArchitecture) noexcept
{
    const Foundation::UInt8* const entry = state.Madt + entryOffset;
    if ((targetArchitecture == AcpiTargetArchitecture::Amd64) && (entry[0U] == MadtIoSapicType))
    {
        return AcpiTableParseError::None;
    }
    switch (entry[0U])
    {
    case MadtLocalApicType:
        if (length != 8U)
        {
            return AcpiTableParseError::InvalidControllerEntry;
        }
        if (targetArchitecture != AcpiTargetArchitecture::Amd64)
        {
            return AcpiTableParseError::UnsupportedInterruptModel;
        }
        return AddProcessor(state, entryOffset);
    case MadtIoApicType:
        if ((length != 12U) || (entry[3U] != 0U) ||
            ((targetArchitecture == AcpiTargetArchitecture::Amd64) &&
             ((ReadLittleEndian32(entry + 4U) & 0xFFFU) != 0U)))
        {
            return AcpiTableParseError::InvalidControllerEntry;
        }
        return AddIoController(state, entry[2U], ReadLittleEndian32(entry + 4U), ReadLittleEndian32(entry + 8U),
                               AcpiIoControllerKind::IoApic);
    case MadtInterruptSourceOverrideType:
        if (length != 10U)
        {
            return AcpiTableParseError::InvalidControllerEntry;
        }
        return AddInterruptOverride(state, entry[2U], entry[3U], ReadLittleEndian32(entry + 4U),
                                    ReadLittleEndian16(entry + 8U));
    case MadtLocalApicAddressOverrideType:
        if ((length != 12U) || (ReadLittleEndian16(entry + 2U) != 0U) || (ReadLittleEndian64(entry + 4U) == 0U) ||
            (state.LocalControllerOverrideCount != 0U))
        {
            return state.LocalControllerOverrideCount != 0U ? AcpiTableParseError::InvalidMadt
                                                            : AcpiTableParseError::InvalidControllerEntry;
        }
        state.LocalControllerPhysicalAddress = ReadLittleEndian64(entry + 4U);
        ++state.LocalControllerOverrideCount;
        return AcpiTableParseError::None;
    case MadtIoSapicType:
        if ((length != 16U) || (entry[3U] != 0U))
        {
            return AcpiTableParseError::InvalidControllerEntry;
        }
        if (targetArchitecture != AcpiTargetArchitecture::Ia64)
        {
            return AcpiTableParseError::UnsupportedInterruptModel;
        }
        return AddIoController(state, entry[2U], ReadLittleEndian64(entry + 8U), ReadLittleEndian32(entry + 4U),
                               AcpiIoControllerKind::IoSapic);
    case MadtLocalSapicType:
    {
        const bool isLegacyEntry = length == 12U;
        const bool isExtendedEntry = length >= 16U;
        if ((!isLegacyEntry && !isExtendedEntry) || ((entry[5U] | entry[6U] | entry[7U]) != 0U) ||
            ((length > 16U) && !HasNullTerminator(entry + 16U, length - 16U)))
        {
            return AcpiTableParseError::InvalidControllerEntry;
        }
        if (targetArchitecture != AcpiTargetArchitecture::Ia64)
        {
            return AcpiTableParseError::UnsupportedInterruptModel;
        }
        return AddProcessor(state, entryOffset);
    }
    case MadtLocalX2ApicType:
        if ((length != 16U) || (ReadLittleEndian16(entry + 2U) != 0U) ||
            (ReadLittleEndian32(entry + 4U) == 0xFFFF'FFFFU))
        {
            return AcpiTableParseError::InvalidControllerEntry;
        }
        if (targetArchitecture != AcpiTargetArchitecture::Amd64)
        {
            return AcpiTableParseError::UnsupportedInterruptModel;
        }
        return AddProcessor(state, entryOffset);
    default:
        return AcpiTableParseError::None;
    }
}

[[nodiscard]] AcpiTableParseError ValidateMadt(ValidationState& state,
                                               const AcpiTargetArchitecture targetArchitecture) noexcept
{
    if ((state.MadtLength < MadtHeaderSizeBytes) || !BytesEqual(state.Madt, "APIC", 4U) || (state.Madt[8U] == 0U))
    {
        return AcpiTableParseError::InvalidMadt;
    }
    state.MadtRevision = state.Madt[8U];
    state.LocalControllerPhysicalAddress = ReadLittleEndian32(state.Madt + 36U);
    state.MadtFlags = ReadLittleEndian32(state.Madt + 40U);
    if ((state.MadtFlags & ~MadtPcatCompatibleBit) != 0U)
    {
        return AcpiTableParseError::InvalidMadt;
    }

    Foundation::Size offset{MadtHeaderSizeBytes};
    Foundation::Size entryCount{0U};
    while (offset < state.MadtLength)
    {
        if (((state.MadtLength - offset) < 2U) || (state.Madt[offset + 1U] < 2U) ||
            (state.Madt[offset + 1U] > (state.MadtLength - offset)) || (entryCount >= MaximumMadtEntries))
        {
            return AcpiTableParseError::InvalidMadt;
        }
        const Foundation::Size entryLength = state.Madt[offset + 1U];
        const AcpiTableParseError entryError = ValidateMadtEntry(state, offset, entryLength, targetArchitecture);
        // External line routing is optional on AMD64; malformed CPU entries
        // and malformed entry framing still reject the mandatory topology.
        const Foundation::UInt8 type = state.Madt[offset];
        if ((entryError != AcpiTableParseError::None) &&
            !((targetArchitecture == AcpiTargetArchitecture::Amd64) &&
              ((type == MadtIoApicType) || (type == MadtInterruptSourceOverrideType))))
        {
            return entryError;
        }
        offset += entryLength;
        ++entryCount;
    }

    if ((state.LocalControllerPhysicalAddress == 0U) || (state.EnabledProcessorCount == 0U))
    {
        return state.EnabledProcessorCount == 0U ? AcpiTableParseError::MissingEnabledProcessor
                                                 : AcpiTableParseError::InvalidMadt;
    }
    if ((state.IoControllerCount == 0U) && (targetArchitecture == AcpiTargetArchitecture::Ia64))
    {
        return AcpiTableParseError::MissingIoController;
    }
    for (Foundation::Size index{0U}; index < state.IoControllerCount; ++index)
    {
        const AcpiIoControllerKind kind = static_cast<AcpiIoControllerKind>(state.IoControllerKinds[index]);
        if (((targetArchitecture == AcpiTargetArchitecture::Amd64) && (kind != AcpiIoControllerKind::IoApic)) ||
            ((targetArchitecture == AcpiTargetArchitecture::Ia64) && (kind != AcpiIoControllerKind::IoSapic)))
        {
            return AcpiTableParseError::UnsupportedInterruptModel;
        }
    }
    return AcpiTableParseError::None;
}

[[nodiscard]] bool PciConfigurationRegionPrecedes(const AcpiPciConfigurationRegion& left,
                                                  const AcpiPciConfigurationRegion& right) noexcept
{
    return (left.Segment < right.Segment) ||
           ((left.Segment == right.Segment) &&
            ((left.FirstBus < right.FirstBus) ||
             ((left.FirstBus == right.FirstBus) &&
              ((left.LastBus < right.LastBus) ||
               ((left.LastBus == right.LastBus) && (left.BaseAddress < right.BaseAddress))))));
}

[[nodiscard]] bool PciConfigurationRegionsOverlap(const AcpiPciConfigurationRegion& left,
                                                  const AcpiPciConfigurationRegion& right) noexcept
{
    if ((left.Segment == right.Segment) && (left.FirstBus <= right.LastBus) && (right.FirstBus <= left.LastBus))
    {
        return true;
    }

    Foundation::UInt64 leftFirstAddress{0U};
    Foundation::UInt64 leftLastAddress{0U};
    Foundation::UInt64 rightFirstAddress{0U};
    Foundation::UInt64 rightLastAddress{0U};
    if (!TryGetPciConfigurationRegionBounds(left, leftFirstAddress, leftLastAddress) ||
        !TryGetPciConfigurationRegionBounds(right, rightFirstAddress, rightLastAddress))
    {
        return true;
    }
    return (leftFirstAddress <= rightLastAddress) && (rightFirstAddress <= leftLastAddress);
}

[[nodiscard, gnu::noinline]] bool ValidateMcfg(const ValidationState& state) noexcept
{
    if ((state.McfgLength < McfgHeaderSizeBytes) || !BytesEqual(state.Mcfg, "MCFG", 4U) || (state.Mcfg[8U] == 0U) ||
        (ReadLittleEndian64(state.Mcfg + 36U) != 0U) ||
        (((state.McfgLength - McfgHeaderSizeBytes) % McfgAllocationSizeBytes) != 0U))
    {
        return false;
    }

    const Foundation::Size allocationCount = (state.McfgLength - McfgHeaderSizeBytes) / McfgAllocationSizeBytes;
    if (allocationCount == 0U)
    {
        return false;
    }
    if (allocationCount > MaximumAcpiPciConfigurationRegions)
    {
        return false;
    }

    for (Foundation::Size index{0U}; index < allocationCount; ++index)
    {
        const Foundation::UInt8* const allocation =
            state.Mcfg + McfgHeaderSizeBytes + (index * McfgAllocationSizeBytes);
        const AcpiPciConfigurationRegion configurationRegion = ReadPciConfigurationRegion(allocation);
        Foundation::UInt64 firstAddress{0U};
        Foundation::UInt64 lastAddress{0U};
        if (!TryGetPciConfigurationRegionBounds(configurationRegion, firstAddress, lastAddress))
        {
            return false;
        }
    }

    for (Foundation::Size leftIndex{0U}; leftIndex < allocationCount; ++leftIndex)
    {
        const Foundation::UInt8* const leftAllocation =
            state.Mcfg + McfgHeaderSizeBytes + (leftIndex * McfgAllocationSizeBytes);
        const AcpiPciConfigurationRegion left = ReadPciConfigurationRegion(leftAllocation);
        for (Foundation::Size rightIndex{leftIndex + 1U}; rightIndex < allocationCount; ++rightIndex)
        {
            const Foundation::UInt8* const rightAllocation =
                state.Mcfg + McfgHeaderSizeBytes + (rightIndex * McfgAllocationSizeBytes);
            const AcpiPciConfigurationRegion right = ReadPciConfigurationRegion(rightAllocation);
            if (PciConfigurationRegionsOverlap(left, right))
            {
                return false;
            }
        }
    }
    return true;
}

// A DRHD's device scopes must tile the rest of it, each with a whole number
// of (device, function) hops and at least one.
[[nodiscard]] bool DmarScopesAreValid(const Foundation::UInt8* const unit, const Foundation::Size unitLength,
                                      const Foundation::Size headerLength) noexcept
{
    Foundation::Size offset{headerLength};
    while (offset < unitLength)
    {
        if ((unitLength - offset) < DmarScopeHeaderSizeBytes)
        {
            return false;
        }
        const Foundation::Size length = unit[offset + 1U];
        if ((length < (DmarScopeHeaderSizeBytes + 2U)) || (((length - DmarScopeHeaderSizeBytes) % 2U) != 0U) ||
            (length > (unitLength - offset)))
        {
            return false;
        }
        for (Foundation::Size path{DmarScopeHeaderSizeBytes}; path < length; path += 2U)
        {
            if ((unit[offset + path] > 31U) || (unit[offset + path + 1U] > 7U))
            {
                return false;
            }
        }
        offset += length;
    }
    return true;
}

// A malformed RMRR is independently skippable once its enclosing length
// has been checked. It must not discard otherwise usable DRHDs.
[[nodiscard]] bool DmarRegionIsValid(const Foundation::UInt8* const structure, const Foundation::Size length) noexcept
{
    if (length < (DmarReservedMemoryMinimumSizeBytes + DmarScopeHeaderSizeBytes + 2U))
    {
        return false;
    }
    const Foundation::UInt64 base = ReadLittleEndian64(structure + 8U);
    const Foundation::UInt64 limit = ReadLittleEndian64(structure + 16U);
    return (base <= limit) && ((base & (PageSizeBytes - 1U)) == 0U) &&
           ((limit & (PageSizeBytes - 1U)) == (PageSizeBytes - 1U)) && (limit != ~Foundation::UInt64{0U}) &&
           DmarScopesAreValid(structure, length, DmarReservedMemoryMinimumSizeBytes);
}

// The DMAR's remapping structures must tile the table; a DRHD must name a
// page-aligned register base and well-formed device scopes. Other structure
// types are skipped.
[[nodiscard, gnu::noinline]] bool ValidateDmar(const ValidationState& state) noexcept
{
    if ((state.DmarLength < DmarHeaderSizeBytes) || !BytesEqual(state.Dmar, "DMAR", 4U) || (state.Dmar[8U] == 0U) ||
        (state.Dmar[36U] > 63U))
    {
        return false;
    }
    Foundation::Size offset{DmarHeaderSizeBytes};
    Foundation::Size structures{0U};
    while (offset < state.DmarLength)
    {
        ++structures;
        if (((state.DmarLength - offset) < DmarStructureHeaderSizeBytes) || (structures > MaximumDmarStructures))
        {
            return false;
        }
        const Foundation::UInt8* const structure = state.Dmar + offset;
        const Foundation::Size length = ReadLittleEndian16(structure + 2U);
        if ((length < DmarStructureHeaderSizeBytes) || (length > (state.DmarLength - offset)))
        {
            return false;
        }
        if (ReadLittleEndian16(structure) == DmarUnitType)
        {
            const Foundation::UInt64 base =
                (length < DmarUnitMinimumSizeBytes) ? 0U : ReadLittleEndian64(structure + 8U);
            if ((base == 0U) || ((base & (PageSizeBytes - 1U)) != 0U) ||
                !DmarScopesAreValid(structure, length, DmarUnitMinimumSizeBytes))
            {
                return false;
            }
        }
        offset += length;
    }
    return true;
}

void AppendDmarScopes(const Foundation::UInt8* const unit, const Foundation::Size unitLength,
                      const Foundation::Size headerLength, const Foundation::UInt8 unitIndex,
                      AcpiDmaDeviceScope (&scopes)[MaximumAcpiDmaDeviceScopes], Foundation::UInt32& count) noexcept
{
    Foundation::Size offset{headerLength};
    while (offset < unitLength)
    {
        const Foundation::UInt8* const scope = unit + offset;
        if (count < MaximumAcpiDmaDeviceScopes)
        {
            AcpiDmaDeviceScope& retained = scopes[count];
            retained = AcpiDmaDeviceScope{
                .Unit = unitIndex,
                .Type = scope[0U],
                .StartBus = scope[5U],
                .PathEntries = static_cast<Foundation::UInt8>((scope[1U] - DmarScopeHeaderSizeBytes) / 2U),
                .Device = scope[6U],
                .Function = scope[7U],
                .Reserved = 0U,
                .Path = {},
                .Padding = 0U,
            };
            for (Foundation::Size hop{1U}; (hop < retained.PathEntries) && (hop < MaximumAcpiDmaScopePath); ++hop)
            {
                retained.Path[(hop - 1U) * 2U] = scope[DmarScopeHeaderSizeBytes + (hop * 2U)];
                retained.Path[(hop * 2U) - 1U] = scope[DmarScopeHeaderSizeBytes + (hop * 2U) + 1U];
            }
        }
        ++count;
        offset += scope[1U];
    }
}

[[gnu::noinline]] void PopulateDmar(const ValidationState& state, AcpiTopology& output) noexcept
{
    if (state.Dmar == nullptr)
    {
        return;
    }
    output.DmarPhysicalAddress = state.DmarPhysicalAddress;
    output.DmarHostAddressWidth = static_cast<Foundation::UInt8>(state.Dmar[36U] + 1U);
    output.DmarFlags = state.Dmar[37U];
    Foundation::Size offset{DmarHeaderSizeBytes};
    while (offset < state.DmarLength)
    {
        const Foundation::UInt8* const structure = state.Dmar + offset;
        const Foundation::UInt16 type = ReadLittleEndian16(structure);
        if (type == DmarUnitType)
        {
            if (output.DmaRemappingUnitCount < MaximumAcpiDmaRemappingUnits)
            {
                output.DmaRemappingUnits[output.DmaRemappingUnitCount] = AcpiDmaRemappingUnit{
                    .RegisterBaseAddress = ReadLittleEndian64(structure + 8U),
                    .Segment = ReadLittleEndian16(structure + 6U),
                    .Flags = structure[4U],
                    .Size = structure[5U],
                    .Reserved = 0U,
                };
                AppendDmarScopes(structure, ReadLittleEndian16(structure + 2U), DmarUnitMinimumSizeBytes,
                                 static_cast<Foundation::UInt8>(output.DmaRemappingUnitCount), output.DmaDeviceScopes,
                                 output.DmaDeviceScopeCount);
            }
            ++output.DmaRemappingUnitCount;
        }
        else if (type == DmarReservedMemoryType)
        {
            if (!DmarRegionIsValid(structure, ReadLittleEndian16(structure + 2U)))
            {
                ++output.MalformedRmrrCount;
                offset += ReadLittleEndian16(structure + 2U);
                continue;
            }
            if (output.ReservedMemoryRegionCount < MaximumAcpiReservedMemoryRegions)
            {
                output.ReservedMemoryRegions[output.ReservedMemoryRegionCount] = AcpiReservedMemoryRegion{
                    .Base = ReadLittleEndian64(structure + 8U),
                    .Limit = ReadLittleEndian64(structure + 16U),
                    .Segment = ReadLittleEndian16(structure + 6U),
                    .Reserved = {},
                };
                AppendDmarScopes(structure, ReadLittleEndian16(structure + 2U), DmarReservedMemoryMinimumSizeBytes,
                                 static_cast<Foundation::UInt8>(output.ReservedMemoryRegionCount),
                                 output.ReservedMemoryScopes, output.ReservedMemoryScopeCount);
            }
            ++output.ReservedMemoryRegionCount;
        }
        offset += ReadLittleEndian16(structure + 2U);
    }
}

[[nodiscard]] Foundation::Size FindOutputIoController(const AcpiTopology& output,
                                                      const Foundation::UInt32 controllerId) noexcept
{
    for (Foundation::Size index{0U}; index < output.IoControllerCount; ++index)
    {
        if (output.IoControllers[index].ControllerId == controllerId)
        {
            return index;
        }
    }
    return MaximumAcpiIoControllers;
}

void AppendIoController(AcpiTopology& output, const Foundation::UInt32 controllerId,
                        const Foundation::UInt64 physicalAddress, const Foundation::UInt32 globalInterruptBase,
                        const AcpiIoControllerKind kind) noexcept
{
    const Foundation::Size existing = FindOutputIoController(output, controllerId);
    if (existing != MaximumAcpiIoControllers)
    {
        if ((output.IoControllers[existing].ControllerKind == AcpiIoControllerKind::IoApic) &&
            (kind == AcpiIoControllerKind::IoSapic))
        {
            output.IoControllers[existing] = AcpiIoControllerTopology{
                .PhysicalAddress = physicalAddress,
                .GlobalSystemInterruptBase = globalInterruptBase,
                .ControllerId = controllerId,
                .ControllerKind = kind,
                .Reserved = 0U,
            };
        }
        return;
    }
    const Foundation::Size index = output.IoControllerCount;
    output.IoControllers[index] = AcpiIoControllerTopology{
        .PhysicalAddress = physicalAddress,
        .GlobalSystemInterruptBase = globalInterruptBase,
        .ControllerId = controllerId,
        .ControllerKind = kind,
        .Reserved = 0U,
    };
    ++output.IoControllerCount;
}

[[gnu::noinline]] void PopulateIvrs(const BootInfo& info, const Foundation::UInt64 address,
                                    AcpiTopology& output) noexcept
{
    if (address == 0U)
    {
        return;
    }
    const Foundation::UInt8* bytes{nullptr};
    Foundation::Size length{0U};
    const bool resolved = TryResolveSdt(info, address, bytes, length);
    const bool valid = resolved && ParseIvrs(bytes, length, output.Ivrs);
    output.Ivrs.PhysicalAddress = address;
    if (!valid)
    {
        ++output.Ivrs.Malformed;
    }
}

// Handoff does not authorize DMA. Preserve safely framed register bases
// independently of optional checksum/scope validation so a rejected firmware
// table cannot leave a pre-boot translation engine running during kernel DMA.
[[gnu::noinline]] void RetainIommuTable(const BootInfo& info, const Foundation::UInt64 address,
                                        AcpiTopology& output) noexcept
{
    const Foundation::UInt8* bytes{nullptr};
    if (!TryResolvePhysicalRange(info, address, 48U, bytes))
    {
        output.IommuHandoffUnsafe = 1U;
        return;
    }
    const Foundation::Size length = ReadLittleEndian32(bytes + 4U);
    if ((length < 48U) || (length > 65536U) || !TryResolvePhysicalRange(info, address, length, bytes))
    {
        output.IommuHandoffUnsafe = 1U;
        return;
    }
    const bool amd = BytesEqual(bytes, "IVRS", 4U);
    for (Foundation::Size offset{48U}; offset < length;)
    {
        if (length - offset < 4U)
        {
            output.IommuHandoffUnsafe = 1U;
            return;
        }
        const Foundation::UInt8* block = bytes + offset;
        const Foundation::Size size = ReadLittleEndian16(block + 2U);
        if ((size < 4U) || (size > length - offset))
        {
            output.IommuHandoffUnsafe = 1U;
            return;
        }
        const bool unit = amd ? ((block[0U] == 0x10U) || (block[0U] == 0x11U) || (block[0U] == 0x40U))
                              : (ReadLittleEndian16(block) == 0U);
        if (unit)
        {
            const Foundation::UInt64 base = size >= (amd ? 24U : 16U) ? ReadLittleEndian64(block + 8U) : 0U;
            if ((base == 0U) || ((base & (amd ? 0x3FFFU : 0xFFFU)) != 0U) || (base > 0x000F'FFFF'FFFF'C000ULL))
            {
                output.IommuHandoffUnsafe = 1U;
            }
            else
            {
                bool duplicate{false};
                for (Foundation::Size previous{0U}; previous < output.IommuHandoffCount; ++previous)
                {
                    duplicate = duplicate || (output.IommuHandoffs[previous].RegisterBase == base);
                }
                if (!duplicate)
                {
                    if (output.IommuHandoffCount == MaximumAcpiIommuHandoffs)
                    {
                        output.IommuHandoffUnsafe = 1U;
                        return;
                    }
                    output.IommuHandoffs[output.IommuHandoffCount++] = {base, amd ? 2U : 1U, 0U};
                }
            }
        }
        offset += size;
    }
}

[[gnu::noinline]] void RetainIommuHandoffs(const BootInfo& info, const ValidationState& state,
                                           AcpiTopology& output) noexcept
{
    const Foundation::UInt8* root{nullptr};
    Foundation::Size length{0U};
    if (!TryResolveSdt(info, state.RootTablePhysicalAddress, root, length))
    {
        output.IommuHandoffUnsafe = 1U;
        return;
    }
    const Foundation::Size stride = state.RootKind == AcpiRootTableKind::Xsdt ? 8U : 4U;
    for (Foundation::Size offset{36U}; offset + stride <= length; offset += stride)
    {
        const Foundation::UInt64 address =
            stride == 8U ? ReadLittleEndian64(root + offset) : ReadLittleEndian32(root + offset);
        const Foundation::UInt8* signature{nullptr};
        if (!TryResolvePhysicalRange(info, address, 4U, signature))
        {
            continue;
        }
        if (BytesEqual(signature, "DMAR", 4U) || BytesEqual(signature, "IVRS", 4U))
        {
            RetainIommuTable(info, address, output);
        }
    }
}

[[gnu::noinline]] void PopulateTopology(const ValidationState& state, AcpiTopology& output) noexcept
{
    output = {};
    output.RsdpPhysicalAddress = state.RsdpPhysicalAddress;
    output.RootTablePhysicalAddress = state.RootTablePhysicalAddress;
    output.MadtPhysicalAddress = state.MadtPhysicalAddress;
    output.LocalControllerPhysicalAddress = state.LocalControllerPhysicalAddress;
    output.RootKind = state.RootKind;
    output.RsdpRevision = state.RsdpRevision;
    output.MadtRevision = state.MadtRevision;
    output.MadtFlags = state.MadtFlags;
    output.McfgPhysicalAddress = state.McfgPhysicalAddress;
    output.PciConfigurationRegionCount =
        state.Mcfg == nullptr
            ? 0U
            : static_cast<Foundation::UInt32>((state.McfgLength - McfgHeaderSizeBytes) / McfgAllocationSizeBytes);
    output.McfgRevision = output.PciConfigurationRegionCount == 0U ? 0U : state.Mcfg[8U];
    for (Foundation::Size index{0U}; index < output.PciConfigurationRegionCount; ++index)
    {
        const Foundation::UInt8* const allocation =
            state.Mcfg + McfgHeaderSizeBytes + (index * McfgAllocationSizeBytes);
        output.PciConfigurationRegions[index] = ReadPciConfigurationRegion(allocation);
    }
    for (Foundation::Size index{1U}; index < output.PciConfigurationRegionCount; ++index)
    {
        const AcpiPciConfigurationRegion value = output.PciConfigurationRegions[index];
        Foundation::Size destination{index};
        while ((destination != 0U) &&
               PciConfigurationRegionPrecedes(value, output.PciConfigurationRegions[destination - 1U]))
        {
            output.PciConfigurationRegions[destination] = output.PciConfigurationRegions[destination - 1U];
            --destination;
        }
        output.PciConfigurationRegions[destination] = value;
    }

    for (Foundation::Size index{0U}; index < state.ProcessorCount; ++index)
    {
        const ProcessorEntry processor = ReadProcessorEntry(state.Madt + state.ProcessorEntryOffsets[index]);
        output.Processors[index] = AcpiProcessorTopology{
            .AcpiUid = processor.AcpiUid,
            .HardwareId = processor.HardwareId,
            .HardwareEid = processor.HardwareEid,
            .Flags = processor.Flags,
            .ControllerKind = processor.ControllerKind,
            .Reserved = 0U,
        };
    }
    output.ProcessorCount = state.ProcessorCount;
    output.IgnoredProcessorCount = state.IgnoredProcessorCount;

    for (Foundation::Size index{0U}; index < state.IoControllerCount; ++index)
    {
        AppendIoController(output, state.IoControllerIds[index], state.IoControllerPhysicalAddresses[index],
                           state.IoControllerGlobalInterruptBases[index],
                           static_cast<AcpiIoControllerKind>(state.IoControllerKinds[index]));
    }
    for (Foundation::Size index{0U}; index < state.InterruptOverrideCount; ++index)
    {
        output.InterruptOverrides[index] = AcpiInterruptOverride{
            .GlobalSystemInterrupt = state.InterruptOverrideGlobalInterrupts[index],
            .Flags = static_cast<Foundation::UInt16>(state.InterruptOverrideSourcesAndFlags[index] >> 4U),
            .Bus = 0U,
            .Source = static_cast<Foundation::UInt8>(state.InterruptOverrideSourcesAndFlags[index] & 0xFU),
        };
    }
    output.InterruptOverrideCount = state.InterruptOverrideCount;
    output.IapcBootArchitecture = state.IapcBootArchitecture;
    output.FadtRevision = state.FadtRevision;
    output.PmTimerAddress = state.PmTimerAddress;
    output.PmTimerSpace = state.PmTimerSpace;
    output.PmTimerBits = state.PmTimerBits;
    output.HpetAddress = state.HpetAddress;
    PopulateDmar(state, output);
    output.DmarPhysicalAddress = state.DmarPhysicalAddress;
}
[[nodiscard]] PowerRegister ReadPowerGas(const Foundation::UInt8* gas, const Foundation::UInt8 width) noexcept
{
    PowerRegister reg{.Address = ReadLittleEndian64(gas + 4U), .Space = gas[0U], .Width = width, .Reserved = {}};
    if (gas[1U] != width * 8U || gas[2U] != 0U || (gas[3U] != 0U && gas[3U] != width) ||
        !ValidPowerRegister(reg, width))
    {
        return {};
    }
    return reg;
}
// ACPI 6.5 §5.2.9 offsets; unsupported GAS forms remain unavailable.
void ReadPowerFadt(const Foundation::UInt8* bytes, const Foundation::Size length, PowerRegisters& power) noexcept
{
    if (length < 116U)
    {
        return;
    }
    power.Flags = ReadLittleEndian32(bytes + 112U);
    if (length >= 129U && bytes[8U] >= 2U && (power.Flags & (1U << 10U)) != 0U)
    {
        power.Reset = ReadPowerGas(bytes + 116U, 1U);
        power.ResetValue = bytes[128U];
    }
    if ((power.Flags & (1U << 20U)) != 0U || bytes[89U] < 2U)
    {
        return;
    }
    const auto smi = ReadLittleEndian32(bytes + 48U);
    if (smi != 0U && smi <= 0xFFFFU && bytes[52U] != 0U)
    {
        power.SmiCommand = smi;
        power.AcpiEnable = bytes[52U];
    }
    power.ControlA = {.Address = ReadLittleEndian32(bytes + 64U), .Space = 1U, .Width = 2U, .Reserved = {}};
    power.ControlB = {.Address = ReadLittleEndian32(bytes + 68U), .Space = 1U, .Width = 2U, .Reserved = {}};
    if (length >= 184U && bytes[8U] >= 3U && ReadLittleEndian64(bytes + 176U) != 0U)
    {
        power.ControlA = ReadPowerGas(bytes + 172U, 2U);
    }
    if (length >= 196U && bytes[8U] >= 3U && ReadLittleEndian64(bytes + 188U) != 0U)
    {
        power.ControlB = ReadPowerGas(bytes + 184U, 2U);
        if (power.ControlB.Address == 0U)
        {
            power.ControlA = {};
        }
    }
    if (!ValidPowerRegister(power.ControlA, 2U))
    {
        power.ControlA = {};
    }
    // A malformed declared B block invalidates shutdown, rather than dropping B.
    if (power.ControlB.Address != 0U && !ValidPowerRegister(power.ControlB, 2U))
    {
        power.ControlA = {};
    }
}
[[gnu::noinline]] void ReadFadt(const BootInfo& bootInfo, const Foundation::UInt64 address, ValidationState& state,
                                PowerRegisters* const power) noexcept
{
    const Foundation::UInt8* bytes{nullptr};
    Foundation::Size length{0U};
    if (!TryResolveSdt(bootInfo, address, bytes, length) || !BytesEqual(bytes, "FACP", 4U))
    {
        return;
    }
    if (power != nullptr)
    {
        ReadPowerFadt(bytes, length, *power);
    }
    if (length >= 111U && bytes[8U] >= 3U)
    {
        state.IapcBootArchitecture = ReadLittleEndian16(bytes + 109U);
        state.FadtRevision = bytes[8U];
    }
    if (length < 116U || (ReadLittleEndian32(bytes + 112U) & (1U << 20U)) != 0U)
    {
        return;
    }
    state.PmTimerBits = (ReadLittleEndian32(bytes + 112U) & (1U << 8U)) != 0U ? 32U : 24U;
    if (length >= 220U && bytes[8U] >= 3U)
    {
        const Foundation::UInt8* const gas = bytes + 208U;
        const Foundation::UInt64 timer = ReadLittleEndian64(gas + 4U);
        // PM_TMR is a 32-bit register even when only its low 24 bits count.
        if (timer != 0U && gas[0U] <= 1U && (gas[1U] == 32U || gas[1U] == state.PmTimerBits) && gas[2U] == 0U &&
            (gas[3U] == 0U || gas[3U] == 3U) &&
            ((gas[0U] == 1U && timer <= 0xFFFCU) ||
             (gas[0U] == 0U && (timer & 3U) == 0U && timer < (Foundation::UInt64{1U} << 47U) - 4U)))
        {
            state.PmTimerAddress = timer;
            state.PmTimerSpace = gas[0U];
            return;
        }
    }
    const Foundation::UInt32 legacy = ReadLittleEndian32(bytes + 76U);
    if (bytes[91U] == 4U && legacy != 0U && legacy <= 0xFFFCU)
    {
        state.PmTimerAddress = legacy;
        state.PmTimerSpace = 1U;
    }
}
[[gnu::noinline]] void ReadHpet(const BootInfo& bootInfo, const Foundation::UInt64 address,
                                ValidationState& state) noexcept
{
    const Foundation::UInt8* bytes{nullptr};
    Foundation::Size length{0U};
    if (!TryResolveSdt(bootInfo, address, bytes, length) || length < 56U || bytes[8U] == 0U)
    {
        return;
    }
    const Foundation::UInt8* const gas = bytes + 40U;
    const Foundation::UInt64 base = ReadLittleEndian64(gas + 4U);
    if (gas[0U] == 0U && gas[2U] == 0U && (gas[1U] == 0U || gas[1U] == 64U) && (gas[3U] == 0U || gas[3U] == 4U) &&
        base != 0U && (base & 4095U) == 0U && base < (Foundation::UInt64{1U} << 47U) - 4096U)
    {
        state.HpetAddress = base;
    }
}
// Keep the root scan's temporary addresses outside Parse's bounded validation frame.
[[nodiscard, gnu::noinline]] AcpiTableParseError FindRootTables(const BootInfo& bootInfo, ValidationState& state,
                                                                PowerRegisters* const power) noexcept
{
    const Foundation::UInt8* rootTable{nullptr};
    Foundation::Size rootLength{0U};
    if (!TryResolveSdt(bootInfo, state.RootTablePhysicalAddress, rootTable, rootLength))
    {
        return AcpiTableParseError::InvalidRootTable;
    }
    const bool isXsdt = state.RootKind == AcpiRootTableKind::Xsdt;
    if (!BytesEqual(rootTable, isXsdt ? "XSDT" : "RSDT", 4U) || (rootTable[8U] == 0U))
    {
        return AcpiTableParseError::InvalidRootTable;
    }
    const Foundation::Size entrySize = isXsdt ? 8U : 4U;
    const Foundation::UInt32 entryShift = isXsdt ? 3U : 2U;
    const Foundation::Size payloadLength = rootLength - SdtHeaderSizeBytes;
    if (((payloadLength & (entrySize - 1U)) != 0U) || ((payloadLength >> entryShift) > MaximumAcpiRootTableEntries))
    {
        return AcpiTableParseError::InvalidRootTable;
    }

    Foundation::UInt32 madtCount{0U};
    Foundation::UInt32 mcfgCount{0U};
    Foundation::UInt32 dmarCount{0U};
    Foundation::UInt32 ivrsCount{0U};
    Foundation::UInt32 fadtCount{0U};
    Foundation::UInt64 fadtAddress{0U};
    Foundation::UInt32 hpetCount{0U};
    Foundation::UInt64 hpetAddress{0U};
    for (Foundation::Size offset{SdtHeaderSizeBytes}; offset < rootLength; offset += entrySize)
    {
        const Foundation::UInt64 tableAddress =
            isXsdt ? ReadLittleEndian64(rootTable + offset) : ReadLittleEndian32(rootTable + offset);
        if (tableAddress == 0U)
        {
            return AcpiTableParseError::InvalidRootTable;
        }
        const Foundation::UInt8* signature{nullptr};
        if (!TryResolvePhysicalRange(bootInfo, tableAddress, 4U, signature))
        {
            continue;
        }
        if (BytesEqual(signature, "APIC", 4U))
        {
            ++madtCount;
            if (madtCount > 1U)
            {
                return AcpiTableParseError::DuplicateMadt;
            }
            state.MadtPhysicalAddress = tableAddress;
        }
        else if (BytesEqual(signature, "MCFG", 4U))
        {
            ++mcfgCount;
            if (mcfgCount == 1U)
            {
                state.McfgPhysicalAddress = tableAddress;
            }
        }
        else if (BytesEqual(signature, "FACP", 4U))
        {
            ++fadtCount;
            fadtAddress = tableAddress;
        }
        else if (BytesEqual(signature, "IVRS", 4U))
        {
            ++ivrsCount;
            state.IvrsPhysicalAddress = (ivrsCount == 1U) ? tableAddress : 0U;
        }
        else if (BytesEqual(signature, "HPET", 4U))
        {
            ++hpetCount;
            hpetAddress = tableAddress;
        }
        else if (BytesEqual(signature, "DMAR", 4U))
        {
            ++dmarCount;
            if (dmarCount == 1U)
            {
                state.DmarPhysicalAddress = tableAddress;
            }
        }
    }
    if (madtCount == 0U)
    {
        return AcpiTableParseError::MadtNotFound;
    }
    if (fadtCount == 1U)
    {
        ReadFadt(bootInfo, fadtAddress, state, power);
    }
    if (hpetCount == 1U)
    {
        ReadHpet(bootInfo, hpetAddress, state);
    }
    if (mcfgCount != 1U)
    {
        state.McfgPhysicalAddress = 0U;
    }
    if (dmarCount != 1U)
    {
        state.DmarPhysicalAddress = 0U;
    }
    return AcpiTableParseError::None;
}
[[nodiscard, gnu::noinline]] AcpiTableParseError
ValidateMandatoryTables(const BootInfo& bootInfo, ValidationState& state,
                        const AcpiTargetArchitecture targetArchitecture, PowerRegisters* const power) noexcept
{
    const Foundation::UInt8* rsdp{nullptr};
    if (!TryResolvePhysicalRange(bootInfo, state.RsdpPhysicalAddress, RsdpV1SizeBytes, rsdp))
    {
        return AcpiTableParseError::TableInReusableMemory;
    }
    if (!BytesEqual(rsdp, "RSD PTR ", 8U) || !ChecksumIsZero(rsdp, RsdpV1SizeBytes))
    {
        return AcpiTableParseError::InvalidRsdp;
    }

    state.RsdpRevision = rsdp[15U];
    if (state.RsdpRevision == 0U)
    {
        state.RootKind = AcpiRootTableKind::Rsdt;
        state.RootTablePhysicalAddress = ReadLittleEndian32(rsdp + 16U);
    }
    else
    {
        if (state.RsdpRevision < 2U)
        {
            return AcpiTableParseError::InvalidRsdp;
        }
        if (!TryResolvePhysicalRange(bootInfo, state.RsdpPhysicalAddress, RsdpV2MinimumSizeBytes, rsdp))
        {
            return AcpiTableParseError::TableInReusableMemory;
        }
        const Foundation::UInt32 rsdpLength = ReadLittleEndian32(rsdp + 20U);
        if ((rsdpLength < RsdpV2MinimumSizeBytes) || (rsdpLength > MaximumRsdpSizeBytes) ||
            !TryResolvePhysicalRange(bootInfo, state.RsdpPhysicalAddress, rsdpLength, rsdp))
        {
            return rsdpLength > MaximumRsdpSizeBytes ? AcpiTableParseError::InvalidRsdp
                                                     : AcpiTableParseError::TableInReusableMemory;
        }
        if (!ChecksumIsZero(rsdp, rsdpLength) || ((rsdp[33U] | rsdp[34U] | rsdp[35U]) != 0U))
        {
            return AcpiTableParseError::InvalidRsdp;
        }
        const Foundation::UInt64 xsdtAddress = ReadLittleEndian64(rsdp + 24U);
        if (xsdtAddress != 0U)
        {
            state.RootKind = AcpiRootTableKind::Xsdt;
            state.RootTablePhysicalAddress = xsdtAddress;
        }
        else
        {
            state.RootKind = AcpiRootTableKind::Rsdt;
            state.RootTablePhysicalAddress = ReadLittleEndian32(rsdp + 16U);
        }
    }
    if (state.RootTablePhysicalAddress == 0U)
    {
        return AcpiTableParseError::InvalidRsdp;
    }

    const AcpiTableParseError rootError = FindRootTables(bootInfo, state, power);
    if (rootError != AcpiTableParseError::None)
    {
        return rootError;
    }
    if (!TryResolveSdt(bootInfo, state.MadtPhysicalAddress, state.Madt, state.MadtLength))
    {
        return AcpiTableParseError::InvalidMadt;
    }

    const AcpiTableParseError madtError = ValidateMadt(state, targetArchitecture);
    if (madtError != AcpiTableParseError::None)
    {
        return madtError;
    }
    return AcpiTableParseError::None;
}
// Keep publication calls outside Parse's validation-state frame.
[[gnu::noinline]] void PublishTopology(const BootInfo& info, const ValidationState& state,
                                       AcpiTopology& output) noexcept
{
    PopulateTopology(state, output);
    PopulateIvrs(info, state.IvrsPhysicalAddress, output);
    RetainIommuHandoffs(info, state, output);
    RetainAcpiDefinitionBlocks(info, output);
}
} // namespace

void AcpiTableParser::CollectDmaExclusions(const BootInfo& bootInfo, const AcpiTopology& topology,
                                           BootDmaExclusions& output) noexcept
{
    output = {};
    const Foundation::UInt8* dmar{nullptr};
    Foundation::Size dmarLength{0U};
    if ((topology.DmarPhysicalAddress != 0U) &&
        TryResolveSdt(bootInfo, topology.DmarPhysicalAddress, dmar, dmarLength) && (dmarLength >= DmarHeaderSizeBytes))
    {
        for (Foundation::Size offset{DmarHeaderSizeBytes}; (dmarLength - offset) >= 4U;)
        {
            const Foundation::UInt8* const structure = dmar + offset;
            const Foundation::Size size = ReadLittleEndian16(structure + 2U);
            if ((size < 4U) || (size > (dmarLength - offset)))
            {
                break;
            }
            offset += size;
            if (ReadLittleEndian16(structure) != DmarReservedMemoryType)
            {
                continue;
            }
            ++output.DeclaredCount;
            if (size < DmarReservedMemoryMinimumSizeBytes)
            {
                continue;
            }
            const Foundation::UInt64 base = ReadLittleEndian64(structure + 8U);
            const Foundation::UInt64 limit = ReadLittleEndian64(structure + 16U);
            if ((base > limit) || (output.Count >= BootDmaExclusionMaximumCount))
            {
                continue;
            }
            output.Ranges[output.Count] = BootDmaExclusion{
                .Base = base,
                .Limit = limit,
                .Segment = ReadLittleEndian16(structure + 6U),
                .Kind = BootDmaExclusionRmrr,
                .Malformed = static_cast<Foundation::UInt8>(DmarRegionIsValid(structure, size) ? 0U : 1U),
                .Reserved = 0U,
            };
            ++output.Count;
        }
    }
    const Foundation::UInt8* ivrs{nullptr};
    Foundation::Size ivrsLength{0U};
    if ((topology.Ivrs.PhysicalAddress != 0U) &&
        TryResolveSdt(bootInfo, topology.Ivrs.PhysicalAddress, ivrs, ivrsLength))
    {
        CollectIvrsExclusions(ivrs, ivrsLength, output);
    }
}

AcpiTableParseError AcpiTableParser::Parse(const BootInfo& bootInfo, const void* const rsdpPointer,
                                           const AcpiTargetArchitecture targetArchitecture, AcpiTopology& output,
                                           const Foundation::UInt32 bspApicId, PowerRegisters* const power) noexcept
{
    // Caller-owned facts keep the validation frame within its 512-byte budget.
    if (power != nullptr)
    {
        *power = {};
    }
    if ((rsdpPointer == nullptr) ||
        ((targetArchitecture != AcpiTargetArchitecture::Amd64) && (targetArchitecture != AcpiTargetArchitecture::Ia64)))
    {
        return AcpiTableParseError::InvalidArgument;
    }
    if (!BootInfoIsValid(bootInfo))
    {
        return AcpiTableParseError::InvalidBootInfo;
    }

    ValidationState state{};
    state.BspApicId = targetArchitecture == AcpiTargetArchitecture::Amd64 ? bspApicId : 0xFFFF'FFFFU;
    state.RsdpPhysicalAddress = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(rsdpPointer));
    const AcpiTableParseError mandatoryError = ValidateMandatoryTables(bootInfo, state, targetArchitecture, power);
    if (mandatoryError != AcpiTableParseError::None)
    {
        if (power != nullptr)
        {
            *power = {};
        }
        return mandatoryError;
    }
    if (state.McfgPhysicalAddress != 0U)
    {
        if (!TryResolveSdt(bootInfo, state.McfgPhysicalAddress, state.Mcfg, state.McfgLength) || !ValidateMcfg(state))
        {
            state.Mcfg = nullptr;
            state.McfgLength = 0U;
            state.McfgPhysicalAddress = 0U;
        }
    }
    else
    {
        state.Mcfg = nullptr;
        state.McfgLength = 0U;
        state.McfgPhysicalAddress = 0U;
    }
    if ((state.DmarPhysicalAddress == 0U) ||
        !TryResolveSdt(bootInfo, state.DmarPhysicalAddress, state.Dmar, state.DmarLength) || !ValidateDmar(state))
    {
        state.Dmar = nullptr;
        state.DmarLength = 0U;
    }
    PublishTopology(bootInfo, state, output);
    return AcpiTableParseError::None;
}
void AcpiTableParser::VisitTables(const BootInfo& bootInfo, const AcpiTopology& topology,
                                  IAcpiTableVisitor& visitor) noexcept
{
    const Foundation::UInt8* root{nullptr};
    Foundation::Size length{0U};
    if (!TryResolveSdt(bootInfo, topology.RootTablePhysicalAddress, root, length))
    {
        return;
    }
    visitor.Visit(TableRecord(topology.RootTablePhysicalAddress, root, length), root);
    const Foundation::Size stride = topology.RootKind == AcpiRootTableKind::Xsdt ? 8U : 4U;
    for (Foundation::Size offset{36U}; offset + stride <= length; offset += stride)
    {
        const Foundation::UInt64 address =
            stride == 8U ? ReadLittleEndian64(root + offset) : ReadLittleEndian32(root + offset);
        const Foundation::UInt8* bytes{nullptr};
        Foundation::Size tableLength{0U};
        if (!TryResolveSdtBytes(bootInfo, address, bytes, tableLength))
        {
            continue;
        }
        const AcpiTableRecord record = TableRecord(address, bytes, tableLength);
        visitor.Visit(record, bytes);
        if (BytesEqual(bytes, "FACP", 4U) && record.ChecksumValid != 0U)
        {
            const Foundation::UInt64 dsdtAddress = DsdtAddress(bytes, tableLength);
            if (TryResolveSdtBytes(bootInfo, dsdtAddress, bytes, tableLength) && BytesEqual(bytes, "DSDT", 4U))
            {
                visitor.Visit(TableRecord(dsdtAddress, bytes, tableLength), bytes);
            }
        }
    }
}
} // namespace Asteria::Boot::Acpi
