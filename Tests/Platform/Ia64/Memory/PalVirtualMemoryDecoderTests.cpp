// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::PalStaticCallResult;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::PalVirtualMemoryDecodeError;
using Platform::Ia64::Memory::PalVirtualMemoryDecoder;

constexpr Foundation::UInt64 AdditionalPageSizeMask{(Foundation::UInt64{1U} << 15U) | (Foundation::UInt64{1U} << 30U)};
constexpr Foundation::UInt8 FullMemoryAttributeMask{(Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) |
                                                    (Foundation::UInt8{1U} << 5U) | (Foundation::UInt8{1U} << 6U)};

[[nodiscard]] constexpr Foundation::UInt64 MakeSummary1(const bool walker, const Foundation::UInt8 physicalAddressBits,
                                                        const Foundation::UInt8 protectionKeyBits,
                                                        const Foundation::UInt8 maximumProtectionKeyRegister,
                                                        const Foundation::UInt8 hashTagIdentifier,
                                                        const Foundation::UInt8 maximumDataTranslationRegister,
                                                        const Foundation::UInt8 maximumInstructionTranslationRegister,
                                                        const Foundation::UInt8 uniqueTranslationCacheCount,
                                                        const Foundation::UInt8 translationCacheLevelCount) noexcept
{
    return (walker ? Foundation::UInt64{1U} : Foundation::UInt64{0U}) |
           (static_cast<Foundation::UInt64>(physicalAddressBits) << 1U) |
           (static_cast<Foundation::UInt64>(protectionKeyBits) << 8U) |
           (static_cast<Foundation::UInt64>(maximumProtectionKeyRegister) << 16U) |
           (static_cast<Foundation::UInt64>(hashTagIdentifier) << 24U) |
           (static_cast<Foundation::UInt64>(maximumDataTranslationRegister) << 32U) |
           (static_cast<Foundation::UInt64>(maximumInstructionTranslationRegister) << 40U) |
           (static_cast<Foundation::UInt64>(uniqueTranslationCacheCount) << 48U) |
           (static_cast<Foundation::UInt64>(translationCacheLevelCount) << 56U);
}

[[nodiscard]] constexpr Foundation::UInt64
MakeSummary2(const Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
             const Foundation::UInt8 regionIdentifierBits,
             const Foundation::UInt16 maximumConcurrentPurgesEncoding) noexcept
{
    return static_cast<Foundation::UInt64>(implementedVirtualAddressMostSignificantBit) |
           (static_cast<Foundation::UInt64>(regionIdentifierBits) << 8U) |
           (static_cast<Foundation::UInt64>(maximumConcurrentPurgesEncoding) << 16U);
}

[[nodiscard]] constexpr PalStaticCallResult FullSummary() noexcept
{
    return PalStaticCallResult{
        .Status = 0,
        .Result1 = MakeSummary1(true, 50U, 24U, 15U, 8U, 31U, 31U, 4U, 2U),
        .Result2 = MakeSummary2(60U, 24U, 0x1234U),
        .Result3 = 0U,
    };
}

[[nodiscard]] constexpr PalStaticCallResult FullPageSizes() noexcept
{
    return PalStaticCallResult{
        .Status = 0,
        .Result1 = ArchitectedMinimumInsertablePageSizeMask | AdditionalPageSizeMask,
        .Result2 = ArchitectedMinimumPurgePageSizeMask | AdditionalPageSizeMask,
        .Result3 = 0U,
    };
}

[[nodiscard]] constexpr PalStaticCallResult FullMemoryAttributes() noexcept
{
    return PalStaticCallResult{
        .Status = 0,
        .Result1 = FullMemoryAttributeMask,
        .Result2 = 0U,
        .Result3 = 0U,
    };
}

[[nodiscard]] bool HasError(const PalStaticCallResult& summary, const PalStaticCallResult& pageSizes,
                            const PalStaticCallResult& memoryAttributes,
                            const PalVirtualMemoryDecodeError expected) noexcept
{
    const auto result = PalVirtualMemoryDecoder::Decode(summary, pageSizes, memoryAttributes);
    return !result.HasValue() && (result.GetError() == expected);
}

