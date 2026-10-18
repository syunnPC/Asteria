// SPDX-License-Identifier: Apache-2.0

#include "User/Session/UiText.hpp"
#include "User/Session/UiTextDrawing.hpp"
#include "User/Graphics/BitmapFont.hpp"
#include "User/Terminal/Cp437.hpp"
#include "User/Text/Utf8.hpp"
namespace Asteria::User::Session
{
using Foundation::Size;
using Foundation::Int32;
using Foundation::Int64;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
namespace
{
constexpr UInt32 FixedSize{UiPixelSize * static_cast<UInt32>(Font::One)};
constexpr UInt32 BitmapWidth{Graphics::Bold8x16.GlyphWidth};
constexpr UInt8 Ellipsis[]{'.', '.', '.'};
[[nodiscard]] Int32 Pixel(const Int64 value) noexcept
{
    return static_cast<Int32>(value < 0 ? -((-value + Font::One / 2) / Font::One)
                                        : (value + Font::One / 2) / Font::One);
}
} // namespace
bool UiText::OpenFace(Font::Font& face, const Font::Bytes data) noexcept
{
    if (face.Open(data) != Font::Error::None || !face.IsCff())
    {
        static_cast<void>(face.Open({}));
        return false;
    }
    // Validate lazy programs with the actual owner's capacities before exposing
    // the face. A damaged optional layout table also refuses the face.
    Font::Outline outline{m_commands, UiOutlineCommands};
    Font::Mask mask{};
    for (UInt32 glyph{}; glyph < face.GetMetrics().GlyphCount; ++glyph)
    {
        if (m_cache.GetCff(face, static_cast<UInt16>(glyph), {FixedSize}, m_type2, outline, m_raster, mask) !=
            Font::Error::None)
        {
            static_cast<void>(face.Open({}));
            return false;
        }
    }
    const UInt8 sample[]{'A', 'V', 'f', 'f', 'i', '.', '.', '.'};
    m_run = {m_glyphs, TitleTextBytes};
    if (Font::Shape(face, {sample, sizeof(sample)}, {FixedSize, true, true}, m_run) != Font::Error::None)
    {
        static_cast<void>(face.Open({}));
        return false;
    }
    return true;
}
bool UiText::Open(const Font::Bytes bold, const Font::Bytes regular) noexcept
{
    m_raster.Edges = m_edges;
    m_raster.Crossings = m_crossings;
    m_raster.EdgeCapacity = UiRasterEdges;
    m_raster.CrossingCapacity = UiRasterCrossings;
    m_raster.Samples = m_samples;
    m_raster.SampleCapacity = UiRasterColumns;
    if (m_cache.Configure(m_bytes, sizeof(m_bytes), m_entries, UiCacheEntries) != Font::Error::None)
    {
        return false;
    }
    const bool b{OpenFace(m_bold, bold)};
    const bool r{OpenFace(m_regular, regular)};
    m_cache.Clear();
    return b && r;
}
bool UiText::Scalable(const bool bold) const noexcept
{
    return (bold ? m_bold : m_regular).IsOpen();
}
bool UiText::Shape(const UInt8* const text, const Size length, const bool bold) noexcept
{
    m_run = {m_glyphs, TitleTextBytes};
    auto& face = bold ? m_bold : m_regular;
    if (face.IsOpen())
    {
        if (Font::Shape(face, {text, length}, {FixedSize, true, true}, m_run) == Font::Error::None)
        {
            return true;
        }
        static_cast<void>(face.Open({}));
        m_cache.Clear();
        m_run = {m_glyphs, TitleTextBytes};
    }
    Size at{};
    while (at < length && m_run.Count < m_run.Capacity)
    {
        const Size first{at};
        const UInt32 scalar{Text::NextCodePoint(text, length, at)};
        m_glyphs[m_run.Count++] = {.Cluster = static_cast<UInt32>(first),
                                   .ClusterEnd = static_cast<UInt32>(at),
                                   .Scalar = scalar,
                                   .X = m_run.AdvanceX,
                                   .AdvanceX = BitmapWidth * Font::One};
        m_run.AdvanceX += BitmapWidth * Font::One;
    }
    return at == length;
}
Int64 UiText::Advance() const noexcept
{
    return m_run.AdvanceX;
}
UInt32 UiText::Width(const UInt8* text, Size length, const bool bold) noexcept
{
    if (text == nullptr || length > TitleTextBytes)
    {
        return 0U;
    }
    Size at{};
    UInt32 marker{};
    if (Text::NextCodePoint(text, length, at) == VolumeCodePoint)
    {
        marker = MarkerWidth(bold);
        if (at < length && text[at] == ' ')
        {
            ++at;
        }
        text += at;
        length -= at;
    }
    if (!Shape(text, length, bold))
    {
        return 0U;
    }
    return marker + static_cast<UInt32>((Advance() + Font::One - 1) / Font::One);
}
UInt32 UiText::MarkerWidth(const bool bold) noexcept
{
    const UInt8 space[]{' '};
    return BitmapWidth + (Scalable(bold) ? Width(space, sizeof(space), bold) : BitmapWidth);
}
Int64 UiText::Fit(const UInt8* const text, const Size length, const bool bold, const UInt32 width,
                  const Size suffixBytes) noexcept
{
    m_length = length;
    m_prefix = length;
    m_suffix = suffixBytes;
    if (!Shape(text, length, bold))
    {
        return 0;
    }
    if (Advance() <= static_cast<Int64>(width) * Font::One)
    {
        return Advance();
    }
    // Walk UTF-8 cluster boundaries from the complete shaped run. Reshape each
    // candidate so terminal kerning/ligature context is correct after the cut.
    const UInt32 count{m_run.Count};
    for (UInt32 i{}; i < count; ++i)
    {
        m_ends[i] = m_glyphs[i].ClusterEnd;
    }
    Size keptSuffix{suffixBytes <= length ? suffixBytes : 0U};
    if (keptSuffix != 0U &&
        Width(text + length - keptSuffix, keptSuffix, bold) + Width(Ellipsis, sizeof(Ellipsis), bold) > width)
    {
        keptSuffix = 0U;
    }
    const Size body{length - suffixBytes};
    for (UInt32 candidate{count + 1U}; candidate != 0U; --candidate)
    {
        const Size prefix{candidate == 1U ? 0U : m_ends[candidate - 2U]};
        if (prefix > body || prefix + sizeof(Ellipsis) + keptSuffix > sizeof(m_fitted))
        {
            continue;
        }
        Size at{};
        for (; at < prefix; ++at)
        {
            m_fitted[at] = text[at];
        }
        for (const UInt8 byte : Ellipsis)
        {
            m_fitted[at++] = byte;
        }
        for (Size i{}; i < keptSuffix; ++i)
        {
            m_fitted[at++] = text[length - keptSuffix + i];
        }
        if (!Shape(m_fitted, at, bold))
        {
            return 0;
        }
        if (Advance() <= static_cast<Int64>(width) * Font::One || prefix == 0U)
        {
            m_length = at;
            m_prefix = prefix;
            m_suffix = keptSuffix;
            return Advance();
        }
    }
    return 0;
}
void UiText::Draw(const Graphics::MemorySurface& surface, const Graphics::Point origin, const bool bold,
                  const Size trustedEnd, const Size suffixStart, const Graphics::Color trusted,
                  const Graphics::Color subtitle, const Graphics::Color background) noexcept
{
    auto& face = bold ? m_bold : m_regular;
    Font::Outline outline{m_commands, UiOutlineCommands};
    Font::Mask mask{};
    for (UInt32 i{}; i < m_run.Count; ++i)
    {
        const auto& glyph = m_glyphs[i];
        const auto color = glyph.Cluster < trustedEnd || glyph.Cluster >= suffixStart ? trusted : subtitle;
        const Int32 x{origin.X + Pixel(glyph.X)};
        if (face.IsOpen() && glyph.Glyph != 0U &&
            m_cache.GetCff(face, glyph.Glyph, {FixedSize}, m_type2, outline, m_raster, mask) == Font::Error::None)
        {
            const Graphics::Point baseline{x, origin.Y + static_cast<Int32>(UiBaseline) - Pixel(glyph.Y)};
            DrawUiMask(surface, mask, baseline, m_drawClip, color);
        }
        else
        {
            Bitmap(surface, {x, origin.Y}, glyph.Scalar, color, background);
        }
    }
}
void UiText::Bitmap(const Graphics::MemorySurface& surface, const Graphics::Point point, const UInt32 scalar,
                    const Graphics::Color foreground, const Graphics::Color background) noexcept
{
    const auto code = Terminal::MapToCp437(scalar);
    DrawUiBitmap(surface, point, m_drawClip, code, foreground, background);
}
void UiText::PaintTitle(const Graphics::MemorySurface& surface, const Graphics::Rect clip, const Graphics::Rect box,
                        const TitleView& title, const Graphics::Color trusted, const Graphics::Color subtitle,
                        const Graphics::Color background) noexcept
{
    const Size length{ComposeTitle(title, m_title)};
    Paint(surface, clip, box, m_title, length, true,
          title.NameLength + (title.Class == ClientClass::VolumeBundle ? 4U : 0U),
          title.NotResponding ? sizeof(NotRespondingMark) - 1U : 0U, trusted, subtitle, background);
}
void UiText::Paint(const Graphics::MemorySurface& surface, const Graphics::Rect clip, Graphics::Rect box,
                   const UInt8* text, Size length, const bool bold, Size trustedEnd, const Size suffixBytes,
                   const Graphics::Color trusted, const Graphics::Color subtitle,
                   const Graphics::Color background) noexcept
{
    const auto cut = Graphics::Intersect(Graphics::ClipToSurface(surface, clip), box);
    if (Graphics::IsEmpty(cut) || text == nullptr || length > TitleTextBytes || suffixBytes > length)
    {
        return;
    }
    auto view = ClipSurface(surface, cut);
    Size at{};
    if (Text::NextCodePoint(text, length, at) == VolumeCodePoint)
    {
        m_drawClip = cut;
        Bitmap(view, {box.X - cut.X, box.Y - cut.Y}, VolumeCodePoint, trusted, background);
        const UInt32 marker{MarkerWidth(bold)};
        if (box.Width <= marker)
        {
            return;
        }
        box.X += static_cast<Int32>(marker);
        box.Width -= marker;
        if (at < length && text[at] == ' ')
        {
            ++at;
        }
        text += at;
        length -= at;
        trustedEnd = trustedEnd > at ? trustedEnd - at : 0U;
    }
    static_cast<void>(Fit(text, length, bold, box.Width, suffixBytes));
    const auto textCut = Graphics::Intersect(cut, box);
    if (Graphics::IsEmpty(textCut))
    {
        return;
    }
    view = ClipSurface(surface, textCut);
    m_drawClip = textCut;
    Draw(view, {box.X - textCut.X, box.Y - textCut.Y}, bold, trustedEnd < m_prefix ? trustedEnd : m_length - m_suffix,
         m_length - m_suffix, trusted, subtitle, background);
}
} // namespace Asteria::User::Session
