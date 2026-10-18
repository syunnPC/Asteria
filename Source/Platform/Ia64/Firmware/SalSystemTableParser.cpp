// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
constexpr Foundation::UInt8 EntrypointDescriptorType{0U};
constexpr Foundation::UInt8 MemoryDescriptorType{1U};
constexpr Foundation::UInt8 PlatformFeaturesDescriptorType{2U};
constexpr Foundation::UInt8 TranslationRegisterDescriptorType{3U};
constexpr Foundation::UInt8 PurgeCoherenceDescriptorType{4U};
constexpr Foundation::UInt8 ApWakeDescriptorType{5U};
constexpr Foundation::UInt8 MaximumDescriptorType{ApWakeDescriptorType};

constexpr Foundation::Size EntrypointDescriptorSize{48U};
constexpr Foundation::Size MemoryDescriptorSize{32U};
constexpr Foundation::Size PlatformFeaturesDescriptorSize{16U};
constexpr Foundation::Size TranslationRegisterDescriptorSize{32U};
constexpr Foundation::Size PurgeCoherenceDescriptorSize{16U};
constexpr Foundation::Size ApWakeDescriptorSize{16U};
constexpr Foundation::Size MinimumDescriptorSize{16U};
constexpr Foundation::Size MaximumDescriptorSize{48U};
constexpr Foundation::UInt64 PhysicalAddressTopBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 VirtualRegionOffsetMask{(Foundation::UInt64{1U} << 61U) - 1U};

struct EntrypointDescriptorValues
{
    Foundation::UInt64 PalProcedure;
    Foundation::UInt64 SalProcedure;
    Foundation::UInt64 SalGlobalData;
};

struct TranslationRegisterDescriptorValues
{
    Foundation::UInt64 VirtualAddress;
    Foundation::UInt64 EncodedPageSize;
};

struct PurgeCoherenceDescriptorValues
{
    Foundation::UInt64 InformationAddress;
    Foundation::UInt32 DomainCount;
};

[[nodiscard]] Foundation::Result<SalSystemTableInformation, SalSystemTableParseError>
Fail(const SalSystemTableParseError error) noexcept
{
    return Foundation::Result<SalSystemTableInformation, SalSystemTableParseError>::Failure(error);
}

[[nodiscard]] Foundation::UInt16 ReadLittleEndian16(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt16>(bytes[0]) |
           static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[1]) << 8U);
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

