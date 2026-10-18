// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"
#include "Platform/Amd64/Memory/InitialPageTableVerifier.hpp"

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
using Platform::Amd64::Memory::BootstrapInitialMappingCount;
using Platform::Amd64::Memory::BootstrapPageTableStoragePageCount;
using Platform::Amd64::Memory::InitialPageTableBuildError;
using Platform::Amd64::Memory::InitialPageTableBuildResult;
using Platform::Amd64::Memory::InitialPageTableBuilder;
using Platform::Amd64::Memory::InitialPageTablePreparationSummary;
using Platform::Amd64::Memory::InitialPageTableVerificationError;
using Platform::Amd64::Memory::InitialPageTableVerifier;
using Platform::Amd64::Memory::PageTableEntriesPerPage;
using Platform::Amd64::Memory::PageTableStorage;
using Platform::Amd64::Memory::PagingCapabilities;
using Platform::Amd64::Memory::PreparedPageTableStorage;

constexpr Foundation::Size StorageEntries{static_cast<Foundation::Size>(BootstrapPageTableStoragePageCount) *
                                          PageTableEntriesPerPage};
constexpr Foundation::UInt64 PresentBit{1U};
constexpr Foundation::UInt64 WritableBit{1U << 1U};
constexpr Foundation::UInt64 ExecuteDisableBit{Foundation::UInt64{1U} << 63U};

alignas(4096) Foundation::UInt64 g_pageTables[StorageEntries]{};
alignas(4096) Foundation::UInt8 g_memoryStack[4U * 4096U]{};
alignas(4096) Foundation::UInt8 g_descriptorTables[2U * 4096U]{};
Foundation::UInt64 g_tableSnapshot[StorageEntries]{};
constinit InitialMappingPlan g_plan;
constinit InitialMappingPlan g_unsealedPlan;

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

