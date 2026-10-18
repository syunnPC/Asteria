// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/InitialRseBackingStorePreparer.hpp"

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
using Platform::Ia64::Memory::InitialRseBackingStorePreparationError;
using Platform::Ia64::Memory::InitialRseBackingStorePreparer;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::WritableRseBackingStoreView;

constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 RsePhysicalStart{ReservedBase + 0x1'8000U};
constexpr Foundation::UInt64 ImageBase{0x0400'0000U};

enum class RseMappingKind : Foundation::UInt8
{
    Valid,
    Missing,
    ConflictingOwner,
    WrongRange,
    WrongPageCount,
    TooManyPages,
    MissingGlobal,
    ExtraExecute,
    VirtualAlias,
    ShortPlan,
};

constinit InitialMappingPlan g_validPlan;
constinit InitialMappingPlan g_missingPlan;
constinit InitialMappingPlan g_conflictingPlan;
constinit InitialMappingPlan g_wrongRangePlan;
constinit InitialMappingPlan g_wrongPageCountPlan;
constinit InitialMappingPlan g_tooManyPagesPlan;
constinit InitialMappingPlan g_missingGlobalPlan;
constinit InitialMappingPlan g_extraExecutePlan;
constinit InitialMappingPlan g_virtualAliasPlan;
constinit InitialMappingPlan g_unsealedPlan;
constinit InitialMappingPlan g_completeUnsealedPlan;
constinit InitialMappingPlan g_shortPlan;
alignas(4096) constinit InitialMappingPlan g_aliasPlan;
alignas(4096) Foundation::UInt8 g_storage[InitialRseBackingStoreSizeBytes];
Foundation::UInt8 g_planSnapshot[sizeof(InitialMappingPlan)];
BootstrapMappingPlanSummary g_summary{
    .StackPhysicalStart = ReservedBase,
    .TranslationStoragePhysicalStart = ReservedBase + 0x8000U,
    .MappingCount = static_cast<Foundation::UInt16>(BootstrapInitialMappingCount),
    .Reserved16 = 0U,
    .Reserved32 = 0U,
    .RseBackingStorePhysicalStart = ReservedBase + 0x1'8000U,
};
alignas(4096) BootstrapMappingPlanSummary g_aliasSummary{
    .StackPhysicalStart = ReservedBase,
    .TranslationStoragePhysicalStart = ReservedBase + 0x8000U,
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

[[nodiscard]] InitialMapping MakeMapping(const Foundation::UInt64 start, const Foundation::UInt64 pageCount,
                                         const MappingPermissions permissions, const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{start},
        .PhysicalStart = PhysicalAddress{start},
        .Pages = PageCount{pageCount},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool BuildPlan(InitialMappingPlan& plan, const RseMappingKind rseKind, const bool seal = true) noexcept
{
    if ((plan.AddMapping(MakeMapping(ReservedBase, 8U, ReadWrite(), InitialMappingOwner::NucleusStack)) !=
         InitialMappingPlanError::None) ||
        (plan.AddMapping(MakeMapping(ReservedBase + 0x8000U, 1U, ReadWrite(), InitialMappingOwner::PageTable)) !=
         InitialMappingPlanError::None) ||
        (plan.AddMapping(MakeMapping(ReservedBase + 0x1'0000U, 8U, ReadExecute(),
                                     InitialMappingOwner::NucleusInterruptVectorTable)) !=
         InitialMappingPlanError::None) ||
        (plan.AddMapping(MakeMapping(ImageBase + 0x1000U, 1U, ReadExecute(), InitialMappingOwner::NucleusCode)) !=
         InitialMappingPlanError::None))
    {
        return false;
    }

    if (rseKind != RseMappingKind::Missing)
    {
        Foundation::UInt64 rseStart{RsePhysicalStart};
        InitialMappingOwner owner{InitialMappingOwner::NucleusRegisterBackingStore};
        MappingPermissions permissions{ReadWrite()};
        Foundation::UInt64 virtualStart{rseStart};
        Foundation::UInt64 pageCount{8U};
        if (rseKind == RseMappingKind::ConflictingOwner)
        {
            owner = InitialMappingOwner::NucleusWritableData;
        }
        else if (rseKind == RseMappingKind::WrongRange)
        {
            rseStart += InitialRseBackingStoreSizeBytes;
        }
        else if (rseKind == RseMappingKind::WrongPageCount)
        {
            pageCount = 7U;
        }
        else if (rseKind == RseMappingKind::TooManyPages)
        {
            pageCount = 9U;
        }
        else if (rseKind == RseMappingKind::MissingGlobal)
        {
            permissions = ReadOnlyWithoutGlobal().With(MappingPermission::Write);
        }
        else if (rseKind == RseMappingKind::ExtraExecute)
        {
            permissions = ReadWrite().With(MappingPermission::Execute);
        }
        else if (rseKind == RseMappingKind::VirtualAlias)
        {
            virtualStart += 0x2000'0000U;
        }
        InitialMapping rseMapping = MakeMapping(rseStart, pageCount, permissions, owner);
        rseMapping.VirtualStart = VirtualAddress{virtualStart};
        if (plan.AddMapping(rseMapping) != InitialMappingPlanError::None)
        {
            return false;
        }
    }

    Foundation::Size expectedMappingCount{BootstrapInitialMappingCount};
    if (rseKind == RseMappingKind::ShortPlan)
    {
        --expectedMappingCount;
    }
    Foundation::Size fillerIndex{0U};
    while (plan.GetMappingCount() < expectedMappingCount)
    {
        const Foundation::UInt64 start = ImageBase + 0x2000U + (static_cast<Foundation::UInt64>(fillerIndex) << 12U);
        if (plan.AddMapping(MakeMapping(start, 1U, ReadOnly(), InitialMappingOwner::BootHandoff)) !=
            InitialMappingPlanError::None)
        {
            return false;
        }
        ++fillerIndex;
    }
    return (plan.GetMappingCount() == expectedMappingCount) &&
           (!seal || (plan.Seal() == InitialMappingPlanError::None));
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

[[nodiscard]] WritableRseBackingStoreView ValidStorage() noexcept
{
    return WritableRseBackingStoreView{
        .Data = g_storage,
        .SizeBytes = sizeof(g_storage),
        .PhysicalStart = RsePhysicalStart,
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
        if (g_planSnapshot[index] != bytes[index])
        {
            return false;
        }
    }
    return true;
}

void CheckFailureWithoutWrite(TestContext& runner, const InitialMappingPlan& plan,
                              const BootstrapMappingPlanSummary& summary, const WritableRseBackingStoreView storage,
                              const InitialRseBackingStorePreparationError expected,
                              const char* const description) noexcept
{
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, description, InitialRseBackingStorePreparer::Prepare(plan, summary, storage) == expected,
                   IsStorageFilled(0xA5U));
}

void TestSuccessfulPreparation(TestContext& runner) noexcept
{
    const bool built = BuildPlan(g_validPlan, RseMappingKind::Valid);
    SnapshotPlan();
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "successful validation is pure and leaves all 32 KiB unchanged", built,
                   InitialRseBackingStorePreparer::Validate(g_validPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::None,
                   IsStorageFilled(0xA5U));
    ASTERIA_EXPECT(runner, "the exact sealed plan zeroes all 32 KiB", built,
                   InitialRseBackingStorePreparer::Prepare(g_validPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::None,
                   IsStorageFilled(0U));
    ASTERIA_EXPECT(runner, "successful preparation leaves both const inputs byte-identical", PlanMatchesSnapshot(),
                   g_summary.StackPhysicalStart == ReservedBase,
                   g_summary.MappingCount == BootstrapInitialMappingCount);

    FillStorage(0x5AU);
    ASTERIA_EXPECT(runner, "equal inputs deterministically reinitialize the complete range",
                   InitialRseBackingStorePreparer::Prepare(g_validPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::None,
                   IsStorageFilled(0U));
}

void TestViewValidation(TestContext& runner) noexcept
{
    WritableRseBackingStoreView storage = ValidStorage();
    storage.Data = nullptr;
    CheckFailureWithoutWrite(runner, g_validPlan, g_summary, storage,
                             InitialRseBackingStorePreparationError::InvalidStorageView,
                             "a null storage pointer is rejected before writing");

    storage = ValidStorage();
    --storage.SizeBytes;
    CheckFailureWithoutWrite(runner, g_validPlan, g_summary, storage,
                             InitialRseBackingStorePreparationError::InvalidStorageView,
                             "a short storage view is rejected before writing");

    storage = ValidStorage();
    ++storage.SizeBytes;
    CheckFailureWithoutWrite(runner, g_validPlan, g_summary, storage,
                             InitialRseBackingStorePreparationError::InvalidStorageView,
                             "a long storage view is rejected before writing");

    storage = ValidStorage();
    ++storage.Data;
    CheckFailureWithoutWrite(runner, g_validPlan, g_summary, storage,
                             InitialRseBackingStorePreparationError::InvalidStorageView,
                             "an unaligned storage alias is rejected before writing");

    storage = ValidStorage();
    storage.Data = reinterpret_cast<Foundation::UInt8*>(~Foundation::UIntPointer{0U} &
                                                        ~static_cast<Foundation::UIntPointer>(0xFFFU));
    CheckFailureWithoutWrite(runner, g_validPlan, g_summary, storage,
                             InitialRseBackingStorePreparationError::InvalidStorageView,
                             "a wrapping storage alias is rejected before writing");

    storage = ValidStorage();
    storage.PhysicalStart += 0x1000U;
    CheckFailureWithoutWrite(runner, g_validPlan, g_summary, storage,
                             InitialRseBackingStorePreparationError::StorageAddressMismatch,
                             "a mismatched physical identity is rejected before writing");

    const WritableRseBackingStoreView alias{
        .Data = reinterpret_cast<Foundation::UInt8*>(&g_aliasPlan),
        .SizeBytes = InitialRseBackingStoreSizeBytes,
        .PhysicalStart = RsePhysicalStart,
    };
    const bool aliasPlanBuilt = BuildPlan(g_aliasPlan, RseMappingKind::Valid);
    ASTERIA_EXPECT(runner, "storage cannot alias the sealed mapping-plan input", aliasPlanBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_aliasPlan, g_summary, alias) ==
                       InitialRseBackingStorePreparationError::StorageAliasesInput);

    const WritableRseBackingStoreView summaryAlias{
        .Data = reinterpret_cast<Foundation::UInt8*>(&g_aliasSummary),
        .SizeBytes = InitialRseBackingStoreSizeBytes,
        .PhysicalStart = RsePhysicalStart,
    };
    ASTERIA_EXPECT(runner, "storage cannot alias the mapping-summary input",
                   InitialRseBackingStorePreparer::Prepare(g_validPlan, g_aliasSummary, summaryAlias) ==
                       InitialRseBackingStorePreparationError::StorageAliasesInput);
}

void TestPlanValidation(TestContext& runner) noexcept
{
    CheckFailureWithoutWrite(runner, g_unsealedPlan, g_summary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingPlan,
                             "an unsealed mapping plan is rejected before writing");

    const bool unsealedBuilt = BuildPlan(g_completeUnsealedPlan, RseMappingKind::Valid, false);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "a complete seven-entry plan is rejected solely when it is unsealed", unsealedBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_completeUnsealedPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::InvalidMappingPlan,
                   IsStorageFilled(0xA5U));

    const bool shortPlanBuilt = BuildPlan(g_shortPlan, RseMappingKind::ShortPlan);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "a sealed plan with the RSE owner but only six entries is rejected", shortPlanBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_shortPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::InvalidMappingPlan,
                   IsStorageFilled(0xA5U));

    BootstrapMappingPlanSummary invalidSummary = g_summary;
    --invalidSummary.MappingCount;
    CheckFailureWithoutWrite(runner, g_validPlan, invalidSummary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingSummary,
                             "a mismatched mapping summary is rejected before writing");

    invalidSummary = g_summary;
    invalidSummary.StackPhysicalStart = 0U;
    CheckFailureWithoutWrite(runner, g_validPlan, invalidSummary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingSummary,
                             "a zero reserved-range base is rejected before writing");

    invalidSummary = g_summary;
    invalidSummary.StackPhysicalStart += 0x1000U;
    invalidSummary.TranslationStoragePhysicalStart += 0x1000U;
    CheckFailureWithoutWrite(runner, g_validPlan, invalidSummary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingSummary,
                             "a misaligned reserved range is rejected before writing");

    invalidSummary = g_summary;
    ++invalidSummary.Reserved16;
    CheckFailureWithoutWrite(runner, g_validPlan, invalidSummary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingSummary,
                             "reserved summary bits are rejected before writing");

    invalidSummary = g_summary;
    ++invalidSummary.Reserved32;
    CheckFailureWithoutWrite(runner, g_validPlan, invalidSummary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingSummary,
                             "the independent 32-bit reserved summary field is rejected");

    invalidSummary = g_summary;
    invalidSummary.TranslationStoragePhysicalStart += 0x1000U;
    CheckFailureWithoutWrite(runner, g_validPlan, invalidSummary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingSummary,
                             "a displaced translation-storage range is rejected before writing");

    invalidSummary = g_summary;
    invalidSummary.StackPhysicalStart = 0xFFFF'FFFF'FFFF'8000U;
    invalidSummary.TranslationStoragePhysicalStart = invalidSummary.StackPhysicalStart + 0x8000U;
    CheckFailureWithoutWrite(runner, g_validPlan, invalidSummary, ValidStorage(),
                             InitialRseBackingStorePreparationError::InvalidMappingSummary,
                             "a wrapping reserved range is rejected before writing");

    const bool missingBuilt = BuildPlan(g_missingPlan, RseMappingKind::Missing);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "a sealed plan without RSE ownership is rejected atomically", missingBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_missingPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::MissingBackingStoreMapping,
                   IsStorageFilled(0xA5U));

    const bool conflictBuilt = BuildPlan(g_conflictingPlan, RseMappingKind::ConflictingOwner);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "another owner cannot cover the RSE physical range", conflictBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_conflictingPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::ConflictingBackingStoreMapping,
                   IsStorageFilled(0xA5U));

    const bool wrongRangeBuilt = BuildPlan(g_wrongRangePlan, RseMappingKind::WrongRange);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "RSE ownership at the wrong range is rejected atomically", wrongRangeBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_wrongRangePlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::InvalidBackingStoreMapping,
                   IsStorageFilled(0xA5U));

    const bool wrongPageCountBuilt = BuildPlan(g_wrongPageCountPlan, RseMappingKind::WrongPageCount);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "an RSE owner mapping with only seven pages is rejected", wrongPageCountBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_wrongPageCountPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::InvalidBackingStoreMapping,
                   IsStorageFilled(0xA5U));

    const bool tooManyPagesBuilt = BuildPlan(g_tooManyPagesPlan, RseMappingKind::TooManyPages);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "an RSE owner mapping with nine pages is rejected", tooManyPagesBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_tooManyPagesPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::InvalidBackingStoreMapping,
                   IsStorageFilled(0xA5U));

    const bool missingGlobalBuilt = BuildPlan(g_missingGlobalPlan, RseMappingKind::MissingGlobal);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "an RSE mapping without Global is rejected before writing", missingGlobalBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_missingGlobalPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::InvalidBackingStoreMapping,
                   IsStorageFilled(0xA5U));

    ASTERIA_EXPECT(runner, "the common plan rejects an RSE mapping with the extra Execute permission",
                   !BuildPlan(g_extraExecutePlan, RseMappingKind::ExtraExecute));

    const bool virtualAliasBuilt = BuildPlan(g_virtualAliasPlan, RseMappingKind::VirtualAlias);
    FillStorage(0xA5U);
    ASTERIA_EXPECT(runner, "a nonidentity RSE mapping is rejected before writing", virtualAliasBuilt,
                   InitialRseBackingStorePreparer::Prepare(g_virtualAliasPlan, g_summary, ValidStorage()) ==
                       InitialRseBackingStorePreparationError::InvalidBackingStoreMapping,
                   IsStorageFilled(0xA5U));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialRseBackingStorePreparerTests"};
    Asteria::Test::TestSuccessfulPreparation(runner);
    Asteria::Test::TestViewValidation(runner);
    Asteria::Test::TestPlanValidation(runner);
    return runner.Finish();
}
