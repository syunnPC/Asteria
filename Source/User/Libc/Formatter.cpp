// SPDX-License-Identifier: Apache-2.0

#include "Include/stdio.h"

#include <limits.h>
#include <stdint.h>

#include "LibcHooks.hpp"
#include "Float.hpp"
#include "Include/ctype.h"
#include "Include/errno.h"
#include "Include/string.h"

namespace Asteria::User::Libc
{
namespace
{
enum class Length : unsigned char
{
    Default,
    Char,
    Short,
    Long,
    LongLong,
    Maximum,
    Size,
    Difference,
    Extended,
};
struct Specification
{
    bool Left;
    bool Plus;
    bool Space;
    bool Alternate;
    bool Zero;
    int Width;
    int Precision;
    Length Modifier;
    char Conversion;
    bool Overflow;
};

class Output final
{
public:
    explicit Output(const FormatSink& sink) noexcept : m_sink{sink}
    {
    }

    void Write(const char* const data, const size_t size, const size_t repetitions = 1U) noexcept
    {
        if (m_failed || size == 0U || repetitions == 0U)
        {
            return;
        }
        if (repetitions > static_cast<size_t>(INT_MAX) / size ||
            size * repetitions > static_cast<size_t>(INT_MAX) - m_count)
        {
            SetError(EOVERFLOW);
            m_failed = true;
            return;
        }
        m_count += size * repetitions;
        if (m_sink.Write != nullptr && !m_sink.Write(m_sink.Context, data, size, repetitions))
        {
            m_failed = true;
        }
    }

    void Character(const char value, const size_t count = 1U) noexcept
    {
        Write(&value, 1U, count);
    }

    [[nodiscard]] size_t Count() const noexcept
    {
        return m_count;
    }

    [[nodiscard]] bool Failed() const noexcept
    {
        return m_failed;
    }

