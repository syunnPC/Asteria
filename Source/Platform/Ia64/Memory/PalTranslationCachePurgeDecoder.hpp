// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_PAL_TRANSLATION_CACHE_PURGE_DECODER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_PAL_TRANSLATION_CACHE_PURGE_DECODER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
/// Bounds the interrupt-disabled local translation-cache purge loop in the
/// initial IA-64 target profile. PAL implementations requiring more operations
/// are rejected before activation begins.
constexpr Foundation::UInt64 MaximumTranslationCachePurgeOperations{65'536U};

enum class PalTranslationCachePurgeDecodeError : Foundation::UInt32
{
    None,
    CallFailed,
    EmptyPurgeLoop,
    OperationLimitExceeded,
    AddressComputationOverflow,
};

/// Retains the exact validated PAL_PTCE_INFO loop operands for one processor.
/// LastPurgeAddress is the final address consumed by ptc.e, not the value after
/// the loop's final stride increments.
struct PalTranslationCachePurgePlan
{
    Foundation::UInt64 BaseAddress;
    Foundation::UInt64 OperationCount;
    Foundation::UInt64 LastPurgeAddress;
    Foundation::UInt32 OuterCount;
    Foundation::UInt32 InnerCount;
    Foundation::UInt32 OuterStride;
    Foundation::UInt32 InnerStride;
};

/// Validates the registers returned by a static PAL_PTCE_INFO procedure.
/// The caller must obtain the result on the processor whose local translation
/// cache will be purged. This class neither invokes PAL nor purges anything.
class PalTranslationCachePurgeDecoder final
{
public:
    [[nodiscard]] static Foundation::Result<PalTranslationCachePurgePlan, PalTranslationCachePurgeDecodeError>
    Decode(const PalStaticCallResult& purgeInformation) noexcept;

private:
    PalTranslationCachePurgeDecoder() = delete;
};

static_assert(sizeof(PalTranslationCachePurgePlan) == 40U);
static_assert(alignof(PalTranslationCachePurgePlan) == 8U);
static_assert(sizeof(PalTranslationCachePurgeDecodeError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_PAL_TRANSLATION_CACHE_PURGE_DECODER_HPP_INCLUDED
