// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <new>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"

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
using Platform::Amd64::Memory::InitialPageTableBuildResult;
using Platform::Amd64::Memory::InitialPageTableBuilder;
using Platform::Amd64::Memory::PageTableStorage;
using Platform::Amd64::Memory::PagingCapabilities;

constexpr Foundation::Size TestStoragePages{32U};
constexpr Foundation::Size EntriesPerPage{Platform::Amd64::Memory::PageTableEntriesPerPage};
constexpr Foundation::UInt64 AddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 WritableBit{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 UserBit{Foundation::UInt64{1U} << 2U};
constexpr Foundation::UInt64 WriteThroughBit{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 CacheDisableBit{Foundation::UInt64{1U} << 4U};
constexpr Foundation::UInt64 PatBit{Foundation::UInt64{1U} << 7U};
constexpr Foundation::UInt64 GlobalBit{Foundation::UInt64{1U} << 8U};
constexpr Foundation::UInt64 ExecuteDisableBit{Foundation::UInt64{1U} << 63U};

alignas(4096) Foundation::UInt64 g_storage[TestStoragePages * EntriesPerPage]{};
alignas(4096) Foundation::UInt64 g_outputAliasStorage[EntriesPerPage]{};
alignas(4096) constinit InitialMappingPlan g_validPlan;
alignas(4096) PagingCapabilities g_aliasCapabilities{};
alignas(4096) PageTableStorage g_aliasStorageDescriptor{};
struct alignas(4096) PlanStartsInsideStorageFixture final
{
    Foundation::UInt8 Prefix[2048U];
    InitialMappingPlan Plan;
};

struct alignas(4096) CapabilitiesCrossStorageBoundaryFixture final
{
    Foundation::UInt8 Prefix[4088U];
    PagingCapabilities Capabilities;
};

struct alignas(4096) DescriptorCrossesStorageBoundaryFixture final
{
    Foundation::UInt8 Prefix[4080U];
    PageTableStorage Storage;
};

struct alignas(4096) OutputBoundaryFixture final
{
    Foundation::UInt64 Prefix[EntriesPerPage];
    Foundation::UInt64 Entries[16U * EntriesPerPage];
};

constinit PlanStartsInsideStorageFixture g_planStartsInsideStorage{};
CapabilitiesCrossStorageBoundaryFixture g_capabilitiesCrossStorageBoundary{};
DescriptorCrossesStorageBoundaryFixture g_descriptorCrossesStorageBoundary{};
OutputBoundaryFixture g_outputCrossingStorage{};
OutputBoundaryFixture g_outputInsideStorage{};
OutputBoundaryFixture g_outputAdjacentStorage{};
static_assert(__builtin_offsetof(OutputBoundaryFixture, Entries) == 4096U);
constinit InitialMappingPlan g_cachePlan;
constinit InitialMappingPlan g_unsealedPlan;
constinit InitialMappingPlan g_nonCanonicalPlan;
constinit InitialMappingPlan g_crossingCanonicalPlan;
constinit InitialMappingPlan g_narrowPhysicalPlan;
constinit InitialMappingPlan g_storageAliasPlan;
constinit InitialMappingPlan g_insufficientPlan;
constinit InitialMappingPlan g_prefixBoundaryPlan;

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

[[nodiscard]] InitialMapping MakeMapping(const Foundation::UInt64 virtualStart, const Foundation::UInt64 physicalStart,
                                         const Foundation::UInt64 pages, const MappingPermissions permissions,
                                         const MemoryCachePolicy cachePolicy, const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = cachePolicy,
        .Owner = owner,
        .Reserved = 0U,
    };
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

[[nodiscard]] PageTableStorage MakeStorage(const Foundation::UInt64 physicalStart,
                                           const Foundation::Size pageCapacity) noexcept
{
    return PageTableStorage{
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Entries = g_storage,
        .PageCapacity = pageCapacity,
        .Reserved = 0U,
    };
}

void FillStorage(const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < (TestStoragePages * EntriesPerPage); ++index)
    {
        g_storage[index] = value ^ static_cast<Foundation::UInt64>(index);
    }
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

[[nodiscard]] bool StorageMatchesFill(const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < (TestStoragePages * EntriesPerPage); ++index)
    {
        if (g_storage[index] != (value ^ static_cast<Foundation::UInt64>(index)))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool StoragePrefixIsZeroAndTailMatchesFill(const Foundation::Size zeroPages,
                                                         const Foundation::UInt64 fillValue) noexcept
{
    const Foundation::Size zeroEntries = zeroPages * EntriesPerPage;
    for (Foundation::Size index{0U}; index < (TestStoragePages * EntriesPerPage); ++index)
    {
        const Foundation::UInt64 expected =
            (index < zeroEntries) ? 0U : (fillValue ^ static_cast<Foundation::UInt64>(index));
        if (g_storage[index] != expected)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr InitialPageTableBuildResult BuildResultSentinel() noexcept
{
    return InitialPageTableBuildResult{
        .RootPhysicalAddress = PhysicalAddress{0x1234'5000U},
        .UsedTablePages = 0x1234U,
        .MappedPages = 0x5566'7788'99AA'BBCCULL,
        .Reserved = 0xCCBB'AA99'8877'6655ULL,
    };
}

[[nodiscard]] bool BuildResultsEqual(const InitialPageTableBuildResult& first,
                                     const InitialPageTableBuildResult& second) noexcept
{
    return (first.RootPhysicalAddress.GetValue() == second.RootPhysicalAddress.GetValue()) &&
           (first.UsedTablePages == second.UsedTablePages) && (first.MappedPages == second.MappedPages) &&
           (first.Reserved == second.Reserved);
}

void CheckFailureUnchanged(TestContext& runner, const InitialMappingPlan& plan, const PagingCapabilities& capabilities,
                           const PageTableStorage& storage, const InitialPageTableBuildError expected,
                           const char* const description) noexcept
{
    constexpr Foundation::UInt64 FillValue{0xA55A'3CC3'9669'F00FULL};
    FillStorage(FillValue);
    InitialPageTableBuildResult output = BuildResultSentinel();
    const InitialPageTableBuildResult expectedOutput = output;
    const InitialPageTableBuildError error = InitialPageTableBuilder::Build(plan, capabilities, storage, output);
    ASTERIA_EXPECT(runner, description, error == expected, BuildResultsEqual(output, expectedOutput),
                   StorageMatchesFill(FillValue));
}

[[nodiscard]] bool GetLeaf(const PageTableStorage& storage, const Foundation::UInt64 virtualAddress,
                           Foundation::UInt64& leaf) noexcept
{
    Foundation::Size tablePage{0U};
    const Foundation::UInt8 shifts[3]{39U, 30U, 21U};
    for (Foundation::Size level{0U}; level < 3U; ++level)
    {
        const Foundation::Size entryIndex = static_cast<Foundation::Size>((virtualAddress >> shifts[level]) & 0x1FFU);
        const Foundation::UInt64 entry = storage.Entries[(tablePage * EntriesPerPage) + entryIndex];
        if ((entry & PresentBit) == 0U)
        {
            return false;
        }
        const Foundation::UInt64 childPhysical = entry & AddressMask;
        if (childPhysical < storage.PhysicalStart.GetValue())
        {
            return false;
        }
        const Foundation::UInt64 pageOffset = (childPhysical - storage.PhysicalStart.GetValue()) >> 12U;
        if (pageOffset >= static_cast<Foundation::UInt64>(storage.PageCapacity))
        {
            return false;
        }
        tablePage = static_cast<Foundation::Size>(pageOffset);
    }
    const Foundation::Size leafIndex = static_cast<Foundation::Size>((virtualAddress >> 12U) & 0x1FFU);
    leaf = storage.Entries[(tablePage * EntriesPerPage) + leafIndex];
    return (leaf & PresentBit) != 0U;
}

[[nodiscard]] bool AddMinimumPlan(InitialMappingPlan& plan, const Foundation::UInt64 base,
                                  const Foundation::UInt64 storageVirtual, const Foundation::UInt64 storagePhysical,
                                  const Foundation::UInt64 storagePages, const bool globalCode) noexcept
{
    MappingPermissions codePermissions = ReadExecute();
    if (globalCode)
    {
        codePermissions = codePermissions.With(MappingPermission::Global);
    }
    return (plan.AddMapping(MakeMapping(base, 0x0010'0000U, 1U, codePermissions, MemoryCachePolicy::WriteBack,
                                        InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeMapping(base + 0x1000U, 0x0011'0000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                        InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeMapping(storageVirtual, storagePhysical, storagePages, ReadWrite(),
                                        MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable)) ==
            InitialMappingPlanError::None);
}

void TestValidBuild(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Base{0xFFFF'8000'0000'0000ULL};
    constexpr Foundation::UInt64 StoragePhysical{0x0020'0000U};
    const bool planReady =
        (g_validPlan.AddMapping(MakeMapping(Base, 0x0010'0000U, 2U, ReadExecute().With(MappingPermission::Global),
                                            MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusCode)) ==
         InitialMappingPlanError::None) &&
        (g_validPlan.AddMapping(MakeMapping(Base + 0x4000U, 0x0011'0000U, 2U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                            InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
        (g_validPlan.AddMapping(MakeMapping(Base + 0x20'0000U, StoragePhysical, 16U, ReadWrite(),
                                            MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable)) ==
         InitialMappingPlanError::None) &&
        (g_validPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "valid high-half input plan seals", planReady);

    const PageTableStorage storage = MakeStorage(StoragePhysical, 16U);
    FillStorage(0xFFFF'FFFF'FFFF'FFFFULL);
    InitialPageTableBuildResult result{};
    const InitialPageTableBuildError error =
        InitialPageTableBuilder::Build(g_validPlan, FullCapabilities(), storage, result);
    ASTERIA_EXPECT(runner, "builder returns exact root, table, and mapped-page counts",
                   error == InitialPageTableBuildError::None, result.RootPhysicalAddress.GetValue() == StoragePhysical,
                   result.UsedTablePages == 5U, result.MappedPages == 20U, result.Reserved == 0U);

    Foundation::UInt64 codeLeaf{0U};
    Foundation::UInt64 stackLeaf{0U};
    Foundation::UInt64 tableLeaf{0U};
    ASTERIA_EXPECT(runner, "nucleus code leaf is supervisor read-execute and global", GetLeaf(storage, Base, codeLeaf),
                   (codeLeaf & AddressMask) == 0x0010'0000U,
                   (codeLeaf & (PresentBit | GlobalBit)) == (PresentBit | GlobalBit),
                   (codeLeaf & (WritableBit | UserBit | ExecuteDisableBit)) == 0U);
    ASTERIA_EXPECT(runner, "nucleus stack leaf is supervisor read-write and non-executable",
                   GetLeaf(storage, Base + 0x4000U, stackLeaf), (stackLeaf & AddressMask) == 0x0011'0000U,
                   (stackLeaf & (PresentBit | WritableBit | ExecuteDisableBit)) ==
                       (PresentBit | WritableBit | ExecuteDisableBit),
                   (stackLeaf & (UserBit | GlobalBit)) == 0U);
    ASTERIA_EXPECT(runner, "page-table storage is itself mapped read-write and non-executable",
                   GetLeaf(storage, Base + 0x20'0000U, tableLeaf), (tableLeaf & AddressMask) == StoragePhysical,
                   (tableLeaf & (PresentBit | WritableBit | ExecuteDisableBit)) ==
                       (PresentBit | WritableBit | ExecuteDisableBit));
    ASTERIA_EXPECT(runner, "upper-half PML4 entry points to the first retained table page",
                   g_storage[256U] == (StoragePhysical + 0x1000U + PresentBit + WritableBit));

    bool unusedPagesZero{true};
    for (Foundation::Size index{5U * EntriesPerPage}; index < (16U * EntriesPerPage); ++index)
    {
        if (g_storage[index] != 0U)
        {
            unusedPagesZero = false;
        }
    }
    ASTERIA_EXPECT(runner, "unused owned table pages are zeroed", unusedPagesZero);
}

void TestCacheEncoding(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Base{0xFFFF'9000'0000'0000ULL};
    constexpr Foundation::UInt64 StoragePhysical{0x0040'0000U};
    const bool planReady =
        AddMinimumPlan(g_cachePlan, Base, Base + 0x20'0000U, StoragePhysical, 16U, false) &&
        (g_cachePlan.AddMapping(
             MakeMapping(Base + 0x2000U, 0x0012'0000U, 1U, ReadExecute(), MemoryCachePolicy::WriteThrough,
                         InitialMappingOwner::FirmwareRuntimeCode)) == InitialMappingPlanError::None) &&
        (g_cachePlan.AddMapping(MakeMapping(Base + 0x3000U, 0x0013'0000U, 1U, ReadWrite(), MemoryCachePolicy::Uncached,
                                            InitialMappingOwner::FirmwareRuntimeData)) ==
         InitialMappingPlanError::None) &&
        (g_cachePlan.AddMapping(MakeMapping(Base + 0x4000U, 0xFEC0'0000U, 1U, ReadWrite(),
                                            MemoryCachePolicy::WriteCombining, InitialMappingOwner::Device)) ==
         InitialMappingPlanError::None) &&
        (g_cachePlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "cache-policy input plan seals", planReady);

    const PageTableStorage storage = MakeStorage(StoragePhysical, 16U);
    InitialPageTableBuildResult result{};
    const InitialPageTableBuildError error =
        InitialPageTableBuilder::Build(g_cachePlan, FullCapabilities(), storage, result);
    Foundation::UInt64 writeThroughLeaf{0U};
    Foundation::UInt64 uncachedLeaf{0U};
    Foundation::UInt64 writeCombiningLeaf{0U};
    ASTERIA_EXPECT(runner, "write-through memory selects PAT index one", error == InitialPageTableBuildError::None,
                   GetLeaf(storage, Base + 0x2000U, writeThroughLeaf),
                   (writeThroughLeaf & (WriteThroughBit | CacheDisableBit | PatBit)) == WriteThroughBit);
    ASTERIA_EXPECT(runner, "uncached memory selects PAT index three", GetLeaf(storage, Base + 0x3000U, uncachedLeaf),
                   (uncachedLeaf & (WriteThroughBit | CacheDisableBit | PatBit)) ==
                       (WriteThroughBit | CacheDisableBit));
    ASTERIA_EXPECT(runner, "write-combining memory selects the configured PAT index four",
                   GetLeaf(storage, Base + 0x4000U, writeCombiningLeaf),
                   (writeCombiningLeaf & (WriteThroughBit | CacheDisableBit | PatBit)) == PatBit);

    PagingCapabilities withoutPat = FullCapabilities();
    withoutPat.PatLayoutConfigured = false;
    CheckFailureUnchanged(runner, g_cachePlan, withoutPat, storage,
                          InitialPageTableBuildError::PageAttributeTableRequired,
                          "cache policy is rejected before output when the exact PAT layout is absent");
}

void TestPlanAndCapabilityFailures(TestContext& runner) noexcept
{
    const PageTableStorage validStorage = MakeStorage(0x0020'0000U, 16U);
    CheckFailureUnchanged(runner, g_unsealedPlan, FullCapabilities(), validStorage,
                          InitialPageTableBuildError::PlanNotSealed,
                          "unsealed plan is rejected without changing output storage");

    PagingCapabilities capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 31U;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::InvalidCapabilities,
                          "physical widths below the AMD64 profile are rejected");
    capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 53U;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::InvalidCapabilities,
                          "physical widths above the architectural entry field are rejected");
    capabilities = FullCapabilities();
    capabilities.Reserved16 = 1U;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::InvalidCapabilities,
                          "nonzero capability reserved padding is rejected");
    capabilities = FullCapabilities();
    capabilities.Reserved = 1U;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::InvalidCapabilities,
                          "nonzero capability reserved state is rejected");
    capabilities = FullCapabilities();
    capabilities.FourLevelPagingEnabled = false;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::FourLevelPagingRequired,
                          "48-bit tables require four-level paging with CR4.LA57 clear");
    capabilities = FullCapabilities();
    capabilities.SupervisorWriteProtectEnabled = false;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::SupervisorWriteProtectRequired,
                          "read-only supervisor mappings require committed CR0.WP state");
    capabilities = FullCapabilities();
    capabilities.ExecuteDisableEnabled = false;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::ExecuteDisableRequired,
                          "non-executable mappings require active EFER.NXE");
    capabilities = FullCapabilities();
    capabilities.GlobalPagesEnabled = false;
    CheckFailureUnchanged(runner, g_validPlan, capabilities, validStorage,
                          InitialPageTableBuildError::GlobalPagesRequired,
                          "global mappings require committed CR4.PGE state");
}

void TestStorageFailures(TestContext& runner) noexcept
{
    PageTableStorage storage = MakeStorage(0x0020'0000U, 16U);
    storage.Entries = nullptr;
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage, InitialPageTableBuildError::InvalidStorage,
                          "null storage view is rejected");
    storage = MakeStorage(0x0020'0000U, 16U);
    storage.Entries = &g_storage[1U];
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage, InitialPageTableBuildError::InvalidStorage,
                          "non-page-aligned storage view is rejected");
    storage = MakeStorage(0x0020'0001U, 16U);
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage, InitialPageTableBuildError::InvalidStorage,
                          "non-page-aligned storage physical address is rejected");
    storage = MakeStorage(0x0020'0000U, 0U);
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage, InitialPageTableBuildError::InvalidStorage,
                          "zero table-page capacity is rejected");
    storage = MakeStorage(0x0020'0000U, Platform::Amd64::Memory::MaximumInitialPageTablePages + 1U);
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage, InitialPageTableBuildError::InvalidStorage,
                          "table-page capacity above the execution bound is rejected");
    storage = MakeStorage(0x0020'0000U, 16U);
    storage.Reserved = 1U;
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage, InitialPageTableBuildError::InvalidStorage,
                          "nonzero storage reserved state is rejected");

    storage = MakeStorage(0x0020'0000U, 2U);
    storage.Entries =
        reinterpret_cast<Foundation::UInt64*>(~Foundation::UIntPointer{0U} - Foundation::UIntPointer{0x0FFFU});
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage, InitialPageTableBuildError::InvalidStorage,
                          "wrapping storage virtual extent is rejected before dereference");

    storage = MakeStorage(0x0050'0000U, 16U);
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage,
                          InitialPageTableBuildError::PageTableStorageNotOwned,
                          "storage outside a PageTable-purpose mapping is rejected");
    storage = MakeStorage(0x0020'0000U, 17U);
    CheckFailureUnchanged(runner, g_validPlan, FullCapabilities(), storage,
                          InitialPageTableBuildError::PageTableStorageNotOwned,
                          "storage extent larger than its PageTable mapping is rejected");
}

void TestInputAliases(TestContext& runner) noexcept
{
    Foundation::UInt8 planSnapshot[sizeof(g_validPlan)]{};
    CopyBytes(&g_validPlan, planSnapshot, sizeof(planSnapshot));
    PageTableStorage planAlias = MakeStorage(0x0020'0000U, 1U);
    planAlias.Entries = reinterpret_cast<Foundation::UInt64*>(&g_validPlan);
    InitialPageTableBuildResult planOutput = BuildResultSentinel();
    const InitialPageTableBuildResult expectedPlanOutput = planOutput;
    const InitialPageTableBuildError planError =
        InitialPageTableBuilder::Build(g_validPlan, FullCapabilities(), planAlias, planOutput);
    ASTERIA_EXPECT(runner, "storage overlapping the sealed mapping plan is rejected without mutation",
                   planError == InitialPageTableBuildError::PageTableStorageInputAlias,
                   BuildResultsEqual(planOutput, expectedPlanOutput),
                   BytesEqual(&g_validPlan, planSnapshot, sizeof(planSnapshot)));

    g_aliasCapabilities = FullCapabilities();
    Foundation::UInt8 capabilitiesSnapshot[sizeof(g_aliasCapabilities)]{};
    CopyBytes(&g_aliasCapabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot));
    PageTableStorage capabilitiesAlias = MakeStorage(0x0020'0000U, 1U);
    const Foundation::UIntPointer capabilitiesAddress = reinterpret_cast<Foundation::UIntPointer>(&g_aliasCapabilities);
    capabilitiesAlias.Entries = reinterpret_cast<Foundation::UInt64*>(capabilitiesAddress);
    InitialPageTableBuildResult capabilitiesOutput = BuildResultSentinel();
    const InitialPageTableBuildResult expectedCapabilitiesOutput = capabilitiesOutput;
    const InitialPageTableBuildError capabilitiesError =
        InitialPageTableBuilder::Build(g_validPlan, g_aliasCapabilities, capabilitiesAlias, capabilitiesOutput);
    ASTERIA_EXPECT(runner, "storage overlapping capabilities is rejected without mutation",
                   capabilitiesError == InitialPageTableBuildError::PageTableStorageInputAlias,
                   BuildResultsEqual(capabilitiesOutput, expectedCapabilitiesOutput),
                   BytesEqual(&g_aliasCapabilities, capabilitiesSnapshot, sizeof(capabilitiesSnapshot)));

    g_aliasStorageDescriptor = MakeStorage(0x0020'0000U, 1U);
    g_aliasStorageDescriptor.Entries = reinterpret_cast<Foundation::UInt64*>(&g_aliasStorageDescriptor);
    Foundation::UInt8 descriptorSnapshot[sizeof(g_aliasStorageDescriptor)]{};
    CopyBytes(&g_aliasStorageDescriptor, descriptorSnapshot, sizeof(descriptorSnapshot));
    InitialPageTableBuildResult descriptorOutput = BuildResultSentinel();
    const InitialPageTableBuildResult expectedDescriptorOutput = descriptorOutput;
    const InitialPageTableBuildError descriptorError =
        InitialPageTableBuilder::Build(g_validPlan, FullCapabilities(), g_aliasStorageDescriptor, descriptorOutput);
    ASTERIA_EXPECT(runner, "storage overlapping its descriptor is rejected without mutation",
                   descriptorError == InitialPageTableBuildError::PageTableStorageInputAlias,
                   BuildResultsEqual(descriptorOutput, expectedDescriptorOutput),
                   BytesEqual(&g_aliasStorageDescriptor, descriptorSnapshot, sizeof(descriptorSnapshot)));

    for (Foundation::Size index{0U}; index < EntriesPerPage; ++index)
    {
        g_outputAliasStorage[index] = 0xA55A'3CC3'9669'F00FULL ^ static_cast<Foundation::UInt64>(index);
    }
    auto* const aliasedOutput =
        ::new (static_cast<void*>(g_outputAliasStorage)) InitialPageTableBuildResult{BuildResultSentinel()};
    Foundation::UInt8 outputAliasSnapshot[sizeof(g_outputAliasStorage)]{};
    CopyBytes(g_outputAliasStorage, outputAliasSnapshot, sizeof(outputAliasSnapshot));
    PageTableStorage outputAlias = MakeStorage(0x0020'0000U, 1U);
    outputAlias.Entries = g_outputAliasStorage;
    const InitialPageTableBuildError outputAliasError =
        InitialPageTableBuilder::Build(g_validPlan, FullCapabilities(), outputAlias, *aliasedOutput);
    ASTERIA_EXPECT(runner, "an explicit result overlapping table storage is rejected before commit",
                   outputAliasError == InitialPageTableBuildError::BuildResultAlias,
                   BytesEqual(g_outputAliasStorage, outputAliasSnapshot, sizeof(outputAliasSnapshot)));
}

void TestBuildResultExtentBoundaries(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 StoragePhysical{0x0020'0000U};
    const auto fillEntries = [](OutputBoundaryFixture& fixture) noexcept
    {
        for (Foundation::Size index{0U}; index < (16U * EntriesPerPage); ++index)
        {
            fixture.Entries[index] = 0xC33C'5AA5'0FF0'9669ULL ^ static_cast<Foundation::UInt64>(index);
        }
    };
    const auto makeStorage = [](OutputBoundaryFixture& fixture) noexcept
    {
        return PageTableStorage{
            .PhysicalStart = PhysicalAddress{StoragePhysical},
            .Entries = fixture.Entries,
            .PageCapacity = 16U,
            .Reserved = 0U,
        };
    };

    fillEntries(g_outputCrossingStorage);
    auto* const crossingOutput =
        ::new (static_cast<void*>(reinterpret_cast<Foundation::UInt8*>(&g_outputCrossingStorage) + 4096U - 16U))
            InitialPageTableBuildResult{BuildResultSentinel()};
    Foundation::UInt8 crossingSnapshot[sizeof(g_outputCrossingStorage)]{};
    CopyBytes(&g_outputCrossingStorage, crossingSnapshot, sizeof(crossingSnapshot));
    const InitialPageTableBuildError crossingError = InitialPageTableBuilder::Build(
        g_validPlan, FullCapabilities(), makeStorage(g_outputCrossingStorage), *crossingOutput);
    ASTERIA_EXPECT(runner, "a result crossing into the beginning of table storage is rejected",
                   crossingError == InitialPageTableBuildError::BuildResultAlias,
                   BytesEqual(&g_outputCrossingStorage, crossingSnapshot, sizeof(crossingSnapshot)));

    fillEntries(g_outputInsideStorage);
    auto* const insideOutput = ::new (static_cast<void*>(&g_outputInsideStorage.Entries[1U]))
        InitialPageTableBuildResult{BuildResultSentinel()};
    Foundation::UInt8 insideSnapshot[sizeof(g_outputInsideStorage)]{};
    CopyBytes(&g_outputInsideStorage, insideSnapshot, sizeof(insideSnapshot));
    const InitialPageTableBuildError insideError = InitialPageTableBuilder::Build(
        g_validPlan, FullCapabilities(), makeStorage(g_outputInsideStorage), *insideOutput);
    ASTERIA_EXPECT(runner, "a result beginning inside table storage is rejected",
                   insideError == InitialPageTableBuildError::BuildResultAlias,
                   BytesEqual(&g_outputInsideStorage, insideSnapshot, sizeof(insideSnapshot)));

    fillEntries(g_outputAdjacentStorage);
    auto* const adjacentOutput = ::new (static_cast<void*>(
        reinterpret_cast<Foundation::UInt8*>(&g_outputAdjacentStorage) + 4096U - sizeof(InitialPageTableBuildResult)))
        InitialPageTableBuildResult{BuildResultSentinel()};
    const InitialPageTableBuildError adjacentError = InitialPageTableBuilder::Build(
        g_validPlan, FullCapabilities(), makeStorage(g_outputAdjacentStorage), *adjacentOutput);
    ASTERIA_EXPECT(runner, "a result ending exactly where table storage begins is accepted",
                   adjacentError == InitialPageTableBuildError::None,
                   adjacentOutput->RootPhysicalAddress.GetValue() == StoragePhysical,
                   adjacentOutput->UsedTablePages == 5U, adjacentOutput->MappedPages == 20U,
                   adjacentOutput->Reserved == 0U);
}

void TestDirectionalInputAliases(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Base{0xFFFF'8800'0000'0000ULL};
    constexpr Foundation::UInt64 StoragePhysical{0x0020'0000U};
    const bool embeddedPlanReady =
        AddMinimumPlan(g_planStartsInsideStorage.Plan, Base, Base + 0x20'0000U, StoragePhysical, 16U, false) &&
        (g_planStartsInsideStorage.Plan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "embedded alias fixture plan seals", embeddedPlanReady);

    Foundation::UInt8 beforePlanSnapshot[sizeof(g_planStartsInsideStorage)]{};
    CopyBytes(&g_planStartsInsideStorage, beforePlanSnapshot, sizeof(beforePlanSnapshot));
    PageTableStorage beforePlanStorage{
        .PhysicalStart = PhysicalAddress{StoragePhysical},
        .Entries = reinterpret_cast<Foundation::UInt64*>(&g_planStartsInsideStorage),
        .PageCapacity = 1U,
        .Reserved = 0U,
    };
    InitialPageTableBuildResult beforePlanOutput = BuildResultSentinel();
    const InitialPageTableBuildResult expectedBeforePlanOutput = beforePlanOutput;
    const InitialPageTableBuildError beforePlanError = InitialPageTableBuilder::Build(
        g_planStartsInsideStorage.Plan, FullCapabilities(), beforePlanStorage, beforePlanOutput);
    ASTERIA_EXPECT(runner, "storage beginning before and ending inside plan is rejected atomically",
                   beforePlanError == InitialPageTableBuildError::PageTableStorageInputAlias,
                   BuildResultsEqual(beforePlanOutput, expectedBeforePlanOutput),
                   BytesEqual(&g_planStartsInsideStorage, beforePlanSnapshot, sizeof(beforePlanSnapshot)));

    Foundation::UInt8 insidePlanSnapshot[sizeof(g_validPlan)]{};
    CopyBytes(&g_validPlan, insidePlanSnapshot, sizeof(insidePlanSnapshot));
    PageTableStorage insidePlanStorage{
        .PhysicalStart = PhysicalAddress{StoragePhysical},
        .Entries =
            reinterpret_cast<Foundation::UInt64*>(reinterpret_cast<Foundation::UIntPointer>(&g_validPlan) + 4096U),
        .PageCapacity = 1U,
        .Reserved = 0U,
    };
    InitialPageTableBuildResult insidePlanOutput = BuildResultSentinel();
    const InitialPageTableBuildResult expectedInsidePlanOutput = insidePlanOutput;
    const InitialPageTableBuildError insidePlanError =
        InitialPageTableBuilder::Build(g_validPlan, FullCapabilities(), insidePlanStorage, insidePlanOutput);
    ASTERIA_EXPECT(runner, "storage beginning inside and ending before plan is rejected atomically",
                   insidePlanError == InitialPageTableBuildError::PageTableStorageInputAlias,
                   BuildResultsEqual(insidePlanOutput, expectedInsidePlanOutput),
                   BytesEqual(&g_validPlan, insidePlanSnapshot, sizeof(insidePlanSnapshot)));

    g_capabilitiesCrossStorageBoundary.Capabilities = FullCapabilities();
    Foundation::UInt8 capabilitiesSnapshot[sizeof(g_capabilitiesCrossStorageBoundary)]{};
    CopyBytes(&g_capabilitiesCrossStorageBoundary, capabilitiesSnapshot, sizeof(capabilitiesSnapshot));
    PageTableStorage insideCapabilitiesStorage{
        .PhysicalStart = PhysicalAddress{StoragePhysical},
        .Entries = reinterpret_cast<Foundation::UInt64*>(
            reinterpret_cast<Foundation::UIntPointer>(&g_capabilitiesCrossStorageBoundary) + 4096U),
        .PageCapacity = 1U,
        .Reserved = 0U,
    };
    InitialPageTableBuildResult insideCapabilitiesOutput = BuildResultSentinel();
    const InitialPageTableBuildResult expectedInsideCapabilitiesOutput = insideCapabilitiesOutput;
    const InitialPageTableBuildError insideCapabilitiesError =
        InitialPageTableBuilder::Build(g_validPlan, g_capabilitiesCrossStorageBoundary.Capabilities,
                                       insideCapabilitiesStorage, insideCapabilitiesOutput);
    ASTERIA_EXPECT(runner, "storage beginning inside capabilities is rejected atomically",
                   insideCapabilitiesError == InitialPageTableBuildError::PageTableStorageInputAlias,
                   BuildResultsEqual(insideCapabilitiesOutput, expectedInsideCapabilitiesOutput),
                   BytesEqual(&g_capabilitiesCrossStorageBoundary, capabilitiesSnapshot, sizeof(capabilitiesSnapshot)));

    auto* const descriptorBase = reinterpret_cast<Foundation::UInt64*>(
        reinterpret_cast<Foundation::UIntPointer>(&g_descriptorCrossesStorageBoundary) + 4096U);
    g_descriptorCrossesStorageBoundary.Storage = PageTableStorage{
        .PhysicalStart = PhysicalAddress{StoragePhysical},
        .Entries = descriptorBase,
        .PageCapacity = 1U,
        .Reserved = 0U,
    };
    Foundation::UInt8 descriptorSnapshot[sizeof(g_descriptorCrossesStorageBoundary)]{};
    CopyBytes(&g_descriptorCrossesStorageBoundary, descriptorSnapshot, sizeof(descriptorSnapshot));
    InitialPageTableBuildResult insideDescriptorOutput = BuildResultSentinel();
    const InitialPageTableBuildResult expectedInsideDescriptorOutput = insideDescriptorOutput;
    const InitialPageTableBuildError insideDescriptorError = InitialPageTableBuilder::Build(
        g_validPlan, FullCapabilities(), g_descriptorCrossesStorageBoundary.Storage, insideDescriptorOutput);
    ASTERIA_EXPECT(runner, "storage beginning inside its descriptor is rejected atomically",
                   insideDescriptorError == InitialPageTableBuildError::PageTableStorageInputAlias,
                   BuildResultsEqual(insideDescriptorOutput, expectedInsideDescriptorOutput),
                   BytesEqual(&g_descriptorCrossesStorageBoundary, descriptorSnapshot, sizeof(descriptorSnapshot)));
}

void TestVirtualExtentBoundaries(TestContext& runner) noexcept
{
    constexpr Foundation::UIntPointer MaximumPointer{~Foundation::UIntPointer{0U}};
    constexpr Foundation::UIntPointer PageMask{Foundation::UIntPointer{0xFFFU}};
    const auto check = [&](const Foundation::Size pages) noexcept
    {
        const Foundation::UIntPointer bytes = static_cast<Foundation::UIntPointer>(pages) << 12U;
        const Foundation::UIntPointer highestAccepted = (MaximumPointer - bytes) & ~PageMask;
        PageTableStorage acceptedBoundary = MakeStorage(0x0050'0000U, pages);
        acceptedBoundary.Entries = reinterpret_cast<Foundation::UInt64*>(highestAccepted);
        InitialPageTableBuildResult acceptedOutput = BuildResultSentinel();
        const InitialPageTableBuildError acceptedError =
            InitialPageTableBuilder::Build(g_validPlan, FullCapabilities(), acceptedBoundary, acceptedOutput);
        ASTERIA_EXPECT(runner, "highest nonwrapping aligned pointer reaches ownership validation",
                       acceptedError == InitialPageTableBuildError::PageTableStorageNotOwned);

        PageTableStorage wrappingBoundary = acceptedBoundary;
        wrappingBoundary.Entries = reinterpret_cast<Foundation::UInt64*>(highestAccepted + 0x1000U);
        InitialPageTableBuildResult wrappingOutput = BuildResultSentinel();
        const InitialPageTableBuildError wrappingError =
            InitialPageTableBuilder::Build(g_validPlan, FullCapabilities(), wrappingBoundary, wrappingOutput);
        ASTERIA_EXPECT(runner, "next aligned pointer is rejected as a wrapping extent",
                       wrappingError == InitialPageTableBuildError::InvalidStorage);
    };

    check(1U);
    check(Platform::Amd64::Memory::MaximumInitialPageTablePages);
}

void TestNativeRangeFailures(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Base{0xFFFF'A000'0000'0000ULL};
    constexpr Foundation::UInt64 StoragePhysical{0x0060'0000U};
    const bool nonCanonicalReady =
        AddMinimumPlan(g_nonCanonicalPlan, Base, Base + 0x20'0000U, StoragePhysical, 16U, false) &&
        (g_nonCanonicalPlan.AddMapping(MakeMapping(0x0000'8000'0000'0000ULL, 0x0030'0000U, 1U, ReadOnly(),
                                                   MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff)) ==
         InitialMappingPlanError::None) &&
        (g_nonCanonicalPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "common plan retains target-neutral virtual shape", nonCanonicalReady);
    CheckFailureUnchanged(runner, g_nonCanonicalPlan, FullCapabilities(), MakeStorage(StoragePhysical, 16U),
                          InitialPageTableBuildError::NonCanonicalVirtualAddress,
                          "first address in the AMD64 canonical hole is rejected");

    const bool crossingReady =
        AddMinimumPlan(g_crossingCanonicalPlan, Base + 0x40'0000U, Base + 0x60'0000U, 0x0070'0000U, 16U, false) &&
        (g_crossingCanonicalPlan.AddMapping(
             MakeMapping(0x0000'7FFF'FFFF'F000ULL, 0x0031'0000U, 2U, ReadOnly(), MemoryCachePolicy::WriteBack,
                         InitialMappingOwner::BootHandoff)) == InitialMappingPlanError::None) &&
        (g_crossingCanonicalPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "common plan retains a range crossing the canonical hole", crossingReady);
    CheckFailureUnchanged(runner, g_crossingCanonicalPlan, FullCapabilities(), MakeStorage(0x0070'0000U, 16U),
                          InitialPageTableBuildError::NonCanonicalVirtualAddress,
                          "range crossing the lower canonical boundary is rejected");

    const bool narrowReady =
        AddMinimumPlan(g_narrowPhysicalPlan, Base + 0x80'0000U, Base + 0xA0'0000U, 0x0080'0000U, 16U, false) &&
        (g_narrowPhysicalPlan.AddMapping(MakeMapping(Base + 0xC0'0000U, 0x0000'0010'0000'0000ULL, 1U, ReadOnly(),
                                                     MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff)) ==
         InitialMappingPlanError::None) &&
        (g_narrowPhysicalPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "common plan retains target-neutral physical width", narrowReady);
    PagingCapabilities narrowCapabilities = FullCapabilities();
    narrowCapabilities.PhysicalAddressBits = 36U;
    CheckFailureUnchanged(runner, g_narrowPhysicalPlan, narrowCapabilities, MakeStorage(0x0080'0000U, 16U),
                          InitialPageTableBuildError::PhysicalAddressOutOfRange,
                          "mapping beyond the implemented physical width is rejected");
}

void TestOwnershipAndCapacity(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Base{0xFFFF'B000'0000'0000ULL};
    constexpr Foundation::UInt64 StoragePhysical{0x0090'0000U};
    const bool aliasReady = AddMinimumPlan(g_storageAliasPlan, Base, Base + 0x20'0000U, StoragePhysical, 16U, false) &&
                            (g_storageAliasPlan.AddMapping(MakeMapping(
                                 Base + 0x40'0000U, StoragePhysical, 16U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                 InitialMappingOwner::NucleusWritableData)) == InitialMappingPlanError::None) &&
                            (g_storageAliasPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "common plan permits exact-protection transitional alias", aliasReady);
    CheckFailureUnchanged(runner, g_storageAliasPlan, FullCapabilities(), MakeStorage(StoragePhysical, 16U),
                          InitialPageTableBuildError::PageTableStorageAlias,
                          "native builder rejects table storage with a non-PageTable purpose alias");

    const bool insufficientReady =
        AddMinimumPlan(g_insufficientPlan, Base + 0x80'0000U, Base + 0xA0'0000U, 0x00A0'0000U, 4U, false) &&
        (g_insufficientPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "four-page storage input plan seals", insufficientReady);
    CheckFailureUnchanged(runner, g_insufficientPlan, FullCapabilities(), MakeStorage(0x00A0'0000U, 4U),
                          InitialPageTableBuildError::InsufficientTableStorage,
                          "exact preflight rejects a fifth required table page before output");
}

void TestPrefixBoundaries(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 StoragePhysical{0x00B0'0000U};
    const bool planReady =
        (g_prefixBoundaryPlan.AddMapping(MakeMapping(0x0000'007F'FFFF'E000ULL, 0x0010'0000U, 1U, ReadExecute(),
                                                     MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusCode)) ==
         InitialMappingPlanError::None) &&
        (g_prefixBoundaryPlan.AddMapping(
             MakeMapping(0x0000'0080'0000'0000ULL, 0x0011'0000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                         InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
        (g_prefixBoundaryPlan.AddMapping(MakeMapping(0xFFFF'8000'0000'0000ULL, StoragePhysical, 16U, ReadWrite(),
                                                     MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable)) ==
         InitialMappingPlanError::None) &&
        (g_prefixBoundaryPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "plan spanning low PML4 entries and upper half seals", planReady);
    InitialPageTableBuildResult result{};
    const InitialPageTableBuildError error = InitialPageTableBuilder::Build(g_prefixBoundaryPlan, FullCapabilities(),
                                                                            MakeStorage(StoragePhysical, 16U), result);
    ASTERIA_EXPECT(runner, "prefix preflight counts three independent four-level branches exactly",
                   error == InitialPageTableBuildError::None, result.UsedTablePages == 10U, result.MappedPages == 18U);
}

void TestInternalInvariantZeroization(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Base{0xFFFF'B100'0000'0000ULL};
    constexpr Foundation::UInt64 StoragePhysical{0x00C0'0000U};
    InitialMappingPlan corruptedPlan;
    const bool planReady = AddMinimumPlan(corruptedPlan, Base, Base + 0x20'0000U, StoragePhysical, 16U, false) &&
                           (corruptedPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "defensive invariant fixture seals before corruption", planReady);
    if (!planReady)
    {
        return;
    }

    auto* const planBytes = reinterpret_cast<Foundation::UInt8*>(&corruptedPlan);
    for (Foundation::Size index{0U}; index < sizeof(Foundation::UInt64); ++index)
    {
        planBytes[sizeof(InitialMapping) + index] = planBytes[index];
    }
    Foundation::UInt8 planSnapshot[sizeof(corruptedPlan)]{};
    CopyBytes(&corruptedPlan, planSnapshot, sizeof(planSnapshot));
    constexpr Foundation::UInt64 FillValue{0x9669'0FF0'A55A'3CC3ULL};
    FillStorage(FillValue);
    InitialPageTableBuildResult output = BuildResultSentinel();
    const InitialPageTableBuildResult expectedOutput = output;
    const PagingCapabilities capabilities = FullCapabilities();
    const PageTableStorage storage = MakeStorage(StoragePhysical, 16U);
    const InitialPageTableBuildError error =
        InitialPageTableBuilder::Build(corruptedPlan, capabilities, storage, output);
    ASTERIA_EXPECT(runner, "a post-commit invariant failure zeroizes storage and preserves output",
                   error == InitialPageTableBuildError::InternalInvariantViolation,
                   BuildResultsEqual(output, expectedOutput), StoragePrefixIsZeroAndTailMatchesFill(16U, FillValue),
                   BytesEqual(&corruptedPlan, planSnapshot, sizeof(planSnapshot)));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialPageTableBuilderTests"};
    Asteria::Test::TestValidBuild(runner);
    Asteria::Test::TestCacheEncoding(runner);
    Asteria::Test::TestPlanAndCapabilityFailures(runner);
    Asteria::Test::TestStorageFailures(runner);
    Asteria::Test::TestInputAliases(runner);
    Asteria::Test::TestBuildResultExtentBoundaries(runner);
    Asteria::Test::TestDirectionalInputAliases(runner);
    Asteria::Test::TestVirtualExtentBoundaries(runner);
    Asteria::Test::TestNativeRangeFailures(runner);
    Asteria::Test::TestOwnershipAndCapacity(runner);
    Asteria::Test::TestPrefixBoundaries(runner);
    Asteria::Test::TestInternalInvariantZeroization(runner);
    return runner.Finish();
}
