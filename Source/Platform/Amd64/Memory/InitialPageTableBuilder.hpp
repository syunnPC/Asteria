// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_BUILDER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_BUILDER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"

namespace Asteria::Platform::Amd64::Memory
{
constexpr Foundation::Size PageTableEntriesPerPage{512U};
constexpr Foundation::Size MaximumInitialPageTablePages{4096U};
constexpr Foundation::UInt64 ExpectedInitialPatValue{0x0007'0401'0007'0406ULL};

enum class InitialPageTableBuildError : Foundation::UInt32
{
    None,
    PlanNotSealed,
    InvalidCapabilities,
    FourLevelPagingRequired,
    SupervisorWriteProtectRequired,
    PageAttributeTableRequired,
    InvalidStorage,
    PageTableStorageInputAlias,
    BuildResultAlias,
    NonCanonicalVirtualAddress,
    PhysicalAddressOutOfRange,
    ExecuteDisableRequired,
    GlobalPagesRequired,
    PageTableStorageNotOwned,
    PageTableStorageAlias,
    InsufficientTableStorage,
    InternalInvariantViolation,
};

/// Describes the paging state that the activation adapter has already established
/// or has committed to establish before loading the generated root.
struct PagingCapabilities
{
    Foundation::UInt8 PhysicalAddressBits;
    bool FourLevelPagingEnabled;
    bool ExecuteDisableEnabled;
    bool SupervisorWriteProtectEnabled;
    bool GlobalPagesEnabled;
    bool PatLayoutConfigured;
    Foundation::UInt16 Reserved16;
    Foundation::UInt32 Reserved;
};

/// Supplies a physically contiguous set of page-table pages through an existing
/// bootstrap mapping. Entries addresses that storage; PhysicalStart is the address
/// encoded into native entries. The caller proves that both refer to the same pages
/// and that the writable range does not overlap any input object.
struct PageTableStorage
{
    Kernel::Memory::PhysicalAddress PhysicalStart;
    Foundation::UInt64* Entries;
    Foundation::Size PageCapacity;
    Foundation::UInt64 Reserved;
};

struct InitialPageTableBuildResult
{
    Kernel::Memory::PhysicalAddress RootPhysicalAddress;
    Foundation::Size UsedTablePages;
    Foundation::UInt64 MappedPages;
    Foundation::UInt64 Reserved;
};

/// Encodes a sealed common mapping plan as four-level AMD64 4 KiB page tables.
/// The explicit result object remains unchanged on failure. Its virtual object
/// range is checked for overlap with every input object and with the complete
/// writable table-storage extent. The caller must additionally prove that no
/// distinct virtual alias maps the result object onto those physical pages.
/// Public validation failures leave table storage unchanged. A defensive
/// internal-invariant failure after commit begins clears the owned table extent
/// before returning instead of restoring its prior bytes.
/// Native activation, control-register changes, and TLB invalidation are separate.
class InitialPageTableBuilder final
{
public:
    [[nodiscard]] static InitialPageTableBuildError Build(const Kernel::Memory::InitialMappingPlan& plan,
                                                          const PagingCapabilities& capabilities,
                                                          const PageTableStorage& storage,
                                                          InitialPageTableBuildResult& output) noexcept;

private:
    InitialPageTableBuilder() = delete;
};

using InitialPageTableBuildFunction = InitialPageTableBuildError (*)(const Kernel::Memory::InitialMappingPlan&,
                                                                     const PagingCapabilities&, const PageTableStorage&,
                                                                     InitialPageTableBuildResult&) noexcept;

static_assert(__is_same(decltype(&InitialPageTableBuilder::Build), InitialPageTableBuildFunction));

static_assert(sizeof(PagingCapabilities) == 12U);
static_assert(alignof(PagingCapabilities) == 4U);
static_assert(__is_aggregate(PagingCapabilities));
static_assert(__is_standard_layout(PagingCapabilities));
static_assert(__is_trivially_copyable(PagingCapabilities));
static_assert(__builtin_offsetof(PagingCapabilities, PhysicalAddressBits) == 0U);
static_assert(__builtin_offsetof(PagingCapabilities, FourLevelPagingEnabled) == 1U);
static_assert(__builtin_offsetof(PagingCapabilities, ExecuteDisableEnabled) == 2U);
static_assert(__builtin_offsetof(PagingCapabilities, SupervisorWriteProtectEnabled) == 3U);
static_assert(__builtin_offsetof(PagingCapabilities, GlobalPagesEnabled) == 4U);
static_assert(__builtin_offsetof(PagingCapabilities, PatLayoutConfigured) == 5U);
static_assert(__builtin_offsetof(PagingCapabilities, Reserved16) == 6U);
static_assert(__builtin_offsetof(PagingCapabilities, Reserved) == 8U);
static_assert(__builtin_offsetof(PagingCapabilities, Reserved16) + sizeof(PagingCapabilities::Reserved16) ==
              __builtin_offsetof(PagingCapabilities, Reserved));
static_assert(__builtin_offsetof(PagingCapabilities, Reserved) + sizeof(PagingCapabilities::Reserved) ==
              sizeof(PagingCapabilities));
static_assert(sizeof(PageTableStorage) == 32U);
static_assert(alignof(PageTableStorage) == 8U);
static_assert(__is_aggregate(PageTableStorage));
static_assert(__is_standard_layout(PageTableStorage));
static_assert(__is_trivially_copyable(PageTableStorage));
static_assert(__builtin_offsetof(PageTableStorage, PhysicalStart) == 0U);
static_assert(__builtin_offsetof(PageTableStorage, Entries) == 8U);
static_assert(__builtin_offsetof(PageTableStorage, PageCapacity) == 16U);
static_assert(__builtin_offsetof(PageTableStorage, Reserved) == 24U);
static_assert(__builtin_offsetof(PageTableStorage, Reserved) + sizeof(PageTableStorage::Reserved) ==
              sizeof(PageTableStorage));
static_assert(sizeof(InitialPageTableBuildResult) == 32U);
static_assert(alignof(InitialPageTableBuildResult) == 8U);
static_assert(__is_aggregate(InitialPageTableBuildResult));
static_assert(__is_standard_layout(InitialPageTableBuildResult));
static_assert(__is_trivially_copyable(InitialPageTableBuildResult));
static_assert(__builtin_offsetof(InitialPageTableBuildResult, RootPhysicalAddress) == 0U);
static_assert(__builtin_offsetof(InitialPageTableBuildResult, UsedTablePages) == 8U);
static_assert(__builtin_offsetof(InitialPageTableBuildResult, MappedPages) == 16U);
static_assert(__builtin_offsetof(InitialPageTableBuildResult, Reserved) == 24U);
static_assert(__builtin_offsetof(InitialPageTableBuildResult, Reserved) +
                  sizeof(InitialPageTableBuildResult::Reserved) ==
              sizeof(InitialPageTableBuildResult));
static_assert(sizeof(InitialPageTableBuildError) == 4U);
} // namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_BUILDER_HPP_INCLUDED
