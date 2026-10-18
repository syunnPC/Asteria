// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_RUNTIME_PAGE_TABLE_MAPPER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_RUNTIME_PAGE_TABLE_MAPPER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"

namespace Asteria::Platform::Amd64::Memory
{
constexpr Foundation::UInt64 MaximumRuntimeMappingPages{512U};
constexpr Foundation::Size MaximumRuntimePageTablePages{Bootstrap::LargeRootArenaPages};

// Ordinary storage is bounded by 64, large by 256.
[[nodiscard]] constexpr bool IsRuntimePageTableCapacity(const Foundation::Size pages) noexcept
{
    return (pages != 0U) && (pages <= Bootstrap::RootArenaPages || pages == Bootstrap::LargeRootArenaPages);
}

enum class RuntimePageTableMapError : Foundation::UInt32
{
    None,
    InvalidMappingPlan,
    MappingTooLarge,
    InvalidCapabilities,
    FourLevelPagingRequired,
    SupervisorWriteProtectRequired,
    PageAttributeTableRequired,
    ExecuteDisableRequired,
    NonCanonicalVirtualAddress,
    PhysicalAddressOutOfRange,
    InvalidStorage,
    MappingAliasesTableStorage,
    InvalidTableEntry,
    MappingAlreadyPresent,
    PhysicalWriteExecuteAlias,
    InsufficientTableStorage,
    /// Unmap: a page of the record has no leaf.
    MappingAbsent,
    /// Unmap: a page's leaf is not the one Map installed for the record.
    MappingMismatch,
};

/// Supplies one existing four-level root and a contiguous arena of page-table
/// pages. Entries addresses the same physical pages beginning at PhysicalStart.
/// Pages in [0, UsedPageCount) belong to the existing tree. The seed tree was
/// accepted by the initial verifier, and every later structural or leaf change
/// comes from a successful Map call. The caller exclusively owns and serializes
/// the complete arena for its lifetime, including the duration of Map.
struct RuntimePageTableStorage
{
    Kernel::Memory::PhysicalAddress PhysicalStart;
    volatile Foundation::UInt64* Entries;
    Foundation::Size PageCapacity;
    Foundation::Size UsedPageCount;
    Foundation::UInt64 Reserved;
};

/// Describes the committed mapping and the exact virtual range that a later
/// AddressSpace layer must invalidate before resuming the faulting thread.
struct RuntimePageTableMapResult
{
    Foundation::Size UsedTablePages;
    Kernel::Memory::PageCount MappedPages;
    Kernel::Memory::VirtualAddress InvalidationStart;
    Foundation::UInt64 Reserved;
};

/// Adds absent 4 KiB leaves to an existing AMD64 four-level page-table tree. Map is a bounded,
/// allocation-free transaction. It first validates the full used arena as one reachable tree with
/// exactly one parent per table page. It then rejects a writable/executable physical alias,
/// validates the full requested range, and verifies table capacity. A user mapping may promote
/// reused supervisor ancestors only during the infallible commit; existing supervisor leaves retain
/// their leaf-level protection. No public failure changes the table arena or output. After
/// preflight there are no remaining fallible steps: newly allocated table pages are cleared before
/// they are linked, leaves are installed, and output is published last. This is an internal
/// encoding primitive, not an authority boundary. The caller supplies an authority-free record
/// validated by AddressSpace, supplies a tree produced by the verified bootstrap builder and prior
/// successful Map calls, enforces system-wide physical cache-alias policy, keeps the arena writable
/// and immovable, keeps output disjoint from the inputs and arena, adopts UsedTablePages before the
/// next operation, and performs the returned translation invalidation before committing the fault
/// claim. Existing mappings are never replaced.
class RuntimePageTableMapper final
{
public:
    [[nodiscard]] static RuntimePageTableMapError Map(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                                      const PagingCapabilities& capabilities,
                                                      const RuntimePageTableStorage& storage,
                                                      RuntimePageTableMapResult& output) noexcept;

    /// Clears the leaves of one record Map installed. After the same preflight as Map, every page
    /// of the record must hold exactly the leaf Map encoded for it, up to the accessed and dirty
    /// bits; then the leaves are cleared and the output names the record's range to invalidate.
    /// Intermediate tables stay, so UsedTablePages is unchanged. No failure changes the arena or
    /// output.
    [[nodiscard]] static RuntimePageTableMapError Unmap(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                                        const PagingCapabilities& capabilities,
                                                        const RuntimePageTableStorage& storage,
                                                        RuntimePageTableMapResult& output) noexcept;

private:
    RuntimePageTableMapper() = delete;
};

static_assert(sizeof(RuntimePageTableMapError) == 4U);
static_assert(sizeof(RuntimePageTableStorage) == 40U);
static_assert(alignof(RuntimePageTableStorage) == 8U);
static_assert(sizeof(RuntimePageTableMapResult) == 32U);
static_assert(alignof(RuntimePageTableMapResult) == 8U);
} // Namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_RUNTIME_PAGE_TABLE_MAPPER_HPP_INCLUDED
