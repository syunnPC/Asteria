// SPDX-License-Identifier: Apache-2.0

#include "User/Graphics/Composition/Divide.hpp"
#if defined(ASTERIA_TARGET_IA64)
// Unsigned remainder for toolkit layout and repeat intervals, without libgcc.
extern "C" Asteria::Foundation::UInt64 __umoddi3(const Asteria::Foundation::UInt64 numerator,
                                                 const Asteria::Foundation::UInt64 denominator) noexcept
{
    if (denominator == 0U)
    {
        __builtin_trap();
    }
    return numerator - Asteria::User::Graphics::Composition::DivideCounter(numerator, denominator) * denominator;
}
#endif
