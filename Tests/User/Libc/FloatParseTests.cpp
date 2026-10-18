// SPDX-License-Identifier: Apache-2.0
#include "LibcTestSupport.hpp"

#include <cerrno>
#include <cstring>
#include <cmath>

#include "User/Libc/Include/stdlib.h"
#include "User/Libc/Include/errno.h"

namespace Asteria::Test::Libc
{
namespace
{
[[nodiscard]] bool Compare(TestContext& context, const char* const text, const size_t step) noexcept
{
    char* hostEnd{nullptr};
    char* ownEnd{nullptr};
    errno = 0;
    const double host = std::strtod(text, &hostEnd);
    const int hostError{errno};
    ASTERIA_LIBC_ERRNO = 0;
    const double own = asteria_libc_strtod(text, &ownEnd);
    const int ownError{ASTERIA_LIBC_ERRNO};
    const bool doubles = std::bit_cast<std::uint64_t>(own) == std::bit_cast<std::uint64_t>(host) && hostEnd == ownEnd &&
                         hostError == ownError;
    ASTERIA_EXPECT(context, "decimal/hex double bits, endptr and errno match glibc", doubles);
    if (!doubles)
    {
        static_cast<void>(std::fprintf(stderr, "step=%zu text=%.1400s host=%a err=%d end=%td own=%a err=%d end=%td\n",
                                       step, text, host, hostError, hostEnd - text, own, ownError, ownEnd - text));
    }
    errno = 0;
    const float hostFloat = std::strtof(text, &hostEnd);
    const int hostFloatError{errno};
    ASTERIA_LIBC_ERRNO = 0;
    const float ownFloat = asteria_libc_strtof(text, &ownEnd);
    const bool floats = std::bit_cast<std::uint32_t>(ownFloat) == std::bit_cast<std::uint32_t>(hostFloat) &&
                        hostEnd == ownEnd && hostFloatError == ASTERIA_LIBC_ERRNO;
    ASTERIA_EXPECT(context, "decimal/hex float is rounded directly to binary32", floats);
    if (!floats)
    {
        static_cast<void>(std::fprintf(stderr, "step=%zu text=%.1400s host=%a err=%d end=%td own=%a err=%d end=%td\n",
                                       step, text, static_cast<double>(hostFloat), hostFloatError, hostEnd - text,
                                       static_cast<double>(ownFloat), ASTERIA_LIBC_ERRNO, ownEnd - text));
    }
    return doubles && floats;
}
} // namespace
} // namespace Asteria::Test::Libc

int main()
{
    Asteria::Test::Libc::TestContext context{"LibcFloatParse"};
    Asteria::Test::Libc::Random random;
    constexpr const char* Boundaries[]{"",
                                       " ",
                                       "-",
                                       ".",
                                       "-0",
                                       "0x",
                                       "0x.p1",
                                       "0x0.0p-100000",
                                       "1e",
                                       "1e+",
                                       "1e-100000",
                                       "1e100000",
                                       "0e100000",
                                       "  +1.2xyz",
                                       "infinit",
                                       "-INFINITYtail",
                                       "NaN",
                                       "nan()",
                                       "nan(payload)",
                                       "-nan(0x1234)",
                                       "nan(9999999999999999999999999999999999)",
                                       "nan(09)",
                                       "nan(foo",
                                       "nan(+1)",
                                       "0x1",
                                       "0x1p",
                                       "0x1.8p+2z",
                                       "0x1p-1074",
                                       "0x1p-1075",
                                       "0x1.0000000000001p-1075",
                                       "0x1.fffffffffffffp-1023",
                                       "0x1.fffffffffffffp1023",
                                       "0x1.fffffffffffff8p1023",
                                       "0x1p-149",
                                       "0x1p-150",
                                       "0x1.fffffep127",
                                       "0x1.ffffffp127",
                                       "2.2250738585072014e-308",
                                       "2.2250738585072012e-308",
                                       "2.2250738585072011e-308",
                                       "4.9406564584124654e-324",
                                       "2.4703282292062327e-324",
                                       "1.7976931348623157e308",
                                       "1.7976931348623159e308",
                                       "1.1754943508222875e-38",
                                       "1.401298464324817e-45",
                                       "1.000000059604644775390625",
                                       "1.0000000596046447753906250001",
                                       "1.00000000000000011102230246251565404236316680908203125",
                                       "1.000000000000000111022302462515654042363166809082031250001"};
    size_t step{0U};
    for (const char* const text : Boundaries)
    {
        static_cast<void>(Asteria::Test::Libc::Compare(context, text, step++));
    }
    for (; step < 16000U && context.GetFailureCount() < 12U; ++step)
    {
        char text[1600U]{};
        size_t offset{0U};
        text[offset++] = (random.Next() & 1U) != 0U ? '-' : '+';
        const bool hex{step % 2U == 0U};
        if (hex)
        {
            text[offset++] = '0';
            text[offset++] = 'x';
        }
        const size_t digits{step % 23U == 0U ? 1300U : static_cast<size_t>(random.Next() % 120U) + 1U};
        const size_t point{static_cast<size_t>(random.Next() % (digits + 1U))};
        for (size_t index{0U}; index <= digits; ++index)
        {
            if (index == point)
            {
                text[offset++] = '.';
            }
            if (index < digits)
            {
                text[offset++] = "0123456789abcdef"[random.Next() % (hex ? 16U : 10U)];
            }
        }
        const int exponent{static_cast<int>(random.Next() % (hex ? 2800U : 1000U)) - (hex ? 1400 : 500)};
        static_cast<void>(std::snprintf(text + offset, sizeof(text) - offset, "%c%+dtail", hex ? 'p' : 'e', exponent));
        static_cast<void>(Asteria::Test::Libc::Compare(context, text, step));
    }
    // A nonzero digit beyond retained precision breaks an exact rounding tie upward.
    char sticky[1500U]{};
    const char* const tie{"1.00000000000000011102230246251565404236316680908203125"};
    std::strcpy(sticky, tie);
    size_t length{std::strlen(sticky)};
    while (length < 1400U)
    {
        sticky[length++] = '0';
    }
    sticky[length++] = '1';
    sticky[length] = '\0';
    static_cast<void>(Asteria::Test::Libc::Compare(context, sticky, step));
    return context.Finish();
}
