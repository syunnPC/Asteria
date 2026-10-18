// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Firmware/Amd64LoadedImageLayout.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
namespace
{
constexpr Foundation::Size DosHeaderSize{64U};
constexpr Foundation::Size PeSignatureSize{4U};
constexpr Foundation::Size CoffHeaderSize{20U};
constexpr Foundation::Size OptionalHeaderSize{240U};
constexpr Foundation::Size SectionHeaderSize{40U};
constexpr Foundation::UInt16 Amd64Machine{0x8664U};
constexpr Foundation::UInt16 Pe32PlusMagic{0x020BU};
constexpr Foundation::UInt16 EfiApplicationSubsystem{10U};
constexpr Foundation::UInt16 ExpectedCoffCharacteristics{0x022EU};
constexpr Foundation::UInt16 ExpectedDllCharacteristics{0x0160U};
constexpr Foundation::UInt32 SectionAlignment{0x1000U};
constexpr Foundation::UInt32 FileAlignment{0x0200U};
constexpr Foundation::UInt32 ExpectedHeaderBytes{0x0400U};
constexpr Foundation::UInt32 DirectoryCount{16U};
constexpr Foundation::Size ImportDirectoryIndex{1U};
constexpr Foundation::Size RelocationDirectoryIndex{5U};
constexpr Foundation::UInt32 CodeSectionCharacteristics{0x6000'0020U};
constexpr Foundation::UInt32 ReadOnlySectionCharacteristics{0x4000'0040U};
constexpr Foundation::UInt32 WritableDataSectionCharacteristics{0xC000'0040U};
constexpr Foundation::UInt32 WritableBssSectionCharacteristics{0xC000'0080U};
constexpr Foundation::UInt32 RelocationSectionCharacteristics{0x4200'0040U};
constexpr Foundation::UInt32 MaximumUInt32{~Foundation::UInt32{0U}};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

struct ExpectedSection
{
    char Name[8U];
    Foundation::UInt32 Characteristics;
    Amd64LoadedImageSectionPurpose Purpose;
    bool HasRawData;
};

constexpr ExpectedSection ExpectedSections[Amd64LoadedImageSectionCount]{
    {".text", CodeSectionCharacteristics, Amd64LoadedImageSectionPurpose::Code, true},
    {".rdata", ReadOnlySectionCharacteristics, Amd64LoadedImageSectionPurpose::ReadOnlyData, true},
    {".rodata", ReadOnlySectionCharacteristics, Amd64LoadedImageSectionPurpose::ReadOnlyData, true},
    {".bss", WritableBssSectionCharacteristics, Amd64LoadedImageSectionPurpose::WritableData, false},
    {".idata", ReadOnlySectionCharacteristics, Amd64LoadedImageSectionPurpose::ReadOnlyData, true},
    {".reloc", RelocationSectionCharacteristics, Amd64LoadedImageSectionPurpose::ReadOnlyData, true},
    {".data", WritableDataSectionCharacteristics, Amd64LoadedImageSectionPurpose::WritableData, true},
};

[[nodiscard]] constexpr bool CanRead(const Foundation::Size offset, const Foundation::Size length,
                                     const Foundation::Size available) noexcept
{
    return (offset <= available) && (length <= (available - offset));
}

[[nodiscard]] Foundation::UInt16 ReadUInt16(const Foundation::UInt8* const bytes,
                                            const Foundation::Size offset) noexcept
{
    return static_cast<Foundation::UInt16>(bytes[offset]) |
           static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] Foundation::UInt32 ReadUInt32(const Foundation::UInt8* const bytes,
                                            const Foundation::Size offset) noexcept
{
    Foundation::UInt32 value{0U};
    for (Foundation::UInt8 index{0U}; index < 4U; ++index)
    {
        value |= static_cast<Foundation::UInt32>(bytes[offset + index])
                 << (static_cast<Foundation::UInt32>(index) * 8U);
    }
    return value;
}

[[nodiscard]] Foundation::UInt64 ReadUInt64(const Foundation::UInt8* const bytes,
                                            const Foundation::Size offset) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::UInt8 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[offset + index])
                 << (static_cast<Foundation::UInt64>(index) * 8U);
    }
    return value;
}

[[nodiscard]] bool HasExpectedName(const Foundation::UInt8* const section, const ExpectedSection& expected) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (section[index] != static_cast<Foundation::UInt8>(expected.Name[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool TryAlignUp(const Foundation::UInt32 value, Foundation::UInt32& aligned) noexcept
{
    constexpr Foundation::UInt32 mask{SectionAlignment - 1U};
    if (value > (MaximumUInt32 - mask))
    {
        return false;
    }
    aligned = (value + mask) & ~mask;
    return aligned != 0U;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] bool IsZeroRange(const Foundation::UInt8* const bytes, const Foundation::Size offset,
                               const Foundation::Size size) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (bytes[offset + index] != 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

Amd64LoadedImageLayoutError Amd64LoadedImageLayoutParser::Parse(const void* const imageBase,
                                                                const Foundation::Size imageSize,
                                                                Amd64LoadedImageLayout& output) noexcept
{
    if (imageBase == nullptr)
    {
        return Amd64LoadedImageLayoutError::NullImage;
    }
    if ((imageSize < ExpectedHeaderBytes) || (imageSize > MaximumAmd64LoadedImageBytes) ||
        ((imageSize & (SectionAlignment - 1U)) != 0U))
    {
        return Amd64LoadedImageLayoutError::ImageSizeOutOfRange;
    }

    const Foundation::UInt64 baseValue =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(imageBase));
    if (((baseValue & (SectionAlignment - 1U)) != 0U) || (baseValue > (MaximumUInt64 - imageSize)))
    {
        return Amd64LoadedImageLayoutError::InvalidImageGeometry;
    }
    const Foundation::UInt64 imageEnd = baseValue + imageSize;
    const Foundation::UInt64 outputStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&output));
    if ((outputStart > (MaximumUInt64 - sizeof(output))) ||
        RangesOverlap(baseValue, imageEnd, outputStart, outputStart + sizeof(output)))
    {
        return Amd64LoadedImageLayoutError::OutputAliasesImage;
    }

    const auto* const bytes = static_cast<const Foundation::UInt8*>(imageBase);
    if (!CanRead(0U, DosHeaderSize, imageSize) || (bytes[0U] != static_cast<Foundation::UInt8>('M')) ||
        (bytes[1U] != static_cast<Foundation::UInt8>('Z')))
    {
        return Amd64LoadedImageLayoutError::InvalidDosHeader;
    }

    const Foundation::Size peOffset = ReadUInt32(bytes, 60U);
    constexpr Foundation::Size peAndCoffBytes{PeSignatureSize + CoffHeaderSize};
    if ((peOffset < DosHeaderSize) || ((peOffset & 3U) != 0U) || !CanRead(peOffset, peAndCoffBytes, imageSize) ||
        (bytes[peOffset] != static_cast<Foundation::UInt8>('P')) ||
        (bytes[peOffset + 1U] != static_cast<Foundation::UInt8>('E')) || (bytes[peOffset + 2U] != 0U) ||
        (bytes[peOffset + 3U] != 0U))
    {
        return Amd64LoadedImageLayoutError::InvalidPeHeader;
    }

    const Foundation::Size coffOffset = peOffset + PeSignatureSize;
    if ((ReadUInt16(bytes, coffOffset) != Amd64Machine) ||
        (ReadUInt16(bytes, coffOffset + 2U) != Amd64LoadedImageSectionCount) ||
        (ReadUInt16(bytes, coffOffset + 16U) != OptionalHeaderSize) ||
        (ReadUInt16(bytes, coffOffset + 18U) != ExpectedCoffCharacteristics))
    {
        return Amd64LoadedImageLayoutError::InvalidCoffHeader;
    }

    const Foundation::Size optionalOffset = coffOffset + CoffHeaderSize;
    if (!CanRead(optionalOffset, OptionalHeaderSize, imageSize) ||
        (ReadUInt16(bytes, optionalOffset) != Pe32PlusMagic) ||
        (ReadUInt32(bytes, optionalOffset + 32U) != SectionAlignment) ||
        (ReadUInt32(bytes, optionalOffset + 36U) != FileAlignment) ||
        (ReadUInt32(bytes, optionalOffset + 60U) != ExpectedHeaderBytes) ||
        (ReadUInt16(bytes, optionalOffset + 68U) != EfiApplicationSubsystem) ||
        (ReadUInt16(bytes, optionalOffset + 70U) != ExpectedDllCharacteristics) ||
        (ReadUInt32(bytes, optionalOffset + 104U) != 0U) ||
        (ReadUInt32(bytes, optionalOffset + 108U) != DirectoryCount))
    {
        return Amd64LoadedImageLayoutError::InvalidOptionalHeader;
    }

    const Foundation::UInt32 entryRva = ReadUInt32(bytes, optionalOffset + 16U);
    const Foundation::UInt32 baseOfCode = ReadUInt32(bytes, optionalOffset + 20U);
    const Foundation::UInt64 preferredImageBase = ReadUInt64(bytes, optionalOffset + 24U);
    const Foundation::UInt32 sizeOfImage = ReadUInt32(bytes, optionalOffset + 56U);
    const bool hasBuildPreferredBase = (preferredImageBase != 0U) && ((preferredImageBase & 0xFFFFU) == 0U);
    const bool hasLoaderRebasedImageBase = preferredImageBase == baseValue;
    if ((!hasBuildPreferredBase && !hasLoaderRebasedImageBase) ||
        (preferredImageBase > (MaximumUInt64 - sizeOfImage)) || (sizeOfImage != imageSize) ||
        (sizeOfImage < (ExpectedHeaderBytes + SectionAlignment)))
    {
        return Amd64LoadedImageLayoutError::InvalidImageGeometry;
    }

    Foundation::UInt32 importRva{0U};
    Foundation::UInt32 importSize{0U};
    Foundation::UInt32 relocationRva{0U};
    Foundation::UInt32 relocationSize{0U};
    for (Foundation::Size index{0U}; index < DirectoryCount; ++index)
    {
        const Foundation::Size directoryOffset = optionalOffset + 112U + (index * 8U);
        const Foundation::UInt32 directoryRva = ReadUInt32(bytes, directoryOffset);
        const Foundation::UInt32 directorySize = ReadUInt32(bytes, directoryOffset + 4U);
        if (index == ImportDirectoryIndex)
        {
            importRva = directoryRva;
            importSize = directorySize;
        }
        else if (index == RelocationDirectoryIndex)
        {
            relocationRva = directoryRva;
            relocationSize = directorySize;
        }
        else if ((directoryRva != 0U) || (directorySize != 0U))
        {
            return Amd64LoadedImageLayoutError::InvalidDirectory;
        }
    }

    const Foundation::Size sectionTableOffset = optionalOffset + OptionalHeaderSize;
    constexpr Foundation::Size sectionTableBytes{Amd64LoadedImageSectionCount * SectionHeaderSize};
    if (!CanRead(sectionTableOffset, sectionTableBytes, ExpectedHeaderBytes) ||
        !CanRead(sectionTableOffset, sectionTableBytes, imageSize))
    {
        return Amd64LoadedImageLayoutError::InvalidSectionTable;
    }

    Amd64LoadedImageLayout layout{
        .ImageBase = baseValue,
        .ImageSize = imageSize,
        .Sections = {},
    };
    Foundation::UInt32 expectedRva{SectionAlignment};
    Foundation::UInt32 expectedRawOffset{ExpectedHeaderBytes};
    Foundation::UInt32 textRva{0U};
    Foundation::UInt32 textVirtualSize{0U};
    Foundation::UInt32 importSectionRva{0U};
    Foundation::UInt32 importSectionSize{0U};
    Foundation::UInt32 relocationSectionRva{0U};
    Foundation::UInt32 relocationSectionSize{0U};
    for (Foundation::Size index{0U}; index < Amd64LoadedImageSectionCount; ++index)
    {
        const Foundation::Size sectionOffset = sectionTableOffset + (index * SectionHeaderSize);
        const Foundation::UInt8* const section = bytes + sectionOffset;
        const ExpectedSection& expected = ExpectedSections[index];
        if (!HasExpectedName(section, expected))
        {
            return Amd64LoadedImageLayoutError::InvalidSectionName;
        }

        const Foundation::UInt32 virtualSize = ReadUInt32(section, 8U);
        const Foundation::UInt32 virtualAddress = ReadUInt32(section, 12U);
        const Foundation::UInt32 rawSize = ReadUInt32(section, 16U);
        const Foundation::UInt32 rawOffset = ReadUInt32(section, 20U);
        const Foundation::UInt32 characteristics = ReadUInt32(section, 36U);
        if (characteristics != expected.Characteristics)
        {
            return Amd64LoadedImageLayoutError::InvalidSectionProtection;
        }
        if ((virtualSize == 0U) || (virtualAddress != expectedRva) ||
            ((virtualAddress & (SectionAlignment - 1U)) != 0U) || (ReadUInt32(section, 24U) != 0U) ||
            (ReadUInt32(section, 28U) != 0U) || (ReadUInt16(section, 32U) != 0U) || (ReadUInt16(section, 34U) != 0U) ||
            (expected.HasRawData && ((rawSize == 0U) || ((rawSize & (FileAlignment - 1U)) != 0U) ||
                                     (rawSize < virtualSize) || (rawOffset != expectedRawOffset))) ||
            (!expected.HasRawData && ((rawSize != 0U) || (rawOffset != 0U))))
        {
            return Amd64LoadedImageLayoutError::InvalidSectionGeometry;
        }

        if (expected.HasRawData)
        {
            if (rawOffset > (MaximumUInt32 - rawSize))
            {
                return Amd64LoadedImageLayoutError::InvalidSectionGeometry;
            }
            expectedRawOffset = rawOffset + rawSize;
        }
        const Foundation::UInt32 loadedBytes = virtualSize > rawSize ? virtualSize : rawSize;
        Foundation::UInt32 alignedBytes{0U};
        if (!TryAlignUp(loadedBytes, alignedBytes) || (virtualAddress > (MaximumUInt32 - alignedBytes)) ||
            ((virtualAddress + alignedBytes) > sizeOfImage) || (baseValue > (MaximumUInt64 - virtualAddress)))
        {
            return Amd64LoadedImageLayoutError::InvalidSectionGeometry;
        }
        layout.Sections[index] = Amd64LoadedImageSection{
            .VirtualStart = baseValue + virtualAddress,
            .PageCount = alignedBytes / SectionAlignment,
            .Purpose = expected.Purpose,
            .Reserved = 0U,
        };
        expectedRva = virtualAddress + alignedBytes;
        if (index == 0U)
        {
            textRva = virtualAddress;
            textVirtualSize = virtualSize;
        }
        else if (index == 4U)
        {
            importSectionRva = virtualAddress;
            importSectionSize = virtualSize;
        }
        else if (index == 5U)
        {
            relocationSectionRva = virtualAddress;
            relocationSectionSize = virtualSize;
        }
    }

    if (expectedRva != sizeOfImage)
    {
        return Amd64LoadedImageLayoutError::InvalidImageGeometry;
    }
    if ((importSectionSize == 0U) || (importRva != importSectionRva) || (importSize != importSectionSize) ||
        !IsZeroRange(bytes, importSectionRva, importSectionSize) || (relocationRva != relocationSectionRva) ||
        (relocationSize == 0U) || (relocationSize > relocationSectionSize))
    {
        return Amd64LoadedImageLayoutError::InvalidDirectory;
    }
    if ((baseOfCode != textRva) || (entryRva < textRva) || (entryRva >= (textRva + textVirtualSize)) ||
        ((entryRva & 0x0FU) != 0U))
    {
        return Amd64LoadedImageLayoutError::InvalidEntrypoint;
    }

    output = layout;
    return Amd64LoadedImageLayoutError::None;
}
} // namespace Asteria::Platform::Amd64::Firmware
