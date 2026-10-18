// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Graphics.hpp"

#include "User/Font/Draw.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/Terminal/Cp437.hpp"

namespace Asteria::User::Toolkit
{
bool Image::Valid() const noexcept
{
    return Pixels != nullptr && Width > 0U && Width <= MaximumAxis && Height > 0U && Height <= MaximumAxis &&
           Stride >= Width * 4U && Stride <= MaximumAxis * 4U &&
           Bytes >= static_cast<Size>(Height - 1U) * Stride + Width * 4U &&
           (Format == ImageFormat::Rgba || Format == ImageFormat::Bgra);
}
Graphics::Graphics(User::Graphics::MemorySurface surface, Rect clip, const Theme& theme) noexcept
    : m_surface{surface}, m_clip{User::Graphics::ClipToSurface(surface, clip)}, m_limit{m_clip}, m_theme{theme}
{
}
void Graphics::FillRect(Rect rect, Color color) noexcept
{
    User::Graphics::FillRect(m_surface, User::Graphics::Intersect(m_clip, rect), color);
}
void Graphics::Line(Point a, Point b, Color color) noexcept
{
    if (a.X < -static_cast<Int32>(MaximumAxis) || a.Y < -static_cast<Int32>(MaximumAxis) ||
        b.X < -static_cast<Int32>(MaximumAxis) || b.Y < -static_cast<Int32>(MaximumAxis) ||
        a.X > static_cast<Int32>(MaximumAxis * MaximumDepth) || b.X > static_cast<Int32>(MaximumAxis * MaximumDepth) ||
        a.Y > static_cast<Int32>(MaximumAxis * MaximumDepth) || b.Y > static_cast<Int32>(MaximumAxis * MaximumDepth))
    {
        return;
    }
    const Int32 dx{a.X < b.X ? b.X - a.X : a.X - b.X};
    const Int32 dy{a.Y < b.Y ? a.Y - b.Y : b.Y - a.Y};
    const Int32 sx{a.X < b.X ? 1 : -1};
    const Int32 sy{a.Y < b.Y ? 1 : -1};
    Int32 error{dx + dy};
    for (UInt32 work{}; work <= MaximumAxis * MaximumDepth * 2U; ++work)
    {
        FillRect({a.X, a.Y, 1U, 1U}, color);
        if (a.X == b.X && a.Y == b.Y)
        {
            break;
        }
        const Int32 twice{2 * error};
        if (twice >= dy)
        {
            error += dy;
            a.X += sx;
        }
        if (twice <= dx)
        {
            error += dx;
            a.Y += sy;
        }
    }
}
void Graphics::Bevel(Rect rect, bool sunken) noexcept
{
    for (UInt32 ring{}; ring < m_theme.Border && rect.Width > 1U && rect.Height > 1U; ++ring)
    {
        const Color top{sunken ? (ring == 0U ? m_theme.Shadow : m_theme.DarkShadow)
                               : (ring == 0U ? m_theme.Highlight : m_theme.Light)};
        const Color bottom{sunken ? (ring == 0U ? m_theme.Highlight : m_theme.Light)
                                  : (ring == 0U ? m_theme.DarkShadow : m_theme.Shadow)};
        FillRect({rect.X, rect.Y, rect.Width - 1U, 1U}, top);
        FillRect({rect.X, rect.Y, 1U, rect.Height - 1U}, top);
        FillRect({rect.X, rect.Y + static_cast<Int32>(rect.Height) - 1, rect.Width, 1U}, bottom);
        FillRect({rect.X + static_cast<Int32>(rect.Width) - 1, rect.Y, 1U, rect.Height}, bottom);
        rect = Inset(rect, 1U);
    }
}
void Graphics::FocusRect(Rect rect) noexcept
{
    rect = Inset(rect, m_theme.FocusInset);
    if (rect.Width == 0U || rect.Height == 0U)
    {
        return;
    }
    for (UInt32 x{}; x < rect.Width; x += 2U)
    {
        FillRect({rect.X + static_cast<Int32>(x), rect.Y, 1U, 1U}, m_theme.Text);
        FillRect({rect.X + static_cast<Int32>(x), rect.Y + static_cast<Int32>(rect.Height) - 1, 1U, 1U}, m_theme.Text);
    }
    for (UInt32 y{}; y < rect.Height; y += 2U)
    {
        FillRect({rect.X, rect.Y + static_cast<Int32>(y), 1U, 1U}, m_theme.Text);
        FillRect({rect.X + static_cast<Int32>(rect.Width) - 1, rect.Y + static_cast<Int32>(y), 1U, 1U}, m_theme.Text);
    }
}
bool Graphics::Shape(TextView text, Font::Run& run) noexcept
{
    FontContext* const context{m_theme.Font};
    if (context == nullptr || context->Face == nullptr || !context->Face->IsOpen())
    {
        return false;
    }
    run = {context->Glyphs, MaximumText, 0U, 0, 0};
    if (Font::Shape(*context->Face, {reinterpret_cast<const UInt8*>(text.Data), text.Length},
                    {m_theme.FontSize * static_cast<UInt32>(Font::One), true, false}, run) != Font::Error::None)
    {
        return false;
    }
    Foundation::Int64 shift{};
    for (UInt32 i{}; i < run.Count; ++i)
    {
        auto& glyph{run.Glyphs[i]};
        glyph.X += shift;
        if (glyph.Glyph == 0U)
        {
            const Foundation::Int64 advance{m_theme.Fallback.GlyphWidth * Font::One};
            shift += advance - glyph.AdvanceX;
            glyph.AdvanceX = advance;
        }
    }
    run.AdvanceX += shift;
    return true;
}
UInt32 Graphics::Measure(TextView text) noexcept
{
    if (!ValidText(text, MaximumText))
    {
        return 0U;
    }
    Font::Run run{};
    if (Shape(text, run))
    {
        return static_cast<UInt32>(run.AdvanceX / Font::One);
    }
    Size at{};
    UInt32 count{};
    while (at < text.Length)
    {
        static_cast<void>(User::Text::NextCodePoint(reinterpret_cast<const UInt8*>(text.Data), text.Length, at));
        ++count;
    }
    return count * m_theme.Fallback.GlyphWidth;
}
void Graphics::Scalar(UInt32 point, Point position, Color color) noexcept
{
    const auto& font{m_theme.Fallback};
    const UInt32 code{User::Terminal::MapToCp437(point)};
    if (code < font.FirstCode || code - font.FirstCode >= font.Count)
    {
        return;
    }
    const UInt8* glyph{font.Data + static_cast<Size>(code - font.FirstCode) * font.GlyphHeight};
    for (UInt32 y{}; y < font.GlyphHeight; ++y)
    {
        for (UInt32 x{}; x < font.GlyphWidth; ++x)
        {
            if ((glyph[y] & (1U << (7U - x))) != 0U)
            {
                FillRect({position.X + static_cast<Int32>(x), position.Y + static_cast<Int32>(y), 1U, 1U}, color);
            }
        }
    }
}
void Graphics::RunText(TextView text, Point position, Color color) noexcept
{
    Font::Run run{};
    if (Shape(text, run))
    {
        FontContext& context{*m_theme.Font};
        Font::Outline outline{context.Commands, Font::MaximumCommands, 0U, 0};
        const auto metrics{Font::GetLineMetrics(*context.Face, m_theme.FontSize * static_cast<UInt32>(Font::One))};
        const Int32 baseline{position.Y + static_cast<Int32>(metrics.Ascender / Font::One)};
        // A surface view translated to the clip ensures DrawMask cannot escape it.
        if (m_clip.Width == 0U || m_clip.Height == 0U)
        {
            return;
        }
        const User::Graphics::MemorySurface clipped{m_surface.Pixels + static_cast<Size>(m_clip.Y) * m_surface.Stride +
                                                        static_cast<Size>(m_clip.X),
                                                    m_clip.Width, m_clip.Height, m_surface.Stride, m_surface.Format};
        for (UInt32 index{}; index < run.Count; ++index)
        {
            const auto& glyph{run.Glyphs[index]};
            Font::Mask mask{};
            const Point pen{position.X + static_cast<Int32>(glyph.X / Font::One), baseline};
            if (glyph.Glyph != 0U &&
                context.Cache.Get(*context.Face, glyph.Glyph, {m_theme.FontSize * static_cast<UInt32>(Font::One), 0, 0},
                                  context.OutlineScratch, outline, context.RasterScratch, mask) == Font::Error::None)
            {
                static_cast<void>(Font::DrawMask(clipped, mask, {pen.X - m_clip.X, pen.Y - m_clip.Y}, color));
            }
            else
            {
                Scalar(glyph.Scalar, {pen.X, position.Y}, color);
            }
        }
        return;
    }
    Size at{};
    while (at < text.Length)
    {
        Scalar(User::Text::NextCodePoint(reinterpret_cast<const UInt8*>(text.Data), text.Length, at), position, color);
        position.X += static_cast<Int32>(m_theme.Fallback.GlyphWidth);
    }
}
void Graphics::Text(TextView text, Rect rect, Color color, Align align, bool ellipsis) noexcept
{
    if (!ValidText(text, MaximumText))
    {
        return;
    }
    const Rect oldClip{m_clip};
    m_clip = User::Graphics::Intersect(m_clip, rect);
    UInt32 width{Measure(text)};
    bool shortened{};
    const auto dots{Utf8("...")};
    const UInt32 dotsWidth{Measure(dots)};
    if (ellipsis && width > rect.Width)
    {
        while (text.Length != 0U && width + dotsWidth > rect.Width)
        {
            --text.Length;
            while (text.Length != 0U && (static_cast<UInt8>(text.Data[text.Length]) & 0xC0U) == 0x80U)
            {
                --text.Length;
            }
            width = Measure(text);
        }
        shortened = true;
        width += dotsWidth;
    }
    const Int32 shift{align == Align::Start ? 0
                                            : static_cast<Int32>(rect.Width > width ? rect.Width - width : 0U) /
                                                  (align == Align::Center ? 2 : 1)};
    Point pen{rect.X + shift,
              rect.Y + static_cast<Int32>(rect.Height > m_theme.FontSize ? rect.Height - m_theme.FontSize : 0U) / 2};
    RunText(text, pen, color);
    if (shortened)
    {
        pen.X += static_cast<Int32>(Measure(text));
        RunText(dots, pen, color);
    }
    m_clip = oldClip;
}
void Graphics::DrawImage(const Image& image, Rect destination, bool scale) noexcept
{
    if (!image.Valid() || destination.Width == 0U || destination.Height == 0U)
    {
        return;
    }
    if (!scale)
    {
        destination.Width = Min(destination.Width, image.Width);
        destination.Height = Min(destination.Height, image.Height);
    }
    const Rect clip{User::Graphics::Intersect(destination, m_clip)};
    for (UInt32 y{}; y < clip.Height; ++y)
    {
        const UInt32 dy{static_cast<UInt32>(clip.Y - destination.Y) + y};
        const UInt32 sy{scale ? static_cast<UInt32>(static_cast<UInt64>(dy) * image.Height / destination.Height) : dy};
        for (UInt32 x{}; x < clip.Width; ++x)
        {
            const UInt32 dx{static_cast<UInt32>(clip.X - destination.X) + x};
            const UInt32 sx{scale ? static_cast<UInt32>(static_cast<UInt64>(dx) * image.Width / destination.Width)
                                  : dx};
            const UInt8* source{image.Pixels + static_cast<Size>(sy) * image.Stride + sx * 4U};
            const UInt32 foreground{User::Graphics::PackColor(
                m_surface.Format, {source[image.Format == ImageFormat::Rgba ? 0U : 2U], source[1U],
                                   source[image.Format == ImageFormat::Rgba ? 2U : 0U]})};
            UInt32& pixel{m_surface.Pixels[static_cast<Size>(static_cast<UInt32>(clip.Y) + y) * m_surface.Stride +
                                           static_cast<UInt32>(clip.X) + x]};
            UInt32 result{};
            for (UInt32 shift{}; shift < 24U; shift += 8U)
            {
                result |= ((((foreground >> shift) & 255U) * source[3U] +
                            ((pixel >> shift) & 255U) * (255U - source[3U]) + 127U) /
                           255U)
                          << shift;
            }
            pixel = result;
        }
    }
}
} // namespace Asteria::User::Toolkit
