// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt8 WriteBackMemoryAttributeBit{Foundation::UInt8{1U} << 0U};
constexpr Foundation::UInt8 UncachedMemoryAttributeBit{Foundation::UInt8{1U} << 4U};
constexpr Foundation::UInt8 ArchitectedMemoryAttributeMask{
    WriteBackMemoryAttributeBit | UncachedMemoryAttributeBit | (Foundation::UInt8{1U} << 5U) |
    (Foundation::UInt8{1U} << 6U) | (Foundation::UInt8{1U} << 7U)};
constexpr Foundation::UInt8 RequiredMemoryAttributeMask{WriteBackMemoryAttributeBit | UncachedMemoryAttributeBit};
constexpr Foundation::UInt64 PageSizeBelowFourKibibytesMask{(Foundation::UInt64{1U} << 12U) - 1U};

[[nodiscard]] Foundation::Result<PalVirtualMemoryCapabilities, PalVirtualMemoryDecodeError>
Fail(const PalVirtualMemoryDecodeError error) noexcept
{
    return Foundation::Result<PalVirtualMemoryCapabilities, PalVirtualMemoryDecodeError>::Failure(error);
}

[[nodiscard]] Foundation::UInt8 ExtractByte(const Foundation::UInt64 value, const Foundation::UInt8 shift) noexcept
{
    return static_cast<Foundation::UInt8>((value >> shift) & 0xFFU);
}
} // namespace

