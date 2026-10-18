// SPDX-License-Identifier: Apache-2.0
#include "User/Libc/Include/stdio.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
[[nodiscard]] std::uint64_t Bits(const std::uint8_t* const data, const size_t size) noexcept
{
    std::uint64_t bits{0U};
    for (size_t index{0U}; index < size && index < 8U; ++index)
    {
        bits |= static_cast<std::uint64_t>(data[index]) << static_cast<unsigned int>(index * 8U);
    }
    return bits;
}
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
    if (size == 0U)
    {
        return 0;
    }
    const auto bits = Bits(data, size);
    char format[64U]{};
    const char conversion = "fFeEgGaAdiuoxXcs"[data[0U] % 16U];
    static_cast<void>(std::snprintf(format, sizeof(format), "%%%s%s%s%s%s*.*%c!", (data[0U] & 1U) != 0U ? "-" : "",
                                    (data[0U] & 2U) != 0U ? "+" : "", (data[0U] & 4U) != 0U ? " " : "",
                                    (data[0U] & 8U) != 0U ? "#" : "", (data[0U] & 16U) != 0U ? "0" : "", conversion));
    const int width{size > 1U ? static_cast<int>(data[1U]) - 128 : 0};
    const int precision{size > 2U ? static_cast<int>(data[2U]) - 1 : -1};
    const size_t capacity{size > 3U ? data[3U] : 256U};
    char own[256U]{};
    char host[256U]{};
    int expected{0};
    int actual{0};
    if (std::strchr("fFeEgGaA", conversion) != nullptr)
    {
        const double value = std::bit_cast<double>(bits);
        expected = std::snprintf(host, capacity, format, width, precision, value);
        actual = asteria_libc_snprintf(own, capacity, format, width, precision, value);
    }
    else if (conversion == 's')
    {
        expected = std::snprintf(host, capacity, format, width, precision, "a string\n");
        actual = asteria_libc_snprintf(own, capacity, format, width, precision, "a string\n");
    }
    else if (conversion == 'd' || conversion == 'i' || conversion == 'c')
    {
        const auto value = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits & 0xFFFFFFFFU));
        expected = std::snprintf(host, capacity, format, width, precision, value);
        actual = asteria_libc_snprintf(own, capacity, format, width, precision, value);
    }
    else
    {
        const auto value = static_cast<unsigned int>(bits & 0xFFFFFFFFU);
        expected = std::snprintf(host, capacity, format, width, precision, value);
        actual = asteria_libc_snprintf(own, capacity, format, width, precision, value);
    }
    Require(actual == expected && std::memcmp(own, host, sizeof(own)) == 0);
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t Seeds[][8U]{
        {0U, 0U, 0U, 255U, 0U, 0U, 0U, 0U},
        {1U, 128U, 32U, 255U, 0U, 0U, 0U, 0x80U},
        {0xFFU, 255U, 255U, 1U, 0U, 0U, 0xF0U, 0x7FU},
        {4U, 17U, 4U, 10U, 0U, 0U, 0xF0U, 0x3FU},
    };
    for (const auto& seed : Seeds)
    {
        emit(seed, sizeof(seed));
    }
}
