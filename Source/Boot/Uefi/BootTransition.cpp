// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/BootTransition.hpp"

#include "Boot/Uefi/AmlBootReport.hpp"
#include "Boot/BootInfoValidation.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumPageCount{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt32 MaximumStandardMemoryType{15U};
constexpr Foundation::UInt32 MinimumOemMemoryType{0x7000'0000U};
constexpr Foundation::Size DescriptorTypeOffset{0U};
constexpr Foundation::Size DescriptorPhysicalStartOffset{8U};
constexpr Foundation::Size DescriptorVirtualStartOffset{16U};
constexpr Foundation::Size DescriptorPageCountOffset{24U};
constexpr Foundation::Size DescriptorAttributesOffset{32U};

enum class FirmwareMemoryType : Foundation::UInt32
{
    Reserved,
    LoaderCode,
    LoaderData,
    BootServicesCode,
    BootServicesData,
    RuntimeServicesCode,
    RuntimeServicesData,
    Conventional,
    Unusable,
    AcpiReclaimable,
    AcpiNonVolatile,
    MemoryMappedIo,
    MemoryMappedIoPort,
    PalCode,
    Persistent,
    Unaccepted,
};

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

[[nodiscard]] PhysicalMemoryKind ConvertMemoryType(const Foundation::UInt32 firmwareType) noexcept
{
    switch (static_cast<FirmwareMemoryType>(firmwareType))
    {
    case FirmwareMemoryType::LoaderCode:
    case FirmwareMemoryType::LoaderData:
        return PhysicalMemoryKind::Loader;
    case FirmwareMemoryType::BootServicesCode:
    case FirmwareMemoryType::BootServicesData:
        return PhysicalMemoryKind::BootServices;
    case FirmwareMemoryType::RuntimeServicesCode:
    case FirmwareMemoryType::RuntimeServicesData:
        return PhysicalMemoryKind::RuntimeServices;
    case FirmwareMemoryType::Conventional:
        return PhysicalMemoryKind::Conventional;
    case FirmwareMemoryType::Unusable:
        return PhysicalMemoryKind::Unusable;
    case FirmwareMemoryType::AcpiReclaimable:
        return PhysicalMemoryKind::AcpiReclaimable;
    case FirmwareMemoryType::AcpiNonVolatile:
        return PhysicalMemoryKind::AcpiNonVolatile;
    case FirmwareMemoryType::MemoryMappedIo:
    case FirmwareMemoryType::MemoryMappedIoPort:
        return PhysicalMemoryKind::MemoryMappedIo;
    case FirmwareMemoryType::PalCode:
        return PhysicalMemoryKind::PalCode;
    case FirmwareMemoryType::Persistent:
        return PhysicalMemoryKind::Persistent;
    case FirmwareMemoryType::Unaccepted:
        return PhysicalMemoryKind::Unaccepted;
    case FirmwareMemoryType::Reserved:
    default:
        return PhysicalMemoryKind::Reserved;
    }
}

[[nodiscard]] bool IsFirmwareMemoryTypeValid(const Foundation::UInt32 firmwareType) noexcept
{
    return (firmwareType <= MaximumStandardMemoryType) || (firmwareType >= MinimumOemMemoryType);
}

[[nodiscard]] bool TryGetPageRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pageCount,
                                      Foundation::UInt64& end) noexcept
{
    if ((pageCount == 0U) || (pageCount > MaximumPageCount) || ((start & PageOffsetMask) != 0U))
    {
        return false;
    }
    if (pageCount == MaximumPageCount)
    {
        if (start != 0U)
        {
            return false;
        }
        end = MaximumUInt64;
        return true;
    }

    const Foundation::UInt64 lastByteOffset = (pageCount << 12U) - 1U;
    if (start > (MaximumUInt64 - lastByteOffset))
    {
        return false;
    }
    end = start + lastByteOffset;
    return true;
}

