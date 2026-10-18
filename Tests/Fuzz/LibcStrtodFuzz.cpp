// SPDX-License-Identifier: Apache-2.0
#include "User/Libc/Include/stdlib.h"

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "User/Libc/Include/stdio.h"
#include "User/Libc/Include/errno.h"

namespace
{
char g_input[4097U]{};
void Require(const bool condition) noexcept
{
    if (!condition)
    {
        std::abort();
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    const size_t length{size < sizeof(g_input) - 1U ? size : sizeof(g_input) - 1U};
    std::memcpy(g_input, data, length);
    g_input[length] = '\0';
    char* hostEnd{nullptr};
    char* ownEnd{nullptr};
    errno = 0;
    const double host = std::strtod(g_input, &hostEnd);
    const int error{errno};
    ASTERIA_LIBC_ERRNO = 0;
    const double own = asteria_libc_strtod(g_input, &ownEnd);
    Require(std::bit_cast<std::uint64_t>(host) == std::bit_cast<std::uint64_t>(own));
    Require(hostEnd == ownEnd && error == ASTERIA_LIBC_ERRNO);
    errno = 0;
    const float hostFloat = std::strtof(g_input, &hostEnd);
    const int floatError{errno};
    ASTERIA_LIBC_ERRNO = 0;
    const float ownFloat = asteria_libc_strtof(g_input, &ownEnd);
    Require(std::bit_cast<std::uint32_t>(hostFloat) == std::bit_cast<std::uint32_t>(ownFloat));
    Require(hostEnd == ownEnd && floatError == ASTERIA_LIBC_ERRNO);
    if (size >= 8U)
    {
        std::uint64_t bits{0U};
        std::memcpy(&bits, data, sizeof(bits));
        if (((bits >> 52U) & 0x7FFU) != 0x7FFU)
        {
            const double value = std::bit_cast<double>(bits);
            char decimal[32U]{};
            Require(asteria_libc_snprintf(decimal, sizeof(decimal), "%.17g", value) > 0);
            const double roundTrip = asteria_libc_strtod(decimal, &ownEnd);
            Require(*ownEnd == '\0' && std::bit_cast<std::uint64_t>(roundTrip) == bits);
        }
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr const char* Seeds[]{
        "0x1p-1075",    "2.2250738585072012e-308", "1e10000",
        "-nan(0x1234)", "0x1.fffffffffffff8p1023", "1.00000000000000011102230246251565404236316680908203125",
        "0x.p1"};
    for (const char* const seed : Seeds)
    {
        emit(reinterpret_cast<const std::uint8_t*>(seed), std::strlen(seed));
    }
}
