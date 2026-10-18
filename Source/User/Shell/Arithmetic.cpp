// SPDX-License-Identifier: Apache-2.0

#include "User/Shell/Internal.hpp"
namespace Asteria::User::Shell
{
namespace
{
using Detail::Integer;
using Detail::Is;
using Detail::Name;
using Detail::NameByte;
using Detail::NameStart;
using Detail::Number;
using Detail::Slice;
using Detail::Space;
struct OperatorInfo
{
    const char* Name;
    unsigned Precedence;
    bool Assignment;
};
constexpr OperatorInfo Operators[]{{"<<=", 2U, true},  {">>=", 2U, true},  {"+=", 2U, true},   {"-=", 2U, true},
                                   {"*=", 2U, true},   {"/=", 2U, true},   {"%=", 2U, true},   {"&=", 2U, true},
                                   {"^=", 2U, true},   {"|=", 2U, true},   {"||", 4U, false},  {"&&", 5U, false},
                                   {"==", 9U, false},  {"!=", 9U, false},  {"<=", 10U, false}, {">=", 10U, false},
                                   {"<<", 11U, false}, {">>", 11U, false}, {",", 1U, false},   {"=", 2U, true},
                                   {"?", 3U, false},   {"|", 6U, false},   {"^", 7U, false},   {"&", 8U, false},
                                   {"<", 10U, false},  {">", 10U, false},  {"+", 12U, false},  {"-", 12U, false},
                                   {"*", 13U, false},  {"/", 13U, false},  {"%", 13U, false}};
struct ArithmeticValue
{
    UInt64 Value{0U};
    Text Name{};
};
class ArithmeticParser final
{
public:
    ArithmeticParser(Engine& engine, Text text) noexcept : m_engine{engine}, m_text{text}
    {
    }
    [[nodiscard]] bool Run(Int64& value) noexcept
    {
        const auto result = Expression(1U, true);
        Spaces();
        value = static_cast<Int64>(result.Value);
        return m_valid && m_cursor == m_text.Length;
    }

private:
    Engine& m_engine;
    Text m_text;
    Size m_cursor{0U};
    Size m_depth{0U};
    bool m_valid{true};
    void Spaces() noexcept
    {
        while (m_cursor < m_text.Length && Space(m_text.Data[m_cursor]))
        {
            ++m_cursor;
        }
    }
    [[nodiscard]] bool Consume(char c) noexcept
    {
        Spaces();
        if (m_cursor < m_text.Length && m_text.Data[m_cursor] == c)
        {
            ++m_cursor;
            return true;
        }
        return false;
    }
    [[nodiscard]] ArithmeticValue Primary(bool active) noexcept
    {
        Spaces();
        if (m_cursor == m_text.Length)
        {
            m_valid = false;
            return {};
        }
        const char c = m_text.Data[m_cursor];
        if (c == '+' || c == '-' || c == '!' || c == '~')
        {
            ++m_cursor;
            ArithmeticValue v = Expression(14U, active);
            v.Name = {};
            if (c == '-')
            {
                v.Value = UInt64{0U} - v.Value;
            }
            if (c == '!')
            {
                v.Value = v.Value == 0U ? 1U : 0U;
            }
            if (c == '~')
            {
                v.Value = ~v.Value;
            }
            return v;
        }
        if (Consume('('))
        {
            ArithmeticValue v = Expression(1U, active);
            if (!Consume(')'))
            {
                m_valid = false;
            }
            return v;
        }
        const Size start = m_cursor;
        if (NameStart(c))
        {
            while (m_cursor < m_text.Length && NameByte(m_text.Data[m_cursor]))
            {
                ++m_cursor;
            }
            ArithmeticValue v{0U, Slice(m_text, start, m_cursor - start)};
            bool present{false};
            const Text value = m_engine.Get(v.Name, present);
            if (active && present && value.Length != 0U && !Integer(value, v.Value))
            {
                m_valid = false;
            }
            return v;
        }
        while (m_cursor < m_text.Length && NameByte(m_text.Data[m_cursor]))
        {
            ++m_cursor;
        }
        ArithmeticValue v{};
        if (!Integer(Slice(m_text, start, m_cursor - start), v.Value))
        {
            m_valid = false;
        }
        return v;
    }
    [[nodiscard]] const OperatorInfo* Operator() noexcept
    {
        Spaces();
        for (const auto& op : Operators)
        {
            const Text name = Literal(op.Name);
            if (name.Length <= m_text.Length - m_cursor && Equal(name, Slice(m_text, m_cursor, name.Length)))
            {
                return &op;
            }
        }
        return nullptr;
    }
    [[nodiscard]] UInt64 Apply(Text op, UInt64 a, UInt64 b, bool active) noexcept
    {
        if (Is(op, "+"))
        {
            return a + b;
        }
        if (Is(op, "-"))
        {
            return a - b;
        }
        if (Is(op, "*"))
        {
            return a * b;
        }
        if (Is(op, "/") || Is(op, "%"))
        {
            if (b == 0U)
            {
                if (active)
                {
                    m_valid = false;
                }
                return 0U;
            }
            const Int64 x = static_cast<Int64>(a);
            const Int64 y = static_cast<Int64>(b);
            if (a == (UInt64{1U} << 63U) && y == -1)
            {
                return Is(op, "/") ? a : 0U;
            }
            return static_cast<UInt64>(Is(op, "/") ? x / y : x % y);
        }
        if (Is(op, "<<") || Is(op, ">>"))
        {
            if (b >= 64U)
            {
                if (active)
                {
                    m_valid = false;
                }
                return 0U;
            }
            return Is(op, "<<") ? a << b : static_cast<UInt64>(static_cast<Int64>(a) >> b);
        }
        if (Is(op, "&"))
        {
            return a & b;
        }
        if (Is(op, "|"))
        {
            return a | b;
        }
        if (Is(op, "^"))
        {
            return a ^ b;
        }
        if (Is(op, "=="))
        {
            return a == b ? 1U : 0U;
        }
        if (Is(op, "!="))
        {
            return a != b ? 1U : 0U;
        }
        if (Is(op, "<"))
        {
            return static_cast<Int64>(a) < static_cast<Int64>(b) ? 1U : 0U;
        }
        if (Is(op, ">"))
        {
            return static_cast<Int64>(a) > static_cast<Int64>(b) ? 1U : 0U;
        }
        if (Is(op, "<="))
        {
            return static_cast<Int64>(a) <= static_cast<Int64>(b) ? 1U : 0U;
        }
        if (Is(op, ">="))
        {
            return static_cast<Int64>(a) >= static_cast<Int64>(b) ? 1U : 0U;
        }
        if (Is(op, "&&"))
        {
            return a != 0U && b != 0U ? 1U : 0U;
        }
        if (Is(op, "||"))
        {
            return a != 0U || b != 0U ? 1U : 0U;
        }
        return b;
    }
    [[nodiscard]] ArithmeticValue Expression(unsigned minimum, bool active) noexcept
    {
        if (m_depth++ >= DepthLimit)
        {
            m_valid = false;
            --m_depth;
            return {};
        }
        ArithmeticValue left = Primary(active);
        while (m_valid)
        {
            const OperatorInfo* op = Operator();
            if (op == nullptr || op->Precedence < minimum)
            {
                break;
            }
            const Text name = Literal(op->Name);
            m_cursor += name.Length;
            if (Is(name, "?"))
            {
                const auto yes = Expression(1U, active && left.Value != 0U);
                if (!Consume(':'))
                {
                    m_valid = false;
                    break;
                }
                const auto no = Expression(3U, active && left.Value == 0U);
                left = left.Value != 0U ? yes : no;
                left.Name = {};
                continue;
            }
            const bool rhsActive =
                active && !(Is(name, "&&") && left.Value == 0U) && !(Is(name, "||") && left.Value != 0U);
            const auto right = Expression(op->Assignment ? op->Precedence : op->Precedence + 1U, rhsActive);
            UInt64 result = right.Value;
            if (op->Assignment)
            {
                if (left.Name.Length == 0U)
                {
                    m_valid = false;
                    break;
                }
                if (name.Length > 1U)
                {
                    result = Apply(Slice(name, 0U, name.Length - 1U), left.Value, right.Value, active);
                }
                if (active && m_valid)
                {
                    char bytes[NumberLimit]{};
                    if (!m_engine.Set(left.Name, Number(static_cast<Int64>(result), bytes)).Ok())
                    {
                        m_valid = false;
                    }
                }
            }
            else
            {
                result = Apply(name, left.Value, right.Value, active);
            }
            left = {result, {}};
        }
        --m_depth;
        return left;
    }
};
} // namespace
bool Engine::Arithmetic(const Text text, Int64& result) noexcept
{
    ArithmeticParser parser{*this, text};
    return parser.Run(result) || Fail(Error::Arithmetic);
}
} // namespace Asteria::User::Shell