[[nodiscard]] bool AreBytesZero(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsBcdByte(const Foundation::UInt8 value) noexcept
{
    return ((value & 0x0FU) <= 9U) && ((value >> 4U) <= 9U);
}

[[nodiscard]] bool IsSupportedRevision(const Foundation::UInt8 major, const Foundation::UInt8 minor) noexcept
{
    return ((major == 0x02U) && (minor >= 0x80U)) || (major == 0x03U);
}

[[nodiscard]] Foundation::Size GetDescriptorSize(const Foundation::UInt8 type) noexcept
{
    switch (type)
    {
    case EntrypointDescriptorType:
        return EntrypointDescriptorSize;
    case MemoryDescriptorType:
        return MemoryDescriptorSize;
    case PlatformFeaturesDescriptorType:
        return PlatformFeaturesDescriptorSize;
    case TranslationRegisterDescriptorType:
        return TranslationRegisterDescriptorSize;
    case PurgeCoherenceDescriptorType:
        return PurgeCoherenceDescriptorSize;
    case ApWakeDescriptorType:
        return ApWakeDescriptorSize;
    default:
        return 0U;
    }
}

[[nodiscard]] bool IsPhysicalAddress(const Foundation::UInt64 address, const Foundation::UInt64 alignment) noexcept
{
    return (address != 0U) && ((address & PhysicalAddressTopBit) == 0U) && ((address & (alignment - 1U)) == 0U);
}

[[nodiscard]] bool IsPhysicalAddressValue(const Foundation::UInt64 address) noexcept
{
    return (address != 0U) && ((address & PhysicalAddressTopBit) == 0U);
}

[[nodiscard]] bool TryReadEntrypointDescriptor(const Foundation::UInt8* const descriptor,
                                               EntrypointDescriptorValues& values) noexcept
{
    values = EntrypointDescriptorValues{
        .PalProcedure = ReadLittleEndian64(descriptor + 8U),
        .SalProcedure = ReadLittleEndian64(descriptor + 16U),
        .SalGlobalData = ReadLittleEndian64(descriptor + 24U),
    };
    return AreBytesZero(descriptor + 1U, 7U) && IsPhysicalAddress(values.PalProcedure, 16U) &&
           IsPhysicalAddress(values.SalProcedure, 16U) && IsPhysicalAddressValue(values.SalGlobalData) &&
           AreBytesZero(descriptor + 32U, 16U);
}

[[nodiscard]] bool IsPlatformFeaturesDescriptorValid(const Foundation::UInt8* const descriptor,
                                                     const Foundation::UInt8 features,
                                                     const Foundation::UInt8 revisionMajor,
                                                     const Foundation::UInt8 revisionMinor) noexcept
{
    const bool supportsItcDriftFeature = (revisionMajor == 0x03U) && (revisionMinor >= 0x30U);
    const Foundation::UInt8 allowedFeatures =
        supportsItcDriftFeature ? Foundation::UInt8{0x0FU} : Foundation::UInt8{0x07U};
    return (features & static_cast<Foundation::UInt8>(~allowedFeatures)) == 0U && AreBytesZero(descriptor + 2U, 14U);
}

[[nodiscard]] bool TryReadTranslationRegisterDescriptor(const Foundation::UInt8* const descriptor,
                                                        TranslationRegisterDescriptorValues& values) noexcept
{
    if ((descriptor[1] != 0U) || (descriptor[2] != 0U) || !AreBytesZero(descriptor + 3U, 5U) ||
        !AreBytesZero(descriptor + 24U, 8U))
    {
        return false;
    }

    values = TranslationRegisterDescriptorValues{
        .VirtualAddress = ReadLittleEndian64(descriptor + 8U),
        .EncodedPageSize = ReadLittleEndian64(descriptor + 16U),
    };
    if ((values.EncodedPageSize & ~Foundation::UInt64{0xFCU}) != 0U)
    {
        return false;
    }

    const Foundation::UInt8 pageShift = static_cast<Foundation::UInt8>((values.EncodedPageSize >> 2U) & 0x3FU);
    if (pageShift < 12U)
    {
        return false;
    }

    const Foundation::UInt64 pageOffsetMask =
        pageShift >= 61U ? VirtualRegionOffsetMask : (Foundation::UInt64{1U} << pageShift) - 1U;
    return (values.VirtualAddress & pageOffsetMask) == 0U;
}

[[nodiscard]] bool TryReadPurgeCoherenceDescriptor(const Foundation::UInt8* const descriptor,
                                                   PurgeCoherenceDescriptorValues& values) noexcept
{
    values = PurgeCoherenceDescriptorValues{
        .InformationAddress = ReadLittleEndian64(descriptor + 8U),
        .DomainCount = ReadLittleEndian32(descriptor + 4U),
    };
    return AreBytesZero(descriptor + 1U, 3U) && (values.DomainCount != 0U) &&
           IsPhysicalAddress(values.InformationAddress, 8U);
}

[[nodiscard]] bool TryReadApWakeDescriptor(const Foundation::UInt8* const descriptor,
                                           Foundation::UInt64& vector) noexcept
{
    vector = ReadLittleEndian64(descriptor + 8U);
    return (descriptor[1] == 0U) && AreBytesZero(descriptor + 2U, 6U) && (vector >= 0x10U) && (vector <= 0xFFU);
}
} // namespace

