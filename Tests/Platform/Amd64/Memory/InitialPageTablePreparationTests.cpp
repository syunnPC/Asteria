// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialPageTablePreparation.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Platform::Amd64::Memory::InitialPageTableBuildError;
using Platform::Amd64::Memory::InitialPageTablePreparation;
using Platform::Amd64::Memory::InitialPageTablePreparationError;
using Platform::Amd64::Memory::InitialPageTablePreparationStatus;
using Platform::Amd64::Memory::PageTableStorage;
using Platform::Amd64::Memory::PagingCapabilities;

constexpr Foundation::Size EntriesPerPage{Platform::Amd64::Memory::PageTableEntriesPerPage};
constexpr Foundation::Size StoragePages{16U};
constexpr Foundation::Size StorageEntries{StoragePages * EntriesPerPage};
constexpr Foundation::UInt64 StoragePhysical{0x0020'0000U};
constexpr Foundation::UInt64 UpperBase{0xFFFF'8000'0000'0000ULL};

alignas(4096) Foundation::UInt64 g_storageA[StorageEntries]{};
alignas(4096) Foundation::UInt64 g_storageB[StorageEntries]{};
alignas(4096) Foundation::UInt64 g_storageC[StorageEntries]{};
alignas(4096) constinit InitialMappingPlan g_planA;
alignas(4096) constinit InitialMappingPlan g_planB;
alignas(4096) constinit InitialMappingPlan g_smallPlan;
alignas(4096) constinit InitialMappingPlan g_unsealedPlan;
alignas(4096) constinit InitialMappingPlan g_nonCanonicalPlan;
alignas(4096) constinit InitialMappingPlan g_physicalRangePlan;
alignas(4096) constinit InitialMappingPlan g_storageAliasPlan;
alignas(4096) InitialPageTablePreparation g_aliasPreparation;
alignas(4096) PagingCapabilities g_aliasCapabilities{};
alignas(4096) PageTableStorage g_aliasStorageDescriptor{};
struct alignas(4096) AdjacentCapabilitiesStorageFixture final
{
    Foundation::UInt8 Prefix[4096U - sizeof(PagingCapabilities)];
    PagingCapabilities Capabilities;
    Foundation::UInt64 Entries[StorageEntries];
};

AdjacentCapabilitiesStorageFixture g_adjacentCapabilitiesStorage{};
struct alignas(4096) StorageBeforeCapabilitiesFixture final
{
    Foundation::UInt64 Entries[StorageEntries];
    PagingCapabilities Capabilities;
};

StorageBeforeCapabilitiesFixture g_storageBeforeCapabilities{};
static_assert(__builtin_offsetof(AdjacentCapabilitiesStorageFixture, Capabilities) ==
              (4096U - sizeof(PagingCapabilities)));
static_assert(__builtin_offsetof(AdjacentCapabilitiesStorageFixture, Entries) == 4096U);
static_assert(__builtin_offsetof(StorageBeforeCapabilitiesFixture, Capabilities) ==
              sizeof(StorageBeforeCapabilitiesFixture::Entries));

[[nodiscard]] constexpr MappingPermissions ReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read};
}

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return ReadOnly().With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return ReadOnly().With(MappingPermission::Execute);
}

[[nodiscard]] constexpr PagingCapabilities FullCapabilities() noexcept
{
    return PagingCapabilities{
        .PhysicalAddressBits = 48U,
        .FourLevelPagingEnabled = true,
        .ExecuteDisableEnabled = true,
        .SupervisorWriteProtectEnabled = true,
        .GlobalPagesEnabled = true,
        .PatLayoutConfigured = true,
        .Reserved16 = 0U,
        .Reserved = 0U,
    };
}

