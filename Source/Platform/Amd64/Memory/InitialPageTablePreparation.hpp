// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_PREPARATION_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_PREPARATION_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Amd64::Memory
{
class RuntimeAddressSpacePageTable;

enum class InitialPageTablePreparationStatus : Foundation::UInt32
{
    None,
    Prepared,
};

enum class InitialPageTablePreparationError : Foundation::UInt32
{
    None,
    AlreadyPrepared,
    PreviousPreparationFailed,
    StorageAliasesPreparation,
    MappingSnapshotRejected,
    PageTableBuildRejected,
    InternalInvariantViolation,
    NotPrepared,
};

struct PreparedPageTableStorage
{
    Kernel::Memory::PhysicalAddress RootPhysicalAddress;
    const Foundation::UInt64* Entries;
    Foundation::Size PageCapacity;
    Foundation::Size UsedTablePages;
};

struct InitialPageTablePreparationSummary
{
    PreparedPageTableStorage Storage;
    Foundation::UInt64 MappedPages;
    Foundation::UInt16 MappingCount;
    Foundation::UInt16 Reserved16;
    Foundation::UInt32 Reserved32;
};

struct InitialPageTablePreparationFailure
{
    InitialPageTablePreparationError Error;
    InitialPageTableBuildError BuildError;
    Kernel::Memory::InitialMappingPlanError MappingError;
    Foundation::UInt32 Reserved;
    Foundation::Size MappingIndex;
};

/// Owns an immutable copy of a sealed common mapping plan and publishes the
/// corresponding AMD64 page-table result exactly once. It does not change CR0,
/// CR3, CR4, EFER, PAT, interrupt state, or the active stack.
/// The caller must serialize Prepare and keep every supplied input stable for the duration of the
/// call. On success, the physical pages addressed by Storage must remain reserved, mapped through
/// Entries, and byte-stable until a later native activation boundary replaces or retires the
/// generated root. The physical arena has one preparation owner at a time: another preparation may
/// own the same pages only after this one's runtime backend retired and its AddressSpace quiesced,
/// and this one ended. Once a runtime backend claims this owner, every previously borrowed
/// table-summary view expires and no caller may inspect or mutate the arena outside that backend.
class InitialPageTablePreparation final
{
public:
    constexpr InitialPageTablePreparation() noexcept = default;

    InitialPageTablePreparation(const InitialPageTablePreparation&) = delete;
    InitialPageTablePreparation& operator=(const InitialPageTablePreparation&) = delete;
    InitialPageTablePreparation(InitialPageTablePreparation&&) = delete;
    InitialPageTablePreparation& operator=(InitialPageTablePreparation&&) = delete;

    [[nodiscard]] Foundation::Result<InitialPageTablePreparationStatus, InitialPageTablePreparationError>
    Prepare(const Kernel::Memory::InitialMappingPlan& mappings, const PagingCapabilities& capabilities,
            const PageTableStorage& storage) noexcept;

    [[nodiscard]] bool IsPrepared() const noexcept;
    [[nodiscard]] bool HasFailed() const noexcept;

    /// Returns the published native summary only while the prepared table has
    /// not been transferred to a runtime backend.
    [[nodiscard]] Foundation::Result<const InitialPageTablePreparationSummary*, InitialPageTablePreparationError>
    TryGetSummary() const noexcept;

    /// Returns the retained sealed mapping copy only after successful preparation.
    [[nodiscard]] Foundation::Result<const Kernel::Memory::InitialMappingPlan*, InitialPageTablePreparationError>
    TryGetMappingPlan() const noexcept;

    /// Returns the first detailed terminal failure retained by Prepare.
    /// Preconditions:
    /// - HasFailed is true.
    [[nodiscard]] const InitialPageTablePreparationFailure& GetFailure() const noexcept;

private:
    friend class RuntimeAddressSpacePageTable;

    enum class State : Foundation::UInt64
    {
        Fresh,
        Failed,
        Prepared,
        RuntimeClaimed,
    };

    [[nodiscard]] InitialPageTablePreparationError ClaimRuntimeOwnership() noexcept;
    [[nodiscard]] State LoadState() const noexcept;

    Kernel::Memory::InitialMappingPlan m_mappingPlan{};
    InitialPageTablePreparationSummary m_summary{};
    InitialPageTablePreparationFailure m_failure{};
    Platform::Synchronization::AtomicUInt64 m_state{};
};

static_assert(sizeof(InitialPageTablePreparationStatus) == 4U);
static_assert(sizeof(InitialPageTablePreparationError) == 4U);
static_assert(sizeof(PreparedPageTableStorage) == 32U);
static_assert(alignof(PreparedPageTableStorage) == 8U);
static_assert(__is_aggregate(PreparedPageTableStorage));
static_assert(__is_standard_layout(PreparedPageTableStorage));
static_assert(__is_trivially_copyable(PreparedPageTableStorage));
static_assert(__builtin_offsetof(PreparedPageTableStorage, RootPhysicalAddress) == 0U);
static_assert(__builtin_offsetof(PreparedPageTableStorage, Entries) == 8U);
static_assert(__builtin_offsetof(PreparedPageTableStorage, PageCapacity) == 16U);
static_assert(__builtin_offsetof(PreparedPageTableStorage, UsedTablePages) == 24U);
static_assert(__builtin_offsetof(PreparedPageTableStorage, UsedTablePages) + sizeof(Foundation::Size) ==
              sizeof(PreparedPageTableStorage));
static_assert(sizeof(InitialPageTablePreparationSummary) == 48U);
static_assert(alignof(InitialPageTablePreparationSummary) == 8U);
static_assert(__is_aggregate(InitialPageTablePreparationSummary));
static_assert(__is_standard_layout(InitialPageTablePreparationSummary));
static_assert(__is_trivially_copyable(InitialPageTablePreparationSummary));
static_assert(__builtin_offsetof(InitialPageTablePreparationSummary, Storage) == 0U);
static_assert(__builtin_offsetof(InitialPageTablePreparationSummary, MappedPages) == 32U);
static_assert(__builtin_offsetof(InitialPageTablePreparationSummary, MappingCount) == 40U);
static_assert(__builtin_offsetof(InitialPageTablePreparationSummary, Reserved16) == 42U);
static_assert(__builtin_offsetof(InitialPageTablePreparationSummary, Reserved32) == 44U);
static_assert(__builtin_offsetof(InitialPageTablePreparationSummary, Reserved32) + sizeof(Foundation::UInt32) ==
              sizeof(InitialPageTablePreparationSummary));
static_assert(sizeof(InitialPageTablePreparationFailure) == 24U);
static_assert(alignof(InitialPageTablePreparationFailure) == 8U);
static_assert(__is_aggregate(InitialPageTablePreparationFailure));
static_assert(__is_standard_layout(InitialPageTablePreparationFailure));
static_assert(__is_trivially_copyable(InitialPageTablePreparationFailure));
static_assert(__builtin_offsetof(InitialPageTablePreparationFailure, Error) == 0U);
static_assert(__builtin_offsetof(InitialPageTablePreparationFailure, BuildError) == 4U);
static_assert(__builtin_offsetof(InitialPageTablePreparationFailure, MappingError) == 8U);
static_assert(__builtin_offsetof(InitialPageTablePreparationFailure, Reserved) == 12U);
static_assert(__builtin_offsetof(InitialPageTablePreparationFailure, MappingIndex) == 16U);
static_assert(__builtin_offsetof(InitialPageTablePreparationFailure, MappingIndex) + sizeof(Foundation::Size) ==
              sizeof(InitialPageTablePreparationFailure));
static_assert(sizeof(InitialPageTablePreparation) == 8'288U);
static_assert(alignof(InitialPageTablePreparation) == 8U);
static_assert(sizeof(Foundation::Result<InitialPageTablePreparationStatus, InitialPageTablePreparationError>) == 12U);
static_assert(alignof(Foundation::Result<InitialPageTablePreparationStatus, InitialPageTablePreparationError>) == 4U);
static_assert(sizeof(Foundation::Result<const InitialPageTablePreparationSummary*, InitialPageTablePreparationError>) ==
              16U);
static_assert(
    alignof(Foundation::Result<const InitialPageTablePreparationSummary*, InitialPageTablePreparationError>) == 8U);
} // Namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_PREPARATION_HPP_INCLUDED
