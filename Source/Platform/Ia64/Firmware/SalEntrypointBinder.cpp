// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/SalEntrypointBinder.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumPageCount{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 Ia64RuntimeAlignmentBytes{8192U};
constexpr Foundation::UInt64 Ia64RuntimeAlignmentMask{Ia64RuntimeAlignmentBytes - 1U};
constexpr Foundation::UInt64 RuntimeMemoryAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 PhysicalAddressTopBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 MaximumPhysicalAddress{PhysicalAddressTopBit - 1U};
constexpr Foundation::UInt32 ExpectedFirmwareDescriptorVersion{1U};
constexpr Foundation::UInt32 MinimumFirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 RuntimeServicesCodeMemoryType{5U};
constexpr Foundation::UInt32 RuntimeServicesDataMemoryType{6U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt32 AcpiReclaimMemoryType{9U};
constexpr Foundation::UInt32 AcpiNonVolatileMemoryType{10U};
constexpr Foundation::UInt32 PalCodeMemoryType{13U};
constexpr Foundation::UInt32 MaximumStandardMemoryType{15U};
constexpr Foundation::UInt32 MinimumOemMemoryType{0x7000'0000U};
constexpr Foundation::UInt64 PalProcedureAccessBytes{16U};
constexpr Foundation::UInt64 SalProcedureAccessBytes{16U};
constexpr Foundation::UInt64 SalGlobalDataAccessBytes{8U};

[[nodiscard]] Foundation::Result<SalEntrypointBinding, SalEntrypointBindError>
Fail(const SalEntrypointBindError error) noexcept
{
    return Foundation::Result<SalEntrypointBinding, SalEntrypointBindError>::Failure(error);
}

[[nodiscard]] Boot::PhysicalMemoryKind ConvertFirmwareMemoryType(const Foundation::UInt32 firmwareType) noexcept
{
    switch (firmwareType)
    {
    case 1U:
    case 2U:
        return Boot::PhysicalMemoryKind::Loader;
    case 3U:
    case 4U:
        return Boot::PhysicalMemoryKind::BootServices;
    case RuntimeServicesCodeMemoryType:
    case RuntimeServicesDataMemoryType:
        return Boot::PhysicalMemoryKind::RuntimeServices;
    case ConventionalMemoryType:
        return Boot::PhysicalMemoryKind::Conventional;
    case 8U:
        return Boot::PhysicalMemoryKind::Unusable;
    case AcpiReclaimMemoryType:
        return Boot::PhysicalMemoryKind::AcpiReclaimable;
    case AcpiNonVolatileMemoryType:
        return Boot::PhysicalMemoryKind::AcpiNonVolatile;
    case 11U:
    case 12U:
        return Boot::PhysicalMemoryKind::MemoryMappedIo;
    case PalCodeMemoryType:
        return Boot::PhysicalMemoryKind::PalCode;
    case 14U:
        return Boot::PhysicalMemoryKind::Persistent;
    case 15U:
        return Boot::PhysicalMemoryKind::Unaccepted;
    case 0U:
    default:
        return Boot::PhysicalMemoryKind::Reserved;
    }
}

[[nodiscard]] bool IsFirmwareMemoryTypeValid(const Foundation::UInt32 firmwareType) noexcept
{
    return (firmwareType <= MaximumStandardMemoryType) || (firmwareType >= MinimumOemMemoryType);
}

[[nodiscard]] bool TryGetRegionEnd(const Boot::PhysicalMemoryRegion& region, Foundation::UInt64& physicalEnd) noexcept
{
    if ((region.PageCount == 0U) || (region.PageCount > MaximumPageCount) ||
        ((region.PhysicalStart & PageOffsetMask) != 0U) ||
        ((region.VirtualStart != 0U) && ((region.VirtualStart & PageOffsetMask) != 0U)))
    {
        return false;
    }

    if (region.PageCount == MaximumPageCount)
    {
        if ((region.PhysicalStart != 0U) || (region.VirtualStart != 0U))
        {
            return false;
        }
        physicalEnd = MaximumUInt64;
        return true;
    }

    const Foundation::UInt64 lastByteOffset = (region.PageCount << 12U) - 1U;
    if ((region.PhysicalStart > (MaximumUInt64 - lastByteOffset)) ||
        ((region.VirtualStart != 0U) && (region.VirtualStart > (MaximumUInt64 - lastByteOffset))))
    {
        return false;
    }
    physicalEnd = region.PhysicalStart + lastByteOffset;
    return true;
}

[[nodiscard]] bool IsAcpiMemoryType(const Foundation::UInt32 firmwareType) noexcept
{
    return (firmwareType == AcpiReclaimMemoryType) || (firmwareType == AcpiNonVolatileMemoryType);
}

[[nodiscard]] bool IsIa64DescriptorPolicyValid(const Boot::PhysicalMemoryRegion& region) noexcept
{
    const bool runtime = (region.Attributes & RuntimeMemoryAttribute) != 0U;
    const bool acpi = IsAcpiMemoryType(region.FirmwareType);
    if (acpi && runtime)
    {
        return false;
    }
    if (!runtime && !acpi)
    {
        return true;
    }
    return ((region.PhysicalStart & Ia64RuntimeAlignmentMask) == 0U) &&
           ((region.VirtualStart == 0U) || ((region.VirtualStart & Ia64RuntimeAlignmentMask) == 0U)) &&
           ((region.PageCount & Foundation::UInt64{1U}) == 0U);
}

[[nodiscard]] bool IsBootInfoValid(const Boot::BootInfo& bootInfo) noexcept
{
    if ((bootInfo.Signature != Boot::BootInfoSignature) || (bootInfo.Version != Boot::BootInfoVersion) ||
        (bootInfo.SizeBytes != sizeof(Boot::BootInfo)) || (bootInfo.MemoryRegionCount == 0U) ||
        (bootInfo.MemoryRegionCount > Boot::MaximumMemoryRegions) ||
        (bootInfo.FirmwareDescriptorVersion != ExpectedFirmwareDescriptorVersion) ||
        (bootInfo.FirmwareDescriptorSize < MinimumFirmwareDescriptorSize) || (bootInfo.Reserved != 0U))
    {
        return false;
    }

    const Foundation::UInt64 expectedMapSize = static_cast<Foundation::UInt64>(bootInfo.MemoryRegionCount) *
                                               static_cast<Foundation::UInt64>(bootInfo.FirmwareDescriptorSize);
    if (bootInfo.FirmwareMemoryMapSizeBytes != expectedMapSize)
    {
        return false;
    }

    Foundation::UInt64 previousEnd{0U};
    bool hasPrevious{false};
    const Foundation::Size regionCount = static_cast<Foundation::Size>(bootInfo.MemoryRegionCount);
    for (Foundation::Size index{0U}; index < regionCount; ++index)
    {
        const Boot::PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        Foundation::UInt64 regionEnd{0U};
        if (!TryGetRegionEnd(region, regionEnd) || !IsFirmwareMemoryTypeValid(region.FirmwareType) ||
            (region.Kind != ConvertFirmwareMemoryType(region.FirmwareType)) || !IsIa64DescriptorPolicyValid(region) ||
            ((region.FirmwareType == ConventionalMemoryType) && ((region.Attributes & RuntimeMemoryAttribute) != 0U)) ||
            (hasPrevious && (region.PhysicalStart <= previousEnd)))
        {
            return false;
        }
        previousEnd = regionEnd;
        hasPrevious = true;
    }
    return true;
}

[[nodiscard]] bool IsPhysicalAddress(const Foundation::UInt64 address, const Foundation::UInt64 alignment) noexcept
{
    return (address != 0U) && ((address & PhysicalAddressTopBit) == 0U) && ((address & (alignment - 1U)) == 0U);
}

[[nodiscard]] bool IsPhysicalAddressValue(const Foundation::UInt64 address) noexcept
{
    return (address != 0U) && ((address & PhysicalAddressTopBit) == 0U);
}

[[nodiscard]] bool IsSalInformationValid(const SalSystemTableInformation& information) noexcept
{
    return (information.Reserved8 == 0U) && IsPhysicalAddress(information.PalProcedurePhysicalAddress, 16U) &&
           IsPhysicalAddress(information.SalProcedurePhysicalAddress, 16U) &&
           IsPhysicalAddressValue(information.SalGlobalDataPhysicalAddress);
}

[[nodiscard]] const Boot::PhysicalMemoryRegion* FindContainingRegion(const Boot::BootInfo& bootInfo,
                                                                     const Foundation::UInt64 address,
                                                                     const Foundation::UInt64 accessBytes) noexcept
{
    if ((accessBytes == 0U) || ((accessBytes - 1U) > MaximumPhysicalAddress) ||
        (address > (MaximumPhysicalAddress - (accessBytes - 1U))))
    {
        return nullptr;
    }
    const Foundation::UInt64 accessEnd = address + accessBytes - 1U;
    const Foundation::Size regionCount = static_cast<Foundation::Size>(bootInfo.MemoryRegionCount);
    for (Foundation::Size index{0U}; index < regionCount; ++index)
    {
        const Boot::PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        Foundation::UInt64 regionEnd{0U};
        if (!TryGetRegionEnd(region, regionEnd))
        {
            return nullptr;
        }
        if ((address >= region.PhysicalStart) && (accessEnd <= regionEnd))
        {
            return &region;
        }
    }
    return nullptr;
}
} // namespace

Foundation::Result<SalEntrypointBinding, SalEntrypointBindError>
SalEntrypointBinder::Bind(const SalSystemTableInformation& salInformation, const Boot::BootInfo& bootInfo) noexcept
{
    if (!IsSalInformationValid(salInformation))
    {
        return Fail(SalEntrypointBindError::InvalidSalInformation);
    }
    if (!IsBootInfoValid(bootInfo))
    {
        return Fail(SalEntrypointBindError::InvalidBootInfo);
    }

    const Boot::PhysicalMemoryRegion* const palRegion =
        FindContainingRegion(bootInfo, salInformation.PalProcedurePhysicalAddress, PalProcedureAccessBytes);
    if ((palRegion == nullptr) || (palRegion->FirmwareType != PalCodeMemoryType) ||
        (palRegion->Kind != Boot::PhysicalMemoryKind::PalCode))
    {
        return Fail(SalEntrypointBindError::PalProcedureOutsidePalCode);
    }

    const Boot::PhysicalMemoryRegion* const salCodeRegion =
        FindContainingRegion(bootInfo, salInformation.SalProcedurePhysicalAddress, SalProcedureAccessBytes);
    if ((salCodeRegion == nullptr) || (salCodeRegion->FirmwareType != RuntimeServicesCodeMemoryType) ||
        (salCodeRegion->Kind != Boot::PhysicalMemoryKind::RuntimeServices))
    {
        return Fail(SalEntrypointBindError::SalProcedureOutsideRuntimeCode);
    }
    if ((salCodeRegion->Attributes & RuntimeMemoryAttribute) == 0U)
    {
        return Fail(SalEntrypointBindError::MissingRuntimeAttribute);
    }

    const Boot::PhysicalMemoryRegion* const salDataRegion =
        FindContainingRegion(bootInfo, salInformation.SalGlobalDataPhysicalAddress, SalGlobalDataAccessBytes);
    if ((salDataRegion == nullptr) ||
        ((salDataRegion->FirmwareType != RuntimeServicesCodeMemoryType) &&
         (salDataRegion->FirmwareType != RuntimeServicesDataMemoryType)) ||
        (salDataRegion->Kind != Boot::PhysicalMemoryKind::RuntimeServices))
    {
        return Fail(SalEntrypointBindError::SalGlobalDataOutsideRuntimeStorage);
    }
    if ((salDataRegion->Attributes & RuntimeMemoryAttribute) == 0U)
    {
        return Fail(SalEntrypointBindError::MissingRuntimeAttribute);
    }

    return Foundation::Result<SalEntrypointBinding, SalEntrypointBindError>::Success(SalEntrypointBinding{
        .PalProcedurePhysicalAddress = salInformation.PalProcedurePhysicalAddress,
        .SalProcedurePhysicalAddress = salInformation.SalProcedurePhysicalAddress,
        .SalGlobalDataPhysicalAddress = salInformation.SalGlobalDataPhysicalAddress,
        .PalCodeRegion = *palRegion,
        .SalCodeRegion = *salCodeRegion,
        .SalGlobalDataRegion = *salDataRegion,
    });
}
} // namespace Asteria::Platform::Ia64::Firmware
