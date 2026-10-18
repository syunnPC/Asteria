// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/Detail/AhciIdentifyDeviceInformationParser.hpp"

namespace Asteria::Kernel::Device::Detail
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumLba28SectorCount{0x0FFF'FFFFULL};

[[nodiscard]] constexpr Foundation::UInt16 ReadIdentifyWord(const Foundation::UInt8* const identifyData,
                                                            const Foundation::Size wordIndex) noexcept
{
    const Foundation::Size offset = wordIndex * 2U;
    return static_cast<Foundation::UInt16>(
        static_cast<Foundation::UInt16>(identifyData[offset]) |
        static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(identifyData[offset + 1U]) << 8U));
}

void ParseAtaString(const Foundation::UInt8* const identifyData, const Foundation::Size firstWord,
                    const Foundation::Size wordCount, char* const output, Foundation::UInt8& outputLength) noexcept
{
    const Foundation::Size characterCount = wordCount * 2U;
    for (Foundation::Size index{0U}; index <= characterCount; ++index)
    {
        output[index] = '\0';
    }
    for (Foundation::Size index{0U}; index < wordCount; ++index)
    {
        const Foundation::Size sourceOffset = (firstWord + index) * 2U;
        output[index * 2U] = static_cast<char>(identifyData[sourceOffset + 1U]);
        output[(index * 2U) + 1U] = static_cast<char>(identifyData[sourceOffset]);
    }

    Foundation::Size length{characterCount};
    for (Foundation::Size index{0U}; index < characterCount; ++index)
    {
        if (output[index] == '\0')
        {
            length = index;
            break;
        }
    }
    while ((length != 0U) && ((output[length - 1U] == ' ') || (output[length - 1U] == '\0')))
    {
        --length;
    }
    for (Foundation::Size index{length}; index <= characterCount; ++index)
    {
        output[index] = '\0';
    }
    outputLength = static_cast<Foundation::UInt8>(length);
}
} // namespace

AhciIdentifyCommandError ParseAhciIdentifyDeviceInformation(const Foundation::UInt8* const identifyData,
                                                            AhciIdentifyDeviceInformation& output) noexcept
{
    AhciIdentifyDeviceInformation information{};
    ParseAtaString(identifyData, 10U, 10U, information.SerialNumber, information.SerialNumberLength);
    ParseAtaString(identifyData, 27U, 20U, information.ModelNumber, information.ModelNumberLength);

    const Foundation::UInt16 commandSetWord = ReadIdentifyWord(identifyData, 83U);
    const bool commandSetWordValid = (commandSetWord & 0xC000U) == 0x4000U;
    information.Uses48BitAddressing = commandSetWordValid && ((commandSetWord & (Foundation::UInt16{1U} << 10U)) != 0U);
    if (information.Uses48BitAddressing)
    {
        if (ReadIdentifyWord(identifyData, 103U) != 0U)
        {
            return AhciIdentifyCommandError::InvalidIdentifyData;
        }
        for (Foundation::Size index{0U}; index < 3U; ++index)
        {
            information.SectorCount |= static_cast<Foundation::UInt64>(ReadIdentifyWord(identifyData, 100U + index))
                                       << (index * 16U);
        }
    }
    else
    {
        const Foundation::UInt16 capabilitiesWord = ReadIdentifyWord(identifyData, 49U);
        if ((capabilitiesWord & (Foundation::UInt16{1U} << 9U)) != 0U)
        {
            information.SectorCount = static_cast<Foundation::UInt64>(ReadIdentifyWord(identifyData, 60U)) |
                                      (static_cast<Foundation::UInt64>(ReadIdentifyWord(identifyData, 61U)) << 16U);
            if ((information.SectorCount == 0U) || (information.SectorCount > MaximumLba28SectorCount))
            {
                return AhciIdentifyCommandError::InvalidIdentifyData;
            }
        }
    }

    information.LogicalSectorSizeBytes = 512U;
    const Foundation::UInt16 sectorDescription = ReadIdentifyWord(identifyData, 106U);
    const bool sectorDescriptionValid = (sectorDescription & 0xC000U) == 0x4000U;
    if (sectorDescriptionValid && ((sectorDescription & (Foundation::UInt16{1U} << 12U)) != 0U))
    {
        const Foundation::UInt64 logicalWords =
            static_cast<Foundation::UInt64>(ReadIdentifyWord(identifyData, 117U)) |
            (static_cast<Foundation::UInt64>(ReadIdentifyWord(identifyData, 118U)) << 16U);
        if (logicalWords <= 256U)
        {
            return AhciIdentifyCommandError::InvalidIdentifyData;
        }
        information.LogicalSectorSizeBytes = logicalWords * 2U;
    }

    information.PhysicalSectorSizeBytes = information.LogicalSectorSizeBytes;
    if (sectorDescriptionValid && ((sectorDescription & (Foundation::UInt16{1U} << 13U)) != 0U))
    {
        const Foundation::UInt16 exponent = sectorDescription & 0x000FU;
        information.PhysicalSectorSizeBytes <<= exponent;
    }
    if ((information.SectorCount != 0U) &&
        (information.LogicalSectorSizeBytes > (MaximumUInt64 / information.SectorCount)))
    {
        return AhciIdentifyCommandError::IdentifyCapacityOverflow;
    }
    information.CapacityBytes = information.SectorCount * information.LogicalSectorSizeBytes;
    output = information;
    return AhciIdentifyCommandError::None;
}
} // namespace Asteria::Kernel::Device::Detail
