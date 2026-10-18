// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Outline.hpp"
#include "User/Font/Internal.hpp"
namespace Asteria::User::Font
{
using Detail::Abs;
using Detail::Coordinate;
using Detail::S16;
using Detail::S32;
using Detail::U8;
namespace
{
[[nodiscard]] Error Emit(Outline& out, const CommandKind kind, const Point a = {}, const Point b = {},
                         const Point c = {}) noexcept
{
    if (out.Count >= out.Capacity || out.Count >= MaximumCommands)
    {
        return Error::BufferTooSmall;
    }
    if (!Coordinate(a.X) || !Coordinate(a.Y) || !Coordinate(b.X) || !Coordinate(b.Y) || !Coordinate(c.X) ||
        !Coordinate(c.Y))
    {
        return Error::CoordinateLimit;
    }
    out.Commands[out.Count++] = {kind, a, b, c};
    return Error::None;
}
[[nodiscard]] Error Move(Type2Workspace& w, Outline& out, const Int64 x, const Int64 y) noexcept
{
    if (w.Path)
    {
        const Error e{Emit(out, CommandKind::Close)};
        if (e != Error::None)
        {
            return e;
        }
    }
    w.Current.X += x;
    w.Current.Y += y;
    w.Path = true;
    return Emit(out, CommandKind::Move, w.Current);
}
[[nodiscard]] Error Line(Type2Workspace& w, Outline& out, const Int64 x, const Int64 y) noexcept
{
    if (!w.Path)
    {
        return Error::Malformed;
    }
    w.Current.X += x;
    w.Current.Y += y;
    return Emit(out, CommandKind::Line, w.Current);
}
[[nodiscard]] Error Curve(Type2Workspace& w, Outline& out, const Int64 x1, const Int64 y1, const Int64 x2,
                          const Int64 y2, const Int64 x3, const Int64 y3) noexcept
{
    if (!w.Path)
    {
        return Error::Malformed;
    }
    const Point a{w.Current.X + x1, w.Current.Y + y1};
    const Point b{a.X + x2, a.Y + y2};
    const Point c{b.X + x3, b.Y + y3};
    const Error error{Emit(out, CommandKind::Cubic, a, b, c)};
    if (error == Error::None)
    {
        w.Current = c;
    }
    return error;
}
[[nodiscard]] Error Width(Type2Workspace& w, const UInt32 expected, const bool stem) noexcept
{
    const bool present{stem ? (w.Count & 1U) != 0U : w.Count == expected + 1U};
    if (!w.WidthSeen)
    {
        if (present)
        {
            w.Width = w.Nominal + w.Operands[0U];
            if (!Coordinate(w.Width))
            {
                return Error::CoordinateLimit;
            }
            for (UInt32 i{1U}; i < w.Count; ++i)
            {
                w.Operands[i - 1U] = w.Operands[i];
            }
            --w.Count;
        }
        w.WidthSeen = true;
    }
    return (stem ? (w.Count & 1U) == 0U : w.Count == expected) ? Error::None : Error::Malformed;
}
[[nodiscard]] Error Number(const Bytes b, Size& at, const UInt8 byte, Int64& value) noexcept
{
    if (byte >= 32U && byte <= 246U)
    {
        value = (static_cast<Int32>(byte) - 139) * One;
    }
    else if (byte >= 247U && byte <= 254U)
    {
        if (!b.Contains(at, 1U))
        {
            return Error::Truncated;
        }
        value = (byte < 251U ? (static_cast<Int32>(byte) - 247) * 256 + U8(b, at) + 108
                             : -(static_cast<Int32>(byte) - 251) * 256 - U8(b, at) - 108) *
                One;
        ++at;
    }
    else
    {
        const Size n{byte == 28U ? 2U : 4U};
        if (!b.Contains(at, n))
        {
            return Error::Truncated;
        }
        value = byte == 28U ? static_cast<Int64>(S16(b, at)) * One : S32(b, at);
        at += n;
    }
    return Error::None;
}
[[nodiscard]] Error Arithmetic(Type2Workspace& w, const UInt8 op) noexcept
{
    auto& s = w.Operands;
    auto& n = w.Count;
    const UInt32 needed{op == 22U ? 4U
                                  : (op == 3U || op == 4U || op == 10U || op == 11U || op == 12U || op == 15U ||
                                             op == 20U || op == 24U || op == 28U || op == 30U
                                         ? 2U
                                         : (op == 23U ? 0U : 1U))};
    if (n < needed)
    {
        return Error::StackUnderflow;
    }
    Int64 a{n == 0U ? 0 : s[n - 1U]};
    Int64 b{n < 2U ? 0 : s[n - 2U]};
    switch (op)
    {
    case 3U:
        s[n - 2U] = (b != 0 && a != 0) ? One : 0;
        --n;
        break;
    case 4U:
        s[n - 2U] = (b != 0 || a != 0) ? One : 0;
        --n;
        break;
    case 5U:
        s[n - 1U] = a == 0 ? One : 0;
        break;
    case 9U:
        s[n - 1U] = Abs(a);
        break;
    case 10U:
        s[n - 2U] = b + a;
        --n;
        break;
    case 11U:
        s[n - 2U] = b - a;
        --n;
        break;
    case 12U:
        if (a == 0)
        {
            return Error::Malformed;
        }
        s[n - 2U] = b * One / a;
        --n;
        break;
    case 14U:
        s[n - 1U] = -a;
        break;
    case 15U:
        s[n - 2U] = b == a ? One : 0;
        --n;
        break;
    case 18U:
        --n;
        break;
    case 20U:
        if (a < 0 || a % One != 0 || a / One >= MaximumTransient)
        {
            return Error::Malformed;
        }
        w.Transient[a / One] = b;
        n -= 2U;
        break;
    case 21U:
        if (a < 0 || a % One != 0 || a / One >= MaximumTransient)
        {
            return Error::Malformed;
        }
        s[n - 1U] = w.Transient[a / One];
        break;
    case 22U:
        s[n - 4U] = s[n - 2U] <= s[n - 1U] ? s[n - 4U] : s[n - 3U];
        n -= 3U;
        break;
    case 23U:
        if (n == MaximumOperands)
        {
            return Error::StackOverflow;
        }
        w.Random ^= w.Random << 13U;
        w.Random ^= w.Random >> 17U;
        w.Random ^= w.Random << 5U;
        s[n++] = (w.Random & 0xFFFFU) + 1U;
        break;
    case 24U:
        if (a != 0 && Abs(b) > 0x7FFFFFFFFFFFFFFFLL / Abs(a))
        {
            return Error::CoordinateLimit;
        }
        s[n - 2U] = b * a / One;
        --n;
        break;
    case 26U:
    {
        if (a < 0)
        {
            return Error::Malformed;
        }
        UInt64 low{};
        UInt64 high{1ULL << 32U};
        UInt64 target{static_cast<UInt64>(a * One)};
        for (UInt32 i{}; i < 32U; ++i)
        {
            const UInt64 mid{low + (high - low) / 2U};
            if (mid * mid <= target)
            {
                low = mid;
            }
            else
            {
                high = mid;
            }
        }
        s[n - 1U] = static_cast<Int64>(low);
        break;
    }
    case 27U:
        if (n == MaximumOperands)
        {
            return Error::StackOverflow;
        }
        s[n++] = a;
        break;
    case 28U:
        s[n - 1U] = b;
        s[n - 2U] = a;
        break;
    case 29U:
        --n;
        if (n == 0U || a % One != 0)
        {
            return Error::Malformed;
        }
        if (a < 0)
        {
            a = 0;
        }
        if (a / One >= n)
        {
            return Error::Malformed;
        }
        s[n] = s[n - 1U - static_cast<UInt32>(a / One)];
        ++n;
        break;
    case 30U:
    {
        n -= 2U;
        if (b < 0 || b % One != 0 || a % One != 0 || b / One > n)
        {
            return Error::Malformed;
        }
        const UInt32 count{static_cast<UInt32>(b / One)};
        if (count == 0U)
        {
            break;
        }
        Int64 shifts{(a / One) % count};
        if (shifts < 0)
        {
            shifts += count;
        }
        for (Int64 shift{}; shift < shifts; ++shift)
        {
            const Int64 last{s[n - 1U]};
            for (UInt32 i{n - 1U}; i > n - count; --i)
            {
                s[i] = s[i - 1U];
            }
            s[n - count] = last;
        }
        break;
    }
    default:
        return Error::Unsupported;
    }
    for (UInt32 i{}; i < n; ++i)
    {
        if (!Coordinate(s[i]))
        {
            return Error::CoordinateLimit;
        }
    }
    return Error::None;
}
[[nodiscard]] Error Flex(Type2Workspace& w, Outline& out, const UInt8 op) noexcept
{
    // #5177 section 4.1: flex hint is ignored; preserve both cubic curves.
    const auto* const s = w.Operands;
    const UInt32 expected{op == 34U ? 7U : (op == 35U ? 13U : (op == 36U ? 9U : 11U))};
    if (w.Count != expected)
    {
        return Error::Malformed;
    }
    Error error{};
    if (op == 34U)
    {
        error = Curve(w, out, s[0U], 0, s[1U], s[2U], s[3U], 0);
        if (error == Error::None)
        {
            error = Curve(w, out, s[4U], 0, s[5U], -s[2U], s[6U], 0);
        }
    }
    else if (op == 36U)
    {
        error = Curve(w, out, s[0U], s[1U], s[2U], s[3U], s[4U], 0);
        if (error == Error::None)
        {
            error = Curve(w, out, s[5U], 0, s[6U], s[7U], s[8U], -s[1U] - s[3U] - s[7U]);
        }
    }
    else
    {
        error = Curve(w, out, s[0U], s[1U], s[2U], s[3U], s[4U], s[5U]);
        if (error == Error::None)
        {
            Int64 x{s[10U]};
            Int64 y{op == 35U ? s[11U] : 0};
            if (op == 37U)
            {
                const Int64 dx{s[0U] + s[2U] + s[4U] + s[6U] + s[8U]};
                const Int64 dy{s[1U] + s[3U] + s[5U] + s[7U] + s[9U]};
                if (Abs(dx) > Abs(dy))
                {
                    y = -dy;
                }
                else
                {
                    x = -dx;
                    y = s[10U];
                }
            }
            error = Curve(w, out, s[6U], s[7U], s[8U], s[9U], x, y);
        }
    }
    w.Count = 0U;
    return error;
}
[[nodiscard]] Error Drawing(Type2Workspace& w, Outline& out, const UInt8 op) noexcept
{
    const auto* const s = w.Operands;
    const UInt32 n{w.Count};
    Error error{Error::None};
    if (op == 4U || op == 21U || op == 22U)
    {
        error = Width(w, op == 21U ? 2U : 1U, false);
        if (error != Error::None)
        {
            return error;
        }
        error = Move(w, out, op == 4U ? 0 : s[0U], op == 22U ? 0 : s[op == 21U ? 1U : 0U]);
    }
    else if (op == 5U)
    {
        if (n == 0U || n % 2U != 0U)
        {
            return Error::Malformed;
        }
        for (UInt32 i{}; i < n && error == Error::None; i += 2U)
        {
            error = Line(w, out, s[i], s[i + 1U]);
        }
    }
    else if (op == 6U || op == 7U)
    {
        if (n == 0U)
        {
            return Error::Malformed;
        }
        for (UInt32 i{}; i < n && error == Error::None; ++i)
        {
            const bool horizontal{((i & 1U) == 0U) == (op == 6U)};
            error = Line(w, out, horizontal ? s[i] : 0, horizontal ? 0 : s[i]);
        }
    }
    else if (op == 8U || op == 24U || op == 25U)
    {
        const UInt32 curves{op == 25U ? 6U : (op == 24U && n >= 2U ? n - 2U : n)};
        const UInt32 lines{op == 25U && n >= 6U ? n - 6U : (op == 24U ? 2U : 0U)};
        if (curves == 0U || curves % 6U != 0U || lines % 2U != 0U || curves + lines != n ||
            ((op == 24U || op == 25U) && lines == 0U))
        {
            return Error::Malformed;
        }
        UInt32 at{};
        if (op == 25U)
        {
            for (; at < lines && error == Error::None; at += 2U)
            {
                error = Line(w, out, s[at], s[at + 1U]);
            }
        }
        for (UInt32 i{}; i < curves && error == Error::None; i += 6U, at += 6U)
        {
            error = Curve(w, out, s[at], s[at + 1U], s[at + 2U], s[at + 3U], s[at + 4U], s[at + 5U]);
        }
        if (op == 24U && error == Error::None)
        {
            error = Line(w, out, s[at], s[at + 1U]);
        }
    }
    else if (op == 26U || op == 27U || op == 30U || op == 31U)
    {
        if (n < 4U || (n % 4U != 0U && n % 4U != 1U))
        {
            return Error::Malformed;
        }
        bool horizontal{op == 27U || op == 31U};
        UInt32 at{};
        Int64 initial{};
        if ((op == 26U || op == 27U) && (n & 1U) != 0U)
        {
            initial = s[at++];
        }
        while (at + 4U <= n && error == Error::None)
        {
            Int64 x1{};
            Int64 y1{};
            Int64 x3{};
            Int64 y3{};
            if (horizontal)
            {
                x1 = s[at];
                y1 = initial;
            }
            else
            {
                x1 = initial;
                y1 = s[at];
            }
            if (op == 26U || op == 27U)
            {
                if (horizontal)
                {
                    x3 = s[at + 3U];
                }
                else
                {
                    y3 = s[at + 3U];
                }
            }
            else
            {
                if (horizontal)
                {
                    y3 = s[at + 3U];
                }
                else
                {
                    x3 = s[at + 3U];
                }
                if (at + 5U == n)
                {
                    if (horizontal)
                    {
                        x3 = s[at + 4U];
                    }
                    else
                    {
                        y3 = s[at + 4U];
                    }
                }
            }
            error = Curve(w, out, x1, y1, s[at + 1U], s[at + 2U], x3, y3);
            at += 4U;
            initial = 0;
            if (op == 30U || op == 31U)
            {
                horizontal = !horizontal;
            }
        }
    }
    else
    {
        return Error::Unsupported;
    }
    w.Count = 0U;
    return error;
}
} // namespace
Error InterpretType2(const Bytes program, const Index& local, const Index& global, const Int64 defaultWidth,
                     const Int64 nominalWidth, Type2Workspace& w, Outline& out) noexcept
{
    out.Count = 0U;
    out.CharstringWidth = 0;
    if (out.Commands == nullptr || out.Capacity == 0U || program.Data == nullptr || !Coordinate(defaultWidth) ||
        !Coordinate(nominalWidth))
    {
        return Error::InvalidArgument;
    }
    if (program.Length > MaximumProgramBytes)
    {
        return Error::Limit;
    }
    w.Count = 0U;
    w.Depth = 0U;
    w.Stems = 0U;
    w.Tokens = 0U;
    w.Random = 1U;
    w.Current = {};
    w.Width = defaultWidth;
    w.Nominal = nominalWidth;
    w.WidthSeen = false;
    w.Path = false;
    for (UInt32 i{}; i < MaximumTransient; ++i)
    {
        w.Transient[i] = 0;
    }
    w.Frames[0U] = {program, 0U};
    for (; w.Tokens < MaximumTokens; ++w.Tokens)
    {
        auto& frame = w.Frames[w.Depth];
        if (!frame.Program.Contains(frame.At, 1U))
        {
            return Error::Truncated;
        }
        const UInt8 op{U8(frame.Program, frame.At++)};
        Error error{Error::None};
        if (op >= 32U || op == 28U)
        {
            if (w.Count == MaximumOperands)
            {
                return Error::StackOverflow;
            }
            error = Number(frame.Program, frame.At, op, w.Operands[w.Count]);
            if (error != Error::None)
            {
                return error;
            }
            ++w.Count;
            continue;
        }
        if (op == 10U || op == 29U)
        {
            if (w.Count == 0U)
            {
                return Error::StackUnderflow;
            }
            const Index& index{op == 10U ? local : global};
            const Int64 argument{w.Operands[--w.Count]};
            const UInt32 bias{index.Count < 1240U ? 107U : (index.Count < 33900U ? 1131U : 32768U)};
            const Int64 id{argument / One + bias};
            if (argument % One != 0 || id < 0 || id >= index.Count)
            {
                return Error::InvalidGlyph;
            }
            if (w.Depth == MaximumSubroutineDepth)
            {
                return Error::RecursionLimit;
            }
            const Bytes sub{index.Object(static_cast<UInt32>(id))};
            if (sub.Data == nullptr || sub.Length > MaximumProgramBytes)
            {
                return Error::Limit;
            }
            w.Frames[++w.Depth] = {sub, 0U};
            continue;
        }
        if (op == 11U)
        {
            if (w.Depth == 0U)
            {
                return Error::Malformed;
            }
            --w.Depth;
            continue;
        }
        if (op == 14U)
        {
            // Deprecated seac form is outside v1 (Appendix C).
            error = Width(w, 0U, false);
            if (error != Error::None)
            {
                return error;
            }
            if (w.Path)
            {
                error = Emit(out, CommandKind::Close);
            }
            if (error == Error::None)
            {
                out.CharstringWidth = w.Width;
            }
            return error;
        }
        if (op == 1U || op == 3U || op == 18U || op == 23U || op == 19U || op == 20U)
        {
            error = Width(w, 0U, true);
            if (error != Error::None)
            {
                return error;
            }
            if (w.Count == 0U && op != 19U && op != 20U)
            {
                return Error::Malformed;
            }
            if (w.Count / 2U > MaximumStems - w.Stems)
            {
                return Error::Limit;
            }
            w.Stems += w.Count / 2U;
            w.Count = 0U;
            if (op == 19U || op == 20U)
            {
                if (w.Stems == 0U)
                {
                    return Error::Malformed;
                }
                const UInt32 mask{(w.Stems + 7U) / 8U};
                if (!frame.Program.Contains(frame.At, mask))
                {
                    return Error::Truncated;
                }
                frame.At += mask;
            }
            continue;
        }
        if (op == 12U)
        {
            if (!frame.Program.Contains(frame.At, 1U))
            {
                return Error::Truncated;
            }
            const UInt8 escaped{U8(frame.Program, frame.At++)};
            error = escaped >= 34U && escaped <= 37U ? Flex(w, out, escaped) : Arithmetic(w, escaped);
        }
        else
        {
            error = Drawing(w, out, op);
        }
        if (error != Error::None)
        {
            return error;
        }
    }
    return Error::OperationLimit;
}
} // namespace Asteria::User::Font