    void Fail(const int error) noexcept
    {
        SetError(error);
        m_failed = true;
    }

private:
    FormatSink m_sink;
    size_t m_count{0U};
    bool m_failed{false};
};

[[nodiscard]] size_t Padding(const Specification& spec, const size_t length) noexcept
{
    return static_cast<size_t>(spec.Width) > length ? static_cast<size_t>(spec.Width) - length : 0U;
}

[[nodiscard]] size_t UnsignedText(uintmax_t number, char* const buffer, const unsigned int base,
                                  const bool upper) noexcept
{
    const char* const alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t length{0U};
    do
    {
        buffer[length++] = alphabet[number % base];
        number /= base;
    } while (number != 0U);
    for (size_t index{0U}; index < length / 2U; ++index)
    {
        const char saved = buffer[index];
        buffer[index] = buffer[length - index - 1U];
        buffer[length - index - 1U] = saved;
    }
    return length;
}

void Text(Output& output, const Specification& spec, const char* const data, const size_t length) noexcept
{
    if (!spec.Left)
    {
        output.Character(' ', Padding(spec, length));
    }
    output.Write(data, length);
    if (spec.Left)
    {
        output.Character(' ', Padding(spec, length));
    }
}

[[nodiscard]] char Sign(const Specification& spec, const bool negative) noexcept
{
    if (negative)
    {
        return '-';
    }
    if (spec.Plus)
    {
        return '+';
    }
    return spec.Space ? ' ' : '\0';
}

void Integer(Output& output, const Specification& spec, const uintmax_t number, const bool negative,
             const unsigned int base, const bool signedValue, const bool pointer) noexcept
{
    char digits[65U]{};
    size_t length = UnsignedText(number, digits, base, spec.Conversion == 'X');
    if (number == 0U && spec.Precision == 0)
    {
        length = 0U;
    }
    const char sign = signedValue || pointer ? Sign(spec, negative) : '\0';
    const bool hexadecimalPrefix{(spec.Alternate && number != 0U && base == 16U) || pointer};
    size_t zeroes{0U};
    if (spec.Precision >= 0 && static_cast<size_t>(spec.Precision) > length)
    {
        zeroes = static_cast<size_t>(spec.Precision) - length;
    }
    if (spec.Alternate && base == 8U && zeroes == 0U && (number != 0U || length == 0U))
    {
        zeroes = 1U;
    }
    const size_t total{length + zeroes + (sign != '\0' ? 1U : 0U) + (hexadecimalPrefix ? 2U : 0U)};
    const size_t padding = Padding(spec, total);
    const bool zeroPadding{spec.Zero && !spec.Left && spec.Precision < 0};
    if (!spec.Left && !zeroPadding)
    {
        output.Character(' ', padding);
    }
    if (sign != '\0')
    {
        output.Character(sign);
    }
    if (hexadecimalPrefix)
    {
        output.Write(spec.Conversion == 'X' ? "0X" : "0x", 2U);
    }
    output.Character('0', zeroes + (zeroPadding ? padding : 0U));
    output.Write(digits, length);
    if (spec.Left)
    {
        output.Character(' ', padding);
    }
}

void ExponentText(Output& output, const int exponent, const char marker, const bool decimal) noexcept
{
    char digits[12U]{};
    const auto magnitude = static_cast<unsigned int>(exponent < 0 ? -exponent : exponent);
    const size_t length = UnsignedText(magnitude, digits, 10U, false);
    output.Character(marker);
    output.Character(exponent < 0 ? '-' : '+');
    if (decimal && length < 2U)
    {
        output.Character('0');
    }
    output.Write(digits, length);
}

void DecimalRange(Output& output, const DecimalView& view, int64_t start, size_t count) noexcept
{
    if (start < 0)
    {
        size_t zeroes = static_cast<size_t>(-start);
        if (zeroes > count)
        {
            zeroes = count;
        }
        output.Character('0', zeroes);
        count -= zeroes;
        start += static_cast<int64_t>(zeroes);
    }
    if (count != 0U && start < static_cast<int64_t>(view.Length))
    {
        size_t available{view.Length - static_cast<size_t>(start)};
        if (available > count)
        {
            available = count;
        }
        output.Write(view.Digits + start, available);
        count -= available;
    }
    output.Character('0', count);
}

void DecimalBody(Output& output, const DecimalView& view, const bool alternate, const bool upper) noexcept
{
    if (view.Scientific)
    {
        DecimalRange(output, view, 0, 1U);
    }
    else if (view.Point <= 0)
    {
        output.Character('0');
    }
    else
    {
        DecimalRange(output, view, 0, static_cast<size_t>(view.Point));
    }
    if (view.Precision != 0 || alternate)
    {
        output.Character('.');
    }
    DecimalRange(output, view, view.Scientific ? 1 : view.Point, static_cast<size_t>(view.Precision));
    if (view.Scientific)
    {
        ExponentText(output, view.Point - 1, upper ? 'E' : 'e', true);
    }
}

struct HexView
{
    uint64_t Mantissa;
    int Exponent;
    int Precision;
};

[[nodiscard]] HexView Hexadecimal(const uint64_t bits, int precision) noexcept
{
    uint64_t mantissa{bits & 0xFFFFFFFFFFFFFU};
    const auto exponent = static_cast<int>((bits >> 52U) & 0x7FFU);
    int power{0};
    if (exponent != 0)
    {
        mantissa |= 0x10000000000000U;
        power = exponent - 1023;
    }
    else if (mantissa != 0U)
    {
        power = -1022;
    }
    if (precision < 0)
    {
        precision = 13;
        uint64_t tail{mantissa};
        while (precision > 0 && (tail & 15U) == 0U)
        {
            --precision;
            tail >>= 4U;
        }
    }
    else if (precision < 13)
    {
        const auto shift = static_cast<unsigned int>(52 - precision * 4);
        const uint64_t mask{(uint64_t{1U} << shift) - 1U};
        const uint64_t halfway{uint64_t{1U} << (shift - 1U)};
        uint64_t retained{mantissa >> shift};
        const uint64_t remainder{mantissa & mask};
        if (remainder > halfway || (remainder == halfway && (retained & 1U) != 0U))
        {
            ++retained;
        }
        mantissa = retained << shift;
    }
    return HexView{.Mantissa = mantissa, .Exponent = power, .Precision = precision};
}

void HexBody(Output& output, const HexView& view, const bool alternate, const bool upper) noexcept
{
    const char* const alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    output.Character(alphabet[view.Mantissa >> 52U]);
    if (view.Precision != 0 || alternate)
    {
        output.Character('.');
    }
    const int actual{view.Precision > 13 ? 13 : view.Precision};
    for (int index{0}; index < actual; ++index)
    {
        const auto shift = static_cast<unsigned int>(48 - index * 4);
        output.Character(alphabet[(view.Mantissa >> shift) & 15U]);
    }
    output.Character('0', static_cast<size_t>(view.Precision - actual));
    ExponentText(output, view.Exponent, upper ? 'P' : 'p', false);
}

void Floating(Output& output, const Specification& spec, const uint64_t bits) noexcept
{
    const char sign = Sign(spec, (bits >> 63U) != 0U);
    const bool upper = spec.Conversion >= 'A' && spec.Conversion <= 'Z';
    const char conversion = static_cast<char>(ASTERIA_LIBC_NAME(tolower)(spec.Conversion));
    const bool hexadecimal{conversion == 'a'};
    if (((bits >> 52U) & 0x7FFU) == 0x7FFU)
    {
        char text[4U]{};
        const bool nan{(bits & 0xFFFFFFFFFFFFFU) != 0U};
        const char* const word = nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        const size_t prefix{sign == '\0' ? 0U : 1U};
        if (prefix != 0U)
        {
            text[0U] = sign;
        }
        static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(text + prefix, word, 3U));
        Text(output, spec, text, prefix + 3U);
        return;
    }
    DecimalView decimal{};
    HexView hex{};
    if (hexadecimal)
    {
        hex = Hexadecimal(bits, spec.Precision);
    }
    else
    {
        decimal = Decimal(bits, conversion, spec.Precision, spec.Alternate);
    }
    const FormatSink measureSink{.Context = nullptr, .Write = nullptr};
    Output measure{measureSink};
    if (hexadecimal)
    {
        HexBody(measure, hex, spec.Alternate, upper);
    }
    else
    {
        DecimalBody(measure, decimal, spec.Alternate, upper);
    }
    if (measure.Failed())
    {
        output.Fail(EOVERFLOW);
        return;
    }
    const size_t total{measure.Count() + (sign != '\0' ? 1U : 0U) + (hexadecimal ? 2U : 0U)};
    const size_t padding = Padding(spec, total);
    if (!spec.Left && !spec.Zero)
    {
        output.Character(' ', padding);
    }
    if (sign != '\0')
    {
        output.Character(sign);
    }
    if (hexadecimal)
    {
        output.Write(upper ? "0X" : "0x", 2U);
    }
    if (!spec.Left && spec.Zero)
    {
        output.Character('0', padding);
    }
    if (hexadecimal)
    {
        HexBody(output, hex, spec.Alternate, upper);
    }
    else
    {
        DecimalBody(output, decimal, spec.Alternate, upper);
    }
    if (spec.Left)
    {
        output.Character(' ', padding);
    }
}

[[nodiscard]] int DecimalInteger(const char*& cursor, bool& overflow) noexcept
{
    int result{0};
    while (*cursor >= '0' && *cursor <= '9')
    {
        const int digit{*cursor - '0'};
        if (result > (INT_MAX - digit) / 10)
        {
            result = INT_MAX;
            overflow = true;
        }
        else
        {
            result = result * 10 + digit;
        }
        ++cursor;
    }
    return result;
}

[[nodiscard]] Specification ReadSpecification(const char*& cursor, va_list& arguments) noexcept
{
    Specification spec{.Left = false,
                       .Plus = false,
                       .Space = false,
                       .Alternate = false,
                       .Zero = false,
                       .Width = 0,
                       .Precision = -1,
                       .Modifier = Length::Default,
                       .Conversion = '\0',
                       .Overflow = false};
    while (true)
    {
        if (*cursor == '-')
        {
            spec.Left = true;
        }
        else if (*cursor == '+')
        {
            spec.Plus = true;
        }
        else if (*cursor == ' ')
        {
            spec.Space = true;
        }
        else if (*cursor == '#')
        {
            spec.Alternate = true;
        }
        else if (*cursor == '0')
        {
            spec.Zero = true;
        }
        else
        {
            break;
        }
        ++cursor;
    }
    if (*cursor == '*')
    {
        spec.Width = va_arg(arguments, int);
        ++cursor;
        if (spec.Width < 0)
        {
            spec.Left = true;
            spec.Overflow = spec.Width == INT_MIN;
            spec.Width = spec.Overflow ? INT_MAX : -spec.Width;
        }
    }
    else
    {
        spec.Width = DecimalInteger(cursor, spec.Overflow);
    }
    if (*cursor == '.')
    {
        ++cursor;
        if (*cursor == '*')
        {
            spec.Precision = va_arg(arguments, int);
            ++cursor;
        }
        else
        {
            spec.Precision = DecimalInteger(cursor, spec.Overflow);
        }
    }
    switch (*cursor)
    {
    case 'h':
        ++cursor;
        spec.Modifier = Length::Short;
        if (*cursor == 'h')
        {
            spec.Modifier = Length::Char;
            ++cursor;
        }
        break;
    case 'l':
        ++cursor;
        spec.Modifier = Length::Long;
        if (*cursor == 'l')
        {
            spec.Modifier = Length::LongLong;
            ++cursor;
        }
        break;
    case 'j':
        spec.Modifier = Length::Maximum;
        ++cursor;
        break;
    case 'z':
        spec.Modifier = Length::Size;
        ++cursor;
        break;
    case 't':
        spec.Modifier = Length::Difference;
        ++cursor;
        break;
    case 'L':
        spec.Modifier = Length::Extended;
        ++cursor;
        break;
    default:
        break;
    }
    spec.Conversion = *cursor;
    if (*cursor != '\0')
    {
        ++cursor;
    }
    return spec;
}

[[nodiscard]] intmax_t SignedArgument(va_list& arguments, const Length length) noexcept
{
    switch (length)
    {
    case Length::Char:
        return static_cast<signed char>(va_arg(arguments, int));
    case Length::Short:
        return static_cast<short>(va_arg(arguments, int));
    case Length::Long:
        return va_arg(arguments, long);
    case Length::LongLong:
        return va_arg(arguments, long long);
    case Length::Maximum:
        return va_arg(arguments, intmax_t);
    case Length::Size:
    case Length::Difference:
        return va_arg(arguments, ptrdiff_t);
    case Length::Default:
    case Length::Extended:
        return va_arg(arguments, int);
    }
    return 0;
}

[[nodiscard]] uintmax_t UnsignedArgument(va_list& arguments, const Length length) noexcept
{
    switch (length)
    {
    case Length::Char:
        return static_cast<unsigned char>(va_arg(arguments, int));
    case Length::Short:
        return static_cast<unsigned short>(va_arg(arguments, int));
    case Length::Long:
        return va_arg(arguments, unsigned long);
    case Length::LongLong:
        return va_arg(arguments, unsigned long long);
    case Length::Maximum:
        return va_arg(arguments, uintmax_t);
    case Length::Size:
    case Length::Difference:
        return va_arg(arguments, size_t);
    case Length::Default:
    case Length::Extended:
        return va_arg(arguments, unsigned int);
    }
    return 0U;
}

void StoreCount(va_list& arguments, const Length length, const size_t count) noexcept
{
    switch (length)
    {
    case Length::Char:
        *va_arg(arguments, signed char*) = static_cast<signed char>(count);
        break;
    case Length::Short:
        *va_arg(arguments, short*) = static_cast<short>(count);
        break;
    case Length::Long:
        *va_arg(arguments, long*) = static_cast<long>(count);
        break;
    case Length::LongLong:
        *va_arg(arguments, long long*) = static_cast<long long>(count);
        break;
    case Length::Maximum:
        *va_arg(arguments, intmax_t*) = static_cast<intmax_t>(count);
        break;
    case Length::Size:
    case Length::Difference:
        *va_arg(arguments, ptrdiff_t*) = static_cast<ptrdiff_t>(count);
        break;
    case Length::Default:
    case Length::Extended:
        *va_arg(arguments, int*) = static_cast<int>(count);
        break;
    }
}

void WideString(Output& output, const Specification& spec, const wchar_t* const text) noexcept
{
    if (text == nullptr)
    {
        Text(output, spec, "(null)", spec.Precision >= 0 && spec.Precision < 6 ? 0U : 6U);
        return;
    }
    size_t length{0U};
    const size_t limit{spec.Precision >= 0 ? static_cast<size_t>(spec.Precision) : SIZE_MAX};
    while (length < limit && text[length] != L'\0')
    {
        if (text[length] < 0 || text[length] > 127)
        {
            output.Fail(EILSEQ);
            return;
        }
        ++length;
    }
    if (!spec.Left)
    {
        output.Character(' ', Padding(spec, length));
    }
    for (size_t index{0U}; index < length; ++index)
    {
        output.Character(static_cast<char>(text[index]));
    }
    if (spec.Left)
    {
        output.Character(' ', Padding(spec, length));
    }
}

void Convert(Output& output, const Specification& spec, va_list& arguments) noexcept
{
    if (spec.Overflow)
    {
        output.Fail(EOVERFLOW);
        return;
    }
    const char conversion{spec.Conversion};
    if (conversion == 'd' || conversion == 'i')
    {
        const intmax_t value = SignedArgument(arguments, spec.Modifier);
        const uintmax_t magnitude = value < 0 ? 0U - static_cast<uintmax_t>(value) : static_cast<uintmax_t>(value);
        Integer(output, spec, magnitude, value < 0, 10U, true, false);
    }
    else if (conversion == 'u' || conversion == 'o' || conversion == 'x' || conversion == 'X')
    {
        const unsigned int base{conversion == 'o' ? 8U : (conversion == 'u' ? 10U : 16U)};
        Integer(output, spec, UnsignedArgument(arguments, spec.Modifier), false, base, false, false);
    }
    else if (conversion == 'c' && spec.Modifier == Length::Long)
    {
        const auto character = va_arg(arguments, unsigned int);
        if (character > 127U)
        {
            output.Fail(EILSEQ);
        }
        else
        {
            const auto byte = static_cast<char>(character);
            Text(output, spec, &byte, 1U);
        }
    }
    else if (conversion == 's' && spec.Modifier == Length::Long)
    {
        WideString(output, spec, va_arg(arguments, const wchar_t*));
    }
    else if (conversion == 'c')
    {
        const char character = static_cast<char>(va_arg(arguments, int));
        Text(output, spec, &character, 1U);
    }
    else if (conversion == 's')
    {
        const char* text = va_arg(arguments, const char*);
        if (text == nullptr)
        {
            text = spec.Precision >= 0 && spec.Precision < 6 ? "" : "(null)";
        }
        const size_t length =
            ASTERIA_LIBC_NAME(strnlen)(text, spec.Precision >= 0 ? static_cast<size_t>(spec.Precision) : SIZE_MAX);
        Text(output, spec, text, length);
    }
    else if (conversion == 'p')
    {
        const auto pointer = reinterpret_cast<uintptr_t>(va_arg(arguments, void*));
        if (pointer == 0U)
        {
            Text(output, spec, "(nil)", 5U);
        }
        else
        {
            Integer(output, spec, pointer, false, 16U, false, true);
        }
    }
    else if (conversion == 'n')
    {
        StoreCount(arguments, spec.Modifier, output.Count());
    }
    else if (conversion == '%')
    {
        output.Character('%');
    }
    else if (conversion == 'f' || conversion == 'F' || conversion == 'e' || conversion == 'E' || conversion == 'g' ||
             conversion == 'G' || conversion == 'a' || conversion == 'A')
    {
        // L is deliberately binary64 precision; consume the target's long-double ABI correctly.
        if (spec.Modifier == Length::Extended)
        {
            const long double value = va_arg(arguments, long double);
            Floating(output, spec, LongDoubleBits(value));
        }
        else
        {
            Floating(output, spec, DoubleBits(va_arg(arguments, double)));
        }
    }
    else
    {
        output.Fail(EINVAL);
    }
}

struct BufferSink
{
    char* Destination;
    size_t Capacity;
    size_t Used;
};

[[nodiscard]] bool WriteBuffer(void* const context, const char* const bytes, const size_t count,
                               size_t repetitions) noexcept
{
    auto& buffer = *static_cast<BufferSink*>(context);
    if (buffer.Capacity == 0U || buffer.Used >= buffer.Capacity - 1U)
    {
        return true;
    }
    size_t remaining{buffer.Capacity - 1U - buffer.Used};
    if (count == 1U)
    {
        const size_t copy{repetitions < remaining ? repetitions : remaining};
        static_cast<void>(ASTERIA_LIBC_NAME(memset)(buffer.Destination + buffer.Used, bytes[0U], copy));
        buffer.Used += copy;
        return true;
    }
    while (remaining != 0U && repetitions != 0U)
    {
        const size_t copy{count < remaining ? count : remaining};
        static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(buffer.Destination + buffer.Used, bytes, copy));
        buffer.Used += copy;
        remaining -= copy;
        --repetitions;
    }
    return true;
}
} // namespace

