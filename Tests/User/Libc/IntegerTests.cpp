// SPDX-License-Identifier: Apache-2.0
#include "LibcTestSupport.hpp"

#include <cerrno>
#include <cstring>
#include <climits>
#include <cinttypes>

#include "User/Libc/Include/stdlib.h"
#include "User/Libc/Include/inttypes.h"
#include "User/Libc/Include/errno.h"

namespace Asteria::Test::Libc
{
namespace
{
template <typename T>
void Compare(TestContext& context, const char* const text, const int base, T (*const host)(const char*, char**, int),
             T (*const own)(const char*, char**, int) noexcept) noexcept
{
    char* hostEnd{nullptr};
    char* ownEnd{nullptr};
    errno = EDOM;
    const T expected = host(text, &hostEnd, base);
    const int hostError{errno};
    ASTERIA_LIBC_ERRNO = EDOM;
    const T actual = own(text, &ownEnd, base);
    ASTERIA_EXPECT(context, "integer result, endptr and errno match glibc", actual == expected, hostEnd == ownEnd,
                   ASTERIA_LIBC_ERRNO == hostError);
    if (actual != expected || hostEnd != ownEnd || ASTERIA_LIBC_ERRNO != hostError)
    {
        static_cast<void>(std::fprintf(stderr, "text=%s base=%d\n", text, base));
    }
}
void All(TestContext& context, const char* const text, const int base) noexcept
{
    Compare(context, text, base, std::strtol, asteria_libc_strtol);
    Compare(context, text, base, std::strtoul, asteria_libc_strtoul);
    Compare(context, text, base, std::strtoll, asteria_libc_strtoll);
    Compare(context, text, base, std::strtoull, asteria_libc_strtoull);
    Compare(context, text, base, ::strtoimax, asteria_libc_strtoimax);
    Compare(context, text, base, ::strtoumax, asteria_libc_strtoumax);
}
} // namespace
} // namespace Asteria::Test::Libc

int main()
{
    Asteria::Test::Libc::TestContext context{"LibcInteger"};
    Asteria::Test::Libc::Random random;
    constexpr const char* Edges[]{"",
                                  " ",
                                  "+",
                                  "-",
                                  "0x",
                                  "0xg",
                                  "0Xf",
                                  "0129",
                                  "-0",
                                  "9223372036854775807",
                                  "9223372036854775808",
                                  "-9223372036854775808",
                                  "-9223372036854775809",
                                  "18446744073709551615",
                                  "-18446744073709551615",
                                  "18446744073709551616",
                                  "-18446744073709551616"};
    for (const char* const text : Edges)
    {
        for (int base{0}; base <= 36; ++base)
        {
            if (base != 1)
            {
                Asteria::Test::Libc::All(context, text, base);
            }
        }
    }
    for (size_t step{0U}; step < 6000U && context.GetFailureCount() < 10U; ++step)
    {
        char text[160U]{};
        size_t offset{0U};
        text[offset++] = ' ';
        text[offset++] = (random.Next() & 1U) != 0U ? '-' : '+';
        const size_t length{static_cast<size_t>(random.Next() % 140U)};
        const unsigned int alphabet{static_cast<unsigned int>(random.Next() % 35U) + 2U};
        for (size_t index{0U}; index < length; ++index)
        {
            text[offset++] = "0123456789abcdefghijklmnopqrstuvwxyz"[random.Next() % alphabet];
        }
        text[offset] = '\0';
        const int base{step % 7U == 0U ? 0 : static_cast<int>(random.Next() % 35U) + 2};
        Asteria::Test::Libc::All(context, text, base);
    }
    ASTERIA_EXPECT(context, "C absolute/division interfaces", asteria_libc_abs(-7) == 7, asteria_libc_labs(-9L) == 9L,
                   asteria_libc_llabs(-11LL) == 11LL, asteria_libc_imaxabs(-13) == 13,
                   asteria_libc_div(-7, 3).quot == -2, asteria_libc_div(-7, 3).rem == -1,
                   asteria_libc_ldiv(7L, -3L).quot == -2, asteria_libc_lldiv(7LL, -3LL).rem == 1,
                   asteria_libc_imaxdiv(-7, -3).quot == 2, asteria_libc_atoi("-19x") == -19,
                   asteria_libc_atol("+1234!") == 1234L, asteria_libc_atoll("-99999999") == -99999999LL);
    asteria_libc_srand(1U);
    ASTERIA_EXPECT(context, "documented LCG sequence", asteria_libc_rand() == 1103527590,
                   asteria_libc_rand() == 377401575, asteria_libc_rand() == 662824084);
    return context.Finish();
}