[[nodiscard]] InitialMapping Mapping(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                     const MappingPermissions permissions, const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{start},
        .PhysicalStart = PhysicalAddress{start},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool Add(InitialMappingPlan& plan, const InitialMapping& mapping) noexcept
{
    return plan.AddMapping(mapping) == InitialMappingPlanError::None;
}

[[nodiscard]] bool BuildPlan() noexcept
{
    const Foundation::UInt64 stack =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_memoryStack));
    const Foundation::UInt64 tables =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_pageTables));
    const Foundation::UInt64 descriptors =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_descriptorTables));
    return Add(g_plan, Mapping(0x0040'0000U, 2U, ReadExecute(), InitialMappingOwner::NucleusCode)) &&
           Add(g_plan, Mapping(0x0040'2000U, 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(g_plan, Mapping(0x0040'3000U, 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(g_plan, Mapping(0x0040'4000U, 2U, ReadWrite(), InitialMappingOwner::NucleusWritableData)) &&
           Add(g_plan, Mapping(0x0040'6000U, 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(g_plan, Mapping(0x0040'7000U, 1U, ReadOnly(), InitialMappingOwner::NucleusReadOnlyData)) &&
           Add(g_plan, Mapping(0x0040'8000U, 1U, ReadWrite(), InitialMappingOwner::NucleusWritableData)) &&
           Add(g_plan, Mapping(stack, 4U, ReadWrite(), InitialMappingOwner::NucleusStack)) &&
           Add(g_plan,
               Mapping(tables, BootstrapPageTableStoragePageCount, ReadWrite(), InitialMappingOwner::PageTable)) &&
           Add(g_plan, Mapping(descriptors, 2U, ReadWrite(), InitialMappingOwner::NucleusDescriptorTables)) &&
           (g_plan.Seal() == InitialMappingPlanError::None) &&
           (g_plan.GetMappingCount() == BootstrapInitialMappingCount);
}

[[nodiscard]] constexpr PagingCapabilities Capabilities() noexcept
{
    return PagingCapabilities{
        .PhysicalAddressBits = 52U,
        .FourLevelPagingEnabled = true,
        .ExecuteDisableEnabled = true,
        .SupervisorWriteProtectEnabled = true,
        .GlobalPagesEnabled = true,
        .PatLayoutConfigured = true,
        .Reserved16 = 0U,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool TablesMatchSnapshot() noexcept
{
    for (Foundation::Size index{0U}; index < StorageEntries; ++index)
    {
        if (g_pageTables[index] != g_tableSnapshot[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Foundation::Size FindEntry(const Foundation::UInt64 value, const Foundation::Size limit) noexcept
{
    for (Foundation::Size index{0U}; index < limit; ++index)
    {
        if (g_pageTables[index] == value)
        {
            return index;
        }
    }
    return StorageEntries;
}

void RunVerifierChecks(TestContext& runner) noexcept
{
    const PageTableStorage storage{
        .PhysicalStart =
            PhysicalAddress{static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_pageTables))},
        .Entries = g_pageTables,
        .PageCapacity = BootstrapPageTableStoragePageCount,
        .Reserved = 0U,
    };
    InitialPageTableBuildResult built{};
    const InitialPageTableBuildError buildError =
        InitialPageTableBuilder::Build(g_plan, Capabilities(), storage, built);
    InitialPageTablePreparationSummary summary{
        .Storage =
            PreparedPageTableStorage{
                .RootPhysicalAddress = built.RootPhysicalAddress,
                .Entries = g_pageTables,
                .PageCapacity = storage.PageCapacity,
                .UsedTablePages = built.UsedTablePages,
            },
        .MappedPages = built.MappedPages,
        .MappingCount = static_cast<Foundation::UInt16>(g_plan.GetMappingCount()),
        .Reserved16 = 0U,
        .Reserved32 = 0U,
    };
    for (Foundation::Size index{0U}; index < StorageEntries; ++index)
    {
        g_tableSnapshot[index] = g_pageTables[index];
    }

    ASTERIA_EXPECT(runner, "the exact sealed plan and every page-table byte verify read-only",
                   buildError == InitialPageTableBuildError::None, built.MappedPages == 31U,
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::None,
                   TablesMatchSnapshot());

    ASTERIA_EXPECT(runner, "an unsealed plan is rejected",
                   InitialPageTableVerifier::Verify(g_unsealedPlan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::PlanNotSealed);

    PagingCapabilities invalidCapabilities = Capabilities();
    invalidCapabilities.PhysicalAddressBits = 31U;
    ASTERIA_EXPECT(runner, "invalid native capabilities are rejected",
                   InitialPageTableVerifier::Verify(g_plan, invalidCapabilities, summary) ==
                       InitialPageTableVerificationError::InvalidCapabilities);

    InitialPageTablePreparationSummary invalidSummary = summary;
    ++invalidSummary.MappingCount;
    ASTERIA_EXPECT(runner, "a mismatched mapping count is rejected",
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), invalidSummary) ==
                       InitialPageTableVerificationError::InvalidSummary);

    invalidSummary = summary;
    invalidSummary.Storage.RootPhysicalAddress =
        PhysicalAddress{summary.Storage.RootPhysicalAddress.GetValue() + 4096U};
    ASTERIA_EXPECT(runner, "the native root must remain inside its exact owned storage extent",
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), invalidSummary) ==
                       InitialPageTableVerificationError::InvalidStorage);

    Foundation::Size nonzeroRoot{0U};
    while ((nonzeroRoot < PageTableEntriesPerPage) && (g_pageTables[nonzeroRoot] == 0U))
    {
        ++nonzeroRoot;
    }
    if (nonzeroRoot == PageTableEntriesPerPage)
    {
        ASTERIA_EXPECT(runner, "the built root contains a reachable entry", false);
        return;
    }
    const Foundation::UInt64 savedRoot = g_pageTables[nonzeroRoot];
    g_pageTables[nonzeroRoot] |= ExecuteDisableBit;
    ASTERIA_EXPECT(runner, "a non-leaf entry with an unexpected flag is rejected",
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::InvalidNonLeafEntry);
    g_pageTables[nonzeroRoot] = savedRoot;

    const Foundation::Size codeLeaf =
        FindEntry(0x0040'0000U | PresentBit, built.UsedTablePages * PageTableEntriesPerPage);
    if (codeLeaf == StorageEntries)
    {
        ASTERIA_EXPECT(runner, "the built tables contain the code leaf", false);
        return;
    }
    const Foundation::UInt64 savedLeaf = g_pageTables[codeLeaf];
    g_pageTables[codeLeaf] ^= WritableBit;
    ASTERIA_EXPECT(runner, "a leaf permission mutation is rejected",
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::UnexpectedLeafEntry);
    g_pageTables[codeLeaf] = 0U;
    ASTERIA_EXPECT(runner, "a missing leaf is rejected",
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::MissingLeafEntry);
    g_pageTables[codeLeaf] = savedLeaf;

    const Foundation::Size firstUnused = built.UsedTablePages * PageTableEntriesPerPage;
    g_pageTables[firstUnused] = PresentBit | ExecuteDisableBit;
    ASTERIA_EXPECT(runner, "a nonzero byte-equivalent entry in unused storage is rejected",
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::NonZeroUnusedStorage);
    g_pageTables[firstUnused] = 0U;

    invalidSummary = summary;
    ++invalidSummary.Storage.UsedTablePages;
    ASTERIA_EXPECT(runner, "a claimed but unreachable table page is rejected",
                   InitialPageTableVerifier::Verify(g_plan, Capabilities(), invalidSummary) ==
                       InitialPageTableVerificationError::MissingTablePage);

    ASTERIA_EXPECT(runner, "all rejected probes leave every table byte restored", TablesMatchSnapshot());
}
alignas(4096) Foundation::UInt64 g_fullArena[64U * PageTableEntriesPerPage]{};
constinit InitialMappingPlan g_runtimePlan;
void RunRuntimeArenaChecks(TestContext& runner) noexcept
{
    bool added = Add(g_runtimePlan, Mapping(0x1000'0000U, 64U, ReadWrite(), InitialMappingOwner::PageTable));
    for (Foundation::Size index{1U}; index <= 60U; ++index)
    {
        added = added &&
                Add(g_runtimePlan, Mapping(0x1000'0000U + index * 0x20'0000U, 1U,
                                           index == 1U ? ReadExecute() : (index == 2U ? ReadWrite() : ReadOnly()),
                                           index == 1U ? InitialMappingOwner::NucleusCode
                                                       : (index == 2U ? InitialMappingOwner::NucleusStack
                                                                      : InitialMappingOwner::NucleusReadOnlyData)));
    }
    ASTERIA_EXPECT(runner, "runtime seed is independent of the firmware section count", added,
                   g_runtimePlan.Seal() == InitialMappingPlanError::None);
    const PageTableStorage storage{PhysicalAddress{0x1000'0000U}, g_fullArena, 64U, 0U};
    InitialPageTableBuildResult built{};
    ASTERIA_EXPECT(runner, "runtime fixture exercises all 64 reachable table pages",
                   InitialPageTableBuilder::Build(g_runtimePlan, Capabilities(), storage, built) ==
                       InitialPageTableBuildError::None,
                   built.UsedTablePages == 64U);
    InitialPageTablePreparationSummary summary{{built.RootPhysicalAddress, g_fullArena, 64U, built.UsedTablePages},
                                               built.MappedPages,
                                               static_cast<Foundation::UInt16>(g_runtimePlan.GetMappingCount()),
                                               0U,
                                               0U};
    ASTERIA_EXPECT(runner, "full runtime arena verifies without a shift by 64",
                   InitialPageTableVerifier::Verify(g_runtimePlan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::None);
    summary.Storage.PageCapacity = 65U;
    ASTERIA_EXPECT(runner, "oversized arena fails before reading beyond storage",
                   InitialPageTableVerifier::Verify(g_runtimePlan, Capabilities(), summary) ==
                       InitialPageTableVerificationError::InvalidSummary);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialPageTableVerifierTests"};
    ASTERIA_EXPECT(runner, "the verifier fixture seals", Asteria::Test::BuildPlan());
    Asteria::Test::RunVerifierChecks(runner);
    Asteria::Test::RunRuntimeArenaChecks(runner);
    return runner.Finish();
}
