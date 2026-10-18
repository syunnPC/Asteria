// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Outline.hpp"
#include "User/Font/Internal.hpp"
namespace Asteria::User::Font
{
using Detail::Coordinate;
using Detail::Mid;
using Detail::S16;
using Detail::U16;
using Detail::U8;
namespace
{
[[nodiscard]] Error Add(Outline& out, const CommandKind kind, const Point a = {}, const Point b = {}) noexcept
{
    if (out.Count >= out.Capacity || out.Count >= MaximumCommands)
    {
        return Error::BufferTooSmall;
    }
    out.Commands[out.Count++] = {kind, a, b, {}};
    return Error::None;
}
[[nodiscard]] Error Simple(const Bytes data, const UInt32 contours, OutlineWorkspace& w) noexcept
{
    // OpenType 1.9 glyf / Simple Glyph Description: repeat flags, delta coordinates.
    if (contours > MaximumPoints - w.ContourCount)
    {
        return Error::Limit;
    }
    Size at{10U};
    if (!data.Contains(at, static_cast<Size>(contours) * 2U + 2U))
    {
        return Error::Truncated;
    }
    const UInt32 start{w.PointCount};
    UInt32 points{};
    UInt32 previous{};
    for (UInt32 i{}; i < contours; ++i)
    {
        const UInt32 end{U16(data, at)};
        at += 2U;
        if (i != 0U && end <= previous)
        {
            return Error::Malformed;
        }
        if (end >= MaximumPoints - start)
        {
            return Error::Limit;
        }
        previous = end;
        w.Ends[w.ContourCount++] = start + end;
        points = end + 1U;
    }
    const UInt16 instructions{U16(data, at)};
    at += 2U;
    if (!data.Contains(at, instructions))
    {
        return Error::Truncated;
    }
    at += instructions;
    w.PointCount += points;
    for (UInt32 i{}; i < points;)
    {
        if (!data.Contains(at, 1U))
        {
            return Error::Truncated;
        }
        const UInt8 flags{U8(data, at++)};
        UInt32 repeat{};
        if ((flags & 0x80U) != 0U)
        {
            return Error::Malformed;
        }
        if ((flags & 8U) != 0U)
        {
            if (!data.Contains(at, 1U))
            {
                return Error::Truncated;
            }
            repeat = U8(data, at++);
        }
        if (repeat >= points - i)
        {
            return Error::Malformed;
        }
        for (UInt32 count{}; count <= repeat; ++count)
        {
            w.Points[start + i++].Flags = flags;
        }
    }
    for (UInt32 axis{}; axis < 2U; ++axis)
    {
        Int64 coordinate{};
        for (UInt32 i{}; i < points; ++i)
        {
            const UInt8 flags{w.Points[start + i].Flags};
            Int32 delta{};
            const UInt8 shortBit{static_cast<UInt8>(axis == 0U ? 2U : 4U)},
                sameBit{static_cast<UInt8>(axis == 0U ? 16U : 32U)};
            if ((flags & shortBit) != 0U)
            {
                if (!data.Contains(at, 1U))
                {
                    return Error::Truncated;
                }
                delta = U8(data, at++);
                if ((flags & sameBit) == 0U)
                {
                    delta = -delta;
                }
            }
            else if ((flags & sameBit) == 0U)
            {
                if (!data.Contains(at, 2U))
                {
                    return Error::Truncated;
                }
                delta = S16(data, at);
                at += 2U;
            }
            coordinate += static_cast<Int64>(delta) * One;
            if (!Coordinate(coordinate))
            {
                return Error::CoordinateLimit;
            }
            if (axis == 0U)
            {
                w.Points[start + i].Position.X = coordinate;
            }
            else
            {
                w.Points[start + i].Position.Y = coordinate;
            }
        }
    }
    return Error::None;
}
[[nodiscard]] Error Transform(CompositeFrame& frame, OutlineWorkspace& w) noexcept
{
    for (UInt32 i{frame.ChildStart}; i < w.PointCount; ++i)
    {
        auto& p = w.Points[i].Position;
        const Int64 x{(frame.A * p.X + frame.C * p.Y) / One};
        const Int64 y{(frame.B * p.X + frame.D * p.Y) / One};
        if (!Coordinate(x) || !Coordinate(y))
        {
            return Error::CoordinateLimit;
        }
        p = {x, y};
    }
    Int64 x{frame.X};
    Int64 y{frame.Y};
    if ((frame.Flags & 2U) == 0U)
    {
        if (frame.Arg1 >= frame.ChildStart - frame.Start || frame.Arg2 >= w.PointCount - frame.ChildStart)
        {
            return Error::Malformed;
        }
        x = w.Points[frame.Start + frame.Arg1].Position.X - w.Points[frame.ChildStart + frame.Arg2].Position.X;
        y = w.Points[frame.Start + frame.Arg1].Position.Y - w.Points[frame.ChildStart + frame.Arg2].Position.Y;
    }
    else if ((frame.Flags & 0x800U) != 0U)
    {
        x = (frame.A * frame.X + frame.C * frame.Y) / One;
        y = (frame.B * frame.X + frame.D * frame.Y) / One;
    }
    for (UInt32 i{frame.ChildStart}; i < w.PointCount; ++i)
    {
        auto& p = w.Points[i].Position;
        p.X += x;
        p.Y += y;
        if (!Coordinate(p.X) || !Coordinate(p.Y))
        {
            return Error::CoordinateLimit;
        }
    }
    return Error::None;
}
[[nodiscard]] Error Enter(const Font& font, const UInt16 glyph, OutlineWorkspace& w) noexcept
{
    if (glyph >= font.GetMetrics().GlyphCount)
    {
        return Error::InvalidGlyph;
    }
    if (w.Depth >= MaximumCompositeDepth)
    {
        return Error::RecursionLimit;
    }
    if (++w.Components > MaximumComponents)
    {
        return Error::OperationLimit;
    }
    for (UInt32 i{}; i < w.Depth; ++i)
    {
        if (w.Composites[i].Glyph == glyph)
        {
            return Error::RecursionLimit;
        }
    }
    const UInt32 start{font.GlyphOffset(glyph)};
    const UInt32 end{font.GlyphOffset(static_cast<UInt32>(glyph) + 1U)};
    auto& f = w.Composites[w.Depth++];
    f.Data = font.Table(Tag('g', 'l', 'y', 'f')).Slice(start, end - start);
    f.At = 0U;
    f.Start = w.PointCount;
    f.ChildStart = w.PointCount;
    f.Glyph = glyph;
    f.Waiting = false;
    f.Instructions = false;
    f.Flags = 0U;
    return Error::None;
}
[[nodiscard]] Error Component(const Font& font, CompositeFrame& f, OutlineWorkspace& w) noexcept
{
    if (!f.Data.Contains(f.At, 4U))
    {
        return Error::Truncated;
    }
    f.Flags = U16(f.Data, f.At);
    const UInt16 glyph{U16(f.Data, f.At + 2U)};
    f.At += 4U;
    const UInt16 flags{f.Flags};
    f.Instructions = f.Instructions || ((flags & 0x100U) != 0U);
    if ((flags & 0xE010U) != 0U || ((flags & 0x1800U) == 0x1800U) ||
        (((flags & 8U) != 0U) + ((flags & 0x40U) != 0U) + ((flags & 0x80U) != 0U) > 1))
    {
        return Error::Malformed;
    }
    const bool words{(flags & 1U) != 0U};
    const Size bytes{words ? 4U : 2U};
    if (!f.Data.Contains(f.At, bytes))
    {
        return Error::Truncated;
    }
    f.Arg1 = words ? U16(f.Data, f.At) : U8(f.Data, f.At);
    f.Arg2 = words ? U16(f.Data, f.At + 2U) : U8(f.Data, f.At + 1U);
    f.At += bytes;
    f.X = static_cast<Int64>(words ? static_cast<Int16>(f.Arg1) : static_cast<Int8>(f.Arg1)) * One;
    f.Y = static_cast<Int64>(words ? static_cast<Int16>(f.Arg2) : static_cast<Int8>(f.Arg2)) * One;
    f.A = One;
    f.B = 0;
    f.C = 0;
    f.D = One;
    const Size matrixBytes{(flags & 8U) != 0U ? 2U : ((flags & 0x40U) != 0U ? 4U : ((flags & 0x80U) != 0U ? 8U : 0U))};
    if (!f.Data.Contains(f.At, matrixBytes))
    {
        return Error::Truncated;
    }
    if (matrixBytes != 0U)
    {
        f.A = static_cast<Int64>(S16(f.Data, f.At)) * 4;
        if (matrixBytes == 2U)
        {
            f.D = f.A;
        }
        else if (matrixBytes == 4U)
        {
            f.D = static_cast<Int64>(S16(f.Data, f.At + 2U)) * 4;
        }
        else
        {
            f.B = static_cast<Int64>(S16(f.Data, f.At + 2U)) * 4;
            f.C = static_cast<Int64>(S16(f.Data, f.At + 4U)) * 4;
            f.D = static_cast<Int64>(S16(f.Data, f.At + 6U)) * 4;
        }
    }
    f.At += matrixBytes;
    f.ChildStart = w.PointCount;
    f.Waiting = true;
    return Enter(font, glyph, w);
}
[[nodiscard]] Error TrueType(const Font& font, const UInt16 glyph, OutlineWorkspace& w) noexcept
{
    w.PointCount = 0U;
    w.ContourCount = 0U;
    w.Components = 0U;
    w.Depth = 0U;
    Error error{Enter(font, glyph, w)};
    if (error != Error::None)
    {
        return error;
    }
    // Explicit frames bound native stack usage independently of composite depth.
    for (UInt32 steps{}; steps <= MaximumComponents * 3U && w.Depth != 0U; ++steps)
    {
        auto& f = w.Composites[w.Depth - 1U];
        if (f.At == 0U)
        {
            if (f.Data.Length == 0U)
            {
                --w.Depth;
                continue;
            }
            if (!f.Data.Contains(0U, 10U))
            {
                return Error::Truncated;
            }
            const Int16 count{S16(f.Data, 0U)};
            if (count >= 0)
            {
                error = Simple(f.Data, static_cast<UInt32>(count), w);
                if (error != Error::None)
                {
                    return error;
                }
                --w.Depth;
                continue;
            }
            if (count != -1)
            {
                return Error::Malformed;
            }
            f.At = 10U;
        }
        if (f.Waiting)
        {
            error = Transform(f, w);
            if (error != Error::None)
            {
                return error;
            }
            f.Waiting = false;
            if ((f.Flags & 0x20U) == 0U)
            {
                if (f.Instructions)
                {
                    if (!f.Data.Contains(f.At, 2U))
                    {
                        return Error::Truncated;
                    }
                    const UInt16 count{U16(f.Data, f.At)};
                    f.At += 2U;
                    if (!f.Data.Contains(f.At, count))
                    {
                        return Error::Truncated;
                    }
                }
                --w.Depth;
                continue;
            }
        }
        error = Component(font, f, w);
        if (error != Error::None)
        {
            return error;
        }
    }
    return w.Depth == 0U ? Error::None : Error::OperationLimit;
}
[[nodiscard]] Point Midpoint(const Point a, const Point b) noexcept
{
    return {Mid(a.X, b.X), Mid(a.Y, b.Y)};
}
[[nodiscard]] Error Contour(const OutlineWorkspace& w, const UInt32 first, const UInt32 last, Outline& out) noexcept
{
    const bool firstOn{(w.Points[first].Flags & 1U) != 0U};
    const bool lastOn{(w.Points[last].Flags & 1U) != 0U};
    Point start{w.Points[first].Position};
    UInt32 at{first};
    UInt32 count{last - first + 1U};
    if (firstOn)
    {
        ++at;
        --count;
    }
    else if (lastOn)
    {
        start = w.Points[last].Position;
        --count;
    }
    else
    {
        start = Midpoint(w.Points[last].Position, start);
    }
    Error error{Add(out, CommandKind::Move, start)};
    bool pending{};
    Point control{};
    for (UInt32 step{}; step < count && error == Error::None; ++step, ++at)
    {
        const auto& p = w.Points[at];
        const bool on{(p.Flags & 1U) != 0U};
        if (on)
        {
            error = pending ? Add(out, CommandKind::Quadratic, control, p.Position)
                            : Add(out, CommandKind::Line, p.Position);
            pending = false;
        }
        else
        {
            if (pending)
            {
                error = Add(out, CommandKind::Quadratic, control, Midpoint(control, p.Position));
            }
            control = p.Position;
            pending = true;
        }
    }
    if (pending && error == Error::None)
    {
        error = Add(out, CommandKind::Quadratic, control, start);
    }
    if (error == Error::None)
    {
        error = Add(out, CommandKind::Close);
    }
    return error;
}
} // namespace
Error LoadOutline(const Font& font, const UInt16 glyph, OutlineWorkspace& w, Outline& out) noexcept
{
    out.Count = 0U;
    out.CharstringWidth = 0;
    if (!font.IsOpen() || out.Commands == nullptr || out.Capacity == 0U)
    {
        return Error::InvalidArgument;
    }
    if (glyph >= font.GetMetrics().GlyphCount)
    {
        return Error::InvalidGlyph;
    }
    if (font.IsCff())
    {
        return InterpretType2(font.Charstrings().Object(glyph), font.LocalSubroutines(), font.GlobalSubroutines(),
                              font.DefaultWidth(), font.NominalWidth(), w.Type2, out);
    }
    const Error error{TrueType(font, glyph, w)};
    if (error != Error::None)
    {
        return error;
    }
    UInt32 first{};
    for (UInt32 i{}; i < w.ContourCount; ++i)
    {
        const UInt32 last{w.Ends[i]};
        const Error result{Contour(w, first, last, out)};
        if (result != Error::None)
        {
            return result;
        }
        first = last + 1U;
    }
    return Error::None;
}
} // namespace Asteria::User::Font
