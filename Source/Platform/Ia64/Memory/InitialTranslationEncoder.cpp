// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialTranslationEncoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using Kernel::Memory::InitialMapping;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MemoryCachePolicy;

constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 ExceptionDeferralBit{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 ReadOnlyAccessRights{Foundation::UInt64{0U} << 9U};
constexpr Foundation::UInt64 ReadExecuteAccessRights{Foundation::UInt64{1U} << 9U};
constexpr Foundation::UInt64 ReadWriteAccessRights{Foundation::UInt64{2U} << 9U};
constexpr Foundation::UInt8 WriteBackMemoryAttribute{0U};
constexpr Foundation::UInt8 UncachedMemoryAttribute{4U};
constexpr Foundation::UInt8 WriteCombiningMemoryAttribute{6U};
constexpr Foundation::UInt64 PreferredPageSizeBit{Foundation::UInt64{1U} << InitialTranslationPageShift};
constexpr Foundation::UInt64 BelowMinimumPageSizeMask{PreferredPageSizeBit - 1U};
constexpr Foundation::UInt8 ArchitectedMemoryAttributeMask{
    (Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) | (Foundation::UInt8{1U} << 5U) |
    (Foundation::UInt8{1U} << 6U) | (Foundation::UInt8{1U} << 7U)};

[[nodiscard]] Foundation::Result<EncodedInitialTranslation, InitialTranslationEncodingError>
Fail(const InitialTranslationEncodingError error) noexcept
{
    return Foundation::Result<EncodedInitialTranslation, InitialTranslationEncodingError>::Failure(error);
}

[[nodiscard]] bool IsCapabilitySetValid(const TranslationCapabilities& capabilities) noexcept
{
    return (capabilities.PhysicalAddressBits >= 32U) && (capabilities.PhysicalAddressBits <= 63U) &&
           (capabilities.ImplementedVirtualAddressMostSignificantBit >= 50U) &&
           (capabilities.ImplementedVirtualAddressMostSignificantBit <= 60U) &&
           (capabilities.RegionIdentifierBits >= 18U) && (capabilities.RegionIdentifierBits <= 24U) &&
           (capabilities.ProtectionKeyBits >= 18U) && (capabilities.ProtectionKeyBits <= 24U) &&
           (capabilities.ProtectionKeyBits >= capabilities.RegionIdentifierBits) &&
           ((capabilities.InsertablePageSizeMask & PreferredPageSizeBit) != 0U) &&
           ((capabilities.InsertablePageSizeMask & BelowMinimumPageSizeMask) == 0U) &&
           ((capabilities.MemoryAttributeMask & static_cast<Foundation::UInt8>(~ArchitectedMemoryAttributeMask)) ==
            0U) &&
           (capabilities.Reserved16 == 0U) && (capabilities.Reserved64 == 0U);
}

[[nodiscard]] bool FitsUnsignedBits(const Foundation::UInt32 value, const Foundation::UInt8 bitCount) noexcept
{
    return static_cast<Foundation::UInt64>(value) < (Foundation::UInt64{1U} << bitCount);
}

[[nodiscard]] bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
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

[[nodiscard]] bool IsVirtualRangeImplemented(const Foundation::UInt64 address, const Foundation::UInt8 pageShift,
                                             const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (pageShift > implementedMostSignificantBit)
    {
        return false;
    }
    const Foundation::UInt64 pageSize = Foundation::UInt64{1U} << pageShift;
    const Foundation::UInt64 maximumAddress = ~Foundation::UInt64{0U};
    if (address > (maximumAddress - (pageSize - 1U)))
    {
        return false;
    }
    const Foundation::UInt64 endAddress = address + pageSize - 1U;
    return ((address >> 61U) == (endAddress >> 61U)) &&
           IsVirtualAddressImplemented(address, implementedMostSignificantBit) &&
           IsVirtualAddressImplemented(endAddress, implementedMostSignificantBit);
}

[[nodiscard]] bool IsPhysicalRangeEncodable(const Foundation::UInt64 address, const Foundation::UInt8 pageShift,
                                            const Foundation::UInt8 physicalAddressBits) noexcept
{
    const Foundation::UInt8 encodableAddressBits =
        physicalAddressBits < 50U ? physicalAddressBits : Foundation::UInt8{50U};
    if (pageShift > encodableAddressBits)
    {
        return false;
    }
    const Foundation::UInt64 pageSize = Foundation::UInt64{1U} << pageShift;
    if ((address & (pageSize - 1U)) != 0U)
    {
        return false;
    }
    if ((address & ~PhysicalPageNumberMask) != 0U)
    {
        return false;
    }
    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << encodableAddressBits;
    return address <= (physicalLimit - pageSize);
}

[[nodiscard]] bool TryGetMemoryAttribute(const MemoryCachePolicy policy, Foundation::UInt8& memoryAttribute) noexcept
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
    default:
        return false;
    }
}

