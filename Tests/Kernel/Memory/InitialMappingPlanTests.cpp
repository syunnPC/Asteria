// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"

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

constexpr Foundation::UInt64 MaximumPhysicalPageCount{0x000F'FFFF'FFFF'FFFFULL};

constinit InitialMappingPlan g_validPlan;
constinit InitialMappingPlan g_invalidPlan;
constinit InitialMappingPlan g_overlapPlan;
constinit InitialMappingPlan g_sealPlan;
constinit InitialMappingPlan g_rseSealPlan;
constinit InitialMappingPlan g_ivtCodeSubstitutionPlan;
constinit InitialMappingPlan g_ivtStackSubstitutionPlan;
constinit InitialMappingPlan g_ivtPageTableSubstitutionPlan;
constinit InitialMappingPlan g_capacityPlan;
constinit InitialMappingPlan g_boundaryPlan;

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
                                         const Foundation::UInt64 pageCount, const MappingPermissions permissions,
                                         const MemoryCachePolicy cachePolicy, const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pageCount},
        .Permissions = permissions,
        .CachePolicy = cachePolicy,
        .Owner = owner,
        .Reserved = 0U,
    };
}

void TestValidPlan(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Base{0xFFFF'8000'0000'0000ULL};
    const InitialMapping code =
        MakeMapping(Base + 0x1000U, 0x0010'0000U, 4U, ReadExecute().With(MappingPermission::Global),
                    MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusCode);
    const InitialMapping readOnly = MakeMapping(Base + 0x5000U, 0x0010'5000U, 2U, ReadOnly(),
                                                MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusReadOnlyData);
    const InitialMapping writable = MakeMapping(Base + 0x7000U, 0x0010'7000U, 3U, ReadWrite(),
                                                MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusWritableData);
    const InitialMapping stack = MakeMapping(Base + 0xA000U, 0x0010'A000U, 4U, ReadWrite(),
                                             MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusStack);
    const InitialMapping pageTable = MakeMapping(Base + 0xE000U, 0x0010'E000U, 2U, ReadWrite(),
                                                 MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable);
    const InitialMapping registerBackingStore =
        MakeMapping(Base + 0x1'0000U, 0x0011'0000U, 4U, ReadWrite(), MemoryCachePolicy::WriteBack,
                    InitialMappingOwner::NucleusRegisterBackingStore);
    const InitialMapping interruptVectorTable =
        MakeMapping(Base + 0x1'4000U, 0x0011'4000U, 8U, ReadExecute(), MemoryCachePolicy::WriteBack,
                    InitialMappingOwner::NucleusInterruptVectorTable);

    ASTERIA_EXPECT(runner, "valid nucleus mappings are accepted in arbitrary input order",
                   g_validPlan.AddMapping(writable) == InitialMappingPlanError::None,
                   g_validPlan.AddMapping(code) == InitialMappingPlanError::None,
                   g_validPlan.AddMapping(pageTable) == InitialMappingPlanError::None,
                   g_validPlan.AddMapping(readOnly) == InitialMappingPlanError::None,
                   g_validPlan.AddMapping(stack) == InitialMappingPlanError::None,
                   g_validPlan.AddMapping(registerBackingStore) == InitialMappingPlanError::None,
                   g_validPlan.AddMapping(interruptVectorTable) == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "unsealed plan reports its retained mapping count", g_validPlan.GetMappingCount() == 7U,
                   !g_validPlan.IsSealed());

    bool sorted{true};
    Foundation::UInt64 previous{0U};
    for (Foundation::Size index{0U}; index < g_validPlan.GetMappingCount(); ++index)
    {
        const auto mapping = g_validPlan.GetMapping(index);
        if (!mapping.HasValue() || ((index != 0U) && (mapping.GetValue().VirtualStart.GetValue() <= previous)))
        {
            sorted = false;
        }
        if (mapping.HasValue())
        {
            previous = mapping.GetValue().VirtualStart.GetValue();
        }
    }
    ASTERIA_EXPECT(runner, "plan stores mappings in strict virtual-address order", sorted);
    ASTERIA_EXPECT(runner, "code, stack, and page-table ownership permit sealing",
                   g_validPlan.Seal() == InitialMappingPlanError::None, g_validPlan.IsSealed());
    ASTERIA_EXPECT(runner, "sealed plans are immutable and cannot be sealed twice",
                   g_validPlan.AddMapping(readOnly) == InitialMappingPlanError::AlreadySealed,
                   g_validPlan.Seal() == InitialMappingPlanError::AlreadySealed, g_validPlan.GetMappingCount() == 7U);
    ASTERIA_EXPECT(runner, "mapping inspection is bounds checked", !g_validPlan.GetMapping(7U).HasValue(),
                   g_validPlan.GetMapping(7U).GetError() == InitialMappingPlanError::IndexOutOfBounds);
}

void TestInvalidMappings(TestContext& runner) noexcept
{
    InitialMapping candidate =
        MakeMapping(0x1000U, 0x2000U, 1U, ReadOnly(), MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff);

    candidate.Pages = PageCount{0U};
    ASTERIA_EXPECT(runner, "zero-page mapping is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.Pages = PageCount{1U};

    candidate.VirtualStart = VirtualAddress{0x1001U};
    ASTERIA_EXPECT(runner, "unaligned virtual address is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.VirtualStart = VirtualAddress{0x1000U};

    candidate.PhysicalStart = PhysicalAddress{0x2001U};
    ASTERIA_EXPECT(runner, "unaligned physical address is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.PhysicalStart = PhysicalAddress{0x2000U};

    candidate.VirtualStart = VirtualAddress{MaximumPhysicalPageCount << 12U};
    ASTERIA_EXPECT(runner, "wrapping virtual extent is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.VirtualStart = VirtualAddress{0x1000U};

    candidate.Reserved = 1U;
    ASTERIA_EXPECT(runner, "nonzero reserved mapping state is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.Reserved = 0U;

    candidate.Permissions = MappingPermissions{static_cast<MappingPermission>(0x8000U)};
    ASTERIA_EXPECT(runner, "unknown permission bit is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.Permissions = MappingPermissions{MappingPermission::Write};
    ASTERIA_EXPECT(runner, "mapping without read permission is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);

    candidate.Permissions = ReadWrite().With(MappingPermission::Execute);
    ASTERIA_EXPECT(runner, "simultaneous writable and executable mapping is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::WriteExecuteMapping);
    candidate.Permissions = ReadOnly();

    candidate.CachePolicy = static_cast<MemoryCachePolicy>(0xFFU);
    ASTERIA_EXPECT(runner, "unknown cache policy is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.CachePolicy = MemoryCachePolicy::WriteBack;

    candidate.Owner = static_cast<InitialMappingOwner>(0xFFU);
    ASTERIA_EXPECT(runner, "unknown mapping owner is rejected",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::InvalidMapping);
    candidate.Owner = InitialMappingOwner::NucleusCode;
    candidate.Permissions = ReadWrite();
    ASTERIA_EXPECT(runner, "nucleus code must be executable and nonwritable",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);
    candidate.Permissions = ReadExecute();
    candidate.CachePolicy = MemoryCachePolicy::Uncached;
    ASTERIA_EXPECT(runner, "nucleus code must use write-back memory",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);

    candidate.Owner = InitialMappingOwner::NucleusInterruptVectorTable;
    candidate.Permissions = ReadWrite();
    candidate.CachePolicy = MemoryCachePolicy::WriteBack;
    ASTERIA_EXPECT(runner, "an interruption-vector table must be executable and nonwritable",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);
    candidate.Permissions = ReadExecute();
    candidate.CachePolicy = MemoryCachePolicy::Uncached;
    ASTERIA_EXPECT(runner, "an interruption-vector table must use write-back memory",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);

    candidate.Owner = InitialMappingOwner::NucleusStack;
    candidate.Permissions = ReadOnly();
    candidate.CachePolicy = MemoryCachePolicy::WriteBack;
    ASTERIA_EXPECT(runner, "nucleus stack must be writable and nonexecutable",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);

    candidate.Owner = InitialMappingOwner::NucleusRegisterBackingStore;
    ASTERIA_EXPECT(runner, "register backing store must be writable and nonexecutable",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);
    candidate.Permissions = ReadWrite();
    candidate.CachePolicy = MemoryCachePolicy::Uncached;
    ASTERIA_EXPECT(runner, "register backing store must use write-back memory",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);

    candidate.Owner = InitialMappingOwner::Device;
    candidate.Permissions = ReadWrite();
    candidate.CachePolicy = MemoryCachePolicy::WriteBack;
    ASTERIA_EXPECT(runner, "device mapping cannot use ordinary write-back policy",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);

    candidate.Owner = InitialMappingOwner::PageTable;
    candidate.CachePolicy = MemoryCachePolicy::Uncached;
    ASTERIA_EXPECT(runner, "page-table mapping must use write-back memory",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);

    candidate.Owner = InitialMappingOwner::NucleusDescriptorTables;
    candidate.Permissions = ReadOnly();
    candidate.CachePolicy = MemoryCachePolicy::WriteBack;
    ASTERIA_EXPECT(runner, "descriptor-table mapping must be writable and nonexecutable",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);
    candidate.Permissions = ReadWrite();
    candidate.CachePolicy = MemoryCachePolicy::Uncached;
    ASTERIA_EXPECT(runner, "descriptor-table mapping must use write-back memory",
                   g_invalidPlan.AddMapping(candidate) == InitialMappingPlanError::OwnerPolicyMismatch);
    ASTERIA_EXPECT(runner, "all invalid mapping failures leave the plan empty", g_invalidPlan.GetMappingCount() == 0U);
}

void TestOverlapPolicy(TestContext& runner) noexcept
{
    const InitialMapping code = MakeMapping(0x1000U, 0x10000U, 4U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                            InitialMappingOwner::NucleusCode);
    ASTERIA_EXPECT(runner, "overlap test code mapping initializes plan",
                   g_overlapPlan.AddMapping(code) == InitialMappingPlanError::None);

    InitialMapping overlap =
        MakeMapping(0x2000U, 0x20000U, 1U, ReadOnly(), MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff);
    ASTERIA_EXPECT(runner, "partial virtual overlap is rejected",
                   g_overlapPlan.AddMapping(overlap) == InitialMappingPlanError::VirtualOverlap);

    overlap = MakeMapping(0x10000U, 0x11000U, 2U, ReadWrite(), MemoryCachePolicy::WriteBack,
                          InitialMappingOwner::NucleusWritableData);
    ASTERIA_EXPECT(runner, "writable alias of executable physical pages is rejected",
                   g_overlapPlan.AddMapping(overlap) == InitialMappingPlanError::PhysicalWriteExecuteAlias);

    overlap = MakeMapping(0x20000U, 0x10000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                          InitialMappingOwner::NucleusCode);
    ASTERIA_EXPECT(runner, "equally protected executable physical alias is permitted",
                   g_overlapPlan.AddMapping(overlap) == InitialMappingPlanError::None);

    const InitialMapping globalCodeAlias =
        MakeMapping(0x28000U, 0x10000U, 1U, ReadExecute().With(MappingPermission::Global), MemoryCachePolicy::WriteBack,
                    InitialMappingOwner::NucleusCode);
    ASTERIA_EXPECT(runner, "physical aliases with different global permission are rejected",
                   g_overlapPlan.AddMapping(globalCodeAlias) == InitialMappingPlanError::PhysicalPermissionAlias);

    const InitialMapping writable = MakeMapping(0x30000U, 0x30000U, 2U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                                InitialMappingOwner::NucleusWritableData);
    const InitialMapping writableAlias = MakeMapping(0x40000U, 0x30000U, 2U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                                     InitialMappingOwner::NucleusWritableData);
    ASTERIA_EXPECT(runner, "equally protected writable physical alias is permitted",
                   g_overlapPlan.AddMapping(writable) == InitialMappingPlanError::None,
                   g_overlapPlan.AddMapping(writableAlias) == InitialMappingPlanError::None);
    const InitialMapping readOnlyAlias =
        MakeMapping(0x48000U, 0x31000U, 1U, ReadOnly(), MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff);
    ASTERIA_EXPECT(runner, "read-only alias of a writable physical page is rejected",
                   g_overlapPlan.AddMapping(readOnlyAlias) == InitialMappingPlanError::PhysicalPermissionAlias);
    const InitialMapping conflictingCacheAlias = MakeMapping(
        0x50000U, 0x30000U, 1U, ReadWrite(), MemoryCachePolicy::Uncached, InitialMappingOwner::FirmwareRuntimeData);
    ASTERIA_EXPECT(runner, "physical aliases with conflicting cache policies are rejected",
                   g_overlapPlan.AddMapping(conflictingCacheAlias) ==
                       InitialMappingPlanError::PhysicalCachePolicyAlias);
    ASTERIA_EXPECT(runner, "overlap failures do not change retained mapping count",
                   g_overlapPlan.GetMappingCount() == 4U);
}

void TestSealRequirements(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "seal requires nucleus code ownership",
                   g_sealPlan.Seal() == InitialMappingPlanError::MissingNucleusCode);
    ASTERIA_EXPECT(runner, "seal requires nucleus stack ownership after code",
                   g_sealPlan.AddMapping(MakeMapping(0x1000U, 0x1000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                                     InitialMappingOwner::NucleusCode)) ==
                       InitialMappingPlanError::None,
                   g_sealPlan.Seal() == InitialMappingPlanError::MissingNucleusStack);
    ASTERIA_EXPECT(runner, "seal requires page-table ownership after code and stack",
                   g_sealPlan.AddMapping(MakeMapping(0x2000U, 0x2000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                                     InitialMappingOwner::NucleusStack)) ==
                       InitialMappingPlanError::None,
                   g_sealPlan.Seal() == InitialMappingPlanError::MissingPageTable);
    ASTERIA_EXPECT(runner, "complete minimum ownership plan seals",
                   g_sealPlan.AddMapping(MakeMapping(0x3000U, 0x3000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                                     InitialMappingOwner::PageTable)) == InitialMappingPlanError::None,
                   g_sealPlan.Seal() == InitialMappingPlanError::None);

    ASTERIA_EXPECT(
        runner, "register backing-store ownership does not satisfy the stack requirement",
        g_rseSealPlan.AddMapping(MakeMapping(0x1000U, 0x1000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                             InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None,
        g_rseSealPlan.AddMapping(MakeMapping(0x2000U, 0x2000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                             InitialMappingOwner::PageTable)) == InitialMappingPlanError::None,
        g_rseSealPlan.AddMapping(MakeMapping(0x3000U, 0x3000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                             InitialMappingOwner::NucleusRegisterBackingStore)) ==
            InitialMappingPlanError::None,
        g_rseSealPlan.Seal() == InitialMappingPlanError::MissingNucleusStack);

    ASTERIA_EXPECT(runner, "interrupt-vector ownership does not satisfy the code requirement",
                   g_ivtCodeSubstitutionPlan.AddMapping(
                       MakeMapping(0x1000U, 0x1000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::NucleusInterruptVectorTable)) == InitialMappingPlanError::None,
                   g_ivtCodeSubstitutionPlan.AddMapping(
                       MakeMapping(0x2000U, 0x2000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None,
                   g_ivtCodeSubstitutionPlan.AddMapping(
                       MakeMapping(0x3000U, 0x3000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::PageTable)) == InitialMappingPlanError::None,
                   g_ivtCodeSubstitutionPlan.Seal() == InitialMappingPlanError::MissingNucleusCode);

    ASTERIA_EXPECT(runner, "interrupt-vector ownership does not satisfy the stack requirement",
                   g_ivtStackSubstitutionPlan.AddMapping(
                       MakeMapping(0x1000U, 0x1000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None,
                   g_ivtStackSubstitutionPlan.AddMapping(
                       MakeMapping(0x2000U, 0x2000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::NucleusInterruptVectorTable)) == InitialMappingPlanError::None,
                   g_ivtStackSubstitutionPlan.AddMapping(
                       MakeMapping(0x3000U, 0x3000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::PageTable)) == InitialMappingPlanError::None,
                   g_ivtStackSubstitutionPlan.Seal() == InitialMappingPlanError::MissingNucleusStack);

    ASTERIA_EXPECT(runner, "interrupt-vector ownership does not satisfy the page-table requirement",
                   g_ivtPageTableSubstitutionPlan.AddMapping(
                       MakeMapping(0x1000U, 0x1000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None,
                   g_ivtPageTableSubstitutionPlan.AddMapping(
                       MakeMapping(0x2000U, 0x2000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None,
                   g_ivtPageTableSubstitutionPlan.AddMapping(
                       MakeMapping(0x3000U, 0x3000U, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::NucleusInterruptVectorTable)) == InitialMappingPlanError::None,
                   g_ivtPageTableSubstitutionPlan.Seal() == InitialMappingPlanError::MissingPageTable);
}

void TestCapacityAndBoundary(TestContext& runner) noexcept
{
    bool filled{true};
    for (Foundation::Size index{0U}; index < Kernel::Memory::MaximumInitialMappings; ++index)
    {
        const Foundation::UInt64 offset = static_cast<Foundation::UInt64>(index) << 12U;
        if (g_capacityPlan.AddMapping(MakeMapping(0x1000'0000U + offset, 0x2000'0000U + offset, 1U, ReadOnly(),
                                                  MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff)) !=
            InitialMappingPlanError::None)
        {
            filled = false;
        }
    }
    ASTERIA_EXPECT(runner, "fixed plan accepts exactly its documented mapping capacity", filled,
                   g_capacityPlan.GetMappingCount() == Kernel::Memory::MaximumInitialMappings);
    ASTERIA_EXPECT(runner, "capacity failure leaves the full plan unchanged",
                   g_capacityPlan.AddMapping(
                       MakeMapping(0x3000'0000U, 0x4000'0000U, 1U, ReadOnly(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::BootHandoff)) == InitialMappingPlanError::CapacityExceeded,
                   g_capacityPlan.GetMappingCount() == Kernel::Memory::MaximumInitialMappings);

    constexpr Foundation::UInt64 HighestValidStart = (MaximumPhysicalPageCount - 1U) << 12U;
    ASTERIA_EXPECT(runner, "highest nonwrapping physical and virtual page is accepted",
                   g_boundaryPlan.AddMapping(
                       MakeMapping(HighestValidStart, HighestValidStart, 1U, ReadOnly(), MemoryCachePolicy::WriteBack,
                                   InitialMappingOwner::BootHandoff)) == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "mapping whose exclusive end wraps is rejected",
                   g_boundaryPlan.AddMapping(MakeMapping(
                       MaximumPhysicalPageCount << 12U, 0x1000U, 1U, ReadOnly(), MemoryCachePolicy::WriteBack,
                       InitialMappingOwner::BootHandoff)) == InitialMappingPlanError::InvalidMapping);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialMappingPlanTests"};
    Asteria::Test::TestValidPlan(runner);
    Asteria::Test::TestInvalidMappings(runner);
    Asteria::Test::TestOverlapPolicy(runner);
    Asteria::Test::TestSealRequirements(runner);
    Asteria::Test::TestCapacityAndBoundary(runner);
    return runner.Finish();
}
