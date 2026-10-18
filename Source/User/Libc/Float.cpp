// SPDX-License-Identifier: Apache-2.0

#include "Float.hpp"

#include <float.h>
#include <limits.h>

#include "Include/stdlib.h"
#include "Include/string.h"
#include "Include/ctype.h"
#include "Include/errno.h"
#include "LibcHooks.hpp"

namespace Asteria::User::Libc
{
static_assert(sizeof(float) == 4U && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128);
static_assert(sizeof(double) == 8U && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024);
static_assert(LDBL_MANT_DIG == 53 || sizeof(long double) == 16U);
namespace
{
// 8192 bits cover a 1200-digit coefficient and every relevant denominator.
// Discarded digits carry a sticky bit: all binary64 rounding midpoints have
// terminating decimal expansions with fewer than 1200 significant digits.
class Big final
{
public:
    void Set(uint64_t value) noexcept
    {
        m_length = 0U;
        do
        {
            m_words[m_length++] = static_cast<uint32_t>(value & 0xFFFFFFFFU);
            value >>= 32U;
        } while (value != 0U);
    }

    void Copy(const Big& other) noexcept
    {
        m_length = other.m_length;
        for (size_t index{0U}; index < m_length; ++index)
        {
            m_words[index] = other.m_words[index];
        }
    }

    void Multiply(const uint32_t factor, const uint32_t add = 0U) noexcept
    {
        uint64_t carry{add};
        for (size_t index{0U}; index < m_length; ++index)
        {
            carry += static_cast<uint64_t>(m_words[index]) * factor;
            m_words[index] = static_cast<uint32_t>(carry & 0xFFFFFFFFU);
            carry >>= 32U;
        }
        if (carry != 0U)
        {
            m_words[m_length++] = static_cast<uint32_t>(carry);
        }
    }

    void Shift(const unsigned int bits) noexcept
    {
        const size_t words{bits / 32U};
        const unsigned int remainder{bits % 32U};
        if (remainder != 0U)
        {
            uint32_t carry{0U};
            for (size_t index{0U}; index < m_length; ++index)
            {
                const uint64_t value = (static_cast<uint64_t>(m_words[index]) << remainder) | carry;
                m_words[index] = static_cast<uint32_t>(value & 0xFFFFFFFFU);
                carry = static_cast<uint32_t>(value >> 32U);
            }
            if (carry != 0U)
            {
                m_words[m_length++] = carry;
            }
        }
        for (size_t index{m_length}; index != 0U; --index)
        {
            m_words[index - 1U + words] = m_words[index - 1U];
        }
        for (size_t index{0U}; index < words; ++index)
        {
            m_words[index] = 0U;
        }
        m_length += words;
        Normalize();
    }

    [[nodiscard]] int Compare(const Big& other) const noexcept
    {
        if (m_length != other.m_length)
        {
            return m_length > other.m_length ? 1 : -1;
        }
        for (size_t index{m_length}; index != 0U; --index)
        {
            if (m_words[index - 1U] != other.m_words[index - 1U])
            {
                return m_words[index - 1U] > other.m_words[index - 1U] ? 1 : -1;
            }
        }
        return 0;
    }

    void Subtract(const Big& other) noexcept
    {
        uint64_t borrow{0U};
        for (size_t index{0U}; index < m_length; ++index)
        {
            const uint64_t right = (index < other.m_length ? other.m_words[index] : 0U) + borrow;
            const uint64_t left{m_words[index]};
            m_words[index] = static_cast<uint32_t>((left - right) & 0xFFFFFFFFU);
            borrow = left < right ? 1U : 0U;
        }
        Normalize();
    }

    [[nodiscard]] unsigned int Divide10() noexcept
    {
        uint64_t carry{0U};
        for (size_t index{m_length}; index != 0U; --index)
        {
            const uint64_t value = (carry << 32U) | m_words[index - 1U];
            m_words[index - 1U] = static_cast<uint32_t>(value / 10U);
            carry = value % 10U;
        }
        Normalize();
        return static_cast<unsigned int>(carry);
    }