[[nodiscard]] bool TryGetAccessRights(const InitialMapping& mapping, Foundation::UInt64& accessRights,
                                      bool& writable) noexcept
{
    constexpr Foundation::UInt16 ReadBit = static_cast<Foundation::UInt16>(MappingPermission::Read);
    constexpr Foundation::UInt16 WriteBit = static_cast<Foundation::UInt16>(MappingPermission::Write);
    constexpr Foundation::UInt16 ExecuteBit = static_cast<Foundation::UInt16>(MappingPermission::Execute);
    constexpr Foundation::UInt16 GlobalBit = static_cast<Foundation::UInt16>(MappingPermission::Global);
    const Foundation::UInt16 relevantPermissions =
        static_cast<Foundation::UInt16>(mapping.Permissions.GetBits() & static_cast<Foundation::UInt16>(~GlobalBit));

    writable = false;
    switch (relevantPermissions)
    {
    case ReadBit:
        accessRights = ReadOnlyAccessRights;
        return true;
    case static_cast<Foundation::UInt16>(ReadBit | ExecuteBit):
        accessRights = ReadExecuteAccessRights;
        return true;
    case static_cast<Foundation::UInt16>(ReadBit | WriteBit):
        accessRights = ReadWriteAccessRights;
        writable = true;
        return true;
    default:
        return false;
    }
}
} // namespace

Foundation::Result<EncodedInitialTranslation, InitialTranslationEncodingError>
InitialTranslationEncoder::EncodePage(const Kernel::Memory::InitialMappingPlan& plan,
                                      const Foundation::Size mappingIndex, const Foundation::UInt64 pageIndex,
                                      const TranslationCapabilities& capabilities,
                                      const RegionConfiguration& region) noexcept
{
    return EncodeRange(plan, mappingIndex, pageIndex, InitialTranslationPageShift, capabilities, region);
}

