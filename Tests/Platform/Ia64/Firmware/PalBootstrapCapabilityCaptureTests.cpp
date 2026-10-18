// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Platform::Ia64::Firmware::PalBootstrapCapabilities;
using Platform::Ia64::Firmware::PalBootstrapCapabilityCapture;
using Platform::Ia64::Firmware::PalBootstrapCapabilityCaptureError;
using Platform::Ia64::Firmware::PalBootstrapCapabilityCapturePhase;
using Platform::Ia64::Firmware::PalStaticCallAdapterError;
using Platform::Ia64::Firmware::SalEntrypointBinding;
using Platform::Ia64::Memory::PalStaticCallResult;
using Platform::Ia64::Memory::PalTranslationCachePurgeDecodeError;
using Platform::Ia64::Memory::PalVirtualMemoryDecodeError;

struct InvocationRecord
{
    Foundation::UInt64 Entry;
    Foundation::UInt64 Procedure;
    Foundation::UInt64 Argument1;
    Foundation::UInt64 Argument2;
    Foundation::UInt64 Argument3;
};

constexpr Foundation::Size MaximumInvocations{5U};
constinit InvocationRecord g_invocations[MaximumInvocations]{};
constinit Foundation::Size g_invocationCount;
constinit Foundation::Size g_unsupportedInvocation;
constinit PalStaticCallResult g_summaryResult{};
constinit PalStaticCallResult g_pageSizeResult{};
constinit PalStaticCallResult g_memoryAttributeResult{};
constinit PalStaticCallResult g_purgeResult{};
constinit PalStaticCallResult g_rseResult{};
Foundation::UInt8 g_bindingSnapshot[sizeof(SalEntrypointBinding)];

[[nodiscard]] constexpr PhysicalMemoryRegion Region(const Foundation::UInt64 physicalStart,
                                                    const Foundation::UInt64 pageCount,
                                                    const Foundation::UInt32 firmwareType,
                                                    const PhysicalMemoryKind kind) noexcept
{
    return PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = 0U,
        .PageCount = pageCount,
        .Attributes = Foundation::UInt64{1U} << 63U,
        .FirmwareType = firmwareType,
        .Kind = kind,
    };
}

[[nodiscard]] constexpr SalEntrypointBinding ValidBinding() noexcept
{
    return SalEntrypointBinding{
        .PalProcedurePhysicalAddress = 0x2000U,
        .SalProcedurePhysicalAddress = 0x4000U,
        .SalGlobalDataPhysicalAddress = 0x6000U,
        .PalCodeRegion = Region(0x1000U, 2U, 13U, PhysicalMemoryKind::PalCode),
        .SalCodeRegion = Region(0x4000U, 2U, 5U, PhysicalMemoryKind::RuntimeServices),
        .SalGlobalDataRegion = Region(0x6000U, 2U, 6U, PhysicalMemoryKind::RuntimeServices),
    };
}

[[nodiscard]] constexpr Foundation::UInt64 MakeSummary1() noexcept
{
    return Foundation::UInt64{1U} | (Foundation::UInt64{50U} << 1U) | (Foundation::UInt64{24U} << 8U) |
           (Foundation::UInt64{15U} << 16U) | (Foundation::UInt64{8U} << 24U) | (Foundation::UInt64{31U} << 32U) |
           (Foundation::UInt64{31U} << 40U) | (Foundation::UInt64{4U} << 48U) | (Foundation::UInt64{2U} << 56U);
}

