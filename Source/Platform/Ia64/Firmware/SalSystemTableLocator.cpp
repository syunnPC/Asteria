// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/SalSystemTableLocator.hpp"

#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
constexpr Foundation::Size MinimumMemoryDescriptorSize{40U};
constexpr Foundation::Size MaximumMemoryDescriptorCount{4096U};
constexpr Foundation::UInt32 MemoryDescriptorVersion{1U};
constexpr Foundation::UInt32 BootServicesDataMemoryType{4U};
constexpr Foundation::UInt32 RuntimeServicesCodeMemoryType{5U};
constexpr Foundation::UInt32 RuntimeServicesDataMemoryType{6U};
constexpr Foundation::UInt32 AcpiReclaimMemoryType{9U};
constexpr Foundation::UInt32 AcpiNonVolatileMemoryType{10U};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumPageCount{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 Ia64RuntimeAlignmentBytes{8192U};
constexpr Foundation::UInt64 Ia64RuntimeAlignmentMask{Ia64RuntimeAlignmentBytes - 1U};
constexpr Foundation::UInt64 RuntimeMemoryAttribute{Foundation::UInt64{1U} << 63U};

[[nodiscard]] Foundation::Result<SalSystemTableLocation, SalSystemTableLocateError>
Fail(const SalSystemTableLocateError error) noexcept
{
    return Foundation::Result<SalSystemTableLocation, SalSystemTableLocateError>::Failure(error);
}

[[nodiscard]] bool AreGuidsEqual(const Boot::Uefi::Guid& first, const Boot::Uefi::Guid& second) noexcept
{
    if ((first.Data1 != second.Data1) || (first.Data2 != second.Data2) || (first.Data3 != second.Data3))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (first.Data4[index] != second.Data4[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Foundation::UInt32 ReadLittleEndian32(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt32 value{0U};
    for (Foundation::UInt8 index{0U}; index < 4U; ++index)
    {
        value |= static_cast<Foundation::UInt32>(bytes[index]) << static_cast<Foundation::UInt8>(index * 8U);
    }
    return value;
}

[[nodiscard]] Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::UInt8 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << static_cast<Foundation::UInt8>(index * 8U);
    }
    return value;
}

[[nodiscard]] bool IsWholeDescriptorArray(const Foundation::Size memoryMapSize,
                                          const Foundation::Size descriptorSize) noexcept
{
    Foundation::Size remaining = memoryMapSize;
    Foundation::Size descriptorCount{0U};
    while ((remaining >= descriptorSize) && (descriptorCount < MaximumMemoryDescriptorCount))
    {
        remaining -= descriptorSize;
        ++descriptorCount;
    }
    return remaining == 0U;
}

[[nodiscard]] bool TryGetDescriptorEnd(const Foundation::UInt64 physicalStart, const Foundation::UInt64 virtualStart,
                                       const Foundation::UInt64 pageCount, Foundation::UInt64& physicalEnd) noexcept
{
    if ((pageCount == 0U) || (pageCount > MaximumPageCount) || ((physicalStart & PageOffsetMask) != 0U) ||
        ((virtualStart != 0U) && ((virtualStart & PageOffsetMask) != 0U)))
    {
        return false;
    }
    const Foundation::UInt64 maximumAddress = ~Foundation::UInt64{0U};
    if (pageCount == MaximumPageCount)
    {
        if ((physicalStart != 0U) || (virtualStart != 0U))
        {
            return false;
        }
        physicalEnd = maximumAddress;
        return true;
    }

    const Foundation::UInt64 byteCount = pageCount << 12U;
    const Foundation::UInt64 lastByteOffset = byteCount - 1U;
    if ((physicalStart > (maximumAddress - lastByteOffset)) ||
        ((virtualStart != 0U) && (virtualStart > (maximumAddress - lastByteOffset))))
    {
        return false;
    }
    physicalEnd = physicalStart + lastByteOffset;
    return true;
}

[[nodiscard]] bool IsAcpiMemoryType(const Foundation::UInt32 type) noexcept
{
    return (type == AcpiReclaimMemoryType) || (type == AcpiNonVolatileMemoryType);
}

[[nodiscard]] bool IsIa64DescriptorPolicyValid(const Foundation::UInt32 type, const Foundation::UInt64 physicalStart,
                                               const Foundation::UInt64 virtualStart,
                                               const Foundation::UInt64 pageCount,
                                               const Foundation::UInt64 attributes) noexcept
{
    const bool runtime = (attributes & RuntimeMemoryAttribute) != 0U;
    const bool acpi = IsAcpiMemoryType(type);
    if (acpi && runtime)
    {
        return false;
    }
    if (!runtime && !acpi)
    {
        return true;
    }
    return ((physicalStart & Ia64RuntimeAlignmentMask) == 0U) &&
           ((virtualStart == 0U) || ((virtualStart & Ia64RuntimeAlignmentMask) == 0U)) &&
           ((pageCount & Foundation::UInt64{1U}) == 0U);
}

[[nodiscard]] bool IsSupportedTableMemoryType(const Foundation::UInt32 type) noexcept
{
    return (type == BootServicesDataMemoryType)
           // UEFI permits a runtime image's code and data to share one Code
           // descriptor. Capture only reads these bytes into owned storage.
           || (type == RuntimeServicesCodeMemoryType) || (type == RuntimeServicesDataMemoryType) ||
           IsAcpiMemoryType(type);
}

[[nodiscard]] bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                 const Foundation::UInt64 secondStart, const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart <= secondEnd) && (secondStart <= firstEnd);
}
} // namespace

