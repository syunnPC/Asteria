// SPDX-License-Identifier: Apache-2.0

#include "CompilerArithmetic.hpp"

#include "Include/assert.h"

#if !defined(ASTERIA_TARGET_AMD64)
namespace Asteria::User::Libc
{
namespace
{
struct QuotientRemainder
{
    uint64_t Quotient;
    uint64_t Remainder;
};

[[nodiscard]] QuotientRemainder Divide(const uint64_t numerator, const uint64_t denominator) noexcept
{
    if (denominator == 0U)
    {
        ASTERIA_LIBC_NAME(__asteria_assert_fail)("nonzero divisor", __FILE__, __LINE__, "Divide");
    }
    uint64_t quotient{0U};
    uint64_t remainder{0U};
    for (unsigned int remaining{64U}; remaining != 0U; --remaining)
    {
        const unsigned int bit{remaining - 1U};
        const bool carry{(remainder >> 63U) != 0U};
        remainder = (remainder << 1U) | ((numerator >> bit) & 1U);
        if (carry || remainder >= denominator)
        {
            // Modulo subtraction also handles the explicit 65th carry bit;
            // because the previous remainder is below denominator, this fits.
            remainder -= denominator;
            quotient |= uint64_t{1U} << bit;
        }
    }
    return QuotientRemainder{.Quotient = quotient, .Remainder = remainder};
}

[[nodiscard]] uint64_t Magnitude(const int64_t value) noexcept
{
    const auto bits = static_cast<uint64_t>(value);
    return value < 0 ? 0U - bits : bits; // Intentional two's-complement modulo conversion.
}

[[nodiscard]] int64_t SignedBits(const uint64_t value) noexcept
{
    if (value <= INT64_MAX)
    {
        return static_cast<int64_t>(value);
    }
    return -static_cast<int64_t>(UINT64_MAX - value) - 1;
}
}
}

extern "C" uint64_t ASTERIA_LIBC_NAME(__udivdi3)(const uint64_t numerator, const uint64_t denominator) noexcept
{
    return Asteria::User::Libc::Divide(numerator, denominator).Quotient;
}
extern "C" uint64_t ASTERIA_LIBC_NAME(__umoddi3)(const uint64_t numerator, const uint64_t denominator) noexcept
{
    return Asteria::User::Libc::Divide(numerator, denominator).Remainder;
}
extern "C" int64_t ASTERIA_LIBC_NAME(__divdi3)(const int64_t numerator, const int64_t denominator) noexcept
{
    const auto quotient = Asteria::User::Libc::Divide(Asteria::User::Libc::Magnitude(numerator),
                                                      Asteria::User::Libc::Magnitude(denominator))
                              .Quotient;
    return Asteria::User::Libc::SignedBits((numerator < 0) != (denominator < 0) ? 0U - quotient : quotient);
}
extern "C" int64_t ASTERIA_LIBC_NAME(__moddi3)(const int64_t numerator, const int64_t denominator) noexcept
{
    const auto remainder = Asteria::User::Libc::Divide(Asteria::User::Libc::Magnitude(numerator),
                                                       Asteria::User::Libc::Magnitude(denominator))
                               .Remainder;
    return Asteria::User::Libc::SignedBits(numerator < 0 ? 0U - remainder : remainder);
}
#endif