    [[nodiscard]] int Bits() const noexcept
    {
        uint32_t top{m_words[m_length - 1U]};
        int result = static_cast<int>((m_length - 1U) * 32U);
        while (top != 0U)
        {
            ++result;
            top >>= 1U;
        }
        return result;
    }

private:
    void Normalize() noexcept
    {
        while (m_length > 1U && m_words[m_length - 1U] == 0U)
        {
            --m_length;
        }
    }
    uint32_t m_words[256U]{};
    // Set or Copy establishes the invariant before any arithmetic operation.
    size_t m_length{0U};
};

struct Workspace
{
    Big Numerator;
    Big Denominator;
    Big Temporary;
    char Digits[1201U];
};
Workspace g_workspace{};

[[nodiscard]] bool Match(const char* text, const char* pattern) noexcept
{
    while (*pattern != '\0')
    {
        if (ASTERIA_LIBC_NAME(tolower)(static_cast<unsigned char>(*text)) != *pattern)
        {
            return false;
        }
        ++text;
        ++pattern;
    }
    return true;
}

[[nodiscard]] unsigned int Digit(const char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'a' && value <= 'f')
    {
        return static_cast<unsigned int>(value - 'a') + 10U;
    }
    if (value >= 'A' && value <= 'F')
    {
        return static_cast<unsigned int>(value - 'A') + 10U;
    }
    return 16U;
}

[[nodiscard]] int64_t Exponent(const char*& cursor, const char marker) noexcept
{
    if (ASTERIA_LIBC_NAME(tolower)(static_cast<unsigned char>(*cursor)) != marker)
    {
        return 0;
    }
    const char* next{cursor + 1U};
    const bool negative{*next == '-'};
    if (*next == '-' || *next == '+')
    {
        ++next;
    }
    if (Digit(*next) >= 10U)
    {
        return 0;
    }
    int64_t result{0};
    while (Digit(*next) < 10U)
    {
        if (result < 100000000000000000LL)
        {
            result = result * 10 + Digit(*next);
        }
        ++next;
    }
    cursor = next;
    return negative ? -result : result;
}

[[nodiscard]] uint64_t NanPayload(const char* start, const char* const end) noexcept
{
    unsigned int base{10U};
    if (start != end && *start == '0')
    {
        base = 8U;
        if (end - start >= 2 && (start[1U] == 'x' || start[1U] == 'X'))
        {
            base = 16U;
            start += 2U;
        }
    }
    uint64_t value{0U};
    bool overflow{false};
    for (const char* cursor{start}; cursor != end; ++cursor)
    {
        const unsigned int digit = Digit(*cursor);
        if (digit >= base)
        {
            return 0U;
        }
        if (value > (UINT64_MAX - digit) / base)
        {
            overflow = true;
        }
        value = value * base + digit; // Intentional modulo arithmetic after overflow detection.
    }
    return overflow ? UINT64_MAX : value;
}

[[nodiscard]] uint64_t RoundRatio(const bool single, const bool sticky) noexcept
{
    auto& numerator = g_workspace.Numerator;
    auto& denominator = g_workspace.Denominator;
    auto& temporary = g_workspace.Temporary;
    const int precision{single ? 24 : 53};
    const int minimum{single ? -126 : -1022};
    const int maximum{single ? 127 : 1023};
    const int bias{single ? 127 : 1023};
    const uint64_t infinity{single ? 0x7F800000U : 0x7FF0000000000000U};
    int exponent{numerator.Bits() - denominator.Bits()};
    if (exponent >= 0)
    {
        temporary.Copy(denominator);
        temporary.Shift(static_cast<unsigned int>(exponent));
        if (numerator.Compare(temporary) < 0)
        {
            --exponent;
        }
    }
    else
    {
        temporary.Copy(numerator);
        temporary.Shift(static_cast<unsigned int>(-exponent));
        if (temporary.Compare(denominator) < 0)
        {
            --exponent;
        }
    }
    if (exponent > maximum)
    {
        SetError(ERANGE);
        return infinity;
    }
    const int quantum = (exponent < minimum ? minimum : exponent) - (precision - 1);
    if (quantum >= 0)
    {
        denominator.Shift(static_cast<unsigned int>(quantum));
    }
    else
    {
        numerator.Shift(static_cast<unsigned int>(-quantum));
    }
    uint64_t quotient{0U};
    for (int bit{numerator.Bits() - denominator.Bits()}; bit >= 0; --bit)
    {
        temporary.Copy(denominator);
        temporary.Shift(static_cast<unsigned int>(bit));
        if (numerator.Compare(temporary) >= 0)
        {
            numerator.Subtract(temporary);
            quotient |= uint64_t{1U} << static_cast<unsigned int>(bit);
        }
    }
    const bool inexact = numerator.Bits() != 0 || sticky;
    numerator.Shift(1U);
    const int rounding = numerator.Compare(denominator);
    const uint64_t hidden = uint64_t{1U} << static_cast<unsigned int>(precision - 1);
    bool tiny{exponent < minimum};
    if (tiny && quotient == hidden - 1U)
    {
        // Tininess after rounding to p significant bits with an unbounded exponent:
        // the normal-boundary threshold is minimum_normal minus a quarter subnormal ULP.
        numerator.Shift(1U);
        temporary.Copy(denominator);
        temporary.Multiply(3U);
        tiny = numerator.Compare(temporary) < 0;
    }
    if (rounding > 0 || (rounding == 0 && (sticky || (quotient & 1U) != 0U)))
    {
        ++quotient;
    }
    if (exponent < minimum)
    {
        if (inexact && tiny)
        {
            SetError(ERANGE);
        }
        return quotient;
    }
    if (quotient == hidden * 2U)
    {
        quotient >>= 1U;
        ++exponent;
    }
    if (exponent > maximum)
    {
        SetError(ERANGE);
        return infinity;
    }
    return (static_cast<uint64_t>(exponent + bias) << static_cast<unsigned int>(precision - 1)) | (quotient - hidden);
}

void RoundDecimal(size_t& length, int& point, const int64_t cut) noexcept
{
    char* const digits = g_workspace.Digits;
    if (cut >= static_cast<int64_t>(length))
    {
        return;
    }
    if (cut < 0)
    {
        digits[0U] = '0';
        length = 1U;
        point = 1;
        return;
    }
    const auto position = static_cast<size_t>(cut);
    bool more{false};
    for (size_t index{position + 1U}; index < length; ++index)
    {
        more = more || digits[index] != '0';
    }
    const bool odd{position != 0U && (digits[position - 1U] - '0') % 2 != 0};
    const bool up = digits[position] > '5' || (digits[position] == '5' && (more || odd));
    length = position;
    if (up)
    {
        size_t cursor{position};
        while (cursor != 0U && digits[cursor - 1U] == '9')
        {
            digits[--cursor] = '0';
        }
        if (cursor != 0U)
        {
            ++digits[cursor - 1U];
        }
        else
        {
            digits[0U] = '1';
            ++point;
            if (length == 0U)
            {
                length = 1U;
            }
        }
    }
    if (length == 0U)
    {
        digits[0U] = '0';
        length = 1U;
        point = 1;
    }
}
} // namespace

