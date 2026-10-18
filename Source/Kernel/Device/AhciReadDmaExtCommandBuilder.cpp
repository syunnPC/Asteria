// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/AhciReadDmaExtCommandBuilder.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumUInt32{0xFFFF'FFFFULL};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt32 CommandFisLengthDwords{5U};
constexpr Foundation::UInt32 PrdtEntryCount{1U};
constexpr Foundation::UInt32 PrdtEntryCountShift{16U};
constexpr Foundation::Size CommandHeaderCtbaOffsetBytes{8U};
constexpr Foundation::Size CommandTablePrdtOffsetBytes{128U};
constexpr Foundation::Size PrdtByteCountOffsetBytes{12U};
constexpr Foundation::UInt8 RegisterHostToDeviceFisType{0x27U};
constexpr Foundation::UInt8 RegisterFisCommandFlag{0x80U};
constexpr Foundation::UInt8 AtaReadDmaExtCommand{0x25U};
constexpr Foundation::UInt8 AtaLbaDeviceFlag{0x40U};

constexpr void ZeroBytes(Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = 0U;
    }
}

constexpr void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

constexpr void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

constexpr void WriteAddress(Foundation::UInt8* const bytes, const Foundation::UInt64 address) noexcept
{
    WriteLittleEndian32(bytes, static_cast<Foundation::UInt32>(address));
    WriteLittleEndian32(bytes + 4U, static_cast<Foundation::UInt32>(address >> 32U));
}

[[nodiscard]] constexpr bool SpansOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                          const Foundation::UInt64 secondStart,
                                          const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart <= secondEnd) && (secondStart <= firstEnd);
}

[[nodiscard]] bool VirtualRangesOverlap(const void* const first, const Foundation::Size firstSizeBytes,
                                        const void* const second, const Foundation::Size secondSizeBytes) noexcept
{
    if ((firstSizeBytes == 0U) || (secondSizeBytes == 0U))
    {
        return false;
    }

    const Foundation::UIntPointer firstStart = reinterpret_cast<Foundation::UIntPointer>(first);
    const Foundation::UIntPointer secondStart = reinterpret_cast<Foundation::UIntPointer>(second);
    const Foundation::UIntPointer firstLength = static_cast<Foundation::UIntPointer>(firstSizeBytes - 1U);
    const Foundation::UIntPointer secondLength = static_cast<Foundation::UIntPointer>(secondSizeBytes - 1U);
    if ((firstStart > (MaximumUIntPointer - firstLength)) || (secondStart > (MaximumUIntPointer - secondLength)))
    {
        return true;
    }

    const Foundation::UIntPointer firstEnd = firstStart + firstLength;
    const Foundation::UIntPointer secondEnd = secondStart + secondLength;
    return (firstStart <= secondEnd) && (secondStart <= firstEnd);
}
} // namespace