void ResetBootInfo(BootInfo& bootInfo, const Foundation::Size memoryMapSize, const Foundation::Size descriptorSize,
                   const Foundation::UInt32 descriptorVersion) noexcept
{
    // The ABI storage owner clears the complete integer-only representation,
    // including padding and unused slots, before publishing a checksum.
    auto* const bytes = reinterpret_cast<Foundation::UInt8*>(&bootInfo);
    for (Foundation::Size index{0U}; index < sizeof(bootInfo); ++index)
    {
        bytes[index] = 0U;
    }
    bootInfo.Signature = BootInfoSignature;
    bootInfo.Version = BootInfoVersion;
    bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(BootInfo));
    bootInfo.MemoryRegionCount = 0U;
    bootInfo.FirmwareDescriptorVersion = descriptorVersion;
    bootInfo.FirmwareDescriptorSize =
        descriptorSize <= 0xFFFF'FFFFU ? static_cast<Foundation::UInt32>(descriptorSize) : 0U;
    bootInfo.Reserved = 0U;
    bootInfo.FirmwareMemoryMapSizeBytes = static_cast<Foundation::UInt64>(memoryMapSize);
    bootInfo.Acpi = {};
    bootInfo.Entropy = {};
    bootInfo.Display = {};
    bootInfo.Inventory = {};
    bootInfo.Files = {};
    bootInfo.Trust = {};
    bootInfo.DmaExclusions = {};
    bootInfo.LowMemoryBlocks = 0U;
    bootInfo.WatchdogSeconds = Abi::WatchdogDefaultSeconds;
    bootInfo.SessionMode = 0U;
    bootInfo.LoginEnabled = 0U;
}

void SortMemoryRegions(BootInfo& bootInfo) noexcept
{
    const Foundation::Size count = static_cast<Foundation::Size>(bootInfo.MemoryRegionCount);
    for (Foundation::Size index{1U}; index < count; ++index)
    {
        const PhysicalMemoryRegion current = bootInfo.MemoryRegions[index];
        Foundation::Size destination = index;
        while ((destination != 0U) && (current.PhysicalStart < bootInfo.MemoryRegions[destination - 1U].PhysicalStart))
        {
            bootInfo.MemoryRegions[destination] = bootInfo.MemoryRegions[destination - 1U];
            --destination;
        }
        bootInfo.MemoryRegions[destination] = current;
    }
}

[[nodiscard]] bool HasNonOverlappingMemoryRegions(const BootInfo& bootInfo) noexcept
{
    const Foundation::Size count = static_cast<Foundation::Size>(bootInfo.MemoryRegionCount);
    for (Foundation::Size index{1U}; index < count; ++index)
    {
        const PhysicalMemoryRegion& previous = bootInfo.MemoryRegions[index - 1U];
        Foundation::UInt64 previousEnd{0U};
        if (!TryGetPageRangeEnd(previous.PhysicalStart, previous.PageCount, previousEnd) ||
            (bootInfo.MemoryRegions[index].PhysicalStart <= previousEnd))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] BootTransitionResult
MakeTransitionResult(const BootTransitionError error, const Foundation::UInt32 attempts, const Status firmwareStatus,
                     const Foundation::Size requiredBytes, const bool bootServicesExited,
                     const Acpi::AcpiTableParseError acpiError = Acpi::AcpiTableParseError::None) noexcept
{
    return BootTransitionResult{
        .Error = error,
        .ExitAttempts = attempts,
        .FirmwareStatus = firmwareStatus,
        .RequiredMemoryMapBytes = requiredBytes,
        .BootServicesExited = static_cast<Foundation::UInt8>(bootServicesExited ? 1U : 0U),
        .Reserved = {},
        .AcpiError = acpiError,
    };
}
}

Foundation::UInt64 ComputeManagedSpanGiB(const void* const memoryMap, const Foundation::Size memoryMapSize,
                                         const Foundation::Size descriptorSize) noexcept
{
    // UEFI 2.11 section 7.2.3's attributes.
    constexpr Foundation::UInt64 WriteBack{Foundation::UInt64{1U} << 3U};
    constexpr Foundation::UInt64 SpecificPurpose{Foundation::UInt64{1U} << 18U};
    constexpr Foundation::UInt64 Runtime{Foundation::UInt64{1U} << 63U};
    constexpr Foundation::UInt64 GiBBytes{Foundation::UInt64{1U} << 30U};
    if ((memoryMap == nullptr) || (descriptorSize < (DescriptorAttributesOffset + 8U)) ||
        ((memoryMapSize % descriptorSize) != 0U))
    {
        return 0U;
    }
    const auto* const bytes = static_cast<const Foundation::UInt8*>(memoryMap);
    Foundation::UInt64 end{0U};
    for (Foundation::Size offset{0U}; offset < memoryMapSize; offset += descriptorSize)
    {
        const Foundation::UInt8* const descriptor = bytes + offset;
        const auto type = static_cast<FirmwareMemoryType>(ReadLittleEndian32(descriptor + DescriptorTypeOffset));
        const Foundation::UInt64 attributes = ReadLittleEndian64(descriptor + DescriptorAttributesOffset);
        Foundation::UInt64 rangeEnd{0U};
        if (((type != FirmwareMemoryType::Conventional) && (type != FirmwareMemoryType::BootServicesCode) &&
             (type != FirmwareMemoryType::BootServicesData)) ||
            ((attributes & WriteBack) == 0U) || ((attributes & (Runtime | SpecificPurpose)) != 0U) ||
            !TryGetPageRangeEnd(ReadLittleEndian64(descriptor + DescriptorPhysicalStartOffset),
                                ReadLittleEndian64(descriptor + DescriptorPageCountOffset), rangeEnd))
        {
            continue;
        }
        end = (rangeEnd > end) ? rangeEnd : end;
    }
    return (end / GiBBytes) + (((end % GiBBytes) != 0U) ? 1U : 0U);
}

