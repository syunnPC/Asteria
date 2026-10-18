// SPDX-License-Identifier: Apache-2.0
#include <bit>
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Platform::Ia64::Memory::EncodedIa64RuntimeTranslation;
using Platform::Ia64::Memory::Ia64RuntimeTranslationEncoder;
using Platform::Ia64::Memory::Ia64RuntimeTranslationEncodingError;
using Platform::Ia64::Memory::MaximumIa64RuntimeMappingPages;
using Platform::Ia64::Memory::TranslationCapabilities;

constexpr Foundation::UInt64 RegionFiveBase{0xA000'0000'0010'0000ULL};
constexpr Foundation::UInt64 PhysicalBase{0x0000'0000'0020'0000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 PrivilegeLevelMask{Foundation::UInt64{3U} << 7U};
constexpr Foundation::UInt64 UserPrivilegeLevel{Foundation::UInt64{3U} << 7U};
constexpr Foundation::UInt64 AccessRightsMask{Foundation::UInt64{7U} << 9U};
constexpr Foundation::UInt64 ReadExecuteAccessRights{Foundation::UInt64{1U} << 9U};
constexpr Foundation::UInt64 ReadWriteAccessRights{Foundation::UInt64{2U} << 9U};
constexpr Foundation::UInt64 ExceptionDeferralBit{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 CheckedZeroTranslationMask{(Foundation::UInt64{1U} << 1U) |
                                                        (Foundation::UInt64{3U} << 50U)};
constexpr Foundation::UInt64 IgnoredTranslationMask{Foundation::UInt64{0x7FFU} << 53U};
constexpr Foundation::UInt64 TranslationInsertionReservedMask{Foundation::UInt64{3U} |
                                                              (Foundation::UInt64{0xFFFF'FFFFU} << 32U)};
constexpr Foundation::UInt64 TranslationInsertionKeyMask{Foundation::UInt64{0x00FF'FFFFU} << 8U};

[[nodiscard]] constexpr PagePermissions ReadOnly() noexcept
{
    return PagePermissions{PagePermission::Read};
}

[[nodiscard]] constexpr PagePermissions ReadExecute() noexcept
{
    return ReadOnly().With(PagePermission::Execute);
}

[[nodiscard]] constexpr PagePermissions ReadWrite() noexcept
{
    return ReadOnly().With(PagePermission::Write);
}

[[nodiscard]] constexpr PagePermissions UserReadOnly() noexcept
{
    return ReadOnly().With(PagePermission::User);
}

[[nodiscard]] constexpr PagePermissions UserReadExecute() noexcept
{
    return ReadExecute().With(PagePermission::User);
}

[[nodiscard]] constexpr PagePermissions UserReadWrite() noexcept
{
    return ReadWrite().With(PagePermission::User);
}

[[nodiscard]] constexpr TranslationCapabilities FullCapabilities() noexcept
{
    return TranslationCapabilities{
        .InsertablePageSizeMask = Foundation::UInt64{1U} << 12U,
        .PhysicalAddressBits = 50U,
        .ImplementedVirtualAddressMostSignificantBit = 60U,
        .RegionIdentifierBits = 24U,
        .ProtectionKeyBits = 24U,
        .MemoryAttributeMask = static_cast<Foundation::UInt8>(
            (Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) | (Foundation::UInt8{1U} << 5U) |
            (Foundation::UInt8{1U} << 6U) | (Foundation::UInt8{1U} << 7U)),
        .HardwareWalkerImplemented = true,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr AddressSpaceMappingRecord
MakePlan(const Foundation::UInt64 virtualStart = RegionFiveBase, const Foundation::UInt64 physicalStart = PhysicalBase,
         const Foundation::UInt64 pages = 3U, const PagePermissions permissions = ReadOnly(),
         const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack,
         const FrameKind kind = FrameKind::Ram) noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = {},
        .DeviceRegion = {},
        .Frame = {},
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .MemoryObjectOffsetPages = 0U,
        .Permissions = permissions,
        .CachePolicy = cachePolicy,
        .Kind = kind,
        .Commitment = {},
        .RuntimeMapping = true,
        .Reserved = {},
    };
}

void CheckError(TestContext& runner, const AddressSpaceMappingRecord& plan, const Foundation::UInt64 pageIndex,
                const TranslationCapabilities& capabilities, const Ia64RuntimeTranslationEncodingError expected,
                const char* const description) noexcept
{
    const auto result = Ia64RuntimeTranslationEncoder::EncodePage(plan, pageIndex, capabilities);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expected);
}

void CheckExactEncoding(TestContext& runner, const PagePermissions permissions,
                        const Foundation::UInt64 expectedAccessRights, const Foundation::UInt64 expectedPrivilegeLevel,
                        const bool expectedDirty, const char* const description) noexcept
{
    const auto result = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(RegionFiveBase, PhysicalBase, 3U, permissions, MemoryCachePolicy::WriteBack, FrameKind::Ram), 1U,
        FullCapabilities());
    if (!result.HasValue())
    {
        ASTERIA_EXPECT(runner, description, false);
        return;
    }

    const EncodedIa64RuntimeTranslation& encoding = result.GetValue();
    const Foundation::UInt64 expectedWord = PhysicalBase + 0x1000U + PresentBit + AccessedBit +
                                            (expectedDirty ? DirtyBit : 0U) + expectedPrivilegeLevel +
                                            expectedAccessRights + ExceptionDeferralBit;
    ASTERIA_EXPECT(runner, description, encoding.TranslationWord == expectedWord,
                   encoding.TranslationInsertion == 0x30U, encoding.FaultingAddress == (RegionFiveBase + 0x1000U));
}

void TestPermissionsAndExactBits(TestContext& runner) noexcept
{
    CheckExactEncoding(runner, ReadOnly(), 0U, 0U, false, "supervisor R encodes AR0, PL0, and a clear dirty bit");
    CheckExactEncoding(runner, ReadExecute(), ReadExecuteAccessRights, 0U, false,
                       "supervisor RX encodes AR1, PL0, and a clear dirty bit");
    CheckExactEncoding(runner, ReadWrite(), ReadWriteAccessRights, 0U, true,
                       "supervisor RW encodes AR2, PL0, and a set dirty bit");
    CheckExactEncoding(runner, UserReadOnly(), 0U, UserPrivilegeLevel, false, "user R encodes AR0 and exactly PL3");
    CheckExactEncoding(runner, UserReadExecute(), ReadExecuteAccessRights, UserPrivilegeLevel, false,
                       "user RX encodes AR1 and exactly PL3");
    CheckExactEncoding(runner, UserReadWrite(), ReadWriteAccessRights, UserPrivilegeLevel, true,
                       "user RW encodes AR2, exactly PL3, and a set dirty bit");

    const auto result = Ia64RuntimeTranslationEncoder::EncodePage(MakePlan(), 0U, FullCapabilities());
    if (!result.HasValue())
    {
        ASTERIA_EXPECT(runner, "exact reserved-field probe encodes", false);
        return;
    }
    const EncodedIa64RuntimeTranslation& encoding = result.GetValue();
    ASTERIA_EXPECT(runner, "translation bits 1 and 51:50 are checked-zero",
                   (encoding.TranslationWord & CheckedZeroTranslationMask) == 0U);
    ASTERIA_EXPECT(runner, "translation software-ignored bits 63:53 remain zero",
                   (encoding.TranslationWord & IgnoredTranslationMask) == 0U);
    ASTERIA_EXPECT(runner, "translation exception-deferral bit 52 is set",
                   (encoding.TranslationWord & ExceptionDeferralBit) != 0U);
    ASTERIA_EXPECT(runner, "translation present and accessed bits are set",
                   (encoding.TranslationWord & (PresentBit | AccessedBit)) == (PresentBit | AccessedBit));
    ASTERIA_EXPECT(runner, "supervisor translation privilege level is exactly zero",
                   (encoding.TranslationWord & PrivilegeLevelMask) == 0U);
    ASTERIA_EXPECT(runner, "read-only translation access rights are exactly zero",
                   (encoding.TranslationWord & AccessRightsMask) == 0U);
    ASTERIA_EXPECT(runner, "ITIR reserved fields 1:0 and 63:32 are zero",
                   (encoding.TranslationInsertion & TranslationInsertionReservedMask) == 0U);
    ASTERIA_EXPECT(runner, "ITIR protection key is exactly zero",
                   (encoding.TranslationInsertion & TranslationInsertionKeyMask) == 0U);
    ASTERIA_EXPECT(runner, "ITIR page size is architected PS 12, never PS zero",
                   (encoding.TranslationInsertion & 0xFCU) == 0x30U);
}

void TestMemoryAttributes(TestContext& runner) noexcept
{
    struct CacheCase
    {
        MemoryCachePolicy Policy;
        Foundation::UInt64 ExpectedBits;
        const char* Description;
    };
    constexpr CacheCase cases[]{
        {MemoryCachePolicy::WriteBack, 0x00U, "write-back encodes MA zero"},
        {MemoryCachePolicy::Uncached, 0x10U, "uncached encodes MA four"},
        {MemoryCachePolicy::WriteCombining, 0x18U, "write-combining encodes MA six"},
    };
    for (const CacheCase& item : cases)
    {
        const auto result = Ia64RuntimeTranslationEncoder::EncodePage(
            MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadWrite(), item.Policy), 0U, FullCapabilities());
        if (!result.HasValue())
        {
            ASTERIA_EXPECT(runner, item.Description, false);
            continue;
        }
        ASTERIA_EXPECT(runner, item.Description, (result.GetValue().TranslationWord & 0x1CU) == item.ExpectedBits);
    }

    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadOnly(), MemoryCachePolicy::WriteThrough), 0U,
               FullCapabilities(), Ia64RuntimeTranslationEncodingError::UnsupportedCachePolicy,
               "write-through has no admitted IA-64 runtime encoding");
    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadOnly(), static_cast<MemoryCachePolicy>(0xFFU)),
               0U, FullCapabilities(), Ia64RuntimeTranslationEncodingError::UnsupportedCachePolicy,
               "unknown cache policy is rejected");

    for (const CacheCase& item : cases)
    {
        TranslationCapabilities capabilities = FullCapabilities();
        const Foundation::UInt8 memoryAttribute = static_cast<Foundation::UInt8>(item.ExpectedBits >> 2U);
        capabilities.MemoryAttributeMask =
            static_cast<Foundation::UInt8>(capabilities.MemoryAttributeMask &
                                           static_cast<Foundation::UInt8>(~(Foundation::UInt8{1U} << memoryAttribute)));
        CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadOnly(), item.Policy), 0U, capabilities,
                   Ia64RuntimeTranslationEncodingError::UnsupportedMemoryAttribute,
                   "each cache policy requires its PAL-admitted MA value");
    }
}

