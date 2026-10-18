// SPDX-License-Identifier: Apache-2.0

#include "Include/stdlib.h"

#include <limits.h>

#include "Include/inttypes.h"
#include "Include/ctype.h"
#include "Include/errno.h"
#include "LibcHooks.hpp"

namespace Asteria::User::Libc
{
static_assert(sizeof(long) == 8U && sizeof(long long) == 8U && sizeof(intmax_t) == 8U);
namespace
{
uint32_t g_random{1U};
struct IntegerResult
{
    uintmax_t Magnitude;
    bool Negative;
    bool Overflow;
};

[[nodiscard]] int Digit(const char character) noexcept
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'z')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'Z')
    {
        return character - 'A' + 10;
    }
    return 36;
}

[[nodiscard]] IntegerResult ParseInteger(const char* const text, char** const end, int base,
                                         const uintmax_t positiveLimit, const uintmax_t negativeLimit) noexcept
{
    if (end != nullptr)
    {
        *end = const_cast<char*>(text); // C ABI: borrow retains the original mutability.
    }
    if (base != 0 && (base < 2 || base > 36))
    {
        SetError(EINVAL);
        return IntegerResult{.Magnitude = 0U, .Negative = false, .Overflow = false};
    }
    const char* cursor{text};
    while (ASTERIA_LIBC_NAME(isspace)(static_cast<unsigned char>(*cursor)))
    {
        ++cursor;
    }
    const bool negative = *cursor == '-';
    if (*cursor == '-' || *cursor == '+')
    {
        ++cursor;
    }
    if ((base == 0 || base == 16) && cursor[0U] == '0' && (cursor[1U] == 'x' || cursor[1U] == 'X') &&
        Digit(cursor[2U]) < 16)
    {
        base = 16;
        cursor += 2U;
    }
    if (base == 0)
    {
        base = *cursor == '0' ? 8 : 10;
    }
    const char* const start = cursor;
    const uintmax_t limit = negative ? negativeLimit : positiveLimit;
    const auto radix = static_cast<unsigned int>(base);
    uintmax_t value{0U};
    bool overflow{false};
    while (Digit(*cursor) < base)
    {
        const auto digit = static_cast<unsigned int>(Digit(*cursor));
        if (value > limit / radix || (value == limit / radix && digit > limit % radix))
        {
            overflow = true;
        }
        else if (!overflow)
        {
            value = value * radix + digit;
        }
        ++cursor;
    }
    if (cursor == start)
    {
        return IntegerResult{.Magnitude = 0U, .Negative = false, .Overflow = false};
    }
    if (end != nullptr)
    {
        *end = const_cast<char*>(cursor);
    }
    if (overflow)
    {
        SetError(ERANGE);
        value = limit;
    }
    return IntegerResult{.Magnitude = value, .Negative = negative, .Overflow = overflow};
}

template <typename TSigned>
[[nodiscard]] TSigned SignedInteger(const char* const text, char** const end, const int base,
                                    const TSigned maximum) noexcept
{
    const uintmax_t limit = static_cast<uintmax_t>(maximum);
    const IntegerResult result = ParseInteger(text, end, base, limit, limit + 1U);
    if (!result.Negative)
    {
        return static_cast<TSigned>(result.Magnitude);
    }
    if (result.Magnitude == 0U)
    {
        return 0;
    }
    return static_cast<TSigned>(-static_cast<TSigned>(result.Magnitude - 1U) - 1);
}
}
}

extern "C" long ASTERIA_LIBC_NAME(strtol)(const char* const text, char** const end, const int base) noexcept
{
    return Asteria::User::Libc::SignedInteger(text, end, base, LONG_MAX);
}

extern "C" long long ASTERIA_LIBC_NAME(strtoll)(const char* const text, char** const end, const int base) noexcept
{
    return Asteria::User::Libc::SignedInteger(text, end, base, static_cast<long long>(INTMAX_MAX));
}

extern "C" intmax_t ASTERIA_LIBC_NAME(strtoimax)(const char* const text, char** const end, const int base) noexcept
{
    return Asteria::User::Libc::SignedInteger(text, end, base, INTMAX_MAX);
}

extern "C" unsigned long ASTERIA_LIBC_NAME(strtoul)(const char* const text, char** const end, const int base) noexcept
{
    const auto result = Asteria::User::Libc::ParseInteger(text, end, base, ULONG_MAX, ULONG_MAX);
    // Modulo arithmetic is the C unsigned-conversion contract (overflow is clamped first).
    if (result.Overflow)
    {
        return ULONG_MAX;
    }
    return result.Negative ? 0UL - static_cast<unsigned long>(result.Magnitude)
                           : static_cast<unsigned long>(result.Magnitude);
}