uint64_t DoubleBits(const double value) noexcept
{
    uint64_t result{0U};
    static_assert(sizeof(value) == sizeof(result));
    static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(&result, &value, sizeof(result)));
    return result;
}

uint64_t LongDoubleBits(const long double& value) noexcept
{
    uint64_t low{0U};
    uint64_t high{0U};
    static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(&low, &value, sizeof(low)));
    static_assert(LDBL_MANT_DIG == 64 || LDBL_MANT_DIG == 113 || LDBL_MANT_DIG == 53);
    if constexpr (LDBL_MANT_DIG == 53)
    {
        return low;
    }
    // ABI adapter: both supported targets use little-endian IEEE representations;
    // AMD64 has an explicit integer bit, IA-64 uses IEEE binary128.
    static_cast<void>(
        ASTERIA_LIBC_NAME(memcpy)(&high, reinterpret_cast<const unsigned char*>(&value) + 8U, sizeof(high)));
    uint64_t sign{0U};
    unsigned int exponent{0U};
    bool nonzero{false};
    int power{0};
    if constexpr (LDBL_MANT_DIG == 64)
    {
        sign = (high & 0x8000U) << 48U;
        exponent = static_cast<unsigned int>(high & 0x7FFFU);
        nonzero = (low & 0x7FFFFFFFFFFFFFFFU) != 0U;
        power = static_cast<int>(exponent == 0U ? 1U : exponent) - 16383 - 63;
        g_workspace.Numerator.Set(low);
    }
    else
    {
        sign = high & 0x8000000000000000U;
        exponent = static_cast<unsigned int>((high >> 48U) & 0x7FFFU);
        high &= 0xFFFFFFFFFFFFU;
        nonzero = high != 0U || low != 0U;
        if (exponent != 0U)
        {
            high |= 0x1000000000000U;
        }
        power = static_cast<int>(exponent == 0U ? 1U : exponent) - 16383 - 112;
        g_workspace.Numerator.Set(high);
        g_workspace.Numerator.Shift(32U);
        g_workspace.Numerator.Multiply(1U, static_cast<uint32_t>(low >> 32U));
        g_workspace.Numerator.Shift(32U);
        g_workspace.Numerator.Multiply(1U, static_cast<uint32_t>(low & 0xFFFFFFFFU));
    }
    if (exponent == 0x7FFFU)
    {
        return sign | (nonzero ? 0x7FF8000000000000U : 0x7FF0000000000000U);
    }
    if (g_workspace.Numerator.Bits() == 0)
    {
        return sign;
    }
    const int order = g_workspace.Numerator.Bits() + power;
    if (order > 1024)
    {
        return sign | 0x7FF0000000000000U;
    }
    if (order < -1075)
    {
        return sign;
    }
    g_workspace.Denominator.Set(1U);
    if (power >= 0)
    {
        g_workspace.Numerator.Shift(static_cast<unsigned int>(power));
    }
    else
    {
        g_workspace.Denominator.Shift(static_cast<unsigned int>(-power));
    }
    const int savedError = *ASTERIA_LIBC_NAME(__asteria_errno_location)();
    const uint64_t rounded = RoundRatio(false, false);
    SetError(savedError);
    return sign | rounded;
}

