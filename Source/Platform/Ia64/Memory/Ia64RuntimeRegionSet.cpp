// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt8 MinimumRegionIdentifierBits{18U};
constexpr Foundation::UInt8 MaximumRegionIdentifierBits{24U};
constexpr Foundation::UInt64 VhptEnableMask{Foundation::UInt64{1U}};
constexpr Foundation::UInt64 ReservedLowMask{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 PreferredPageSizeMask{0x3FU << 2U};
constexpr Foundation::UInt64 RegionIdentifierMask{0x00FF'FFFFU << 8U};
constexpr Foundation::UInt64 ArchitectedRegionRegisterMask{VhptEnableMask | PreferredPageSizeMask |
                                                           RegionIdentifierMask};

[[nodiscard]] constexpr bool AreCapabilitiesValid(const TranslationCapabilities& capabilities) noexcept
{
    return (capabilities.RegionIdentifierBits >= MinimumRegionIdentifierBits) &&
           (capabilities.RegionIdentifierBits <= MaximumRegionIdentifierBits) && (capabilities.Reserved16 == 0U) &&
           (capabilities.Reserved64 == 0U);
}

[[nodiscard]] constexpr bool IsRegionIdentifierRepresentable(const Foundation::UInt32 regionIdentifier,
                                                             const Foundation::UInt8 regionIdentifierBits) noexcept
{
    return static_cast<Foundation::UInt64>(regionIdentifier) < (Foundation::UInt64{1U} << regionIdentifierBits);
}

[[nodiscard]] constexpr Foundation::UInt32 GetRegionIdentifier(const Foundation::UInt64 regionRegister) noexcept
{
    return static_cast<Foundation::UInt32>((regionRegister & RegionIdentifierMask) >> 8U);
}

[[nodiscard]] constexpr Foundation::UInt8 GetPreferredPageShift(const Foundation::UInt64 regionRegister) noexcept
{
    return static_cast<Foundation::UInt8>((regionRegister & PreferredPageSizeMask) >> 2U);
}

[[nodiscard]] constexpr Ia64RuntimeRegionSetError
ValidateBootstrapWord(const Foundation::UInt64 regionRegister, const Foundation::UInt8 regionIdentifierBits) noexcept
{
    if (((regionRegister & ReservedLowMask) != 0U) || ((regionRegister & ~ArchitectedRegionRegisterMask) != 0U))
    {
        return Ia64RuntimeRegionSetError::InvalidBootstrapRegionRegister;
    }
    if (!IsRegionIdentifierRepresentable(GetRegionIdentifier(regionRegister), regionIdentifierBits))
    {
        return Ia64RuntimeRegionSetError::UnrepresentableRegionIdentifier;
    }
    if (((regionRegister & VhptEnableMask) != 0U) ||
        (GetPreferredPageShift(regionRegister) != Ia64BootstrapPreferredPageShift))
    {
        return Ia64RuntimeRegionSetError::UnsupportedBootstrapRegionRegister;
    }
    return Ia64RuntimeRegionSetError::None;
}
} // namespace

Ia64RuntimeRegionSetError Ia64RuntimeRegionSet::AdoptBootstrap(const Ia64BootstrapRegionRegisterWords& bootstrap,
                                                               const TranslationCapabilities& capabilities,
                                                               Ia64RuntimeRegionSet& output) noexcept
{
    if (output.m_built)
    {
        return Ia64RuntimeRegionSetError::OutputAlreadyBuilt;
    }
    if (!AreCapabilitiesValid(capabilities))
    {
        return Ia64RuntimeRegionSetError::InvalidCapabilities;
    }

    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        const Ia64RuntimeRegionSetError error =
            ValidateBootstrapWord(bootstrap.RegionRegisters[index], capabilities.RegionIdentifierBits);
        if (error != Ia64RuntimeRegionSetError::None)
        {
            return error;
        }
        const Foundation::UInt64 expected = (static_cast<Foundation::UInt64>(Ia64SharedBootstrapRegionIdentifier +
                                                                             static_cast<Foundation::UInt32>(index))
                                             << 8U) |
                                            (static_cast<Foundation::UInt64>(Ia64BootstrapPreferredPageShift) << 2U);
        if (bootstrap.RegionRegisters[index] != expected)
        {
            return index == 0U ? Ia64RuntimeRegionSetError::SharedBootstrapAnchorMismatch
                               : Ia64RuntimeRegionSetError::BootstrapRegionSetMismatch;
        }
    }

    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        output.m_regionRegisters[index] = bootstrap.RegionRegisters[index];
    }
    output.m_built = true;
    return Ia64RuntimeRegionSetError::None;
}

Ia64RuntimeRegionSetError Ia64RuntimeRegionSet::Construct(const Ia64RegionIdentifierAllocationReceipt& receipt,
                                                          const TranslationCapabilities& capabilities,
                                                          Ia64RuntimeRegionSet& output) noexcept
{
    if (output.m_built)
    {
        return Ia64RuntimeRegionSetError::OutputAlreadyBuilt;
    }
    if (!AreCapabilitiesValid(capabilities))
    {
        return Ia64RuntimeRegionSetError::InvalidCapabilities;
    }
    if (!receipt.IsValid())
    {
        return Ia64RuntimeRegionSetError::InvalidReceipt;
    }
    Foundation::UInt64 regionRegisters[Ia64RuntimeRegionCount]{};
    regionRegisters[0U] = Ia64SharedBootstrapRegionRegister;
    for (Foundation::Size index{1U}; index < Ia64RuntimeRegionCount; ++index)
    {
        const auto candidate = receipt.GetRegionIdentifier(index);
        if (!candidate.HasValue())
        {
            return Ia64RuntimeRegionSetError::InvalidReceipt;
        }
        if (!IsRegionIdentifierRepresentable(candidate.GetValue(), capabilities.RegionIdentifierBits))
        {
            return Ia64RuntimeRegionSetError::UnrepresentableRegionIdentifier;
        }
        regionRegisters[index] = (static_cast<Foundation::UInt64>(candidate.GetValue()) << 8U) |
                                 (static_cast<Foundation::UInt64>(Ia64BootstrapPreferredPageShift) << 2U);
    }

    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        output.m_regionRegisters[index] = regionRegisters[index];
    }
    output.m_built = true;
    return Ia64RuntimeRegionSetError::None;
}

Foundation::Result<Foundation::UInt64, Ia64RuntimeRegionSetError>
Ia64RuntimeRegionSet::GetRegionRegister(const Foundation::Size regionIndex) const noexcept
{
    using ResultType = Foundation::Result<Foundation::UInt64, Ia64RuntimeRegionSetError>;

    if (!m_built)
    {
        return ResultType::Failure(Ia64RuntimeRegionSetError::RegionSetNotBuilt);
    }
    if (regionIndex >= Ia64RuntimeRegionCount)
    {
        return ResultType::Failure(Ia64RuntimeRegionSetError::RegionIndexOutOfBounds);
    }
    return ResultType::Success(m_regionRegisters[regionIndex]);
}
} // namespace Asteria::Platform::Ia64::Memory