Foundation::Result<SalSystemTableInformation, SalSystemTableParseError>
SalSystemTableParser::Parse(const Foundation::UInt8* const bytes, const Foundation::Size availableBytes) noexcept
{
    if (bytes == nullptr)
    {
        return Fail(SalSystemTableParseError::NullBuffer);
    }
    if (availableBytes < SalSystemTableHeaderSize)
    {
        return Fail(SalSystemTableParseError::HeaderTooShort);
    }
    if ((bytes[0] != static_cast<Foundation::UInt8>('S')) || (bytes[1] != static_cast<Foundation::UInt8>('S')) ||
        (bytes[2] != static_cast<Foundation::UInt8>('T')) || (bytes[3] != static_cast<Foundation::UInt8>('_')))
    {
        return Fail(SalSystemTableParseError::InvalidSignature);
    }

    const Foundation::UInt32 totalTableLength = ReadLittleEndian32(bytes + 4U);
    const Foundation::UInt16 entryCount = ReadLittleEndian16(bytes + 10U);
    const Foundation::UInt64 minimumTableLength = static_cast<Foundation::UInt64>(SalSystemTableHeaderSize) +
                                                  (static_cast<Foundation::UInt64>(entryCount) * MinimumDescriptorSize);
    const Foundation::UInt64 maximumTableLength = static_cast<Foundation::UInt64>(SalSystemTableHeaderSize) +
                                                  (static_cast<Foundation::UInt64>(entryCount) * MaximumDescriptorSize);
    if ((static_cast<Foundation::UInt64>(totalTableLength) < minimumTableLength) ||
        (static_cast<Foundation::UInt64>(totalTableLength) > maximumTableLength))
    {
        return Fail(SalSystemTableParseError::InvalidTableLength);
    }
    if (static_cast<Foundation::UInt64>(totalTableLength) > static_cast<Foundation::UInt64>(availableBytes))
    {
        return Fail(SalSystemTableParseError::TruncatedTable);
    }

    const Foundation::UInt16 salRevision = ReadLittleEndian16(bytes + 8U);
    const Foundation::UInt16 salAVersion = ReadLittleEndian16(bytes + 20U);
    const Foundation::UInt16 salBVersion = ReadLittleEndian16(bytes + 22U);
    const Foundation::UInt8 revisionMinor = static_cast<Foundation::UInt8>(salRevision & 0xFFU);
    const Foundation::UInt8 revisionMajor = static_cast<Foundation::UInt8>(salRevision >> 8U);
    if (!IsBcdByte(revisionMinor) || !IsBcdByte(revisionMajor) ||
        !IsBcdByte(static_cast<Foundation::UInt8>(salAVersion & 0xFFU)) ||
        !IsBcdByte(static_cast<Foundation::UInt8>(salAVersion >> 8U)) ||
        !IsBcdByte(static_cast<Foundation::UInt8>(salBVersion & 0xFFU)) ||
        !IsBcdByte(static_cast<Foundation::UInt8>(salBVersion >> 8U)))
    {
        return Fail(SalSystemTableParseError::InvalidRevisionEncoding);
    }
    if (!IsSupportedRevision(revisionMajor, revisionMinor))
    {
        return Fail(SalSystemTableParseError::UnsupportedRevision);
    }
    if (!AreBytesZero(bytes + 13U, 7U) || !AreBytesZero(bytes + 88U, 8U))
    {
        return Fail(SalSystemTableParseError::InvalidHeaderReservedField);
    }

    Foundation::UInt8 checksum{0U};
    for (Foundation::UInt32 index{0U}; index < totalTableLength; ++index)
    {
        checksum = static_cast<Foundation::UInt8>(checksum + bytes[index]);
    }
    if (checksum != 0U)
    {
        return Fail(SalSystemTableParseError::InvalidChecksum);
    }

    SalSystemTableInformation information{
        .PalProcedurePhysicalAddress = 0U,
        .SalProcedurePhysicalAddress = 0U,
        .SalGlobalDataPhysicalAddress = 0U,
        .TranslationRegisterVirtualAddress = 0U,
        .TranslationRegisterEncodedPageSize = 0U,
        .CoherenceDomainInformationPhysicalAddress = 0U,
        .ApWakeVector = 0U,
        .TotalTableLength = totalTableLength,
        .CoherenceDomainCount = 0U,
        .SalRevision = salRevision,
        .EntryCount = entryCount,
        .SalAVersion = salAVersion,
        .SalBVersion = salBVersion,
        .MemoryDescriptorCount = 0U,
        .PlatformFeatures = 0U,
        .HasPlatformFeatures = false,
        .HasTranslationRegister = false,
        .HasPurgeCoherenceInformation = false,
        .HasApWakeVector = false,
        .Reserved8 = 0U,
    };
    bool hasEntrypoint{false};
    Foundation::UInt8 previousType{0U};
    bool hasPreviousType{false};
    Foundation::Size offset{SalSystemTableHeaderSize};

    for (Foundation::UInt16 entryIndex{0U}; entryIndex < entryCount; ++entryIndex)
    {
        if (offset >= totalTableLength)
        {
            return Fail(SalSystemTableParseError::DescriptorTruncated);
        }
        const Foundation::UInt8 type = bytes[offset];
        if (type > MaximumDescriptorType)
        {
            return Fail(SalSystemTableParseError::UnknownDescriptorType);
        }
        if (hasPreviousType && (type < previousType))
        {
            return Fail(SalSystemTableParseError::InvalidDescriptorOrder);
        }

        const Foundation::Size descriptorSize = GetDescriptorSize(type);
        if ((descriptorSize > totalTableLength) ||
            (offset > (static_cast<Foundation::Size>(totalTableLength) - descriptorSize)))
        {
            return Fail(SalSystemTableParseError::DescriptorTruncated);
        }
        const Foundation::UInt8* const descriptor = bytes + offset;

        switch (type)
        {
        case EntrypointDescriptorType:
        {
            if (hasEntrypoint)
            {
                return Fail(SalSystemTableParseError::DuplicateDescriptor);
            }
            EntrypointDescriptorValues values{};
            if (!TryReadEntrypointDescriptor(descriptor, values))
            {
                return Fail(SalSystemTableParseError::InvalidEntrypointDescriptor);
            }
            information.PalProcedurePhysicalAddress = values.PalProcedure;
            information.SalProcedurePhysicalAddress = values.SalProcedure;
            information.SalGlobalDataPhysicalAddress = values.SalGlobalData;
            hasEntrypoint = true;
            break;
        }
        case MemoryDescriptorType:
            ++information.MemoryDescriptorCount;
            break;
        case PlatformFeaturesDescriptorType:
        {
            if (information.HasPlatformFeatures)
            {
                return Fail(SalSystemTableParseError::DuplicateDescriptor);
            }
            const Foundation::UInt8 features = descriptor[1];
            if (!IsPlatformFeaturesDescriptorValid(descriptor, features, revisionMajor, revisionMinor))
            {
                return Fail(SalSystemTableParseError::InvalidPlatformFeaturesDescriptor);
            }
            information.PlatformFeatures = features;
            information.HasPlatformFeatures = true;
            break;
        }
        case TranslationRegisterDescriptorType:
        {
            if (information.HasTranslationRegister)
            {
                return Fail(SalSystemTableParseError::DuplicateDescriptor);
            }
            TranslationRegisterDescriptorValues values{};
            if (!TryReadTranslationRegisterDescriptor(descriptor, values))
            {
                return Fail(SalSystemTableParseError::InvalidTranslationRegisterDescriptor);
            }
            information.TranslationRegisterVirtualAddress = values.VirtualAddress;
            information.TranslationRegisterEncodedPageSize = values.EncodedPageSize;
            information.HasTranslationRegister = true;
            break;
        }
        case PurgeCoherenceDescriptorType:
        {
            if (information.HasPurgeCoherenceInformation)
            {
                return Fail(SalSystemTableParseError::DuplicateDescriptor);
            }
            PurgeCoherenceDescriptorValues values{};
            if (!TryReadPurgeCoherenceDescriptor(descriptor, values))
            {
                return Fail(SalSystemTableParseError::InvalidPurgeCoherenceDescriptor);
            }
            information.CoherenceDomainCount = values.DomainCount;
            information.CoherenceDomainInformationPhysicalAddress = values.InformationAddress;
            information.HasPurgeCoherenceInformation = true;
            break;
        }
        case ApWakeDescriptorType:
        {
            if (information.HasApWakeVector)
            {
                return Fail(SalSystemTableParseError::DuplicateDescriptor);
            }
            Foundation::UInt64 vector{0U};
            if (!TryReadApWakeDescriptor(descriptor, vector))
            {
                return Fail(SalSystemTableParseError::InvalidApWakeDescriptor);
            }
            information.ApWakeVector = vector;
            information.HasApWakeVector = true;
            break;
        }
        default:
            return Fail(SalSystemTableParseError::UnknownDescriptorType);
        }

        previousType = type;
        hasPreviousType = true;
        offset += descriptorSize;
    }

    if (offset != totalTableLength)
    {
        return Fail(SalSystemTableParseError::TableLengthMismatch);
    }
    if (!hasEntrypoint)
    {
        return Fail(SalSystemTableParseError::MissingEntrypointDescriptor);
    }

    return Foundation::Result<SalSystemTableInformation, SalSystemTableParseError>::Success(information);
}
} // namespace Asteria::Platform::Ia64::Firmware
