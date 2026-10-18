// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Layout.hpp"

#include "User/Font/LayoutInternal.hpp"
#include "User/Text/Utf8.hpp"
namespace Asteria::User::Font
{
using Detail::Budget;
using Detail::Ligatures;
using Detail::Pair;
namespace
{
[[nodiscard]] Int64 ScaleUnits(const Int64 value, const UInt32 size, const UInt16 units) noexcept
{
    return value * size / units;
}
void Apply(PositionedGlyph& glyph, const Adjustment a) noexcept
{
    glyph.X += a.XPlacement;
    glyph.Y += a.YPlacement;
    glyph.AdvanceX += a.XAdvance;
    glyph.AdvanceY += a.YAdvance;
}
} // namespace
Error Kerning(const Font& font, const UInt16 left, const UInt16 right, PairAdjustment& adjustment) noexcept
{
    adjustment = {};
    if (!font.IsOpen())
    {
        return Error::InvalidArgument;
    }
    if (left >= font.GetMetrics().GlyphCount || right >= font.GetMetrics().GlyphCount)
    {
        return Error::InvalidGlyph;
    }
    Budget budget{};
    return Pair(font, left, right, adjustment, budget);
}
LineMetrics GetLineMetrics(const Font& font, const UInt32 pixelSize) noexcept
{
    if (!font.IsOpen() || pixelSize == 0U || pixelSize > MaximumMaskAxis * One)
    {
        return {};
    }
    const auto& m = font.GetMetrics();
    const Int64 a{m.UseTypoMetrics ? m.TypoAscender : m.Ascender};
    const Int64 d{m.UseTypoMetrics ? m.TypoDescender : m.Descender};
    const Int64 gap{m.UseTypoMetrics ? m.TypoLineGap : m.LineGap};
    return {ScaleUnits(a, pixelSize, m.UnitsPerEm), ScaleUnits(d, pixelSize, m.UnitsPerEm),
            ScaleUnits(gap, pixelSize, m.UnitsPerEm), ScaleUnits(a - d + gap, pixelSize, m.UnitsPerEm)};
}
Error Shape(const Font& font, const Bytes utf8, const LayoutOptions& options, Run& output) noexcept
{
    output.Count = 0U;
    output.AdvanceX = 0;
    output.AdvanceY = 0;
    if (!font.IsOpen() || (utf8.Data == nullptr && utf8.Length != 0U) || output.Glyphs == nullptr ||
        options.PixelSize == 0U || options.PixelSize > MaximumMaskAxis * One)
    {
        return Error::InvalidArgument;
    }
    if (utf8.Length > MaximumTextBytes)
    {
        return Error::Limit;
    }
    for (Size at{}; at < utf8.Length;)
    {
        if (output.Count >= output.Capacity || output.Count >= MaximumRunGlyphs)
        {
            return Error::BufferTooSmall;
        }
        const UInt32 start{static_cast<UInt32>(at)};
        const UInt32 point{Text::NextCodePoint(utf8.Data, utf8.Length, at)};
        auto& glyph = output.Glyphs[output.Count++];
        glyph = {};
        glyph.Glyph = font.Glyph(point);
        glyph.Scalar = point;
        glyph.Cluster = start;
        glyph.ClusterEnd = static_cast<UInt32>(at);
    }
    Budget budget{};
    if (options.Ligatures)
    {
        const Error e{Ligatures(font, output, budget)};
        if (e != Error::None)
        {
            return e;
        }
    }
    for (UInt32 i{}; i < output.Count; ++i)
    {
        auto& glyph = output.Glyphs[i];
        glyph.AdvanceX = glyph.Scalar == 10U ? 0 : font.Horizontal(glyph.Glyph).Advance;
    }
    if (options.Kerning)
    {
        for (UInt32 i{}; i + 1U < output.Count; ++i)
        {
            if (output.Glyphs[i].Scalar == 10U || output.Glyphs[i + 1U].Scalar == 10U)
            {
                continue;
            }
            PairAdjustment pair{};
            const Error e{Pair(font, output.Glyphs[i].Glyph, output.Glyphs[i + 1U].Glyph, pair, budget)};
            if (e != Error::None)
            {
                return e;
            }
            Apply(output.Glyphs[i], pair.First);
            Apply(output.Glyphs[i + 1U], pair.Second);
        }
    }
    Int64 x{};
    Int64 y{};
    const UInt16 units{font.GetMetrics().UnitsPerEm};
    const LineMetrics line{GetLineMetrics(font, options.PixelSize)};
    for (UInt32 i{}; i < output.Count; ++i)
    {
        auto& glyph = output.Glyphs[i];
        glyph.X = x + ScaleUnits(glyph.X, options.PixelSize, units);
        glyph.Y = y + ScaleUnits(glyph.Y, options.PixelSize, units);
        glyph.AdvanceX = ScaleUnits(glyph.AdvanceX, options.PixelSize, units);
        glyph.AdvanceY = ScaleUnits(glyph.AdvanceY, options.PixelSize, units);
        x += glyph.AdvanceX;
        y += glyph.AdvanceY;
        if (glyph.Scalar == 10U)
        {
            x = 0;
            y -= line.Height;
        }
    }
    output.AdvanceX = x;
    output.AdvanceY = y;
    return Error::None;
}
Error Measure(const Font& font, const Bytes utf8, const LayoutOptions& options, Run& scratch, Int64& width,
              Int64& height) noexcept
{
    width = 0;
    height = 0;
    const Error error{Shape(font, utf8, options, scratch)};
    if (error != Error::None)
    {
        return error;
    }
    Int64 current{};
    UInt32 lines{scratch.Count == 0U ? 0U : 1U};
    for (UInt32 i{}; i < scratch.Count; ++i)
    {
        if (scratch.Glyphs[i].Scalar == 10U)
        {
            if (current > width)
            {
                width = current;
            }
            current = 0;
            ++lines;
        }
        else
        {
            current += scratch.Glyphs[i].AdvanceX;
        }
    }
    if (current > width)
    {
        width = current;
    }
    height = GetLineMetrics(font, options.PixelSize).Height * lines;
    return Error::None;
}
Error Wrap(const Run& run, const Int64 width, Line* const lines, const UInt32 capacity, UInt32& count) noexcept
{
    count = 0U;
    if (run.Glyphs == nullptr || run.Count > run.Capacity || run.Count > MaximumRunGlyphs || lines == nullptr ||
        width <= 0)
    {
        return Error::InvalidArgument;
    }
    for (UInt32 i{}; i < run.Count; ++i)
    {
        if (run.Glyphs[i].AdvanceX < -MaximumGlyphAdvance || run.Glyphs[i].AdvanceX > MaximumGlyphAdvance)
        {
            return Error::CoordinateLimit;
        }
    }
    UInt32 first{};
    while (first < run.Count)
    {
        UInt32 end{first};
        UInt32 space{first};
        Int64 current{};
        Int64 spaceWidth{};
        for (; end < run.Count; ++end)
        {
            const auto& g = run.Glyphs[end];
            if (g.Scalar == 10U)
            {
                break;
            }
            if (g.Scalar == 32U)
            {
                space = end + 1U;
                spaceWidth = current;
            }
            if (current + g.AdvanceX > width && end != first)
            {
                if (space > first)
                {
                    end = space;
                    current = spaceWidth;
                }
                break;
            }
            current += g.AdvanceX;
        }
        if (count >= capacity)
        {
            return Error::BufferTooSmall;
        }
        UInt32 visible{end - first};
        while (visible != 0U && run.Glyphs[first + visible - 1U].Scalar == 32U)
        {
            --visible;
        }
        Int64 visibleWidth{};
        for (UInt32 i{}; i < visible; ++i)
        {
            visibleWidth += run.Glyphs[first + i].AdvanceX;
        }
        lines[count++] = {first, visible, visibleWidth};
        first = end;
        if (first < run.Count && run.Glyphs[first].Scalar == 10U)
        {
            ++first;
            if (first == run.Count)
            {
                if (count >= capacity)
                {
                    return Error::BufferTooSmall;
                }
                lines[count++] = {first, 0U, 0};
            }
        }
    }
    return Error::None;
}
} // namespace Asteria::User::Font
