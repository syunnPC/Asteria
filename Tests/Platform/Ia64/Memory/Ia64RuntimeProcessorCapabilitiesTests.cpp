// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Ia64/Memory/Ia64RuntimeProcessorCapabilities.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Memory::Ia64CommonRuntimeProcessorCapabilities;
using Platform::Ia64::Memory::Ia64RuntimeProcessorCapabilities;
using Platform::Ia64::Memory::Ia64RuntimeProcessorCapabilitiesError;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::TranslationCapabilities;

constexpr Foundation::UInt64 Page4KiB{Foundation::UInt64{1U} << 12U};
constexpr Foundation::UInt64 Page16KiB{Foundation::UInt64{1U} << 14U};
constexpr Foundation::UInt64 Page64KiB{Foundation::UInt64{1U} << 16U};
constexpr Foundation::UInt64 Page256KiB{Foundation::UInt64{1U} << 18U};
constexpr Foundation::UInt64 Page1MiB{Foundation::UInt64{1U} << 20U};
constexpr Foundation::UInt64 Page256MiB{Foundation::UInt64{1U} << 28U};
constexpr Foundation::UInt64 Page4GiB{Foundation::UInt64{1U} << 32U};
constexpr Foundation::UInt8 RequiredMemoryAttributes{0x11U};

[[nodiscard]] constexpr PalVirtualMemoryCapabilities MakeCapabilities(
    const Foundation::UInt64 insertablePageSizeMask = Page4KiB, const Foundation::UInt64 purgePageSizeMask = Page4KiB,
    const Foundation::UInt8 physicalAddressBits = 48U, const Foundation::UInt8 virtualAddressMostSignificantBit = 57U,
    const Foundation::UInt8 regionIdentifierBits = 20U,
    const Foundation::UInt8 memoryAttributeMask = RequiredMemoryAttributes, const bool hardwareWalkerImplemented = true,
    const Foundation::UInt8 marker = 0U) noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = insertablePageSizeMask,
                .PhysicalAddressBits = physicalAddressBits,
                .ImplementedVirtualAddressMostSignificantBit = virtualAddressMostSignificantBit,
                .RegionIdentifierBits = regionIdentifierBits,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = memoryAttributeMask,
                .HardwareWalkerImplemented = hardwareWalkerImplemented,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = purgePageSizeMask,
        .InstructionTranslationRegisterCount = static_cast<Foundation::UInt16>(Foundation::UInt16{8U} + marker),
        .DataTranslationRegisterCount = static_cast<Foundation::UInt16>(Foundation::UInt16{9U} + marker),
        .ProtectionKeyRegisterCount = static_cast<Foundation::UInt16>(Foundation::UInt16{16U} + marker),
        .MaximumConcurrentPurgesEncoding = marker,
        .HashTagIdentifier = marker,
        .UniqueTranslationCacheCount = 1U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr bool Equal(const PalVirtualMemoryCapabilities& first,
                                   const PalVirtualMemoryCapabilities& second) noexcept
{
    return (first.Translation.InsertablePageSizeMask == second.Translation.InsertablePageSizeMask) &&
           (first.Translation.PhysicalAddressBits == second.Translation.PhysicalAddressBits) &&
           (first.Translation.ImplementedVirtualAddressMostSignificantBit ==
            second.Translation.ImplementedVirtualAddressMostSignificantBit) &&
           (first.Translation.RegionIdentifierBits == second.Translation.RegionIdentifierBits) &&
           (first.Translation.ProtectionKeyBits == second.Translation.ProtectionKeyBits) &&
           (first.Translation.MemoryAttributeMask == second.Translation.MemoryAttributeMask) &&
           (first.Translation.HardwareWalkerImplemented == second.Translation.HardwareWalkerImplemented) &&
           (first.Translation.Reserved16 == second.Translation.Reserved16) &&
           (first.Translation.Reserved64 == second.Translation.Reserved64) &&
           (first.PurgePageSizeMask == second.PurgePageSizeMask) &&
           (first.InstructionTranslationRegisterCount == second.InstructionTranslationRegisterCount) &&
           (first.DataTranslationRegisterCount == second.DataTranslationRegisterCount) &&
           (first.ProtectionKeyRegisterCount == second.ProtectionKeyRegisterCount) &&
           (first.MaximumConcurrentPurgesEncoding == second.MaximumConcurrentPurgesEncoding) &&
           (first.HashTagIdentifier == second.HashTagIdentifier) &&
           (first.UniqueTranslationCacheCount == second.UniqueTranslationCacheCount) &&
           (first.TranslationCacheLevelCount == second.TranslationCacheLevelCount) &&
           (first.Reserved8 == second.Reserved8) && (first.Reserved32 == second.Reserved32);
}