int Format(const FormatSink& sink, const char* const format, va_list arguments) noexcept
{
    const auto& lock = GetHooks().ConversionLock;
    Lock(lock);
    va_list copy;
    va_copy(copy, arguments);
    Output output{sink};
    const char* cursor{format};
    while (*cursor != '\0' && !output.Failed())
    {
        const char* const start = cursor;
        while (*cursor != '\0' && *cursor != '%')
        {
            ++cursor;
        }
        output.Write(start, static_cast<size_t>(cursor - start));
        if (*cursor == '%')
        {
            ++cursor;
            const Specification spec = ReadSpecification(cursor, copy);
            Convert(output, spec, copy);
        }
    }
    va_end(copy);
    Unlock(lock);
    return output.Failed() ? -1 : static_cast<int>(output.Count());
}
} // namespace Asteria::User::Libc

extern "C" int ASTERIA_LIBC_NAME(vsnprintf)(char* const destination, const size_t capacity, const char* const format,
                                            va_list arguments) noexcept
{
    Asteria::User::Libc::BufferSink buffer{.Destination = destination, .Capacity = capacity, .Used = 0U};
    const Asteria::User::Libc::FormatSink sink{.Context = &buffer, .Write = Asteria::User::Libc::WriteBuffer};
    const int result = Asteria::User::Libc::Format(sink, format, arguments);
    if (capacity != 0U)
    {
        destination[buffer.Used] = '\0';
    }
    return result;
}

extern "C" int ASTERIA_LIBC_NAME(snprintf)(char* const destination, const size_t capacity, const char* const format,
                                           ...) noexcept
{
    va_list arguments;
    va_start(arguments, format);
    const int result = ASTERIA_LIBC_NAME(vsnprintf)(destination, capacity, format, arguments);
    va_end(arguments);
    return result;
}

extern "C" int ASTERIA_LIBC_NAME(vsprintf)(char* const destination, const char* const format,
                                           va_list arguments) noexcept
{
    return ASTERIA_LIBC_NAME(vsnprintf)(destination, SIZE_MAX, format, arguments);
}

extern "C" int ASTERIA_LIBC_NAME(sprintf)(char* const destination, const char* const format, ...) noexcept
{
    va_list arguments;
    va_start(arguments, format);
    const int result = ASTERIA_LIBC_NAME(vsprintf)(destination, format, arguments);
    va_end(arguments);
    return result;
}
