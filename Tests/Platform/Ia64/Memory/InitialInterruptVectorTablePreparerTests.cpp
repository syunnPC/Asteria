// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTablePreparer.hpp"

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
using Platform::Ia64::Memory::BootstrapInitialMappingCount;
using Platform::Ia64::Memory::BootstrapMappingPlanSummary;
using Platform::Ia64::Memory::InitialInterruptVectorTablePreparationError;
using Platform::Ia64::Memory::InitialInterruptVectorTablePreparer;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;

constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 TranslationStoragePhysicalStart{ReservedBase + 0x8000U};
constexpr Foundation::UInt64 TablePhysicalStart{ReservedBase + 0x1'0000U};
constexpr Foundation::UInt64 RsePhysicalStart{ReservedBase + 0x1'8000U};
constexpr Foundation::UInt64 ImageBase{0x0400'0000U};
constexpr Foundation::UInt8 ExpectedBundle[16U]{
    0x11U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U,
};

enum class TableMappingKind : Foundation::UInt8
{
    Valid,
    Missing,
    ConflictingOwner,
    WrongRange,
    TooFewPages,
    TooManyPages,
    MissingGlobal,
    ExtraWrite,
    VirtualAlias,
    PartialFirstPageAlias,
    PartialLastPageAlias,
    ShortPlan,
};

constinit InitialMappingPlan g_validPlan;
constinit InitialMappingPlan g_deterministicPlan;
constinit InitialMappingPlan g_missingPlan;
constinit InitialMappingPlan g_conflictingPlan;
constinit InitialMappingPlan g_wrongRangePlan;
constinit InitialMappingPlan g_tooFewPagesPlan;
constinit InitialMappingPlan g_tooManyPagesPlan;
constinit InitialMappingPlan g_missingGlobalPlan;
constinit InitialMappingPlan g_extraWritePlan;
constinit InitialMappingPlan g_virtualAliasPlan;
constinit InitialMappingPlan g_partialFirstPageAliasPlan;
constinit InitialMappingPlan g_partialLastPageAliasPlan;
constinit InitialMappingPlan g_unsealedPlan;
constinit InitialMappingPlan g_shortPlan;
alignas(32U * 1024U) constinit InitialMappingPlan g_aliasPlan;
alignas(32U * 1024U) Foundation::UInt8 g_storage[InitialInterruptVectorTableSizeBytes];
Foundation::UInt8 g_planSnapshot[sizeof(InitialMappingPlan)];
BootstrapMappingPlanSummary g_summary{
    .StackPhysicalStart = ReservedBase,
    .TranslationStoragePhysicalStart = TranslationStoragePhysicalStart,
    .MappingCount = static_cast<Foundation::UInt16>(BootstrapInitialMappingCount),
    .Reserved16 = 0U,
    .Reserved32 = 0U,
    .RseBackingStorePhysicalStart = ReservedBase + 0x1'8000U,
};
alignas(32U * 1024U) BootstrapMappingPlanSummary g_aliasSummary{
    .StackPhysicalStart = ReservedBase,
    .TranslationStoragePhysicalStart = TranslationStoragePhysicalStart,
    .MappingCount = static_cast<Foundation::UInt16>(BootstrapInitialMappingCount),
    .Reserved16 = 0U,
    .Reserved32 = 0U,
    .RseBackingStorePhysicalStart = ReservedBase + 0x1'8000U,
};

[[nodiscard]] constexpr MappingPermissions ReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Global);
}

[[nodiscard]] constexpr MappingPermissions ReadOnlyWithoutGlobal() noexcept
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

[[nodiscard]] InitialMapping MakeMapping(const Foundation::UInt64 virtualStart, const Foundation::UInt64 physicalStart,
                                         const Foundation::UInt64 pageCount, const MappingPermissions permissions,
                                         const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pageCount},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool Add(InitialMappingPlan& plan, const Foundation::UInt64 start, const Foundation::UInt64 pages,
                       const MappingPermissions permissions, const InitialMappingOwner owner) noexcept
{
    return plan.AddMapping(MakeMapping(start, start, pages, permissions, owner)) == InitialMappingPlanError::None;
}

[[nodiscard]] bool BuildPlan(InitialMappingPlan& plan, const TableMappingKind tableKind,
                             const bool seal = true) noexcept
{
    if (!Add(plan, ReservedBase, 8U, ReadWrite(), InitialMappingOwner::NucleusStack) ||
        !Add(plan, TranslationStoragePhysicalStart, 1U, ReadWrite(), InitialMappingOwner::PageTable) ||
        !Add(plan, ImageBase + 0x1000U, 1U, ReadExecute(), InitialMappingOwner::NucleusCode))
    {
        return false;
    }

    bool includesRse{true};
    if (tableKind != TableMappingKind::Missing)
    {
        Foundation::UInt64 physicalStart{TablePhysicalStart};
        Foundation::UInt64 virtualStart{TablePhysicalStart};
        Foundation::UInt64 pages{8U};
        MappingPermissions permissions{ReadExecute()};
        InitialMappingOwner owner{InitialMappingOwner::NucleusInterruptVectorTable};
        if (tableKind == TableMappingKind::ConflictingOwner)
        {
            owner = InitialMappingOwner::NucleusCode;
        }
        else if (tableKind == TableMappingKind::WrongRange)
        {
            physicalStart = ReservedBase + 0x20'000U;
            virtualStart = physicalStart;
        }
        else if (tableKind == TableMappingKind::TooFewPages)
        {
            pages = 7U;
        }
        else if (tableKind == TableMappingKind::TooManyPages)
        {
            pages = 9U;
            includesRse = false;
        }
        else if (tableKind == TableMappingKind::MissingGlobal)
        {
            permissions = ReadOnlyWithoutGlobal().With(MappingPermission::Execute);
        }
        else if (tableKind == TableMappingKind::ExtraWrite)
        {
            permissions = ReadExecute().With(MappingPermission::Write);
        }
        else if (tableKind == TableMappingKind::VirtualAlias)
        {
            virtualStart += 0x2000'0000U;
        }

        if (plan.AddMapping(MakeMapping(virtualStart, physicalStart, pages, permissions, owner)) !=
            InitialMappingPlanError::None)
        {
            return false;
        }
    }

    if (includesRse && !Add(plan, RsePhysicalStart, 8U, ReadWrite(), InitialMappingOwner::NucleusRegisterBackingStore))
    {
        return false;
    }

    if ((tableKind == TableMappingKind::PartialFirstPageAlias) || (tableKind == TableMappingKind::PartialLastPageAlias))
    {
        const Foundation::UInt64 aliasPhysicalStart =
            tableKind == TableMappingKind::PartialFirstPageAlias ? TablePhysicalStart : TablePhysicalStart + 0x7000U;
        if (plan.AddMapping(MakeMapping(0x0800'0000U, aliasPhysicalStart, 1U, ReadExecute(),
                                        InitialMappingOwner::NucleusCode)) != InitialMappingPlanError::None)
        {
            return false;
        }
    }

    Foundation::Size targetCount{BootstrapInitialMappingCount};
    if (tableKind == TableMappingKind::ShortPlan)
    {
        --targetCount;
    }
    Foundation::Size fillerIndex{0U};
    while (plan.GetMappingCount() < targetCount)
    {
        const Foundation::UInt64 start = ImageBase + 0x2000U + (static_cast<Foundation::UInt64>(fillerIndex) << 12U);
        if (!Add(plan, start, 1U, ReadOnly(), InitialMappingOwner::BootHandoff))
        {
            return false;
        }
        ++fillerIndex;
    }

    return (plan.GetMappingCount() == targetCount) && (!seal || (plan.Seal() == InitialMappingPlanError::None));
}

void FillStorage(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_storage); ++index)
    {
        g_storage[index] = value;
    }
}