[[nodiscard]] constexpr bool Equal(const Ia64CommonRuntimeProcessorCapabilities& actual,
                                   const Ia64CommonRuntimeProcessorCapabilities& expected) noexcept
{
    return (actual.InsertablePageSizeMask == expected.InsertablePageSizeMask) &&
           (actual.PurgePageSizeMask == expected.PurgePageSizeMask) &&
           (actual.PhysicalAddressBits == expected.PhysicalAddressBits) &&
           (actual.ImplementedVirtualAddressMostSignificantBit ==
            expected.ImplementedVirtualAddressMostSignificantBit) &&
           (actual.RegionIdentifierBits == expected.RegionIdentifierBits) &&
           (actual.MemoryAttributeMask == expected.MemoryAttributeMask) &&
           (actual.HardwareWalkerImplemented == expected.HardwareWalkerImplemented) &&
           (actual.Reserved[0U] == expected.Reserved[0U]) && (actual.Reserved[1U] == expected.Reserved[1U]) &&
           (actual.Reserved[2U] == expected.Reserved[2U]);
}

[[nodiscard]] bool IsUninitialized(const Ia64RuntimeProcessorCapabilities& profile) noexcept
{
    const auto mask = profile.GetAdmittedProcessorMask();
    const auto common = profile.GetCommonCapabilities();
    const auto processor = profile.GetProcessorCapabilities(CpuId{0U});
    return !mask.HasValue() && (mask.GetError() == Ia64RuntimeProcessorCapabilitiesError::NotInitialized) &&
           !common.HasValue() && (common.GetError() == Ia64RuntimeProcessorCapabilitiesError::NotInitialized) &&
           !processor.HasValue() && (processor.GetError() == Ia64RuntimeProcessorCapabilitiesError::NotInitialized);
}

[[nodiscard]] bool
FailureIsAtomic(const Foundation::UInt8 admittedProcessorMask,
                const PalVirtualMemoryCapabilities (&capabilities)[Kernel::Scheduler::MaximumProcessorCount],
                const Ia64RuntimeProcessorCapabilitiesError expected) noexcept
{
    Ia64RuntimeProcessorCapabilities profile;
    if ((profile.Initialize(admittedProcessorMask, capabilities) != expected) || !IsUninitialized(profile))
    {
        return false;
    }

    PalVirtualMemoryCapabilities retryCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    retryCapabilities[0U] = MakeCapabilities();
    return (profile.Initialize(0x01U, retryCapabilities) == Ia64RuntimeProcessorCapabilitiesError::None) &&
           profile.GetCommonCapabilities().HasValue();
}

