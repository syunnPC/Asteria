// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialTranslationEncoder.hpp"

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
using Platform::Ia64::Memory::EncodedInitialTranslation;
using Platform::Ia64::Memory::InitialTranslationEncoder;
using Platform::Ia64::Memory::InitialTranslationEncodingError;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::TranslationCapabilities;

constexpr Foundation::UInt64 Base{0xE000'0000'0010'0000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 ExceptionDeferralBit{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 ReadExecuteAccessRights{Foundation::UInt64{1U} << 9U};
constexpr Foundation::UInt64 ReadWriteAccessRights{Foundation::UInt64{2U} << 9U};

constinit InitialMappingPlan g_plan;
constinit InitialMappingPlan g_unsealedPlan;
constinit InitialMappingPlan g_globalPlan;
constinit InitialMappingPlan g_mixedGlobalPlan;
constinit InitialMappingPlan g_nativeRangePlan;
constinit InitialMappingPlan g_signExtendedPlan;
constinit InitialMappingPlan g_largePagePlan;

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

[[nodiscard]] constexpr TranslationCapabilities FullCapabilities() noexcept
{
    return TranslationCapabilities{
        .InsertablePageSizeMask = Foundation::UInt64{1U} << 12U,
        .PhysicalAddressBits = 50U,
        .ImplementedVirtualAddressMostSignificantBit = 60U,
        .RegionIdentifierBits = 24U,
        .ProtectionKeyBits = 24U,
        .MemoryAttributeMask =
            static_cast<Foundation::UInt8>((Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) |
                                           (Foundation::UInt8{1U} << 5U) | (Foundation::UInt8{1U} << 6U)),
        .HardwareWalkerImplemented = true,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr RegionConfiguration PrivateRegion() noexcept
{
    return RegionConfiguration{
        .RegionIdentifier = 0x0012'3456U,
        .ProtectionKey = 0x0065'4321U,
        .VhptWalkerEnabled = false,
        .SharedAcrossAddressSpaces = false,
        .Reserved16 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr RegionConfiguration SharedRegion() noexcept
{
    RegionConfiguration region = PrivateRegion();
    region.VhptWalkerEnabled = true;
    region.SharedAcrossAddressSpaces = true;
    return region;
}

[[nodiscard]] constexpr RegionConfiguration PrivateWalkerRegion() noexcept
{
    RegionConfiguration region = PrivateRegion();
    region.VhptWalkerEnabled = true;
    return region;
}

[[nodiscard]] bool AddRequiredMappings(InitialMappingPlan& plan, const Foundation::UInt64 virtualBase,
                                       const Foundation::UInt64 physicalBase) noexcept
{
    return (plan.AddMapping(MakeMapping(virtualBase, physicalBase, 1U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                        InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeMapping(virtualBase + 0x1000U, physicalBase + 0x1000U, 1U, ReadWrite(),
                                        MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusStack)) ==
            InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeMapping(virtualBase + 0x2000U, physicalBase + 0x2000U, 1U, ReadWrite(),
                                        MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable)) ==
            InitialMappingPlanError::None);
}

void CheckError(TestContext& runner, const InitialMappingPlan& plan, const Foundation::Size mappingIndex,
                const Foundation::UInt64 pageIndex, const TranslationCapabilities& capabilities,
                const RegionConfiguration& region, const InitialTranslationEncodingError expected,
                const char* const description) noexcept
{
    const auto result = InitialTranslationEncoder::EncodePage(plan, mappingIndex, pageIndex, capabilities, region);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expected);
}

void CheckRangeError(TestContext& runner, const InitialMappingPlan& plan, const Foundation::Size mappingIndex,
                     const Foundation::UInt64 firstPageIndex, const Foundation::UInt8 pageShift,
                     const TranslationCapabilities& capabilities, const RegionConfiguration& region,
                     const InitialTranslationEncodingError expected, const char* const description) noexcept
{
    const auto result =
        InitialTranslationEncoder::EncodeRange(plan, mappingIndex, firstPageIndex, pageShift, capabilities, region);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expected);
}

void TestExactEncoding(TestContext& runner) noexcept
{
    const bool ready =
        (g_plan.AddMapping(MakeMapping(Base, 0x0010'0000U, 2U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                       InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
        (g_plan.AddMapping(MakeMapping(Base + 0x10'000U, 0x0011'0000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                       InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
        (g_plan.AddMapping(MakeMapping(Base + 0x20'000U, 0x0012'0000U, 1U, ReadWrite(), MemoryCachePolicy::WriteBack,
                                       InitialMappingOwner::PageTable)) == InitialMappingPlanError::None) &&
        (g_plan.AddMapping(MakeMapping(Base + 0x30'000U, 0x0013'0000U, 1U, ReadOnly(), MemoryCachePolicy::WriteBack,
                                       InitialMappingOwner::BootHandoff)) == InitialMappingPlanError::None) &&
        (g_plan.AddMapping(MakeMapping(Base + 0x40'000U, 0x0014'0000U, 1U, ReadWrite(), MemoryCachePolicy::Uncached,
                                       InitialMappingOwner::FirmwareRuntimeData)) == InitialMappingPlanError::None) &&
        (g_plan.AddMapping(MakeMapping(Base + 0x50'000U, 0x0015'0000U, 1U, ReadWrite(),
                                       MemoryCachePolicy::WriteCombining, InitialMappingOwner::Device)) ==
         InitialMappingPlanError::None) &&
        (g_plan.AddMapping(MakeMapping(Base + 0x60'000U, 0x0016'0000U, 1U, ReadExecute(),
                                       MemoryCachePolicy::WriteThrough, InitialMappingOwner::FirmwareRuntimeCode)) ==
         InitialMappingPlanError::None) &&
        (g_plan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "mixed IA-64 encoding plan seals", ready);

    const auto codeResult =
        InitialTranslationEncoder::EncodePage(g_plan, 0U, 1U, FullCapabilities(), PrivateWalkerRegion());
    const Foundation::UInt64 expectedCodeWord =
        0x0010'0000U + 0x1000U + PresentBit + AccessedBit + ExceptionDeferralBit + ReadExecuteAccessRights;
    ASTERIA_EXPECT(runner, "private RX page produces exact PTE, ITIR, IFA, and walker-enabled RR words",
                   codeResult.HasValue(), codeResult.GetValue().TranslationWord == expectedCodeWord,
                   codeResult.GetValue().TranslationInsertion == ((Foundation::UInt64{0x0065'4321U} << 8U) | 0x30U),
                   codeResult.GetValue().FaultingAddress == (Base + 0x1000U),
                   codeResult.GetValue().RegionRegister == ((Foundation::UInt64{0x0012'3456U} << 8U) | 0x31U));

    const auto stackResult = InitialTranslationEncoder::EncodePage(g_plan, 1U, 0U, FullCapabilities(), PrivateRegion());
    const Foundation::UInt64 expectedStackWord =
        0x0011'0000U + PresentBit + AccessedBit + DirtyBit + ExceptionDeferralBit + ReadWriteAccessRights;
    ASTERIA_EXPECT(runner, "private RW page is dirty, supervisor-only, and walker-disabled", stackResult.HasValue(),
                   stackResult.GetValue().TranslationWord == expectedStackWord,
                   (stackResult.GetValue().RegionRegister & 1U) == 0U);

    const auto readOnlyResult =
        InitialTranslationEncoder::EncodePage(g_plan, 3U, 0U, FullCapabilities(), PrivateRegion());
    ASTERIA_EXPECT(runner, "private read-only page uses AR zero without a dirty bit", readOnlyResult.HasValue(),
                   readOnlyResult.GetValue().TranslationWord ==
                       (0x0013'0000U + PresentBit + AccessedBit + ExceptionDeferralBit));

    const auto uncachedResult =
        InitialTranslationEncoder::EncodePage(g_plan, 4U, 0U, FullCapabilities(), PrivateRegion());
    const auto writeCombiningResult =
        InitialTranslationEncoder::EncodePage(g_plan, 5U, 0U, FullCapabilities(), PrivateRegion());
    ASTERIA_EXPECT(runner, "uncached policy encodes MA value four", uncachedResult.HasValue(),
                   (uncachedResult.GetValue().TranslationWord & 0x1CU) == 0x10U);
    ASTERIA_EXPECT(runner, "write-combining policy encodes MA value six", writeCombiningResult.HasValue(),
                   (writeCombiningResult.GetValue().TranslationWord & 0x1CU) == 0x18U);

    CheckError(runner, g_plan, 6U, 0U, FullCapabilities(), PrivateRegion(),
               InitialTranslationEncodingError::UnsupportedCachePolicy,
               "IA-64 rejects the common write-through policy with a typed error");

    const bool globalReady =
        (g_globalPlan.AddMapping(MakeMapping(0xA000'0000'0010'0000ULL, 0x0017'0000U, 1U, ReadWrite(),
                                             MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusStack)) ==
         InitialMappingPlanError::None) &&
        (g_globalPlan.AddMapping(MakeMapping(0xC000'0000'0010'0000ULL, 0x0018'0000U, 1U, ReadWrite(),
                                             MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable)) ==
         InitialMappingPlanError::None) &&
        (g_globalPlan.AddMapping(MakeMapping(Base, 0x0019'0000U, 1U, ReadExecute().With(MappingPermission::Global),
                                             MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusCode)) ==
         InitialMappingPlanError::None) &&
        (g_globalPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "global code is isolated in its own IA-64 virtual region", globalReady);
    const auto globalResult =
        InitialTranslationEncoder::EncodePage(g_globalPlan, 2U, 0U, FullCapabilities(), SharedRegion());
    ASTERIA_EXPECT(runner, "global mapping is admitted through an explicitly shared region identifier",
                   globalResult.HasValue(), globalResult.GetValue().FaultingAddress == Base);

    const bool mixedReady =
        (g_mixedGlobalPlan.AddMapping(MakeMapping(
             Base + 0x100'0000U, 0x001A'0000U, 1U, ReadExecute().With(MappingPermission::Global),
             MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
        (g_mixedGlobalPlan.AddMapping(MakeMapping(Base + 0x100'1000U, 0x001B'0000U, 1U, ReadWrite(),
                                                  MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusStack)) ==
         InitialMappingPlanError::None) &&
        (g_mixedGlobalPlan.AddMapping(MakeMapping(Base + 0x100'2000U, 0x001C'0000U, 1U, ReadWrite(),
                                                  MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable)) ==
         InitialMappingPlanError::None) &&
        (g_mixedGlobalPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "common plan retains mixed global state within one region", mixedReady);
    CheckError(runner, g_mixedGlobalPlan, 0U, 0U, FullCapabilities(), SharedRegion(),
               InitialTranslationEncodingError::GlobalRegionMismatch,
               "IA-64 rejects mixed global and private mappings within one virtual region");
}

void TestPlanAndRegionFailures(TestContext& runner) noexcept
{
    CheckError(runner, g_unsealedPlan, 0U, 0U, FullCapabilities(), PrivateRegion(),
               InitialTranslationEncodingError::PlanNotSealed, "unsealed mapping plan is rejected");
    CheckError(runner, g_plan, g_plan.GetMappingCount(), 0U, FullCapabilities(), PrivateRegion(),
               InitialTranslationEncodingError::MappingIndexOutOfBounds,
               "mapping index at the plan extent is rejected");
    CheckError(runner, g_plan, 0U, 2U, FullCapabilities(), SharedRegion(),
               InitialTranslationEncodingError::PageIndexOutOfBounds, "page index at the mapping extent is rejected");
    CheckError(runner, g_globalPlan, 2U, 0U, FullCapabilities(), PrivateRegion(),
               InitialTranslationEncodingError::GlobalRegionMismatch,
               "global mapping requires a shared region identifier contract");
    CheckError(runner, g_plan, 1U, 0U, FullCapabilities(), SharedRegion(),
               InitialTranslationEncodingError::GlobalRegionMismatch,
               "private mapping cannot be inserted through a shared region contract");

    RegionConfiguration region = PrivateRegion();
    region.RegionIdentifier = 0x0100'0000U;
    CheckError(runner, g_plan, 1U, 0U, FullCapabilities(), region,
               InitialTranslationEncodingError::InvalidRegionConfiguration,
               "region identifier beyond the implemented 24 bits is rejected");
    region = PrivateRegion();
    region.ProtectionKey = 0x0100'0000U;
    CheckError(runner, g_plan, 1U, 0U, FullCapabilities(), region,
               InitialTranslationEncodingError::InvalidRegionConfiguration,
               "protection key beyond the implemented 24 bits is rejected");
    region = PrivateRegion();
    region.Reserved16 = 1U;
    CheckError(runner, g_plan, 1U, 0U, FullCapabilities(), region,
               InitialTranslationEncodingError::InvalidRegionConfiguration,
               "nonzero region reserved state is rejected");
    region = PrivateRegion();
    region.VhptWalkerEnabled = true;
    TranslationCapabilities capabilities = FullCapabilities();
    capabilities.HardwareWalkerImplemented = false;
    CheckError(runner, g_plan, 1U, 0U, capabilities, region, InitialTranslationEncodingError::HardwareWalkerUnavailable,
               "VHPT walking cannot be enabled when PAL reports no hardware walker");
}

void TestVariablePageRanges(TestContext& runner) noexcept
{
    const bool ready =
        (g_largePagePlan.AddMapping(MakeMapping(Base, 0x0040'0000U, 32U, ReadExecute(), MemoryCachePolicy::WriteBack,
                                                InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
        (g_largePagePlan.AddMapping(MakeMapping(Base + 0x20'000U, 0x0042'0000U, 1U, ReadWrite(),
                                                MemoryCachePolicy::WriteBack, InitialMappingOwner::NucleusStack)) ==
         InitialMappingPlanError::None) &&
        (g_largePagePlan.AddMapping(MakeMapping(Base + 0x21'000U, 0x0042'1000U, 1U, ReadWrite(),
                                                MemoryCachePolicy::WriteBack, InitialMappingOwner::PageTable)) ==
         InitialMappingPlanError::None) &&
        (g_largePagePlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "large-page IA-64 encoding plan seals", ready);

    TranslationCapabilities capabilities = FullCapabilities();
    capabilities.InsertablePageSizeMask |= Foundation::UInt64{1U} << 16U;
    const auto largeResult =
        InitialTranslationEncoder::EncodeRange(g_largePagePlan, 0U, 16U, 16U, capabilities, PrivateRegion());
    const Foundation::UInt64 expectedWord =
        0x0041'0000U + PresentBit + AccessedBit + ExceptionDeferralBit + ReadExecuteAccessRights;
    ASTERIA_EXPECT(runner, "64 KiB range encodes exact words while RR retains the 4 KiB walker preference",
                   largeResult.HasValue(), largeResult.GetValue().TranslationWord == expectedWord,
                   largeResult.GetValue().TranslationInsertion == ((Foundation::UInt64{0x0065'4321U} << 8U) | 0x40U),
                   largeResult.GetValue().FaultingAddress == (Base + 0x10'000U),
                   largeResult.GetValue().RegionRegister == ((Foundation::UInt64{0x0012'3456U} << 8U) | 0x30U));

    CheckRangeError(runner, g_largePagePlan, 0U, 0U, 11U, capabilities, PrivateRegion(),
                    InitialTranslationEncodingError::InvalidPageShift,
                    "a translation smaller than the common 4 KiB base page is rejected");
    CheckRangeError(runner, g_largePagePlan, 0U, 0U, 61U, capabilities, PrivateRegion(),
                    InitialTranslationEncodingError::InvalidPageShift,
                    "a translation wider than one IA-64 virtual region is rejected");
    CheckRangeError(runner, g_largePagePlan, 0U, 0U, 14U, capabilities, PrivateRegion(),
                    InitialTranslationEncodingError::UnsupportedPageSize,
                    "a structurally valid range size still requires PAL insertion support");
    CheckRangeError(runner, g_largePagePlan, 0U, 32U, 16U, capabilities, PrivateRegion(),
                    InitialTranslationEncodingError::PageIndexOutOfBounds,
                    "a range beginning at the mapping extent is rejected");
    CheckRangeError(runner, g_largePagePlan, 0U, 24U, 16U, capabilities, PrivateRegion(),
                    InitialTranslationEncodingError::TranslationRangeOutOfBounds,
                    "a large translation extending beyond its mapping is rejected");
    CheckRangeError(runner, g_largePagePlan, 0U, 1U, 16U, capabilities, PrivateRegion(),
                    InitialTranslationEncodingError::MisalignedTranslationRange,
                    "a large translation requires natural virtual and physical alignment");
}

void TestCapabilityFailures(TestContext& runner) noexcept
{
    TranslationCapabilities capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 31U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "physical address width below the architectural minimum is rejected");
    capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 64U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "physical address width above the PAL field contract is rejected");
    capabilities = FullCapabilities();
    capabilities.ImplementedVirtualAddressMostSignificantBit = 49U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "implemented VA bit below the architectural minimum is rejected");
    capabilities = FullCapabilities();
    capabilities.ImplementedVirtualAddressMostSignificantBit = 61U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "implemented VA bit overlapping the region number is rejected");
    capabilities = FullCapabilities();
    capabilities.RegionIdentifierBits = 17U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "RID width below the architectural minimum is rejected");
    capabilities = FullCapabilities();
    capabilities.ProtectionKeyBits = 25U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "protection-key width above the field is rejected");
    capabilities = FullCapabilities();
    capabilities.ProtectionKeyBits = 18U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "protection-key width smaller than the RID width is rejected");
    capabilities = FullCapabilities();
    capabilities.Reserved64 = 1U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities, "nonzero capability reserved state is rejected");
    capabilities = FullCapabilities();
    capabilities.InsertablePageSizeMask = 0U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "missing PAL support for the fixed 4 KiB RR preference is rejected");
    capabilities = FullCapabilities();
    capabilities.InsertablePageSizeMask = Foundation::UInt64{1U} << 16U;
    CheckRangeError(runner, g_largePagePlan, 0U, 0U, 16U, capabilities, PrivateRegion(),
                    InitialTranslationEncodingError::InvalidCapabilities,
                    "large-page support cannot omit the fixed 4 KiB RR preference");
    capabilities = FullCapabilities();
    capabilities.InsertablePageSizeMask |= Foundation::UInt64{1U} << 11U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "sub-4 KiB insertable capability bits are rejected");
    capabilities = FullCapabilities();
    capabilities.MemoryAttributeMask = 0U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::UnsupportedMemoryAttribute,
               "mapping memory attribute must be reported by PAL");
    capabilities = FullCapabilities();
    capabilities.MemoryAttributeMask = static_cast<Foundation::UInt8>(
        capabilities.MemoryAttributeMask & static_cast<Foundation::UInt8>(~(Foundation::UInt8{1U} << 6U)));
    CheckError(runner, g_plan, 5U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::UnsupportedMemoryAttribute,
               "write-combining mapping requires implemented MA value six");
    capabilities = FullCapabilities();
    capabilities.MemoryAttributeMask |= Foundation::UInt8{1U} << 1U;
    CheckError(runner, g_plan, 1U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::InvalidCapabilities,
               "undefined PAL memory-attribute bits are rejected");
}

void TestNativeAddressRanges(TestContext& runner) noexcept
{
    const bool rangeReady =
        AddRequiredMappings(g_nativeRangePlan, 0x0000'0000'0100'0000ULL, 0x0020'0000U) &&
        (g_nativeRangePlan.AddMapping(MakeMapping(0x0040'0000'0000'0000ULL, 0x0030'0000U, 1U, ReadOnly(),
                                                  MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff)) ==
         InitialMappingPlanError::None) &&
        (g_nativeRangePlan.AddMapping(MakeMapping(0x0040'0000'0000'1000ULL, 0x0000'1000'0000'0000ULL, 1U, ReadOnly(),
                                                  MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff)) ==
         InitialMappingPlanError::None) &&
        (g_nativeRangePlan.AddMapping(MakeMapping(0x0040'0000'0000'2000ULL, 0x0004'0000'0000'0000ULL, 1U, ReadOnly(),
                                                  MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff)) ==
         InitialMappingPlanError::None) &&
        (g_nativeRangePlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "target-neutral plan retains IA-64 native range probes", rangeReady);

    TranslationCapabilities capabilities = FullCapabilities();
    capabilities.ImplementedVirtualAddressMostSignificantBit = 53U;
    CheckError(runner, g_nativeRangePlan, 3U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::UnimplementedVirtualAddress,
               "non-sign-extended payload above IMPL_VA_MSB is rejected");

    capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 44U;
    CheckError(runner, g_nativeRangePlan, 4U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::PhysicalAddressOutOfRange,
               "physical page at the reported PAL limit is rejected");

    capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 63U;
    CheckError(runner, g_nativeRangePlan, 5U, 0U, capabilities, PrivateRegion(),
               InitialTranslationEncodingError::PhysicalAddressOutOfRange,
               "physical page beyond the current 50-bit PPN field is rejected");

    const bool signExtendedReady =
        AddRequiredMappings(g_signExtendedPlan, 0x0000'0000'0200'0000ULL, 0x0040'0000U) &&
        (g_signExtendedPlan.AddMapping(MakeMapping(0x7FFF'FFFF'FFFF'E000ULL, 0x0050'0000U, 1U, ReadOnly(),
                                                   MemoryCachePolicy::WriteBack, InitialMappingOwner::BootHandoff)) ==
         InitialMappingPlanError::None) &&
        (g_signExtendedPlan.Seal() == InitialMappingPlanError::None);
    ASTERIA_EXPECT(runner, "sign-extended region payload probe plan seals", signExtendedReady);
    capabilities = FullCapabilities();
    capabilities.ImplementedVirtualAddressMostSignificantBit = 53U;
    const auto signExtendedResult =
        InitialTranslationEncoder::EncodePage(g_signExtendedPlan, 3U, 0U, capabilities, PrivateRegion());
    ASTERIA_EXPECT(runner, "proper sign extension within a nonzero virtual region is accepted",
                   signExtendedResult.HasValue(),
                   signExtendedResult.GetValue().FaultingAddress == 0x7FFF'FFFF'FFFF'E000ULL);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialTranslationEncoderTests"};
    Asteria::Test::TestExactEncoding(runner);
    Asteria::Test::TestPlanAndRegionFailures(runner);
    Asteria::Test::TestVariablePageRanges(runner);
    Asteria::Test::TestCapabilityFailures(runner);
    Asteria::Test::TestNativeAddressRanges(runner);
    return runner.Finish();
}