uint64_t ParseFloat(const char* const text, char** const end, const bool single) noexcept
{
    if (end != nullptr)
    {
        *end = const_cast<char*>(text); // External C borrow.
    }
    const char* cursor{text};
    while (ASTERIA_LIBC_NAME(isspace)(static_cast<unsigned char>(*cursor)))
    {
        ++cursor;
    }
    const bool negative{*cursor == '-'};
    if (*cursor == '-' || *cursor == '+')
    {
        ++cursor;
    }
    const uint64_t sign = negative ? (uint64_t{1U} << (single ? 31U : 63U)) : 0U;
    const uint64_t infinity{single ? 0x7F800000U : 0x7FF0000000000000U};
    if (Match(cursor, "inf"))
    {
        cursor += Match(cursor, "infinity") ? 8U : 3U;
        if (end != nullptr)
        {
            *end = const_cast<char*>(cursor);
        }
        return sign | infinity;
    }
    if (Match(cursor, "nan"))
    {
        uint64_t payload{0U};
        cursor += 3U;
        if (*cursor == '(')
        {
            const char* next{cursor + 1U};
            while (ASTERIA_LIBC_NAME(isalnum)(static_cast<unsigned char>(*next)) || *next == '_')
            {
                ++next;
            }
            if (*next == ')')
            {
                payload = NanPayload(cursor + 1U, next);
                cursor = next + 1U;
            }
        }
        if (end != nullptr)
        {
            *end = const_cast<char*>(cursor);
        }
        const uint64_t quiet{single ? 0x400000U : 0x8000000000000U};
        return sign | infinity | quiet | (payload & (quiet - 1U));
    }
    bool hex{false};
    if (cursor[0U] == '0' && (cursor[1U] == 'x' || cursor[1U] == 'X'))
    {
        const char* next{cursor + 2U};
        if (*next == '.')
        {
            ++next;
        }
        if (Digit(*next) < 16U)
        {
            hex = true;
            cursor += 2U;
        }
    }
    auto& numerator = g_workspace.Numerator;
    auto& denominator = g_workspace.Denominator;
    numerator.Set(0U);
    denominator.Set(1U);
    const unsigned int base{hex ? 16U : 10U};
    const size_t limit{hex ? 300U : 1200U};
    size_t stored{0U};
    int64_t discarded{0};
    int64_t fractional{0};
    bool dot{false};
    bool any{false};
    bool nonzero{false};
    bool sticky{false};
    while (true)
    {
        if (*cursor == '.' && !dot)
        {
            dot = true;
            ++cursor;
            continue;
        }
        const unsigned int digit = Digit(*cursor);
        if (digit >= base)
        {
            break;
        }
        any = true;
        if (dot)
        {
            ++fractional;
        }
        nonzero = nonzero || digit != 0U;
        if (nonzero)
        {
            if (stored < limit)
            {
                numerator.Multiply(base, digit);
                ++stored;
            }
            else
            {
                ++discarded;
                sticky = sticky || digit != 0U;
            }
        }
        ++cursor;
    }
    if (!any)
    {
        return 0U;
    }
    int64_t scale = Exponent(cursor, hex ? 'p' : 'e');
    scale += (discarded - fractional) * (hex ? 4 : 1);
    if (end != nullptr)
    {
        *end = const_cast<char*>(cursor);
    }
    if (!nonzero)
    {
        return sign;
    }
    const int64_t order = scale + (hex ? numerator.Bits() : static_cast<int64_t>(stored));
    if (order > (hex ? 1100 : 310))
    {
        SetError(ERANGE);
        return sign | infinity;
    }
    if (order < (hex ? -1200 : -350))
    {
        SetError(ERANGE);
        return sign;
    }
    if (hex)
    {
        if (scale >= 0)
        {
            numerator.Shift(static_cast<unsigned int>(scale));
        }
        else
        {
            denominator.Shift(static_cast<unsigned int>(-scale));
        }
    }
    else
    {
        while (scale > 0)
        {
            numerator.Multiply(10U);
            --scale;
        }
        while (scale < 0)
        {
            denominator.Multiply(10U);
            ++scale;
        }
    }
    return sign | RoundRatio(single, sticky);
}