void TestCpu0ProfileAndOfflineInputs(TestContext& runner) noexcept
{
    Ia64RuntimeProcessorCapabilities profile;
    ASTERIA_EXPECT(runner, "an unpublished default profile rejects every lookup", IsUninitialized(profile));

    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    capabilities[0U] =
        MakeCapabilities(Page4KiB | Page64KiB, Page4KiB | Page64KiB | Page4GiB, 50U, 59U, 22U, 0xD1U, true, 7U);
    for (Foundation::UInt8 processor{1U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        capabilities[processor] = MakeCapabilities();
        capabilities[processor].Translation.PhysicalAddressBits = 0U;
        capabilities[processor].Translation.InsertablePageSizeMask = 1U;
        capabilities[processor].Translation.MemoryAttributeMask = 0xFFU;
        capabilities[processor].Translation.Reserved64 = processor;
        capabilities[processor].Reserved32 = processor;
    }

    ASTERIA_EXPECT(runner, "a CPU0-only profile ignores malformed offline array elements",
                   profile.Initialize(0x01U, capabilities) == Ia64RuntimeProcessorCapabilitiesError::None);

    const auto mask = profile.GetAdmittedProcessorMask();
    const auto exact = profile.GetProcessorCapabilities(CpuId{0U});
    const auto common = profile.GetCommonCapabilities();
    const Ia64CommonRuntimeProcessorCapabilities expectedCommon{
        .InsertablePageSizeMask = Page4KiB | Page64KiB,
        .PurgePageSizeMask = Page4KiB | Page64KiB | Page4GiB,
        .PhysicalAddressBits = 50U,
        .ImplementedVirtualAddressMostSignificantBit = 59U,
        .RegionIdentifierBits = 22U,
        .MemoryAttributeMask = 0xD1U,
        .HardwareWalkerImplemented = true,
        .Reserved = {},
    };
    ASTERIA_EXPECT(runner, "the admitted CPU0 mask is exposed by value", mask.HasValue(), mask.GetValue() == 0x01U);
    ASTERIA_EXPECT(runner, "the exact admitted CPU0 decoder record is retained", exact.HasValue(),
                   Equal(exact.GetValue(), capabilities[0U]));
    ASTERIA_EXPECT(runner, "a CPU0-only common subset equals its consumed PAL fields", common.HasValue(),
                   Equal(common.GetValue(), expectedCommon));

    const auto offline = profile.GetProcessorCapabilities(CpuId{1U});
    const auto invalidFour = profile.GetProcessorCapabilities(CpuId{4U});
    const auto invalidMaximum = profile.GetProcessorCapabilities(CpuId{0xFFU});
    ASTERIA_EXPECT(runner, "lookup rejects a valid but unadmitted processor ID", !offline.HasValue(),
                   offline.GetError() == Ia64RuntimeProcessorCapabilitiesError::ProcessorNotAdmitted);
    ASTERIA_EXPECT(runner, "lookup rejects processor IDs outside the four-entry capacity", !invalidFour.HasValue(),
                   invalidFour.GetError() == Ia64RuntimeProcessorCapabilitiesError::InvalidProcessor,
                   !invalidMaximum.HasValue(),
                   invalidMaximum.GetError() == Ia64RuntimeProcessorCapabilitiesError::InvalidProcessor);

    if (!exact.HasValue() || !common.HasValue())
    {
        ASTERIA_EXPECT(runner, "value-copy isolation requires successful capability lookups", false);
        return;
    }
    PalVirtualMemoryCapabilities copiedExact = exact.GetValue();
    copiedExact.Translation.PhysicalAddressBits = 32U;
    Ia64CommonRuntimeProcessorCapabilities copiedCommon = common.GetValue();
    copiedCommon.PhysicalAddressBits = 32U;
    const auto exactAgain = profile.GetProcessorCapabilities(CpuId{0U});
    const auto commonAgain = profile.GetCommonCapabilities();
    ASTERIA_EXPECT(runner, "returned values provide no mutable alias into the published profile",
                   copiedExact.Translation.PhysicalAddressBits == 32U, copiedCommon.PhysicalAddressBits == 32U,
                   exactAgain.HasValue(), exactAgain.GetValue().Translation.PhysicalAddressBits == 50U,
                   commonAgain.HasValue(), commonAgain.GetValue().PhysicalAddressBits == 50U);
}

void TestTwoProcessorSparseReduction(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    capabilities[1U] = MakeCapabilities(Page4KiB | Page64KiB | Page1MiB, Page4KiB | Page64KiB | Page1MiB | Page4GiB,
                                        52U, 60U, 24U, 0xD1U, true, 1U);
    capabilities[3U] = MakeCapabilities(Page4KiB | Page64KiB | Page256KiB, Page4KiB | Page64KiB | Page256KiB | Page4GiB,
                                        48U, 57U, 21U, 0x71U, false, 3U);

    Ia64RuntimeProcessorCapabilities profile;
    const auto error = profile.Initialize(0x0AU, capabilities);
    const auto common = profile.GetCommonCapabilities();
    const Ia64CommonRuntimeProcessorCapabilities expected{
        .InsertablePageSizeMask = Page4KiB | Page64KiB,
        .PurgePageSizeMask = Page4KiB | Page64KiB | Page4GiB,
        .PhysicalAddressBits = 48U,
        .ImplementedVirtualAddressMostSignificantBit = 57U,
        .RegionIdentifierBits = 21U,
        .MemoryAttributeMask = 0x51U,
        .HardwareWalkerImplemented = false,
        .Reserved = {},
    };
    const auto first = profile.GetProcessorCapabilities(CpuId{1U});
    const auto second = profile.GetProcessorCapabilities(CpuId{3U});
    ASTERIA_EXPECT(runner, "two sparse CPUs reduce widths, masks, and walker availability exactly",
                   error == Ia64RuntimeProcessorCapabilitiesError::None, common.HasValue(),
                   Equal(common.GetValue(), expected));
    ASTERIA_EXPECT(runner, "sparse admitted slots retain their complete distinct records", first.HasValue(),
                   Equal(first.GetValue(), capabilities[1U]), second.HasValue(),
                   Equal(second.GetValue(), capabilities[3U]));
}

void TestThreeProcessorReduction(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    capabilities[0U] = MakeCapabilities(Page4KiB | Page64KiB | Page1MiB, Page4KiB | Page64KiB | Page1MiB | Page256MiB,
                                        55U, 60U, 19U, 0xF1U, true);
    capabilities[2U] = MakeCapabilities(Page4KiB | Page256KiB | Page1MiB, Page4KiB | Page256KiB | Page1MiB | Page256MiB,
                                        45U, 58U, 23U, 0x71U, true);
    capabilities[3U] = MakeCapabilities(Page4KiB | Page16KiB | Page1MiB, Page4KiB | Page16KiB | Page1MiB | Page256MiB,
                                        50U, 54U, 22U, 0x91U, true);

    Ia64RuntimeProcessorCapabilities profile;
    const auto error = profile.Initialize(0x0DU, capabilities);
    const auto common = profile.GetCommonCapabilities();
    const Ia64CommonRuntimeProcessorCapabilities expected{
        .InsertablePageSizeMask = Page4KiB | Page1MiB,
        .PurgePageSizeMask = Page4KiB | Page1MiB | Page256MiB,
        .PhysicalAddressBits = 45U,
        .ImplementedVirtualAddressMostSignificantBit = 54U,
        .RegionIdentifierBits = 19U,
        .MemoryAttributeMask = RequiredMemoryAttributes,
        .HardwareWalkerImplemented = true,
        .Reserved = {},
    };
    ASTERIA_EXPECT(runner, "three CPUs contribute independent minima and mask intersections",
                   error == Ia64RuntimeProcessorCapabilitiesError::None, common.HasValue(),
                   Equal(common.GetValue(), expected));
}

void TestFourProcessorReduction(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        capabilities[processor] = MakeCapabilities(
            Page4KiB | (Foundation::UInt64{1U} << (14U + processor * 2U)),
            Page4KiB | Page4GiB | (Foundation::UInt64{1U} << (14U + processor * 2U)),
            static_cast<Foundation::UInt8>(53U - processor), static_cast<Foundation::UInt8>(60U - processor),
            static_cast<Foundation::UInt8>(24U - processor),
            static_cast<Foundation::UInt8>(0x11U | (0x20U << processor)), processor != 2U, processor);
    }

    Ia64RuntimeProcessorCapabilities profile;
    const auto error = profile.Initialize(0x0FU, capabilities);
    const auto common = profile.GetCommonCapabilities();
    const Ia64CommonRuntimeProcessorCapabilities expected{
        .InsertablePageSizeMask = Page4KiB,
        .PurgePageSizeMask = Page4KiB | Page4GiB,
        .PhysicalAddressBits = 50U,
        .ImplementedVirtualAddressMostSignificantBit = 57U,
        .RegionIdentifierBits = 21U,
        .MemoryAttributeMask = RequiredMemoryAttributes,
        .HardwareWalkerImplemented = false,
        .Reserved = {},
    };
    bool everyExactRecordRetained{true};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const auto exact = profile.GetProcessorCapabilities(CpuId{processor});
        everyExactRecordRetained =
            everyExactRecordRetained && exact.HasValue() && Equal(exact.GetValue(), capabilities[processor]);
    }
    ASTERIA_EXPECT(runner, "all four CPUs reduce to only the universally safe runtime subset",
                   error == Ia64RuntimeProcessorCapabilitiesError::None, common.HasValue(),
                   Equal(common.GetValue(), expected));
    ASTERIA_EXPECT(runner, "the four-entry profile retains every target-local PAL record exactly",
                   everyExactRecordRetained);
}

