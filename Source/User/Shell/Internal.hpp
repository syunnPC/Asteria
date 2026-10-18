// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SHELL_INTERNAL_HPP_INCLUDED
#define ASTERIA_USER_SHELL_INTERNAL_HPP_INCLUDED
#include "User/Shell/Shell.hpp"
namespace Asteria::User::Shell::Detail
{
[[nodiscard]] constexpr bool Blank(char c) noexcept
{
    return c == ' ' || c == '\t';
}
[[nodiscard]] constexpr bool Space(char c) noexcept
{
    return Blank(c) || c == '\n';
}
[[nodiscard]] constexpr bool Digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}
[[nodiscard]] constexpr bool NameStart(char c) noexcept
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] constexpr bool NameByte(char c) noexcept
{
    return NameStart(c) || Digit(c);
}
[[nodiscard]] inline bool Name(Text t) noexcept
{
    if (t.Length == 0U || !NameStart(t.Data[0U]))
    {
        return false;
    }
    for (Size i{1U}; i < t.Length; ++i)
    {
        if (!NameByte(t.Data[i]))
        {
            return false;
        }
    }
    return true;
}
inline void SkipContinuation(Text text, Size& cursor) noexcept
{
    while (cursor + 1U < text.Length && text.Data[cursor] == '\\' && text.Data[cursor + 1U] == '\n')
    {
        cursor += 2U;
    }
}
[[nodiscard]] inline bool ParameterName(Text text) noexcept
{
    Size i{0U};
    SkipContinuation(text, i);
    if (i == text.Length)
    {
        return false;
    }
    const bool named = NameStart(text.Data[i]);
    const bool numbered = Digit(text.Data[i]);
    if (!named && !numbered)
    {
        const char c = text.Data[i++];
        SkipContinuation(text, i);
        return i == text.Length && (c == '@' || c == '*' || c == '#' || c == '?' || c == '!' || c == '-' || c == '$');
    }
    do
    {
        ++i;
        SkipContinuation(text, i);
    } while (i < text.Length && (named ? NameByte(text.Data[i]) : Digit(text.Data[i])));
    return i == text.Length;
}
[[nodiscard]] inline Text Slice(Text t, Size start, Size length) noexcept
{
    return {t.Data + start, length};
}
[[nodiscard]] inline bool Is(Text t, const char* s) noexcept
{
    return Equal(t, Literal(s));
}
[[nodiscard]] inline Span MakeSpan(Size start, Size length) noexcept
{
    return {static_cast<UInt16>(start), static_cast<UInt16>(length)};
}
[[nodiscard]] inline Size ScalarBytes(Text t, Size i) noexcept
{
    const auto c = static_cast<Foundation::UInt8>(t.Data[i]);
    return c < 0x80U ? 1U : (c < 0xE0U ? 2U : (c < 0xF0U ? 3U : 4U));
}
[[nodiscard]] Result Validate(Text text) noexcept;
[[nodiscard]] inline Size IfsWidth(Text input, Size i, Text ifs) noexcept
{
    for (Size j{0U}; j < ifs.Length;)
    {
        const Size n = ScalarBytes(ifs, j);
        if (n <= input.Length - i && Equal(Slice(input, i, n), Slice(ifs, j, n)))
        {
            return n;
        }
        j += n;
    }
    return 0U;
}
// C integer constants, with an optional sign. Used by arithmetic and printf.
[[nodiscard]] inline bool Integer(Text text, UInt64& value) noexcept
{
    Size i{0U};
    bool negative{false};
    value = 0U;
    if (i < text.Length && (text.Data[i] == '-' || text.Data[i] == '+'))
    {
        negative = text.Data[i++] == '-';
    }
    unsigned base{10U};
    if (i < text.Length && text.Data[i] == '0')
    {
        base = 8U;
        if (i + 1U < text.Length && (text.Data[i + 1U] == 'x' || text.Data[i + 1U] == 'X'))
        {
            base = 16U;
            i += 2U;
        }
    }
    const Size start = i;
    for (; i < text.Length; ++i)
    {
        const char c = text.Data[i];
        const unsigned digit =
            Digit(c) ? static_cast<unsigned>(c - '0')
                     : (c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                             : (c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : base));
        if (digit >= base)
        {
            if (negative)
            {
                value = UInt64{0U} - value;
            }
            return false;
        }
        value = value * base + digit;
    }
    if (negative)
    {
        value = UInt64{0U} - value;
    }
    return i != start;
}
// Skip a quote or expansion beginning at cursor. Cursor is immediately after
// its closing delimiter on success. Recognition only, no evaluation (§2.3).
[[nodiscard]] bool Unit(Text text, Size& cursor, Size depth, Result& error) noexcept;
[[nodiscard]] Result Tokenize(Program& program) noexcept;
[[nodiscard]] bool DollarByte(Text text, Size& cursor, char& byte, Result& error) noexcept;
[[nodiscard]] inline bool Decimal(Text t, UInt64& value) noexcept
{
    value = 0U;
    if (t.Length == 0U)
    {
        return false;
    }
    for (Size i{0U}; i < t.Length; ++i)
    {
        if (!Digit(t.Data[i]))
        {
            return false;
        }
        const UInt64 digit = static_cast<UInt64>(t.Data[i] - '0');
        if (value > (~UInt64{0U} - digit) / 10U)
        {
            return false;
        }
        value = value * 10U + digit;
    }
    return true;
}
[[nodiscard]] Text Number(Int64 number, char* bytes) noexcept;
[[nodiscard]] Text UnsignedNumber(UInt64 number, char* bytes, unsigned base = 10U) noexcept;
[[nodiscard]] inline bool SpecialBuiltin(Text name) noexcept
{
    constexpr const char* names[]{":", "export", "unset", "set", "shift", "exit", "return", "eval", "."};
    for (const char* n : names)
    {
        if (Is(name, n))
        {
            return true;
        }
    }
    return false;
}
[[nodiscard]] inline bool BuiltinName(Text name) noexcept
{
    if (SpecialBuiltin(name))
    {
        return true;
    }
    constexpr const char* names[]{"cd", "pwd", "echo", "printf", "read", "test", "[", "true", "false", "type"};
    for (const char* n : names)
    {
        if (Is(name, n))
        {
            return true;
        }
    }
    return false;
}
inline constexpr Foundation::UInt8 Quoted{1U};
inline constexpr Foundation::UInt8 Splittable{2U};
} // namespace Asteria::User::Shell::Detail
#endif