[[nodiscard]] bool IsStorageFilled(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_storage); ++index)
    {
        if (g_storage[index] != value)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsFailStopTable() noexcept
{
    for (Foundation::Size offset{0U}; offset < sizeof(g_storage); offset += sizeof(ExpectedBundle))
    {
        for (Foundation::Size index{0U}; index < sizeof(ExpectedBundle); ++index)
        {
            if (g_storage[offset + index] != ExpectedBundle[index])
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] WritableInterruptVectorTableView ValidStorage() noexcept
{
    return WritableInterruptVectorTableView{
        .Data = g_storage,
        .SizeBytes = sizeof(g_storage),
        .PhysicalStart = TablePhysicalStart,
    };
}

void SnapshotPlan() noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&g_validPlan);
    for (Foundation::Size index{0U}; index < sizeof(g_planSnapshot); ++index)
    {
        g_planSnapshot[index] = bytes[index];
    }
}

[[nodiscard]] bool PlanMatchesSnapshot() noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&g_validPlan);
    for (Foundation::Size index{0U}; index < sizeof(g_planSnapshot); ++index)
    {
        if (bytes[index] != g_planSnapshot[index])
        {
            return false;
        }
    }
    return true;
}

void CheckFailure(TestContext& runner, const InitialMappingPlan& plan, const BootstrapMappingPlanSummary& summary,
                  const WritableInterruptVectorTableView storage,
                  const InitialInterruptVectorTablePreparationError expected, const char* const description) noexcept
{
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, description,
                   InitialInterruptVectorTablePreparer::Prepare(plan, summary, storage) == expected,
                   IsStorageFilled(0xA5U));
}

