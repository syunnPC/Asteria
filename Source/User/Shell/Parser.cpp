// SPDX-License-Identifier: Apache-2.0

#include "User/Shell/Internal.hpp"
namespace Asteria::User::Shell
{
namespace
{
using Detail::Decimal;
using Detail::Is;
using Detail::MakeSpan;
using Detail::Name;
using Detail::Slice;
using Detail::Tokenize;
using Detail::Validate;
class Parser final
{
public:
    explicit Parser(Program& program) noexcept : m_program{program}
    {
    }
    [[nodiscard]] Result Run() noexcept
    {
        Lines();
        m_program.Root = List(true);
        if (m_error.Ok() && Kind() != TokenKind::End)
        {
            Fail(Error::Syntax);
        }
        return m_error;
    }

private:
    Program& m_program;
    Size m_cursor{0U};
    Size m_depth{0U};
    Result m_error{};
    [[nodiscard]] TokenKind Kind(Size look = 0U) const noexcept
    {
        return m_cursor + look <= m_program.TokenCount ? m_program.Tokens[m_cursor + look].Kind : TokenKind::End;
    }
    [[nodiscard]] Text WordText(Size look = 0U) const noexcept
    {
        return m_cursor + look < m_program.TokenCount ? m_program.View(m_program.Tokens[m_cursor + look].Spelling)
                                                      : Text{};
    }
    [[nodiscard]] bool Keyword(const char* word) const noexcept
    {
        return Kind() == TokenKind::Word && Is(WordText(), word);
    }
    void Fail(Error error) noexcept
    {
        if (m_error.Ok())
        {
            m_error = {error, m_program.Tokens[m_cursor].Original.Offset, 2};
        }
    }
    [[nodiscard]] bool Take(TokenKind kind) noexcept
    {
        if (Kind() != kind)
        {
            return false;
        }
        ++m_cursor;
        return true;
    }
    [[nodiscard]] bool TakeKeyword(const char* word) noexcept
    {
        if (!Keyword(word))
        {
            return false;
        }
        ++m_cursor;
        return true;
    }
    void Require(TokenKind kind) noexcept
    {
        if (!Take(kind))
        {
            Fail(Error::Syntax);
        }
    }
    void RequireKeyword(const char* word) noexcept
    {
        if (!TakeKeyword(word))
        {
            Fail(Error::Syntax);
        }
    }
    void Lines() noexcept
    {
        while (Take(TokenKind::Newline))
        {
        }
    }
    [[nodiscard]] bool Stop() const noexcept
    {
        return Kind() == TokenKind::End || Kind() == TokenKind::RightParen || Kind() == TokenKind::DoubleSemi ||
               Kind() == TokenKind::SemiAnd || Keyword("}") || Keyword("then") || Keyword("else") || Keyword("elif") ||
               Keyword("fi") || Keyword("do") || Keyword("done") || Keyword("esac");
    }
    [[nodiscard]] UInt16 New(NodeKind kind) noexcept
    {
        if (m_program.NodeCount >= NodeLimit)
        {
            Fail(Error::NodeCapacity);
            return 0U;
        }
        const UInt16 index = ++m_program.NodeCount;
        m_program.Nodes[index] = {};
        m_program.Nodes[index].Kind = kind;
        return index;
    }
    [[nodiscard]] UInt16 AddWord(UInt16& first, bool assignment = false) noexcept
    {
        if (Kind() != TokenKind::Word)
        {
            Fail(Error::Syntax);
            return 0U;
        }
        if (m_program.WordCount >= WordLimit)
        {
            Fail(Error::WordCapacity);
            return 0U;
        }
        const UInt16 index = ++m_program.WordCount;
        m_program.Words[index] = {m_program.Tokens[m_cursor++].Spelling, 0U, assignment};
        if (first == 0U)
        {
            first = index;
        }
        else
        {
            UInt16 last = first;
            for (Size i{0U}; i < WordLimit && m_program.Words[last].Next != 0U; ++i)
            {
                last = m_program.Words[last].Next;
            }
            m_program.Words[last].Next = index;
        }
        return index;
    }
    [[nodiscard]] UInt16 List(bool empty = false) noexcept
    {
        Lines();
        if (Stop())
        {
            if (!empty)
            {
                Fail(Error::Syntax);
            }
            return 0U;
        }
        const UInt16 root = New(NodeKind::List);
        UInt16 tail{0U};
        while (m_error.Ok() && !Stop())
        {
            UInt16 entry = AndOr();
            if (Take(TokenKind::Amp))
            {
                const UInt16 background = New(NodeKind::Background);
                m_program.Nodes[background].Left = entry;
                entry = background;
            }
            else if (Take(TokenKind::Semi))
            {
            }
            else if (Kind() == TokenKind::Newline)
            {
            }
            else
            {
                if (tail == 0U)
                {
                    m_program.Nodes[root].Left = entry;
                }
                else
                {
                    m_program.Nodes[tail].Next = entry;
                }
                break;
            }
            if (tail == 0U)
            {
                m_program.Nodes[root].Left = entry;
            }
            else
            {
                m_program.Nodes[tail].Next = entry;
            }
            tail = entry;
            Lines();
        }
        return root;
    }
    [[nodiscard]] UInt16 AndOr() noexcept
    {
        UInt16 left = Pipeline();
        Size operators{0U};
        while (m_error.Ok() && (Kind() == TokenKind::And || Kind() == TokenKind::Or))
        {
            if (++operators >= DepthLimit)
            {
                Fail(Error::Depth);
                break;
            }
            const NodeKind kind = Take(TokenKind::And) ? NodeKind::And : NodeKind::Or;
            if (kind == NodeKind::Or)
            {
                ++m_cursor;
            }
            Lines();
            const UInt16 right = Pipeline();
            const UInt16 node = New(kind);
            m_program.Nodes[node].Left = left;
            m_program.Nodes[node].Right = right;
            left = node;
        }
        return left;
    }
    [[nodiscard]] UInt16 Pipeline() noexcept
    {
        const bool negate = TakeKeyword("!");
        UInt16 left = Command();
        if (Kind() == TokenKind::Pipe)
        {
            const UInt16 root = New(NodeKind::Pipeline);
            m_program.Nodes[root].Left = left;
            Size count{1U};
            while (m_error.Ok() && Take(TokenKind::Pipe))
            {
                if (++count > FieldLimit)
                {
                    Fail(Error::NodeCapacity);
                    break;
                }
                Lines();
                const UInt16 next = Command();
                m_program.Nodes[left].Next = next;
                left = next;
            }
            left = root;
        }
        if (negate)
        {
            const UInt16 root = New(NodeKind::Negate);
            m_program.Nodes[root].Left = left;
            left = root;
        }
        return left;
    }
    [[nodiscard]] bool IsRedirect(TokenKind kind) const noexcept
    {
        return kind >= TokenKind::Less && kind <= TokenKind::Clobber;
    }
    [[nodiscard]] bool Redirection(UInt16 node) noexcept
    {
        UInt64 descriptor{0U};
        bool explicitDescriptor{false};
        if (Kind() == TokenKind::Word && IsRedirect(Kind(1U)) &&
            m_program.Tokens[m_cursor].Original.Offset + m_program.Tokens[m_cursor].Original.Length ==
                m_program.Tokens[m_cursor + 1U].Original.Offset &&
            Decimal(WordText(), descriptor))
        {
            explicitDescriptor = true;
            if (descriptor > 65535U)
            {
                Fail(Error::Syntax);
                return false;
            }
            ++m_cursor;
        }
        if (!IsRedirect(Kind()))
        {
            return false;
        }
        const TokenKind kind = Kind();
        ++m_cursor;
        if (Kind() != TokenKind::Word)
        {
            Fail(Error::Syntax);
            return false;
        }
        if (m_program.RedirectCount >= RedirectLimit)
        {
            Fail(Error::RedirectCapacity);
            return false;
        }
        if (!explicitDescriptor)
        {
            descriptor = kind == TokenKind::Less || kind == TokenKind::Here || kind == TokenKind::HereStrip ||
                                 kind == TokenKind::LessAnd || kind == TokenKind::ReadWrite
                             ? 0U
                             : 1U;
        }
        const Token& target = m_program.Tokens[m_cursor++];
        const UInt16 index = ++m_program.RedirectCount;
        m_program.Redirects[index] = {kind, static_cast<UInt16>(descriptor), target.Spelling, target.Body,
                                      0U,   target.QuotedDelimiter};
        UInt16& first = m_program.Nodes[node].Redirects;
        if (first == 0U)
        {
            first = index;
        }
        else
        {
            UInt16 last = first;
            for (Size i{0U}; i < RedirectLimit && m_program.Redirects[last].Next != 0U; ++i)
            {
                last = m_program.Redirects[last].Next;
            }
            m_program.Redirects[last].Next = index;
        }
        return true;
    }
    [[nodiscard]] bool Assignment(Text text) const noexcept
    {
        Size i{0U};
        while (i < text.Length && text.Data[i] != '=')
        {
            ++i;
        }
        return i < text.Length && Name(Slice(text, 0U, i));
    }
    [[nodiscard]] UInt16 If() noexcept
    {
        const UInt16 root = New(NodeKind::If);
        m_program.Nodes[root].Left = List();
        RequireKeyword("then");
        m_program.Nodes[root].Right = List();
        if (TakeKeyword("elif"))
        {
            if (m_depth >= DepthLimit)
            {
                Fail(Error::Depth);
            }
            else
            {
                ++m_depth;
                m_program.Nodes[root].Third = If();
                --m_depth;
            }
        }
        else if (TakeKeyword("else"))
        {
            m_program.Nodes[root].Third = List();
        }
        return root;
    }
    [[nodiscard]] UInt16 For() noexcept
    {
        const UInt16 root = New(NodeKind::For);
        if (!Name(WordText()))
        {
            Fail(Error::InvalidName);
            return root;
        }
        static_cast<void>(AddWord(m_program.Nodes[root].Words));
        Lines();
        if (TakeKeyword("in"))
        {
            while (m_error.Ok() && Kind() == TokenKind::Word)
            {
                static_cast<void>(AddWord(m_program.Nodes[root].Words));
            }
            if (!Take(TokenKind::Semi) && !Take(TokenKind::Newline))
            {
                Fail(Error::Syntax);
            }
        }
        else
        {
            m_program.Nodes[root].ImplicitParameters = true;
            static_cast<void>(Take(TokenKind::Semi));
        }
        Lines();
        RequireKeyword("do");
        m_program.Nodes[root].Left = List();
        RequireKeyword("done");
        return root;
    }
    [[nodiscard]] UInt16 Case() noexcept
    {
        const UInt16 root = New(NodeKind::Case);
        static_cast<void>(AddWord(m_program.Nodes[root].Words));
        Lines();
        RequireKeyword("in");
        Lines();
        UInt16 tail{0U};
        while (m_error.Ok() && !Keyword("esac") && Kind() != TokenKind::End)
        {
            static_cast<void>(Take(TokenKind::LeftParen));
            const UInt16 item = New(NodeKind::CaseItem);
            static_cast<void>(AddWord(m_program.Nodes[item].Words));
            while (Take(TokenKind::Pipe) && m_error.Ok())
            {
                static_cast<void>(AddWord(m_program.Nodes[item].Words));
            }
            Require(TokenKind::RightParen);
            m_program.Nodes[item].Left = List(true);
            if (tail == 0U)
            {
                m_program.Nodes[root].Left = item;
            }
            else
            {
                m_program.Nodes[tail].Next = item;
            }
            tail = item;
            if (Take(TokenKind::SemiAnd))
            {
                m_program.Nodes[item].ImplicitParameters = true;
            }
            else if (!Take(TokenKind::DoubleSemi))
            {
                break;
            }
            Lines();
        }
        RequireKeyword("esac");
        return root;
    }
    [[nodiscard]] UInt16 Command() noexcept
    {
        if (!m_error.Ok())
        {
            return 0U;
        }
        if (m_depth >= DepthLimit)
        {
            Fail(Error::Depth);
            return 0U;
        }
        ++m_depth;
        const Size start = m_program.Tokens[m_cursor].Original.Offset;
        UInt16 root{0U};
        if (Take(TokenKind::LeftParen))
        {
            root = New(NodeKind::Subshell);
            m_program.Nodes[root].Left = List();
            Require(TokenKind::RightParen);
        }
        else if (TakeKeyword("{"))
        {
            root = New(NodeKind::Group);
            m_program.Nodes[root].Left = List();
            RequireKeyword("}");
        }
        else if (TakeKeyword("if"))
        {
            root = If();
            RequireKeyword("fi");
        }
        else if (Keyword("while") || Keyword("until"))
        {
            const bool until = TakeKeyword("until");
            if (!until)
            {
                ++m_cursor;
            }
            root = New(until ? NodeKind::Until : NodeKind::While);
            m_program.Nodes[root].Left = List();
            RequireKeyword("do");
            m_program.Nodes[root].Right = List();
            RequireKeyword("done");
        }
        else if (TakeKeyword("for"))
        {
            root = For();
        }
        else if (TakeKeyword("case"))
        {
            root = Case();
        }
        else if (Kind() == TokenKind::Word && Name(WordText()) && Kind(1U) == TokenKind::LeftParen &&
                 Kind(2U) == TokenKind::RightParen)
        {
            root = New(NodeKind::Function);
            static_cast<void>(AddWord(m_program.Nodes[root].Words));
            m_cursor += 2U;
            Lines();
            m_program.Nodes[root].Left = Command();
            if (m_program.Nodes[m_program.Nodes[root].Left].Kind == NodeKind::Simple ||
                m_program.Nodes[m_program.Nodes[root].Left].Kind == NodeKind::Function)
            {
                Fail(Error::Syntax);
            }
        }
        else
        {
            if (Stop() || Keyword("!") || Keyword("in"))
            {
                Fail(Error::Syntax);
                --m_depth;
                return 0U;
            }
            root = New(NodeKind::Simple);
            bool command{false};
            bool any{false};
            while (m_error.Ok())
            {
                if (Redirection(root))
                {
                    any = true;
                    continue;
                }
                if (Kind() != TokenKind::Word)
                {
                    break;
                }
                const bool assignment = !command && Assignment(WordText());
                if (!assignment)
                {
                    command = true;
                }
                static_cast<void>(AddWord(m_program.Nodes[root].Words, assignment));
                any = true;
            }
            if (!any)
            {
                Fail(Error::Syntax);
            }
        }
        if (root != 0U && m_program.Nodes[root].Kind != NodeKind::Function)
        {
            while (m_error.Ok() && Redirection(root))
            {
            }
        }
        Size end = m_cursor == 0U ? start
                                  : m_program.Tokens[m_cursor - 1U].Original.Offset +
                                        m_program.Tokens[m_cursor - 1U].Original.Length;
        // A function's trailing here-documents are read after its syntactic
        // delimiter token. Keep their terminator lines in the delayed body.
        for (UInt16 r = m_program.Nodes[root].Redirects; r != 0U; r = m_program.Redirects[r].Next)
        {
            const Redirect& redirect = m_program.Redirects[r];
            if (redirect.Kind != TokenKind::Here && redirect.Kind != TokenKind::HereStrip)
            {
                continue;
            }
            Size bodyEnd = static_cast<Size>(redirect.Body.Offset) + redirect.Body.Length;
            while (bodyEnd < m_program.Length && m_program.Source[bodyEnd] != '\n')
            {
                ++bodyEnd;
            }
            if (bodyEnd < m_program.Length)
            {
                ++bodyEnd;
            }
            if (bodyEnd > end)
            {
                end = bodyEnd;
            }
        }
        m_program.Nodes[root].Source = MakeSpan(start, end - start);
        --m_depth;
        return root;
    }
};
} // namespace
Result Parse(const Text source, Program& output) noexcept
{
    output.Length = 0U;
    output.StorageLength = 0U;
    output.TokenCount = 0U;
    output.NodeCount = 0U;
    output.WordCount = 0U;
    output.RedirectCount = 0U;
    output.Root = 0U;
    output.Diagnostic = {};
    if (source.Length > InputLimit)
    {
        output.Diagnostic = {Error::InputTooLarge, InputLimit, 2};
        return output.Diagnostic;
    }
    output.Diagnostic = Detail::Validate(source);
    if (!output.Diagnostic.Ok())
    {
        return output.Diagnostic;
    }
    for (Size i{0U}; i < source.Length; ++i)
    {
        output.Source[i] = source.Data[i];
    }
    output.Source[source.Length] = '\0';
    output.Length = source.Length;
    output.StorageLength = source.Length;
    output.Diagnostic = Detail::Tokenize(output);
    if (output.Diagnostic.Ok())
    {
        Parser parser{output};
        output.Diagnostic = parser.Run();
    }
    return output.Diagnostic;
}
} // namespace Asteria::User::Shell
