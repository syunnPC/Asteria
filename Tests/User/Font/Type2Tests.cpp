// SPDX-License-Identifier: Apache-2.0
#include "User/Font/Outline.hpp"
#include "User/Font/Internal.hpp"
#include "Support/TestContext.hpp"
#include "User/Font/Fixture.hpp"
using Asteria::Foundation::Int32;
using Asteria::User::Font::Command;
using Asteria::User::Font::CommandKind;
namespace Detail = Asteria::User::Font::Detail;
using Asteria::User::Font::Error;
using Asteria::User::Font::Index;
using Asteria::User::Font::InterpretType2;
using Asteria::User::Font::MaximumCommands;
using Asteria::User::Font::MaximumOperands;
using Asteria::User::Font::MaximumProgramBytes;
using Asteria::User::Font::One;
using Asteria::User::Font::Outline;
using Asteria::User::Font::Size;
using Asteria::User::Font::Type2Workspace;
using Asteria::User::Font::UInt32;
using Asteria::User::Font::UInt8;
using Asteria::TestSupport::FontFixture::Append;
using Asteria::TestSupport::FontFixture::Buffer;

using Asteria::TestSupport::FontFixture::IndexOf;
using Asteria::TestSupport::FontFixture::Number;
using Asteria::TestSupport::FontFixture::RectangleProgram;
using Asteria::TestSupport::FontFixture::U16;
using Asteria::TestSupport::FontFixture::U32;
using Asteria::TestSupport::FontFixture::Vector;
namespace
{
static Type2Workspace workspace{};
static Command commands[MaximumCommands]{};
[[nodiscard]] Index GetIndex(const Buffer& data)
{
    Index index{};
    Size at{};
    static_cast<void>(Detail::ReadIndex({data.data(), data.size()}, at, index));
    return index;
}
[[nodiscard]] Error Execute(const Buffer& program, Outline& out, const Index& local = {}, const Index& global = {})
{
    return InterpretType2({program.data(), program.size()}, local, global, 500 * One, 600 * One, workspace, out);
}
void Operators(Asteria::TestSupport::TestContext& t)
{
    Outline out{commands, MaximumCommands, 0U, 0};
    Buffer b{};
    Number(b, 50);
    Number(b, 10);
    Number(b, 20);
    b.push_back(21U);
    b.push_back(14U);
    ASTERIA_EXPECT(t, "width before first move uses nominalWidthX", Execute(b, out) == Error::None,
                   out.CharstringWidth == 650 * One, commands[0U].A.X == 10 * One, commands[0U].A.Y == 20 * One);
    b = {};
    Number(b, 50);
    b.push_back(14U);
    ASTERIA_EXPECT(t, "width-only empty glyph", Execute(b, out) == Error::None, out.Count == 0U,
                   out.CharstringWidth == 650 * One);
    b = {};
    Number(b, 50);
    Number(b, 0);
    Number(b, 10);
    b.push_back(1U);
    b.push_back(19U);
    b.push_back(0x80U);
    Number(b, 0);
    Number(b, 0);
    b.push_back(21U);
    b.push_back(14U);
    ASTERIA_EXPECT(t, "hints parsed and ignored with initial width", Execute(b, out) == Error::None,
                   workspace.Stems == 1U, out.CharstringWidth == 650 * One);
    const Buffer missingMask{139U, 149U, 1U, 19U};
    ASTERIA_EXPECT(t, "truncated hint mask", Execute(missingMask, out) == Error::Truncated);
    b = {255U, 0U, 1U, 128U, 0U, 255U, 0xFFU, 0xFEU, 128U, 0U, 21U, 14U};
    ASTERIA_EXPECT(t, "16.16 operands including negative fractions", Execute(b, out) == Error::None,
                   commands[0U].A.X == One + One / 2, commands[0U].A.Y == -One - One / 2);
    for (const UInt32 op : {5U, 6U, 7U, 8U, 24U, 25U, 26U, 27U, 30U, 31U})
    {
        b = {139U, 139U, 21U};
        const UInt32 n{op == 8U ? 12U : (op == 24U || op == 25U ? 8U : (op >= 26U ? 9U : (op == 5U ? 4U : 3U)))};
        for (UInt32 i{}; i < n; ++i)
        {
            Number(b, static_cast<Int32>(i + 1U));
        }
        b.push_back(static_cast<UInt8>(op));
        b.push_back(14U);
        ASTERIA_EXPECT(t, "every line/curve operator form is supported", Execute(b, out) == Error::None,
                       out.Count >= 3U, commands[out.Count - 1U].Kind == CommandKind::Close);
    }
    for (const UInt32 op : {34U, 35U, 36U, 37U})
    {
        b = {139U, 139U, 21U};
        const UInt32 count{op == 34U ? 7U : (op == 35U ? 13U : (op == 36U ? 9U : 11U))};
        for (UInt32 i{}; i < count; ++i)
        {
            Number(b, 10);
        }
        b.push_back(12U);
        b.push_back(static_cast<UInt8>(op));
        b.push_back(14U);
        ASTERIA_EXPECT(t, "flex variants produce two exact cubic curves", Execute(b, out) == Error::None,
                       out.Count == 4U, commands[1U].Kind == CommandKind::Cubic,
                       commands[2U].Kind == CommandKind::Cubic, commands[2U].C.X == (op == 37U ? 0 : 60 * One),
                       commands[2U].C.Y == (op == 35U || op == 37U ? 60 * One : 0));
    }
    // Arithmetic results become the x coordinate of a subsequent move.
    struct ArithmeticCase
    {
        UInt8 Op;
        Int32 Left;
        Int32 Right;
        Int32 Expected;
    };
    for (const ArithmeticCase c : {ArithmeticCase{3U, 1, 2, 1},
                                   {4U, 0, 0, 0},
                                   {10U, 2, 3, 5},
                                   {11U, 7, 2, 5},
                                   {12U, 12, 3, 4},
                                   {15U, 2, 2, 1},
                                   {24U, 3, 4, 12}})
    {
        b = {};
        Number(b, c.Left);
        Number(b, c.Right);
        b.push_back(12U);
        b.push_back(c.Op);
        Number(b, 0);
        b.push_back(21U);
        b.push_back(14U);
        ASTERIA_EXPECT(t, "arithmetic results are exact", Execute(b, out) == Error::None,
                       commands[0U].A.X == c.Expected * One);
    }
    b = {};
    Number(b, 9);
    b.push_back(12U);
    b.push_back(26U);
    Number(b, 0);
    b.push_back(21U);
    b.push_back(14U);
    ASTERIA_EXPECT(t, "integer square root in 16.16", Execute(b, out) == Error::None, commands[0U].A.X == 3 * One);
    b = {};
    Number(b, 42);
    Number(b, 5);
    b.push_back(12U);
    b.push_back(20U);
    Number(b, 5);
    b.push_back(12U);
    b.push_back(21U);
    Number(b, 0);
    b.push_back(21U);
    b.push_back(14U);
    ASTERIA_EXPECT(t, "transient put/get", Execute(b, out) == Error::None, commands[0U].A.X == 42 * One);
    const Buffer underflow{12U, 10U, 14U};
    ASTERIA_EXPECT(t, "operand underflow", Execute(underflow, out) == Error::StackUnderflow);
    const Buffer divzero{140U, 139U, 12U, 12U, 14U};
    ASTERIA_EXPECT(t, "division by zero", Execute(divzero, out) == Error::Malformed);
    b.assign(MaximumOperands + 1U, 139U);
    b.push_back(14U);
    ASTERIA_EXPECT(t, "operand overflow", Execute(b, out) == Error::StackOverflow);
    b.assign(MaximumProgramBytes + 1U, 139U);
    ASTERIA_EXPECT(t, "program byte limit", Execute(b, out) == Error::Limit);
    out.Capacity = 1U;
    ASTERIA_EXPECT(t, "outline output capacity", Execute(RectangleProgram(), out) == Error::BufferTooSmall);
    out.Capacity = MaximumCommands;
}
void Subroutines(Asteria::TestSupport::TestContext& t)
{
    Outline out{commands, MaximumCommands, 0U, 0};
    Buffer sub{139U, 139U, 21U, 11U};
    auto localBytes = IndexOf({sub});
    const auto local = GetIndex(localBytes);
    Buffer main{};
    Number(main, -107);
    main.push_back(10U);
    main.push_back(14U);
    ASTERIA_EXPECT(t, "local subroutine bias and return", Execute(main, out, local) == Error::None, out.Count == 2U);
    main[1U] = 29U;
    ASTERIA_EXPECT(t, "global subroutine bias and return", Execute(main, out, {}, local) == Error::None);
    sub = {};
    Number(sub, -107);
    sub.push_back(10U);
    sub.push_back(11U);
    localBytes = IndexOf({sub});
    main[1U] = 10U;
    ASTERIA_EXPECT(t, "cyclic subroutine bounded by depth",
                   Execute(main, out, GetIndex(localBytes)) == Error::RecursionLimit);
    for (const UInt32 n : {1240U, 33900U})
    {
        Vector<Buffer> objects(n, Buffer{11U});
        objects[0U] = Buffer{139U, 139U, 21U, 11U};
        // Four-byte INDEX offsets for large test indexes.
        Buffer bytes{};
        U16(bytes, n);
        bytes.push_back(4U);
        UInt32 at{1U};
        U32(bytes, at);
        for (const auto& object : objects)
        {
            at += static_cast<UInt32>(object.size());
            U32(bytes, at);
        }
        for (const auto& object : objects)
        {
            Append(bytes, object);
        }
        main = {};
        Number(main, n == 1240U ? -1131 : -32768);
        main.push_back(10U);
        main.push_back(14U);
        ASTERIA_EXPECT(t, "both large subroutine bias thresholds", Execute(main, out, GetIndex(bytes)) == Error::None,
                       out.Count == 2U);
    }
    sub = {};
    for (UInt32 i{}; i < 1000U; ++i)
    {
        sub.push_back(139U);
        sub.push_back(12U);
        sub.push_back(18U);
    }
    sub.push_back(11U);
    localBytes = IndexOf({sub});
    main = {};
    for (UInt32 i{}; i < 40U; ++i)
    {
        Number(main, -107);
        main.push_back(10U);
    }
    main.push_back(14U);
    ASTERIA_EXPECT(t, "aggregate token budget spans subroutine calls",
                   Execute(main, out, GetIndex(localBytes)) == Error::OperationLimit);
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext t{"Type2"};
    Operators(t);
    Subroutines(t);
    return t.Finish();
}