void TestSuccess(TestContext& runner) noexcept
{
    const bool built = BuildPlan(g_validPlan, TableMappingKind::Valid);
    SnapshotPlan();
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "validation accepts the exact table contract without writing", built,
                   InitialInterruptVectorTablePreparer::Validate(g_validPlan, g_summary, ValidStorage()) ==
                       InitialInterruptVectorTablePreparationError::None,
                   IsStorageFilled(0xA5U));
    ASTERIA_EXPECT(runner, "preparation publishes the exact fail-stop bundle in all 2048 slots",
                   InitialInterruptVectorTablePreparer::Prepare(g_validPlan, g_summary, ValidStorage()) ==
                       InitialInterruptVectorTablePreparationError::None,
                   IsFailStopTable());
    ASTERIA_EXPECT(runner, "successful publication leaves both const inputs byte-identical", PlanMatchesSnapshot(),
                   g_summary.StackPhysicalStart == ReservedBase,
                   g_summary.MappingCount == BootstrapInitialMappingCount);

    const bool deterministicBuilt = BuildPlan(g_deterministicPlan, TableMappingKind::Valid);
    FillStorage(0x5AU);
    ASTERIA_EXPECT(runner, "equal inputs deterministically republish the complete table", deterministicBuilt,
                   InitialInterruptVectorTablePreparer::Prepare(g_deterministicPlan, g_summary, ValidStorage()) ==
                       InitialInterruptVectorTablePreparationError::None,
                   IsFailStopTable());
}

