// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PagePermission;

constexpr Foundation::UInt64 PageSizeBytes{Foundation::UInt64{1U} << Ia64RuntimeTranslationPageShift};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 UserPrivilegeLevel{Foundation::UInt64{3U} << 7U};
constexpr Foundation::UInt64 ReadOnlyAccessRights{Foundation::UInt64{0U} << 9U};
constexpr Foundation::UInt64 ReadExecuteAccessRights{Foundation::UInt64{1U} << 9U};
constexpr Foundation::UInt64 ReadWriteAccessRights{Foundation::UInt64{2U} << 9U};
constexpr Foundation::UInt64 ExceptionDeferralBit{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt8 WriteBackMemoryAttribute{0U};
constexpr Foundation::UInt8 UncachedMemoryAttribute{4U};
constexpr Foundation::UInt8 WriteCombiningMemoryAttribute{6U};
constexpr Foundation::UInt64 FourKibibytePageSizeBit{Foundation::UInt64{1U} << Ia64RuntimeTranslationPageShift};
constexpr Foundation::UInt64 BelowMinimumPageSizeMask{FourKibibytePageSizeBit - 1U};
constexpr Foundation::UInt8 ArchitectedMemoryAttributeMask{
    (Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) | (Foundation::UInt8{1U} << 5U) |
    (Foundation::UInt8{1U} << 6U) | (Foundation::UInt8{1U} << 7U)};

[[nodiscard]] Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationEncodingError>
Fail(const Ia64RuntimeTranslationEncodingError error) noexcept
{
    return Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationEncodingError>::Failure(error);
}

[[nodiscard]] bool ReservedBytesAreZero(const AddressSpaceMappingRecord& mapping) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(mapping.Reserved); ++index)
    {
        if (mapping.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsKnownFrameKind(const FrameKind kind) noexcept
{
    switch (kind)
    {
    case FrameKind::Ram:
    case FrameKind::Device:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsCapabilitySetValid(const TranslationCapabilities& capabilities) noexcept
{
    return (capabilities.PhysicalAddressBits >= 32U) && (capabilities.PhysicalAddressBits <= 63U) &&
           (capabilities.ImplementedVirtualAddressMostSignificantBit >= 50U) &&
           (capabilities.ImplementedVirtualAddressMostSignificantBit <= 60U) &&
           ((capabilities.InsertablePageSizeMask & BelowMinimumPageSizeMask) == 0U) &&
           ((capabilities.MemoryAttributeMask & static_cast<Foundation::UInt8>(~ArchitectedMemoryAttributeMask)) ==
            0U) &&
           (capabilities.Reserved16 == 0U) && (capabilities.Reserved64 == 0U);
}

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == 60U)
    {
        return true;
    }

    const Foundation::UInt8 unimplementedBitCount = static_cast<Foundation::UInt8>(60U - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] constexpr bool IsPhysicalPageEncodable(const Foundation::UInt64 address,
                                                     const Foundation::UInt8 physicalAddressBits) noexcept
{
    const Foundation::UInt8 encodableAddressBits =
        physicalAddressBits < 50U ? physicalAddressBits : Foundation::UInt8{50U};
    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << encodableAddressBits;
    return ((address & PageOffsetMask) == 0U) && ((address & ~PhysicalPageNumberMask) == 0U) &&
           (address <= (physicalLimit - PageSizeBytes));
}

[[nodiscard]] constexpr bool TryGetMemoryAttribute(const MemoryCachePolicy policy,
                                                   Foundation::UInt8& memoryAttribute) noexcept
{
    switch (policy)
    {
    case MemoryCachePolicy::WriteBack:
        memoryAttribute = WriteBackMemoryAttribute;
        return true;
    case MemoryCachePolicy::Uncached:
        memoryAttribute = UncachedMemoryAttribute;
        return true;
    case MemoryCachePolicy::WriteCombining:
        memoryAttribute = WriteCombiningMemoryAttribute;
        return true;
    case MemoryCachePolicy::WriteThrough:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr bool TryGetAccessRights(const AddressSpaceMappingRecord& mapping,
                                                Foundation::UInt64& accessRights, bool& writable) noexcept
{
    if (!mapping.Permissions.HasOnlyKnownBits() || !mapping.Permissions.Contains(PagePermission::Read))
    {
        return false;
    }

    const bool write = mapping.Permissions.Contains(PagePermission::Write);
    const bool execute = mapping.Permissions.Contains(PagePermission::Execute);
    if (write && execute)
    {
        return false;
    }

    writable = write;
    if (write)
    {
        accessRights = ReadWriteAccessRights;
    }
    else if (execute)
    {
        accessRights = ReadExecuteAccessRights;
    }
    else
    {
        accessRights = ReadOnlyAccessRights;
    }
    return true;
}
} // namespace

Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationEncodingError>
Ia64RuntimeTranslationEncoder::EncodePage(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                          const Foundation::UInt64 pageIndex,
                                          const TranslationCapabilities& capabilities) noexcept
{
    const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
    const Foundation::UInt64 physicalStart = mapping.PhysicalStart.GetValue();
    const Foundation::UInt64 pageCount = mapping.Pages.GetValue();
    if (!mapping.RuntimeMapping || ((virtualStart & PageOffsetMask) != 0U) ||
        ((physicalStart & PageOffsetMask) != 0U) || (pageCount == 0U) || !IsKnownFrameKind(mapping.Kind) ||
        !ReservedBytesAreZero(mapping))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::InvalidMappingPlan);
    }
    if (pageCount > MaximumIa64RuntimeMappingPages)
    {
        return Fail(Ia64RuntimeTranslationEncodingError::MappingTooLarge);
    }
    if (pageIndex >= pageCount)
    {
        return Fail(Ia64RuntimeTranslationEncodingError::PageIndexOutOfBounds);
    }

    const Foundation::UInt64 sizeBytes = pageCount << Ia64RuntimeTranslationPageShift;
    const Foundation::UInt64 inclusiveSpanBytes = sizeBytes - 1U;
    if ((virtualStart > (MaximumUInt64 - inclusiveSpanBytes)) || (physicalStart > (MaximumUInt64 - inclusiveSpanBytes)))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::InvalidMappingPlan);
    }
    const Foundation::UInt64 virtualLast = virtualStart + inclusiveSpanBytes;
    if ((virtualStart >> 61U) != (virtualLast >> 61U))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::CrossRegionMapping);
    }
    if ((virtualStart >> 61U) == 0U)
    {
        return Fail(Ia64RuntimeTranslationEncodingError::RegionZeroReserved);
    }

    if (!IsCapabilitySetValid(capabilities))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::InvalidCapabilities);
    }
    if ((capabilities.InsertablePageSizeMask & FourKibibytePageSizeBit) == 0U)
    {
        return Fail(Ia64RuntimeTranslationEncodingError::UnsupportedPageSize);
    }
    if (!IsVirtualAddressImplemented(virtualStart, capabilities.ImplementedVirtualAddressMostSignificantBit) ||
        !IsVirtualAddressImplemented(virtualLast, capabilities.ImplementedVirtualAddressMostSignificantBit))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::UnimplementedVirtualAddress);
    }

    const Foundation::UInt64 physicalLastPage = physicalStart + (sizeBytes - PageSizeBytes);
    if (!IsPhysicalPageEncodable(physicalStart, capabilities.PhysicalAddressBits) ||
        !IsPhysicalPageEncodable(physicalLastPage, capabilities.PhysicalAddressBits))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange);
    }

    const Foundation::UInt64 pageOffset = pageIndex << Ia64RuntimeTranslationPageShift;
    const Foundation::UInt64 virtualAddress = virtualStart + pageOffset;
    const Foundation::UInt64 physicalAddress = physicalStart + pageOffset;
    if (!IsPhysicalPageEncodable(physicalAddress, capabilities.PhysicalAddressBits))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange);
    }

    Foundation::UInt8 memoryAttribute{0U};
    if (!TryGetMemoryAttribute(mapping.CachePolicy, memoryAttribute))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::UnsupportedCachePolicy);
    }
    if ((capabilities.MemoryAttributeMask & static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << memoryAttribute)) ==
        0U)
    {
        return Fail(Ia64RuntimeTranslationEncodingError::UnsupportedMemoryAttribute);
    }

    Foundation::UInt64 accessRights{0U};
    bool writable{false};
    if (!TryGetAccessRights(mapping, accessRights, writable))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::UnsupportedPermissions);
    }
    if ((mapping.Kind == FrameKind::Device) && mapping.Permissions.Contains(PagePermission::Execute))
    {
        return Fail(Ia64RuntimeTranslationEncodingError::DeviceExecutionDenied);
    }

    Foundation::UInt64 translationWord = physicalAddress | PresentBit |
                                         (static_cast<Foundation::UInt64>(memoryAttribute) << 2U) | AccessedBit |
                                         accessRights | ExceptionDeferralBit;
    if (writable)
    {
        translationWord |= DirtyBit;
    }
    if (mapping.Permissions.Contains(PagePermission::User))
    {
        translationWord |= UserPrivilegeLevel;
    }

    return Foundation::Result<EncodedIa64RuntimeTranslation, Ia64RuntimeTranslationEncodingError>::Success(
        EncodedIa64RuntimeTranslation{
            .TranslationWord = translationWord,
            .TranslationInsertion = Foundation::UInt64{Ia64RuntimeTranslationPageShift} << 2U,
            .FaultingAddress = virtualAddress,
        });
}
} // namespace Asteria::Platform::Ia64::Memory
