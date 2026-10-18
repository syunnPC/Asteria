// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_BOOTSTRAP_MAPPING_PLAN_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_BOOTSTRAP_MAPPING_PLAN_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
/// The run-time memory reservation holds the state the interruption and
/// refill paths touch with PSR.ic clear (Ia64BspRuntimeMemory) in one
/// naturally aligned page that one locked DTR maps, whatever the object's
/// size within it and wherever the reservation lands
///. The boot
/// prefers a 16 MiB page; SelectRuntimeMemoryReservationPageShift names the
/// fallback.
constexpr Foundation::UInt8 RuntimeMemoryReservationPreferredPageShift{24U};
constexpr Foundation::UInt64 RuntimeMemoryReservationPreferredBytes{Foundation::UInt64{1U}
                                                                    << RuntimeMemoryReservationPreferredPageShift};
constexpr Foundation::UInt8 RuntimeMemoryReservationMaximumPageShift{28U};
constexpr Foundation::Size BootstrapRuntimeMemoryMappingCount{1U};
constexpr Foundation::UInt64 BootstrapStackPageOffset{0U};
constexpr Foundation::UInt64 BootstrapStackPageCount{8U};
constexpr Foundation::UInt64 BootstrapTranslationStoragePageOffset{BootstrapStackPageOffset + BootstrapStackPageCount};
constexpr Foundation::UInt64 BootstrapTranslationStoragePageCount{1U};
constexpr Foundation::UInt64 BootstrapActivationGuardPageOffset{BootstrapTranslationStoragePageOffset +
                                                                BootstrapTranslationStoragePageCount};
constexpr Foundation::UInt64 BootstrapActivationGuardPageCount{7U};
constexpr Foundation::UInt64 BootstrapInterruptVectorTablePageOffset{BootstrapActivationGuardPageOffset +
                                                                     BootstrapActivationGuardPageCount};
constexpr Foundation::UInt64 BootstrapInterruptVectorTablePageCount{8U};
constexpr Foundation::UInt64 BootstrapInterruptVectorTableAlignmentPages{8U};
constexpr Foundation::UInt64 BootstrapRseBackingStorePageOffset{BootstrapInterruptVectorTablePageOffset +
                                                                BootstrapInterruptVectorTablePageCount};
constexpr Foundation::UInt64 BootstrapRseBackingStorePageCount{8U};
constexpr Foundation::UInt64 BootstrapReservedPageCount{BootstrapRseBackingStorePageOffset +
                                                        BootstrapRseBackingStorePageCount};