void TestStorageAndSummaryFailures(TestContext& runner) noexcept
{
    WritableInterruptVectorTableView storage = ValidStorage();
    storage.Data = nullptr;
    CheckFailure(runner, g_validPlan, g_summary, storage,
                 InitialInterruptVectorTablePreparationError::InvalidStorageView,
                 "a null storage pointer is rejected before writing");

    storage = ValidStorage();
    --storage.SizeBytes;
    CheckFailure(runner, g_validPlan, g_summary, storage,
                 InitialInterruptVectorTablePreparationError::InvalidStorageView,
                 "a short table view is rejected before writing");

    storage = ValidStorage();
    ++storage.SizeBytes;
    CheckFailure(runner, g_validPlan, g_summary, storage,
                 InitialInterruptVectorTablePreparationError::InvalidStorageView,
                 "a long table view is rejected before writing");

    storage = ValidStorage();
    ++storage.Data;
    CheckFailure(runner, g_validPlan, g_summary, storage,
                 InitialInterruptVectorTablePreparationError::InvalidStorageView,
                 "an unaligned table view is rejected before writing");

    storage = ValidStorage();
    storage.Data = reinterpret_cast<Foundation::UInt8*>(~Foundation::UIntPointer{0U} - 0x7FFFU);
    CheckFailure(runner, g_validPlan, g_summary, storage,
                 InitialInterruptVectorTablePreparationError::InvalidStorageView,
                 "a wrapping table view is rejected before writing");

    storage = ValidStorage();
    storage.PhysicalStart += 0x8000U;
    CheckFailure(runner, g_validPlan, g_summary, storage,
                 InitialInterruptVectorTablePreparationError::StorageAddressMismatch,
                 "a mismatched physical identity is rejected before writing");

    BootstrapMappingPlanSummary summary = g_summary;
    summary.StackPhysicalStart = 0U;
    CheckFailure(runner, g_validPlan, summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingSummary,
                 "a zero reserved-range base is rejected before writing");

    summary = g_summary;
    summary.StackPhysicalStart += 0x1000U;
    CheckFailure(runner, g_validPlan, summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingSummary,
                 "a reserved range without 32 KiB alignment is rejected");

    summary = g_summary;
    --summary.MappingCount;
    CheckFailure(runner, g_validPlan, summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingSummary,
                 "a stale mapping count is rejected before writing");

    summary = g_summary;
    summary.Reserved16 = 1U;
    CheckFailure(runner, g_validPlan, summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingSummary,
                 "reserved summary state is rejected before writing");

    summary = g_summary;
    summary.Reserved32 = 1U;
    CheckFailure(runner, g_validPlan, summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingSummary,
                 "the independent reserved summary field is rejected");

    summary = g_summary;
    summary.TranslationStoragePhysicalStart += 0x1000U;
    CheckFailure(runner, g_validPlan, summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingSummary,
                 "a displaced translation-storage range is rejected");
}

