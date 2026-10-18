// SPDX-License-Identifier: Apache-2.0

#include "User/Session/Decorations.hpp"
#include "User/Session/UiText.hpp"
#include "User/Text/Utf8.hpp"
#include "User/Terminal/Cp437.hpp"

#include "User/Graphics/BitmapFont.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/Graphics/Composition/Divide.hpp"
#include "User/Session/Palette.hpp"

namespace Asteria::User::Session
{
namespace
{
using Foundation::Int32;
using Foundation::Int64;
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Graphics::Color;
using Graphics::MemorySurface;
using Graphics::Point;
using Graphics::Rect;

constexpr Int64 Int32Minimum{-2147483647LL - 1LL};
constexpr Int64 Int32Maximum{2147483647LL};

/// A rectangle given by 64-bit edges, cut to Int32 range; empty when the
/// extent is not positive.
[[nodiscard]] Rect MakeRect(Int64 left, Int64 top, const Int64 width, const Int64 height) noexcept
{
    if ((width <= 0) || (height <= 0))
    {
        return Rect{};
    }
    Int64 right = left + width;
    Int64 bottom = top + height;
    left = left < Int32Minimum ? Int32Minimum : left;
    top = top < Int32Minimum ? Int32Minimum : top;
    right = right > Int32Maximum ? Int32Maximum : right;
    bottom = bottom > Int32Maximum ? Int32Maximum : bottom;
    if ((right <= left) || (bottom <= top))
    {
        return Rect{};
    }
    return Rect{.X = static_cast<Int32>(left),
                .Y = static_cast<Int32>(top),
                .Width = static_cast<UInt32>(right - left),
                .Height = static_cast<UInt32>(bottom - top)};
}

/// A view of the surface over clip, which already lies inside the surface,
/// and the desktop point of the view's (0, 0).
struct View
{
    MemorySurface Surface;
    Int64 X;
    Int64 Y;
};

[[nodiscard]] View MakeView(const MemorySurface& surface, const Rect clip) noexcept
{
    return View{.Surface = ClipSurface(surface, clip), .X = clip.X, .Y = clip.Y};
}

/// Fills a desktop rectangle given by 64-bit edges, clipped to the view.
void Fill(const View& view, const Int64 left, const Int64 top, const Int64 width, const Int64 height,
          const Color color) noexcept
{
    if ((width <= 0) || (height <= 0) || (view.Surface.Width == 0U))
    {
        return;
    }
    // Cut to the view first so the translated rectangle fits Int32.
    const Int64 viewRight = view.X + view.Surface.Width;
    const Int64 viewBottom = view.Y + view.Surface.Height;
    const Int64 cutLeft = left > view.X ? left : view.X;
    const Int64 cutTop = top > view.Y ? top : view.Y;
    const Int64 right = (left + width) < viewRight ? (left + width) : viewRight;
    const Int64 bottom = (top + height) < viewBottom ? (top + height) : viewBottom;
    if ((right <= cutLeft) || (bottom <= cutTop))
    {
        return;
    }
    Graphics::FillRect(view.Surface,
                       Rect{.X = static_cast<Int32>(cutLeft - view.X),
                            .Y = static_cast<Int32>(cutTop - view.Y),
                            .Width = static_cast<UInt32>(right - cutLeft),
                            .Height = static_cast<UInt32>(bottom - cutTop)},
                       color);
}

/// The four bevel lines of a raised (or, with the colours swapped, sunken)
/// edge, outermost first; later fills overwrite earlier ones at the corners.
struct Bevel
{
    Color OuterTopLeft;
    Color OuterBottomRight;
    Color InnerTopLeft;
    Color InnerBottomRight;
};

void PaintBevel(const View& view, const Rect box, const Bevel& bevel) noexcept
{
    const Int64 x = box.X;
    const Int64 y = box.Y;
    const Int64 w = box.Width;
    const Int64 h = box.Height;
    Fill(view, x, y, w, 1, bevel.OuterTopLeft);
    Fill(view, x, y, 1, h, bevel.OuterTopLeft);
    Fill(view, x, y + h - 1, w, 1, bevel.OuterBottomRight);
    Fill(view, x + w - 1, y, 1, h, bevel.OuterBottomRight);
    Fill(view, x + 1, y + 1, w - 2, 1, bevel.InnerTopLeft);
    Fill(view, x + 1, y + 1, 1, h - 2, bevel.InnerTopLeft);
    Fill(view, x + 1, y + h - 2, w - 2, 1, bevel.InnerBottomRight);
    Fill(view, x + w - 2, y + 1, 1, h - 2, bevel.InnerBottomRight);
}

constexpr Bevel WindowBevel{
    .OuterTopLeft = Light, .OuterBottomRight = DarkShadow, .InnerTopLeft = Highlight, .InnerBottomRight = Shadow};
constexpr Bevel RaisedButtonBevel{
    .OuterTopLeft = Highlight, .OuterBottomRight = DarkShadow, .InnerTopLeft = Light, .InnerBottomRight = Shadow};
constexpr Bevel PressedButtonBevel{
    .OuterTopLeft = DarkShadow, .OuterBottomRight = Highlight, .InnerTopLeft = Shadow, .InnerBottomRight = Light};

/// Rings 2 and 3: the face between the bevel and an inset of FrameWidth.
void PaintFaceRing(const View& view, const Rect box) noexcept
{
    const Int64 x = box.X;
    const Int64 y = box.Y;
    const Int64 w = box.Width;
    const Int64 h = box.Height;
    const Int64 frame = FrameWidth;
    Fill(view, x + 2, y + 2, w - 4, frame - 2, Face);
    Fill(view, x + 2, y + h - frame, w - 4, frame - 2, Face);
    Fill(view, x + 2, y + frame, frame - 2, h - (2 * frame), Face);
    Fill(view, x + w - frame, y + frame, frame - 2, h - (2 * frame), Face);
}

/// Draws one glyph of the font at a desktop point, clipped to the view.
void PutGlyph(const View& view, const Int64 x, const Int64 y, const UInt8 code, const Color foreground,
              const Color background) noexcept
{
    const Int64 left = x - view.X;
    const Int64 top = y - view.Y;
    if ((left + Graphics::Bold8x16.GlyphWidth <= 0) || (top + Graphics::Bold8x16.GlyphHeight <= 0) ||
        (left >= view.Surface.Width) || (top >= view.Surface.Height))
    {
        return;
    }
    Graphics::DrawGlyph(view.Surface, static_cast<Int32>(left), static_cast<Int32>(top), Graphics::Bold8x16, code,
                        foreground, background);
}

/// Places text one glyph after another from a starting column, up to a limit.
class TextRun final
{
public:
    TextRun(const View& view, const Int64 x, const Int64 y, const UInt32 limit) noexcept
        : m_view{view}, m_x{x}, m_y{y}, m_limit{limit}
    {
    }