extern "C" unsigned long long ASTERIA_LIBC_NAME(strtoull)(const char* const text, char** const end,
                                                          const int base) noexcept
{
    const auto result = Asteria::User::Libc::ParseInteger(text, end, base, UINTMAX_MAX, UINTMAX_MAX);
    if (result.Overflow)
    {
        return static_cast<unsigned long long>(UINTMAX_MAX);
    }
    return result.Negative ? 0ULL - static_cast<unsigned long long>(result.Magnitude)
                           : static_cast<unsigned long long>(result.Magnitude);
}

extern "C" uintmax_t ASTERIA_LIBC_NAME(strtoumax)(const char* const text, char** const end, const int base) noexcept
{
    return ASTERIA_LIBC_NAME(strtoul)(text, end, base);
}

extern "C" int ASTERIA_LIBC_NAME(atoi)(const char* const text) noexcept
{
    return Asteria::User::Libc::SignedInteger(text, nullptr, 10, INT_MAX);
}

extern "C" long ASTERIA_LIBC_NAME(atol)(const char* const text) noexcept
{
    return ASTERIA_LIBC_NAME(strtol)(text, nullptr, 10);
}

extern "C" long long ASTERIA_LIBC_NAME(atoll)(const char* const text) noexcept
{
    return ASTERIA_LIBC_NAME(strtoll)(text, nullptr, 10);
}

extern "C" int ASTERIA_LIBC_NAME(abs)(const int value) noexcept
{
    return value < 0 ? -value : value;
}
extern "C" long ASTERIA_LIBC_NAME(labs)(const long value) noexcept
{
    return value < 0 ? -value : value;
}
extern "C" long long ASTERIA_LIBC_NAME(llabs)(const long long value) noexcept
{
    return value < 0 ? -value : value;
}
extern "C" intmax_t ASTERIA_LIBC_NAME(imaxabs)(const intmax_t value) noexcept
{
    return value < 0 ? -value : value;
}
extern "C" ASTERIA_LIBC_TYPE(div_t) ASTERIA_LIBC_NAME(div)(const int numerator, const int denominator) noexcept
{
    return ASTERIA_LIBC_TYPE(div_t){.quot = numerator / denominator, .rem = numerator % denominator};
}
extern "C" ASTERIA_LIBC_TYPE(ldiv_t) ASTERIA_LIBC_NAME(ldiv)(const long numerator, const long denominator) noexcept
{
    return ASTERIA_LIBC_TYPE(ldiv_t){.quot = numerator / denominator, .rem = numerator % denominator};
}
extern "C" ASTERIA_LIBC_TYPE(lldiv_t)
    ASTERIA_LIBC_NAME(lldiv)(const long long numerator, const long long denominator) noexcept
{
    return ASTERIA_LIBC_TYPE(lldiv_t){.quot = numerator / denominator, .rem = numerator % denominator};
}
extern "C" ASTERIA_LIBC_TYPE(imaxdiv_t)
    ASTERIA_LIBC_NAME(imaxdiv)(const intmax_t numerator, const intmax_t denominator) noexcept
{
    return ASTERIA_LIBC_TYPE(imaxdiv_t){.quot = numerator / denominator, .rem = numerator % denominator};
}
extern "C" int ASTERIA_LIBC_NAME(rand)() noexcept
{
    // POSIX example LCG, reduced modulo 2^31; deterministic on both ISAs.
    const auto& lock = Asteria::User::Libc::GetHooks().HeapLock;
    Asteria::User::Libc::Lock(lock);
    Asteria::User::Libc::g_random = (Asteria::User::Libc::g_random * 1103515245U + 12345U) & 0x7FFFFFFFU;
    const auto result = static_cast<int>(Asteria::User::Libc::g_random);
    Asteria::User::Libc::Unlock(lock);
    return result;
}
extern "C" void ASTERIA_LIBC_NAME(srand)(const unsigned int seed) noexcept
{
    const auto& lock = Asteria::User::Libc::GetHooks().HeapLock;
    Asteria::User::Libc::Lock(lock);
    Asteria::User::Libc::g_random = seed;
    Asteria::User::Libc::Unlock(lock);
}
