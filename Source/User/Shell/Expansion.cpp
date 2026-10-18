// SPDX-License-Identifier: Apache-2.0

#include "User/Shell/Internal.hpp"
namespace Asteria::User::Shell
{
using Detail::Decimal;
using Detail::Digit;
using Detail::DollarByte;
using Detail::IfsWidth;
using Detail::Is;
using Detail::Name;
using Detail::NameByte;
using Detail::NameStart;
using Detail::Number;
using Detail::ParameterName;
using Detail::Quoted;
using Detail::ScalarBytes;
using Detail::SkipContinuation;
using Detail::Slice;
using Detail::Space;
using Detail::Splittable;
using Detail::Unit;
using Detail::UnsignedNumber;
using Detail::Validate;
namespace
{
[[nodiscard]] bool Contains(Text text, char c) noexcept
{
    for (Size i{0U}; i < text.Length; ++i)
    {
        if (text.Data[i] == c)
        {
            return true;
        }
    }
    return false;
}
[[nodiscard]] bool CharacterClass(Text name, char byte) noexcept
{
    const auto c = static_cast<Foundation::UInt8>(byte);
    const bool lower = c >= 'a' && c <= 'z';
    const bool upper = c >= 'A' && c <= 'Z';
    const bool digit = c >= '0' && c <= '9';
    const bool alpha = lower || upper;
    const bool graph = c >= 33U && c <= 126U;
    if (Is(name, "alnum"))
    {
        return alpha || digit;
    }
    if (Is(name, "alpha"))
    {
        return alpha;
    }
    if (Is(name, "blank"))
    {
        return c == ' ' || c == '\t';
    }
    if (Is(name, "cntrl"))
    {
        return c < 32U || c == 127U;
    }
    if (Is(name, "digit"))
    {
        return digit;
    }
    if (Is(name, "graph"))
    {
        return graph;
    }
    if (Is(name, "lower"))
    {
        return lower;
    }
    if (Is(name, "print"))
    {
        return c >= 32U && c <= 126U;
    }
    if (Is(name, "punct"))
    {
        return graph && !alpha && !digit;
    }
    if (Is(name, "space"))
    {
        return c == ' ' || (c >= 9U && c <= 13U);
    }
    if (Is(name, "upper"))
    {
        return upper;
    }
    if (Is(name, "xdigit"))
    {
        return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    return false;
}
[[nodiscard]] bool Less(Text a, Text b) noexcept
{
    const Size n = a.Length < b.Length ? a.Length : b.Length;
    for (Size i{0U}; i < n; ++i)
    {
        const auto x = static_cast<Foundation::UInt8>(a.Data[i]);
        const auto y = static_cast<Foundation::UInt8>(b.Data[i]);
        if (x != y)
        {
            return x < y;
        }
    }
    return a.Length < b.Length;
}
} // Namespace
bool Engine::Append(ExpansionScratch& out, const Text value, const Foundation::UInt8 flags) noexcept
{
    if (value.Length > ExpansionLimit - out.Length)
    {
        return Fail(Error::ExpansionCapacity);
    }
    for (Size i{0U}; i < value.Length; ++i)
    {
        out.Bytes[out.Length] = value.Data[i];
        out.Flags[out.Length++] = flags;
    }
    return true;
}
Text Engine::Special(const Text name, ExpansionScratch& scratch, bool& present) noexcept
{
    present = true;
    if (name.Length == 1U)
    {
        switch (name.Data[0U])
        {
        case '?':
            return Number(Environment.LastStatus, scratch.Number);
        case '#':
            return UnsignedNumber(Environment.ParameterCount, scratch.Number);
        case '$':
            return UnsignedNumber(Environment.ProcessId, scratch.Number);
        case '!':
            present = Environment.BackgroundId != 0U;
            return present ? UnsignedNumber(Environment.BackgroundId, scratch.Number) : Text{};
        case '-':
        {
            Size length{0U};
            if (Environment.AllExport)
            {
                scratch.Number[length++] = 'a';
            }
            if (Environment.Errexit)
            {
                scratch.Number[length++] = 'e';
            }
            if (Environment.NoGlob)
            {
                scratch.Number[length++] = 'f';
            }
            if (Environment.NoExecute)
            {
                scratch.Number[length++] = 'n';
            }
            if (Environment.Nounset)
            {
                scratch.Number[length++] = 'u';
            }
            if (Environment.Trace)
            {
                scratch.Number[length++] = 'x';
            }
            if (Environment.NoClobber)
            {
                scratch.Number[length++] = 'C';
            }
            return {scratch.Number, length};
        }
        case '0':
            return {Environment.Name.Bytes, Environment.Name.Length};
        default:
            break;
        }
    }
    UInt64 position{0U};
    if (Decimal(name, position))
    {
        present = position <= Environment.ParameterCount;
        if (position == 0U)
        {
            return {Environment.Name.Bytes, Environment.Name.Length};
        }
        return present ? Text{Environment.Parameters[position - 1U].Bytes, Environment.Parameters[position - 1U].Length}
                       : Text{};
    }
    return Get(name, present);
}
bool Engine::Parameter(const Text expression, ExpansionScratch& out, const bool quoted) noexcept
{
    Size cursor{0U};
    bool length{false};
    SkipContinuation(expression, cursor);
    if (cursor < expression.Length && expression.Data[cursor] == '#')
    {
        Size next = cursor + 1U;
        SkipContinuation(expression, next);
        if (next < expression.Length && ParameterName(Slice(expression, next, expression.Length - next)))
        {
            length = true;
            cursor = next;
        }
    }
    if (cursor >= expression.Length)
    {
        return Fail(Error::Syntax);
    }
    const bool named = NameStart(expression.Data[cursor]);
    const bool numbered = Digit(expression.Data[cursor]);
    Size nameLength{0U};
    if (!named && !numbered && !Contains(Literal("@*#?!-$"), expression.Data[cursor]))
    {
        return Fail(Error::Syntax);
    }
    do
    {
        if (nameLength >= NameLimit)
        {
            return Fail(Error::InvalidName);
        }
        out.ParameterName[nameLength++] = expression.Data[cursor++];
        SkipContinuation(expression, cursor);
    } while (cursor < expression.Length &&
             (named ? NameByte(expression.Data[cursor]) : (numbered && Digit(expression.Data[cursor]))));
    const Text name{out.ParameterName, nameLength};
    const auto flags = static_cast<Foundation::UInt8>(quoted ? Quoted : Splittable);
    if (cursor == expression.Length && !length && (Is(name, "@") || Is(name, "*")))
    {
        bool set{false};
        const Text ifs = Get(Literal("IFS"), set);
        const Text join = set ? (ifs.Length != 0U ? Slice(ifs, 0U, ScalarBytes(ifs, 0U)) : Text{}) : Literal(" ");
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            if (i != 0U)
            {
                if (out.Split && (Is(name, "@") || !quoted))
                {
                    if (out.BoundaryCount >= FieldLimit)
                    {
                        return Fail(Error::FieldCapacity);
                    }
                    out.Boundaries[out.BoundaryCount++] = out.Length;
                }
                else if (!Append(out, join, flags))
                {
                    return false;
                }
            }
            if (!Append(out, {Environment.Parameters[i].Bytes, Environment.Parameters[i].Length}, flags))
            {
                return false;
            }
        }
        if (quoted && Environment.ParameterCount != 0U)
        {
            out.KeepEmpty = true;
        }
        return true;
    }
    bool present{false};
    Text value = Special(name, out, present);
    if (length)
    {
        if (cursor != expression.Length)
        {
            return Fail(Error::Syntax);
        }
        if (!present && Environment.Nounset)
        {
            return Fail(Error::ParameterUnset);
        }
        Size characters{0U};
        for (Size i{0U}; i < value.Length; i += ScalarBytes(value, i))
        {
            ++characters;
        }
        return Append(out, UnsignedNumber(characters, out.Number), flags);
    }
    if (cursor == expression.Length)
    {
        if (!present && Environment.Nounset)
        {
            return Fail(Error::ParameterUnset);
        }
        return Append(out, value, flags);
    }
    bool colon{false};
    if (expression.Data[cursor] == ':')
    {
        colon = true;
        ++cursor;
    }
    if (cursor == expression.Length)
    {
        return Fail(Error::Syntax);
    }
    const char operation = expression.Data[cursor++];
    bool longest{false};
    if ((operation == '#' || operation == '%') && cursor < expression.Length && expression.Data[cursor] == operation)
    {
        longest = true;
        ++cursor;
    }
    if (!Contains(Literal("-+=?#%"), operation) || (colon && (operation == '#' || operation == '%')))
    {
        return Fail(Error::Syntax);
    }
    const Text word = Slice(expression, cursor, expression.Length - cursor);
    const bool missing = !present || (colon && value.Length == 0U);
    if (operation == '-' && !missing)
    {
        return Append(out, value, flags);
    }
    if (operation == '=' && !missing)
    {
        return Append(out, value, flags);
    }
    if (operation == '?' && !missing)
    {
        return Append(out, value, flags);
    }
    if (operation == '+' && missing)
    {
        return true;
    }
    if (m_expansionDepth >= ScriptLimit)
    {
        return Fail(Error::Depth);
    }
    ExpansionScratch& temporary = m_expansions[m_expansionDepth++];
    temporary.Split = out.Split;
    temporary.Length = 0U;
    temporary.KeepEmpty = false;
    temporary.BoundaryCount = 0U;
    temporary.MarkerCount = 0U;
    const bool trim = operation == '#' || operation == '%';
    bool ok = ExpandInto(word, temporary, trim ? false : quoted);
    if (ok && trim)
    {
        if (!present && Environment.Nounset)
        {
            ok = Fail(Error::ParameterUnset);
        }
        Size chosen = operation == '#' ? 0U : value.Length;
        bool matched{false};
        for (Size n{0U}; ok && n <= value.Length; ++n)
        {
            // Match only at UTF-8 scalar boundaries.
            if (n < value.Length && (static_cast<Foundation::UInt8>(value.Data[n]) & 0xC0U) == 0x80U)
            {
                continue;
            }
            const Text candidate = operation == '#' ? Slice(value, 0U, n) : Slice(value, n, value.Length - n);
            if (Match({temporary.Bytes, temporary.Length}, candidate, temporary.Flags))
            {
                if (!matched || (operation == '#' ? longest : !longest))
                {
                    chosen = n;
                }
                matched = true;
                if ((operation == '#' && !longest) || (operation == '%' && longest))
                {
                    break;
                }
            }
        }
        if (ok)
        {
            ok = Append(out, operation == '#' ? Slice(value, chosen, value.Length - chosen) : Slice(value, 0U, chosen),
                        flags);
        }
    }
    else if (ok && operation == '?')
    {
        if (temporary.Length != 0U)
        {
            static_cast<void>(Write({temporary.Bytes, temporary.Length}, 2U));
        }
        ok = Fail(Error::ParameterUnset);
    }
    else if (ok)
    {
        if (operation == '=')
        {
            const Result assigned = Set(name, {temporary.Bytes, temporary.Length});
            if (!assigned.Ok())
            {
                ok = Fail(assigned.Code);
            }
        }
        if (ok)
        {
            if (temporary.Length > ExpansionLimit - out.Length)
            {
                ok = Fail(Error::ExpansionCapacity);
            }
            else
            {
                const Size base = out.Length;
                for (Size i{0U}; i < temporary.BoundaryCount; ++i)
                {
                    if (out.BoundaryCount >= FieldLimit)
                    {
                        ok = Fail(Error::FieldCapacity);
                        break;
                    }
                    out.Boundaries[out.BoundaryCount++] = base + temporary.Boundaries[i];
                }
                for (Size i{0U}; i < temporary.MarkerCount; ++i)
                {
                    if (out.MarkerCount >= FieldLimit)
                    {
                        ok = Fail(Error::FieldCapacity);
                        break;
                    }
                    out.EmptyMarkers[out.MarkerCount++] = base + temporary.EmptyMarkers[i];
                }
                for (Size i{0U}; ok && i < temporary.Length; ++i)
                {
                    out.Bytes[out.Length] = temporary.Bytes[i];
                    out.Flags[out.Length++] = static_cast<Foundation::UInt8>(temporary.Flags[i] | flags);
                }
                out.KeepEmpty = out.KeepEmpty || temporary.KeepEmpty;
            }
        }
    }
    --m_expansionDepth;
    return ok;
}
bool Engine::Substitution(const Text word, Size& i, ExpansionScratch& out, const bool quoted) noexcept
{
    const char c = word.Data[i];
    Size end = i;
    Result recognition{};
    if (!Unit(word, end, 0U, recognition))
    {
        return Fail(recognition.Code, recognition.Offset);
    }
    const bool parameter = c == '$' && word.Data[i + 1U] == '{';
    const bool arithmetic = c == '$' && word.Data[i + 1U] == '(' && i + 2U < word.Length && word.Data[i + 2U] == '(';
    if (parameter)
    {
        if (!Parameter(Slice(word, i + 2U, end - i - 3U), out, quoted))
        {
            return false;
        }
    }
    else if (arithmetic)
    {
        if (end < i + 5U || word.Data[end - 2U] != ')')
        {
            return Fail(Error::Arithmetic);
        }
        if (m_expansionDepth >= ScriptLimit)
        {
            return Fail(Error::Depth);
        }
        ExpansionScratch& temporary = m_expansions[m_expansionDepth++];
        temporary.Length = 0U;
        temporary.BoundaryCount = 0U;
        temporary.KeepEmpty = false;
        temporary.MarkerCount = 0U;
        Int64 value{0};
        const bool ok = ExpandInto(Slice(word, i + 3U, end - i - 5U), temporary, true) &&
                        Arithmetic({temporary.Bytes, temporary.Length}, value);
        --m_expansionDepth;
        if (!ok || !Append(out, Number(value, out.Number), quoted ? Quoted : Splittable))
        {
            return false;
        }
    }
    else
    {
        if (m_host.Capture == nullptr)
        {
            return Fail(Error::Host);
        }
        if (m_childDepth + 1U >= ScriptLimit)
        {
            return Fail(Error::Depth);
        }
        Text source = Slice(word, i + (c == '`' ? 1U : 2U), end - i - (c == '`' ? 2U : 3U));
        if (c == '`')
        {
            out.Temporary.Used = 0U;
            Buffer unescaped{out.Temporary.Bytes, ExpansionLimit};
            for (Size j{0U}; j < source.Length; ++j)
            {
                if (source.Data[j] == '\\' && j + 1U < source.Length && Contains(Literal("$`\\"), source.Data[j + 1U]))
                {
                    ++j;
                }
                if (!unescaped.Put(source.Data[j]))
                {
                    return Fail(Error::ExpansionCapacity);
                }
            }
            source = unescaped.View();
        }
        // Capture uses another scratch slot so backquote input and
        // output never alias, and nested expansion cannot overwrite it.
        if (m_expansionDepth >= ScriptLimit)
        {
            return Fail(Error::Depth);
        }
        ExpansionScratch& capture = m_expansions[m_expansionDepth++];
        Buffer buffer{capture.Bytes, ExpansionLimit};
        int status{0};
        ++m_childDepth;
        const bool ok = m_host.Capture(m_host.Context, *this, source, buffer, status);
        --m_childDepth;
        --m_expansionDepth;
        if (!ok || buffer.Length > buffer.Capacity)
        {
            return Fail(Error::Host);
        }
        while (buffer.Length != 0U && buffer.Data[buffer.Length - 1U] == '\n')
        {
            --buffer.Length;
        }
        const Result valid = Validate(buffer.View());
        if (!valid.Ok())
        {
            return Fail(valid.Code, valid.Offset);
        }
        if (m_executionDepth != 0U)
        {
            m_execution[m_executionDepth - 1U].HasSubstitution = true;
            m_execution[m_executionDepth - 1U].SubstitutionStatus = status;
        }
        if (!Append(out, buffer.View(), quoted ? Quoted : Splittable))
        {
            return false;
        }
    }
    i = end;
    return true;
}
bool Engine::ExpandInto(const Text word, ExpansionScratch& out, const bool quoted, const bool here,
                        const bool assignment) noexcept
{
    Size quoteStart{0U};
    bool atOnly{false};
    char quote = quoted ? '"' : '\0';
    for (Size i{0U}; i < word.Length;)
    {
        if (!Step())
        {
            return false;
        }
        const char c = word.Data[i];
        if (!here && c == '~' && (i == 0U || (i != 0U && word.Data[i - 1U] == ':' && assignment && !quoted)) &&
            quote == 0)
        {
            Size end = i + 1U;
            while (end < word.Length && word.Data[end] != '/' && word.Data[end] != ':')
            {
                ++end;
            }
            bool present{false};
            Text home{};
            if (end == i + 1U)
            {
                home = Get(Literal("HOME"), present);
            }
            else if (m_filesystem.Home != nullptr)
            {
                out.Temporary.Used = 0U;
                Buffer buffer{out.Temporary.Bytes, ExpansionLimit};
                present = m_filesystem.Home(m_filesystem.Context, Slice(word, i + 1U, end - i - 1U), buffer);
                home = buffer.View();
            }
            if (present)
            {
                if (!Append(out, home, Quoted))
                {
                    return false;
                }
                i = end;
                continue;
            }
        }
        if (!here && !quoted && (c == '\'' || c == '"') && (quote == 0 || quote == c))
        {
            if (quote == 0)
            {
                quote = c;
                quoteStart = out.Length;
                atOnly = false;
                if (c == '"' && Environment.ParameterCount == 0U)
                {
                    const Text tail = Slice(word, i + 1U, word.Length - i - 1U);
                    atOnly = (tail.Length >= 3U && Equal(Slice(tail, 0U, 3U), Literal("$@\""))) ||
                             (tail.Length >= 5U && Equal(Slice(tail, 0U, 5U), Literal("${@}\"")));
                }
            }
            else
            {
                quote = 0;
                if (out.Length == quoteStart && !atOnly)
                {
                    if (out.MarkerCount >= FieldLimit)
                    {
                        return Fail(Error::FieldCapacity);
                    }
                    out.EmptyMarkers[out.MarkerCount++] = out.Length;
                }
            }
            ++i;
            continue;
        }
        if (c == '\\' && quote != '\'')
        {
            if (i + 1U == word.Length)
            {
                return Fail(Error::UnterminatedQuote, i);
            }
            const char next = word.Data[i + 1U];
            if (next == '\n')
            {
                i += 2U;
                continue;
            }
            if (quote == 0 || next == '$' || next == '`' || next == '\\' || (!here && next == '"'))
            {
                if (!Append(out, Slice(word, i + 1U, 1U), Quoted))
                {
                    return false;
                }
                i += 2U;
                continue;
            }
        }
        if (!here && c == '$' && quote == 0 && i + 1U < word.Length && word.Data[i + 1U] == '\'')
        {
            i += 2U;
            out.KeepEmpty = true;
            while (i < word.Length && word.Data[i] != '\'')
            {
                char byte = word.Data[i++];
                if (byte == '\\')
                {
                    Result error{};
                    if (!DollarByte(word, i, byte, error))
                    {
                        return Fail(error.Code, error.Offset);
                    }
                }
                if (!Append(out, {&byte, 1U}, Quoted))
                {
                    return false;
                }
            }
            if (i == word.Length)
            {
                return Fail(Error::UnterminatedQuote);
            }
            ++i;
            continue;
        }
        if ((c == '$' || c == '`') && quote != '\'')
        {
            if (c == '`' || (i + 1U < word.Length && (word.Data[i + 1U] == '(' || word.Data[i + 1U] == '{')))
            {
                if (!Substitution(word, i, out, quote != 0))
                {
                    return false;
                }
                continue;
            }
            if (i + 1U < word.Length && (NameStart(word.Data[i + 1U]) || Digit(word.Data[i + 1U]) ||
                                         Contains(Literal("@*#?!-$"), word.Data[i + 1U])))
            {
                Size end = i + 2U;
                if (NameStart(word.Data[i + 1U]))
                {
                    while (end < word.Length && NameByte(word.Data[end]))
                    {
                        ++end;
                    }
                }
                if (!Parameter(Slice(word, i + 1U, end - i - 1U), out, quote != 0))
                {
                    return false;
                }
                i = end;
                continue;
            }
        }
        if (!Append(out, Slice(word, i, 1U), quote != 0 ? Quoted : 0U))
        {
            return false;
        }
        ++i;
    }
    if (!here && !quoted && quote != 0)
    {
        return Fail(Error::UnterminatedQuote);
    }
    return true;
}
bool Engine::EmitField(ExpansionScratch& out, Size start, Size end, Fields& output, bool glob) noexcept
{
    const Text value{out.Bytes + start, end - start};
    bool pattern{false};
    for (Size j = start; j < end; ++j)
    {
        if ((out.Flags[j] & Quoted) == 0U && Contains(Literal("*?["), out.Bytes[j]))
        {
            pattern = true;
        }
    }
    out.Temporary.Count = 0U;
    out.Temporary.Used = 0U;
    if (glob && !Environment.NoGlob && pattern)
    {
        if (!Pathnames(value, out.Flags + start, out))
        {
            return Fail(Error::Host);
        }
        // Sort the borrowed field descriptors, not their bytes.
        for (Size a{1U}; a < out.Temporary.Count; ++a)
        {
            for (Size b = a; b != 0U && Less(out.Temporary.At(b), out.Temporary.At(b - 1U)); --b)
            {
                const Span saved = out.Temporary.Items[b];
                out.Temporary.Items[b] = out.Temporary.Items[b - 1U];
                out.Temporary.Items[b - 1U] = saved;
            }
        }
    }
    if (out.Temporary.Count == 0U)
    {
        if (!output.Add(value))
        {
            return Fail(output.Count >= FieldLimit ? Error::FieldCapacity : Error::ExpansionCapacity);
        }
    }
    else
    {
        for (Size j{0U}; j < out.Temporary.Count; ++j)
        {
            if (!output.Add(out.Temporary.At(j)))
            {
                return Fail(output.Count >= FieldLimit ? Error::FieldCapacity : Error::ExpansionCapacity);
            }
        }
    }
    return m_error.Ok();
}
Result Engine::Expand(const Text word, Fields& output, const bool split, const bool glob,
                      const bool assignment) noexcept
{
    const bool standalone = m_executionDepth == 0U && m_expansionDepth == 0U;
    if (standalone)
    {
        m_error = {};
        m_steps = 0U;
        if (!ValidateState())
        {
            return m_error;
        }
    }
    if (word.Length > InputLimit)
    {
        return {Error::InputTooLarge, InputLimit, 2};
    }
    const Result valid = Validate(word);
    if (!valid.Ok())
    {
        return valid;
    }
    if (m_expansionDepth >= ScriptLimit)
    {
        return {Error::Depth, 0U, 2};
    }
    ExpansionScratch& out = m_expansions[m_expansionDepth++];
    out.Split = split;
    out.Length = 0U;
    out.KeepEmpty = false;
    out.BoundaryCount = 0U;
    out.MarkerCount = 0U;
    out.Temporary.Count = 0U;
    out.Temporary.Used = 0U;
    bool ok = ExpandInto(word, out, false, false, assignment);
    if (ok)
    {
        const Result expandedValid = Validate({out.Bytes, out.Length});
        if (!expandedValid.Ok())
        {
            ok = Fail(expandedValid.Code, expandedValid.Offset);
        }
    }
    bool present{false};
    Text ifs = Get(Literal("IFS"), present);
    if (!present)
    {
        ifs = Literal(" \t\n");
    }
    Size segmentStart{0U};
    for (Size segment{0U}; ok && segment <= out.BoundaryCount; ++segment)
    {
        const Size segmentEnd = segment < out.BoundaryCount ? out.Boundaries[segment] : out.Length;
        Size start = segmentStart;
        for (Size i = start; ok && i <= segmentEnd;)
        {
            Size width{0U};
            if (i < segmentEnd && split && (out.Flags[i] & Splittable) != 0U && (out.Flags[i] & Quoted) == 0U)
            {
                width = IfsWidth({out.Bytes, segmentEnd}, i, ifs);
            }
            if (i != segmentEnd && width == 0U)
            {
                ++i;
                continue;
            }
            Size after = i + width;
            bool nonwhite = width != 0U && !Space(out.Bytes[i]);
            if (width != 0U && !nonwhite)
            {
                while (after < segmentEnd && (out.Flags[after] & Splittable) != 0U && (out.Flags[after] & Quoted) == 0U)
                {
                    const Size n = IfsWidth({out.Bytes, segmentEnd}, after, ifs);
                    if (n == 0U || !Space(out.Bytes[after]))
                    {
                        break;
                    }
                    after += n;
                }
                if (after < segmentEnd && (out.Flags[after] & Splittable) != 0U && (out.Flags[after] & Quoted) == 0U &&
                    !Space(out.Bytes[after]))
                {
                    const Size n = IfsWidth({out.Bytes, segmentEnd}, after, ifs);
                    if (n != 0U)
                    {
                        nonwhite = true;
                        after += n;
                    }
                }
            }
            if (width != 0U)
            {
                while (after < segmentEnd && (out.Flags[after] & Splittable) != 0U &&
                       (out.Flags[after] & Quoted) == 0U && Space(out.Bytes[after]))
                {
                    const Size n = IfsWidth({out.Bytes, segmentEnd}, after, ifs);
                    if (n == 0U)
                    {
                        break;
                    }
                    after += n;
                }
            }
            bool marker{false};
            for (Size m{0U}; m < out.MarkerCount; ++m)
            {
                marker = marker || (out.EmptyMarkers[m] >= start && out.EmptyMarkers[m] <= i);
            }
            const bool emit = i > start || nonwhite || marker ||
                              (i == segmentEnd && start == segmentStart && (out.KeepEmpty || !split));
            if (emit)
            {
                ok = EmitField(out, start, i, output, glob);
            }
            if (i == segmentEnd)
            {
                break;
            }
            i = after;
            start = i;
        }
        segmentStart = segmentEnd;
    }
    --m_expansionDepth;
    return ok ? Result{} : m_error;
}
bool Engine::Pathnames(const Text pattern, const Foundation::UInt8* const flags, ExpansionScratch& scratch) noexcept
{
    if (m_filesystem.ListDirectory == nullptr)
    {
        return Fail(Error::Host);
    }
    scratch.Temporary.Count = 0U;
    scratch.Temporary.Used = 0U;
    const bool absolute = pattern.Length != 0U && pattern.Data[0U] == '/';
    if (!scratch.Temporary.Add(absolute ? Literal("/") : Text{}))
    {
        return Fail(Error::ExpansionCapacity);
    }
    Size cursor = absolute ? 1U : 0U;
    while (cursor < pattern.Length && Step())
    {
        while (cursor < pattern.Length && pattern.Data[cursor] == '/')
        {
            ++cursor;
        }
        if (cursor == pattern.Length)
        {
            break;
        }
        const Size start = cursor;
        while (cursor < pattern.Length && pattern.Data[cursor] != '/')
        {
            ++cursor;
        }
        const Text component = Slice(pattern, start, cursor - start);
        scratch.Paths.Count = 0U;
        scratch.Paths.Used = 0U;
        for (Size i{0U}; i < scratch.Temporary.Count && Step(); ++i)
        {
            const Text directory = scratch.Temporary.At(i);
            if (Is(component, ".") || Is(component, ".."))
            {
                // The directory callback excludes. and..; pass traversal
                // through unchanged so the host resolves symbolic links.
                Buffer path{scratch.PathBytes, ExpansionLimit};
                if (!path.Append(directory) ||
                    (directory.Length != 0U && directory.Data[directory.Length - 1U] != '/' && !path.Put('/')) ||
                    !path.Append(component))
                {
                    return Fail(Error::ExpansionCapacity);
                }
                FileInfo info{};
                if (m_filesystem.Inspect == nullptr || !m_filesystem.Inspect(m_filesystem.Context, path.View(), info) ||
                    !info.Exists || !info.Directory)
                {
                    continue;
                }
                if (cursor < pattern.Length && cursor + 1U == pattern.Length && !path.Put('/'))
                {
                    return Fail(Error::ExpansionCapacity);
                }
                if (!scratch.Paths.Add(path.View()))
                {
                    return Fail(scratch.Paths.Count >= FieldLimit ? Error::FieldCapacity : Error::ExpansionCapacity);
                }
                continue;
            }
            scratch.Entries.Count = 0U;
            scratch.Entries.Used = 0U;
            if (!m_filesystem.ListDirectory(m_filesystem.Context, directory.Length == 0U ? Literal(".") : directory,
                                            scratch.Entries))
            {
                continue;
            }
            if (scratch.Entries.Count > FieldLimit || scratch.Entries.Used > ExpansionLimit)
            {
                return Fail(Error::Host);
            }
            for (Size j{0U}; j < scratch.Entries.Count && Step(); ++j)
            {
                const Span entrySpan = scratch.Entries.Items[j];
                if (static_cast<Size>(entrySpan.Offset) + entrySpan.Length > scratch.Entries.Used)
                {
                    return Fail(Error::Host);
                }
                const Text entry = scratch.Entries.At(j);
                if (entry.Length == 0U || Is(entry, ".") || Is(entry, "..") || !Validate(entry).Ok())
                {
                    return Fail(Error::Host);
                }
                for (Size k{0U}; k < entry.Length; ++k)
                {
                    if (entry.Data[k] == '/')
                    {
                        return Fail(Error::Host);
                    }
                }
                if (entry.Data[0U] == '.' && component.Data[0U] != '.')
                {
                    continue;
                }
                if (!Match(component, entry, flags + start))
                {
                    continue;
                }
                Buffer path{scratch.PathBytes, ExpansionLimit};
                if (!path.Append(directory) ||
                    (directory.Length != 0U && directory.Data[directory.Length - 1U] != '/' && !path.Put('/')) ||
                    !path.Append(entry))
                {
                    return Fail(Error::ExpansionCapacity);
                }
                if (cursor < pattern.Length && cursor + 1U == pattern.Length)
                {
                    // A trailing slash must name a directory.
                    FileInfo info{};
                    if (m_filesystem.Inspect == nullptr ||
                        !m_filesystem.Inspect(m_filesystem.Context, path.View(), info) || !info.Directory)
                    {
                        continue;
                    }
                    if (!path.Put('/'))
                    {
                        return Fail(Error::ExpansionCapacity);
                    }
                }
                if (!scratch.Paths.Add(path.View()))
                {
                    return Fail(scratch.Paths.Count >= FieldLimit ? Error::FieldCapacity : Error::ExpansionCapacity);
                }
            }
        }
        scratch.Temporary.Count = 0U;
        scratch.Temporary.Used = 0U;
        for (Size i{0U}; i < scratch.Paths.Count; ++i)
        {
            if (!scratch.Temporary.Add(scratch.Paths.At(i)))
            {
                return Fail(Error::ExpansionCapacity);
            }
        }
        if (scratch.Temporary.Count == 0U)
        {
            break;
        }
    }
    return m_error.Ok();
}
bool Engine::Match(const Text pattern, const Text value, const Foundation::UInt8* const quotes) noexcept
{
    if (m_executionDepth == 0U && m_expansionDepth == 0U)
    {
        m_error = {};
        m_steps = 0U;
    }
    if (pattern.Length > ExpansionLimit || value.Length > ExpansionLimit)
    {
        return Fail(Error::InputTooLarge);
    }
    if (!Validate(pattern).Ok() || !Validate(value).Ok())
    {
        return Fail(Error::InvalidUtf8);
    }
    // Single-star retry is linear in value length times pattern length. Each
    // retry consumes the shared work budget (§2.14), avoiding exponential search.
    Size p{0U};
    Size v{0U};
    Size star{0U};
    Size retry{0U};
    bool haveStar{false};
    while (v < value.Length)
    {
        if (!Step())
        {
            return false;
        }
        const bool quoted = p < pattern.Length && quotes != nullptr && (quotes[p] & Quoted) != 0U;
        const bool escaped = p + 1U < pattern.Length && !quoted && pattern.Data[p] == '\\';
        if (p < pattern.Length && !quoted && !escaped && pattern.Data[p] == '*')
        {
            star = ++p;
            retry = v;
            haveStar = true;
            continue;
        }
        bool matched{false};
        Size next = p;
        if (escaped)
        {
            const Size n = ScalarBytes(pattern, p + 1U);
            const Size m = ScalarBytes(value, v);
            matched = n == m && Equal(Slice(pattern, p + 1U, n), Slice(value, v, m));
            next = p + 1U + n;
        }
        else if (p < pattern.Length && !quoted && pattern.Data[p] == '?')
        {
            matched = true;
            next = p + 1U;
        }
        else if (p < pattern.Length && !quoted && pattern.Data[p] == '[')
        {
            Size end = p + 1U;
            const bool negate = end < pattern.Length && pattern.Data[end] == '!';
            if (negate)
            {
                ++end;
            }
            const Size first = end;
            if (end < pattern.Length && pattern.Data[end] == ']')
            {
                ++end;
            }
            while (end < pattern.Length && pattern.Data[end] != ']')
            {
                if (pattern.Data[end] == '[' && end + 1U < pattern.Length && pattern.Data[end + 1U] == ':')
                {
                    Size close = end + 2U;
                    while (close + 1U < pattern.Length &&
                           !(pattern.Data[close] == ':' && pattern.Data[close + 1U] == ']'))
                    {
                        ++close;
                    }
                    if (close + 1U < pattern.Length)
                    {
                        end = close + 2U;
                        continue;
                    }
                }
                ++end;
            }
            if (end < pattern.Length)
            {
                const Size bytes = ScalarBytes(value, v);
                for (Size j = first; j < end;)
                {
                    if (pattern.Data[j] == '[' && j + 1U < end && pattern.Data[j + 1U] == ':')
                    {
                        Size close = j + 2U;
                        while (close + 1U < end && !(pattern.Data[close] == ':' && pattern.Data[close + 1U] == ']'))
                        {
                            ++close;
                        }
                        if (close + 1U < end)
                        {
                            matched = matched || (bytes == 1U && CharacterClass(Slice(pattern, j + 2U, close - j - 2U),
                                                                                value.Data[v]));
                            j = close + 2U;
                            continue;
                        }
                    }
                    const Size n = ScalarBytes(pattern, j);
                    if (j + n + 1U < end && pattern.Data[j + n] == '-' &&
                        (quotes == nullptr || (quotes[j + n] & Quoted) == 0U) && bytes == 1U && n == 1U &&
                        ScalarBytes(pattern, j + n + 1U) == 1U)
                    {
                        matched = matched || (static_cast<Foundation::UInt8>(value.Data[v]) >=
                                                  static_cast<Foundation::UInt8>(pattern.Data[j]) &&
                                              static_cast<Foundation::UInt8>(value.Data[v]) <=
                                                  static_cast<Foundation::UInt8>(pattern.Data[j + 2U]));
                        j += 3U;
                    }
                    else
                    {
                        matched = matched || (n == bytes && Equal(Slice(pattern, j, n), Slice(value, v, bytes)));
                        j += n;
                    }
                }
                if (negate)
                {
                    matched = !matched;
                }
                next = end + 1U;
            }
            else
            {
                matched = value.Data[v] == '[';
                next = p + 1U;
            }
        }
        else if (p < pattern.Length)
        {
            const Size n = ScalarBytes(pattern, p);
            const Size m = ScalarBytes(value, v);
            matched = n == m && Equal(Slice(pattern, p, n), Slice(value, v, m));
            next = p + n;
        }
        if (matched)
        {
            p = next;
            v += ScalarBytes(value, v);
        }
        else if (haveStar && retry < value.Length)
        {
            retry += ScalarBytes(value, retry);
            v = retry;
            p = star;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.Length && pattern.Data[p] == '*' && (quotes == nullptr || (quotes[p] & Quoted) == 0U))
    {
        ++p;
    }
    return p == pattern.Length;
}
} // Namespace Asteria::User::Shell
