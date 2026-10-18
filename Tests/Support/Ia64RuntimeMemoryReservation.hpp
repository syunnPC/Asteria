// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_IA64_RUNTIME_MEMORY_RESERVATION_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_IA64_RUNTIME_MEMORY_RESERVATION_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"

namespace Asteria::TestSupport
{
/// Hosted IA-64 fixtures place the run-time memory reservation (the preferred
/// page, Ia64TranslationRegisterBudget.md) one preferred page above their
/// bootstrap reservation, whose base is aligned to that page. One Conventional
/// final-map region of Ia64FixtureReservationRegionPageCount pages from the
/// bootstrap base then proves the provenance of both reservations.
constexpr Foundation::UInt64 Ia64FixtureRuntimeMemoryOffsetBytes{
    Platform::Ia64::Memory::RuntimeMemoryReservationPreferredBytes};
constexpr Foundation::UInt64 Ia64FixtureRuntimeMemoryPageCount{
    Platform::Ia64::Memory::RuntimeMemoryReservationPreferredBytes / Kernel::Memory::PhysicalPageSizeBytes};
constexpr Foundation::UInt64 Ia64FixtureReservationRegionPageCount{2U * Ia64FixtureRuntimeMemoryPageCount};

/// The run-time memory reservation of a fixture whose bootstrap reservation
/// starts at reservedBase (a multiple of the preferred page), with the
/// attributes of its final-map region.
[[nodiscard]] constexpr Kernel::Memory::PhysicalPageRange
Ia64FixtureRuntimeMemoryPages(const Foundation::UInt64 reservedBase,
                              const Foundation::UInt64 attributes = 0x8U) noexcept
{
    return Kernel::Memory::PhysicalPageRange{
        .PhysicalStart = reservedBase + Ia64FixtureRuntimeMemoryOffsetBytes,
        .PageCount = Ia64FixtureRuntimeMemoryPageCount,
        .Attributes = attributes,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
}
} // namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_IA64_RUNTIME_MEMORY_RESERVATION_HPP_INCLUDED