Foundation::Result<PalVirtualMemoryCapabilities, PalVirtualMemoryDecodeError>
PalVirtualMemoryDecoder::Decode(const PalStaticCallResult& summary, const PalStaticCallResult& pageSizes,
                                const PalStaticCallResult& memoryAttributes) noexcept
{
    if (summary.Status != 0)
    {
        return Fail(PalVirtualMemoryDecodeError::SummaryCallFailed);
    }
    if (pageSizes.Status != 0)
    {
        return Fail(PalVirtualMemoryDecodeError::PageSizeCallFailed);
    }
    if (memoryAttributes.Status != 0)
    {
        return Fail(PalVirtualMemoryDecodeError::MemoryAttributeCallFailed);
    }

    if (((summary.Result2 >> 32U) != 0U) || (summary.Result3 != 0U))
    {
        return Fail(PalVirtualMemoryDecodeError::SummaryReservedResult);
    }
    if (pageSizes.Result3 != 0U)
    {
        return Fail(PalVirtualMemoryDecodeError::PageSizeReservedResult);
    }
    if (((memoryAttributes.Result1 >> 8U) != 0U) || (memoryAttributes.Result2 != 0U) ||
        (memoryAttributes.Result3 != 0U))
    {
        return Fail(PalVirtualMemoryDecodeError::MemoryAttributeReservedResult);
    }

    const Foundation::UInt8 physicalAddressBits = static_cast<Foundation::UInt8>((summary.Result1 >> 1U) & 0x7FU);
    const Foundation::UInt8 protectionKeyBits = ExtractByte(summary.Result1, 8U);
    const Foundation::UInt16 protectionKeyRegisterCount =
        static_cast<Foundation::UInt16>(ExtractByte(summary.Result1, 16U)) + 1U;
    const Foundation::UInt8 hashTagIdentifier = ExtractByte(summary.Result1, 24U);
    const Foundation::UInt16 dataTranslationRegisterCount =
        static_cast<Foundation::UInt16>(ExtractByte(summary.Result1, 32U)) + 1U;
    const Foundation::UInt16 instructionTranslationRegisterCount =
        static_cast<Foundation::UInt16>(ExtractByte(summary.Result1, 40U)) + 1U;
    const Foundation::UInt8 uniqueTranslationCacheCount = ExtractByte(summary.Result1, 48U);
    const Foundation::UInt8 translationCacheLevelCount = ExtractByte(summary.Result1, 56U);
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit = ExtractByte(summary.Result2, 0U);
    const Foundation::UInt8 regionIdentifierBits = ExtractByte(summary.Result2, 8U);
    const Foundation::UInt16 maximumConcurrentPurgesEncoding =
        static_cast<Foundation::UInt16>((summary.Result2 >> 16U) & 0xFFFFU);

    if ((physicalAddressBits < 32U) || (physicalAddressBits > 63U) ||
        (implementedVirtualAddressMostSignificantBit < 50U) || (implementedVirtualAddressMostSignificantBit > 60U) ||
        (regionIdentifierBits < 18U) || (regionIdentifierBits > 24U))
    {
        return Fail(PalVirtualMemoryDecodeError::InvalidAddressCapabilities);
    }
    if ((protectionKeyBits < 18U) || (protectionKeyBits > 24U) || (protectionKeyBits < regionIdentifierBits) ||
        (protectionKeyRegisterCount < 16U))
    {
        return Fail(PalVirtualMemoryDecodeError::InvalidProtectionCapabilities);
    }
    if ((instructionTranslationRegisterCount < 8U) || (dataTranslationRegisterCount < 8U))
    {
        return Fail(PalVirtualMemoryDecodeError::InvalidTranslationRegisterCapabilities);
    }
    const Foundation::UInt16 maximumUniqueTranslationCaches =
        static_cast<Foundation::UInt16>(translationCacheLevelCount) * 2U;
    if ((translationCacheLevelCount == 0U) || (uniqueTranslationCacheCount == 0U) ||
        (static_cast<Foundation::UInt16>(uniqueTranslationCacheCount) > maximumUniqueTranslationCaches))
    {
        return Fail(PalVirtualMemoryDecodeError::InvalidTranslationCacheTopology);
    }

    if ((pageSizes.Result1 & ArchitectedMinimumInsertablePageSizeMask) != ArchitectedMinimumInsertablePageSizeMask)
    {
        return Fail(PalVirtualMemoryDecodeError::MissingMandatoryInsertablePageSize);
    }
    if ((pageSizes.Result2 & ArchitectedMinimumPurgePageSizeMask) != ArchitectedMinimumPurgePageSizeMask)
    {
        return Fail(PalVirtualMemoryDecodeError::MissingMandatoryPurgePageSize);
    }
    if (((pageSizes.Result1 | pageSizes.Result2) & PageSizeBelowFourKibibytesMask) != 0U)
    {
        return Fail(PalVirtualMemoryDecodeError::PageSizeBelowArchitectedMinimum);
    }
    if ((pageSizes.Result1 & ~pageSizes.Result2) != 0U)
    {
        return Fail(PalVirtualMemoryDecodeError::InsertablePageSizeNotPurgeable);
    }

    const Foundation::UInt8 memoryAttributeMask = static_cast<Foundation::UInt8>(memoryAttributes.Result1);
    if ((memoryAttributeMask & static_cast<Foundation::UInt8>(~ArchitectedMemoryAttributeMask)) != 0U)
    {
        return Fail(PalVirtualMemoryDecodeError::InvalidMemoryAttributeCapabilities);
    }
    if ((memoryAttributeMask & RequiredMemoryAttributeMask) != RequiredMemoryAttributeMask)
    {
        return Fail(PalVirtualMemoryDecodeError::MissingRequiredMemoryAttribute);
    }

    return Foundation::Result<PalVirtualMemoryCapabilities, PalVirtualMemoryDecodeError>::Success(
        PalVirtualMemoryCapabilities{
            .Translation =
                TranslationCapabilities{
                    .InsertablePageSizeMask = pageSizes.Result1,
                    .PhysicalAddressBits = physicalAddressBits,
                    .ImplementedVirtualAddressMostSignificantBit = implementedVirtualAddressMostSignificantBit,
                    .RegionIdentifierBits = regionIdentifierBits,
                    .ProtectionKeyBits = protectionKeyBits,
                    .MemoryAttributeMask = memoryAttributeMask,
                    .HardwareWalkerImplemented = (summary.Result1 & 1U) != 0U,
                    .Reserved16 = 0U,
                    .Reserved64 = 0U,
                },
            .PurgePageSizeMask = pageSizes.Result2,
            .InstructionTranslationRegisterCount = instructionTranslationRegisterCount,
            .DataTranslationRegisterCount = dataTranslationRegisterCount,
            .ProtectionKeyRegisterCount = protectionKeyRegisterCount,
            .MaximumConcurrentPurgesEncoding = maximumConcurrentPurgesEncoding,
            .HashTagIdentifier = hashTagIdentifier,
            .UniqueTranslationCacheCount = uniqueTranslationCacheCount,
            .TranslationCacheLevelCount = translationCacheLevelCount,
            .Reserved8 = 0U,
            .Reserved32 = 0U,
        });
}
} // namespace Asteria::Platform::Ia64::Memory
