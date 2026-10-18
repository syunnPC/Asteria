// SPDX-License-Identifier: Apache-2.0
#include "LibcTestSupport.hpp"

#include <cstdarg>
#include <cstring>
#include <limits>
#include <cinttypes>
#include <climits>

#include "User/Libc/Include/stdio.h"
#include "User/Libc/Include/errno.h"
#include "User/Libc/LibcHooks.hpp"

namespace Asteria::Test::Libc
{
namespace
{
[[nodiscard]] bool Compare(TestContext& context, const size_t capacity, const char* const format, ...) noexcept
{
    char expected[4096U];
    char actual[4096U];
    std::memset(expected, 0x5A, sizeof(expected));
    std::memset(actual, 0x5A, sizeof(actual));
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    const int host = std::vsnprintf(expected, capacity, format, arguments);
    const int own = asteria_libc_vsnprintf(actual, capacity, format, copy);
    va_end(copy);
    va_end(arguments);
    const bool same = host == own && std::memcmp(actual, expected, sizeof(actual)) == 0;
    ASTERIA_EXPECT(context, "formatter matches glibc including truncation and untouched tail", same);
    if (!same)
    {
        static_cast<void>(std::fprintf(stderr, "format=%s capacity=%zu host=%d [%s] own=%d [%s]\n", format, capacity,
                                       host, expected, own, actual));
    }
    return same;
}

void Integers(TestContext& context, Random& random) noexcept
{
    constexpr const char* Lengths[]{"hh", "h", "", "l", "ll", "j", "z", "t"};
    constexpr char Conversions[]{'d', 'i', 'u', 'o', 'x', 'X'};
    for (size_t step{0U}; step < 6000U && context.GetFailureCount() < 10U; ++step)
    {
        char format[64U]{};
        const size_t modifier = step % 8U;
        const char conversion = Conversions[(step / 8U) % 6U];
        const size_t flags{(step / 48U) % 32U};
        static_cast<void>(std::snprintf(format, sizeof(format), "A%%%s%s%s%s%s*.*%s%cZ", (flags & 1U) != 0U ? "-" : "",
                                        (flags & 2U) != 0U ? "+" : "", (flags & 4U) != 0U ? " " : "",
                                        (flags & 8U) != 0U ? "#" : "", (flags & 16U) != 0U ? "0" : "",
                                        Lengths[modifier], conversion));
        const int width{static_cast<int>(random.Next() % 65U) - 32};
        const int precision{static_cast<int>(random.Next() % 25U) - 5};
        const auto value = random.Next();
        const size_t capacity{step % 3U == 0U ? 128U : static_cast<size_t>(random.Next() % 80U)};
        if (conversion == 'd' || conversion == 'i')
        {
            const auto signedValue = std::bit_cast<std::int64_t>(value);
            if (modifier < 3U)
            {
                static_cast<void>(
                    Compare(context, capacity, format, width, precision, static_cast<int>(signedValue % INT32_MAX)));
            }
            else if (modifier == 4U)
            {
                static_cast<void>(
                    Compare(context, capacity, format, width, precision, static_cast<long long>(signedValue)));
            }
            else
            {
                static_cast<void>(Compare(context, capacity, format, width, precision, static_cast<long>(signedValue)));
            }
        }
        else if (modifier < 3U)
        {
            static_cast<void>(Compare(context, capacity, format, width, precision, static_cast<unsigned int>(value)));
        }
        else if (modifier == 4U)
        {
            static_cast<void>(
                Compare(context, capacity, format, width, precision, static_cast<unsigned long long>(value)));
        }
        else
        {
            static_cast<void>(Compare(context, capacity, format, width, precision, static_cast<unsigned long>(value)));
        }
    }
    static_cast<void>(Compare(context, 128U, "%d %lld %llu %#.0o %.0x", INT32_MIN, LLONG_MIN, ULLONG_MAX, 0U, 0U));
}

void FloatingPoint(TestContext& context, Random& random) noexcept
{
    constexpr char Conversions[]{'f', 'F', 'e', 'E', 'g', 'G', 'a', 'A'};
    constexpr double Edges[]{0.0,
                             -0.0,
                             0.5,
                             1.5,
                             2.5,
                             9.9995,
                             0.000099995,
                             0x1p-1074,
                             0x1.fffffffffffffp-1023,
                             0x1p-1022,
                             0x1.fffffffffffffp1023,
                             0x1.fffffffffffffp0,
                             0x1.0000000000001p0,
                             0x1.8p0,
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()};
    for (const double value : Edges)
    {
        for (const char conversion : Conversions)
        {
            for (unsigned int flags{0U}; flags < 32U; ++flags)
            {
                char format[64U]{};
                static_cast<void>(std::snprintf(format, sizeof(format), "%%%s%s%s%s%s*.*%c",
                                                (flags & 1U) != 0U ? "-" : "", (flags & 2U) != 0U ? "+" : "",
                                                (flags & 4U) != 0U ? " " : "", (flags & 8U) != 0U ? "#" : "",
                                                (flags & 16U) != 0U ? "0" : "", conversion));
                if (!Compare(context, 2048U, format, 28, static_cast<int>(flags % 15U) - 1, value))
                {
                    return;
                }
            }
        }
    }
    for (size_t step{0U}; step < 8000U && context.GetFailureCount() < 10U; ++step)
    {
        const std::uint64_t bits = random.Next();
        const double value = std::bit_cast<double>(bits);
        char format[64U]{};
        static_cast<void>(std::snprintf(format, sizeof(format), "%%+#0*.*%c!", Conversions[step % 8U]));
        const int precision{static_cast<int>(random.Next() % 60U) - 1};
        if (!Compare(context, static_cast<size_t>(random.Next() % 2000U), format,
                     static_cast<int>(random.Next() % 60U) - 20, precision, value))
        {
            static_cast<void>(std::fprintf(stderr, "step=%zu bits=%016llx precision=%d\n", step,
                                           static_cast<unsigned long long>(bits), precision));
        }
    }
    static_cast<void>(
        Compare(context, 4096U, "%.1100f %.1100e %.1100g %.1100a", 0x1p-1074, 0x1p-1074, 0x1p-1074, 0x1p-1074));
    char longHex[32U]{};
    ASTERIA_EXPECT(context, "documented binary64 long-double hex representation",
                   asteria_libc_snprintf(longHex, sizeof(longHex), "%La", 1.5L) == 8,
                   std::strcmp(longHex, "0x1.8p+0") == 0);
    static_cast<void>(Compare(context, 128U, "%Lf %Le %Lg", 1.5L, 1.5L, 1.5L));
}

struct StreamBuffer
{
    char Bytes[128U];
    size_t Used;
    size_t Calls;
    bool Fail;
};
[[nodiscard]] bool WriteStream(void* const context, const char* const bytes, const size_t count,
                               const size_t repetitions) noexcept
{
    auto& stream = *static_cast<StreamBuffer*>(context);
    ++stream.Calls;
    if (stream.Fail)
    {
        Asteria::User::Libc::SetError(EIO);
        return false;
    }
    for (size_t repeat{0U}; repeat < repetitions; ++repeat)
    {
        if (count > sizeof(stream.Bytes) - stream.Used)
        {
            return false;
        }
        std::memcpy(stream.Bytes + stream.Used, bytes, count);
        stream.Used += count;
    }
    return true;
}
[[nodiscard]] int StreamFormat(StreamBuffer& stream, const char* const format, ...) noexcept
{
    const Asteria::User::Libc::FormatSink sink{.Context = &stream, .Write = WriteStream};
    va_list arguments;
    va_start(arguments, format);
    const int result = Asteria::User::Libc::Format(sink, format, arguments);
    va_end(arguments);
    return result;
}

void Miscellaneous(TestContext& context) noexcept
{
    StreamBuffer stream{.Bytes = {}, .Used = 0U, .Calls = 0U, .Fail = false};
    ASTERIA_EXPECT(context, "callback sink receives the same formatted bytes",
                   StreamFormat(stream, "%05d/%.2f/%s", 7, 1.25, "ok") == 13, stream.Used == 13U,
                   std::memcmp(stream.Bytes, "00007/1.25/ok", 13U) == 0);
    stream.Fail = true;
    ASTERIA_EXPECT(context, "callback failure propagates its error and stops formatting",
                   StreamFormat(stream, "%d", 123) == -1, ASTERIA_LIBC_ERRNO == EIO);
    char text[128U]{};
    const char* const stringFormat{"%*.*s %c %% %p %p"};
    static_cast<void>(Compare(context, 128U, stringFormat, -12, 5, "abcdef", 0, static_cast<void*>(text),
                              static_cast<void*>(nullptr)));
    static_cast<void>(Compare(context, 128U, "%lc %12.3ls %ls", static_cast<unsigned int>('Z'), L"abcdef", L""));
    signed char tiny{0};
    short small{0};
    int normal{0};
    long large{0};
    long long largest{0};
    intmax_t maximum{0};
    ptrdiff_t size{0};
    ptrdiff_t difference{0};
    const int result = asteria_libc_snprintf(text, 4U, "a%hhnb%hnc%nd%lne%llnf%jng%znh%tn", &tiny, &small, &normal,
                                             &large, &largest, &maximum, &size, &difference);
    ASTERIA_EXPECT(context, "all count lengths include discarded output", result == 8, tiny == 1, small == 2,
                   normal == 3, large == 4, largest == 5, maximum == 6, size == 7, difference == 8,
                   std::strcmp(text, "abc") == 0);
    ASTERIA_EXPECT(context, "sprintf uses shared engine", asteria_libc_sprintf(text, "%s:%d", "x", 7) == 3,
                   std::strcmp(text, "x:7") == 0);
    ASTERIA_EXPECT(context, "zero capacity may use null", asteria_libc_snprintf(nullptr, 0U, "%d", 123) == 3);
    ASTERIA_EXPECT(context, "huge precision computes length without iterating over discarded zeroes",
                   asteria_libc_snprintf(text, sizeof(text), "%.*f", INT32_MAX - 2, 1.0) == INT32_MAX);
    ASTERIA_EXPECT(context, "negative INT_MIN width cannot be represented in a C return value",
                   asteria_libc_snprintf(text, sizeof(text), "%*d", INT_MIN, 1) == -1, ASTERIA_LIBC_ERRNO == EOVERFLOW);
    ASTERIA_EXPECT(context, "alternate general conversion checks precision arithmetic",
                   asteria_libc_snprintf(text, sizeof(text), "%#.*g", INT_MAX, 0.001) == -1,
                   ASTERIA_LIBC_ERRNO == EOVERFLOW);
    ASTERIA_EXPECT(context, "literal width overflow",
                   asteria_libc_snprintf(text, sizeof(text), "%2147483648d", 1) == -1, ASTERIA_LIBC_ERRNO == EOVERFLOW);
    ASTERIA_EXPECT(context, "C locale wide encoding rejects non-ASCII",
                   asteria_libc_snprintf(text, sizeof(text), "%lc", 128U) == -1, ASTERIA_LIBC_ERRNO == EILSEQ);
    ASTERIA_EXPECT(context, "return overflow reports EOVERFLOW",
                   asteria_libc_snprintf(text, sizeof(text), "%.*f", INT32_MAX, 1.0) == -1,
                   ASTERIA_LIBC_ERRNO == EOVERFLOW);
}
} // namespace
} // namespace Asteria::Test::Libc

int main()
{
    Asteria::Test::Libc::TestContext context{"LibcFormatter"};
    Asteria::Test::Libc::Random random;
    Asteria::Test::Libc::Integers(context, random);
    Asteria::Test::Libc::FloatingPoint(context, random);
    Asteria::Test::Libc::Miscellaneous(context);
    return context.Finish();
}
