// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/AhciIdentifyDmaPageBuilder.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumUInt32{0xFFFF'FFFFULL};
constexpr Foundation::UInt32 CommandFisLengthDwords{5U};
constexpr Foundation::UInt32 PrdtEntryCount{1U};
constexpr Foundation::UInt32 PrdtEntryCountShift{16U};
constexpr Foundation::UInt32 IdentifyDataByteCountMinusOne{511U};
constexpr Foundation::Size CommandTablePrdtOffsetBytes{128U};
constexpr Foundation::Size CommandHeaderCtbaOffsetBytes{8U};
constexpr Foundation::Size PrdtDataBaseOffsetBytes{0U};
constexpr Foundation::Size PrdtByteCountOffsetBytes{12U};
constexpr Foundation::UInt8 RegisterHostToDeviceFisType{0x27U};
constexpr Foundation::UInt8 RegisterFisCommandFlag{0x80U};
constexpr Foundation::UInt8 AtaIdentifyDeviceCommand{0xECU};

constexpr void ZeroBytes(Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = 0U;
    }
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
    WriteLittleEndian32(bytes, static_cast<Foundation::UInt32>(address & MaximumUInt32));
    WriteLittleEndian32(bytes + 4U, static_cast<Foundation::UInt32>(address >> 32U));
}
} // namespace

AhciIdentifyDmaPageBuildResult AhciIdentifyDmaPageBuilder::Build(const AhciDmaPageWritableView page,
                                                                 const bool supports64BitAddressing) noexcept
{
    if (page.Data == nullptr)
    {
        return AhciIdentifyDmaPageBuildResult::Failure(AhciIdentifyDmaPageError::NullBuffer);
    }
    if (page.SizeBytes != PageSizeBytes)
    {
        return AhciIdentifyDmaPageBuildResult::Failure(AhciIdentifyDmaPageError::InvalidBufferSize);
    }

    const Foundation::UIntPointer virtualAddress = reinterpret_cast<Foundation::UIntPointer>(page.Data);
    if ((virtualAddress % PageAlignmentBytes) != 0U)
    {
        return AhciIdentifyDmaPageBuildResult::Failure(AhciIdentifyDmaPageError::InvalidVirtualAlignment);
    }
    if (page.PhysicalAddress > (MaximumUInt64 - (PageSizeBytes - 1U)))
    {
        return AhciIdentifyDmaPageBuildResult::Failure(AhciIdentifyDmaPageError::PhysicalAddressWrap);
    }
    if ((page.PhysicalAddress % PageAlignmentBytes) != 0U)
    {
        return AhciIdentifyDmaPageBuildResult::Failure(AhciIdentifyDmaPageError::InvalidPhysicalAlignment);
    }
    if (!supports64BitAddressing && (page.PhysicalAddress > (MaximumUInt32 - (PageSizeBytes - 1U))))
    {
        return AhciIdentifyDmaPageBuildResult::Failure(AhciIdentifyDmaPageError::ControllerAddressWidthExceeded);
    }

    const AhciIdentifyDmaPageLayout layout{
        .CommandListPhysicalAddress = page.PhysicalAddress + CommandListOffsetBytes,
        .ReceivedFisPhysicalAddress = page.PhysicalAddress + ReceivedFisOffsetBytes,
        .CommandTablePhysicalAddress = page.PhysicalAddress + CommandTableOffsetBytes,
        .IdentifyDataPhysicalAddress = page.PhysicalAddress + IdentifyDataOffsetBytes,
    };

    ZeroBytes(page.Data + CommandListOffsetBytes, CommandListSizeBytes);
    ZeroBytes(page.Data + ReceivedFisOffsetBytes, ReceivedFisSizeBytes);
    ZeroBytes(page.Data + CommandTableOffsetBytes, CommandTableSizeBytes);
    ZeroBytes(page.Data + IdentifyDataOffsetBytes, IdentifyDataSizeBytes);

    Foundation::UInt8* const commandHeader = page.Data + CommandListOffsetBytes;
    const Foundation::UInt32 commandHeaderDescription =
        CommandFisLengthDwords | (PrdtEntryCount << PrdtEntryCountShift);
    WriteLittleEndian32(commandHeader, commandHeaderDescription);
    WriteAddress(commandHeader + CommandHeaderCtbaOffsetBytes, layout.CommandTablePhysicalAddress);

    Foundation::UInt8* const commandFis = page.Data + CommandTableOffsetBytes;
    commandFis[0U] = RegisterHostToDeviceFisType;
    commandFis[1U] = RegisterFisCommandFlag;
    commandFis[2U] = AtaIdentifyDeviceCommand;

    Foundation::UInt8* const prdt = commandFis + CommandTablePrdtOffsetBytes;
    WriteAddress(prdt + PrdtDataBaseOffsetBytes, layout.IdentifyDataPhysicalAddress);
    WriteLittleEndian32(prdt + PrdtByteCountOffsetBytes, IdentifyDataByteCountMinusOne);

    return AhciIdentifyDmaPageBuildResult::Success(layout);
}
} // namespace Asteria::Kernel::Device
