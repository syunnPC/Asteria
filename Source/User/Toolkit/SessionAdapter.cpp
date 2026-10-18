// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/SessionAdapter.hpp"

namespace Asteria::User::Toolkit
{
void SessionAdapter::ClearWords(const UInt64 first) noexcept
{
    for (auto& word : m_words)
    {
        word = 0U;
    }
    m_words[0U] = first;
}
bool SessionAdapter::Send(const UInt32 method) noexcept
{
    const bool sent = m_connection.Request(method, m_words);
    m_healthy = m_healthy && sent;
    return sent;
}
Status SessionAdapter::Bind(Form& form, const UInt32 width, const UInt32 height) noexcept
{
    if (m_form != nullptr || !form.App().IsUiThread())
    {
        return Status::Invalid;
    }
    m_images[0U] = m_connection.Image(0U);
    m_images[1U] = m_connection.Image(1U);
    for (const auto& view : m_images)
    {
        Session::ClientImagePlan plan{};
        if (view.Width > MaximumAxis || view.Height > MaximumAxis || view.Stride > MaximumAxis ||
            !plan.Configure(view.Stride, view.Height) || !User::Graphics::IsUsable(view))
        {
            return Status::Limit;
        }
    }
    if (width == 0U || height == 0U || width > m_images[0U].Width || height > m_images[0U].Height)
    {
        return Status::Limit;
    }
    const auto bound = form.Bind(m_images[0U], m_images[1U], *this);
    if (bound != Status::Ok)
    {
        return bound;
    }
    m_form = &form;
    if (form.Resize(width, height) != Status::Ok)
    {
        return Status::Limit;
    }
    ClearWords();
    m_words[1U] = Session::SessionToplevelRole;
    if (!Send(Session::SessionCreateSurfaceMethod))
    {
        return Status::Invalid;
    }
    ClearWords();
    m_words[1U] = Session::PackSessionPair(48U, 32U);
    m_words[2U] = Session::PackSessionPair(m_images[0U].Width, m_images[0U].Height);
    m_words[3U] = Session::PackSessionPair(1U, 1U);
    return Send(Session::SessionSetSizeHintsMethod) ? Status::Ok : Status::Invalid;
}
bool SessionAdapter::Present(const UInt32 image, const Rect* const damage, const UInt32 count) noexcept
{
    if (!Healthy() || m_form == nullptr || m_form->UiClosed() || image >= 2U || (m_lent & (1U << image)) != 0U ||
        damage == nullptr || count == 0U || count > Session::SessionMaximumDamage || m_serial == ~UInt64{0U})
    {
        return false;
    }
    const Rect bounds = m_form->UiBounds();
    // Redefine only a returned image, then lend the complete view.
    ClearWords(image + 1U);
    m_words[1U] = image + 1U;
    m_words[3U] = Session::PackSessionPair(bounds.Width, bounds.Height);
    m_words[4U] = m_images[image].Stride;
    m_words[5U] = static_cast<UInt64>(m_images[image].Format);
    if (!Send(Session::SessionDefineImageMethod))
    {
        return false;
    }
    ClearWords();
    m_words[1U] = image + 1U;
    m_words[2U] = m_serial + 1U;
    m_words[3U] = Session::SessionWantFrameShown;
    m_words[4U] = m_arrange;
    m_words[5U] = count;
    for (UInt32 i{}; i < count; ++i)
    {
        const Rect clipped = User::Graphics::Intersect(damage[i], bounds);
        if (User::Graphics::IsEmpty(clipped))
        {
            return false;
        }
        m_words[6U + i] = Session::PackSessionDamage(static_cast<UInt32>(clipped.X), static_cast<UInt32>(clipped.Y),
                                                     clipped.Width, clipped.Height);
    }
    if (!Send(Session::SessionPresentMethod))
    {
        return false;
    }
    ++m_serial;
    m_lent |= 1U << image;
    return true;
}
void SessionAdapter::Pointer(const EventKind kind, const Point position, const UInt32 buttons,
                             const UInt32 button) noexcept
{
    Event event{kind};
    event.Position = position;
    event.Buttons = buttons;
    event.Button = button;
    static_cast<void>(m_form->App().Dispatch(*m_form, event));
}
namespace
{
[[nodiscard]] Key TranslateKey(const Foundation::UInt16 usage) noexcept
{
    // USB HID Usage Tables v1.5, §10 table 10.1.
    switch (usage)
    {
    case 4U:
        return Key::A;
    case 40U:
    case 88U:
        return Key::Enter;
    case 41U:
        return Key::Escape;
    case 42U:
        return Key::Backspace;
    case 43U:
        return Key::Tab;
    case 44U:
        return Key::Space;
    case 74U:
        return Key::Home;
    case 76U:
        return Key::Delete;
    case 77U:
        return Key::End;
    case 79U:
        return Key::Right;
    case 80U:
        return Key::Left;
    case 81U:
        return Key::Down;
    case 82U:
        return Key::Up;
    default:
        return Key::None;
    }
}
} // Namespace
void SessionAdapter::KeyEvent(const Foundation::UInt16 usage, const UInt8 modifiers, const bool down) noexcept
{
    Event event{down ? EventKind::KeyDown : EventKind::KeyUp};
    event.Code = TranslateKey(usage);
    event.Shift = (modifiers & Input::KeymapShift) != 0U;
    event.ControlKey = (modifiers & Input::KeymapControl) != 0U;
    event.Alt = (modifiers & Input::KeymapAlt) != 0U;
    UInt8 textModifiers = modifiers & static_cast<UInt8>(~Input::KeymapControl);
    if (m_caps && usage >= 4U && usage <= 29U)
    {
        textModifiers ^= 2U;
    }
    const auto character = Input::LayoutKeyCharacter(m_layout, usage, textModifiers);
    event.Scalar = character.Present ? character.CodePoint : 0U;
    static_cast<void>(m_form->App().Dispatch(*m_form, event));
    if (down && character.Present && character.CodePoint >= 0x20U && character.CodePoint != 0x7FU &&
        !event.ControlKey && !event.Alt && (modifiers & Input::KeymapGui) == 0U)
    {
        event.Kind = EventKind::KeyPress;
        static_cast<void>(m_form->App().Dispatch(*m_form, event));
    }
}
void SessionAdapter::Handle(const Session::SessionEvent& event) noexcept
{
    using K = Session::SessionEventKind;
    const auto* const w = event.Words;
    if (event.Kind == K::ImageReturned)
    {
        for (UInt32 image{}; image < 2U; ++image)
        {
            if ((w[0U] & m_lent & (1U << image)) != 0U)
            {
                static_cast<void>(m_form->ReturnImage(image));
            }
        }
        m_lent &= ~static_cast<UInt32>(w[0U]);
        return;
    }
    if (event.Kind == K::PoolRefused || event.Kind == K::ImageRefused || event.Kind == K::RequestRefused)
    {
        m_healthy = false;
        return;
    }
    if (w[0U] != 1U || m_form->UiClosed())
    {
        return;
    }
    switch (event.Kind)
    {
    case K::Arrange:
        if (m_form->Resize(static_cast<UInt32>(w[2U]), static_cast<UInt32>(w[2U] >> 32U)) != Status::Ok)
        {
            m_healthy = false;
            break;
        }
        m_arrange = w[1U];
        m_flags = w[3U];
        break;
    case K::FrameShown:
        m_shown = w[1U];
        break;
    case K::KeyFocus:
        m_focused = w[1U] != 0U;
        if (!m_focused)
        {
            m_savedFocus = m_form->App().UiFocus();
            m_repeat.Reset();
            m_buttons = 0U;
            static_cast<void>(m_form->App().ReleaseInput(*m_form));
        }
        else if (m_savedFocus != nullptr)
        {
            static_cast<void>(m_form->App().Focus(m_savedFocus));
        }
        break;
    case K::CloseAsked:
        static_cast<void>(m_form->Close());
        break;
    case K::PointerEnter:
    case K::PointerMotion:
        m_position = {static_cast<Int32>(w[1U]), static_cast<Int32>(w[1U] >> 32U)};
        Pointer(EventKind::MouseMove, m_position, static_cast<UInt32>(w[2U]));
        break;
    case K::PointerLeave:
        Pointer(EventKind::MouseMove, {-static_cast<Int32>(MaximumAxis), -static_cast<Int32>(MaximumAxis)}, m_buttons);
        break;
    case K::PointerButtons:
    {
        m_position = {static_cast<Int32>(w[1U]), static_cast<Int32>(w[1U] >> 32U)};
        const UInt32 buttons = static_cast<UInt32>(w[2U]);
        for (UInt32 bit{1U}; bit <= 4U; bit <<= 1U)
        {
            if (((buttons ^ m_buttons) & bit) != 0U)
            {
                Pointer((buttons & bit) != 0U ? EventKind::MouseDown : EventKind::MouseUp, m_position, buttons, bit);
            }
        }
        m_buttons = buttons;
        break;
    }
    case K::Wheel:
    {
        Event wheel{EventKind::MouseWheel};
        wheel.Position = m_position;
        wheel.Wheel = static_cast<Int32>(w[1U]);
        static_cast<void>(m_form->App().Dispatch(*m_form, wheel));
        break;
    }
    case K::Key:
    {
        const bool down = w[1U] == Session::SessionKeyDown;
        if (down && !m_focused)
        {
            break;
        }
        const auto usage = static_cast<Foundation::UInt16>(w[2U]);
        if (usage == 57U && down)
        {
            m_caps = !m_caps;
        }
        const Input::InputEvent key{.Kind = down ? Input::EventKind::KeyDown : Input::EventKind::KeyUp,
                                    .Modifiers = static_cast<UInt8>(w[3U]),
                                    .Usage = usage,
                                    .Source = 1U,
                                    .Dx = 0,
                                    .Dy = 0,
                                    .Buttons = 0U,
                                    .Reserved = {},
                                    .Wheel = 0};
        m_repeat.Feed(key, Now() * 1000U);
        KeyEvent(usage, key.Modifiers, down);
        break;
    }
    default:
        break;
    }
}
bool SessionAdapter::Poll(SourceEvent&) noexcept
{
    if (m_form == nullptr)
    {
        return false;
    }
    Session::SessionEvent event{};
    for (UInt32 i{}; i < Session::SessionMaximumEvents && m_connection.Poll(event); ++i)
    {
        Handle(event);
        if (m_observer != nullptr)
        {
            m_observer(m_observerContext, event);
        }
    }
    if (m_form->UiClosed() && !m_destroyed)
    {
        ClearWords();
        m_destroyed = Send(Session::SessionDestroySurfaceMethod);
        m_repeat.Reset();
    }
    if (!Healthy() || (m_destroyed && m_lent == 0U))
    {
        static_cast<void>(m_form->App().Stop());
        return false;
    }
    Input::InputEvent repeat{};
    if (m_focused && m_repeat.TakeDue(Now() * 1000U, repeat) != 0U)
    {
        KeyEvent(repeat.Usage, repeat.Modifiers, true);
    }
    return false;
}
void SessionAdapter::WaitUntil(const UInt64 deadline) noexcept
{
    const UInt64 now = Now();
    const UInt64 repeat = m_repeat.WaitMicroseconds(now * 1000U, Session::ClientRetryUs);
    const UInt64 next = now + (repeat + 999U) / 1000U;
    m_connection.WaitUntil(deadline < next ? deadline : next);
}
} // Namespace Asteria::User::Toolkit
