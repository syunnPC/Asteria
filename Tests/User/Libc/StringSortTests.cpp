// SPDX-License-Identifier: Apache-2.0
#include "LibcTestSupport.hpp"

#include <cstring>
#include <strings.h>
#include <cctype>

#include "User/Libc/Include/string.h"
#include "User/Libc/Include/strings.h"
#include "User/Libc/Include/ctype.h"
#include "User/Libc/Include/stdlib.h"

namespace Asteria::Test::Libc
{
namespace
{
char* (*volatile g_hostCopy)(char*, const char*, size_t) = ::strncpy;
char* (*volatile g_hostAppend)(char*, const char*, size_t) = ::strncat;

[[nodiscard]] int Order(const int value) noexcept
{
    return (value > 0) - (value < 0);
}
[[nodiscard]] int CompareInt(const void* const left, const void* const right) noexcept
{
    const int a{*static_cast<const int*>(left)};
    const int b{*static_cast<const int*>(right)};
    return (a > b) - (a < b);
}
void Strings(TestContext& context, Random& random) noexcept
{
    for (size_t step{0U}; step < 6000U; ++step)
    {
        char first[160U]{};
        char second[160U]{};
        char expected[512U]{};
        char actual[512U]{};
        const size_t a{static_cast<size_t>(random.Next() % 150U)};
        const size_t b{static_cast<size_t>(random.Next() % 150U)};
        for (size_t index{0U}; index < a; ++index)
        {
            first[index] = static_cast<char>((random.Next() % 255U) + 1U);
        }
        for (size_t index{0U}; index < b; ++index)
        {
            second[index] = static_cast<char>((random.Next() % 255U) + 1U);
        }
        const size_t count{static_cast<size_t>(random.Next() % 160U)};
        const int character{static_cast<int>(random.Next() % 300U)};
        ASTERIA_EXPECT(context, "string lengths and comparisons", asteria_libc_strlen(first) == std::strlen(first),
                       asteria_libc_strnlen(first, count) == ::strnlen(first, count),
                       Order(asteria_libc_strcmp(first, second)) == Order(std::strcmp(first, second)),
                       Order(asteria_libc_strncmp(first, second, count)) == Order(std::strncmp(first, second, count)),
                       Order(asteria_libc_strcasecmp(first, second)) == Order(::strcasecmp(first, second)),
                       Order(asteria_libc_strncasecmp(first, second, count)) ==
                           Order(::strncasecmp(first, second, count)),
                       Order(asteria_libc_strcoll(first, second)) == Order(std::strcoll(first, second)));
        ASTERIA_EXPECT(context, "searches return matching borrows",
                       asteria_libc_strchr(first, character) == std::strchr(first, character),
                       asteria_libc_strrchr(first, character) == std::strrchr(first, character),
                       asteria_libc_strspn(first, second) == std::strspn(first, second),
                       asteria_libc_strcspn(first, second) == std::strcspn(first, second),
                       asteria_libc_strpbrk(first, second) == std::strpbrk(first, second),
                       asteria_libc_strstr(first, second) == std::strstr(first, second),
                       asteria_libc_memchr(first, character, count) == std::memchr(first, character, count),
                       Order(asteria_libc_memcmp(first, second, count)) == Order(std::memcmp(first, second, count)));
        static_cast<void>(g_hostCopy(expected, first, count));
        static_cast<void>(asteria_libc_strncpy(actual, first, count));
        ASTERIA_EXPECT(context, "strncpy includes padding", std::memcmp(actual, expected, sizeof(actual)) == 0);
        static_cast<void>(std::strcpy(expected, first));
        static_cast<void>(asteria_libc_strcpy(actual, first));
        static_cast<void>(g_hostAppend(expected, second, count));
        static_cast<void>(asteria_libc_strncat(actual, second, count));
        ASTERIA_EXPECT(context, "strncat terminates after bounded append", std::strcmp(actual, expected) == 0);
        static_cast<void>(std::strcat(expected, first));
        static_cast<void>(asteria_libc_strcat(actual, first));
        ASTERIA_EXPECT(context, "strcat appends", std::strcmp(actual, expected) == 0);
        const size_t source{static_cast<size_t>(random.Next() % 256U)};
        const size_t destination{static_cast<size_t>(random.Next() % 256U)};
        const size_t bytes{static_cast<size_t>(random.Next() % 256U)};
        static_cast<void>(std::memmove(expected + destination, expected + source, bytes));
        static_cast<void>(asteria_libc_memmove(actual + destination, actual + source, bytes));
        ASTERIA_EXPECT(context, "overlapping memmove", std::memcmp(actual, expected, sizeof(actual)) == 0);
        static_cast<void>(std::memset(expected, character, count));
        static_cast<void>(asteria_libc_memset(actual, character, count));
        static_cast<void>(std::memcpy(expected + 256U, first, count));
        static_cast<void>(asteria_libc_memcpy(actual + 256U, first, count));
        ASTERIA_EXPECT(context, "byte copies and fills", std::memcmp(actual, expected, sizeof(actual)) == 0);
        ASTERIA_EXPECT(context, "C locale transform length",
                       asteria_libc_strxfrm(actual, first, sizeof(actual)) ==
                           std::strxfrm(expected, first, sizeof(expected)),
                       std::strcmp(actual, expected) == 0);
    }
    char host[] = ";a::bc,d;;ef";
    char own[] = ";a::bc,d;;ef";
    char* hostState{nullptr};
    char* ownState{nullptr};
    char* a{::strtok_r(host, ";:,", &hostState)};
    char* b{asteria_libc_strtok_r(own, ";:,", &ownState)};
    while (a != nullptr || b != nullptr)
    {
        if (!ASTERIA_EXPECT(context, "tokenization state model", a != nullptr, b != nullptr, std::strcmp(a, b) == 0,
                            hostState - host == ownState - own))
        {
            break;
        }
        a = ::strtok_r(nullptr, ";:,", &hostState);
        b = asteria_libc_strtok_r(nullptr, ";:,", &ownState);
    }
    ASTERIA_EXPECT(context, "empty needle and zero-character searches", asteria_libc_strstr(own, "") == own,
                   asteria_libc_strchr(own, 0) == own + std::strlen(own),
                   asteria_libc_strrchr(own, 0) == own + std::strlen(own));
    char token[] = " a b ";
    ASTERIA_EXPECT(context, "strtok shared state", std::strcmp(asteria_libc_strtok(token, " "), "a") == 0,
                   std::strcmp(asteria_libc_strtok(nullptr, " "), "b") == 0,
                   asteria_libc_strtok(nullptr, " ") == nullptr);
    for (int error{0}; error <= 133; ++error)
    {
        ASTERIA_EXPECT(context, "Asteria error messages in C locale",
                       std::strcmp(asteria_libc_strerror(error), std::strerror(error)) == 0);
    }
    ASTERIA_EXPECT(context, "unknown error messages",
                   std::strcmp(asteria_libc_strerror(-2147483647 - 1), std::strerror(-2147483647 - 1)) == 0);
}

void Sorting(TestContext& context, Random& random) noexcept
{
    for (size_t step{0U}; step < 2000U; ++step)
    {
        int actual[256U]{};
        int expected[256U]{};
        const size_t count{static_cast<size_t>(random.Next() % 256U)};
        for (size_t index{0U}; index < count; ++index)
        {
            actual[index] = step % 3U == 0U
                                ? static_cast<int>(index)
                                : (step % 3U == 1U ? -static_cast<int>(index) : static_cast<int>(random.Next() % 16U));
            expected[index] = actual[index];
        }
        std::qsort(expected, count, sizeof(int), CompareInt);
        asteria_libc_qsort(actual, count, sizeof(int), CompareInt);
        ASTERIA_EXPECT(context, "heapsort agrees for ordered, reverse and duplicate-heavy arrays",
                       std::memcmp(actual, expected, sizeof(actual)) == 0);
        const int key{static_cast<int>(random.Next() % 300U) - 10};
        const auto* host = static_cast<const int*>(std::bsearch(&key, expected, count, sizeof(int), CompareInt));
        const auto* own = static_cast<const int*>(asteria_libc_bsearch(&key, actual, count, sizeof(int), CompareInt));
        ASTERIA_EXPECT(context, "binary search result agrees", (host == nullptr) == (own == nullptr),
                       own == nullptr || *own == key);
    }
}

void Characters(TestContext& context) noexcept
{
    for (int value{-1}; value <= 255; ++value)
    {
        ASTERIA_EXPECT(
            context, "all C locale ctype inputs", (asteria_libc_isalnum(value) != 0) == (std::isalnum(value) != 0),
            (asteria_libc_isalpha(value) != 0) == (std::isalpha(value) != 0),
            (asteria_libc_isblank(value) != 0) == (std::isblank(value) != 0),
            (asteria_libc_iscntrl(value) != 0) == (std::iscntrl(value) != 0),
            (asteria_libc_isdigit(value) != 0) == (std::isdigit(value) != 0),
            (asteria_libc_isgraph(value) != 0) == (std::isgraph(value) != 0),
            (asteria_libc_islower(value) != 0) == (std::islower(value) != 0),
            (asteria_libc_isprint(value) != 0) == (std::isprint(value) != 0),
            (asteria_libc_ispunct(value) != 0) == (std::ispunct(value) != 0),
            (asteria_libc_isspace(value) != 0) == (std::isspace(value) != 0),
            (asteria_libc_isupper(value) != 0) == (std::isupper(value) != 0),
            (asteria_libc_isxdigit(value) != 0) == (std::isxdigit(value) != 0),
            asteria_libc_tolower(value) == std::tolower(value), asteria_libc_toupper(value) == std::toupper(value));
    }
}
} // namespace
} // namespace Asteria::Test::Libc

int main()
{
    Asteria::Test::Libc::TestContext context{"LibcStringSort"};
    Asteria::Test::Libc::Random random;
    Asteria::Test::Libc::Strings(context, random);
    Asteria::Test::Libc::Sorting(context, random);
    Asteria::Test::Libc::Characters(context);
    return context.Finish();
}