AhciReadDmaExtCommandBuildResult
AhciReadDmaExtCommandBuilder::Build(const AhciReadDmaExtCommandRequest& request) noexcept
{
    const AhciReadDmaExtCommandPageWritableView& commandPage = request.CommandPage;
    if (commandPage.Data == nullptr)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::NullCommandPage);
    }
    if (commandPage.SizeBytes != CommandPageSizeBytes)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::InvalidCommandPageSize);
    }
    const Foundation::UIntPointer virtualAddress = reinterpret_cast<Foundation::UIntPointer>(commandPage.Data);
    if ((virtualAddress % CommandPageAlignmentBytes) != 0U)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(
            AhciReadDmaExtCommandError::InvalidCommandPageVirtualAlignment);
    }
    if (VirtualRangesOverlap(commandPage.Data, CommandPageSizeBytes, &request, sizeof(request)))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::CommandPageOverlapsRequest);
    }
    if (commandPage.PhysicalAddress > (MaximumUInt64 - (CommandPageSizeBytes - 1U)))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::CommandPagePhysicalAddressWrap);
    }
    if ((commandPage.PhysicalAddress % CommandPageAlignmentBytes) != 0U)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(
            AhciReadDmaExtCommandError::InvalidCommandPagePhysicalAlignment);
    }

    if (request.SectorCount == 0U)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::ZeroSectorCount);
    }
    if (request.SectorCount > MaximumSectorCount)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::SectorCountNotEncodable);
    }
    if (!request.Supports48BitAddressing)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(
            AhciReadDmaExtCommandError::DeviceDoesNotSupport48BitAddressing);
    }
    if ((request.LogicalSectorSizeBytes < MinimumLogicalSectorSizeBytes) ||
        ((request.LogicalSectorSizeBytes % LogicalSectorSizeAlignmentBytes) != 0U))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::UnsupportedLogicalSectorSize);
    }
    if (request.LogicalSectorSizeBytes > (MaximumUInt64 / request.SectorCount))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::TransferByteCountOverflow);
    }
    const Foundation::UInt64 transferSizeBytes = request.SectorCount * request.LogicalSectorSizeBytes;
    if (transferSizeBytes > MaximumPrdtTransferSizeBytes)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::TransferExceedsPrdtEntry);
    }

    if (request.StartingLba > MaximumLba)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::LbaNotEncodable);
    }
    const Foundation::UInt64 sectorsAfterFirst = request.SectorCount - 1U;
    if (sectorsAfterFirst > (MaximumLba - request.StartingLba))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::LbaNotEncodable);
    }
    if ((request.StartingLba >= request.DeviceSectorCount) ||
        (request.SectorCount > (request.DeviceSectorCount - request.StartingLba)))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::TransferOutsideDeviceCapacity);
    }

    if (request.DataBuffer.SizeBytes != transferSizeBytes)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::InvalidDataBufferSize);
    }
    if (request.DataBuffer.PhysicalAddress > (MaximumUInt64 - (transferSizeBytes - 1U)))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::DataBufferPhysicalAddressWrap);
    }
    if ((request.DataBuffer.PhysicalAddress % DataBufferAlignmentBytes) != 0U)
    {
        return AhciReadDmaExtCommandBuildResult::Failure(
            AhciReadDmaExtCommandError::InvalidDataBufferPhysicalAlignment);
    }

    const Foundation::UInt64 commandPageEnd = commandPage.PhysicalAddress + (CommandPageSizeBytes - 1U);
    const Foundation::UInt64 dataBufferEnd = request.DataBuffer.PhysicalAddress + (transferSizeBytes - 1U);
    if (!request.Supports64BitAddressing && ((commandPageEnd > MaximumUInt32) || (dataBufferEnd > MaximumUInt32)))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::ControllerAddressWidthExceeded);
    }
    if (SpansOverlap(commandPage.PhysicalAddress, commandPageEnd, request.DataBuffer.PhysicalAddress, dataBufferEnd))
    {
        return AhciReadDmaExtCommandBuildResult::Failure(AhciReadDmaExtCommandError::CommandPageOverlapsDataBuffer);
    }

    const AhciReadDmaExtCommandLayout layout{
        .CommandListPhysicalAddress = commandPage.PhysicalAddress + CommandListOffsetBytes,
        .ReceivedFisPhysicalAddress = commandPage.PhysicalAddress + ReceivedFisOffsetBytes,
        .CommandTablePhysicalAddress = commandPage.PhysicalAddress + CommandTableOffsetBytes,
        .DataBufferPhysicalAddress = request.DataBuffer.PhysicalAddress,
        .TransferSizeBytes = transferSizeBytes,
    };

    ZeroBytes(commandPage.Data + CommandListOffsetBytes, CommandListSizeBytes);
    ZeroBytes(commandPage.Data + ReceivedFisOffsetBytes, ReceivedFisSizeBytes);
    ZeroBytes(commandPage.Data + CommandTableOffsetBytes, CommandTableSizeBytes);

    Foundation::UInt8* const commandHeader = commandPage.Data + CommandListOffsetBytes;
    const Foundation::UInt32 commandHeaderDescription =
        CommandFisLengthDwords | (PrdtEntryCount << PrdtEntryCountShift);
    WriteLittleEndian32(commandHeader, commandHeaderDescription);
    WriteAddress(commandHeader + CommandHeaderCtbaOffsetBytes, layout.CommandTablePhysicalAddress);

    Foundation::UInt8* const commandFis = commandPage.Data + CommandTableOffsetBytes;
    commandFis[0U] = RegisterHostToDeviceFisType;
    commandFis[1U] = RegisterFisCommandFlag;
    commandFis[2U] = AtaReadDmaExtCommand;
    commandFis[4U] = static_cast<Foundation::UInt8>(request.StartingLba);
    commandFis[5U] = static_cast<Foundation::UInt8>(request.StartingLba >> 8U);
    commandFis[6U] = static_cast<Foundation::UInt8>(request.StartingLba >> 16U);
    commandFis[7U] = AtaLbaDeviceFlag;
    commandFis[8U] = static_cast<Foundation::UInt8>(request.StartingLba >> 24U);
    commandFis[9U] = static_cast<Foundation::UInt8>(request.StartingLba >> 32U);
    commandFis[10U] = static_cast<Foundation::UInt8>(request.StartingLba >> 40U);
    WriteLittleEndian16(commandFis + 12U, static_cast<Foundation::UInt16>(request.SectorCount));

    Foundation::UInt8* const prdt = commandFis + CommandTablePrdtOffsetBytes;
    WriteAddress(prdt, request.DataBuffer.PhysicalAddress);
    WriteLittleEndian32(prdt + PrdtByteCountOffsetBytes, static_cast<Foundation::UInt32>(transferSizeBytes - 1U));

    return AhciReadDmaExtCommandBuildResult::Success(layout);
}
} // namespace Asteria::Kernel::Device