void TestOffsetsAndPlanBounds(TestContext& runner) noexcept
{
    const auto firstResult = Ia64RuntimeTranslationEncoder::EncodePage(MakePlan(), 0U, FullCapabilities());
    const auto lastResult = Ia64RuntimeTranslationEncoder::EncodePage(MakePlan(), 2U, FullCapabilities());
    ASTERIA_EXPECT(runner, "page index zero uses both exact range starts", firstResult.HasValue(),
                   firstResult.GetValue().FaultingAddress == RegionFiveBase,
                   (firstResult.GetValue().TranslationWord & 0x0003'FFFF'FFFF'F000ULL) == PhysicalBase);
    ASTERIA_EXPECT(runner, "last page index applies the exact 4 KiB virtual and physical offsets",
                   lastResult.HasValue(), lastResult.GetValue().FaultingAddress == (RegionFiveBase + 0x2000U),
                   (lastResult.GetValue().TranslationWord & 0x0003'FFFF'FFFF'F000ULL) == (PhysicalBase + 0x2000U));

    const Foundation::UInt64 maximumRangeStart{0xA000'0000'1000'0000ULL};
    const auto maximumResult = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(maximumRangeStart, PhysicalBase, MaximumIa64RuntimeMappingPages), MaximumIa64RuntimeMappingPages - 1U,
        FullCapabilities());
    ASTERIA_EXPECT(runner, "maximum bounded mapping and its last page are accepted", maximumResult.HasValue(),
                   maximumResult.GetValue().FaultingAddress ==
                       maximumRangeStart + ((MaximumIa64RuntimeMappingPages - 1U) << 12U));

    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, 0U), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::InvalidMappingPlan, "zero-page plan is rejected");
    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, MaximumIa64RuntimeMappingPages + 1U), 0U,
               FullCapabilities(), Ia64RuntimeTranslationEncodingError::MappingTooLarge,
               "plan above the runtime mapping bound is rejected");
    CheckError(runner, MakePlan(), 3U, FullCapabilities(), Ia64RuntimeTranslationEncodingError::PageIndexOutOfBounds,
               "page index at the plan extent is rejected");
    CheckError(runner, MakePlan(RegionFiveBase + 1U), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::InvalidMappingPlan, "misaligned virtual start is rejected");
    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase + 1U), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::InvalidMappingPlan, "misaligned physical start is rejected");
    CheckError(runner, MakePlan(0xFFFF'FFFF'FFFF'F000ULL, PhysicalBase, 2U), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::InvalidMappingPlan, "virtual range addition overflow is rejected");
    CheckError(runner, MakePlan(RegionFiveBase, 0xFFFF'FFFF'FFFF'F000ULL, 2U), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::InvalidMappingPlan, "physical range addition overflow is rejected");
}

void TestVirtualRegionsAndImplementation(TestContext& runner) noexcept
{
    const auto topPageResult = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(0xFFFF'FFFF'FFFF'F000ULL, PhysicalBase, 1U), 0U, FullCapabilities());
    ASTERIA_EXPECT(runner, "region-seven top page ending exactly at UINT64_MAX is accepted", topPageResult.HasValue(),
                   topPageResult.GetValue().FaultingAddress == 0xFFFF'FFFF'FFFF'F000ULL);
    CheckError(runner, MakePlan(0x0000'0000'0010'0000ULL, PhysicalBase, 1U), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::RegionZeroReserved,
               "runtime region zero remains reserved for shared pinned seed mappings");
    CheckError(runner, MakePlan(0x3FFF'FFFF'FFFF'F000ULL, PhysicalBase, 2U), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::CrossRegionMapping,
               "mapping crossing an IA-64 region boundary is rejected");

    TranslationCapabilities capabilities = FullCapabilities();
    capabilities.ImplementedVirtualAddressMostSignificantBit = 53U;
    CheckError(runner, MakePlan(0x2040'0000'0000'0000ULL, PhysicalBase, 1U), 0U, capabilities,
               Ia64RuntimeTranslationEncodingError::UnimplementedVirtualAddress,
               "non-sign-extended payload above IMPL_VA_MSB is rejected");
    CheckError(runner, MakePlan(0x201F'FFFF'FFFF'F000ULL, PhysicalBase, 2U), 0U, capabilities,
               Ia64RuntimeTranslationEncodingError::UnimplementedVirtualAddress,
               "implemented first page cannot hide an unimplemented plan tail");
    const auto signExtendedResult = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(0x3FFF'FFFF'FFFF'E000ULL, PhysicalBase, 1U), 0U, capabilities);
    ASTERIA_EXPECT(runner, "properly sign-extended payload in a nonzero region is accepted",
                   signExtendedResult.HasValue(),
                   signExtendedResult.GetValue().FaultingAddress == 0x3FFF'FFFF'FFFF'E000ULL);
}

void TestPhysicalLimits(TestContext& runner) noexcept
{
    TranslationCapabilities capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 44U;
    const Foundation::UInt64 last44BitPage = (Foundation::UInt64{1U} << 44U) - 0x1000U;
    const auto boundaryResult =
        Ia64RuntimeTranslationEncoder::EncodePage(MakePlan(RegionFiveBase, last44BitPage, 1U), 0U, capabilities);
    ASTERIA_EXPECT(runner, "last page below the PAL physical-width limit is accepted", boundaryResult.HasValue(),
                   (boundaryResult.GetValue().TranslationWord & 0x0003'FFFF'FFFF'F000ULL) == last44BitPage);
    const auto rangeBoundaryResult = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(RegionFiveBase, last44BitPage - 0x1000U, 2U), 0U, capabilities);
    ASTERIA_EXPECT(runner, "complete physical plan ending at the PAL width boundary is accepted",
                   rangeBoundaryResult.HasValue());
    CheckError(runner, MakePlan(RegionFiveBase, Foundation::UInt64{1U} << 44U, 1U), 0U, capabilities,
               Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange,
               "page at the PAL physical-width limit is rejected");
    CheckError(runner, MakePlan(RegionFiveBase, last44BitPage, 2U), 0U, capabilities,
               Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange,
               "page zero cannot hide a later page beyond the PAL physical-width limit");

    capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 51U;
    const Foundation::UInt64 lastPpnPage = (Foundation::UInt64{1U} << 50U) - 0x1000U;
    const auto ppnBoundaryResult =
        Ia64RuntimeTranslationEncoder::EncodePage(MakePlan(RegionFiveBase, lastPpnPage, 1U), 0U, capabilities);
    ASTERIA_EXPECT(runner, "last page in the IA-64 50-bit PPN field is accepted", ppnBoundaryResult.HasValue());
    const auto ppnRangeBoundaryResult = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(RegionFiveBase, lastPpnPage - 0x1000U, 2U), 0U, capabilities);
    ASTERIA_EXPECT(runner, "complete physical plan ending at the 50-bit PPN boundary is accepted",
                   ppnRangeBoundaryResult.HasValue());
    CheckError(runner, MakePlan(RegionFiveBase, Foundation::UInt64{1U} << 50U, 1U), 0U, capabilities,
               Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange,
               "page beyond the IA-64 50-bit PPN field is rejected");
    CheckError(runner, MakePlan(RegionFiveBase, lastPpnPage, 2U), 0U, capabilities,
               Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange,
               "page zero cannot hide a later page beyond the IA-64 50-bit PPN field");

    const auto zeroResult =
        Ia64RuntimeTranslationEncoder::EncodePage(MakePlan(RegionFiveBase, 0U, 1U), 0U, FullCapabilities());
    ASTERIA_EXPECT(runner, "physical page zero is encodable even though authorized Frames exclude it",
                   zeroResult.HasValue(), (zeroResult.GetValue().TranslationWord & 0x0003'FFFF'FFFF'F000ULL) == 0U);
}

void TestCapabilities(TestContext& runner) noexcept
{
    TranslationCapabilities capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 31U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "physical width below 32 is invalid");
    capabilities = FullCapabilities();
    capabilities.PhysicalAddressBits = 64U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "physical width above the PAL field contract is invalid");
    capabilities = FullCapabilities();
    capabilities.ImplementedVirtualAddressMostSignificantBit = 49U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "implemented VA MSB below 50 is invalid");
    capabilities = FullCapabilities();
    capabilities.ImplementedVirtualAddressMostSignificantBit = 61U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "implemented VA MSB overlapping the region number is invalid");
    capabilities = FullCapabilities();
    capabilities.InsertablePageSizeMask = 0U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::UnsupportedPageSize,
               "PAL capability must admit the fixed PS 12 insertion");
    capabilities = FullCapabilities();
    capabilities.InsertablePageSizeMask |= Foundation::UInt64{1U} << 11U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "sub-4 KiB PAL page-size bits are invalid");
    capabilities = FullCapabilities();
    capabilities.MemoryAttributeMask |= Foundation::UInt8{1U} << 1U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "reserved PAL memory-attribute bits are invalid");
    capabilities = FullCapabilities();
    capabilities.Reserved16 = 1U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "nonzero 16-bit capability reserve is invalid");
    capabilities = FullCapabilities();
    capabilities.Reserved64 = 1U;
    CheckError(runner, MakePlan(), 0U, capabilities, Ia64RuntimeTranslationEncodingError::InvalidCapabilities,
               "nonzero 64-bit capability reserve is invalid");

    capabilities = FullCapabilities();
    capabilities.RegionIdentifierBits = 0U;
    capabilities.ProtectionKeyBits = 0U;
    capabilities.HardwareWalkerImplemented = false;
    const auto irrelevantResult = Ia64RuntimeTranslationEncoder::EncodePage(MakePlan(), 0U, capabilities);
    ASTERIA_EXPECT(runner,
                   "RID, protection-key width, and walker fields are not consumed by "
                   "key-zero software refill encoding",
                   irrelevantResult.HasValue(),
                   (irrelevantResult.GetValue().TranslationInsertion & TranslationInsertionKeyMask) == 0U);
}

void TestMalformedPlans(TestContext& runner) noexcept
{
    AddressSpaceMappingRecord plan = MakePlan();
    plan.RuntimeMapping = false;
    CheckError(runner, plan, 0U, FullCapabilities(), Ia64RuntimeTranslationEncodingError::InvalidMappingPlan,
               "non-runtime records are rejected");
    plan = MakePlan();
    plan.Reserved[2] = 1U;
    CheckError(runner, plan, 0U, FullCapabilities(), Ia64RuntimeTranslationEncodingError::InvalidMappingPlan,
               "nonzero plan reserved state is rejected");
    plan = MakePlan();
    plan.Kind = static_cast<FrameKind>(0xFFU);
    CheckError(runner, plan, 0U, FullCapabilities(), Ia64RuntimeTranslationEncodingError::InvalidMappingPlan,
               "malformed FrameKind is rejected");

    const PagePermissions unknownPermissions = std::bit_cast<PagePermissions>(Foundation::UInt8{0x11U});
    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, 1U, unknownPermissions), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::UnsupportedPermissions, "unknown permission bits are rejected");
    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, 1U, PagePermissions{}), 0U, FullCapabilities(),
               Ia64RuntimeTranslationEncodingError::UnsupportedPermissions,
               "mapping without required read permission is rejected");
    CheckError(runner, MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadWrite().With(PagePermission::Execute)), 0U,
               FullCapabilities(), Ia64RuntimeTranslationEncodingError::UnsupportedPermissions,
               "writable executable mapping is rejected");
    CheckError(
        runner,
        MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadExecute(), MemoryCachePolicy::Uncached, FrameKind::Device), 0U,
        FullCapabilities(), Ia64RuntimeTranslationEncodingError::DeviceExecutionDenied, "device execution is rejected");

    const auto deviceDataResult = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadWrite(), MemoryCachePolicy::WriteCombining, FrameKind::Device),
        0U, FullCapabilities());
    ASTERIA_EXPECT(runner, "non-executable device data uses the ordinary admitted MA encoding",
                   deviceDataResult.HasValue());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeTranslationEncoderTests"};
    Asteria::Test::TestPermissionsAndExactBits(runner);
    Asteria::Test::TestMemoryAttributes(runner);
    Asteria::Test::TestOffsetsAndPlanBounds(runner);
    Asteria::Test::TestVirtualRegionsAndImplementation(runner);
    Asteria::Test::TestPhysicalLimits(runner);
    Asteria::Test::TestCapabilities(runner);
    Asteria::Test::TestMalformedPlans(runner);
    return runner.Finish();
}
