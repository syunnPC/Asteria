// SPDX-License-Identifier: Apache-2.0

#include "User/Shell/Internal.hpp"
namespace Asteria::User::Shell
{
Text Literal(const char* const text) noexcept
{
    Size length{0U};
    if (text != nullptr)
    {
        while (length <= InputLimit && text[length] != '\0')
        {
            ++length;
        }
    }
    return {text, length};
}
bool Equal(const Text left, const Text right) noexcept
{
    if (left.Length != right.Length)
    {
        return false;
    }
    for (Size i{0U}; i < left.Length; ++i)
    {
        if (left.Data[i] != right.Data[i])
        {
            return false;
        }
    }
    return true;
}
bool Buffer::Append(const Text text) noexcept
{
    if (Length > Capacity || text.Length > Capacity - Length)
    {
        return false;
    }
    for (Size i{0U}; i < text.Length; ++i)
    {
        Data[Length++] = text.Data[i];
    }
    return true;
}
bool Buffer::Put(const char byte) noexcept
{
    return Append({&byte, 1U});
}
bool Fields::Add(const Text value) noexcept
{
    if (Count >= FieldLimit || Used > ExpansionLimit || value.Length > ExpansionLimit - Used)
    {
        return false;
    }
    Items[Count++] = Detail::MakeSpan(Used, value.Length);
    for (Size i{0U}; i < value.Length; ++i)
    {
        Bytes[Used++] = value.Data[i];
    }
    return true;
}
Text ErrorMessage(const Error code) noexcept
{
    switch (code)
    {
    case Error::None:
        return Literal("success");
    case Error::InputTooLarge:
        return Literal("input limit exceeded");
    case Error::InvalidUtf8:
        return Literal("invalid UTF-8");
    case Error::NulByte:
        return Literal("NUL is not shell text");
    case Error::TokenCapacity:
        return Literal("token limit exceeded");
    case Error::NodeCapacity:
        return Literal("AST node limit exceeded");
    case Error::WordCapacity:
        return Literal("word limit exceeded");
    case Error::RedirectCapacity:
        return Literal("redirection limit exceeded");
    case Error::Syntax:
        return Literal("invalid shell syntax");
    case Error::UnterminatedQuote:
        return Literal("unterminated quote");
    case Error::UnterminatedExpansion:
        return Literal("unterminated expansion");
    case Error::MissingHereDocument:
        return Literal("missing here-document delimiter");
    case Error::Depth:
        return Literal("nesting limit exceeded");
    case Error::ExpansionCapacity:
        return Literal("expansion limit exceeded");
    case Error::FieldCapacity:
        return Literal("field limit exceeded");
    case Error::VariableCapacity:
        return Literal("variable limit exceeded");
    case Error::InvalidName:
        return Literal("invalid variable name");
    case Error::ValueCapacity:
        return Literal("value limit exceeded");
    case Error::FunctionCapacity:
        return Literal("function limit exceeded");
    case Error::Arithmetic:
        return Literal("invalid arithmetic expression");
    case Error::ParameterUnset:
        return Literal("parameter unset or null");
    case Error::Host:
        return Literal("injected host operation failed or unavailable");
    case Error::Steps:
        return Literal("execution step limit exceeded");
    case Error::InvalidProgram:
        return Literal("program was not successfully parsed");
    }
    return Literal("unknown shell error");
}
namespace Detail
{
Result Validate(const Text text) noexcept
{
    if (text.Length != 0U && text.Data == nullptr)
    {
        return {Error::InvalidUtf8, 0U, 2};
    }
    for (Size i{0U}; i < text.Length;)
    {
        const auto c = static_cast<Foundation::UInt8>(text.Data[i]);
        if (c == 0U)
        {
            return {Error::NulByte, i, 2};
        }
        if (c < 0x80U)
        {
            ++i;
            continue;
        }
        const Size n =
            c >= 0xC2U && c <= 0xDFU ? 2U : (c >= 0xE0U && c <= 0xEFU ? 3U : (c >= 0xF0U && c <= 0xF4U ? 4U : 0U));
        if (n == 0U || n > text.Length - i)
        {
            return {Error::InvalidUtf8, i, 2};
        }
        UInt32 scalar = static_cast<UInt32>(c & (n == 2U ? 0x1FU : (n == 3U ? 0x0FU : 0x07U)));
        for (Size j{1U}; j < n; ++j)
        {
            const auto next = static_cast<Foundation::UInt8>(text.Data[i + j]);
            if ((next & 0xC0U) != 0x80U)
            {
                return {Error::InvalidUtf8, i + j, 2};
            }
            scalar = (scalar << 6U) | (next & 0x3FU);
        }
        if ((n == 3U && scalar < 0x800U) || (n == 4U && scalar < 0x10000U) ||
            (scalar >= 0xD800U && scalar <= 0xDFFFU) || scalar > 0x10FFFFU)
        {
            return {Error::InvalidUtf8, i, 2};
        }
        i += n;
    }
    return {};
}
bool DollarByte(const Text text, Size& cursor, char& byte, Result& error) noexcept
{
    if (cursor >= text.Length)
    {
        error = {Error::UnterminatedQuote, cursor, 2};
        return false;
    }
    byte = text.Data[cursor++];
    switch (byte)
    {
    case 'a':
        byte = '\a';
        break;
    case 'b':
        byte = '\b';
        break;
    case 'e':
        byte = '\x1B';
        break;
    case 'f':
        byte = '\f';
        break;
    case 'n':
        byte = '\n';
        break;
    case 'r':
        byte = '\r';
        break;
    case 't':
        byte = '\t';
        break;
    case 'v':
        byte = '\v';
        break;
    case '\\':
    case '\'':
    case '"':
        break;
    case 'c':
        if (cursor >= text.Length)
        {
            error = {Error::Syntax, cursor, 2};
            return false;
        }
        byte = text.Data[cursor++];
        if (byte == '\\')
        {
            if (cursor >= text.Length || text.Data[cursor++] != '\\')
            {
                error = {Error::Syntax, cursor, 2};
                return false;
            }
        }
        byte = byte == '?' ? static_cast<char>(127) : static_cast<char>(static_cast<Foundation::UInt8>(byte) & 31U);
        break;
    default:
    {
        unsigned value{0U};
        unsigned count{0U};
        const bool hex = byte == 'x';
        const unsigned base = hex ? 16U : 8U;
        if (!hex)
        {
            --cursor;
        }
        while (cursor < text.Length && count < (hex ? 2U : 3U))
        {
            const char c = text.Data[cursor];
            const unsigned digit =
                Digit(c) ? static_cast<unsigned>(c - '0')
                         : (c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                                 : (c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : base));
            if (digit >= base)
            {
                break;
            }
            value = value * base + digit;
            ++cursor;
            ++count;
        }
        if (count == 0U || value > 255U)
        {
            error = {Error::Syntax, cursor, 2};
            return false;
        }
        byte = static_cast<char>(static_cast<Foundation::UInt8>(value));
        break;
    }
    }
    if (byte == 0)
    {
        error = {Error::NulByte, cursor, 2};
        return false;
    }
    return true;
}
Text UnsignedNumber(UInt64 number, char* const bytes, const unsigned base) noexcept
{
    Size used{0U};
    do
    {
        const unsigned digit = static_cast<unsigned>(number % base);
        bytes[used++] = static_cast<char>(digit < 10U ? '0' + digit : 'a' + digit - 10U);
        number /= base;
    } while (number != 0U && used < NumberLimit);
    for (Size i{0U}; i < used / 2U; ++i)
    {
        const char c = bytes[i];
        bytes[i] = bytes[used - i - 1U];
        bytes[used - i - 1U] = c;
    }
    return {bytes, used};
}
Text Number(const Int64 number, char* const bytes) noexcept
{
    if (number >= 0)
    {
        return UnsignedNumber(static_cast<UInt64>(number), bytes);
    }
    bytes[0U] = '-';
    const Text digits = UnsignedNumber(UInt64{0U} - static_cast<UInt64>(number), bytes + 1U);
    return {bytes, digits.Length + 1U};
}
} // namespace Detail
} // namespace Asteria::User::Shell
