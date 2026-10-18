// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_COMPOSITION_DIVIDE_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_COMPOSITION_DIVIDE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Graphics::Composition
{
/// Geometry division without compiler-runtime dependencies on freestanding
/// IA-64. At most 32 steps; denominator must be nonzero, zero safely yields 0.
[[nodiscard]] constexpr Foundation::UInt32 Divide(const Foundation::UInt32 numerator,
                                                  const Foundation::UInt32 denominator) noexcept
{
    if (denominator == 0U)
    {
        return 0U;
    }
    if (denominator == 1U)
    {
        return numerator;
    }
    if (denominator == 2U)
    {
        return numerator >> 1U;
    }
    if (denominator == 4U)
    {
        return numerator >> 2U;
    }
    if (denominator == 8U)
    {
        return numerator >> 3U;
    }
    if (denominator == 16U)
    {
        return numerator >> 4U;
    }
    Foundation::UInt64 remainder{0U};
    Foundation::UInt32 quotient{0U};
    for (Foundation::UInt32 bit{32U}; bit != 0U; --bit)
    {
        remainder = (remainder << 1U) | ((numerator >> (bit - 1U)) & 1U);
        if (remainder >= denominator)
        {
            remainder -= denominator;
            quotient |= Foundation::UInt32{1U} << (bit - 1U);
        }
    }
    return quotient;
}
/// Counter scaling once per pass. Unlike geometry, the counter and scale are
/// 64-bit; use bounded division so IA-64 components need no compiler runtime.
[[nodiscard]] constexpr Foundation::UInt64 DivideCounter(const Foundation::UInt64 numerator,
                                                         const Foundation::UInt64 denominator) noexcept
{
    if (denominator == 0U)
    {
        return 0U;
    }
    Foundation::UInt64 quotient{0U};
    Foundation::UInt64 remainder{0U};
    for (Foundation::UInt32 bit{64U}; bit != 0U; --bit)
    {
        const bool carry = (remainder >> 63U) != 0U;
        remainder = (remainder << 1U) | ((numerator >> (bit - 1U)) & 1U);
        if (carry || (remainder >= denominator))
        {
            // Intentional wrap when carry represents the 65th remainder bit.
            remainder -= denominator;
            quotient |= Foundation::UInt64{1U} << (bit - 1U);
        }
    }
    return quotient;
}
} // namespace Asteria::User::Graphics::Composition
#endif // ASTERIA_USER_GRAPHICS_COMPOSITION_DIVIDE_HPP_INCLUDED
