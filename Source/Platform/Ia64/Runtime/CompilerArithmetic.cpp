// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Runtime/CompilerArithmetic.hpp"

#include "Platform/FatalTrap.hpp"

namespace Asteria::Platform::Ia64::Runtime
{
namespace
{
struct Division
{
    Foundation::UInt64 Quotient;
    Foundation::UInt64 Remainder;
};

// Binary long division: each iteration consumes one numerator bit and keeps
// remainder < denominator. The explicit carry is the 65th bit; subtraction
// deliberately wraps when it is set, leaving a representable remainder.
[[nodiscard]] Division Divide(const Foundation::UInt64 numerator, const Foundation::UInt64 denominator) noexcept
{
    if (denominator == 0U)
    {
        FatalTrap();
    }
    Division result{};
    for (Foundation::Size bit{64U}; bit != 0U;)
    {
        --bit;
        const bool carry{(result.Remainder >> 63U) != 0U};
        result.Remainder = (result.Remainder << 1U) | ((numerator >> bit) & 1U);
        if (carry || result.Remainder >= denominator)
        {
            result.Remainder -= denominator;
            result.Quotient |= Foundation::UInt64{1U} << bit;
        }
    }
    return result;
}
}
}

extern "C" Asteria::Foundation::UInt64 __udivdi3(const Asteria::Foundation::UInt64 numerator,
                                                 const Asteria::Foundation::UInt64 denominator) noexcept
{
    return Asteria::Platform::Ia64::Runtime::Divide(numerator, denominator).Quotient;
}
extern "C" Asteria::Foundation::UInt64 __umoddi3(const Asteria::Foundation::UInt64 numerator,
                                                 const Asteria::Foundation::UInt64 denominator) noexcept
{
    return Asteria::Platform::Ia64::Runtime::Divide(numerator, denominator).Remainder;
}
