// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_RUNTIME_COMPILER_ARITHMETIC_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_RUNTIME_COMPILER_ARITHMETIC_HPP_INCLUDED

#include "Foundation/Types.hpp"

// IA-64 compiler runtime ABI. Unsigned division is a bounded software leaf;
// callers must check nonzero divisors. A zero divisor is an invariant failure.
extern "C" Asteria::Foundation::UInt64 __udivdi3(Asteria::Foundation::UInt64 numerator,
                                                 Asteria::Foundation::UInt64 denominator) noexcept;
extern "C" Asteria::Foundation::UInt64 __umoddi3(Asteria::Foundation::UInt64 numerator,
                                                 Asteria::Foundation::UInt64 denominator) noexcept;

#endif