constexpr Foundation::UInt64 BootstrapReservedAlignmentPages{BootstrapInterruptVectorTableAlignmentPages};
/// The old disjoint-reservation shape remains available to physical bootstrap
/// callers. A reservation contained at the runtime page's start selects the
/// consolidated shape; its whole RW/NX page owns all bootstrap data.
constexpr Foundation::Size BootstrapConsolidatedMappingCount{4U};
constexpr Foundation::UInt64 BootstrapRuntimeObjectOffsetBytes{BootstrapReservedPageCount *
                                                               Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::Size BootstrapLoadedImageMappingCount{3U};
constexpr Foundation::Size BootstrapInitialMappingCount{BootstrapLoadedImageMappingCount + 4U +
                                                        BootstrapRuntimeMemoryMappingCount};

[[nodiscard]] constexpr bool IsBootstrapMappingCountValid(const Foundation::Size count) noexcept
{
    return (count == BootstrapInitialMappingCount) || (count == BootstrapConsolidatedMappingCount);
}

/// The immutable bootstrap fail-stop table; the target definition lives in.text.
[[nodiscard]] Foundation::UInt64 GetStaticBootstrapInterruptVectorTableAddress() noexcept;

static_assert(BootstrapStackPageOffset == 0U);
static_assert(BootstrapStackPageCount == 8U);
static_assert(BootstrapTranslationStoragePageOffset == 8U);
static_assert(BootstrapTranslationStoragePageCount == 1U);
static_assert(BootstrapActivationGuardPageOffset == 9U);
static_assert(BootstrapActivationGuardPageCount == 7U);
static_assert(BootstrapInterruptVectorTablePageOffset == 16U);
static_assert(BootstrapInterruptVectorTablePageCount == 8U);
static_assert(BootstrapRseBackingStorePageOffset == 24U);
static_assert(BootstrapRseBackingStorePageCount == 8U);
static_assert(BootstrapReservedPageCount == 32U);
static_assert(Firmware::Ia64LoadedImageSectionCount == 6U);
static_assert(BootstrapLoadedImageMappingCount == 3U);
static_assert(BootstrapRuntimeMemoryMappingCount == 1U);
static_assert(BootstrapInitialMappingCount == 8U);
static_assert(RuntimeMemoryReservationPreferredBytes == 16U * 1024U * 1024U);
static_assert((ArchitectedMinimumInsertablePageSizeMask & RuntimeMemoryReservationPreferredBytes) != 0U);
static_assert((ArchitectedMinimumInsertablePageSizeMask &
               (Foundation::UInt64{1U} << RuntimeMemoryReservationMaximumPageShift)) != 0U);
static_assert(BootstrapInterruptVectorTableAlignmentPages == 8U);
static_assert(BootstrapReservedAlignmentPages == 8U);
static_assert((BootstrapReservedAlignmentPages & (BootstrapReservedAlignmentPages - 1U)) == 0U);
static_assert((BootstrapInterruptVectorTablePageOffset % BootstrapInterruptVectorTableAlignmentPages) == 0U);
static_assert(BootstrapStackPageOffset + BootstrapStackPageCount == BootstrapTranslationStoragePageOffset);
static_assert(BootstrapTranslationStoragePageOffset + BootstrapTranslationStoragePageCount ==
              BootstrapActivationGuardPageOffset);
static_assert(BootstrapActivationGuardPageOffset + BootstrapActivationGuardPageCount ==
              BootstrapInterruptVectorTablePageOffset);
static_assert(BootstrapInterruptVectorTablePageOffset + BootstrapInterruptVectorTablePageCount ==
              BootstrapRseBackingStorePageOffset);
static_assert(BootstrapRseBackingStorePageOffset + BootstrapRseBackingStorePageCount == BootstrapReservedPageCount);

enum class BootstrapMappingPlanError : Foundation::UInt32
{
    None,
    OutputNotEmpty,
    InvalidImageLayout,
    InvalidBootInfo,
    InvalidImageMemoryProvenance,
    InvalidReservedPages,
    InvalidReservedMemoryProvenance,
    ReservedPagesOverlapImage,
    MappingPlanRejected,
    InternalInvariantViolation,
    InvalidRuntimeMemoryPages,
    InvalidRuntimeMemoryProvenance,
    RuntimeMemoryPagesOverlap,
};

/// The page shift of the run-time memory reservation the boot tries first
/// (Preferred) and the one it falls back to when no free range can hold the
/// preferred page (Fallback); zero where no size qualifies, and Fallback is
/// zero when it equals Preferred. Both are insertable sizes of the processor's
/// PAL-reported mask that also belong to the architected minimum set (so
/// every processor of the system can insert them), hold objectBytes, and do
/// not exceed 256 MiB. The preferred page is the 16 MiB one when it holds the
/// object; the fallback is the smallest qualifying size.
struct RuntimeMemoryReservationPageShifts
{
    Foundation::UInt8 Preferred;
    Foundation::UInt8 Fallback;
};

[[nodiscard]] constexpr RuntimeMemoryReservationPageShifts
SelectRuntimeMemoryReservationPageShifts(const Foundation::UInt64 insertablePageSizeMask,
                                         const Foundation::UInt64 objectBytes) noexcept
{
    const Foundation::UInt64 usable = insertablePageSizeMask & ArchitectedMinimumInsertablePageSizeMask;
    Foundation::UInt8 smallest{0U};
    for (Foundation::UInt8 shift{12U}; shift <= RuntimeMemoryReservationMaximumPageShift; ++shift)
    {
        const Foundation::UInt64 bytes = Foundation::UInt64{1U} << shift;
        if (((usable & bytes) != 0U) && (bytes >= objectBytes))
        {
            smallest = shift;
            break;
        }
    }
    if ((objectBytes == 0U) || (smallest == 0U))
    {
        return RuntimeMemoryReservationPageShifts{
            .Preferred = 0U,
            .Fallback = 0U,
        };
    }
    const bool preferredFits = ((usable & RuntimeMemoryReservationPreferredBytes) != 0U) &&
                               (RuntimeMemoryReservationPreferredBytes >= objectBytes);
    if (!preferredFits)
    {
        return RuntimeMemoryReservationPageShifts{
            .Preferred = smallest,
            .Fallback = 0U,
        };
    }
    return RuntimeMemoryReservationPageShifts{
        .Preferred = RuntimeMemoryReservationPreferredPageShift,
        .Fallback = smallest == RuntimeMemoryReservationPreferredPageShift ? Foundation::UInt8{0U} : smallest,
    };
}

struct BootstrapMappingPlanFailure
{
    BootstrapMappingPlanError Error;
    Kernel::Memory::InitialMappingPlanError MappingError;
    Foundation::Size MappingIndex;
};

struct BootstrapMappingPlanSummary
{
    Foundation::UInt64 StackPhysicalStart;
    Foundation::UInt64 TranslationStoragePhysicalStart;
    Foundation::UInt16 MappingCount;
    Foundation::UInt16 Reserved16;
    Foundation::UInt32 Reserved32;
    Foundation::UInt64 RseBackingStorePhysicalStart;
};

/// These functions accept only a summary returned by a successful Build call.
/// In the consolidated shape the lower guard lies outside the runtime page;
/// in the disjoint shape it lies between translation storage and the IVT.
/// The builder initializes no storage or architectural register.
[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapActivationGuardPhysicalStart(const BootstrapMappingPlanSummary& summary) noexcept
{
    if (summary.MappingCount == BootstrapConsolidatedMappingCount)
    {
        return summary.StackPhysicalStart - (BootstrapActivationGuardPageCount * Kernel::Memory::MappingPageSizeBytes);
    }
    return summary.StackPhysicalStart + (BootstrapActivationGuardPageOffset * Kernel::Memory::MappingPageSizeBytes);
}

[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapInterruptVectorTablePhysicalStart(const BootstrapMappingPlanSummary& summary) noexcept
{
    if (summary.MappingCount == BootstrapConsolidatedMappingCount)
    {
        return GetStaticBootstrapInterruptVectorTableAddress();
    }
    return summary.StackPhysicalStart +
           (BootstrapInterruptVectorTablePageOffset * Kernel::Memory::MappingPageSizeBytes);
}

[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapRseBackingStorePhysicalStart(const BootstrapMappingPlanSummary& summary) noexcept
{
    if (summary.MappingCount == BootstrapConsolidatedMappingCount)
    {
        return summary.RseBackingStorePhysicalStart;
    }
    return summary.StackPhysicalStart + (BootstrapRseBackingStorePageOffset * Kernel::Memory::MappingPageSizeBytes);
}

[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapReservedPhysicalEnd(const BootstrapMappingPlanSummary& summary) noexcept
{
    return summary.StackPhysicalStart + (BootstrapReservedPageCount * Kernel::Memory::MappingPageSizeBytes);
}

/// Builds the identity-mapped Global Region-0 seed before native activation.
/// Adjacent equally protected PE sections form three image mappings.
/// runtimeMemoryPages is one naturally aligned architected insertable page.
/// A reservedPages bootstrap view at its start selects consolidation: one
/// RW/NX NucleusBootstrapRuntimeData mapping supplies stack, translation
/// storage, RSE and runtime-object coverage. The fail-stop IVT is then static
/// text. A disjoint bootstrap reservation retains four separate mappings and
/// a NucleusWritableData runtime mapping. No storage or register is initialized.
/// finalBootInfo proves complete Loader provenance for the image and matching
/// Conventional/write-back provenance for both data views. Native boot retains
/// unmapped page-end guards outside its runtime page in the allocator.
class BootstrapMappingPlanBuilder final
{
public:
    [[nodiscard]] static Foundation::Result<BootstrapMappingPlanSummary, BootstrapMappingPlanFailure>
    Build(const Firmware::Ia64LoadedImageLayout& image, const Boot::BootInfo& finalBootInfo,
          const Kernel::Memory::PhysicalPageRange& reservedPages,
          const Kernel::Memory::PhysicalPageRange& runtimeMemoryPages,
          Kernel::Memory::InitialMappingPlan& output) noexcept;

private:
    BootstrapMappingPlanBuilder() = delete;
};

static_assert(sizeof(BootstrapMappingPlanError) == 4U);
static_assert(sizeof(BootstrapMappingPlanFailure) == 16U);
static_assert(alignof(BootstrapMappingPlanFailure) == 8U);
static_assert(__is_aggregate(BootstrapMappingPlanFailure));
static_assert(__is_standard_layout(BootstrapMappingPlanFailure));
static_assert(__is_trivially_copyable(BootstrapMappingPlanFailure));
static_assert(__builtin_offsetof(BootstrapMappingPlanFailure, Error) == 0U);
static_assert(__builtin_offsetof(BootstrapMappingPlanFailure, MappingError) == 4U);
static_assert(__builtin_offsetof(BootstrapMappingPlanFailure, MappingIndex) == 8U);
static_assert(sizeof(BootstrapMappingPlanSummary) == 32U);
static_assert(alignof(BootstrapMappingPlanSummary) == 8U);
static_assert(__is_aggregate(BootstrapMappingPlanSummary));
static_assert(__is_standard_layout(BootstrapMappingPlanSummary));
static_assert(__is_trivially_copyable(BootstrapMappingPlanSummary));
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, StackPhysicalStart) == 0U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, TranslationStoragePhysicalStart) == 8U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, MappingCount) == 16U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, Reserved16) == 18U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, Reserved32) == 20U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, RseBackingStorePhysicalStart) == 24U);
static_assert(sizeof(Foundation::Result<BootstrapMappingPlanSummary, BootstrapMappingPlanFailure>) == 56U);
static_assert(alignof(Foundation::Result<BootstrapMappingPlanSummary, BootstrapMappingPlanFailure>) == 8U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_BOOTSTRAP_MAPPING_PLAN_HPP_INCLUDED