void TestMasksAndOneShotBehavior(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    capabilities[0U] = MakeCapabilities();
    ASTERIA_EXPECT(runner, "a zero admitted mask is rejected atomically and can be retried",
                   FailureIsAtomic(0U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidProcessorMask));
    ASTERIA_EXPECT(runner, "a mask bit beyond CPU3 is rejected atomically and can be retried",
                   FailureIsAtomic(0x10U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidProcessorMask));
    ASTERIA_EXPECT(runner, "a mask with several out-of-capacity bits is rejected atomically",
                   FailureIsAtomic(0xFFU, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidProcessorMask));

    Ia64RuntimeProcessorCapabilities profile;
    const auto firstError = profile.Initialize(0x01U, capabilities);
    const auto before = profile.GetProcessorCapabilities(CpuId{0U});
    const auto beforeCommon = profile.GetCommonCapabilities();
    PalVirtualMemoryCapabilities replacement[Kernel::Scheduler::MaximumProcessorCount]{};
    replacement[2U] =
        MakeCapabilities(Page4KiB | Page1MiB, Page4KiB | Page1MiB, 32U, 53U, 18U, RequiredMemoryAttributes, false, 12U);
    const auto repeatedError = profile.Initialize(0x04U, replacement);
    const auto afterMask = profile.GetAdmittedProcessorMask();
    const auto after = profile.GetProcessorCapabilities(CpuId{0U});
    const auto afterCommon = profile.GetCommonCapabilities();
    ASTERIA_EXPECT(runner, "repeated initialization cannot replace any published profile state",
                   firstError == Ia64RuntimeProcessorCapabilitiesError::None,
                   repeatedError == Ia64RuntimeProcessorCapabilitiesError::AlreadyInitialized, afterMask.HasValue(),
                   afterMask.GetValue() == 0x01U, before.HasValue(), after.HasValue(),
                   Equal(before.GetValue(), after.GetValue()), beforeCommon.HasValue(), afterCommon.HasValue(),
                   Equal(beforeCommon.GetValue(), afterCommon.GetValue()));
}