[[nodiscard]] InitialMapping MakeMapping(const Foundation::UInt64 virtualStart, const Foundation::UInt64 physicalStart,
                                         const Foundation::UInt64 pages, const MappingPermissions permissions,
                                         const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool AddPlanMappings(InitialMappingPlan& plan, const bool reverseOrder,
                                   const Foundation::UInt64 storagePages = StoragePages) noexcept
{
    const InitialMapping code = MakeMapping(UpperBase, 0x0010'0000U, 2U, ReadExecute().With(MappingPermission::Global),
                                            InitialMappingOwner::NucleusCode);
    const InitialMapping stack =
        MakeMapping(UpperBase + 0x4000U, 0x0011'0000U, 2U, ReadWrite(), InitialMappingOwner::NucleusStack);
    const InitialMapping tables =
        MakeMapping(UpperBase + 0x20'0000U, StoragePhysical, storagePages, ReadWrite(), InitialMappingOwner::PageTable);
    if (reverseOrder)
    {
        return (plan.AddMapping(tables) == InitialMappingPlanError::None) &&
               (plan.AddMapping(stack) == InitialMappingPlanError::None) &&
               (plan.AddMapping(code) == InitialMappingPlanError::None);
    }
    return (plan.AddMapping(code) == InitialMappingPlanError::None) &&
           (plan.AddMapping(stack) == InitialMappingPlanError::None) &&
           (plan.AddMapping(tables) == InitialMappingPlanError::None);
}

[[nodiscard]] bool PreparePlans() noexcept
{
    return AddPlanMappings(g_planA, false) && (g_planA.Seal() == InitialMappingPlanError::None) &&
           AddPlanMappings(g_planB, true) && (g_planB.Seal() == InitialMappingPlanError::None) &&
           AddPlanMappings(g_smallPlan, false, 4U) && (g_smallPlan.Seal() == InitialMappingPlanError::None) &&
           AddPlanMappings(g_unsealedPlan, false) && AddPlanMappings(g_nonCanonicalPlan, false) &&
           (g_nonCanonicalPlan.AddMapping(MakeMapping(0x0000'8000'0000'0000ULL, 0x0030'0000U, 1U, ReadOnly(),
                                                      InitialMappingOwner::BootHandoff)) ==
            InitialMappingPlanError::None) &&
           (g_nonCanonicalPlan.Seal() == InitialMappingPlanError::None) &&
           AddPlanMappings(g_physicalRangePlan, false) &&
           (g_physicalRangePlan.AddMapping(MakeMapping(UpperBase + 0x30'0000U, 0x0000'0010'0000'0000ULL, 1U, ReadOnly(),
                                                       InitialMappingOwner::BootHandoff)) ==
            InitialMappingPlanError::None) &&
           (g_physicalRangePlan.Seal() == InitialMappingPlanError::None) &&
           AddPlanMappings(g_storageAliasPlan, false) &&
           (g_storageAliasPlan.AddMapping(MakeMapping(UpperBase + 0x40'0000U, StoragePhysical, StoragePages,
                                                      ReadWrite(), InitialMappingOwner::NucleusWritableData)) ==
            InitialMappingPlanError::None) &&
           (g_storageAliasPlan.Seal() == InitialMappingPlanError::None);
}

[[nodiscard]] PageTableStorage MakeStorage(Foundation::UInt64* const entries,
                                           const Foundation::Size pages = StoragePages) noexcept
{
    return PageTableStorage{
        .PhysicalStart = PhysicalAddress{StoragePhysical},
        .Entries = entries,
        .PageCapacity = pages,
        .Reserved = 0U,
    };
}

void Fill(Foundation::UInt64* const entries, const Foundation::Size count, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        entries[index] = value ^ static_cast<Foundation::UInt64>(index);
    }
}

[[nodiscard]] bool MatchesFill(const Foundation::UInt64* const entries, const Foundation::Size count,
                               const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (entries[index] != (value ^ static_cast<Foundation::UInt64>(index)))
        {
            return false;
        }
    }
    return true;
}

void CopyBytes(const void* const source, Foundation::UInt8* const destination, const Foundation::Size size) noexcept
{
    const auto* const bytes = static_cast<const Foundation::UInt8*>(source);
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        destination[index] = bytes[index];
    }
}

