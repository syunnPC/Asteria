// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/PalTranslationCachePurgeDecoder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::MaximumTranslationCachePurgeOperations;
using Platform::Ia64::Memory::PalStaticCallResult;
using Platform::Ia64::Memory::PalTranslationCachePurgeDecodeError;
using Platform::Ia64::Memory::PalTranslationCachePurgeDecoder;
using Platform::Ia64::Memory::PalTranslationCachePurgePlan;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr PalStaticCallResult
MakePurgeInformation(const Foundation::UInt64 baseAddress, const Foundation::UInt32 outerCount,
                     const Foundation::UInt32 innerCount, const Foundation::UInt32 outerStride,
                     const Foundation::UInt32 innerStride, const Foundation::Int64 status = 0) noexcept
{
    return PalStaticCallResult{
        .Status = status,
        .Result1 = baseAddress,
        .Result2 = (static_cast<Foundation::UInt64>(outerCount) << 32U) | static_cast<Foundation::UInt64>(innerCount),
        .Result3 = (static_cast<Foundation::UInt64>(outerStride) << 32U) | static_cast<Foundation::UInt64>(innerStride),
    };
}

[[nodiscard]] bool HasError(const PalStaticCallResult& purgeInformation,
                            const PalTranslationCachePurgeDecodeError expected) noexcept
{
    const auto result = PalTranslationCachePurgeDecoder::Decode(purgeInformation);
    return !result.HasValue() && (result.GetError() == expected);
}

[[nodiscard]] bool IsExactPlan(const PalTranslationCachePurgePlan& plan, const Foundation::UInt64 baseAddress,
                               const Foundation::UInt64 operationCount, const Foundation::UInt64 lastPurgeAddress,
                               const Foundation::UInt32 outerCount, const Foundation::UInt32 innerCount,
                               const Foundation::UInt32 outerStride, const Foundation::UInt32 innerStride) noexcept
{
    return (plan.BaseAddress == baseAddress) && (plan.OperationCount == operationCount) &&
           (plan.LastPurgeAddress == lastPurgeAddress) && (plan.OuterCount == outerCount) &&
           (plan.InnerCount == innerCount) && (plan.OuterStride == outerStride) && (plan.InnerStride == innerStride);
}

void TestExactDecode(TestContext& runner) noexcept
{
    const PalStaticCallResult information{MakePurgeInformation(0x1000U, 3U, 4U, 0x100U, 0x20U)};
    const auto result = PalTranslationCachePurgeDecoder::Decode(information);
    ASTERIA_EXPECT(runner, "PAL_PTCE_INFO fields decode into the exact nested-loop plan", result.HasValue(),
                   IsExactPlan(result.GetValue(), 0x1000U, 12U, 0x1360U, 3U, 4U, 0x100U, 0x20U));

    const auto repeatedAddress =
        PalTranslationCachePurgeDecoder::Decode(MakePurgeInformation(0xABCDEFU, 2U, 3U, 0U, 0U));
    ASTERIA_EXPECT(runner, "zero PAL strides are retained without an invented restriction", repeatedAddress.HasValue(),
                   IsExactPlan(repeatedAddress.GetValue(), 0xABCDEFU, 6U, 0xABCDEFU, 2U, 3U, 0U, 0U));
}

void TestQemuCompatibleResult(TestContext& runner) noexcept
{
    const auto result = PalTranslationCachePurgeDecoder::Decode(MakePurgeInformation(0U, 1U, 1U, 0U, 0U));
    ASTERIA_EXPECT(runner, "QEMU's one-by-one zero-stride PAL_PTCE_INFO result is accepted", result.HasValue(),
                   IsExactPlan(result.GetValue(), 0U, 1U, 0U, 1U, 1U, 0U, 0U));

    const auto unusedStrides =
        PalTranslationCachePurgeDecoder::Decode(MakePurgeInformation(MaximumUInt64, 1U, 1U, 0xFFFFFFFFU, 0xFFFFFFFFU));
    ASTERIA_EXPECT(runner, "unused one-by-one strides do not cause a false overflow rejection",
                   unusedStrides.HasValue(), unusedStrides.GetValue().LastPurgeAddress == MaximumUInt64);
}

void TestCallAndCountFailures(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(
        runner, "PAL unimplemented status is rejected",
        HasError(MakePurgeInformation(0U, 1U, 1U, 0U, 0U, -2), PalTranslationCachePurgeDecodeError::CallFailed));
    ASTERIA_EXPECT(
        runner, "PAL invalid-argument status is rejected",
        HasError(MakePurgeInformation(0U, 1U, 1U, 0U, 0U, -3), PalTranslationCachePurgeDecodeError::CallFailed));
    ASTERIA_EXPECT(
        runner, "unknown nonzero PAL status is rejected",
        HasError(MakePurgeInformation(0U, 1U, 1U, 0U, 0U, 1), PalTranslationCachePurgeDecodeError::CallFailed));

    ASTERIA_EXPECT(
        runner, "zero outer count is rejected",
        HasError(MakePurgeInformation(0U, 0U, 1U, 0U, 0U), PalTranslationCachePurgeDecodeError::EmptyPurgeLoop));
    ASTERIA_EXPECT(
        runner, "zero inner count is rejected",
        HasError(MakePurgeInformation(0U, 1U, 0U, 0U, 0U), PalTranslationCachePurgeDecodeError::EmptyPurgeLoop));
    ASTERIA_EXPECT(
        runner, "two zero counts are rejected as an empty loop",
        HasError(MakePurgeInformation(0U, 0U, 0U, 0U, 0U), PalTranslationCachePurgeDecodeError::EmptyPurgeLoop));
}