void TestExactDecode(TestContext& runner) noexcept
{
    const auto result = PalVirtualMemoryDecoder::Decode(FullSummary(), FullPageSizes(), FullMemoryAttributes());
    const PalVirtualMemoryCapabilities expected{
        .Translation =
            {
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask | AdditionalPageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = 24U,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = FullMemoryAttributeMask,
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask | AdditionalPageSizeMask,
        .InstructionTranslationRegisterCount = 32U,
        .DataTranslationRegisterCount = 32U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 0x1234U,
        .HashTagIdentifier = 8U,
        .UniqueTranslationCacheCount = 4U,
        .TranslationCacheLevelCount = 2U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
    ASTERIA_EXPECT(
        runner, "PAL return fields decode into the exact validated capability record", result.HasValue(),
        result.GetValue().Translation.InsertablePageSizeMask == expected.Translation.InsertablePageSizeMask,
        result.GetValue().Translation.PhysicalAddressBits == expected.Translation.PhysicalAddressBits,
        result.GetValue().Translation.ImplementedVirtualAddressMostSignificantBit ==
            expected.Translation.ImplementedVirtualAddressMostSignificantBit,
        result.GetValue().Translation.RegionIdentifierBits == expected.Translation.RegionIdentifierBits,
        result.GetValue().Translation.ProtectionKeyBits == expected.Translation.ProtectionKeyBits,
        result.GetValue().Translation.MemoryAttributeMask == expected.Translation.MemoryAttributeMask,
        result.GetValue().Translation.HardwareWalkerImplemented, result.GetValue().Translation.Reserved16 == 0U,
        result.GetValue().Translation.Reserved64 == 0U,
        result.GetValue().PurgePageSizeMask == expected.PurgePageSizeMask,
        result.GetValue().InstructionTranslationRegisterCount == 32U,
        result.GetValue().DataTranslationRegisterCount == 32U, result.GetValue().ProtectionKeyRegisterCount == 16U,
        result.GetValue().MaximumConcurrentPurgesEncoding == 0x1234U, result.GetValue().HashTagIdentifier == 8U,
        result.GetValue().UniqueTranslationCacheCount == 4U, result.GetValue().TranslationCacheLevelCount == 2U,
        result.GetValue().Reserved8 == 0U, result.GetValue().Reserved32 == 0U);
}

void TestCallAndReservedFailures(TestContext& runner) noexcept
{
    PalStaticCallResult summary = FullSummary();
    summary.Status = -2;
    ASTERIA_EXPECT(
        runner, "failed PAL_VM_SUMMARY is rejected",
        HasError(summary, FullPageSizes(), FullMemoryAttributes(), PalVirtualMemoryDecodeError::SummaryCallFailed));

    PalStaticCallResult pageSizes = FullPageSizes();
    pageSizes.Status = -3;
    ASTERIA_EXPECT(
        runner, "failed PAL_VM_PAGE_SIZE is rejected",
        HasError(FullSummary(), pageSizes, FullMemoryAttributes(), PalVirtualMemoryDecodeError::PageSizeCallFailed));

    PalStaticCallResult memoryAttributes = FullMemoryAttributes();
    memoryAttributes.Status = 1;
    ASTERIA_EXPECT(runner, "failed PAL_MEM_ATTRIB is rejected",
                   HasError(FullSummary(), FullPageSizes(), memoryAttributes,
                            PalVirtualMemoryDecodeError::MemoryAttributeCallFailed));

    summary = FullSummary();
    summary.Result2 |= Foundation::UInt64{1U} << 32U;
    ASTERIA_EXPECT(
        runner, "reserved PAL_VM_SUMMARY result2 bits are rejected",
        HasError(summary, FullPageSizes(), FullMemoryAttributes(), PalVirtualMemoryDecodeError::SummaryReservedResult));
    summary = FullSummary();
    summary.Result3 = 1U;
    ASTERIA_EXPECT(
        runner, "reserved PAL_VM_SUMMARY result3 is rejected",
        HasError(summary, FullPageSizes(), FullMemoryAttributes(), PalVirtualMemoryDecodeError::SummaryReservedResult));

    pageSizes = FullPageSizes();
    pageSizes.Result3 = 1U;
    ASTERIA_EXPECT(runner, "reserved PAL_VM_PAGE_SIZE result is rejected",
                   HasError(FullSummary(), pageSizes, FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::PageSizeReservedResult));

    bool allMemoryReservedRejected{true};
    for (Foundation::UInt8 resultIndex{0U}; resultIndex < 3U; ++resultIndex)
    {
        memoryAttributes = FullMemoryAttributes();
        if (resultIndex == 0U)
        {
            memoryAttributes.Result1 |= Foundation::UInt64{1U} << 8U;
        }
        else if (resultIndex == 1U)
        {
            memoryAttributes.Result2 = 1U;
        }
        else
        {
            memoryAttributes.Result3 = 1U;
        }
        allMemoryReservedRejected =
            allMemoryReservedRejected && HasError(FullSummary(), FullPageSizes(), memoryAttributes,
                                                  PalVirtualMemoryDecodeError::MemoryAttributeReservedResult);
    }
    ASTERIA_EXPECT(runner, "every reserved PAL_MEM_ATTRIB return field is rejected", allMemoryReservedRejected);
}

void TestSummaryCapabilityFailures(TestContext& runner) noexcept
{
    PalStaticCallResult summary = FullSummary();
    summary.Result1 = MakeSummary1(true, 31U, 24U, 15U, 8U, 31U, 31U, 4U, 2U);
    ASTERIA_EXPECT(runner, "physical width below 32 bits is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidAddressCapabilities));
    summary.Result1 = MakeSummary1(true, 64U, 24U, 15U, 8U, 31U, 31U, 4U, 2U);
    ASTERIA_EXPECT(runner, "physical width above the 63-bit address space is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidAddressCapabilities));

    summary = FullSummary();
    summary.Result2 = MakeSummary2(49U, 24U, 0U);
    ASTERIA_EXPECT(runner, "implemented virtual-address bit below 50 is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidAddressCapabilities));
    summary.Result2 = MakeSummary2(61U, 24U, 0U);
    ASTERIA_EXPECT(runner, "implemented virtual-address bit overlapping the VRN is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidAddressCapabilities));
    summary.Result2 = MakeSummary2(60U, 17U, 0U);
    ASTERIA_EXPECT(runner, "RID width below 18 bits is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidAddressCapabilities));
    summary.Result2 = MakeSummary2(60U, 25U, 0U);
    ASTERIA_EXPECT(runner, "RID width above 24 bits is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidAddressCapabilities));

    summary = FullSummary();
    summary.Result1 = MakeSummary1(true, 50U, 17U, 15U, 8U, 31U, 31U, 4U, 2U);
    ASTERIA_EXPECT(runner, "protection-key width below 18 bits is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidProtectionCapabilities));
    summary.Result1 = MakeSummary1(true, 50U, 25U, 15U, 8U, 31U, 31U, 4U, 2U);
    ASTERIA_EXPECT(runner, "protection-key width above 24 bits is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidProtectionCapabilities));
    summary.Result1 = MakeSummary1(true, 50U, 23U, 15U, 8U, 31U, 31U, 4U, 2U);
    ASTERIA_EXPECT(runner, "protection-key width smaller than RID width is rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidProtectionCapabilities));
    summary.Result1 = MakeSummary1(true, 50U, 24U, 14U, 8U, 31U, 31U, 4U, 2U);
    ASTERIA_EXPECT(runner, "fewer than 16 protection-key registers are rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidProtectionCapabilities));

    summary = FullSummary();
    summary.Result1 = MakeSummary1(true, 50U, 24U, 15U, 8U, 6U, 31U, 4U, 2U);
    ASTERIA_EXPECT(runner, "fewer than eight data translation registers are rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidTranslationRegisterCapabilities));
    summary.Result1 = MakeSummary1(true, 50U, 24U, 15U, 8U, 31U, 6U, 4U, 2U);
    ASTERIA_EXPECT(runner, "fewer than eight instruction translation registers are rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidTranslationRegisterCapabilities));

    summary = FullSummary();
    summary.Result1 = MakeSummary1(true, 50U, 24U, 15U, 8U, 31U, 31U, 1U, 0U);
    ASTERIA_EXPECT(runner, "zero translation-cache levels are rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidTranslationCacheTopology));
    summary.Result1 = MakeSummary1(true, 50U, 24U, 15U, 8U, 31U, 31U, 0U, 1U);
    ASTERIA_EXPECT(runner, "zero unique translation caches are rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidTranslationCacheTopology));
    summary.Result1 = MakeSummary1(true, 50U, 24U, 15U, 8U, 31U, 31U, 5U, 2U);
    ASTERIA_EXPECT(runner, "more than two unique caches per cache level are rejected",
                   HasError(summary, FullPageSizes(), FullMemoryAttributes(),
                            PalVirtualMemoryDecodeError::InvalidTranslationCacheTopology));
}

void TestPageAndAttributeFailures(TestContext& runner) noexcept
{
    const Foundation::UInt8 mandatoryInsertableShifts[10]{12U, 13U, 14U, 16U, 18U, 20U, 22U, 24U, 26U, 28U};
    bool everyInsertableBitRequired{true};
    for (const Foundation::UInt8 shift : mandatoryInsertableShifts)
    {
        PalStaticCallResult pageSizes = FullPageSizes();
        pageSizes.Result1 &= ~(Foundation::UInt64{1U} << shift);
        everyInsertableBitRequired =
            everyInsertableBitRequired && HasError(FullSummary(), pageSizes, FullMemoryAttributes(),
                                                   PalVirtualMemoryDecodeError::MissingMandatoryInsertablePageSize);
    }
    ASTERIA_EXPECT(runner, "every architecturally mandatory insertable page size is required",
                   everyInsertableBitRequired);

    const Foundation::UInt8 mandatoryPurgeShifts[11]{12U, 13U, 14U, 16U, 18U, 20U, 22U, 24U, 26U, 28U, 32U};
    bool everyPurgeBitRequired{true};
    for (const Foundation::UInt8 shift : mandatoryPurgeShifts)
    {
        PalStaticCallResult pageSizes = FullPageSizes();
        pageSizes.Result2 &= ~(Foundation::UInt64{1U} << shift);
        everyPurgeBitRequired =
            everyPurgeBitRequired && HasError(FullSummary(), pageSizes, FullMemoryAttributes(),
                                              PalVirtualMemoryDecodeError::MissingMandatoryPurgePageSize);
    }
    ASTERIA_EXPECT(runner, "every architecturally mandatory purge page size is required", everyPurgeBitRequired);

    bool everySubFourKibibyteBitRejected{true};
    for (Foundation::UInt8 shift{0U}; shift < 12U; ++shift)
    {
        PalStaticCallResult pageSizes = FullPageSizes();
        const Foundation::UInt64 bit = Foundation::UInt64{1U} << shift;
        pageSizes.Result1 |= bit;
        pageSizes.Result2 |= bit;
        everySubFourKibibyteBitRejected =
            everySubFourKibibyteBitRejected && HasError(FullSummary(), pageSizes, FullMemoryAttributes(),
                                                        PalVirtualMemoryDecodeError::PageSizeBelowArchitectedMinimum);

        pageSizes = FullPageSizes();
        pageSizes.Result2 |= bit;
        everySubFourKibibyteBitRejected =
            everySubFourKibibyteBitRejected && HasError(FullSummary(), pageSizes, FullMemoryAttributes(),
                                                        PalVirtualMemoryDecodeError::PageSizeBelowArchitectedMinimum);
    }
    ASTERIA_EXPECT(runner, "all insertion and purge sizes below the 4 KiB minimum are rejected",
                   everySubFourKibibyteBitRejected);

    bool everyOptionalInsertableSizeRequiresPurge{true};
    for (Foundation::UInt8 shift{12U}; shift < 64U; ++shift)
    {
        const Foundation::UInt64 bit = Foundation::UInt64{1U} << shift;
        if ((ArchitectedMinimumPurgePageSizeMask & bit) != 0U)
        {
            continue;
        }
        PalStaticCallResult pageSizes = FullPageSizes();
        pageSizes.Result1 |= bit;
        pageSizes.Result2 &= ~bit;
        everyOptionalInsertableSizeRequiresPurge =
            everyOptionalInsertableSizeRequiresPurge &&
            HasError(FullSummary(), pageSizes, FullMemoryAttributes(),
                     PalVirtualMemoryDecodeError::InsertablePageSizeNotPurgeable);
    }
    ASTERIA_EXPECT(runner, "every optional insertable page size must also be purgeable",
                   everyOptionalInsertableSizeRequiresPurge);

    PalStaticCallResult purgeOnlyPageSize = FullPageSizes();
    purgeOnlyPageSize.Result2 |= Foundation::UInt64{1U} << 33U;
    ASTERIA_EXPECT(
        runner, "a processor-specific purge-only page size remains valid",
        PalVirtualMemoryDecoder::Decode(FullSummary(), purgeOnlyPageSize, FullMemoryAttributes()).HasValue());

    bool everyReservedAttributeRejected{true};
    for (Foundation::UInt8 shift{1U}; shift <= 3U; ++shift)
    {
        PalStaticCallResult memoryAttributes = FullMemoryAttributes();
        memoryAttributes.Result1 |= Foundation::UInt64{1U} << shift;
        everyReservedAttributeRejected =
            everyReservedAttributeRejected && HasError(FullSummary(), FullPageSizes(), memoryAttributes,
                                                       PalVirtualMemoryDecodeError::InvalidMemoryAttributeCapabilities);
    }
    ASTERIA_EXPECT(runner, "every reserved translation memory-attribute encoding is rejected",
                   everyReservedAttributeRejected);

    PalStaticCallResult memoryAttributes = FullMemoryAttributes();
    memoryAttributes.Result1 &= ~(Foundation::UInt64{1U} << 0U);
    ASTERIA_EXPECT(runner, "kernel profile requires write-back memory",
                   HasError(FullSummary(), FullPageSizes(), memoryAttributes,
                            PalVirtualMemoryDecodeError::MissingRequiredMemoryAttribute));
    memoryAttributes = FullMemoryAttributes();
    memoryAttributes.Result1 &= ~(Foundation::UInt64{1U} << 4U);
    ASTERIA_EXPECT(runner, "kernel profile requires uncached device memory",
                   HasError(FullSummary(), FullPageSizes(), memoryAttributes,
                            PalVirtualMemoryDecodeError::MissingRequiredMemoryAttribute));
}

void TestBoundaryCapabilities(TestContext& runner) noexcept
{
    PalStaticCallResult summary{
        .Status = 0,
        .Result1 = MakeSummary1(false, 32U, 18U, 15U, 0U, 7U, 7U, 2U, 1U),
        .Result2 = MakeSummary2(53U, 18U, 0U),
        .Result3 = 0U,
    };
    auto result = PalVirtualMemoryDecoder::Decode(summary, FullPageSizes(), FullMemoryAttributes());
    ASTERIA_EXPECT(runner, "architectural minimum capabilities decode without a hardware walker", result.HasValue(),
                   !result.GetValue().Translation.HardwareWalkerImplemented,
                   result.GetValue().Translation.PhysicalAddressBits == 32U,
                   result.GetValue().Translation.ImplementedVirtualAddressMostSignificantBit == 53U,
                   result.GetValue().Translation.RegionIdentifierBits == 18U,
                   result.GetValue().Translation.ProtectionKeyBits == 18U,
                   result.GetValue().InstructionTranslationRegisterCount == 8U,
                   result.GetValue().DataTranslationRegisterCount == 8U,
                   result.GetValue().ProtectionKeyRegisterCount == 16U);

    summary = PalStaticCallResult{
        .Status = 0,
        .Result1 = MakeSummary1(true, 63U, 24U, 255U, 255U, 255U, 255U, 255U, 128U),
        .Result2 = MakeSummary2(60U, 24U, 0xFFFFU),
        .Result3 = 0U,
    };
    result = PalVirtualMemoryDecoder::Decode(summary, FullPageSizes(), FullMemoryAttributes());
    ASTERIA_EXPECT(
        runner, "maximum encoded counts widen without wrapping and preserve PAL special values", result.HasValue(),
        result.GetValue().InstructionTranslationRegisterCount == 256U,
        result.GetValue().DataTranslationRegisterCount == 256U, result.GetValue().ProtectionKeyRegisterCount == 256U,
        result.GetValue().MaximumConcurrentPurgesEncoding == 0xFFFFU, result.GetValue().HashTagIdentifier == 255U,
        result.GetValue().UniqueTranslationCacheCount == 255U, result.GetValue().TranslationCacheLevelCount == 128U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"PalVirtualMemoryDecoderTests"};
    Asteria::Test::TestExactDecode(runner);
    Asteria::Test::TestCallAndReservedFailures(runner);
    Asteria::Test::TestSummaryCapabilityFailures(runner);
    Asteria::Test::TestPageAndAttributeFailures(runner);
    Asteria::Test::TestBoundaryCapabilities(runner);
    return runner.Finish();
}
