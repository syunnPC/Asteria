// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/PalTranslationCachePurgeDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] Foundation::Result<PalTranslationCachePurgePlan, PalTranslationCachePurgeDecodeError>
Fail(const PalTranslationCachePurgeDecodeError error) noexcept
{
    return Foundation::Result<PalTranslationCachePurgePlan, PalTranslationCachePurgeDecodeError>::Failure(error);
}

[[nodiscard]] bool TryAdd(const Foundation::UInt64 left, const Foundation::UInt64 right,
                          Foundation::UInt64& sum) noexcept
{
    if (left > (MaximumUInt64 - right))
    {
        return false;
    }
    sum = left + right;
    return true;
}
} // namespace

Foundation::Result<PalTranslationCachePurgePlan, PalTranslationCachePurgeDecodeError>
PalTranslationCachePurgeDecoder::Decode(const PalStaticCallResult& purgeInformation) noexcept
{
    if (purgeInformation.Status != 0)
    {
        return Fail(PalTranslationCachePurgeDecodeError::CallFailed);
    }

    const Foundation::UInt32 outerCount = static_cast<Foundation::UInt32>(purgeInformation.Result2 >> 32U);
    const Foundation::UInt32 innerCount = static_cast<Foundation::UInt32>(purgeInformation.Result2);
    const Foundation::UInt32 outerStride = static_cast<Foundation::UInt32>(purgeInformation.Result3 >> 32U);
    const Foundation::UInt32 innerStride = static_cast<Foundation::UInt32>(purgeInformation.Result3);

    if ((outerCount == 0U) || (innerCount == 0U))
    {
        return Fail(PalTranslationCachePurgeDecodeError::EmptyPurgeLoop);
    }

    const Foundation::UInt64 operationCount =
        static_cast<Foundation::UInt64>(outerCount) * static_cast<Foundation::UInt64>(innerCount);
    if (operationCount > MaximumTranslationCachePurgeOperations)
    {
        return Fail(PalTranslationCachePurgeDecodeError::OperationLimitExceeded);
    }

    // PAL's nested loop increments by InnerStride after every purge and by
    // OuterStride after each completed inner loop. Before the final purge it
    // has therefore applied OperationCount - 1 inner strides and
    // OuterCount - 1 outer strides. The operation bound makes both products
    // narrower than 49 bits, but the additions are still checked explicitly.
    const Foundation::UInt64 innerOffset = (operationCount - 1U) * static_cast<Foundation::UInt64>(innerStride);
    const Foundation::UInt64 outerOffset =
        (static_cast<Foundation::UInt64>(outerCount) - 1U) * static_cast<Foundation::UInt64>(outerStride);
    Foundation::UInt64 finalOffset{0U};
    if (!TryAdd(innerOffset, outerOffset, finalOffset))
    {
        return Fail(PalTranslationCachePurgeDecodeError::AddressComputationOverflow);
    }
    Foundation::UInt64 lastPurgeAddress{0U};
    if (!TryAdd(purgeInformation.Result1, finalOffset, lastPurgeAddress))
    {
        return Fail(PalTranslationCachePurgeDecodeError::AddressComputationOverflow);
    }

    return Foundation::Result<PalTranslationCachePurgePlan, PalTranslationCachePurgeDecodeError>::Success(
        PalTranslationCachePurgePlan{
            .BaseAddress = purgeInformation.Result1,
            .OperationCount = operationCount,
            .LastPurgeAddress = lastPurgeAddress,
            .OuterCount = outerCount,
            .InnerCount = innerCount,
            .OuterStride = outerStride,
            .InnerStride = innerStride,
        });
}
} // namespace Asteria::Platform::Ia64::Memory
