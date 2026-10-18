// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_ARITHMETIC_HPP_INCLUDED
#define ASTERIA_USER_FONT_ARITHMETIC_HPP_INCLUDED
#include "Foundation/Types.hpp"
#if defined(ASTERIA_TARGET_IA64)
// IA-64 compiler ABI, only in the freestanding UI image. Shared bounded division is the project's
// Composition::DivideCounter.
extern "C"
{
[[nodiscard]] Asteria::Foundation::UInt64 __udivdi3(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept;
[[nodiscard]] Asteria::Foundation::Int64 __divdi3(Asteria::Foundation::Int64, Asteria::Foundation::Int64) noexcept;
[[nodiscard]] Asteria::Foundation::Int64 __moddi3(Asteria::Foundation::Int64, Asteria::Foundation::Int64) noexcept;
}
#endif
#endif
