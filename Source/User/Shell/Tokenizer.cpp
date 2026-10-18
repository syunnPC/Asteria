// SPDX-License-Identifier: Apache-2.0

#include "User/Shell/Internal.hpp"
namespace Asteria::User::Shell::Detail
{
namespace
{
[[nodiscard]] bool CommandEnd(Text text, Size& cursor, Size depth, Result& error) noexcept;
[[nodiscard]] bool HereBody(Text text, Size& cursor, Text raw, bool strip, Span& body, bool& quoted,
                            Result& error) noexcept;
} // namespace
bool Unit(const Text text, Size& cursor, const Size depth, Result& error) noexcept
{
    const Size start = cursor;
    if (depth >= DepthLimit)
    {
        error = {Error::Depth, start, 2};
        return false;
    }
    char end{0};
    bool expansion{false};
    bool single{false};
    bool dollarSingle{false};
    if (text.Data[cursor] == '\\')
    {
        if (cursor + 1U >= text.Length)
        {
            error = {Error::UnterminatedQuote, cursor, 2};
            return false;
        }
        cursor += 2U;
        return true;
    }
    if (text.Data[cursor] == '$' && cursor + 1U < text.Length && text.Data[cursor + 1U] == '\'')
    {
        dollarSingle = true;
        single = true;
        end = '\'';
        cursor += 2U;
    }
    else if (text.Data[cursor] == '\'' || text.Data[cursor] == '"' || text.Data[cursor] == '`')
    {
        end = text.Data[cursor++];
        single = end == '\'';
    }
    else if (text.Data[cursor] == '$' && cursor + 1U < text.Length &&
             (text.Data[cursor + 1U] == '(' || text.Data[cursor + 1U] == '{'))
    {
        end = text.Data[cursor + 1U] == '(' ? ')' : '}';
        expansion = true;
        cursor += 2U;
        if (end == ')' && (cursor >= text.Length || text.Data[cursor] != '('))
        {
            return CommandEnd(text, cursor, depth, error);
        }
    }
    else
    {
        ++cursor;
        return true;
    }
    Size parentheses{0U};
    for (; cursor < text.Length;)
    {
        const char c = text.Data[cursor];
        if (c == end && parentheses == 0U)
        {
            ++cursor;
            return true;
        }
        if (single)
        {
            if (dollarSingle && c == '\\')
            {
                if (!Unit(text, cursor, depth + 1U, error))
                {
                    return false;
                }
            }
            else
            {
                ++cursor;
            }
            continue;
        }
        if (end == '`')
        {
            if (c == '\\')
            {
                if (!Unit(text, cursor, depth + 1U, error))
                {
                    return false;
                }
            }
            else
            {
                ++cursor;
            }
            continue;
        }
        if (expansion && end == ')' && c == '(')
        {
            ++parentheses;
            if (parentheses >= DepthLimit)
            {
                error = {Error::Depth, cursor, 2};
                return false;
            }
            ++cursor;
            continue;
        }
        if (expansion && end == ')' && c == ')' && parentheses != 0U)
        {
            --parentheses;
            ++cursor;
            continue;
        }
        if (c == '\\' || c == '`' || (expansion && (c == '\'' || c == '"')) ||
            (c == '$' && cursor + 1U < text.Length && (text.Data[cursor + 1U] == '(' || text.Data[cursor + 1U] == '{')))
        {
            if (!Unit(text, cursor, depth + 1U, error))
            {
                return false;
            }
        }
        else
        {
            ++cursor;
        }
    }
    error = {expansion ? Error::UnterminatedExpansion : Error::UnterminatedQuote, start, 2};
    return false;
}
namespace
{
[[nodiscard]] TokenKind Operator(const Text text, Size& cursor) noexcept
{
    const char c = text.Data[cursor++];
    const char next = cursor < text.Length ? text.Data[cursor] : '\0';
    if (c == '\n')
    {
        return TokenKind::Newline;
    }
    if (c == '(')
    {
        return TokenKind::LeftParen;
    }
    if (c == ')')
    {
        return TokenKind::RightParen;
    }
    if (c == ';')
    {
        if (next == ';')
        {
            ++cursor;
            return TokenKind::DoubleSemi;
        }
        if (next == '&')
        {
            ++cursor;
            return TokenKind::SemiAnd;
        }
        return TokenKind::Semi;
    }
    if (c == '&')
    {
        if (next == '&')
        {
            ++cursor;
            return TokenKind::And;
        }
        return TokenKind::Amp;
    }
    if (c == '|')
    {
        if (next == '|')
        {
            ++cursor;
            return TokenKind::Or;
        }
        return TokenKind::Pipe;
    }
    if (c == '<')
    {
        if (next == '<')
        {
            ++cursor;
            if (cursor < text.Length && text.Data[cursor] == '-')
            {
                ++cursor;
                return TokenKind::HereStrip;
            }
            return TokenKind::Here;
        }
        if (next == '&')
        {
            ++cursor;
            return TokenKind::LessAnd;
        }
        if (next == '>')
        {
            ++cursor;
            return TokenKind::ReadWrite;
        }
        return TokenKind::Less;
    }
    if (c == '>')
    {
        if (next == '>')
        {
            ++cursor;
            return TokenKind::Append;
        }
        if (next == '&')
        {
            ++cursor;
            return TokenKind::GreatAnd;
        }
        if (next == '|')
        {
            ++cursor;
            return TokenKind::Clobber;
        }
        return TokenKind::Great;
    }
    --cursor;
    return TokenKind::Word;
}
[[nodiscard]] bool Delimiter(const Text text, Buffer& output, bool& quoted) noexcept
{
    char quote{0};
    bool dollar{false};
    quoted = false;
    for (Size i{0U}; i < text.Length; ++i)
    {
        char c = text.Data[i];
        if (c == '$' && quote == 0 && i + 1U < text.Length && text.Data[i + 1U] == '\'')
        {
            dollar = true;
            quote = '\'';
            quoted = true;
            ++i;
            continue;
        }
        if (dollar && c == '\\')
        {
            Size next = i + 1U;
            Result error{};
            if (!DollarByte(text, next, c, error))
            {
                return false;
            }
            i = next - 1U;
            if (!output.Put(c))
            {
                return false;
            }
            continue;
        }
        if ((c == '\'' || c == '"') && quote == 0)
        {
            quote = c;
            quoted = true;
            continue;
        }
        if (c == quote)
        {
            quote = 0;
            dollar = false;
            continue;
        }
        if (c == '\\' && quote != '\'' && i + 1U < text.Length)
        {
            const char next = text.Data[i + 1U];
            if (quote == 0 || next == '$' || next == '`' || next == '\\' || next == '"' || next == '\n')
            {
                quoted = true;
                ++i;
                if (next != '\n' && !output.Put(next))
                {
                    return false;
                }
                continue;
            }
        }
        if (!output.Put(c))
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] bool HereBody(const Text text, Size& cursor, const Text raw, const bool strip, Span& body, bool& quoted,
                            Result& error) noexcept
{
    char bytes[NameLimit]{};
    Buffer delimiter{bytes, NameLimit};
    if (!Delimiter(raw, delimiter, quoted))
    {
        error = {Error::ValueCapacity, cursor, 2};
        return false;
    }
    const Size bodyStart = cursor;
    while (cursor <= text.Length)
    {
        const Size lineStart = cursor;
        Size length{0U};
        bool equal{true};
        while (cursor < text.Length && text.Data[cursor] != '\n')
        {
            if (!quoted && text.Data[cursor] == '\\' && cursor + 1U < text.Length && text.Data[cursor + 1U] == '\n')
            {
                cursor += 2U;
                continue;
            }
            if (!quoted && text.Data[cursor] == '\\' && cursor + 1U < text.Length && text.Data[cursor + 1U] == '\\')
            {
                for (Size j{0U}; j < 2U; ++j)
                {
                    const char byte = text.Data[cursor++];
                    if (length >= delimiter.Length || delimiter.Data[length] != byte)
                    {
                        equal = false;
                    }
                    ++length;
                }
                continue;
            }
            const char c = text.Data[cursor++];
            if (strip && length == 0U && c == '\t')
            {
                continue;
            }
            if (length >= delimiter.Length || delimiter.Data[length] != c)
            {
                equal = false;
            }
            ++length;
        }
        if (equal && length == delimiter.Length)
        {
            body = MakeSpan(bodyStart, lineStart - bodyStart);
            if (cursor < text.Length)
            {
                ++cursor;
            }
            return true;
        }
        if (cursor == text.Length)
        {
            break;
        }
        ++cursor;
    }
    error = {Error::MissingHereDocument, bodyStart, 2};
    return false;
}
[[nodiscard]] bool CommandEnd(const Text text, Size& cursor, const Size depth, Result& error) noexcept
{
    Span delimiters[CommandRedirectLimit]{};
    bool strip[CommandRedirectLimit]{};
    Size pending{0U};
    Size parentheses{0U};
    Foundation::UInt8 cases[DepthLimit]{};
    Size caseDepth{0U};
    bool commandStart{true};
    while (cursor < text.Length)
    {
        if (Blank(text.Data[cursor]))
        {
            ++cursor;
            continue;
        }
        if (text.Data[cursor] == '#')
        {
            while (cursor < text.Length && text.Data[cursor] != '\n')
            {
                ++cursor;
            }
            continue;
        }
        if (text.Data[cursor] == '\n')
        {
            ++cursor;
            commandStart = true;
            for (Size j{0U}; j < pending; ++j)
            {
                Span body{};
                bool quoted{false};
                if (!HereBody(text, cursor, Slice(text, delimiters[j].Offset, delimiters[j].Length), strip[j], body,
                              quoted, error))
                {
                    return false;
                }
            }
            pending = 0U;
            continue;
        }
        if (text.Data[cursor] == ')')
        {
            if (caseDepth != 0U && cases[caseDepth - 1U] == 2U)
            {
                cases[caseDepth - 1U] = 3U;
                ++cursor;
                commandStart = true;
                continue;
            }
            if (parentheses == 0U)
            {
                ++cursor;
                return true;
            }
            --parentheses;
            ++cursor;
            commandStart = false;
            continue;
        }
        if (text.Data[cursor] == '(')
        {
            ++cursor;
            if (!(caseDepth != 0U && cases[caseDepth - 1U] == 2U))
            {
                ++parentheses;
            }
            if (parentheses + depth >= DepthLimit)
            {
                error = {Error::Depth, cursor, 2};
                return false;
            }
            commandStart = true;
            continue;
        }
        if (text.Data[cursor] == '<' && cursor + 1U < text.Length && text.Data[cursor + 1U] == '<')
        {
            if (pending >= CommandRedirectLimit)
            {
                error = {Error::RedirectCapacity, cursor, 2};
                return false;
            }
            cursor += 2U;
            strip[pending] = cursor < text.Length && text.Data[cursor] == '-';
            if (strip[pending])
            {
                ++cursor;
            }
            while (cursor < text.Length && Blank(text.Data[cursor]))
            {
                ++cursor;
            }
            const Size start = cursor;
            while (cursor < text.Length && !Space(text.Data[cursor]) && text.Data[cursor] != ';' &&
                   text.Data[cursor] != '&' && text.Data[cursor] != '|' && text.Data[cursor] != ')')
            {
                if (!Unit(text, cursor, depth + 1U, error))
                {
                    return false;
                }
            }
            if (cursor == start)
            {
                error = {Error::Syntax, cursor, 2};
                return false;
            }
            delimiters[pending++] = MakeSpan(start, cursor - start);
            continue;
        }
        if (text.Data[cursor] == ';' || text.Data[cursor] == '&' || text.Data[cursor] == '|')
        {
            const char c = text.Data[cursor++];
            if (c == ';' && cursor < text.Length && (text.Data[cursor] == ';' || text.Data[cursor] == '&'))
            {
                ++cursor;
                if (caseDepth != 0U)
                {
                    cases[caseDepth - 1U] = 2U;
                }
            }
            commandStart = true;
            continue;
        }
        const Size start = cursor;
        while (cursor < text.Length && !Space(text.Data[cursor]) && text.Data[cursor] != ';' &&
               text.Data[cursor] != '&' && text.Data[cursor] != '|' && text.Data[cursor] != '(' &&
               text.Data[cursor] != ')')
        {
            if (!Unit(text, cursor, depth + 1U, error))
            {
                return false;
            }
        }
        const Text word = Slice(text, start, cursor - start);
        if (Is(word, "case") && commandStart)
        {
            if (caseDepth >= DepthLimit)
            {
                error = {Error::Depth, start, 2};
                return false;
            }
            cases[caseDepth++] = 1U;
        }
        else if (caseDepth != 0U && cases[caseDepth - 1U] == 1U && Is(word, "in"))
        {
            cases[caseDepth - 1U] = 2U;
        }
        else if (caseDepth != 0U && Is(word, "esac") && (commandStart || cases[caseDepth - 1U] == 2U))
        {
            --caseDepth;
        }
        commandStart = Is(word, "if") || Is(word, "while") || Is(word, "until") || Is(word, "!") || Is(word, "then") ||
                       Is(word, "do") || Is(word, "else") || Is(word, "elif") || Is(word, "{");
    }
    error = {Error::UnterminatedExpansion, cursor, 2};
    return false;
}
} // namespace
Result Tokenize(Program& program) noexcept
{
    const Text text{program.Source, program.Length};
    Size cursor{0U};
    UInt16 pending[CommandRedirectLimit]{};
    Size pendingCount{0U};
    bool needDelimiter{false};
    bool strip{false};
    while (cursor < text.Length)
    {
        if (Blank(text.Data[cursor]))
        {
            ++cursor;
            continue;
        }
        if (text.Data[cursor] == '\\' && cursor + 1U < text.Length && text.Data[cursor + 1U] == '\n')
        {
            cursor += 2U;
            continue;
        }
        if (text.Data[cursor] == '#')
        {
            while (cursor < text.Length && text.Data[cursor] != '\n')
            {
                ++cursor;
            }
            continue;
        }
        if (program.TokenCount >= TokenLimit)
        {
            return {Error::TokenCapacity, cursor, 2};
        }
        const Size start = cursor;
        const TokenKind kind = Operator(text, cursor);
        if (kind == TokenKind::Word)
        {
            Result error{};
            while (cursor < text.Length)
            {
                const char c = text.Data[cursor];
                if (Space(c) || c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>')
                {
                    break;
                }
                if (c == '\'' || c == '"' || c == '\\' || c == '`' ||
                    (c == '$' && cursor + 1U < text.Length &&
                     (text.Data[cursor + 1U] == '(' || text.Data[cursor + 1U] == '{' ||
                      text.Data[cursor + 1U] == '\'')))
                {
                    if (!Unit(text, cursor, 0U, error))
                    {
                        return error;
                    }
                }
                else
                {
                    ++cursor;
                }
            }
        }
        Token& token = program.Tokens[program.TokenCount++];
        token = {kind, MakeSpan(start, cursor - start), {}, MakeSpan(start, cursor - start), false};
        if (needDelimiter)
        {
            if (kind != TokenKind::Word)
            {
                return {Error::Syntax, start, 2};
            }
            if (pendingCount >= CommandRedirectLimit)
            {
                return {Error::RedirectCapacity, start, 2};
            }
            pending[pendingCount++] = static_cast<UInt16>(program.TokenCount - 1U);
            token.Body.Offset = static_cast<UInt16>(strip ? 1U : 0U);
            needDelimiter = false;
        }
        if (kind == TokenKind::Here || kind == TokenKind::HereStrip)
        {
            needDelimiter = true;
            strip = kind == TokenKind::HereStrip;
        }
        if (kind == TokenKind::Newline && pendingCount != 0U)
        {
            // The small delimiter buffer remains below the target stack limit.
            for (Size index{0U}; index < pendingCount; ++index)
            {
                Token& delimiterToken = program.Tokens[pending[index]];
                const bool stripTabs = delimiterToken.Body.Offset != 0U;
                Result error{};
                if (!HereBody(text, cursor, program.View(delimiterToken.Spelling), stripTabs, delimiterToken.Body,
                              delimiterToken.QuotedDelimiter, error))
                {
                    return error;
                }
            }
            pendingCount = 0U;
        }
    }
    if (needDelimiter)
    {
        return {Error::Syntax, cursor, 2};
    }
    if (pendingCount != 0U)
    {
        return {Error::MissingHereDocument, cursor, 2};
    }
    program.Tokens[program.TokenCount] = {TokenKind::End, MakeSpan(cursor, 0U), {}, MakeSpan(cursor, 0U), false};
    // Remove continuations from the outer lexical word before grammar
    // classification. Quoted units retain their spelling for expansion. The
    // original input remains available for here bodies and function source.
    for (Size index{0U}; index < program.TokenCount; ++index)
    {
        Token& token = program.Tokens[index];
        if (token.Kind != TokenKind::Word)
        {
            continue;
        }
        const Text raw = program.View(token.Spelling);
        bool continued{false};
        for (Size i{0U}; i + 1U < raw.Length; ++i)
        {
            continued = continued || (raw.Data[i] == '\\' && raw.Data[i + 1U] == '\n');
        }
        if (!continued)
        {
            continue;
        }
        const Size start = program.StorageLength;
        Buffer canonical{program.Source + start, SourceStorageLimit - start};
        for (Size i{0U}; i < raw.Length;)
        {
            if (raw.Data[i] == '\\' && i + 1U < raw.Length && raw.Data[i + 1U] == '\n')
            {
                i += 2U;
                continue;
            }
            Size end = i;
            Result error{};
            if (!Unit(raw, end, 0U, error))
            {
                return error;
            }
            if (!canonical.Append(Slice(raw, i, end - i)))
            {
                return {Error::InputTooLarge, token.Original.Offset, 2};
            }
            i = end;
        }
        program.StorageLength += canonical.Length;
        token.Spelling = MakeSpan(start, canonical.Length);
    }
    return {};
}
} // namespace Asteria::User::Shell::Detail
