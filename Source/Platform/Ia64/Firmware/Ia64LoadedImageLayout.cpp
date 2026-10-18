// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
constexpr Foundation::Size DosHeaderSize{64U};
constexpr Foundation::Size PeSignatureSize{4U};
constexpr Foundation::Size CoffHeaderSize{20U};
constexpr Foundation::Size OptionalHeaderSize{240U};
constexpr Foundation::Size SectionHeaderSize{40U};
constexpr Foundation::UInt16 Ia64Machine{0x0200U};
constexpr Foundation::UInt16 Pe32PlusMagic{0x020BU};
constexpr Foundation::UInt16 EfiApplicationSubsystem{10U};
constexpr Foundation::UInt16 ExpectedCoffCharacteristics{0x022EU};
constexpr Foundation::UInt16 ExpectedDllCharacteristics{0x0160U};
constexpr Foundation::UInt32 SectionAlignment{0x1000U};
constexpr Foundation::UInt32 FileAlignment{0x0200U};
constexpr Foundation::UInt32 ExpectedHeaderBytes{0x0400U};
constexpr Foundation::UInt32 CodeSectionCharacteristics{0x6000'0020U};
constexpr Foundation::UInt32 ReadOnlySectionCharacteristics{0x4000'0040U};
constexpr Foundation::UInt32 WritableDataSectionCharacteristics{0xC000'0040U};
constexpr Foundation::UInt32 BssSectionCharacteristics{0xC000'0080U};
constexpr Foundation::UInt32 MaximumUInt32{~Foundation::UInt32{0U}};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

struct ExpectedSection
{
    Foundation::UInt32 Characteristics;
    Ia64LoadedImageSectionPurpose Purpose;
    bool HasRawData;
};

[[nodiscard]] constexpr ExpectedSection Expected(const Foundation::Size index) noexcept
{
    if (index == 0U)
    {
        return ExpectedSection{CodeSectionCharacteristics, Ia64LoadedImageSectionPurpose::Code, true};
    }
    if (index < 4U)
    {
        return ExpectedSection{ReadOnlySectionCharacteristics, Ia64LoadedImageSectionPurpose::ReadOnlyData, true};
    }
    if (index == 4U)
    {
        return ExpectedSection{WritableDataSectionCharacteristics, Ia64LoadedImageSectionPurpose::WritableData, true};
    }
    return ExpectedSection{BssSectionCharacteristics, Ia64LoadedImageSectionPurpose::WritableData, false};
}

[[nodiscard]] Foundation::Result<Ia64LoadedImageLayout, Ia64LoadedImageLayoutError>
Fail(const Ia64LoadedImageLayoutError error) noexcept
{
    return Foundation::Result<Ia64LoadedImageLayout, Ia64LoadedImageLayoutError>::Failure(error);
}

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

[[nodiscard]] bool HasExpectedName(const Foundation::UInt8* const section, const Foundation::Size index) noexcept
{
    Foundation::UInt64 name{0U};
    for (Foundation::Size byte{0U}; byte < 8U; ++byte)
    {
        name |= static_cast<Foundation::UInt64>(section[byte]) << (byte * 8U);
    }
    // Immediate comparisons keep even the O0 pre-copy parser independent of
    // image-global storage. These are little-endian PE section names.
    return ((index == 0U) && (name == 0x0000'0074'7865'742EU)) || ((index == 1U) && (name == 0x0061'7461'646F'722EU)) ||
           ((index == 2U) && (name == 0x0000'0000'6470'6F2EU)) || ((index == 3U) && (name == 0x0000'636F'6C65'722EU)) ||
           ((index == 4U) && (name == 0x0000'0061'7461'642EU)) || ((index == 5U) && (name == 0x0000'0000'7373'622EU));
}

[[nodiscard]] bool TryAlignUp(const Foundation::UInt32 value, Foundation::UInt32& aligned) noexcept
{
    constexpr Foundation::UInt32 Mask{SectionAlignment - 1U};
    if (value > (MaximumUInt32 - Mask))
    {
        return false;
    }
    aligned = (value + Mask) & ~Mask;
    return aligned != 0U;
}
} // namespace

Foundation::Result<Ia64LoadedImageLayout, Ia64LoadedImageLayoutError>
Ia64LoadedImageLayoutParser::Parse(const void* const imageBase, const Foundation::Size imageSize,
                                   const Ia64LoadedImagePlacement placement) noexcept
{
    if (imageBase == nullptr)
    {
        return Fail(Ia64LoadedImageLayoutError::NullImage);
    }
    const Foundation::UInt64 baseValue =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(imageBase));
    if ((imageSize < ExpectedHeaderBytes) || ((imageSize & (SectionAlignment - 1U)) != 0U))
    {
        return Fail(Ia64LoadedImageLayoutError::ImageSizeOutOfRange);
    }
    if (((baseValue & (Ia64LoadedImageBaseAlignmentBytes - 1U)) != 0U) || (baseValue > (MaximumUInt64 - imageSize)))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidImageGeometry);
    }

    const auto* const bytes = static_cast<const Foundation::UInt8*>(imageBase);
    if (!CanRead(0U, DosHeaderSize, imageSize) || (bytes[0U] != static_cast<Foundation::UInt8>('M')) ||
        (bytes[1U] != static_cast<Foundation::UInt8>('Z')))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidDosHeader);
    }

    const Foundation::Size peOffset = ReadUInt32(bytes, 60U);
    constexpr Foundation::Size PeAndCoffBytes{PeSignatureSize + CoffHeaderSize};
    if ((peOffset < DosHeaderSize) || ((peOffset & 3U) != 0U) || !CanRead(peOffset, PeAndCoffBytes, imageSize) ||
        (bytes[peOffset] != static_cast<Foundation::UInt8>('P')) ||
        (bytes[peOffset + 1U] != static_cast<Foundation::UInt8>('E')) || (bytes[peOffset + 2U] != 0U) ||
        (bytes[peOffset + 3U] != 0U))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidPeHeader);
    }

    const Foundation::Size coffOffset = peOffset + PeSignatureSize;
    if ((ReadUInt16(bytes, coffOffset) != Ia64Machine) ||
        (ReadUInt16(bytes, coffOffset + 2U) != Ia64LoadedImageSectionCount) ||
        (ReadUInt16(bytes, coffOffset + 16U) != OptionalHeaderSize) ||
        (ReadUInt16(bytes, coffOffset + 18U) != ExpectedCoffCharacteristics))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidCoffHeader);
    }

    const Foundation::Size optionalOffset = coffOffset + CoffHeaderSize;
    if (!CanRead(optionalOffset, OptionalHeaderSize, imageSize) ||
        (ReadUInt16(bytes, optionalOffset) != Pe32PlusMagic) ||
        (ReadUInt32(bytes, optionalOffset + 32U) != SectionAlignment) ||
        (ReadUInt32(bytes, optionalOffset + 36U) != FileAlignment) ||
        (ReadUInt32(bytes, optionalOffset + 60U) != ExpectedHeaderBytes) ||
        (ReadUInt16(bytes, optionalOffset + 68U) != EfiApplicationSubsystem) ||
        (ReadUInt16(bytes, optionalOffset + 70U) != ExpectedDllCharacteristics) ||
        (ReadUInt32(bytes, optionalOffset + 104U) != 0U) || (ReadUInt32(bytes, optionalOffset + 108U) != 16U))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidOptionalHeader);
    }

    const Foundation::UInt32 entryRva = ReadUInt32(bytes, optionalOffset + 16U);
    const Foundation::UInt32 baseOfCode = ReadUInt32(bytes, optionalOffset + 20U);
    const Foundation::UInt32 sizeOfImage = ReadUInt32(bytes, optionalOffset + 56U);
    if ((sizeOfImage != imageSize) || (sizeOfImage < (ExpectedHeaderBytes + SectionAlignment)))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidImageGeometry);
    }

    const Foundation::Size sectionTableOffset = optionalOffset + OptionalHeaderSize;
    constexpr Foundation::Size SectionTableBytes{Ia64LoadedImageSectionCount * SectionHeaderSize};
    if (!CanRead(sectionTableOffset, SectionTableBytes, ExpectedHeaderBytes) ||
        !CanRead(sectionTableOffset, SectionTableBytes, imageSize))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidSectionTable);
    }

    if ((placement != Ia64LoadedImagePlacement::Firmware) && (placement != Ia64LoadedImagePlacement::AlignedCopy))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidImageGeometry);
    }
    if ((placement == Ia64LoadedImagePlacement::AlignedCopy) &&
        ((baseValue & (Ia64RelocatedImageAlignmentBytes - 1U)) != 0U))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidImageGeometry);
    }
    Ia64LoadedImageLayout layout{
        .ImageBase = baseValue,
        .ImageSize = imageSize,
        .Placement = placement,
        .Sections = {},
    };
    Foundation::UInt32 expectedRva{0U};
    Foundation::UInt32 codeRva{0U};
    Foundation::UInt32 opdRva{0U};
    Foundation::UInt32 opdVirtualSize{0U};
    for (Foundation::Size index{0U}; index < Ia64LoadedImageSectionCount; ++index)
    {
        const Foundation::Size sectionOffset = sectionTableOffset + (index * SectionHeaderSize);
        const Foundation::UInt8* const section = bytes + sectionOffset;
        const ExpectedSection expected = Expected(index);
        if (!HasExpectedName(section, index))
        {
            return Fail(Ia64LoadedImageLayoutError::InvalidSectionName);
        }

        const Foundation::UInt32 virtualSize = ReadUInt32(section, 8U);
        const Foundation::UInt32 virtualAddress = ReadUInt32(section, 12U);
        const Foundation::UInt32 rawSize = ReadUInt32(section, 16U);
        const Foundation::UInt32 characteristics = ReadUInt32(section, 36U);
        if (characteristics != expected.Characteristics)
        {
            return Fail(Ia64LoadedImageLayoutError::InvalidSectionProtection);
        }
        const bool hasExpectedVirtualAddress =
            index == 0U ? virtualAddress >= SectionAlignment : virtualAddress == expectedRva;
        if ((virtualSize == 0U) || !hasExpectedVirtualAddress || ((virtualAddress & (SectionAlignment - 1U)) != 0U) ||
            (expected.HasRawData &&
             ((rawSize == 0U) || ((rawSize & (FileAlignment - 1U)) != 0U) || (rawSize < virtualSize))) ||
            (!expected.HasRawData && (rawSize != 0U)))
        {
            return Fail(Ia64LoadedImageLayoutError::InvalidSectionGeometry);
        }

        const Foundation::UInt32 loadedBytes = virtualSize > rawSize ? virtualSize : rawSize;
        Foundation::UInt32 alignedBytes{0U};
        if (!TryAlignUp(loadedBytes, alignedBytes) || (virtualAddress > (MaximumUInt32 - alignedBytes)) ||
            ((virtualAddress + alignedBytes) > sizeOfImage) || (baseValue > (MaximumUInt64 - virtualAddress)))
        {
            return Fail(Ia64LoadedImageLayoutError::InvalidSectionGeometry);
        }
        layout.Sections[index] = Ia64LoadedImageSection{
            .VirtualStart = baseValue + virtualAddress,
            .PageCount = alignedBytes / SectionAlignment,
            .Purpose = expected.Purpose,
            .Reserved = 0U,
        };
        expectedRva = virtualAddress + alignedBytes;
        if (index == 0U)
        {
            codeRva = virtualAddress;
        }
        if (index == 2U)
        {
            opdRva = virtualAddress;
            opdVirtualSize = virtualSize;
        }
    }

    if ((expectedRva != sizeOfImage) || (baseOfCode != codeRva) || (entryRva != opdRva) || (opdVirtualSize < 16U) ||
        ((entryRva & 0x0FU) != 0U))
    {
        return Fail(Ia64LoadedImageLayoutError::InvalidEntrypoint);
    }

    return Foundation::Result<Ia64LoadedImageLayout, Ia64LoadedImageLayoutError>::Success(layout);
}
} // namespace Asteria::Platform::Ia64::Firmware