Foundation::Result<EncodedInitialTranslation, InitialTranslationEncodingError>
InitialTranslationEncoder::EncodeRange(const Kernel::Memory::InitialMappingPlan& plan,
                                       const Foundation::Size mappingIndex, const Foundation::UInt64 firstPageIndex,
                                       const Foundation::UInt8 pageShift, const TranslationCapabilities& capabilities,
                                       const RegionConfiguration& region) noexcept
{
    if (!plan.IsSealed())
    {
        return Fail(InitialTranslationEncodingError::PlanNotSealed);
    }
    if (!IsCapabilitySetValid(capabilities))
    {
        return Fail(InitialTranslationEncodingError::InvalidCapabilities);
    }
    if ((pageShift < InitialTranslationPageShift) || (pageShift > 60U))
    {
        return Fail(InitialTranslationEncodingError::InvalidPageShift);
    }
    if ((capabilities.InsertablePageSizeMask & (Foundation::UInt64{1U} << pageShift)) == 0U)
    {
        return Fail(InitialTranslationEncodingError::UnsupportedPageSize);
    }
    if ((region.Reserved16 != 0U) || (region.Reserved32 != 0U) ||
        !FitsUnsignedBits(region.RegionIdentifier, capabilities.RegionIdentifierBits) ||
        !FitsUnsignedBits(region.ProtectionKey, capabilities.ProtectionKeyBits))
    {
        return Fail(InitialTranslationEncodingError::InvalidRegionConfiguration);
    }
    if (region.VhptWalkerEnabled && !capabilities.HardwareWalkerImplemented)
    {
        return Fail(InitialTranslationEncodingError::HardwareWalkerUnavailable);
    }

    const auto mappingResult = plan.GetMapping(mappingIndex);
    if (!mappingResult.HasValue())
    {
        return Fail(InitialTranslationEncodingError::MappingIndexOutOfBounds);
    }
    const InitialMapping mapping = mappingResult.GetValue();
    if (firstPageIndex >= mapping.Pages.GetValue())
    {
        return Fail(InitialTranslationEncodingError::PageIndexOutOfBounds);
    }
    const Foundation::UInt64 coveredBasePages = Foundation::UInt64{1U} << (pageShift - InitialTranslationPageShift);
    if (coveredBasePages > (mapping.Pages.GetValue() - firstPageIndex))
    {
        return Fail(InitialTranslationEncodingError::TranslationRangeOutOfBounds);
    }
    const Foundation::UInt64 pageOffset = firstPageIndex << InitialTranslationPageShift;
    const Foundation::UInt64 virtualAddress = mapping.VirtualStart.GetValue() + pageOffset;
    const Foundation::UInt64 physicalAddress = mapping.PhysicalStart.GetValue() + pageOffset;
    const Foundation::UInt64 translationSize = Foundation::UInt64{1U} << pageShift;
    if (((virtualAddress & (translationSize - 1U)) != 0U) || ((physicalAddress & (translationSize - 1U)) != 0U))
    {
        return Fail(InitialTranslationEncodingError::MisalignedTranslationRange);
    }
    const Foundation::UInt8 virtualRegion = static_cast<Foundation::UInt8>(virtualAddress >> 61U);
    for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
    {
        const auto regionMappingResult = plan.GetMapping(index);
        if (!regionMappingResult.HasValue())
        {
            return Fail(InitialTranslationEncodingError::InternalInvariantViolation);
        }
        const InitialMapping regionMapping = regionMappingResult.GetValue();
        const Foundation::UInt64 regionMappingEnd =
            regionMapping.VirtualStart.GetValue() + (regionMapping.Pages.GetValue() << InitialTranslationPageShift);
        const Foundation::UInt8 firstRegion =
            static_cast<Foundation::UInt8>(regionMapping.VirtualStart.GetValue() >> 61U);
        const Foundation::UInt8 lastRegion = static_cast<Foundation::UInt8>((regionMappingEnd - 1U) >> 61U);
        if ((virtualRegion >= firstRegion) && (virtualRegion <= lastRegion))
        {
            const bool mappingIsGlobal = regionMapping.Permissions.Contains(MappingPermission::Global);
            if (mappingIsGlobal != region.SharedAcrossAddressSpaces)
            {
                return Fail(InitialTranslationEncodingError::GlobalRegionMismatch);
            }
        }
    }

    if (!IsVirtualRangeImplemented(virtualAddress, pageShift, capabilities.ImplementedVirtualAddressMostSignificantBit))
    {
        return Fail(InitialTranslationEncodingError::UnimplementedVirtualAddress);
    }

    if (!IsPhysicalRangeEncodable(physicalAddress, pageShift, capabilities.PhysicalAddressBits))
    {
        return Fail(InitialTranslationEncodingError::PhysicalAddressOutOfRange);
    }

    Foundation::UInt8 memoryAttribute{0U};
    if (!TryGetMemoryAttribute(mapping.CachePolicy, memoryAttribute))
    {
        return Fail(InitialTranslationEncodingError::UnsupportedCachePolicy);
    }
    if ((capabilities.MemoryAttributeMask & static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << memoryAttribute)) ==
        0U)
    {
        return Fail(InitialTranslationEncodingError::UnsupportedMemoryAttribute);
    }

    Foundation::UInt64 accessRights{0U};
    bool writable{false};
    if (!TryGetAccessRights(mapping, accessRights, writable))
    {
        return Fail(InitialTranslationEncodingError::UnsupportedPermissions);
    }

    Foundation::UInt64 translationWord = physicalAddress | PresentBit |
                                         (static_cast<Foundation::UInt64>(memoryAttribute) << 2U) | AccessedBit |
                                         ExceptionDeferralBit | accessRights;
    if (writable)
    {
        translationWord |= DirtyBit;
    }

    const Foundation::UInt64 translationInsertion = (static_cast<Foundation::UInt64>(region.ProtectionKey) << 8U) |
                                                    (static_cast<Foundation::UInt64>(pageShift) << 2U);
    const Foundation::UInt64 regionRegister =
        (static_cast<Foundation::UInt64>(region.RegionIdentifier) << 8U) |
        (static_cast<Foundation::UInt64>(InitialTranslationPageShift) << 2U) |
        (region.VhptWalkerEnabled ? Foundation::UInt64{1U} : Foundation::UInt64{0U});

    return Foundation::Result<EncodedInitialTranslation, InitialTranslationEncodingError>::Success(
        EncodedInitialTranslation{
            .TranslationWord = translationWord,
            .TranslationInsertion = translationInsertion,
            .FaultingAddress = virtualAddress,
            .RegionRegister = regionRegister,
        });
}
} // namespace Asteria::Platform::Ia64::Memory
