// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_LIBC_COMPILER_ARITHMETIC_HPP_INCLUDED
#define ASTERIA_USER_LIBC_COMPILER_ARITHMETIC_HPP_INCLUDED

#include "Include/asteria_libc.h"

#include <stdint.h>

// IA-64 GCC's compiler ABI. Hosted builds expose only prefixed names for verification.
// AMD64 emits hardware integer division and exports none of these helpers.
#if !defined(ASTERIA_TARGET_AMD64)
extern "C"
{
[[nodiscard]] uint64_t ASTERIA_LIBC_NAME(__udivdi3)(uint64_t numerator, uint64_t denominator) noexcept;
[[nodiscard]] uint64_t ASTERIA_LIBC_NAME(__umoddi3)(uint64_t numerator, uint64_t denominator) noexcept;
[[nodiscard]] int64_t ASTERIA_LIBC_NAME(__divdi3)(int64_t numerator, int64_t denominator) noexcept;
[[nodiscard]] int64_t ASTERIA_LIBC_NAME(__moddi3)(int64_t numerator, int64_t denominator) noexcept;
}
#endif

#endif