[[nodiscard]] bool BytesEqual(const void* const value, const Foundation::UInt8* const expected,
                              const Foundation::Size size) noexcept
{
    const auto* const bytes = static_cast<const Foundation::UInt8*>(value);
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool MappingsEqual(const InitialMapping& first, const InitialMapping& second) noexcept
{
    return (first.VirtualStart.GetValue() == second.VirtualStart.GetValue()) &&
           (first.PhysicalStart.GetValue() == second.PhysicalStart.GetValue()) &&
           (first.Pages.GetValue() == second.Pages.GetValue()) &&
           (first.Permissions.GetBits() == second.Permissions.GetBits()) && (first.CachePolicy == second.CachePolicy) &&
           (first.Owner == second.Owner) && (first.Reserved == second.Reserved);
}

[[nodiscard]] bool PlansEqual(const InitialMappingPlan& first, const InitialMappingPlan& second) noexcept
{
    if (!first.IsSealed() || !second.IsSealed() || (first.GetMappingCount() != second.GetMappingCount()))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < first.GetMappingCount(); ++index)
    {
        const auto firstMapping = first.GetMapping(index);
        const auto secondMapping = second.GetMapping(index);
        if (!firstMapping.HasValue() || !secondMapping.HasValue() ||
            !MappingsEqual(firstMapping.GetValue(), secondMapping.GetValue()))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool StorageEqual(const Foundation::UInt64* const first, const Foundation::UInt64* const second,
                                const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

void TestSuccessfulPublication(TestContext& runner) noexcept
{
    InitialPageTablePreparation preparation;
    ASTERIA_EXPECT(runner, "fresh preparation exposes no unpublished state", !preparation.IsPrepared(),
                   !preparation.HasFailed(), !preparation.TryGetSummary().HasValue(),
                   preparation.TryGetSummary().GetError() == InitialPageTablePreparationError::NotPrepared,
                   !preparation.TryGetMappingPlan().HasValue(),
                   preparation.TryGetMappingPlan().GetError() == InitialPageTablePreparationError::NotPrepared);

    Foundation::UInt8 planSnapshot[sizeof(g_planA)]{};
    const PagingCapabilities capabilities = FullCapabilities();
    const PageTableStorage storage = MakeStorage(g_storageA);
    Foundation::UInt8 capabilitiesSnapshot[sizeof(capabilities)]{};
    Foundation::UInt8 storageSnapshot[sizeof(storage)]{};
    CopyBytes(&g_planA, planSnapshot, sizeof(planSnapshot));
    CopyBytes(&capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot));
    CopyBytes(&storage, storageSnapshot, sizeof(storageSnapshot));
    Fill(g_storageA, StorageEntries, 0xA55A'9966'3CC3'F00FULL);

    const auto result = preparation.Prepare(g_planA, capabilities, storage);
    const auto summary = preparation.TryGetSummary();
    const auto retainedPlan = preparation.TryGetMappingPlan();
    ASTERIA_EXPECT(runner, "successful preparation publishes summary and retained mapping plan last", result.HasValue(),
                   result.GetValue() == InitialPageTablePreparationStatus::Prepared, preparation.IsPrepared(),
                   !preparation.HasFailed(), summary.HasValue(), retainedPlan.HasValue());
    ASTERIA_EXPECT(runner, "published summary retains exact root, extent, and mapping counts", summary.HasValue(),
                   summary.GetValue()->Storage.RootPhysicalAddress.GetValue() == StoragePhysical,
                   summary.GetValue()->Storage.Entries == g_storageA,
                   summary.GetValue()->Storage.PageCapacity == StoragePages,
                   summary.GetValue()->Storage.UsedTablePages == 5U, summary.GetValue()->MappedPages == 20U,
                   summary.GetValue()->MappingCount == 3U, summary.GetValue()->Reserved16 == 0U,
                   summary.GetValue()->Reserved32 == 0U);
    ASTERIA_EXPECT(runner, "preparation owns an exact sealed copy instead of borrowing the source plan",
                   retainedPlan.HasValue(), retainedPlan.GetValue() != &g_planA,
                   PlansEqual(*retainedPlan.GetValue(), g_planA));
    ASTERIA_EXPECT(runner, "successful preparation preserves every caller-owned input object",
                   BytesEqual(&g_planA, planSnapshot, sizeof(planSnapshot)),
                   BytesEqual(&capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot)),
                   BytesEqual(&storage, storageSnapshot, sizeof(storageSnapshot)));

    bool unusedPagesZero{true};
    for (Foundation::Size index{5U * EntriesPerPage}; index < StorageEntries; ++index)
    {
        if (g_storageA[index] != 0U)
        {
            unusedPagesZero = false;
        }
    }
    ASTERIA_EXPECT(runner, "successful preparation clears every unused owned table page", unusedPagesZero);

    Foundation::UInt8 tableSnapshot[sizeof(g_storageA)]{};
    CopyBytes(g_storageA, tableSnapshot, sizeof(tableSnapshot));
    const auto second = preparation.Prepare(g_planA, capabilities, storage);
    ASTERIA_EXPECT(runner, "a prepared owner rejects reuse without changing published tables", !second.HasValue(),
                   second.GetError() == InitialPageTablePreparationError::AlreadyPrepared,
                   BytesEqual(g_storageA, tableSnapshot, sizeof(tableSnapshot)));
}

void TestDeterministicSnapshot(TestContext& runner) noexcept
{
    InitialPageTablePreparation first;
    InitialPageTablePreparation second;
    Fill(g_storageA, StorageEntries, 0x1111'2222'3333'4444ULL);
    Fill(g_storageB, StorageEntries, 0xFFFF'EEEE'DDDD'CCCCULL);
    const auto firstResult = first.Prepare(g_planA, FullCapabilities(), MakeStorage(g_storageA));
    const auto secondResult = second.Prepare(g_planB, FullCapabilities(), MakeStorage(g_storageB));
    const auto firstPlan = first.TryGetMappingPlan();
    const auto secondPlan = second.TryGetMappingPlan();
    const auto firstSummary = first.TryGetSummary();
    const auto secondSummary = second.TryGetSummary();
    ASTERIA_EXPECT(runner, "insertion-order permutations produce identical snapshots and table bytes",
                   firstResult.HasValue(), secondResult.HasValue(), firstPlan.HasValue(), secondPlan.HasValue(),
                   PlansEqual(*firstPlan.GetValue(), *secondPlan.GetValue()),
                   StorageEqual(g_storageA, g_storageB, StorageEntries));
    ASTERIA_EXPECT(runner, "deterministic summaries differ only in their borrowed storage pointer",
                   firstSummary.HasValue(), secondSummary.HasValue(),
                   firstSummary.GetValue()->Storage.RootPhysicalAddress.GetValue() ==
                       secondSummary.GetValue()->Storage.RootPhysicalAddress.GetValue(),
                   firstSummary.GetValue()->Storage.PageCapacity == secondSummary.GetValue()->Storage.PageCapacity,
                   firstSummary.GetValue()->Storage.UsedTablePages == secondSummary.GetValue()->Storage.UsedTablePages,
                   firstSummary.GetValue()->MappedPages == secondSummary.GetValue()->MappedPages,
                   firstSummary.GetValue()->MappingCount == secondSummary.GetValue()->MappingCount);
}

void CheckBuilderFailureForwarding(TestContext& runner, const InitialMappingPlan& plan,
                                   const PagingCapabilities& capabilities, const PageTableStorage& storage,
                                   const InitialPageTableBuildError expectedBuildError,
                                   const char* const description) noexcept
{
    constexpr Foundation::UInt64 FillValue{0x6A5A'6996'C33C'0FF0ULL};
    Fill(g_storageC, StorageEntries, FillValue);
    Foundation::UInt8 planSnapshot[sizeof(plan)]{};
    Foundation::UInt8 capabilitiesSnapshot[sizeof(capabilities)]{};
    Foundation::UInt8 storageSnapshot[sizeof(storage)]{};
    CopyBytes(&plan, planSnapshot, sizeof(planSnapshot));
    CopyBytes(&capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot));
    CopyBytes(&storage, storageSnapshot, sizeof(storageSnapshot));

    InitialPageTablePreparation preparation;
    const auto result = preparation.Prepare(plan, capabilities, storage);
    const auto summary = preparation.TryGetSummary();
    const auto retainedPlan = preparation.TryGetMappingPlan();
    ASTERIA_EXPECT(
        runner, description, !result.HasValue(),
        result.GetError() == InitialPageTablePreparationError::PageTableBuildRejected, preparation.HasFailed(),
        !preparation.IsPrepared(),
        preparation.GetFailure().Error == InitialPageTablePreparationError::PageTableBuildRejected,
        preparation.GetFailure().BuildError == expectedBuildError,
        preparation.GetFailure().MappingError == InitialMappingPlanError::None, preparation.GetFailure().Reserved == 0U,
        preparation.GetFailure().MappingIndex == 0U, !summary.HasValue(),
        summary.GetError() == InitialPageTablePreparationError::PreviousPreparationFailed, !retainedPlan.HasValue(),
        retainedPlan.GetError() == InitialPageTablePreparationError::PreviousPreparationFailed,
        MatchesFill(g_storageC, StorageEntries, FillValue), BytesEqual(&plan, planSnapshot, sizeof(planSnapshot)),
        BytesEqual(&capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot)),
        BytesEqual(&storage, storageSnapshot, sizeof(storageSnapshot)));
}

void TestTerminalFailures(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 FillValue{0x0123'4567'89AB'CDEFULL};
    Fill(g_storageC, StorageEntries, FillValue);
    InitialPageTablePreparation unsealed;
    const auto unsealedResult = unsealed.Prepare(g_unsealedPlan, FullCapabilities(), MakeStorage(g_storageC));
    ASTERIA_EXPECT(
        runner, "unsealed input forwards the exact builder error without writing storage", !unsealedResult.HasValue(),
        unsealedResult.GetError() == InitialPageTablePreparationError::PageTableBuildRejected, unsealed.HasFailed(),
        unsealed.GetFailure().Error == InitialPageTablePreparationError::PageTableBuildRejected,
        unsealed.GetFailure().BuildError == InitialPageTableBuildError::PlanNotSealed,
        unsealed.GetFailure().MappingError == InitialMappingPlanError::None, unsealed.GetFailure().Reserved == 0U,
        unsealed.GetFailure().MappingIndex == 0U, MatchesFill(g_storageC, StorageEntries, FillValue));

    const PagingCapabilities retryCapabilities = FullCapabilities();
    const PageTableStorage retryStorage = MakeStorage(g_storageC);
    Foundation::UInt8 failedOwnerSnapshot[sizeof(unsealed)]{};
    Foundation::UInt8 retryPlanSnapshot[sizeof(g_planA)]{};
    Foundation::UInt8 retryCapabilitiesSnapshot[sizeof(retryCapabilities)]{};
    Foundation::UInt8 retryStorageSnapshot[sizeof(retryStorage)]{};
    CopyBytes(&unsealed, failedOwnerSnapshot, sizeof(failedOwnerSnapshot));
    CopyBytes(&g_planA, retryPlanSnapshot, sizeof(retryPlanSnapshot));
    CopyBytes(&retryCapabilities, retryCapabilitiesSnapshot, sizeof(retryCapabilitiesSnapshot));
    CopyBytes(&retryStorage, retryStorageSnapshot, sizeof(retryStorageSnapshot));
    const auto retryResult = unsealed.Prepare(g_planA, retryCapabilities, retryStorage);
    ASTERIA_EXPECT(
        runner, "a failed preparation remains terminal and retry is byte-inert", !unsealed.TryGetSummary().HasValue(),
        unsealed.TryGetSummary().GetError() == InitialPageTablePreparationError::PreviousPreparationFailed,
        !unsealed.TryGetMappingPlan().HasValue(),
        unsealed.TryGetMappingPlan().GetError() == InitialPageTablePreparationError::PreviousPreparationFailed,
        !retryResult.HasValue(), retryResult.GetError() == InitialPageTablePreparationError::PreviousPreparationFailed,
        BytesEqual(&unsealed, failedOwnerSnapshot, sizeof(failedOwnerSnapshot)),
        BytesEqual(&g_planA, retryPlanSnapshot, sizeof(retryPlanSnapshot)),
        BytesEqual(&retryCapabilities, retryCapabilitiesSnapshot, sizeof(retryCapabilitiesSnapshot)),
        BytesEqual(&retryStorage, retryStorageSnapshot, sizeof(retryStorageSnapshot)),
        MatchesFill(g_storageC, StorageEntries, FillValue));

    PagingCapabilities capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 31U;
    CheckBuilderFailureForwarding(runner, g_planA, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::InvalidCapabilities,
                                  "invalid physical width is forwarded exactly without side effects");
    capabilities = FullCapabilities();
    capabilities.Reserved16 = 1U;
    CheckBuilderFailureForwarding(runner, g_planA, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::InvalidCapabilities,
                                  "reserved capability state is forwarded exactly without side effects");
    capabilities = FullCapabilities();
    capabilities.FourLevelPagingEnabled = false;
    CheckBuilderFailureForwarding(runner, g_planA, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::FourLevelPagingRequired,
                                  "four-level paging failure is forwarded exactly without side effects");
    capabilities = FullCapabilities();
    capabilities.SupervisorWriteProtectEnabled = false;
    CheckBuilderFailureForwarding(runner, g_planA, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::SupervisorWriteProtectRequired,
                                  "write-protect failure is forwarded exactly without side effects");
    capabilities = FullCapabilities();
    capabilities.PatLayoutConfigured = false;
    CheckBuilderFailureForwarding(runner, g_planA, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::PageAttributeTableRequired,
                                  "PAT-layout failure is forwarded exactly without side effects");
    capabilities = FullCapabilities();
    capabilities.ExecuteDisableEnabled = false;
    CheckBuilderFailureForwarding(runner, g_planA, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::ExecuteDisableRequired,
                                  "execute-disable failure is forwarded exactly without side effects");
    capabilities = FullCapabilities();
    capabilities.GlobalPagesEnabled = false;
    CheckBuilderFailureForwarding(runner, g_planA, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::GlobalPagesRequired,
                                  "global-page failure is forwarded exactly without side effects");

    CheckBuilderFailureForwarding(runner, g_nonCanonicalPlan, FullCapabilities(), MakeStorage(g_storageC),
                                  InitialPageTableBuildError::NonCanonicalVirtualAddress,
                                  "noncanonical mapping failure is forwarded exactly without side effects");

    capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 36U;
    CheckBuilderFailureForwarding(runner, g_physicalRangePlan, capabilities, MakeStorage(g_storageC),
                                  InitialPageTableBuildError::PhysicalAddressOutOfRange,
                                  "physical-width mapping failure is forwarded exactly without side effects");

    PageTableStorage unownedStorage = MakeStorage(g_storageC);
    unownedStorage.PhysicalStart = PhysicalAddress{0x0050'0000U};
    CheckBuilderFailureForwarding(runner, g_planA, FullCapabilities(), unownedStorage,
                                  InitialPageTableBuildError::PageTableStorageNotOwned,
                                  "unowned table extent failure is forwarded exactly without side effects");

    CheckBuilderFailureForwarding(runner, g_storageAliasPlan, FullCapabilities(), MakeStorage(g_storageC),
                                  InitialPageTableBuildError::PageTableStorageAlias,
                                  "physical table alias failure is forwarded exactly without side effects");

    CheckBuilderFailureForwarding(runner, g_smallPlan, FullCapabilities(), MakeStorage(g_storageC, 4U),
                                  InitialPageTableBuildError::InsufficientTableStorage,
                                  "insufficient storage is measured before commit and forwarded exactly");

    PageTableStorage invalidStorage = MakeStorage(g_storageC, 2U);
    invalidStorage.Reserved = 1U;
    CheckBuilderFailureForwarding(runner, g_planA, FullCapabilities(), invalidStorage,
                                  InitialPageTableBuildError::InvalidStorage,
                                  "invalid storage metadata is forwarded exactly without side effects");

    InitialPageTablePreparation wrapping;
    PageTableStorage wrappingStorage = MakeStorage(g_storageC, 2U);
    wrappingStorage.Entries =
        reinterpret_cast<Foundation::UInt64*>(~Foundation::UIntPointer{0U} - Foundation::UIntPointer{0x0FFFU});
    const auto wrappingResult = wrapping.Prepare(g_planA, FullCapabilities(), wrappingStorage);
    ASTERIA_EXPECT(runner, "a wrapping writable view is rejected before dereference", !wrappingResult.HasValue(),
                   wrapping.GetFailure().BuildError == InitialPageTableBuildError::InvalidStorage);
}

void CheckAliasRejection(TestContext& runner, const InitialMappingPlan& plan, const PagingCapabilities& capabilities,
                         const PageTableStorage& storage, const char* const description) noexcept
{
    Foundation::UInt8 ownerSnapshot[sizeof(g_aliasPreparation)]{};
    Foundation::UInt8 planSnapshot[sizeof(plan)]{};
    Foundation::UInt8 capabilitiesSnapshot[sizeof(capabilities)]{};
    Foundation::UInt8 storageSnapshot[sizeof(storage)]{};
    CopyBytes(&g_aliasPreparation, ownerSnapshot, sizeof(ownerSnapshot));
    CopyBytes(&plan, planSnapshot, sizeof(planSnapshot));
    CopyBytes(&capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot));
    CopyBytes(&storage, storageSnapshot, sizeof(storageSnapshot));
    const auto result = g_aliasPreparation.Prepare(plan, capabilities, storage);
    ASTERIA_EXPECT(runner, description, !result.HasValue(),
                   result.GetError() == InitialPageTablePreparationError::StorageAliasesPreparation,
                   !g_aliasPreparation.IsPrepared(), !g_aliasPreparation.HasFailed(),
                   BytesEqual(&g_aliasPreparation, ownerSnapshot, sizeof(ownerSnapshot)),
                   BytesEqual(&plan, planSnapshot, sizeof(planSnapshot)),
                   BytesEqual(&capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot)),
                   BytesEqual(&storage, storageSnapshot, sizeof(storageSnapshot)));
}

void TestAliasPreflight(TestContext& runner) noexcept
{
    PageTableStorage ownerAlias = MakeStorage(reinterpret_cast<Foundation::UInt64*>(&g_aliasPreparation), 1U);
    CheckAliasRejection(runner, g_planA, FullCapabilities(), ownerAlias,
                        "storage overlapping its preparation owner is rejected while still fresh");

    PageTableStorage planAlias = MakeStorage(reinterpret_cast<Foundation::UInt64*>(&g_planA), 3U);
    CheckAliasRejection(runner, g_planA, FullCapabilities(), planAlias,
                        "storage overlapping the source plan is rejected while still fresh");

    g_aliasCapabilities = FullCapabilities();
    const Foundation::UIntPointer capabilitiesAddress = reinterpret_cast<Foundation::UIntPointer>(&g_aliasCapabilities);
    PageTableStorage capabilitiesAlias = MakeStorage(reinterpret_cast<Foundation::UInt64*>(capabilitiesAddress), 1U);
    CheckAliasRejection(runner, g_planA, g_aliasCapabilities, capabilitiesAlias,
                        "storage overlapping capabilities is rejected while still fresh");

    g_aliasStorageDescriptor = MakeStorage(g_storageC, 1U);
    g_aliasStorageDescriptor.Entries = reinterpret_cast<Foundation::UInt64*>(&g_aliasStorageDescriptor);
    CheckAliasRejection(runner, g_planA, FullCapabilities(), g_aliasStorageDescriptor,
                        "storage overlapping its descriptor is rejected while still fresh");

    PageTableStorage ownerTailAlias = MakeStorage(
        reinterpret_cast<Foundation::UInt64*>(reinterpret_cast<Foundation::UIntPointer>(&g_aliasPreparation) + 8192U),
        1U);
    CheckAliasRejection(runner, g_planA, FullCapabilities(), ownerTailAlias,
                        "storage overlapping only the owner's final page is rejected while fresh");

    PageTableStorage planTailAlias = MakeStorage(
        reinterpret_cast<Foundation::UInt64*>(reinterpret_cast<Foundation::UIntPointer>(&g_planA) + 8192U), 1U);
    CheckAliasRejection(runner, g_planA, FullCapabilities(), planTailAlias,
                        "storage overlapping only the plan's final page is rejected while fresh");

    const auto retry = g_aliasPreparation.Prepare(g_planA, FullCapabilities(), MakeStorage(g_storageC));
    ASTERIA_EXPECT(runner, "pure alias rejections leave the owner eligible for one valid attempt", retry.HasValue(),
                   g_aliasPreparation.IsPrepared());
}

void TestHalfOpenAdjacency(TestContext& runner) noexcept
{
    g_adjacentCapabilitiesStorage.Capabilities = FullCapabilities();
    Fill(g_adjacentCapabilitiesStorage.Entries, StorageEntries, 0x0F0F'F0F0'55AA'AA55ULL);
    Foundation::UInt8 capabilitiesSnapshot[sizeof(g_adjacentCapabilitiesStorage.Capabilities)]{};
    CopyBytes(&g_adjacentCapabilitiesStorage.Capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot));
    InitialPageTablePreparation preparation;
    const auto result = preparation.Prepare(g_planA, g_adjacentCapabilitiesStorage.Capabilities,
                                            MakeStorage(g_adjacentCapabilitiesStorage.Entries));
    ASTERIA_EXPECT(
        runner, "a table extent beginning exactly after an input object is accepted", result.HasValue(),
        preparation.IsPrepared(),
        BytesEqual(&g_adjacentCapabilitiesStorage.Capabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot)));

    g_storageBeforeCapabilities.Capabilities = FullCapabilities();
    Fill(g_storageBeforeCapabilities.Entries, StorageEntries, 0xF0F0'0F0F'AA55'55AAULL);
    Foundation::UInt8 trailingCapabilitiesSnapshot[sizeof(g_storageBeforeCapabilities.Capabilities)]{};
    CopyBytes(&g_storageBeforeCapabilities.Capabilities, trailingCapabilitiesSnapshot,
              sizeof(trailingCapabilitiesSnapshot));
    InitialPageTablePreparation reversePreparation;
    const auto reverseResult = reversePreparation.Prepare(g_planA, g_storageBeforeCapabilities.Capabilities,
                                                          MakeStorage(g_storageBeforeCapabilities.Entries));
    ASTERIA_EXPECT(runner, "a table extent ending exactly before an input object is accepted", reverseResult.HasValue(),
                   reversePreparation.IsPrepared(),
                   BytesEqual(&g_storageBeforeCapabilities.Capabilities, trailingCapabilitiesSnapshot,
                              sizeof(trailingCapabilitiesSnapshot)));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialPageTablePreparationTests"};
    ASTERIA_EXPECT(runner, "all preparation fixtures build and seal", Asteria::Test::PreparePlans());
    Asteria::Test::TestSuccessfulPublication(runner);
    Asteria::Test::TestDeterministicSnapshot(runner);
    Asteria::Test::TestTerminalFailures(runner);
    Asteria::Test::TestAliasPreflight(runner);
    Asteria::Test::TestHalfOpenAdjacency(runner);
    return runner.Finish();
}
