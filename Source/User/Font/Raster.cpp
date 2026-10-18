// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Raster.hpp"
#include "User/Font/Internal.hpp"
namespace Asteria::User::Font
{
using Detail::Abs;
using Detail::Coordinate;
using Detail::Mid;
namespace
{
[[nodiscard]] bool Work(RasterScratch& w, const UInt32 cost = 1U) noexcept
{
    if (cost > MaximumRasterWork - w.Work)
    {
        return false;
    }
    w.Work += cost;
    return true;
}
[[nodiscard]] Int64 Floor(const Int64 value) noexcept
{
    return value >= 0 ? value / One : -((-value + One - 1) / One);
}
[[nodiscard]] Int64 Ceil(const Int64 value) noexcept
{
    return -Floor(-value);
}
[[nodiscard]] Point Middle(const Point a, const Point b) noexcept
{
    return {Mid(a.X, b.X), Mid(a.Y, b.Y)};
}
[[nodiscard]] Error Scale(const Point p, const UInt16 units, const RasterOptions& options, Point& out) noexcept
{
    if (!Coordinate(p.X) || !Coordinate(p.Y))
    {
        return Error::CoordinateLimit;
    }
    out.X = p.X * options.PixelSize / (static_cast<Int64>(units) * One) + options.OffsetX;
    out.Y = -p.Y * options.PixelSize / (static_cast<Int64>(units) * One) + options.OffsetY;
    return Abs(out.X) <= MaximumPixelCoordinate && Abs(out.Y) <= MaximumPixelCoordinate ? Error::None
                                                                                        : Error::CoordinateLimit;
}
[[nodiscard]] Error AddEdge(RasterScratch& w, const Point a, const Point b) noexcept
{
    if (a.Y == b.Y)
    {
        return Error::None;
    }
    if (w.EdgeCount == w.EdgeCapacity)
    {
        return Error::Limit;
    }
    w.Edges[w.EdgeCount++] = {a, b};
    return Error::None;
}
[[nodiscard]] bool Flat(const CurveFrame& f) noexcept
{
    // Convex-hull second differences bound deviation under subdivision.
    constexpr Int64 tolerance{One / 16};
    return Abs(f.P[0U].X - 2 * f.P[1U].X + f.P[2U].X) <= tolerance &&
           Abs(f.P[0U].Y - 2 * f.P[1U].Y + f.P[2U].Y) <= tolerance &&
           Abs(f.P[1U].X - 2 * f.P[2U].X + f.P[3U].X) <= tolerance &&
           Abs(f.P[1U].Y - 2 * f.P[2U].Y + f.P[3U].Y) <= tolerance;
}
[[nodiscard]] Error Flatten(RasterScratch& w, const Point p0, const Point p1, const Point p2, const Point p3) noexcept
{
    w.Curves[0U] = {{p0, p1, p2, p3}, 0U};
    w.CurveCount = 1U;
    while (w.CurveCount != 0U)
    {
        if (!Work(w))
        {
            return Error::OperationLimit;
        }
        auto& frame = w.Curves[w.CurveCount - 1U];
        if (Flat(frame))
        {
            const Error e{AddEdge(w, frame.P[0U], frame.P[3U])};
            if (e != Error::None)
            {
                return e;
            }
            --w.CurveCount;
            continue;
        }
        if (frame.Depth == MaximumCurveDepth || w.CurveCount == MaximumCurveDepth + 1U)
        {
            return Error::Limit;
        }
        // Right half remains in its frame, left half is visited first.
        auto& left = w.Curves[w.CurveCount++];
        const Point ab{Middle(frame.P[0U], frame.P[1U])}, bc{Middle(frame.P[1U], frame.P[2U])},
            cd{Middle(frame.P[2U], frame.P[3U])};
        const Point abc{Middle(ab, bc)};
        const Point bcd{Middle(bc, cd)};
        const Point centre{Middle(abc, bcd)};
        left.P[0U] = frame.P[0U];
        left.P[1U] = ab;
        left.P[2U] = abc;
        left.P[3U] = centre;
        left.Depth = frame.Depth + 1U;
        frame.P[0U] = centre;
        frame.P[1U] = bcd;
        frame.P[2U] = cd;
        ++frame.Depth;
    }
    return Error::None;
}
[[nodiscard]] Error Edges(const Outline& outline, const UInt16 units, const RasterOptions& options,
                          RasterScratch& w) noexcept
{
    Point current{};
    Point start{};
    bool path{};
    for (UInt32 i{}; i < outline.Count; ++i)
    {
        const auto& c = outline.Commands[i];
        Point a{};
        Point b{};
        Point end{};
        Error error{Error::None};
        if (c.Kind != CommandKind::Close)
        {
            error = Scale(c.A, units, options, a);
            if (error != Error::None)
            {
                return error;
            }
        }
        switch (c.Kind)
        {
        case CommandKind::Move:
            if (path)
            {
                error = AddEdge(w, current, start);
            }
            start = a;
            current = a;
            path = true;
            break;
        case CommandKind::Line:
            if (!path)
            {
                return Error::Malformed;
            }
            error = AddEdge(w, current, a);
            current = a;
            break;
        case CommandKind::Quadratic:
            if (!path)
            {
                return Error::Malformed;
            }
            error = Scale(c.B, units, options, end);
            if (error != Error::None)
            {
                return error;
            }
            b = {end.X + (a.X - end.X) * 2 / 3, end.Y + (a.Y - end.Y) * 2 / 3};
            a = {current.X + (a.X - current.X) * 2 / 3, current.Y + (a.Y - current.Y) * 2 / 3};
            error = Flatten(w, current, a, b, end);
            current = end;
            break;
        case CommandKind::Cubic:
            if (!path)
            {
                return Error::Malformed;
            }
            error = Scale(c.B, units, options, b);
            if (error != Error::None)
            {
                return error;
            }
            error = Scale(c.C, units, options, end);
            if (error != Error::None)
            {
                return error;
            }
            error = Flatten(w, current, a, b, end);
            current = end;
            break;
        case CommandKind::Close:
            if (!path)
            {
                return Error::Malformed;
            }
            error = AddEdge(w, current, start);
            current = start;
            path = false;
            break;
        default:
            return Error::Malformed;
        }
        if (error != Error::None)
        {
            return error;
        }
    }
    return path ? AddEdge(w, current, start) : Error::None;
}
void Swap(Crossing& a, Crossing& b) noexcept
{
    const Crossing temp{a};
    a = b;
    b = temp;
}
[[nodiscard]] bool Sift(RasterScratch& w, UInt32 root, const UInt32 count) noexcept
{
    while (root < count / 2U)
    {
        if (!Work(w))
        {
            return false;
        }
        UInt32 child{root * 2U + 1U};
        if (child + 1U < count && w.Crossings[child].X < w.Crossings[child + 1U].X)
        {
            ++child;
        }
        if (w.Crossings[root].X >= w.Crossings[child].X)
        {
            break;
        }
        Swap(w.Crossings[root], w.Crossings[child]);
        root = child;
    }
    return true;
}
[[nodiscard]] Error Sort(RasterScratch& w, const UInt32 count) noexcept
{
    for (UInt32 root{count / 2U}; root != 0U; --root)
    {
        if (!Sift(w, root - 1U, count))
        {
            return Error::OperationLimit;
        }
    }
    for (UInt32 end{count}; end > 1U; --end)
    {
        Swap(w.Crossings[0U], w.Crossings[end - 1U]);
        if (!Sift(w, 0U, end - 1U))
        {
            return Error::OperationLimit;
        }
    }
    return Error::None;
}
[[nodiscard]] Error Scan(RasterScratch& w, const Int64 y, const Int32 left, const UInt32 width) noexcept
{
    if (!Work(w, w.EdgeCount))
    {
        return Error::OperationLimit;
    }
    UInt32 count{};
    for (UInt32 i{}; i < w.EdgeCount; ++i)
    {
        const auto& edge = w.Edges[i];
        const bool down{edge.A.Y < edge.B.Y};
        const Point a{down ? edge.A : edge.B};
        const Point b{down ? edge.B : edge.A};
        if (y < a.Y || y >= b.Y)
        {
            continue;
        }
        const Int64 dx{b.X - a.X};
        const UInt64 part{static_cast<UInt64>(Abs(dx)) * static_cast<UInt64>(y - a.Y) / static_cast<UInt64>(b.Y - a.Y)};
        if (count == w.CrossingCapacity)
        {
            return Error::Limit;
        }
        w.Crossings[count++] = {a.X + (dx < 0 ? -static_cast<Int64>(part) : static_cast<Int64>(part)), down ? 1 : -1};
    }
    Error error{Sort(w, count)};
    if (error != Error::None)
    {
        return error;
    }
    Int32 winding{};
    Int64 start{};
    for (UInt32 i{}; i < count;)
    {
        const Int64 x{w.Crossings[i].X};
        const Int32 before{winding};
        do
        {
            winding += w.Crossings[i++].Winding;
        } while (i < count && w.Crossings[i].X == x);
        if (before == 0 && winding != 0)
        {
            start = x;
        }
        else if (before != 0 && winding == 0)
        {
            // Samples at cell centres; spans are half open [start,x).
            Int64 first{Ceil((start - static_cast<Int64>(left) * One) * CoverageSamples - One / 2)};
            Int64 last{Ceil((x - static_cast<Int64>(left) * One) * CoverageSamples - One / 2)};
            if (first < 0)
            {
                first = 0;
            }
            if (last > static_cast<Int64>(width) * CoverageSamples)
            {
                last = static_cast<Int64>(width) * CoverageSamples;
            }
            if (last > first && !Work(w, static_cast<UInt32>(last - first)))
            {
                return Error::OperationLimit;
            }
            for (Int64 sample{first}; sample < last; ++sample)
            {
                ++w.Samples[sample / CoverageSamples];
            }
        }
    }
    return winding == 0 ? Error::None : Error::Malformed;
}
} // namespace
Error Rasterize(const Outline& outline, const UInt16 units, const RasterOptions& options, RasterScratch& w,
                UInt8* const pixels, const Size capacity, Mask& mask) noexcept
{
    mask = {};
    w.EdgeCount = 0U;
    w.Work = 0U;
    if (w.Edges == nullptr || w.Crossings == nullptr || w.EdgeCapacity == 0U || w.EdgeCapacity > MaximumEdges ||
        w.CrossingCapacity == 0U || w.CrossingCapacity > MaximumEdges || w.Samples == nullptr ||
        w.SampleCapacity == 0U || w.SampleCapacity > MaximumMaskAxis || units < 16U || units > 16384U ||
        options.PixelSize == 0U || options.PixelSize > MaximumMaskAxis * One || Abs(options.OffsetX) > One ||
        Abs(options.OffsetY) > One || outline.Count > outline.Capacity || outline.Count > MaximumCommands ||
        (outline.Commands == nullptr && outline.Count != 0U) || pixels == nullptr)
    {
        return Error::InvalidArgument;
    }
    Error error{Edges(outline, units, options, w)};
    if (error != Error::None)
    {
        return error;
    }
    mask.Pixels = pixels;
    mask.Length = capacity;
    if (w.EdgeCount == 0U)
    {
        return Error::None;
    }
    Int64 minX{w.Edges[0U].A.X};
    Int64 maxX{minX};
    Int64 minY{w.Edges[0U].A.Y};
    Int64 maxY{minY};
    for (UInt32 i{}; i < w.EdgeCount; ++i)
    {
        for (UInt32 point{}; point < 2U; ++point)
        {
            const Point p{point == 0U ? w.Edges[i].A : w.Edges[i].B};
            if (p.X < minX)
            {
                minX = p.X;
            }
            if (p.X > maxX)
            {
                maxX = p.X;
            }
            if (p.Y < minY)
            {
                minY = p.Y;
            }
            if (p.Y > maxY)
            {
                maxY = p.Y;
            }
        }
    }
    const Int64 left{Floor(minX)};
    const Int64 top{Floor(minY)};
    const Int64 width{Ceil(maxX) - left};
    const Int64 height{Ceil(maxY) - top};
    if (width > w.SampleCapacity || height > MaximumMaskAxis)
    {
        return Error::Limit;
    }
    if (static_cast<Size>(width * height) > capacity)
    {
        return Error::BufferTooSmall;
    }
    mask.Left = static_cast<Int32>(left);
    mask.Top = static_cast<Int32>(top);
    mask.Width = static_cast<UInt32>(width);
    mask.Height = static_cast<UInt32>(height);
    mask.Stride = mask.Width;
    for (UInt32 row{}; row < mask.Height; ++row)
    {
        for (UInt32 col{}; col < mask.Width; ++col)
        {
            w.Samples[col] = 0U;
        }
        for (UInt32 sub{}; sub < CoverageSamples; ++sub)
        {
            const Int64 y{(top + row) * One + static_cast<Int64>(2U * sub + 1U) * (One / (2U * CoverageSamples))};
            error = Scan(w, y, mask.Left, mask.Width);
            if (error != Error::None)
            {
                mask = {};
                return error;
            }
        }
        for (UInt32 col{}; col < mask.Width; ++col)
        {
            pixels[static_cast<Size>(row) * mask.Stride + col] = static_cast<UInt8>(
                (w.Samples[col] * 255U + CoverageSamples * CoverageSamples / 2U) / (CoverageSamples * CoverageSamples));
        }
    }
    return Error::None;
}
} // namespace Asteria::User::Font