void TestMalformedConsumedFields(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};

    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.PhysicalAddressBits = 31U;
    ASTERIA_EXPECT(
        runner, "a physical-address width below the architected range is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidAddressCapabilities));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.PhysicalAddressBits = 64U;
    ASTERIA_EXPECT(
        runner, "a physical-address width above the architected range is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidAddressCapabilities));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.ImplementedVirtualAddressMostSignificantBit = 49U;
    ASTERIA_EXPECT(
        runner, "an implemented virtual-address MSB below its range is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidAddressCapabilities));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.ImplementedVirtualAddressMostSignificantBit = 61U;
    ASTERIA_EXPECT(
        runner, "an implemented virtual-address MSB above its range is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidAddressCapabilities));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.RegionIdentifierBits = 17U;
    ASTERIA_EXPECT(
        runner, "an RID width below the architected range is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidAddressCapabilities));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.RegionIdentifierBits = 25U;
    ASTERIA_EXPECT(
        runner, "an RID width above the architected range is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidAddressCapabilities));

    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.InsertablePageSizeMask |= 1U << 11U;
    ASTERIA_EXPECT(
        runner, "an insertable page size below 4 KiB is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidPageSizeCapabilities));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].PurgePageSizeMask |= 1U;
    ASTERIA_EXPECT(
        runner, "a purge page size below 4 KiB is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidPageSizeCapabilities));
    capabilities[0U] = MakeCapabilities(Page4KiB | Page64KiB, Page4KiB);
    ASTERIA_EXPECT(
        runner, "an insertable page size that cannot be purged is rejected",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidPageSizeCapabilities));

    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.MemoryAttributeMask |= 1U << 1U;
    ASTERIA_EXPECT(runner, "a reserved memory-attribute encoding is rejected",
                   FailureIsAtomic(0x01U, capabilities,
                                   Ia64RuntimeProcessorCapabilitiesError::InvalidMemoryAttributeCapabilities));

    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.Reserved16 = 1U;
    ASTERIA_EXPECT(runner, "a retained translation Reserved16 field is rejected",
                   FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidReservedFields));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.Reserved64 = 1U;
    ASTERIA_EXPECT(runner, "a retained translation Reserved64 field is rejected",
                   FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidReservedFields));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Reserved8 = 1U;
    ASTERIA_EXPECT(runner, "a retained outer Reserved8 field is rejected",
                   FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidReservedFields));
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Reserved32 = 1U;
    ASTERIA_EXPECT(runner, "a retained outer Reserved32 field is rejected",
                   FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::InvalidReservedFields));
}

void TestIncompatibleCommonRequirements(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    capabilities[0U] = MakeCapabilities(Page64KiB, Page64KiB);
    ASTERIA_EXPECT(
        runner, "the common profile must retain 4 KiB insertion and purge",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::IncompatibleCommonCapabilities));

    capabilities[0U] = MakeCapabilities(Page4KiB, Page4KiB, 48U, 57U, 20U, 0x10U);
    ASTERIA_EXPECT(
        runner, "the common profile must retain write-back memory",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::IncompatibleCommonCapabilities));
    capabilities[0U] = MakeCapabilities(Page4KiB, Page4KiB, 48U, 57U, 20U, 0x01U);
    ASTERIA_EXPECT(
        runner, "the common profile must retain uncached memory",
        FailureIsAtomic(0x01U, capabilities, Ia64RuntimeProcessorCapabilitiesError::IncompatibleCommonCapabilities));
}

