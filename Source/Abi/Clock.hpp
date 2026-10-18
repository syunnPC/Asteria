// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_CLOCK_HPP_INCLUDED
#define ASTERIA_ABI_CLOCK_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Abi
{
inline constexpr Foundation::UInt64 MaximumForwardClockDelta{~Foundation::UInt64{0U} >> 1U};
[[nodiscard]] constexpr Foundation::UInt64 ClampClockTicks(const Foundation::UInt64 now,
                                                           const Foundation::UInt64 previous) noexcept
{
    return (now - previous) <= MaximumForwardClockDelta ? now : previous;
}
// A small cross-processor skew yields zero elapsed. Finite intervals span less than half the
// counter period; modular forward deltas also preserve legitimate counter wrap (the terminal's host
// model).
[[nodiscard]] constexpr Foundation::UInt64 ElapsedClockTicks(const Foundation::UInt64 now,
                                                             const Foundation::UInt64 before) noexcept
{
    const auto delta = now - before;
    return delta <= MaximumForwardClockDelta ? delta : 0U;
}
}
#endif
