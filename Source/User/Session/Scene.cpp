// SPDX-License-Identifier: Apache-2.0

#include "User/Session/Scene.hpp"
#include "User/Session/UiText.hpp"
#include "User/Text/Utf8.hpp"

#include "User/Graphics/Blit.hpp"
#include "User/Session/Palette.hpp"
#include "User/Session/SessionFrame.hpp"

namespace Asteria::User::Session
{
namespace
{
using Foundation::Int32;
using Foundation::Int64;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;
using Graphics::MemorySurface;
using Graphics::Point;
using Graphics::Rect;

constexpr auto MaximumOutputExtent = SessionMaximumOutputExtent;

[[nodiscard]] bool SameRect(const Rect first, const Rect second) noexcept
{
    return (first.X == second.X) && (first.Y == second.Y) && (first.Width == second.Width) &&
           (first.Height == second.Height);
}

[[nodiscard]] bool SameLook(const WindowLook& first, const WindowLook& second) noexcept
{
    return (first.State == second.State) && (first.NotResponding == second.NotResponding) &&
           (first.Maximized == second.Maximized) && (first.Close == second.Close) &&
           (first.Maximize == second.Maximize);
}

[[nodiscard]] UInt32 Smaller(const UInt32 first, const UInt32 second) noexcept
{
    return first < second ? first : second;
}

/// Fills a desktop rectangle, already inside the view's rectangle clip, of
/// the view.
void FillInside(const MemorySurface& view, const Rect clip, const Rect rect, const Graphics::Color color) noexcept
{
    const Rect cut = Graphics::Intersect(rect, clip);
    if (Graphics::IsEmpty(cut))
    {
        return;
    }
    Graphics::FillRect(view,
                       Rect{.X = static_cast<Int32>(static_cast<Int64>(cut.X) - clip.X),
                            .Y = static_cast<Int32>(static_cast<Int64>(cut.Y) - clip.Y),
                            .Width = cut.Width,
                            .Height = cut.Height},
                       color);
}

constexpr WindowLook HiddenLook{.State = TitleState::Unfocused,
                                .NotResponding = false,
                                .Maximized = false,
                                .Close = ButtonState::Raised,
                                .Maximize = ButtonState::Raised};
} // namespace

Rect UnionRect(const Rect first, const Rect second) noexcept
{
    if (Graphics::IsEmpty(first))
    {
        return second;
    }
    if (Graphics::IsEmpty(second))
    {
        return first;
    }
    const Int64 left = first.X < second.X ? first.X : second.X;
    const Int64 top = first.Y < second.Y ? first.Y : second.Y;
    const Int64 firstRight = static_cast<Int64>(first.X) + first.Width;
    const Int64 secondRight = static_cast<Int64>(second.X) + second.Width;
    const Int64 firstBottom = static_cast<Int64>(first.Y) + first.Height;
    const Int64 secondBottom = static_cast<Int64>(second.Y) + second.Height;
    const Int64 right = firstRight > secondRight ? firstRight : secondRight;
    const Int64 bottom = firstBottom > secondBottom ? firstBottom : secondBottom;
    return Rect{.X = static_cast<Int32>(left),
                .Y = static_cast<Int32>(top),
                .Width = static_cast<UInt32>(right - left),
                .Height = static_cast<UInt32>(bottom - top)};
}

bool ContainsRect(const Rect outer, const Rect inner) noexcept
{
    if (Graphics::IsEmpty(inner))
    {
        return true;
    }
    return (inner.X >= outer.X) && (inner.Y >= outer.Y) &&
           ((static_cast<Int64>(inner.X) + inner.Width) <= (static_cast<Int64>(outer.X) + outer.Width)) &&
           ((static_cast<Int64>(inner.Y) + inner.Height) <= (static_cast<Int64>(outer.Y) + outer.Height));
}

bool IsPrintableUtf8(const UInt8* const text, const Size length) noexcept
{
    return Text::ValidUtf8(text, length, true);
}

namespace
{
[[nodiscard]] Size TextColumns(const UInt8* text, const Size length) noexcept
{
    Size at{0U};
    Size columns{0U};
    while (at < length)
    {
        static_cast<void>(Text::NextCodePoint(text, length, at));
        ++columns;
    }
    return columns;
}
} // namespace

void Scene::Configure(const UInt32 width, const UInt32 height, const Graphics::PixelFormat format) noexcept
{
    const bool valid = (width != 0U) && (height != 0U) && (width <= MaximumOutputExtent) &&
                       (height <= MaximumOutputExtent) && Graphics::IsValidPixelFormat(format);
    m_width = valid ? width : 0U;
    m_height = valid ? height : 0U;
    m_format = valid ? format : Graphics::PixelFormat::Invalid;
    for (Window& window : m_windows)
    {
        window = Window{};
        window.Look = HiddenLook;
    }
    m_count = 0U;
    m_switcher = false;
    m_switcherLength = 0U;
    m_outlineShown = false;
    m_damage.Configure(Output());
    m_composed.Configure(Output());
    DamageAll();
}

Rect Scene::Output() const noexcept
{
    return Rect{.X = 0, .Y = 0, .Width = m_width, .Height = m_height};
}

Graphics::PixelFormat Scene::Format() const noexcept
{
    return m_format;
}

bool Scene::Show(const Size slot, const Rect frame) noexcept
{
    if ((slot >= MaximumWindows) || m_windows[slot].Shown || (m_width == 0U))
    {
        return false;
    }
    Window& window = m_windows[slot];
    window = Window{};
    window.Frame = frame;
    window.Look = HiddenLook;
    window.Class = ClientClass::SystemModule;
    window.Shown = true;
    m_order[m_count++] = static_cast<UInt8>(slot);
    m_damage.Add(frame);
    return true;
}

void Scene::Hide(const Size slot) noexcept
{
    const Size index = OrderIndex(slot);
    if (index == NoWindow)
    {
        return;
    }
    m_damage.Add(m_windows[slot].Frame);
    for (Size move{index}; (move + 1U) < m_count; ++move)
    {
        m_order[move] = m_order[move + 1U];
    }
    --m_count;
    m_windows[slot] = Window{};
    m_windows[slot].Look = HiddenLook;
}

bool Scene::IsShown(const Size slot) const noexcept
{
    return (slot < MaximumWindows) && m_windows[slot].Shown;
}

void Scene::Raise(const Size slot) noexcept
{
    const Size index = OrderIndex(slot);
    if ((index == NoWindow) || ((index + 1U) == m_count))
    {
        return;
    }
    for (Size move{index}; (move + 1U) < m_count; ++move)
    {
        m_order[move] = m_order[move + 1U];
    }
    m_order[m_count - 1U] = static_cast<UInt8>(slot);
    m_damage.Add(m_windows[slot].Frame);
}

void Scene::SetFrame(const Size slot, const Rect frame) noexcept
{
    if (!IsShown(slot) || SameRect(m_windows[slot].Frame, frame))
    {
        return;
    }
    Window& window = m_windows[slot];
    m_damage.Add(window.Frame);
    m_damage.Add(frame);
    window.Frame = frame;
    // Pending client damage is inside the old content; the new frame is
    // damaged whole.
    window.Pending = Rect{};
}

void Scene::SetLook(const Size slot, const WindowLook& look) noexcept
{
    if (!IsShown(slot) || SameLook(m_windows[slot].Look, look))
    {
        return;
    }
    m_windows[slot].Look = look;
    DamageTitle(slot);
}

bool Scene::SetTitle(const Size slot, const UInt8* const name, const Size length, const ClientClass client) noexcept
{
    if (!IsShown(slot) || (length == 0U) || (length > TitleNameBytes) || !IsPrintableUtf8(name, length) ||
        ((client != ClientClass::SystemModule) && (client != ClientClass::VolumeBundle)))
    {
        return false;
    }
    Window& window = m_windows[slot];
    for (Size index{0U}; index < TitleNameBytes; ++index)
    {
        window.Name[index] = index < length ? name[index] : UInt8{0U};
    }
    window.NameLength = static_cast<UInt8>(length);
    window.Class = client;
    DamageTitle(slot);
    return true;
}

bool Scene::SetSubtitle(const Size slot, const UInt8* const text, const Size length) noexcept
{
    if (!IsShown(slot) || (length > SubtitleBytes) || !Text::ValidUtf8(text, length))
    {
        return false;
    }
    Window& window = m_windows[slot];
    for (Size index{0U}; index < SubtitleBytes; ++index)
    {
        window.Subtitle[index] = index < length ? text[index] : UInt8{0U};
    }
    window.SubtitleLength = static_cast<UInt8>(length);
    DamageTitle(slot);
    return true;
}

void Scene::Present(const Size slot, const ImageView& image, const Rect* const damage, const Size count) noexcept
{
    if (!IsShown(slot))
    {
        return;
    }
    Window& window = m_windows[slot];
    const Rect content = ContentRect(window.Frame);
    const bool whole = !window.HasImage || (window.Image.Width != image.Width) ||
                       (window.Image.Height != image.Height) || (window.Image.Format != image.Format) ||
                       (damage == nullptr) || (count > MaximumPresentDamage);
    window.Image = image;
    window.HasImage = true;
    if (whole)
    {
        window.Pending = UnionRect(window.Pending, content);
        return;
    }
    for (Size index{0U}; index < count; ++index)
    {
        const Rect local =
            Graphics::Intersect(damage[index], Rect{.X = 0, .Y = 0, .Width = content.Width, .Height = content.Height});
        if (Graphics::IsEmpty(local))
        {
            continue;
        }
        window.Pending =
            UnionRect(window.Pending, Rect{.X = static_cast<Int32>(static_cast<Int64>(content.X) + local.X),
                                           .Y = static_cast<Int32>(static_cast<Int64>(content.Y) + local.Y),
                                           .Width = local.Width,
                                           .Height = local.Height});
    }
}

Foundation::Size Scene::Title(const Foundation::Size slot, Foundation::UInt8 (&out)[TitleTextBytes]) const noexcept
{
    if (!IsShown(slot))
    {
        return 0U;
    }
    const auto& w = m_windows[slot];
    return ComposeTitle(TitleView{w.Name, w.NameLength, w.Subtitle, w.SubtitleLength, w.Class, w.Look.State,
                                  w.Look.NotResponding, w.Look.Maximized, w.Look.Close, w.Look.Maximize},
                        out);
}

void Scene::ShowSwitcher(const UInt8* const text, const Size length) noexcept
{
    const Size kept = length < TitleTextBytes ? length : TitleTextBytes;
    if (!Text::ValidUtf8(text, kept))
    {
        return;
    }
    if (m_switcher)
    {
        m_damage.Add(SwitcherBox());
    }
    for (Size index{0U}; index < TitleTextBytes; ++index)
    {
        m_switcherText[index] = index < kept ? text[index] : UInt8{0U};
    }
    m_switcherLength = kept;
    m_switcher = true;
    m_damage.Add(SwitcherBox());
}

void Scene::HideSwitcher() noexcept
{
    if (m_switcher)
    {
        m_switcher = false;
        m_damage.Add(SwitcherBox());
    }
}

void Scene::ShowOutline(const Rect outline) noexcept
{
    if (m_outlineShown && SameRect(m_outline, outline))
    {
        return;
    }
    if (m_outlineShown)
    {
        m_damage.Add(m_outline);
    }
    m_outline = outline;
    m_outlineShown = true;
    m_damage.Add(outline);
}

void Scene::HideOutline() noexcept
{
    if (m_outlineShown)
    {
        m_outlineShown = false;
        m_damage.Add(m_outline);
    }
}

void Scene::DamageAll() noexcept
{
    m_damage.Add(Output());
}

Size Scene::Count() const noexcept
{
    return m_count;
}

Size Scene::At(const Size index) const noexcept
{
    return index < m_count ? m_order[index] : NoWindow;
}

Size Scene::TopmostAt(const Point point) const noexcept
{
    for (Size index{m_count}; index != 0U; --index)
    {
        const Size slot = m_order[index - 1U];
        if (ContainsRect(m_windows[slot].Frame, Rect{.X = point.X, .Y = point.Y, .Width = 1U, .Height = 1U}))
        {
            return slot;
        }
    }
    return NoWindow;
}

Rect Scene::Frame(const Size slot) const noexcept
{
    return IsShown(slot) ? m_windows[slot].Frame : Rect{};
}

WindowLook Scene::Look(const Size slot) const noexcept
{
    return IsShown(slot) ? m_windows[slot].Look : HiddenLook;
}

bool Scene::HasDamage() const noexcept
{
    if (m_damage.Count() != 0U)
    {
        return true;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (!Graphics::IsEmpty(m_windows[m_order[index]].Pending))
        {
            return true;
        }
    }
    return false;
}

Size Scene::Compose(const MemorySurface& output) noexcept
{
    if (!Usable(output))
    {
        return 0U;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        Window& window = m_windows[m_order[index]];
        m_damage.Add(Graphics::Intersect(window.Pending, ContentRect(window.Frame)));
        window.Pending = Rect{};
    }
    const Size count = m_damage.Count();
    for (Size index{0U}; index < count; ++index)
    {
        ComposeRect(output, m_damage.At(index), true);
    }
    m_composed = m_damage;
    m_damage.Clear();
    return count;
}

const Graphics::Composition::DamageList& Scene::Composed() const noexcept
{
    return m_composed;
}

void Scene::RenderAll(const MemorySurface& output) const noexcept
{
    if (Usable(output))
    {
        ComposeRect(output, Output(), false);
    }
}

bool Scene::Usable(const MemorySurface& output) const noexcept
{
    return (m_width != 0U) && Graphics::IsUsable(output) && (output.Width == m_width) && (output.Height == m_height) &&
           (output.Format == m_format);
}

Size Scene::OrderIndex(const Size slot) const noexcept
{
    if (!IsShown(slot))
    {
        return NoWindow;
    }
    for (Size index{0U}; index < m_count; ++index)
    {
        if (m_order[index] == slot)
        {
            return index;
        }
    }
    return NoWindow;
}

Rect Scene::SwitcherBox() const noexcept
{
    return m_ui == nullptr
               ? SwitcherRect(m_width, m_height, TextColumns(m_switcherText, m_switcherLength))
               : SwitcherRectPixels(m_width, m_height, m_ui->Width(m_switcherText, m_switcherLength, false));
}
void Scene::DamageTitle(const Size slot) noexcept
{
    m_damage.Add(TitleBarRect(m_windows[slot].Frame));
}

void Scene::ComposeRect(const MemorySurface& output, const Rect clip, const bool shortcut) const noexcept
{
    const Rect area = Graphics::ClipToSurface(output, clip);
    if (Graphics::IsEmpty(area))
    {
        return;
    }
    Size first{0U};
    bool covered{false};
    if (shortcut)
    {
        for (Size index{m_count}; index != 0U; --index)
        {
            if (ContainsRect(m_windows[m_order[index - 1U]].Frame, area))
            {
                first = index - 1U;
                covered = true;
                break;
            }
        }
    }
    if (!covered)
    {
        const MemorySurface view = ClipSurface(output, area);
        Graphics::FillRect(view, Graphics::Bounds(view), Desktop);
    }
    for (Size index{first}; index < m_count; ++index)
    {
        PaintWindow(output, area, m_windows[m_order[index]]);
    }
    if (m_outlineShown)
    {
        PaintOutline(output, area, m_outline);
    }
    if (m_switcher)
    {
        PaintSwitcher(output, area, SwitcherBox(), m_switcherText, m_switcherLength, m_ui);
    }
}

void Scene::PaintWindow(const MemorySurface& output, const Rect clip, const Window& window) const noexcept
{
    if (Graphics::IsEmpty(Graphics::Intersect(window.Frame, clip)))
    {
        return;
    }
    const TitleView title{.Name = window.Name,
                          .NameLength = window.NameLength,
                          .Subtitle = window.Subtitle,
                          .SubtitleLength = window.SubtitleLength,
                          .Class = window.Class,
                          .State = window.Look.State,
                          .NotResponding = window.Look.NotResponding,
                          .Maximized = window.Look.Maximized,
                          .Close = window.Look.Close,
                          .Maximize = window.Look.Maximize};
    PaintFrame(output, clip, window.Frame, title, m_ui);

    const Rect content = ContentRect(window.Frame);
    const Rect area = Graphics::Intersect(content, clip);
    if (Graphics::IsEmpty(area))
    {
        return;
    }
    const MemorySurface view = ClipSurface(output, area);
    const bool image = window.HasImage && Graphics::IsUsable(window.Image) && (window.Image.Format == m_format);
    const UInt32 width = image ? Smaller(window.Image.Width, content.Width) : 0U;
    const UInt32 height = image ? Smaller(window.Image.Height, content.Height) : 0U;
    if (image)
    {
        static_cast<void>(Graphics::CopyRect(view, window.Image, Rect{.X = 0, .Y = 0, .Width = width, .Height = height},
                                             Point{.X = static_cast<Int32>(static_cast<Int64>(content.X) - area.X),
                                                   .Y = static_cast<Int32>(static_cast<Int64>(content.Y) - area.Y)}));
    }
    // The image's uncovered right strip (full height), then the bottom strip.
    FillInside(view, area,
               Rect{.X = static_cast<Int32>(static_cast<Int64>(content.X) + width),
                    .Y = content.Y,
                    .Width = content.Width - width,
                    .Height = content.Height},
               Face);
    FillInside(view, area,
               Rect{.X = content.X,
                    .Y = static_cast<Int32>(static_cast<Int64>(content.Y) + height),
                    .Width = width,
                    .Height = content.Height - height},
               Face);
}
} // namespace Asteria::User::Session