void ResetStub() noexcept
{
    for (Foundation::Size index{0U}; index < MaximumInvocations; ++index)
    {
        g_invocations[index] = InvocationRecord{};
    }
    g_invocationCount = 0U;
    g_unsupportedInvocation = 0U;
    g_summaryResult = PalStaticCallResult{
        .Status = 0,
        .Result1 = MakeSummary1(),
        .Result2 = Foundation::UInt64{60U} | (Foundation::UInt64{24U} << 8U),
        .Result3 = 0U,
    };
    g_pageSizeResult = PalStaticCallResult{
        .Status = 0,
        .Result1 = 0x0000'0001'5555'7000U,
        .Result2 = 0x0000'0001'5555'7000U,
        .Result3 = 0U,
    };
    g_memoryAttributeResult = PalStaticCallResult{
        .Status = 0,
        .Result1 = 0x71U,
        .Result2 = 0U,
        .Result3 = 0U,
    };
    g_purgeResult = PalStaticCallResult{
        .Status = 0,
        .Result1 = 0U,
        .Result2 = (Foundation::UInt64{1U} << 32U) | 1U,
        .Result3 = 0U,
    };
    g_rseResult = PalStaticCallResult{
        .Status = 0,
        .Result1 = 96U,
        .Result2 = 0U,
        .Result3 = 0U,
    };
}

void SnapshotBinding(const SalEntrypointBinding& binding) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&binding);
    for (Foundation::Size index{0U}; index < sizeof(binding); ++index)
    {
        g_bindingSnapshot[index] = bytes[index];
    }
}