void TestUnrelatedPalFieldsAreNotRevalidated(TestContext& runner) noexcept
{
    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    capabilities[0U] = MakeCapabilities();
    capabilities[0U].Translation.ProtectionKeyBits = 0U;
    capabilities[0U].InstructionTranslationRegisterCount = 0U;
    capabilities[0U].DataTranslationRegisterCount = 0xFFFFU;
    capabilities[0U].ProtectionKeyRegisterCount = 1U;
    capabilities[0U].MaximumConcurrentPurgesEncoding = 0xFFFFU;
    capabilities[0U].UniqueTranslationCacheCount = 0U;
    capabilities[0U].TranslationCacheLevelCount = 0U;

    Ia64RuntimeProcessorCapabilities profile;
    const auto error = profile.Initialize(0x01U, capabilities);
    const auto retained = profile.GetProcessorCapabilities(CpuId{0U});
    ASTERIA_EXPECT(runner, "unconsumed PAL facts are retained exactly without redundant validation",
                   error == Ia64RuntimeProcessorCapabilitiesError::None, retained.HasValue(),
                   Equal(retained.GetValue(), capabilities[0U]));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeProcessorCapabilitiesTests"};
    Asteria::Test::TestCpu0ProfileAndOfflineInputs(runner);
    Asteria::Test::TestTwoProcessorSparseReduction(runner);
    Asteria::Test::TestThreeProcessorReduction(runner);
    Asteria::Test::TestFourProcessorReduction(runner);
    Asteria::Test::TestMasksAndOneShotBehavior(runner);
    Asteria::Test::TestMalformedConsumedFields(runner);
    Asteria::Test::TestIncompatibleCommonRequirements(runner);
    Asteria::Test::TestUnrelatedPalFieldsAreNotRevalidated(runner);
    return runner.Finish();
}