DecimalView Decimal(const uint64_t bits, const char conversion, int precision, const bool alternate) noexcept
{
    auto& number = g_workspace.Numerator;
    uint64_t mantissa{bits & 0xFFFFFFFFFFFFFU};
    const auto binaryExponent = static_cast<int>((bits >> 52U) & 0x7FFU);
    int exponent{binaryExponent == 0 ? -1074 : binaryExponent - 1075};
    if (binaryExponent != 0)
    {
        mantissa |= 0x10000000000000U;
    }
    number.Set(mantissa);
    int scale{0};
    if (exponent >= 0)
    {
        number.Shift(static_cast<unsigned int>(exponent));
    }
    else
    {
        scale = exponent;
        while (exponent < 0)
        {
            number.Multiply(5U);
            ++exponent;
        }
    }
    char* const digits = g_workspace.Digits;
    size_t length{0U};
    do
    {
        digits[length++] = static_cast<char>('0' + number.Divide10());
    } while (number.Bits() != 0);
    for (size_t index{0U}; index < length / 2U; ++index)
    {
        const char saved = digits[index];
        digits[index] = digits[length - index - 1U];
        digits[length - index - 1U] = saved;
    }
    int point = static_cast<int>(length) + scale;
    if (mantissa == 0U)
    {
        point = 1;
    }
    if (precision < 0)
    {
        precision = 6;
    }
    bool scientific{conversion == 'e'};
    if (conversion == 'g')
    {
        if (precision == 0)
        {
            precision = 1;
        }
        RoundDecimal(length, point, precision);
        scientific = point - 1 < -4 || point - 1 >= precision;
        if (!alternate)
        {
            while (length > 1U && digits[length - 1U] == '0')
            {
                --length;
            }
        }
        const int significant = alternate ? precision : static_cast<int>(length);
        const int64_t fractional =
            scientific ? static_cast<int64_t>(significant) - 1 : static_cast<int64_t>(significant) - point;
        precision = fractional <= 0 ? 0 : static_cast<int>(fractional > INT_MAX ? INT_MAX : fractional);
    }
    else
    {
        const int64_t cut = scientific ? static_cast<int64_t>(precision) + 1 : static_cast<int64_t>(point) + precision;
        RoundDecimal(length, point, cut);
    }
    return DecimalView{
        .Digits = digits, .Length = length, .Point = point, .Precision = precision, .Scientific = scientific};
}
} // namespace Asteria::User::Libc

extern "C" double ASTERIA_LIBC_NAME(strtod)(const char* const text, char** const end) noexcept
{
    const auto& lock = Asteria::User::Libc::GetHooks().ConversionLock;
    Asteria::User::Libc::Lock(lock);
    const uint64_t bits = Asteria::User::Libc::ParseFloat(text, end, false);
    Asteria::User::Libc::Unlock(lock);
    double value{0.0};
    static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(&value, &bits, sizeof(value)));
    return value;
}

extern "C" float ASTERIA_LIBC_NAME(strtof)(const char* const text, char** const end) noexcept
{
    const auto& lock = Asteria::User::Libc::GetHooks().ConversionLock;
    Asteria::User::Libc::Lock(lock);
    const auto bits = static_cast<uint32_t>(Asteria::User::Libc::ParseFloat(text, end, true));
    Asteria::User::Libc::Unlock(lock);
    float value{0.0F};
    static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(&value, &bits, sizeof(value)));
    return value;
}
