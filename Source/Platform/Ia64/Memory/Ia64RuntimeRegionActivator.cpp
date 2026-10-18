// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeRegionActivator.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ActivateRuntimeRegions(
    Asteria::Foundation::UInt64 regionRegister1, Asteria::Foundation::UInt64 regionRegister2,
    Asteria::Foundation::UInt64 regionRegister3, Asteria::Foundation::UInt64 regionRegister4,
    Asteria::Foundation::UInt64 regionRegister5, Asteria::Foundation::UInt64 regionRegister6,
    Asteria::Foundation::UInt64 regionRegister7) noexcept;

// The target entry point is in Ia64RuntimeRegionActivator.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 PreferredPageSizeMask{Foundation::UInt64{0x3FU} << 2U};
constexpr Foundation::UInt64 RegionIdentifierMask{Foundation::UInt64{0x00FF'FFFFU} << 8U};
constexpr Foundation::UInt64 ArchitectedRegionRegisterMask{PreferredPageSizeMask | RegionIdentifierMask};
constexpr Foundation::UInt64 RequiredPreferredPageSize{Foundation::UInt64{Ia64BootstrapPreferredPageShift} << 2U};
#if defined(__ia64__)
constexpr Foundation::UInt64 PhysicalBootstrapRequiredClearPsrMask{0x0000'0010'0802'E000ULL};
#endif
constexpr Foundation::UInt64 AdapterSuccess{0U};
constexpr Foundation::UInt64 AdapterRejectedBeforeCommit{1U};

[[nodiscard]] constexpr Foundation::UInt32 GetRegionIdentifier(const Foundation::UInt64 regionRegister) noexcept
{
    return static_cast<Foundation::UInt32>((regionRegister & RegionIdentifierMask) >> 8U);
}

[[nodiscard]] constexpr bool IsPrivateRegionRegisterValid(const Foundation::UInt64 regionRegister) noexcept
{
    const Foundation::UInt32 regionIdentifier = GetRegionIdentifier(regionRegister);
    return ((regionRegister & ~ArchitectedRegionRegisterMask) == 0U) &&
           ((regionRegister & PreferredPageSizeMask) == RequiredPreferredPageSize) && (regionIdentifier != 0U) &&
           (regionIdentifier != Ia64SharedBootstrapRegionIdentifier);
}

[[nodiscard]] Ia64RuntimeRegionActivationError
CollectValidatedRegionRegisters(const Ia64RuntimeRegionSet& regionSet,
                                Foundation::UInt64 (&regionRegisters)[Ia64RuntimeRegionCount]) noexcept
{
    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        const auto regionRegister = regionSet.GetRegionRegister(index);
        if (!regionRegister.HasValue())
        {
            return Ia64RuntimeRegionActivationError::InvalidRegionSet;
        }
        regionRegisters[index] = regionRegister.GetValue();
    }

    if (regionRegisters[0U] != Ia64SharedBootstrapRegionRegister)
    {
        return Ia64RuntimeRegionActivationError::InvalidRegionSet;
    }
    for (Foundation::Size index{1U}; index < Ia64RuntimeRegionCount; ++index)
    {
        if (!IsPrivateRegionRegisterValid(regionRegisters[index]))
        {
            return Ia64RuntimeRegionActivationError::InvalidRegionSet;
        }
        const Foundation::UInt32 regionIdentifier = GetRegionIdentifier(regionRegisters[index]);
        for (Foundation::Size previous{1U}; previous < index; ++previous)
        {
            if (GetRegionIdentifier(regionRegisters[previous]) == regionIdentifier)
            {
                return Ia64RuntimeRegionActivationError::InvalidRegionSet;
            }
        }
    }
    return Ia64RuntimeRegionActivationError::None;
}

[[nodiscard]] constexpr Ia64RuntimeRegionActivationError
MapAdapterStatus(const Foundation::UInt64 adapterStatus) noexcept
{
    if (adapterStatus == AdapterSuccess)
    {
        return Ia64RuntimeRegionActivationError::None;
    }
    if (adapterStatus == AdapterRejectedBeforeCommit)
    {
        return Ia64RuntimeRegionActivationError::RejectedBeforeCommit;
    }
    return Ia64RuntimeRegionActivationError::CommittedStateUnknown;
}
} // namespace

Ia64RuntimeRegionActivationError Ia64RuntimeRegionActivator::Activate(const Ia64RuntimeRegionSet& regionSet) noexcept
{
    Foundation::UInt64 regionRegisters[Ia64RuntimeRegionCount]{};
    const Ia64RuntimeRegionActivationError validation = CollectValidatedRegionRegisters(regionSet, regionRegisters);
    if (validation != Ia64RuntimeRegionActivationError::None)
    {
        return validation;
    }

    const Foundation::UInt64 adapterStatus = AsteriaIa64ActivateRuntimeRegions(
        regionRegisters[1U], regionRegisters[2U], regionRegisters[3U], regionRegisters[4U], regionRegisters[5U],
        regionRegisters[6U], regionRegisters[7U]);
    return MapAdapterStatus(adapterStatus);
}

ASTERIA_NOINLINE_NOIPA Ia64RuntimeRegionActivationError
Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(const Ia64RuntimeRegionSet& regionSet) noexcept
{
    Foundation::UInt64 regionRegisters[Ia64RuntimeRegionCount]{};
    const Ia64RuntimeRegionActivationError validation = CollectValidatedRegionRegisters(regionSet, regionRegisters);
    if (validation != Ia64RuntimeRegionActivationError::None)
    {
        return validation;
    }

#if defined(__ia64__)
    Foundation::UInt64 processorState{0U};
    __asm__ volatile("mov %0 = psr\n"
                     ";;\n"
                     : "=r"(processorState)
                     :
                     : "memory");
    if ((processorState & PhysicalBootstrapRequiredClearPsrMask) != 0U)
    {
        return Ia64RuntimeRegionActivationError::RejectedBeforeCommit;
    }
#endif

    const Foundation::UInt64 adapterStatus = AsteriaIa64ActivateRuntimeRegions(
        regionRegisters[1U], regionRegisters[2U], regionRegisters[3U], regionRegisters[4U], regionRegisters[5U],
        regionRegisters[6U], regionRegisters[7U]);
    return MapAdapterStatus(adapterStatus);
}
} // namespace Asteria::Platform::Ia64::Memory