[[nodiscard]] bool BindingMatchesSnapshot(const SalEntrypointBinding& binding) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&binding);
    for (Foundation::Size index{0U}; index < sizeof(binding); ++index)
    {
        if (bytes[index] != g_bindingSnapshot[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool AreCallResultsEqual(const PalStaticCallResult& first,
                                                 const PalStaticCallResult& second) noexcept
{
    return (first.Status == second.Status) && (first.Result1 == second.Result1) && (first.Result2 == second.Result2) &&
           (first.Result3 == second.Result3);
}

[[nodiscard]] bool RawResultsMatchSnapshots(const PalStaticCallResult& summary, const PalStaticCallResult& pageSizes,
                                            const PalStaticCallResult& memoryAttributes,
                                            const PalStaticCallResult& purgeInformation) noexcept
{
    return AreCallResultsEqual(g_summaryResult, summary) && AreCallResultsEqual(g_pageSizeResult, pageSizes) &&
           AreCallResultsEqual(g_memoryAttributeResult, memoryAttributes) &&
           AreCallResultsEqual(g_purgeResult, purgeInformation);
}

[[nodiscard]] bool AreRecordsEqual(const PalBootstrapCapabilities& first,
                                   const PalBootstrapCapabilities& second) noexcept
{
    const auto* const firstBytes = reinterpret_cast<const Foundation::UInt8*>(&first);
    const auto* const secondBytes = reinterpret_cast<const Foundation::UInt8*>(&second);
    for (Foundation::Size index{0U}; index < sizeof(first); ++index)
    {
        if (firstBytes[index] != secondBytes[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool
HasExactError(const Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>& result,
              const PalBootstrapCapabilityCapturePhase phase, const PalStaticCallAdapterError adapterError,
              const PalVirtualMemoryDecodeError virtualMemoryError,
              const PalTranslationCachePurgeDecodeError purgeError) noexcept
{
    if (result.HasValue())
    {
        return false;
    }
    const PalBootstrapCapabilityCaptureError error = result.GetError();
    return (error.Phase == phase) && (error.AdapterError == adapterError) &&
           (error.VirtualMemoryError == virtualMemoryError) && (error.TranslationCachePurgeError == purgeError);
}

[[nodiscard]] bool HasExactInvocationPrefix(const Foundation::Size count) noexcept
{
    constexpr Foundation::UInt64 ExpectedProcedures[MaximumInvocations]{
        8U, 34U, 5U, 6U, 19U,
    };
    if (g_invocationCount != count)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        const InvocationRecord& invocation = g_invocations[index];
        if ((invocation.Entry != 0x2000U) || (invocation.Procedure != ExpectedProcedures[index]) ||
            (invocation.Argument1 != 0U) || (invocation.Argument2 != 0U) || (invocation.Argument3 != 0U))
        {
            return false;
        }
    }
    return true;
}

void TestSuccessfulCapture(TestContext& runner) noexcept
{
    ResetStub();
    const SalEntrypointBinding binding = ValidBinding();
    SnapshotBinding(binding);
    const auto result = PalBootstrapCapabilityCapture::Capture(binding);
    bool exactRecord{false};
    if (result.HasValue())
    {
        const PalBootstrapCapabilities& record = result.GetValue();
        exactRecord =
            (record.VirtualMemory.Translation.PhysicalAddressBits == 50U) &&
            (record.VirtualMemory.Translation.ImplementedVirtualAddressMostSignificantBit == 60U) &&
            (record.VirtualMemory.Translation.RegionIdentifierBits == 24U) &&
            (record.VirtualMemory.Translation.ProtectionKeyBits == 24U) &&
            (record.VirtualMemory.Translation.MemoryAttributeMask == 0x71U) &&
            record.VirtualMemory.Translation.HardwareWalkerImplemented &&
            (record.VirtualMemory.Translation.InsertablePageSizeMask == 0x0000'0001'5555'7000U) &&
            (record.VirtualMemory.PurgePageSizeMask == 0x0000'0001'5555'7000U) &&
            (record.VirtualMemory.InstructionTranslationRegisterCount == 32U) &&
            (record.VirtualMemory.DataTranslationRegisterCount == 32U) &&
            (record.VirtualMemory.ProtectionKeyRegisterCount == 16U) &&
            (record.VirtualMemory.MaximumConcurrentPurgesEncoding == 0U) &&
            (record.VirtualMemory.HashTagIdentifier == 8U) &&
            (record.VirtualMemory.UniqueTranslationCacheCount == 4U) &&
            (record.VirtualMemory.TranslationCacheLevelCount == 2U) &&
            (record.TranslationCachePurge.BaseAddress == 0U) && (record.TranslationCachePurge.OperationCount == 1U) &&
            (record.TranslationCachePurge.LastPurgeAddress == 0U) && (record.TranslationCachePurge.OuterCount == 1U) &&
            (record.TranslationCachePurge.InnerCount == 1U) && (record.TranslationCachePurge.OuterStride == 0U) &&
            (record.TranslationCachePurge.InnerStride == 0U);
    }
    ASTERIA_EXPECT(runner, "the exact QEMU Montecito PAL results publish one decoded record", exactRecord);
    ASTERIA_EXPECT(runner, "PAL procedures execute once in fixed fail-fast order with zero arguments",
                   HasExactInvocationPrefix(5U));
    ASTERIA_EXPECT(runner, "successful capability capture leaves the complete binding unchanged",
                   BindingMatchesSnapshot(binding));

    PalBootstrapCapabilities retained{};
    if (result.HasValue())
    {
        retained = result.GetValue();
    }
    g_summaryResult = PalStaticCallResult{};
    g_pageSizeResult = PalStaticCallResult{};
    g_memoryAttributeResult = PalStaticCallResult{};
    g_purgeResult = PalStaticCallResult{};
    g_rseResult = PalStaticCallResult{};
    ASTERIA_EXPECT(runner, "published capabilities retain no reference to raw PAL result storage", result.HasValue(),
                   AreRecordsEqual(retained, result.GetValue()));

    ResetStub();
    const auto repeated = PalBootstrapCapabilityCapture::Capture(binding);
    ASTERIA_EXPECT(runner, "equal PAL results produce byte-identical capability records", result.HasValue(),
                   repeated.HasValue(), AreRecordsEqual(result.GetValue(), repeated.GetValue()));

    ResetStub();
    g_summaryResult.Result2 |= Foundation::UInt64{0x1234U} << 16U;
    g_purgeResult = PalStaticCallResult{
        .Status = 0,
        .Result1 = 0x1000U,
        .Result2 = (Foundation::UInt64{2U} << 32U) | 3U,
        .Result3 = (Foundation::UInt64{0x2000U} << 32U) | 0x1000U,
    };
    const auto generic = PalBootstrapCapabilityCapture::Capture(binding);
    ASTERIA_EXPECT(runner, "a non-QEMU valid PAL profile retains exact generic purge operands", generic.HasValue(),
                   generic.GetValue().VirtualMemory.MaximumConcurrentPurgesEncoding == 0x1234U,
                   generic.GetValue().TranslationCachePurge.BaseAddress == 0x1000U,
                   generic.GetValue().TranslationCachePurge.OperationCount == 6U,
                   generic.GetValue().TranslationCachePurge.LastPurgeAddress == 0x8000U,
                   generic.GetValue().TranslationCachePurge.OuterCount == 2U,
                   generic.GetValue().TranslationCachePurge.InnerCount == 3U,
                   generic.GetValue().TranslationCachePurge.OuterStride == 0x2000U,
                   generic.GetValue().TranslationCachePurge.InnerStride == 0x1000U);
}

void TestAdapterFailures(TestContext& runner) noexcept
{
    SalEntrypointBinding binding = ValidBinding();
    binding.PalProcedurePhysicalAddress = 0U;
    ResetStub();
    const auto invalidBinding = PalBootstrapCapabilityCapture::Capture(binding);
    ASTERIA_EXPECT(runner, "an invalid binding fails at the first PAL phase without invocation",
                   HasExactError(invalidBinding, PalBootstrapCapabilityCapturePhase::VirtualMemorySummaryInvocation,
                                 PalStaticCallAdapterError::InvalidBinding, PalVirtualMemoryDecodeError::None,
                                 PalTranslationCachePurgeDecodeError::None),
                   g_invocationCount == 0U);

    constexpr PalBootstrapCapabilityCapturePhase ExpectedPhases[]{
        PalBootstrapCapabilityCapturePhase::VirtualMemorySummaryInvocation,
        PalBootstrapCapabilityCapturePhase::VirtualMemoryPageSizeInvocation,
        PalBootstrapCapabilityCapturePhase::MemoryAttributeInvocation,
        PalBootstrapCapabilityCapturePhase::TranslationCachePurgeInvocation,
        PalBootstrapCapabilityCapturePhase::RseInformationInvocation,
    };
    bool allAdapterFailures{true};
    binding = ValidBinding();
    for (Foundation::Size ordinal{1U}; ordinal <= MaximumInvocations; ++ordinal)
    {
        ResetStub();
        g_unsupportedInvocation = ordinal;
        const auto result = PalBootstrapCapabilityCapture::Capture(binding);
        allAdapterFailures =
            allAdapterFailures &&
            HasExactError(result, ExpectedPhases[ordinal - 1U], PalStaticCallAdapterError::UnsupportedProcessorState,
                          PalVirtualMemoryDecodeError::None, PalTranslationCachePurgeDecodeError::None) &&
            HasExactInvocationPrefix(ordinal);
    }
    ASTERIA_EXPECT(runner, "every adapter rejection retains its exact phase and stops later calls", allAdapterFailures);
}

void TestVirtualMemoryDecodeFailures(TestContext& runner) noexcept
{
    const SalEntrypointBinding binding = ValidBinding();
    constexpr PalVirtualMemoryDecodeError ExpectedErrors[]{
        PalVirtualMemoryDecodeError::SummaryCallFailed,
        PalVirtualMemoryDecodeError::PageSizeCallFailed,
        PalVirtualMemoryDecodeError::MemoryAttributeCallFailed,
        PalVirtualMemoryDecodeError::SummaryReservedResult,
        PalVirtualMemoryDecodeError::PageSizeReservedResult,
        PalVirtualMemoryDecodeError::MemoryAttributeReservedResult,
        PalVirtualMemoryDecodeError::InvalidAddressCapabilities,
        PalVirtualMemoryDecodeError::InvalidProtectionCapabilities,
        PalVirtualMemoryDecodeError::InvalidTranslationRegisterCapabilities,
        PalVirtualMemoryDecodeError::InvalidTranslationCacheTopology,
        PalVirtualMemoryDecodeError::MissingMandatoryInsertablePageSize,
        PalVirtualMemoryDecodeError::MissingMandatoryPurgePageSize,
        PalVirtualMemoryDecodeError::PageSizeBelowArchitectedMinimum,
        PalVirtualMemoryDecodeError::InsertablePageSizeNotPurgeable,
        PalVirtualMemoryDecodeError::InvalidMemoryAttributeCapabilities,
        PalVirtualMemoryDecodeError::MissingRequiredMemoryAttribute,
    };
    bool allFailures{true};
    for (const PalVirtualMemoryDecodeError expected : ExpectedErrors)
    {
        ResetStub();
        Foundation::Size expectedCallCount{3U};
        switch (expected)
        {
        case PalVirtualMemoryDecodeError::None:
            break;
        case PalVirtualMemoryDecodeError::SummaryCallFailed:
            g_summaryResult.Status = -1;
            expectedCallCount = 1U;
            break;
        case PalVirtualMemoryDecodeError::PageSizeCallFailed:
            g_pageSizeResult.Status = -1;
            expectedCallCount = 2U;
            break;
        case PalVirtualMemoryDecodeError::MemoryAttributeCallFailed:
            g_memoryAttributeResult.Status = -1;
            break;
        case PalVirtualMemoryDecodeError::SummaryReservedResult:
            g_summaryResult.Result3 = 1U;
            break;
        case PalVirtualMemoryDecodeError::PageSizeReservedResult:
            g_pageSizeResult.Result3 = 1U;
            break;
        case PalVirtualMemoryDecodeError::MemoryAttributeReservedResult:
            g_memoryAttributeResult.Result2 = 1U;
            break;
        case PalVirtualMemoryDecodeError::InvalidAddressCapabilities:
            g_summaryResult.Result1 =
                (g_summaryResult.Result1 & ~(Foundation::UInt64{0x7FU} << 1U)) | (Foundation::UInt64{31U} << 1U);
            break;
        case PalVirtualMemoryDecodeError::InvalidProtectionCapabilities:
            g_summaryResult.Result1 =
                (g_summaryResult.Result1 & ~(Foundation::UInt64{0xFFU} << 8U)) | (Foundation::UInt64{23U} << 8U);
            break;
        case PalVirtualMemoryDecodeError::InvalidTranslationRegisterCapabilities:
            g_summaryResult.Result1 =
                (g_summaryResult.Result1 & ~(Foundation::UInt64{0xFFU} << 32U)) | (Foundation::UInt64{6U} << 32U);
            break;
        case PalVirtualMemoryDecodeError::InvalidTranslationCacheTopology:
            g_summaryResult.Result1 &= ~(Foundation::UInt64{0xFFU} << 48U);
            break;
        case PalVirtualMemoryDecodeError::MissingMandatoryInsertablePageSize:
            g_pageSizeResult.Result1 &= ~(Foundation::UInt64{1U} << 12U);
            break;
        case PalVirtualMemoryDecodeError::MissingMandatoryPurgePageSize:
            g_pageSizeResult.Result2 &= ~(Foundation::UInt64{1U} << 32U);
            break;
        case PalVirtualMemoryDecodeError::PageSizeBelowArchitectedMinimum:
            g_pageSizeResult.Result1 |= Foundation::UInt64{1U};
            g_pageSizeResult.Result2 |= Foundation::UInt64{1U};
            break;
        case PalVirtualMemoryDecodeError::InsertablePageSizeNotPurgeable:
            g_pageSizeResult.Result1 |= Foundation::UInt64{1U} << 15U;
            g_pageSizeResult.Result2 &= ~(Foundation::UInt64{1U} << 15U);
            break;
        case PalVirtualMemoryDecodeError::InvalidMemoryAttributeCapabilities:
            g_memoryAttributeResult.Result1 |= Foundation::UInt64{1U} << 1U;
            break;
        case PalVirtualMemoryDecodeError::MissingRequiredMemoryAttribute:
            g_memoryAttributeResult.Result1 &= ~Foundation::UInt64{1U};
            break;
        }

        SnapshotBinding(binding);
        const PalStaticCallResult summarySnapshot{g_summaryResult};
        const PalStaticCallResult pageSizeSnapshot{g_pageSizeResult};
        const PalStaticCallResult memoryAttributeSnapshot{g_memoryAttributeResult};
        const PalStaticCallResult purgeSnapshot{g_purgeResult};
        const auto result = PalBootstrapCapabilityCapture::Capture(binding);
        allFailures =
            allFailures &&
            HasExactError(result, PalBootstrapCapabilityCapturePhase::VirtualMemoryDecoding,
                          PalStaticCallAdapterError::None, expected, PalTranslationCachePurgeDecodeError::None) &&
            HasExactInvocationPrefix(expectedCallCount) && BindingMatchesSnapshot(binding) &&
            RawResultsMatchSnapshots(summarySnapshot, pageSizeSnapshot, memoryAttributeSnapshot, purgeSnapshot);
    }
    ASTERIA_EXPECT(runner, "all virtual-memory errors are forwarded exactly without input mutation", allFailures);

    ResetStub();
    SnapshotBinding(binding);
    g_summaryResult.Status = -1;
    g_unsupportedInvocation = 2U;
    const auto priority = PalBootstrapCapabilityCapture::Capture(binding);
    ASTERIA_EXPECT(runner, "an earlier PAL status has priority and failure leaves binding unchanged",
                   HasExactError(priority, PalBootstrapCapabilityCapturePhase::VirtualMemoryDecoding,
                                 PalStaticCallAdapterError::None, PalVirtualMemoryDecodeError::SummaryCallFailed,
                                 PalTranslationCachePurgeDecodeError::None),
                   HasExactInvocationPrefix(1U), BindingMatchesSnapshot(binding));
}

void TestPurgeDecodeFailures(TestContext& runner) noexcept
{
    const SalEntrypointBinding binding = ValidBinding();
    constexpr PalTranslationCachePurgeDecodeError ExpectedErrors[]{
        PalTranslationCachePurgeDecodeError::CallFailed,
        PalTranslationCachePurgeDecodeError::EmptyPurgeLoop,
        PalTranslationCachePurgeDecodeError::OperationLimitExceeded,
        PalTranslationCachePurgeDecodeError::AddressComputationOverflow,
    };
    bool allFailures{true};
    for (const PalTranslationCachePurgeDecodeError expected : ExpectedErrors)
    {
        ResetStub();
        switch (expected)
        {
        case PalTranslationCachePurgeDecodeError::None:
            break;
        case PalTranslationCachePurgeDecodeError::CallFailed:
            g_purgeResult.Status = -1;
            break;
        case PalTranslationCachePurgeDecodeError::EmptyPurgeLoop:
            g_purgeResult.Result2 = 1U;
            break;
        case PalTranslationCachePurgeDecodeError::OperationLimitExceeded:
            g_purgeResult.Result2 = (Foundation::UInt64{65'537U} << 32U) | 1U;
            break;
        case PalTranslationCachePurgeDecodeError::AddressComputationOverflow:
            g_purgeResult.Result1 = ~Foundation::UInt64{0U} - 8U;
            g_purgeResult.Result2 = (Foundation::UInt64{2U} << 32U) | 2U;
            g_purgeResult.Result3 = (Foundation::UInt64{3U} << 32U) | 2U;
            break;
        }

        SnapshotBinding(binding);
        const PalStaticCallResult summarySnapshot{g_summaryResult};
        const PalStaticCallResult pageSizeSnapshot{g_pageSizeResult};
        const PalStaticCallResult memoryAttributeSnapshot{g_memoryAttributeResult};
        const PalStaticCallResult purgeSnapshot{g_purgeResult};
        const auto result = PalBootstrapCapabilityCapture::Capture(binding);
        allFailures =
            allFailures &&
            HasExactError(result, PalBootstrapCapabilityCapturePhase::TranslationCachePurgeDecoding,
                          PalStaticCallAdapterError::None, PalVirtualMemoryDecodeError::None, expected) &&
            HasExactInvocationPrefix(4U) && BindingMatchesSnapshot(binding) &&
            RawResultsMatchSnapshots(summarySnapshot, pageSizeSnapshot, memoryAttributeSnapshot, purgeSnapshot);
    }
    ASTERIA_EXPECT(runner, "all purge errors are forwarded exactly without input mutation", allFailures);
}
/// PAL_RSE_INFO must report exactly the 96 physical stacked registers the PL3
/// entry scrubs cover, with no reserved bit; each other answer stops the
/// capture after all five calls with only the phase set.
void TestRseInformation(TestContext& runner) noexcept
{
    const SalEntrypointBinding binding = ValidBinding();
    ResetStub();
    g_rseResult.Result2 = 0x3U;
    ASTERIA_EXPECT(runner, "both PAL_RSE_INFO eager hints are accepted",
                   PalBootstrapCapabilityCapture::Capture(binding).HasValue(), HasExactInvocationPrefix(5U));

    constexpr PalStaticCallResult Rejected[]{
        {.Status = -1, .Result1 = 96U, .Result2 = 0U, .Result3 = 0U},
        {.Status = 0, .Result1 = 95U, .Result2 = 0U, .Result3 = 0U},
        {.Status = 0, .Result1 = 97U, .Result2 = 0U, .Result3 = 0U},
        {.Status = 0, .Result1 = 192U, .Result2 = 0U, .Result3 = 0U},
        {.Status = 0, .Result1 = 96U, .Result2 = 0x4U, .Result3 = 0U},
        {.Status = 0, .Result1 = 96U, .Result2 = 0U, .Result3 = 1U},
    };
    bool allRejected{true};
    for (const PalStaticCallResult& rejected : Rejected)
    {
        ResetStub();
        g_rseResult = rejected;
        SnapshotBinding(binding);
        const auto result = PalBootstrapCapabilityCapture::Capture(binding);
        allRejected = allRejected &&
                      HasExactError(result, PalBootstrapCapabilityCapturePhase::RseInformationDecoding,
                                    PalStaticCallAdapterError::None, PalVirtualMemoryDecodeError::None,
                                    PalTranslationCachePurgeDecodeError::None) &&
                      HasExactInvocationPrefix(5U) && BindingMatchesSnapshot(binding);
    }
    ASTERIA_EXPECT(runner, "a failed PAL_RSE_INFO, another stacked-register count or a reserved bit stops the capture",
                   allRejected);
}
} // namespace
} // namespace Asteria::Test

extern "C" Asteria::Platform::Ia64::Memory::PalStaticCallResult AsteriaIa64InvokePhysicalPalStatic(
    const Asteria::Foundation::UInt64 entryPhysicalAddress, const Asteria::Foundation::UInt64 procedureIndex,
    const Asteria::Foundation::UInt64 argument1, const Asteria::Foundation::UInt64 argument2,
    const Asteria::Foundation::UInt64 argument3, Asteria::Foundation::UInt64* const invocationState) noexcept
{
    const Asteria::Foundation::Size ordinal = Asteria::Test::g_invocationCount + 1U;
    if (Asteria::Test::g_invocationCount < Asteria::Test::MaximumInvocations)
    {
        Asteria::Test::g_invocations[Asteria::Test::g_invocationCount] = Asteria::Test::InvocationRecord{
            .Entry = entryPhysicalAddress,
            .Procedure = procedureIndex,
            .Argument1 = argument1,
            .Argument2 = argument2,
            .Argument3 = argument3,
        };
    }
    Asteria::Test::g_invocationCount = ordinal;
    *invocationState = (ordinal == Asteria::Test::g_unsupportedInvocation) ? 1U : 0U;
    switch (procedureIndex)
    {
    case 5U:
        return Asteria::Test::g_memoryAttributeResult;
    case 6U:
        return Asteria::Test::g_purgeResult;
    case 8U:
        return Asteria::Test::g_summaryResult;
    case 19U:
        return Asteria::Test::g_rseResult;
    case 34U:
        return Asteria::Test::g_pageSizeResult;
    default:
        return Asteria::Platform::Ia64::Memory::PalStaticCallResult{
            .Status = -2,
            .Result1 = 0U,
            .Result2 = 0U,
            .Result3 = 0U,
        };
    }
}

int main()
{
    TestContext runner{"PalBootstrapCapabilityCaptureTests"};
    Asteria::Test::TestSuccessfulCapture(runner);
    Asteria::Test::TestAdapterFailures(runner);
    Asteria::Test::TestVirtualMemoryDecodeFailures(runner);
    Asteria::Test::TestPurgeDecodeFailures(runner);
    Asteria::Test::TestRseInformation(runner);
    return runner.Finish();
}
