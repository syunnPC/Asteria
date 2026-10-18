// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_DMA_EXCLUSIONS_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_DMA_EXCLUSIONS_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
/// The firmware DMA reservations the kernel keeps out of every memory import: every VT-d RMRR and
/// AMD-Vi IVMD range the tables declare, in table order, up to BootDmaExclusionMaximumCount of
/// them, malformed ones included when their base does not exceed their limit. DeclaredCount counts
/// every RMRR and IVMD structure, so a count above the maximum (which stops the boot) is never
/// mistaken for a complete list.
constexpr Foundation::Size BootDmaExclusionMaximumCount{64U};

/// Which table declared a range.
constexpr Foundation::UInt8 BootDmaExclusionRmrr{1U};
constexpr Foundation::UInt8 BootDmaExclusionIvmd{2U};

/// One range as declared: Base and Limit are its first and last byte (an
/// IVMD's start and length become the enclosing pages, as the IVRS parser
/// rounds them); Malformed is 1 when the structure failed the parser's other
/// checks, which drop it from the retained topology.
struct BootDmaExclusion
{
    Foundation::UInt64 Base;
    Foundation::UInt64 Limit;
    Foundation::UInt16 Segment;
    Foundation::UInt8 Kind;
    Foundation::UInt8 Malformed;
    Foundation::UInt32 Reserved;
};

struct BootDmaExclusions
{
    Foundation::UInt32 Count;
    Foundation::UInt32 DeclaredCount;
    BootDmaExclusion Ranges[BootDmaExclusionMaximumCount];
};

/// Whether the list is well formed: Count within the storage and at most
/// DeclaredCount, each listed range known and ordered (Base <= Limit) with
/// zero reserved bytes, and every unused range zero. It does not judge
/// DeclaredCount: the kernel stops the boot when that exceeds the maximum.
[[nodiscard]] constexpr bool IsValidBootDmaExclusions(const BootDmaExclusions& list) noexcept
{
    if ((list.Count > BootDmaExclusionMaximumCount) || (list.Count > list.DeclaredCount))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < BootDmaExclusionMaximumCount; ++index)
    {
        const BootDmaExclusion& range = list.Ranges[index];
        if (index >= list.Count)
        {
            if ((range.Base != 0U) || (range.Limit != 0U) || (range.Segment != 0U) || (range.Kind != 0U) ||
                (range.Malformed != 0U) || (range.Reserved != 0U))
            {
                return false;
            }
            continue;
        }
        if ((range.Base > range.Limit) || (range.Malformed > 1U) || (range.Reserved != 0U) ||
            ((range.Kind != BootDmaExclusionRmrr) && (range.Kind != BootDmaExclusionIvmd)))
        {
            return false;
        }
    }
    return true;
}

/// Whether the list names every range the tables declare: DeclaredCount within the storage. The
/// kernel stops the boot otherwise, since an incomplete list could let a later import hand out
/// pages a device still reaches.
[[nodiscard]] constexpr bool IsCompleteBootDmaExclusions(const BootDmaExclusions& list) noexcept
{
    return list.DeclaredCount <= BootDmaExclusionMaximumCount;
}

static_assert(sizeof(BootDmaExclusion) == 24U);
static_assert(__builtin_offsetof(BootDmaExclusion, Segment) == 16U);
static_assert(sizeof(BootDmaExclusions) == 8U + (BootDmaExclusionMaximumCount * sizeof(BootDmaExclusion)));
static_assert(sizeof(BootDmaExclusions) == 1544U);
} // Namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_DMA_EXCLUSIONS_HPP_INCLUDED