bool NormalizeMemoryMap(const void* const memoryMap, const Foundation::Size memoryMapSize,
                        const Foundation::Size descriptorSize, const Foundation::UInt32 descriptorVersion,
                        BootInfo& bootInfo) noexcept
{
    ResetBootInfo(bootInfo, memoryMapSize, descriptorSize, descriptorVersion);
    if ((memoryMap == nullptr) || (memoryMapSize == 0U) || (descriptorSize < MinimumMemoryDescriptorSize) ||
        (descriptorSize > 0xFFFF'FFFFU) || (descriptorVersion != MemoryDescriptorVersion))
    {
        return false;
    }

    const auto* const bytes = static_cast<const Foundation::UInt8*>(memoryMap);
    Foundation::Size offset{0U};
    Foundation::Size regionCount{0U};
    while (offset < memoryMapSize)
    {
        if (((memoryMapSize - offset) < descriptorSize) || (regionCount >= MaximumMemoryRegions))
        {
            bootInfo.MemoryRegionCount = 0U;
            return false;
        }

        const Foundation::UInt8* const descriptor = bytes + offset;
        const Foundation::UInt32 firmwareType = ReadLittleEndian32(descriptor + DescriptorTypeOffset);
        const Foundation::UInt64 physicalStart = ReadLittleEndian64(descriptor + DescriptorPhysicalStartOffset);
        const Foundation::UInt64 virtualStart = ReadLittleEndian64(descriptor + DescriptorVirtualStartOffset);
        const Foundation::UInt64 pageCount = ReadLittleEndian64(descriptor + DescriptorPageCountOffset);
        const Foundation::UInt64 attributes = ReadLittleEndian64(descriptor + DescriptorAttributesOffset);
        Foundation::UInt64 physicalEnd{0U};
        if (!IsFirmwareMemoryTypeValid(firmwareType) || !TryGetPageRangeEnd(physicalStart, pageCount, physicalEnd))
        {
            bootInfo.MemoryRegionCount = 0U;
            return false;
        }
        Foundation::UInt64 virtualEnd{0U};
        if ((virtualStart != 0U) && !TryGetPageRangeEnd(virtualStart, pageCount, virtualEnd))
        {
            bootInfo.MemoryRegionCount = 0U;
            return false;
        }

        bootInfo.MemoryRegions[regionCount] = PhysicalMemoryRegion{
            .PhysicalStart = physicalStart,
            .VirtualStart = virtualStart,
            .PageCount = pageCount,
            .Attributes = attributes,
            .FirmwareType = firmwareType,
            .Kind = ConvertMemoryType(firmwareType),
        };
        ++regionCount;
        offset += descriptorSize;
    }

    bootInfo.MemoryRegionCount = static_cast<Foundation::UInt32>(regionCount);
    SortMemoryRegions(bootInfo);
    if (!HasNonOverlappingMemoryRegions(bootInfo))
    {
        bootInfo.MemoryRegionCount = 0U;
        return false;
    }
    return true;
}

BootTransitionResult ExitBootServicesAndCaptureMemoryMap(
    const Handle imageHandle, BootServices& bootServices, void* const memoryMapBuffer,
    const Foundation::Size memoryMapCapacity, BootInfo& bootInfo, const AcpiCaptureRequest* const acpiCapture,
    const FinalMemoryMapPreparation preparation, const BootEntropy* const entropy, const BootDisplay* const display,
    const BootInventoryLocation* const inventory, const BootFileRegion* const files,
    const IFinalMemoryMapPreparation* const preparationOwner) noexcept
{
    ResetBootInfo(bootInfo, 0U, 0U, 0U);
    if ((imageHandle == nullptr) || (memoryMapBuffer == nullptr) || (memoryMapCapacity < MinimumMemoryDescriptorSize) ||
        (bootServices.GetMemoryMapFunction == nullptr) || (bootServices.ExitBootServicesFunction == nullptr))
    {
        return MakeTransitionResult(BootTransitionError::InvalidArgument, 0U, InvalidParameter, 0U, false);
    }
    if ((entropy != nullptr) && (GetBootEntropyState(*entropy) != BootEntropyState::Published))
    {
        return MakeTransitionResult(BootTransitionError::EntropyRejected, 0U, InvalidParameter, 0U, false);
    }

    for (Foundation::UInt32 attempt{1U}; attempt <= MaximumExitBootServicesAttempts; ++attempt)
    {
        Foundation::Size memoryMapSize{memoryMapCapacity};
        Foundation::Size mapKey{0U};
        Foundation::Size descriptorSize{0U};
        Foundation::UInt32 descriptorVersion{0U};
        const Status mapStatus = bootServices.GetMemoryMapFunction(&memoryMapSize, memoryMapBuffer, &mapKey,
                                                                   &descriptorSize, &descriptorVersion);
        if (mapStatus != Success)
        {
            return MakeTransitionResult(mapStatus == BufferTooSmall ? BootTransitionError::MemoryMapBufferTooSmall
                                                                    : BootTransitionError::GetMemoryMapFailed,
                                        attempt - 1U, mapStatus, memoryMapSize, false);
        }
        if (memoryMapSize > memoryMapCapacity)
        {
            return MakeTransitionResult(BootTransitionError::MemoryMapBufferTooSmall, attempt - 1U, Success,
                                        memoryMapSize, false);
        }
        if (!NormalizeMemoryMap(memoryMapBuffer, memoryMapSize, descriptorSize, descriptorVersion, bootInfo))
        {
            return MakeTransitionResult(BootTransitionError::InvalidMemoryMap, attempt - 1U, Success, memoryMapSize,
                                        false);
        }
        if (acpiCapture != nullptr)
        {
            const Acpi::AcpiTableParseError acpiError =
                (acpiCapture->Reserved != 0U)
                    ? Acpi::AcpiTableParseError::InvalidArgument
                    : Acpi::AcpiTableParser::Parse(bootInfo, acpiCapture->RsdpPointer, acpiCapture->TargetArchitecture,
                                                   bootInfo.Acpi, acpiCapture->BspApicId, &bootInfo.Power);
            if (acpiError != Acpi::AcpiTableParseError::None)
            {
                ResetBootInfo(bootInfo, 0U, 0U, 0U);
                return MakeTransitionResult(BootTransitionError::AcpiTopologyRejected, attempt - 1U, Success,
                                            memoryMapSize, false, acpiError);
            }
        }
        ApplyAmlBootEvidence(bootInfo.Acpi, &bootInfo.Power);
        if (entropy != nullptr)
        {
            bootInfo.Entropy = *entropy;
        }
        if (inventory != nullptr)
        {
            bootInfo.Inventory = *inventory;
        }
        if (files != nullptr)
        {
            bootInfo.Files = *files;
        }
        if (display != nullptr)
        {
            bootInfo.Display = *display;
        }
        SealBootInfo(bootInfo);
        if ((preparation != nullptr) && !preparation(bootInfo))
        {
            ResetBootInfo(bootInfo, 0U, 0U, 0U);
            return MakeTransitionResult(BootTransitionError::MemoryMapPreparationRejected, attempt - 1U, Success,
                                        memoryMapSize, false);
        }
        if ((preparationOwner != nullptr) && !preparationOwner->Prepare(bootInfo))
        {
            ResetBootInfo(bootInfo, 0U, 0U, 0U);
            return MakeTransitionResult(BootTransitionError::MemoryMapPreparationRejected, attempt - 1U, Success,
                                        memoryMapSize, false);
        }

        SealBootInfo(bootInfo);
        const Status exitStatus = bootServices.ExitBootServicesFunction(imageHandle, mapKey);
        if (exitStatus == Success)
        {
            return MakeTransitionResult(BootTransitionError::None, attempt, Success, memoryMapSize, true);
        }
        ResetBootInfo(bootInfo, 0U, 0U, 0U);
        if (exitStatus != InvalidParameter)
        {
            return MakeTransitionResult(BootTransitionError::ExitBootServicesFailed, attempt, exitStatus, memoryMapSize,
                                        false);
        }
    }

    ResetBootInfo(bootInfo, 0U, 0U, 0U);
    return MakeTransitionResult(BootTransitionError::ExitBootServicesRetryLimit, MaximumExitBootServicesAttempts,
                                InvalidParameter, 0U, false);
}
} // namespace Asteria::Boot::Uefi
