// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Arithmetic.hpp"
#include "User/Graphics/Composition/Divide.hpp"
#if defined(ASTERIA_TARGET_IA64)
namespace
{
using Asteria::Foundation::Int64;
using Asteria::Foundation::UInt64;
[[nodiscard]] UInt64 Magnitude(const Int64 value) noexcept
{
    const auto bits = static_cast<UInt64>(value);
    return value < 0 ? 0U - bits : bits;
}
[[nodiscard]] Int64 Signed(const UInt64 value) noexcept
{
    return value <= 0x7fffffffffffffffULL ? static_cast<Int64>(value) : -static_cast<Int64>(~value) - 1;
}
} // namespace
extern "C" Asteria::Foundation::UInt64 __udivdi3(const Asteria::Foundation::UInt64 numerator,
                                                 const Asteria::Foundation::UInt64 denominator) noexcept
{
    if (denominator == 0U)
    {
        __builtin_trap();
    }
    return Asteria::User::Graphics::Composition::DivideCounter(numerator, denominator);
}
extern "C" Asteria::Foundation::Int64 __divdi3(const Asteria::Foundation::Int64 numerator,
                                               const Asteria::Foundation::Int64 denominator) noexcept
{
    const auto quotient = __udivdi3(Magnitude(numerator), Magnitude(denominator));
    return Signed((numerator < 0) != (denominator < 0) ? 0U - quotient : quotient);
}
extern "C" Asteria::Foundation::Int64 __moddi3(const Asteria::Foundation::Int64 numerator,
                                               const Asteria::Foundation::Int64 denominator) noexcept
{
    const auto n = Magnitude(numerator);
    const auto d = Magnitude(denominator);
    const auto remainder = n - __udivdi3(n, d) * d;
    return Signed(numerator < 0 ? 0U - remainder : remainder);
}
#endif