Foundation::Result<SalSystemTableLocation, SalSystemTableLocateError>
SalSystemTableLocator::Locate(const Boot::Uefi::ConfigurationTable* const configurationTables,
                              const Foundation::Size configurationTableCount, const void* const memoryMap,
                              const Foundation::Size memoryMapSize, const Foundation::Size memoryDescriptorSize,
                              const Foundation::UInt32 memoryDescriptorVersion) noexcept
{
    if ((configurationTableCount > MaximumFirmwareConfigurationTables) ||
        ((configurationTableCount != 0U) && (configurationTables == nullptr)))
    {
        return Fail(SalSystemTableLocateError::InvalidConfigurationTableArray);
    }

    const void* tablePointer{nullptr};
    bool foundTable{false};
    for (Foundation::Size index{0U}; index < configurationTableCount; ++index)
    {
        if (!AreGuidsEqual(configurationTables[index].VendorGuid, SalSystemTableGuid))
        {
            continue;
        }
        if (foundTable)
        {
            return Fail(SalSystemTableLocateError::DuplicateSalSystemTable);
        }
        foundTable = true;
        tablePointer = configurationTables[index].VendorTable;
    }
    if (!foundTable)
    {
        return Fail(SalSystemTableLocateError::SalSystemTableNotFound);
    }
    if (tablePointer == nullptr)
    {
        return Fail(SalSystemTableLocateError::NullSalSystemTable);
    }

    if ((memoryMap == nullptr) || (memoryMapSize < memoryDescriptorSize) ||
        (memoryDescriptorSize < MinimumMemoryDescriptorSize) || (memoryDescriptorVersion != MemoryDescriptorVersion) ||
        !IsWholeDescriptorArray(memoryMapSize, memoryDescriptorSize))
    {
        return Fail(SalSystemTableLocateError::InvalidMemoryMap);
    }

    const Foundation::UInt64 tableAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(tablePointer));
    const auto* const memoryMapBytes = static_cast<const Foundation::UInt8*>(memoryMap);
    Foundation::Size descriptorCount{0U};
    Foundation::Size matchingDescriptorCount{0U};
    Foundation::Size matchingDescriptorOffset{0U};
    Foundation::UInt64 matchingDescriptorStart{0U};
    Foundation::UInt64 matchingDescriptorEnd{0U};
    Foundation::UInt64 matchingAttributes{0U};
    Foundation::UInt32 matchingType{0U};

    for (Foundation::Size offset{0U}; offset < memoryMapSize; offset += memoryDescriptorSize)
    {
        if (descriptorCount >= MaximumMemoryDescriptorCount)
        {
            return Fail(SalSystemTableLocateError::InvalidMemoryMap);
        }
        ++descriptorCount;
        const Foundation::UInt8* const descriptor = memoryMapBytes + offset;
        const Foundation::UInt32 type = ReadLittleEndian32(descriptor);
        const Foundation::UInt64 physicalStart = ReadLittleEndian64(descriptor + 8U);
        const Foundation::UInt64 virtualStart = ReadLittleEndian64(descriptor + 16U);
        const Foundation::UInt64 pageCount = ReadLittleEndian64(descriptor + 24U);
        const Foundation::UInt64 attributes = ReadLittleEndian64(descriptor + 32U);
        Foundation::UInt64 descriptorEnd{0U};
        if (!TryGetDescriptorEnd(physicalStart, virtualStart, pageCount, descriptorEnd) ||
            !IsIa64DescriptorPolicyValid(type, physicalStart, virtualStart, pageCount, attributes))
        {
            return Fail(SalSystemTableLocateError::InvalidMemoryMap);
        }

        if ((tableAddress >= physicalStart) && (tableAddress <= descriptorEnd))
        {
            ++matchingDescriptorCount;
            matchingDescriptorOffset = offset;
            matchingDescriptorStart = physicalStart;
            matchingDescriptorEnd = descriptorEnd;
            matchingAttributes = attributes;
            matchingType = type;
        }
    }

    if (matchingDescriptorCount == 0U)
    {
        return Fail(SalSystemTableLocateError::TableAddressNotInMemoryMap);
    }
    if (matchingDescriptorCount != 1U)
    {
        return Fail(SalSystemTableLocateError::AmbiguousMemoryDescriptor);
    }
    const Foundation::Size availableBytes = static_cast<Foundation::Size>(matchingDescriptorEnd - tableAddress + 1U);
    if (availableBytes < SalSystemTableHeaderSize)
    {
        return Fail(SalSystemTableLocateError::HeaderOutsideMemoryDescriptor);
    }

    for (Foundation::Size offset{0U}; offset < memoryMapSize; offset += memoryDescriptorSize)
    {
        if (offset == matchingDescriptorOffset)
        {
            continue;
        }
        const Foundation::UInt8* const descriptor = memoryMapBytes + offset;
        const Foundation::UInt64 physicalStart = ReadLittleEndian64(descriptor + 8U);
        const Foundation::UInt64 virtualStart = ReadLittleEndian64(descriptor + 16U);
        const Foundation::UInt64 pageCount = ReadLittleEndian64(descriptor + 24U);
        Foundation::UInt64 descriptorEnd{0U};
        if (!TryGetDescriptorEnd(physicalStart, virtualStart, pageCount, descriptorEnd))
        {
            return Fail(SalSystemTableLocateError::InvalidMemoryMap);
        }
        if (RangesOverlap(matchingDescriptorStart, matchingDescriptorEnd, physicalStart, descriptorEnd))
        {
            return Fail(SalSystemTableLocateError::AmbiguousMemoryDescriptor);
        }
    }

    if (!IsSupportedTableMemoryType(matchingType))
    {
        return Fail(SalSystemTableLocateError::UnsupportedMemoryType);
    }
    if (((matchingType == RuntimeServicesCodeMemoryType) || (matchingType == RuntimeServicesDataMemoryType)) &&
        ((matchingAttributes & RuntimeMemoryAttribute) == 0U))
    {
        return Fail(SalSystemTableLocateError::MissingRuntimeAttribute);
    }

    return Foundation::Result<SalSystemTableLocation, SalSystemTableLocateError>::Success(SalSystemTableLocation{
        .Bytes = static_cast<const Foundation::UInt8*>(tablePointer),
        .AvailableBytes = availableBytes,
        .PhysicalAddress = tableAddress,
        .MemoryAttributes = matchingAttributes,
        .FirmwareMemoryType = matchingType,
        .Reserved = 0U,
    });
}
} // namespace Asteria::Platform::Ia64::Firmware