    void Write(const UInt8* const text, const Size length, const Color foreground, const Color background,
               const bool subtitle = false) noexcept
    {
        Size index{0U};
        while ((index < length) && (m_column < m_limit))
        {
            const UInt32 point{Text::NextCodePoint(text, length, index)};
            const UInt32 shown{
                (subtitle && (!Text::IsPrintable(point) || (point == VolumeCodePoint))) ? SubtitleReplacement : point};
            PutGlyph(m_view, m_x + (static_cast<Int64>(m_column) * Graphics::Bold8x16.GlyphWidth), m_y,
                     Terminal::MapToCp437(shown), foreground, background);
            ++m_column;
        }
    }

    void Write(const char* const text, const Color foreground, const Color background) noexcept
    {
        Size length{0U};
        while (text[length] != '\0')
        {
            ++length;
        }
        Write(reinterpret_cast<const UInt8*>(text), length, foreground, background);
    }

private:
    const View& m_view;
    Int64 m_x;
    Int64 m_y;
    UInt32 m_limit;
    UInt32 m_column{0U};
};

/// A button glyph's rows of set bits, as fills.
void PaintGlyphRows(const View& view, const Int64 x, const Int64 y, const UInt16* const rows, const UInt32 width,
                    const UInt32 height) noexcept
{
    for (UInt32 row{0U}; row < height; ++row)
    {
        UInt32 column{0U};
        while (column < width)
        {
            if (((rows[row] >> (width - 1U - column)) & 1U) == 0U)
            {
                ++column;
                continue;
            }
            const UInt32 start = column;
            while ((column < width) && (((rows[row] >> (width - 1U - column)) & 1U) != 0U))
            {
                ++column;
            }
            Fill(view, x + start, y + row, column - start, 1, Glyph);
        }
    }
}

/// One button glyph: its rows, size and place inside the button.
struct ButtonGlyph
{
    const UInt16* Rows;
    UInt32 Width;
    UInt32 Height;
    UInt32 Left;
    UInt32 Top;
};

constexpr ButtonGlyph CloseMark{.Rows = CloseGlyph,
                                .Width = CloseGlyphWidth,
                                .Height = CloseGlyphHeight,
                                .Left = CloseGlyphLeft,
                                .Top = CloseGlyphTop};
constexpr ButtonGlyph MaximizeMark{.Rows = MaximizeGlyph,
                                   .Width = MaximizeGlyphWidth,
                                   .Height = MaximizeGlyphHeight,
                                   .Left = MaximizeGlyphLeft,
                                   .Top = MaximizeGlyphTop};
constexpr ButtonGlyph RestoreMark{.Rows = RestoreGlyph,
                                  .Width = RestoreGlyphWidth,
                                  .Height = RestoreGlyphHeight,
                                  .Left = RestoreGlyphLeft,
                                  .Top = RestoreGlyphTop};

void PaintButton(const View& view, const Rect button, const ButtonState state, const ButtonGlyph& glyph) noexcept
{
    const bool pressed = state == ButtonState::Pressed;
    PaintBevel(view, button, pressed ? PressedButtonBevel : RaisedButtonBevel);
    Fill(view, static_cast<Int64>(button.X) + 2, static_cast<Int64>(button.Y) + 2, static_cast<Int64>(button.Width) - 4,
         static_cast<Int64>(button.Height) - 4, Face);
    const Int64 offset = pressed ? 1 : 0;
    PaintGlyphRows(view, static_cast<Int64>(button.X) + glyph.Left + offset,
                   static_cast<Int64>(button.Y) + glyph.Top + offset, glyph.Rows, glyph.Width, glyph.Height);
}

[[nodiscard]] bool Inside(const Rect rect, const Point point) noexcept
{
    return (point.X >= rect.X) && (point.Y >= rect.Y) &&
           (static_cast<Int64>(point.X) < static_cast<Int64>(rect.X) + rect.Width) &&
           (static_cast<Int64>(point.Y) < static_cast<Int64>(rect.Y) + rect.Height);
}

/// Writes text into out from at, up to TitleTextBytes; returns the new end.
[[nodiscard]] Size Append(UInt8 (&out)[TitleTextBytes], Size at, const UInt8* const text, const Size length,
                          const bool replace) noexcept
{
    Size index{0U};
    while ((index < length) && (at < TitleTextBytes))
    {
        const UInt32 point{Text::NextCodePoint(text, length, index)};
        const UInt32 shown{(replace && (!Text::IsPrintable(point) || (point == VolumeCodePoint))) ? SubtitleReplacement
                                                                                                  : point};
        UInt8 bytes[4U]{};
        const Size count{Text::EncodeUtf8(shown, bytes)};
        if (count > (TitleTextBytes - at))
        {
            break;
        }
        for (Size byte{0U}; byte < count; ++byte)
        {
            out[at++] = bytes[byte];
        }
    }
    return at;
}

[[nodiscard]] Size Append(UInt8 (&out)[TitleTextBytes], const Size at, const char* const text) noexcept
{
    Size length{0U};
    while (text[length] != '\0')
    {
        ++length;
    }
    return Append(out, at, reinterpret_cast<const UInt8*>(text), length, false);
}
} // namespace

MemorySurface ClipSurface(const MemorySurface& surface, const Rect clip) noexcept
{
    const Rect inside = Graphics::ClipToSurface(surface, clip);
    if (Graphics::IsEmpty(inside) || !Graphics::IsUsable(surface))
    {
        return MemorySurface{.Pixels = nullptr, .Width = 0U, .Height = 0U, .Stride = 0U, .Format = surface.Format};
    }
    return MemorySurface{.Pixels = surface.Pixels +
                                   (static_cast<Size>(static_cast<UInt32>(inside.Y)) * surface.Stride) +
                                   static_cast<UInt32>(inside.X),
                         .Width = inside.Width,
                         .Height = inside.Height,
                         .Stride = surface.Stride,
                         .Format = surface.Format};
}

Rect ContentRect(const Rect frame) noexcept
{
    return MakeRect(static_cast<Int64>(frame.X) + FrameWidth,
                    static_cast<Int64>(frame.Y) + FrameWidth + TitleBarHeight + SeparatorHeight,
                    static_cast<Int64>(frame.Width) - FrameExtraWidth,
                    static_cast<Int64>(frame.Height) - FrameExtraHeight);
}

Rect TitleBarRect(const Rect frame) noexcept
{
    const Int64 height = static_cast<Int64>(frame.Height) - (2 * FrameWidth);
    return MakeRect(static_cast<Int64>(frame.X) + FrameWidth, static_cast<Int64>(frame.Y) + FrameWidth,
                    static_cast<Int64>(frame.Width) - (2 * FrameWidth),
                    height < TitleBarHeight ? height : TitleBarHeight);
}

Rect CloseButtonRect(const Rect frame) noexcept
{
    return MakeRect(static_cast<Int64>(frame.X) + frame.Width - FrameWidth - ButtonRightInset - ButtonWidth,
                    static_cast<Int64>(frame.Y) + FrameWidth + ButtonTop, ButtonWidth, ButtonHeight);
}

Rect MaximizeButtonRect(const Rect frame) noexcept
{
    return MakeRect(static_cast<Int64>(frame.X) + frame.Width - FrameWidth - ButtonRightInset - (2 * ButtonWidth) -
                        ButtonGap,
                    static_cast<Int64>(frame.Y) + FrameWidth + ButtonTop, ButtonWidth, ButtonHeight);
}

Rect FrameAroundContent(const Point contentOrigin, const UInt32 width, const UInt32 height) noexcept
{
    return MakeRect(static_cast<Int64>(contentOrigin.X) - FrameWidth,
                    static_cast<Int64>(contentOrigin.Y) - (FrameExtraHeight - FrameWidth),
                    static_cast<Int64>(width) + FrameExtraWidth, static_cast<Int64>(height) + FrameExtraHeight);
}

Rect MaximizedFrame(const Rect output) noexcept
{
    return output;
}

Rect SwitcherRect(const UInt32 width, const UInt32 height, const Size length) noexcept
{
    return SwitcherRectPixels(width, height,
                              static_cast<UInt32>(length < TitleTextBytes ? length : TitleTextBytes) *
                                  Graphics::Bold8x16.GlyphWidth);
}

Rect SwitcherRectPixels(const UInt32 width, const UInt32 height, const UInt32 textPixels) noexcept
{
    const UInt64 wanted = static_cast<UInt64>(textPixels) + (2U * SwitcherPadding);
    const UInt64 limit = width > SwitcherMargin ? (width - SwitcherMargin) : width;
    const UInt32 boxWidth = static_cast<UInt32>(wanted < limit ? wanted : limit);
    const UInt32 x = width > boxWidth ? ((width - boxWidth) >> 1U) : 0U;
    const UInt32 y = height > SwitcherHeight ? ((height - SwitcherHeight) >> 1U) : 0U;
    return MakeRect(x, y, boxWidth, SwitcherHeight);
}

Size ComposeTitle(const TitleView& title, UInt8 (&out)[TitleTextBytes]) noexcept
{
    Size at{0U};
    if (title.Class == ClientClass::VolumeBundle)
    {
        at += Text::EncodeUtf8(VolumeCodePoint, out);
        out[at++] = 0x20U;
    }
    at = Append(out, at, title.Name, title.NameLength < TitleNameBytes ? title.NameLength : TitleNameBytes, false);
    if (title.SubtitleLength != 0U)
    {
        at = Append(out, at, SubtitleMark);
        at = Append(out, at, title.Subtitle,
                    title.SubtitleLength < SubtitleBytes ? title.SubtitleLength : SubtitleBytes, true);
    }
    if (title.NotResponding)
    {
        at = Append(out, at, NotRespondingMark);
    }
    return at;
}

FramePart HitTest(const Rect frame, const Point point, const bool maximized) noexcept
{
    if (!Inside(frame, point))
    {
        return FramePart::None;
    }
    const Int64 left = static_cast<Int64>(point.X) - frame.X;
    const Int64 top = static_cast<Int64>(point.Y) - frame.Y;
    const Int64 right = static_cast<Int64>(frame.Width) - 1 - left;
    const Int64 bottom = static_cast<Int64>(frame.Height) - 1 - top;
    const Int64 zone = ResizeZone;
    const Int64 corner = CornerExtent;
    const bool onLeft = left < zone;
    const bool onRight = right < zone;
    const bool onTop = top < zone;
    const bool onBottom = bottom < zone;
    if (!maximized && (onLeft || onRight || onTop || onBottom))
    {
        const bool nearLeft = left < corner;
        const bool nearRight = right < corner;
        const bool nearTop = top < corner;
        const bool nearBottom = bottom < corner;
        if ((onTop && nearLeft) || (onLeft && nearTop))
        {
            return FramePart::ResizeTopLeft;
        }
        if ((onTop && nearRight) || (onRight && nearTop))
        {
            return FramePart::ResizeTopRight;
        }
        if ((onBottom && nearLeft) || (onLeft && nearBottom))
        {
            return FramePart::ResizeBottomLeft;
        }
        if ((onBottom && nearRight) || (onRight && nearBottom))
        {
            return FramePart::ResizeBottomRight;
        }
        if (onLeft)
        {
            return FramePart::ResizeLeft;
        }
        if (onRight)
        {
            return FramePart::ResizeRight;
        }
        return onTop ? FramePart::ResizeTop : FramePart::ResizeBottom;
    }
    const Rect title = TitleBarRect(frame);
    if (Inside(Graphics::Intersect(CloseButtonRect(frame), title), point))
    {
        return FramePart::CloseButton;
    }
    if (Inside(Graphics::Intersect(MaximizeButtonRect(frame), title), point))
    {
        return FramePart::MaximizeButton;
    }
    if (Inside(ContentRect(frame), point))
    {
        return FramePart::Content;
    }
    if (Inside(title, point))
    {
        return FramePart::Title;
    }
    return FramePart::Frame;
}

void PaintFrame(const MemorySurface& surface, const Rect clip, const Rect frame, const TitleView& title,
                UiText* const ui) noexcept
{
    const Rect area = Graphics::Intersect(Graphics::ClipToSurface(surface, clip), frame);
    if (Graphics::IsEmpty(area))
    {
        return;
    }
    const View view = MakeView(surface, area);
    PaintBevel(view, frame, WindowBevel);
    PaintFaceRing(view, frame);
    // The separator between the title bar and the content.
    Fill(view, static_cast<Int64>(frame.X) + FrameWidth, static_cast<Int64>(frame.Y) + FrameWidth + TitleBarHeight,
         static_cast<Int64>(frame.Width) - (2 * FrameWidth), SeparatorHeight, Face);

    const Rect bar = TitleBarRect(frame);
    const Rect barArea = Graphics::Intersect(area, bar);
    if (Graphics::IsEmpty(barArea))
    {
        return;
    }
    const View barView = MakeView(surface, barArea);
    const bool unfocused = title.State == TitleState::Unfocused;
    const Color background = (title.State == TitleState::Focused)
                                 ? ActiveTitle
                                 : ((title.State == TitleState::Attention) ? AttentionTitle : InactiveTitle);
    const Color text = unfocused ? InactiveTitleText : ActiveTitleText;
    const Color subtitle = unfocused ? InactiveSubtitle : ActiveSubtitle;
    Fill(barView, bar.X, bar.Y, bar.Width, bar.Height, background);

    const Rect maximize = MaximizeButtonRect(frame);
    const Int64 textLeft = static_cast<Int64>(bar.X) + TitleTextInset;
    const Int64 textRight = static_cast<Int64>(maximize.X) - TitleTextGap;
    const UInt32 columns =
        (textRight > textLeft)
            ? Graphics::Composition::Divide(static_cast<UInt32>(textRight - textLeft), Graphics::Bold8x16.GlyphWidth)
            : 0U;
    if (ui != nullptr && textRight > textLeft)
    {
        ui->PaintTitle(surface, barArea,
                       {static_cast<Int32>(textLeft), bar.Y + static_cast<Int32>(TitleTextTop),
                        static_cast<UInt32>(textRight - textLeft), UiTextHeight},
                       title, text, subtitle, background);
    }
    else if (ui == nullptr)
    {
        TextRun run{barView, textLeft, static_cast<Int64>(bar.Y) + TitleTextTop, columns};
        if (title.Class == ClientClass::VolumeBundle)
        {
            UInt8 prefix[4U]{};
            const Size count{Text::EncodeUtf8(VolumeCodePoint, prefix)};
            prefix[count] = 0x20U;
            run.Write(prefix, count + 1U, text, background);
        }
        run.Write(title.Name, title.NameLength < TitleNameBytes ? title.NameLength : TitleNameBytes, text, background);
        if (title.SubtitleLength != 0U)
        {
            run.Write(SubtitleMark, subtitle, background);
            const Size length = title.SubtitleLength < SubtitleBytes ? title.SubtitleLength : SubtitleBytes;
            run.Write(title.Subtitle, length, subtitle, background, true);
        }
        if (title.NotResponding)
        {
            run.Write(NotRespondingMark, text, background);
        }
    }

    PaintButton(barView, maximize, title.Maximize, title.Maximized ? RestoreMark : MaximizeMark);
    PaintButton(barView, CloseButtonRect(frame), title.Close, CloseMark);
}

void PaintSwitcher(const MemorySurface& surface, const Rect clip, const Rect box, const UInt8* const text,
                   const Size length, UiText* const ui) noexcept
{
    const Rect area = Graphics::Intersect(Graphics::ClipToSurface(surface, clip), box);
    if (Graphics::IsEmpty(area))
    {
        return;
    }
    const View view = MakeView(surface, area);
    PaintBevel(view, box, WindowBevel);
    Fill(view, static_cast<Int64>(box.X) + 2, static_cast<Int64>(box.Y) + 2, static_cast<Int64>(box.Width) - 4,
         static_cast<Int64>(box.Height) - 4, Face);
    const UInt32 columns =
        box.Width > (2U * SwitcherPadding)
            ? Graphics::Composition::Divide(box.Width - (2U * SwitcherPadding), Graphics::Bold8x16.GlyphWidth)
            : 0U;
    if (ui != nullptr)
    {
        const UInt32 width{box.Width > 2U * SwitcherPadding ? box.Width - 2U * SwitcherPadding : 0U};
        Size suffix{sizeof(NotRespondingMark) - 1U};
        if (length < suffix)
        {
            suffix = 0U;
        }
        for (Size i{}; i < suffix; ++i)
        {
            if (text[length - suffix + i] != static_cast<UInt8>(NotRespondingMark[i]))
            {
                suffix = 0U;
                break;
            }
        }
        ui->Paint(surface, area,
                  {box.X + static_cast<Int32>(SwitcherPadding), box.Y + static_cast<Int32>(SwitcherTextTop), width,
                   UiTextHeight},
                  text, length, false, length, suffix, Glyph, Glyph, Face);
        return;
    }
    TextRun run{view, static_cast<Int64>(box.X) + SwitcherPadding, static_cast<Int64>(box.Y) + SwitcherTextTop,
                columns};
    run.Write(text, length, Glyph, Face);
}

void PaintOutline(const MemorySurface& surface, const Rect clip, const Rect outline) noexcept
{
    const Rect area = Graphics::Intersect(Graphics::ClipToSurface(surface, clip), outline);
    if (Graphics::IsEmpty(area))
    {
        return;
    }
    const View view = MakeView(surface, area);
    const Int64 x = outline.X;
    const Int64 y = outline.Y;
    const Int64 w = outline.Width;
    const Int64 h = outline.Height;
    const Int64 line = OutlineWidth;
    Fill(view, x, y, w, line, DarkShadow);
    Fill(view, x, y + h - line, w, line, DarkShadow);
    Fill(view, x, y, line, h, DarkShadow);
    Fill(view, x + w - line, y, line, h, DarkShadow);
}
} // namespace Asteria::User::Session
