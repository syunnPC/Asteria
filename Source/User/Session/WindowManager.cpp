// SPDX-License-Identifier: Apache-2.0

#include "User/Session/WindowManager.hpp"
#include "User/Graphics/Composition/Divide.hpp"
#include "User/Input/Keymap.hpp"
namespace Asteria::User::Session
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::Int32;
using Foundation::Int64;
using Graphics::Rect;
using Graphics::Point;
using K = SessionEventKind;
namespace
{
[[nodiscard]] Int32 Clamp(const Int64 value, const Int64 low, const Int64 high) noexcept
{
    return static_cast<Int32>(value < low ? low : (value > high ? high : value));
}
[[nodiscard]] bool Near(const Point a, const Point b) noexcept
{
    const Int64 x = static_cast<Int64>(a.X) - b.X;
    const Int64 y = static_cast<Int64>(a.Y) - b.Y;
    return (x >= -static_cast<Int64>(DoubleClickDistance)) && (x <= DoubleClickDistance) &&
           (y >= -static_cast<Int64>(DoubleClickDistance)) && (y <= DoubleClickDistance);
}
} // Namespace
UInt32 SnapSize(const Int64 request, const UInt32 minimum, const UInt32 maximum, const UInt32 increment,
                const UInt32 base) noexcept
{
    const UInt32 low = minimum == 0U ? 1U : minimum;
    const UInt32 high = maximum == 0U ? static_cast<UInt32>(SessionMaximumExtent) : maximum;
    UInt32 size = static_cast<UInt32>(Clamp(request, low, high));
    if (increment != 0U)
    {
        const UInt32 sizeRemainder = size - Graphics::Composition::Divide(size, increment) * increment;
        const UInt32 baseRemainder = base - Graphics::Composition::Divide(base, increment) * increment;
        const UInt32 delta =
            sizeRemainder >= baseRemainder ? sizeRemainder - baseRemainder : sizeRemainder + increment - baseRemainder;
        if (delta <= size)
        {
            size -= delta;
        }
        if (size < low)
        {
            size += increment;
        }
        // Inconsistent hints may contain no lattice point in [min,max].
        // Bounds win in that case ( leaves this tie unspecified).
        if (size > high)
        {
            size = high;
        }
    }
    return size;
}
void WindowManager::Configure(Scene& scene, ClientEvents* const clients) noexcept
{
    m_scene = &scene;
    m_clients = clients;
    m_mruCount = 0U;
    m_focus = Scene::NoWindow;
    m_grab = Scene::NoWindow;
    m_hover = Scene::NoWindow;
    m_lastTitle = Scene::NoWindow;
    m_selected = Scene::NoWindow;
    m_cascade = 0U;
    m_launch = 0U;
    m_serial = 0U;
    m_lastClick = 0U;
    m_buttons = 0U;
    m_ignoredButtons = 0U;
    m_switching = false;
    m_newTerminal = false;
    m_outlineMove = false;
    m_grabOutline = false;
    for (auto& window : m_windows)
    {
        window = Window{};
    }
    for (auto& consumed : m_consumed)
    {
        consumed = 0U;
    }
    for (auto& owner : m_keyOwner)
    {
        owner = 0U;
    }
    for (auto& first : m_firstWindow)
    {
        first = false;
    }
}
void WindowManager::Event(const Size slot, const K kind, const UInt64 a, const UInt64 b, const UInt64 c,
                          const UInt64 now) noexcept
{
    if ((slot < Scene::MaximumWindows) && (m_clients != nullptr))
    {
        static_cast<void>(m_clients[slot >> 2U].Queue(SessionEvent{kind, {(slot & 3U) + 1U, a, b, c}}, now));
    }
}
bool WindowManager::Map(const Size slot, const UInt32 width, const UInt32 height, const UInt64 launch,
                        const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || (slot >= Scene::MaximumWindows) || (width == 0U) || (height == 0U) ||
        (width > SessionMaximumExtent) || (height > SessionMaximumExtent))
    {
        return false;
    }
    const Rect output = m_scene->Output();
    Rect frame{0, 0, width + FrameExtraWidth, height + FrameExtraHeight};
    if ((frame.Width <= output.Width) && (frame.Height <= output.Height))
    {
        UInt32 origin = CascadeOrigin + CascadeStep * m_cascade;
        if ((static_cast<UInt64>(origin) + frame.Width > output.Width) ||
            (static_cast<UInt64>(origin) + frame.Height > output.Height))
        {
            m_cascade = 0U;
            origin = CascadeOrigin;
        }
        frame.X = static_cast<Int32>(origin);
        frame.Y = static_cast<Int32>(origin);
        ++m_cascade;
    }
    if (!m_scene->Show(slot, frame))
    {
        return false;
    }
    m_windows[slot] = Window{};
    m_windows[slot].IgnoreUntil = now + SystemRaiseIgnoreUs;
    m_mru[m_mruCount++] = static_cast<Foundation::UInt8>(slot);
    // Only the first window may consume the client's launch.
    const bool first = !m_firstWindow[slot / SessionMaximumSurfaces];
    m_firstWindow[slot / SessionMaximumSurfaces] = true;
    if ((m_focus == Scene::NoWindow) || (first && (launch != 0U) && (launch == m_launch)))
    {
        Focus(slot, now);
    }
    else
    {
        auto look = m_scene->Look(slot);
        look.State = TitleState::Attention;
        m_scene->SetLook(slot, look);
    }
    Arrange(slot, now);
    return true;
}
void WindowManager::Unmap(const Size slot, const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || !m_scene->IsShown(slot))
    {
        return;
    }
    if (m_clients != nullptr)
    {
        m_clients[slot >> 2U].ReleaseKeys((slot & 3U) + 1U, now);
        m_clients[slot >> 2U].DestroySurface((slot & 3U) + 1U);
    }
    for (auto& owner : m_keyOwner)
    {
        if (owner == slot + 1U)
        {
            owner = 0U;
        }
    }
    for (Size i{0U}; i < m_mruCount; ++i)
    {
        if (m_mru[i] == slot)
        {
            for (Size j{i + 1U}; j < m_mruCount; ++j)
            {
                m_mru[j - 1U] = m_mru[j];
            }
            --m_mruCount;
            break;
        }
    }
    m_scene->Hide(slot);
    if (m_grab == slot)
    {
        m_grab = Scene::NoWindow;
        m_grabOutline = false;
        m_part = FramePart::None;
        m_scene->HideOutline();
    }
    if (m_lastTitle == slot)
    {
        m_lastTitle = Scene::NoWindow;
    }
    if (m_hover == slot)
    {
        m_hover = Scene::NoWindow;
    }
    if (m_focus == slot)
    {
        m_focus = Scene::NoWindow;
        Focus(m_mruCount == 0U ? Scene::NoWindow : m_mru[0U], now);
    }
    if (m_switching)
    {
        m_switching = false;
        m_scene->HideSwitcher();
    }
}
void WindowManager::Raise(const Size slot, const bool user, const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || !m_scene->IsShown(slot))
    {
        return;
    }
    m_scene->Raise(slot);
    if (!user)
    {
        m_windows[slot].IgnoreUntil = now + SystemRaiseIgnoreUs;
    }
}
void WindowManager::Focus(const Size slot, const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || ((slot != Scene::NoWindow) && !m_scene->IsShown(slot)) || (m_focus == slot))
    {
        return;
    }
    if (m_focus != Scene::NoWindow)
    {
        if (m_clients != nullptr)
        {
            m_clients[m_focus >> 2U].ReleaseKeys((m_focus & 3U) + 1U, now);
        }
        for (auto& owner : m_keyOwner)
        {
            if (owner == m_focus + 1U)
            {
                owner = 0U;
            }
        }
        Event(m_focus, K::KeyFocus, 0U, 0U, 0U, now);
        auto look = m_scene->Look(m_focus);
        look.State = TitleState::Unfocused;
        m_scene->SetLook(m_focus, look);
    }
    m_focus = slot;
    if (slot == Scene::NoWindow)
    {
        return;
    }
    Event(slot, K::KeyFocus, 1U, 0U, 0U, now);
    auto look = m_scene->Look(slot);
    look.State = TitleState::Focused;
    m_scene->SetLook(slot, look);
    m_scene->Raise(slot);
    Size index{0U};
    for (; index < m_mruCount; ++index)
    {
        if (m_mru[index] == slot)
        {
            break;
        }
    }
    for (; index > 0U; --index)
    {
        m_mru[index] = m_mru[index - 1U];
    }
    m_mru[0U] = static_cast<Foundation::UInt8>(slot);
}
bool WindowManager::SetHints(const Size slot, const SizeHints& hints) noexcept
{
    if ((slot >= Scene::MaximumWindows) || (hints.MinWidth > SessionMaximumExtent) ||
        (hints.MinHeight > SessionMaximumExtent) || (hints.MaxWidth > SessionMaximumExtent) ||
        (hints.MaxHeight > SessionMaximumExtent) || ((hints.MaxWidth != 0U) && (hints.MaxWidth < hints.MinWidth)) ||
        ((hints.MaxHeight != 0U) && (hints.MaxHeight < hints.MinHeight)) ||
        (hints.IncrementWidth > SessionMaximumExtent) || (hints.IncrementHeight > SessionMaximumExtent) ||
        (hints.BaseWidth > SessionMaximumExtent) || (hints.BaseHeight > SessionMaximumExtent))
    {
        return false;
    }
    m_windows[slot].Hints = hints;
    return true;
}
void WindowManager::Arrange(const Size slot, const UInt64 now) noexcept
{
    Window& window = m_windows[slot];
    if (window.Arrange != 0U)
    {
        window.PendingArrange = true;
        return;
    }
    const Rect content = ContentRect(m_scene->Frame(slot));
    const auto look = m_scene->Look(slot);
    window.Arrange = ++m_serial;
    window.PendingArrange = false;
    Event(slot, K::Arrange, window.Arrange, PackSessionPair(content.Width, content.Height),
          (slot == m_focus ? SessionArrangeFocused : 0U) | (look.Maximized ? SessionArrangeMaximized : 0U) |
              ((m_grab == slot) && (m_part >= FramePart::ResizeLeft) ? SessionArrangeResizing : 0U),
          now);
}
void WindowManager::AcceptArrange(const Size slot, const UInt64 serial, const UInt64 now) noexcept
{
    if ((slot >= Scene::MaximumWindows) || (m_scene == nullptr) || !m_scene->IsShown(slot))
    {
        return;
    }
    Window& window = m_windows[slot];
    if ((serial != 0U) && (serial == window.Arrange))
    {
        window.Arrange = 0U;
        if (window.PendingArrange)
        {
            Arrange(slot, now);
        }
    }
}
void WindowManager::Maximize(const Size slot, const UInt64 now) noexcept
{
    auto look = m_scene->Look(slot);
    Window& window = m_windows[slot];
    if (!look.Maximized)
    {
        window.Restore = m_scene->Frame(slot);
        m_scene->SetFrame(slot, m_scene->Output());
    }
    else
    {
        m_scene->SetFrame(slot, window.Restore);
    }
    look.Maximized = !look.Maximized;
    m_scene->SetLook(slot, look);
    Arrange(slot, now);
}
void WindowManager::Close(const Size slot, const UInt64 now) noexcept
{
    if (slot != Scene::NoWindow)
    {
        Event(slot, K::CloseAsked, 0U, 0U, 0U, now);
    }
}
void WindowManager::Move(const Point position, const UInt64 now) noexcept
{
    const Int64 dx = static_cast<Int64>(position.X) - m_anchor.X;
    const Int64 dy = static_cast<Int64>(position.Y) - m_anchor.Y;
    Rect frame = m_start;
    const Rect output = m_scene->Output();
    if (m_part == FramePart::Title)
    {
        if (m_scene->Look(m_grab).Maximized)
        {
            return;
        }
        // Movement ends a click candidate, including movement beyond an edge where the clamped
        // frame itself cannot move further.
        if (!Near(position, m_anchor))
        {
            m_lastTitle = Scene::NoWindow;
        }
        const Int64 keep = frame.Width < VisibleTitleWidth ? frame.Width : VisibleTitleWidth;
        frame.X =
            Clamp(static_cast<Int64>(m_start.X) + dx, keep - frame.Width, static_cast<Int64>(output.Width) - keep);
        frame.Y = Clamp(static_cast<Int64>(m_start.Y) + dy, -static_cast<Int64>(FrameWidth),
                        output.Height > FrameWidth + TitleBarHeight ? output.Height - FrameWidth - TitleBarHeight : 0U);
        m_dragFrame = frame;
        if (m_grabOutline)
        {
            m_scene->ShowOutline(frame);
        }
        else
        {
            m_scene->SetFrame(m_grab, frame);
        }
        return;
    }
    const bool left = (m_part == FramePart::ResizeLeft) || (m_part == FramePart::ResizeTopLeft) ||
                      (m_part == FramePart::ResizeBottomLeft);
    const bool right = (m_part == FramePart::ResizeRight) || (m_part == FramePart::ResizeTopRight) ||
                       (m_part == FramePart::ResizeBottomRight);
    const bool top = (m_part == FramePart::ResizeTop) || (m_part == FramePart::ResizeTopLeft) ||
                     (m_part == FramePart::ResizeTopRight);
    const bool bottom = (m_part == FramePart::ResizeBottom) || (m_part == FramePart::ResizeBottomLeft) ||
                        (m_part == FramePart::ResizeBottomRight);
    if (!(left || right || top || bottom))
    {
        return;
    }
    const auto& hints = m_windows[m_grab].Hints;
    if (left || right)
    {
        frame.Width =
            FrameExtraWidth + SnapSize(static_cast<Int64>(m_start.Width) - FrameExtraWidth + (left ? -dx : dx),
                                       hints.MinWidth, hints.MaxWidth, hints.IncrementWidth, hints.BaseWidth);
        if (left)
        {
            frame.X = Clamp(static_cast<Int64>(m_start.X) + m_start.Width - frame.Width, -2147483647LL, 2147483647LL);
        }
    }
    if (top || bottom)
    {
        frame.Height =
            FrameExtraHeight + SnapSize(static_cast<Int64>(m_start.Height) - FrameExtraHeight + (top ? -dy : dy),
                                        hints.MinHeight, hints.MaxHeight, hints.IncrementHeight, hints.BaseHeight);
        if (top)
        {
            frame.Y = Clamp(static_cast<Int64>(m_start.Y) + m_start.Height - frame.Height, -2147483647LL, 2147483647LL);
        }
    }
    m_scene->SetFrame(m_grab, frame);
    Arrange(m_grab, now);
}
void WindowManager::Pointer(const Point position, const Foundation::UInt8 physicalButtons, const Int32 wheel,
                            const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || (physicalButtons > 7U))
    {
        return;
    }
    m_ignoredButtons = static_cast<Foundation::UInt8>(m_ignoredButtons & physicalButtons);
    const Foundation::UInt8 buttons = static_cast<Foundation::UInt8>(physicalButtons & ~m_ignoredButtons);
    const bool press = (buttons & static_cast<Foundation::UInt8>(~m_buttons)) != 0U;
    Size target = m_grab != Scene::NoWindow ? m_grab : m_scene->TopmostAt(position);
    if (press && (target != Scene::NoWindow) && (now < m_windows[target].IgnoreUntil))
    {
        m_ignoredButtons = static_cast<Foundation::UInt8>(m_ignoredButtons | (buttons & ~m_buttons));
        m_position = position;
        return;
    }
    if ((m_grab == Scene::NoWindow) && (target != Scene::NoWindow) &&
        (HitTest(m_scene->Frame(target), position, m_scene->Look(target).Maximized) != FramePart::Content))
    {
        if (!press)
        {
            target = Scene::NoWindow;
        }
    }
    if ((m_grab == Scene::NoWindow) && (target != m_hover))
    {
        if (m_hover != Scene::NoWindow)
        {
            const Rect content = ContentRect(m_scene->Frame(m_hover));
            Event(m_hover, K::PointerLeave,
                  PackSessionPosition(Clamp(static_cast<Int64>(position.X) - content.X, -2147483648LL, 2147483647LL),
                                      Clamp(static_cast<Int64>(position.Y) - content.Y, -2147483648LL, 2147483647LL)),
                  0U, 0U, now);
        }
        m_hover = target;
        if (target != Scene::NoWindow)
        {
            const Rect content = ContentRect(m_scene->Frame(target));
            Event(target, K::PointerEnter,
                  PackSessionPosition(Clamp(static_cast<Int64>(position.X) - content.X, -2147483648LL, 2147483647LL),
                                      Clamp(static_cast<Int64>(position.Y) - content.Y, -2147483648LL, 2147483647LL)),
                  0U, 0U, now);
        }
    }
    if (press && (m_grab == Scene::NoWindow) && (target != Scene::NoWindow))
    {
        Focus(target, now);
        Raise(target, true, now);
        m_grab = target;
        m_anchor = position;
        m_start = m_scene->Frame(target);
        m_dragFrame = m_start;
        m_part = HitTest(m_start, position, m_scene->Look(target).Maximized);
        // One move policy for the whole grab. A Presented fence may change LastPass while input is
        // still held down.
        m_grabOutline = (m_part == FramePart::Title) && !m_scene->Look(target).Maximized && m_outlineMove;
        if ((m_part == FramePart::Title) && (m_lastTitle == target) && (now - m_lastClick <= DoubleClickUs) &&
            Near(position, m_clickPoint))
        {
            // This press is a maximize/restore gesture. Neither Move nor release may overwrite it
            // with the saved drag frame.
            Maximize(target, now);
            m_lastTitle = Scene::NoWindow;
            m_part = FramePart::None;
            m_grabOutline = false;
        }
        else if (m_part == FramePart::Title)
        {
            m_lastTitle = target;
            m_lastClick = now;
            m_clickPoint = position;
        }
        else
        {
            m_lastTitle = Scene::NoWindow;
        }
        if ((m_part == FramePart::CloseButton) || (m_part == FramePart::MaximizeButton))
        {
            auto look = m_scene->Look(target);
            if (m_part == FramePart::CloseButton)
            {
                look.Close = ButtonState::Pressed;
            }
            else
            {
                look.Maximize = ButtonState::Pressed;
            }
            m_scene->SetLook(target, look);
        }
    }
    if (m_grab != Scene::NoWindow)
    {
        target = m_grab;
        if (buttons != 0U)
        {
            Move(position, now);
        }
    }
    if (target != Scene::NoWindow)
    {
        const Rect content = ContentRect(m_scene->Frame(target));
        const UInt64 local =
            PackSessionPosition(Clamp(static_cast<Int64>(position.X) - content.X, -2147483647LL, 2147483647LL),
                                Clamp(static_cast<Int64>(position.Y) - content.Y, -2147483647LL, 2147483647LL));
        const FramePart part = m_grab == target ? m_part : FramePart::Content;
        if (part == FramePart::Content)
        {
            if (buttons != m_buttons)
            {
                Event(target, K::PointerButtons, local, buttons, 0U, now);
            }
            if ((position.X != m_position.X) || (position.Y != m_position.Y))
            {
                Event(target, K::PointerMotion, local, buttons, 0U, now);
            }
            if (wheel != 0)
            {
                Event(target, K::Wheel, static_cast<UInt32>(wheel), 0U, 0U, now);
            }
        }
    }
    if ((buttons == 0U) && (m_grab != Scene::NoWindow))
    {
        const Size slot = m_grab;
        const FramePart hit = HitTest(m_scene->Frame(slot), position, m_scene->Look(slot).Maximized);
        if ((m_part == FramePart::CloseButton) || (m_part == FramePart::MaximizeButton))
        {
            auto look = m_scene->Look(slot);
            look.Close = ButtonState::Raised;
            look.Maximize = ButtonState::Raised;
            m_scene->SetLook(slot, look);
            if (hit == m_part)
            {
                if (m_part == FramePart::CloseButton)
                {
                    Close(slot, now);
                }
                else
                {
                    Maximize(slot, now);
                }
            }
        }
        if ((m_part == FramePart::Title) && m_grabOutline)
        {
            m_scene->SetFrame(slot, m_dragFrame);
            m_scene->HideOutline();
        }
        m_grab = Scene::NoWindow;
        m_grabOutline = false;
        if (m_part >= FramePart::ResizeLeft)
        {
            Arrange(slot, now);
        }
        m_part = FramePart::None;
    }
    m_position = position;
    m_buttons = buttons;
}
void WindowManager::Switch(const bool reverse) noexcept
{
    if (m_mruCount == 0U)
    {
        return;
    }
    Size index{0U};
    if (m_switching)
    {
        for (; index < m_mruCount; ++index)
        {
            if (m_mru[index] == m_selected)
            {
                break;
            }
        }
    }
    index = reverse ? (index == 0U ? m_mruCount - 1U : index - 1U) : (index + 1U == m_mruCount ? 0U : index + 1U);
    m_selected = m_mru[index];
    m_switching = true;
    // The scene exposes the trusted whole title, so the switcher uses the
    // same sanitization and not-responding suffix as the decoration.
    Foundation::UInt8 text[TitleTextBytes]{};
    const Size length = m_scene->Title(m_selected, text);
    m_scene->ShowSwitcher(text, length);
}
bool WindowManager::Shortcut(const Foundation::UInt16 usage, const Foundation::UInt8 modifiers, const bool down,
                             const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || (usage < 4U) || (usage > 0xE7U))
    {
        return false;
    }
    const bool alt = (modifiers & Input::KeymapAlt) != 0U;
    const bool control = (modifiers & Input::KeymapControl) != 0U;
    if (!down && ((usage == 0xE2U) || (usage == 0xE6U)) && m_switching)
    {
        Focus(m_selected, now);
        m_scene->HideSwitcher();
        m_switching = false;
        return true;
    }
    if (m_consumed[usage] != 0U)
    {
        if (!down)
        {
            m_consumed[usage] = 0U;
        }
        return true;
    }
    if (down && alt && ((modifiers & Input::KeymapGui) == 0U) &&
        ((usage == 0x2BU) || (usage == 0x3DU) || (control && (usage == 0x17U))))
    {
        m_consumed[usage] = 1U;
        if (usage == 0x2BU)
        {
            Switch((modifiers & Input::KeymapShift) != 0U);
        }
        else if (usage == 0x3DU)
        {
            Close(m_focus, now);
        }
        else
        {
            m_newTerminal = true;
        }
        return true;
    }
    return false;
}
bool WindowManager::Key(const Foundation::UInt16 usage, const Foundation::UInt8 modifiers, const bool down,
                        const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || (usage < 4U) || (usage > 0xE7U))
    {
        return false;
    }
    if (Shortcut(usage, modifiers, down, now))
    {
        return true;
    }
    if (down)
    {
        if (m_focus == Scene::NoWindow)
        {
            return false;
        }
        m_keyOwner[usage] = static_cast<Foundation::UInt8>(m_focus + 1U);
        Event(m_focus, K::Key, SessionKeyDown, usage, modifiers, now);
    }
    else if (m_keyOwner[usage] != 0U)
    {
        Event(m_keyOwner[usage] - 1U, K::Key, SessionKeyUp, usage, modifiers, now);
        m_keyOwner[usage] = 0U;
    }
    return false;
}
void WindowManager::Tick(const UInt64 now) noexcept
{
    if ((m_scene == nullptr) || (m_clients == nullptr))
    {
        return;
    }
    for (Size i{0U}; i < SessionMaximumClients; ++i)
    {
        m_clients[i].Tick(now);
    }
    for (Size i{0U}; i < Scene::MaximumWindows; ++i)
    {
        if (m_scene->IsShown(i))
        {
            auto look = m_scene->Look(i);
            look.NotResponding = m_clients[i >> 2U].NotResponding();
            m_scene->SetLook(i, look);
        }
    }
}
} // Namespace Asteria::User::Session
