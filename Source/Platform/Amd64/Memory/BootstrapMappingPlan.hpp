// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_BOOTSTRAP_MAPPING_PLAN_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_BOOTSTRAP_MAPPING_PLAN_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Firmware/Amd64LoadedImageLayout.hpp"

namespace Asteria::Platform::Amd64::Memory
{
constexpr Foundation::UInt64 BootstrapGuardPageOffset{0U};
constexpr Foundation::UInt64 BootstrapGuardPageCount{1U};
constexpr Foundation::UInt64 BootstrapStackPageOffset{BootstrapGuardPageOffset + BootstrapGuardPageCount};
constexpr Foundation::UInt64 BootstrapStackPageCount{4U};
constexpr Foundation::UInt64 BootstrapPageTableStoragePageOffset{BootstrapStackPageOffset + BootstrapStackPageCount};
constexpr Foundation::UInt64 BootstrapPageTableStoragePageCount{16U};
constexpr Foundation::UInt64 BootstrapDescriptorTableStoragePageOffset{BootstrapPageTableStoragePageOffset +
                                                                       BootstrapPageTableStoragePageCount};
constexpr Foundation::UInt64 BootstrapDescriptorTableStoragePageCount{2U};
constexpr Foundation::UInt64 BootstrapReservedPageCount{BootstrapDescriptorTableStoragePageOffset +
                                                        BootstrapDescriptorTableStoragePageCount};
constexpr Foundation::UInt64 BootstrapReservedAlignmentPages{1U};
constexpr Foundation::Size BootstrapInitialMappingCount{Firmware::Amd64LoadedImageSectionCount + 3U};

/// Mapped addresses lie in two disjoint bounding envelopes: one image of at
/// most 2 MiB and one reservation smaller than 2 MiB. Each envelope can cross
/// at most one boundary, and therefore touch at most two prefixes, at each of
/// the PML4, PDPT, and page-directory levels.
constexpr Foundation::UInt64 BootstrapMaximumRequiredPageTablePages{1U + (2U * 2U * 3U)};

static_assert(BootstrapGuardPageOffset == 0U);
static_assert(BootstrapStackPageOffset == 1U);
static_assert(BootstrapPageTableStoragePageOffset == 5U);
static_assert(BootstrapDescriptorTableStoragePageOffset == 21U);
static_assert(BootstrapReservedPageCount == 23U);
static_assert(BootstrapInitialMappingCount == 10U);
static_assert(BootstrapMaximumRequiredPageTablePages == 13U);
static_assert(BootstrapPageTableStoragePageCount >= BootstrapMaximumRequiredPageTablePages);
static_assert((BootstrapReservedAlignmentPages & (BootstrapReservedAlignmentPages - 1U)) == 0U);

enum class BootstrapMappingPlanError : Foundation::UInt32
{
    None,
    OutputNotEmpty,
    InvalidImageLayout,
    InvalidBootInfo,
    InvalidImageMemoryProvenance,
    InvalidReservedPages,
    ReservedPagesOverlapImage,
    InvalidReservedMemoryProvenance,
    InternalInvariantViolation,
};

struct BootstrapMappingPlanSummary
{
    Foundation::UInt64 ReservedPhysicalStart;
    Foundation::UInt64 StackPhysicalStart;
    Foundation::UInt64 PageTableStoragePhysicalStart;
    Foundation::UInt64 DescriptorTableStoragePhysicalStart;
    Foundation::UInt16 MappingCount;
    Foundation::UInt16 Reserved16;
    Foundation::UInt32 Reserved32;
};

/// These functions accept only a summary produced by a successful Build call.
[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapGuardPhysicalStart(const BootstrapMappingPlanSummary& summary) noexcept
{
    return summary.ReservedPhysicalStart + (BootstrapGuardPageOffset * Kernel::Memory::MappingPageSizeBytes);
}

[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapStackPhysicalEnd(const BootstrapMappingPlanSummary& summary) noexcept
{
    return summary.StackPhysicalStart + (BootstrapStackPageCount * Kernel::Memory::MappingPageSizeBytes);
}

[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapPageTableStoragePhysicalEnd(const BootstrapMappingPlanSummary& summary) noexcept
{
    return summary.PageTableStoragePhysicalStart +
           (BootstrapPageTableStoragePageCount * Kernel::Memory::MappingPageSizeBytes);
}

[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapDescriptorTableStoragePhysicalEnd(const BootstrapMappingPlanSummary& summary) noexcept
{
    return summary.DescriptorTableStoragePhysicalStart +
           (BootstrapDescriptorTableStoragePageCount * Kernel::Memory::MappingPageSizeBytes);
}

[[nodiscard]] constexpr Foundation::UInt64
GetBootstrapReservedPhysicalEnd(const BootstrapMappingPlanSummary& summary) noexcept
{
    return summary.ReservedPhysicalStart + (BootstrapReservedPageCount * Kernel::Memory::MappingPageSizeBytes);
}

/// Builds the identity-mapped ownership plan consumed by the AMD64 page-table
/// encoder. The first reserved page remains unmapped below the downward-growing
/// stack; the sixteen page-table pages and two descriptor-table pages are
/// mapped read/write and NX. This boundary neither writes reserved memory nor
/// performs privileged activation.
///
/// Every input and both outputs must denote distinct stable objects throughout
/// the call. The caller additionally proves that alternate virtual aliases do
/// not make either output share backing storage with an input or reserved page.
/// The summary remains unchanged on every failure. Public validation failures
/// also leave a fresh mapping plan unchanged. An internal-invariant failure may
/// leave a terminal, unsealed partial plan that must not be reused.
class BootstrapMappingPlanBuilder final
{
public:
    /// Revalidates the immutable inputs without constructing another plan.
    /// outputSummary is assigned only after every validation succeeds.
    [[nodiscard]] static BootstrapMappingPlanError Validate(const Firmware::Amd64LoadedImageLayout& image,
                                                            const Boot::BootInfo& finalBootInfo,
                                                            const Kernel::Memory::PhysicalPageRange& reservedPages,
                                                            BootstrapMappingPlanSummary& outputSummary) noexcept;

    [[nodiscard]] static BootstrapMappingPlanError Build(const Firmware::Amd64LoadedImageLayout& image,
                                                         const Boot::BootInfo& finalBootInfo,
                                                         const Kernel::Memory::PhysicalPageRange& reservedPages,
                                                         Kernel::Memory::InitialMappingPlan& outputPlan,
                                                         BootstrapMappingPlanSummary& outputSummary) noexcept;

private:
    BootstrapMappingPlanBuilder() = delete;
};

using BootstrapMappingPlanBuildFunction = BootstrapMappingPlanError (*)(const Firmware::Amd64LoadedImageLayout&,
                                                                        const Boot::BootInfo&,
                                                                        const Kernel::Memory::PhysicalPageRange&,
                                                                        Kernel::Memory::InitialMappingPlan&,
                                                                        BootstrapMappingPlanSummary&) noexcept;
using BootstrapMappingPlanValidateFunction = BootstrapMappingPlanError (*)(const Firmware::Amd64LoadedImageLayout&,
                                                                           const Boot::BootInfo&,
                                                                           const Kernel::Memory::PhysicalPageRange&,
                                                                           BootstrapMappingPlanSummary&) noexcept;

static_assert(__is_same(decltype(&BootstrapMappingPlanBuilder::Validate), BootstrapMappingPlanValidateFunction));
static_assert(__is_same(decltype(&BootstrapMappingPlanBuilder::Build), BootstrapMappingPlanBuildFunction));
static_assert(sizeof(BootstrapMappingPlanError) == 4U);
static_assert(sizeof(BootstrapMappingPlanSummary) == 40U);
static_assert(alignof(BootstrapMappingPlanSummary) == 8U);
static_assert(__is_aggregate(BootstrapMappingPlanSummary));
static_assert(__is_standard_layout(BootstrapMappingPlanSummary));
static_assert(__is_trivially_copyable(BootstrapMappingPlanSummary));
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, ReservedPhysicalStart) == 0U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, StackPhysicalStart) == 8U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, PageTableStoragePhysicalStart) == 16U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, DescriptorTableStoragePhysicalStart) == 24U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, MappingCount) == 32U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, Reserved16) == 34U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, Reserved32) == 36U);
static_assert(__builtin_offsetof(BootstrapMappingPlanSummary, Reserved32) +
                  sizeof(BootstrapMappingPlanSummary::Reserved32) ==
              sizeof(BootstrapMappingPlanSummary));
} // namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_BOOTSTRAP_MAPPING_PLAN_HPP_INCLUDED