void TestOperationBound(TestContext& runner) noexcept
{
    const auto squareLimit = PalTranslationCachePurgeDecoder::Decode(MakePurgeInformation(0U, 256U, 256U, 0U, 0U));
    ASTERIA_EXPECT(runner, "a 256-by-256 purge loop reaches the operation bound exactly", squareLimit.HasValue(),
                   squareLimit.GetValue().OperationCount == MaximumTranslationCachePurgeOperations);

    const auto linearLimit = PalTranslationCachePurgeDecoder::Decode(MakePurgeInformation(0U, 65'536U, 1U, 0U, 0U));
    ASTERIA_EXPECT(runner, "a linear purge loop reaches the operation bound exactly", linearLimit.HasValue(),
                   linearLimit.GetValue().OperationCount == MaximumTranslationCachePurgeOperations);

    ASTERIA_EXPECT(runner, "one operation above the profile bound is rejected",
                   HasError(MakePurgeInformation(0U, 65'537U, 1U, 0U, 0U),
                            PalTranslationCachePurgeDecodeError::OperationLimitExceeded));
    ASTERIA_EXPECT(runner, "the maximum PAL counts are rejected without multiplication wrap",
                   HasError(MakePurgeInformation(0U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0U, 0U),
                            PalTranslationCachePurgeDecodeError::OperationLimitExceeded));
}

void TestAddressBoundaries(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 FinalOffset{9U};
    const auto exactMaximum =
        PalTranslationCachePurgeDecoder::Decode(MakePurgeInformation(MaximumUInt64 - FinalOffset, 2U, 2U, 3U, 2U));
    ASTERIA_EXPECT(runner, "the final purge may address the last UInt64 value exactly", exactMaximum.HasValue(),
                   exactMaximum.GetValue().OperationCount == 4U,
                   exactMaximum.GetValue().LastPurgeAddress == MaximumUInt64);
    ASTERIA_EXPECT(runner, "a final purge address one byte beyond UInt64 is rejected",
                   HasError(MakePurgeInformation((MaximumUInt64 - FinalOffset) + 1U, 2U, 2U, 3U, 2U),
                            PalTranslationCachePurgeDecodeError::AddressComputationOverflow));

    ASTERIA_EXPECT(runner, "operation-limit failure precedes address arithmetic",
                   HasError(MakePurgeInformation(MaximumUInt64, 65'537U, 1U, 0xFFFFFFFFU, 0xFFFFFFFFU),
                            PalTranslationCachePurgeDecodeError::OperationLimitExceeded));
}

void TestInputImmutabilityAndDeterminism(TestContext& runner) noexcept
{
    const PalStaticCallResult information{MakePurgeInformation(0x123456789ABCDEF0U, 7U, 9U, 0x40U, 0x80U)};
    const PalStaticCallResult snapshot{information};
    const auto first = PalTranslationCachePurgeDecoder::Decode(information);
    const auto second = PalTranslationCachePurgeDecoder::Decode(information);
    ASTERIA_EXPECT(runner, "decoding leaves every PAL return register unchanged", information.Status == snapshot.Status,
                   information.Result1 == snapshot.Result1, information.Result2 == snapshot.Result2,
                   information.Result3 == snapshot.Result3);
    ASTERIA_EXPECT(runner, "equal PAL inputs produce equal validated plans", first.HasValue(), second.HasValue(),
                   IsExactPlan(first.GetValue(), second.GetValue().BaseAddress, second.GetValue().OperationCount,
                               second.GetValue().LastPurgeAddress, second.GetValue().OuterCount,
                               second.GetValue().InnerCount, second.GetValue().OuterStride,
                               second.GetValue().InnerStride));

    PalStaticCallResult failed{information};
    failed.Status = -2;
    const PalStaticCallResult failedSnapshot{failed};
    static_cast<void>(PalTranslationCachePurgeDecoder::Decode(failed));
    ASTERIA_EXPECT(runner, "failed decoding also leaves every PAL return register unchanged",
                   failed.Status == failedSnapshot.Status, failed.Result1 == failedSnapshot.Result1,
                   failed.Result2 == failedSnapshot.Result2, failed.Result3 == failedSnapshot.Result3);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"PalTranslationCachePurgeDecoderTests"};
    Asteria::Test::TestExactDecode(runner);
    Asteria::Test::TestQemuCompatibleResult(runner);
    Asteria::Test::TestCallAndCountFailures(runner);
    Asteria::Test::TestOperationBound(runner);
    Asteria::Test::TestAddressBoundaries(runner);
    Asteria::Test::TestInputImmutabilityAndDeterminism(runner);
    return runner.Finish();
}