void TestPlanAndMappingFailures(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "the unsealed fixture retains the complete mapping count",
                   BuildPlan(g_unsealedPlan, TableMappingKind::Valid, false));
    CheckFailure(runner, g_unsealedPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingPlan,
                 "a complete but unsealed plan is rejected before writing");

    ASTERIA_EXPECT(runner, "the short sealed fixture retains one fewer mapping",
                   BuildPlan(g_shortPlan, TableMappingKind::ShortPlan));
    CheckFailure(runner, g_shortPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidMappingPlan,
                 "a sealed plan with only six entries is rejected");

    ASTERIA_EXPECT(runner, "the missing-owner fixture remains otherwise complete",
                   BuildPlan(g_missingPlan, TableMappingKind::Missing));
    CheckFailure(runner, g_missingPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::MissingTableMapping,
                 "a sealed plan without IVT ownership is rejected atomically");

    ASTERIA_EXPECT(runner, "the conflicting-owner fixture remains sealable",
                   BuildPlan(g_conflictingPlan, TableMappingKind::ConflictingOwner));
    CheckFailure(runner, g_conflictingPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::ConflictingTableMapping,
                 "another owner cannot cover the IVT physical range");

    ASTERIA_EXPECT(runner, "the displaced-owner fixture remains sealable",
                   BuildPlan(g_wrongRangePlan, TableMappingKind::WrongRange));
    CheckFailure(runner, g_wrongRangePlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidTableMapping,
                 "IVT ownership at the wrong range is rejected");

    ASTERIA_EXPECT(runner, "the short-owner fixture remains sealable",
                   BuildPlan(g_tooFewPagesPlan, TableMappingKind::TooFewPages));
    CheckFailure(runner, g_tooFewPagesPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidTableMapping,
                 "an IVT mapping with only seven pages is rejected");

    ASTERIA_EXPECT(runner, "the long-owner fixture remains sealable",
                   BuildPlan(g_tooManyPagesPlan, TableMappingKind::TooManyPages));
    CheckFailure(runner, g_tooManyPagesPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidTableMapping,
                 "an IVT mapping with nine pages is rejected");

    ASTERIA_EXPECT(runner, "the non-Global owner fixture remains sealable",
                   BuildPlan(g_missingGlobalPlan, TableMappingKind::MissingGlobal));
    CheckFailure(runner, g_missingGlobalPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidTableMapping,
                 "an IVT mapping without Global is rejected before writing");

    ASTERIA_EXPECT(runner, "the common plan rejects an IVT mapping with the extra Write permission",
                   !BuildPlan(g_extraWritePlan, TableMappingKind::ExtraWrite));

    ASTERIA_EXPECT(runner, "the nonidentity-owner fixture remains sealable",
                   BuildPlan(g_virtualAliasPlan, TableMappingKind::VirtualAlias));
    CheckFailure(runner, g_virtualAliasPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::InvalidTableMapping,
                 "a nonidentity IVT mapping is rejected before writing");

    ASTERIA_EXPECT(runner, "the first-page physical-alias fixture remains sealable",
                   BuildPlan(g_partialFirstPageAliasPlan, TableMappingKind::PartialFirstPageAlias));
    CheckFailure(runner, g_partialFirstPageAliasPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::ConflictingTableMapping,
                 "a non-IVT owner cannot alias the first IVT page");

    ASTERIA_EXPECT(runner, "the last-page physical-alias fixture remains sealable",
                   BuildPlan(g_partialLastPageAliasPlan, TableMappingKind::PartialLastPageAlias));
    CheckFailure(runner, g_partialLastPageAliasPlan, g_summary, ValidStorage(),
                 InitialInterruptVectorTablePreparationError::ConflictingTableMapping,
                 "a non-IVT owner cannot alias the last IVT page");
}

void TestInputAliases(TestContext& runner) noexcept
{
    const bool aliasPlanBuilt = BuildPlan(g_aliasPlan, TableMappingKind::Valid);
    auto* const planBytes = reinterpret_cast<Foundation::UInt8*>(&g_aliasPlan);
    const WritableInterruptVectorTableView planAlias{
        .Data = planBytes,
        .SizeBytes = InitialInterruptVectorTableSizeBytes,
        .PhysicalStart = TablePhysicalStart,
    };
    ASTERIA_EXPECT(runner, "table storage cannot alias the sealed mapping-plan input", aliasPlanBuilt,
                   InitialInterruptVectorTablePreparer::Prepare(g_aliasPlan, g_summary, planAlias) ==
                       InitialInterruptVectorTablePreparationError::StorageAliasesInput);

    auto* const summaryBytes = reinterpret_cast<Foundation::UInt8*>(&g_aliasSummary);
    const WritableInterruptVectorTableView summaryAlias{
        .Data = summaryBytes,
        .SizeBytes = InitialInterruptVectorTableSizeBytes,
        .PhysicalStart = TablePhysicalStart,
    };
    ASTERIA_EXPECT(runner, "table storage cannot alias the mapping-summary input",
                   InitialInterruptVectorTablePreparer::Prepare(g_validPlan, g_aliasSummary, summaryAlias) ==
                       InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   g_aliasSummary.StackPhysicalStart == ReservedBase);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialInterruptVectorTablePreparerTests"};
    Asteria::Test::TestSuccess(runner);
    Asteria::Test::TestStorageAndSummaryFailures(runner);
    Asteria::Test::TestPlanAndMappingFailures(runner);
    Asteria::Test::TestInputAliases(runner);
    return runner.Finish();
}
