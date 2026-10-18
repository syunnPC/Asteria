// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Control.hpp"

#include "User/Toolkit/Form.hpp"

namespace Asteria::User::Toolkit
{
Control::Control(Application& application, Kind kind) noexcept : m_application{application}, m_kind{kind}
{
    m_bounds = {0, 0, application.UiTheme().DefaultWidth, application.UiTheme().DefaultHeight};
    m_baseline = m_bounds;
    m_registered = application.Register(*this) == Status::Ok;
}
Control::~Control() noexcept
{
    // Borrowed controls must be quiescent and destroyed on their owning UI thread.
    if (m_parent != nullptr)
    {
        UiInvalidate();
        for (UInt32 i{}; i < m_parent->m_childCount; ++i)
        {
            if (m_parent->m_children[i] != this)
            {
                continue;
            }
            for (UInt32 j{i + 1U}; j < m_parent->m_childCount; ++j)
            {
                m_parent->m_children[j - 1U] = m_parent->m_children[j];
            }
            --m_parent->m_childCount;
            break;
        }
        m_parent = nullptr;
    }
    for (UInt32 i{}; i < m_childCount; ++i)
    {
        m_children[i]->m_parent = nullptr;
    }
    m_application.Unregister(*this);
}
Status Control::Add(Control& child) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (&child.m_application != &m_application || &child == this || child.m_kind == Kind::Form ||
                child.m_parent != nullptr || UiDescendantOf(&child) || !m_registered || !child.m_registered)
            {
                return Status::Invalid;
            }
            if (m_childCount == MaximumChildren)
            {
                return Status::Limit;
            }
            UInt32 depth{1U};
            for (Control* at{this}; at->m_parent != nullptr; at = at->m_parent)
            {
                ++depth;
            }
            // Existing subtree depth is checked without recursion or a large stack.
            Control* stack[MaximumDepth]{};
            UInt32 indices[MaximumDepth]{};
            UInt32 level{};
            stack[0U] = &child;
            for (UInt32 work{}; work < MaximumControls * 2U; ++work)
            {
                if (depth + level + 1U > MaximumDepth)
                {
                    return Status::Limit;
                }
                Control* current{stack[level]};
                if (indices[level] < current->m_childCount)
                {
                    Control* next{current->m_children[indices[level]++]};
                    ++level;
                    stack[level] = next;
                    indices[level] = 0U;
                }
                else if (level == 0U)
                {
                    break;
                }
                else
                {
                    --level;
                }
            }
            child.m_parent = this;
            child.m_baseline = child.m_bounds;
            const Rect client{UiClient()};
            child.m_parentSize = {static_cast<Int32>(client.Width), static_cast<Int32>(client.Height)};
            child.m_tabOrder = m_childCount;
            m_children[m_childCount++] = &child;
            UiLayout();
            UiInvalidate();
            return Status::Ok;
        });
}
Status Control::Remove(Control& child) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            for (UInt32 i{}; i < m_childCount; ++i)
            {
                if (m_children[i] != &child)
                {
                    continue;
                }
                child.UiInvalidate();
                if (m_application.UiFocus() != nullptr && m_application.UiFocus()->UiDescendantOf(&child))
                {
                    static_cast<void>(m_application.Focus(nullptr));
                }
                for (UInt32 j{i + 1U}; j < m_childCount; ++j)
                {
                    m_children[j - 1U] = m_children[j];
                }
                --m_childCount;
                child.m_parent = nullptr;
                UiLayout();
                UiInvalidate();
                return Status::Ok;
            }
            return Status::Invalid;
        });
}
Status Control::BringToFront() noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (m_parent == nullptr)
            {
                return Status::Invalid;
            }
            for (UInt32 i{}; i < m_parent->m_childCount; ++i)
            {
                if (m_parent->m_children[i] != this)
                {
                    continue;
                }
                for (UInt32 j{i + 1U}; j < m_parent->m_childCount; ++j)
                {
                    m_parent->m_children[j - 1U] = m_parent->m_children[j];
                }
                m_parent->m_children[m_parent->m_childCount - 1U] = this;
                UiInvalidate();
                break;
            }
            return Status::Ok;
        });
}
void Control::UiSetBounds(Rect bounds, bool baseline) noexcept
{
    const bool resize{bounds.Width != m_bounds.Width || bounds.Height != m_bounds.Height};
    const bool changed{resize || bounds.X != m_bounds.X || bounds.Y != m_bounds.Y};
    if (changed)
    {
        UiInvalidate();
        m_bounds = bounds;
        UiInvalidate();
    }
    if (baseline)
    {
        m_baseline = bounds;
        const Rect client{m_parent != nullptr ? m_parent->UiClient() : bounds};
        m_parentSize = {static_cast<Int32>(client.Width), static_cast<Int32>(client.Height)};
    }
    if (resize)
    {
        UiLayout();
        Event event{EventKind::Resize};
        UiEmit(event);
    }
}
Status Control::SetBounds(Rect bounds) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (bounds.X < -static_cast<Int32>(MaximumAxis) || bounds.Y < -static_cast<Int32>(MaximumAxis) ||
                bounds.X > static_cast<Int32>(MaximumAxis) || bounds.Y > static_cast<Int32>(MaximumAxis) ||
                bounds.Width > MaximumAxis || bounds.Height > MaximumAxis)
            {
                return Status::Invalid;
            }
            UiSetBounds(bounds, true);
            return Status::Ok;
        });
}
Status Control::UiAssignText(TextView text) noexcept
{
    if (!ValidText(text, MaximumText))
    {
        return text.Length > MaximumText ? Status::Limit : Status::Invalid;
    }
    if (!UiAcceptText(text))
    {
        return Status::Invalid;
    }
    bool same{text.Length == m_textLength};
    for (UInt32 i{}; same && i < text.Length; ++i)
    {
        same = m_text[i] == text.Data[i];
    }
    if (same)
    {
        return Status::Ok;
    }
    // memmove permits callers to assign a substring of the current UI text.
    if (text.Length != 0U)
    {
        __builtin_memmove(m_text, text.Data, text.Length);
    }
    m_textLength = text.Length;
    m_text[m_textLength] = '\0';
    UiTextChanged();
    UiInvalidate();
    if (m_autoSize && m_parent != nullptr)
    {
        m_parent->UiLayout();
    }
    Event event{EventKind::TextChanged};
    UiEmit(event);
    return Status::Ok;
}
void Control::UiTextChanged() noexcept
{
}
bool Control::UiAcceptText(TextView) const noexcept
{
    return true;
}
Status Control::SetText(TextView text) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            return UiAssignText(text);
        });
}
Status Control::CopyText(char* buffer, UInt32 capacity, UInt32& length) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            length = m_textLength;
            if (buffer == nullptr || capacity <= m_textLength)
            {
                return Status::Limit;
            }
            __builtin_memcpy(buffer, m_text, m_textLength + 1U);
            return Status::Ok;
        });
}
void Control::UiFlagChanged() noexcept
{
    if (!UiEffective())
    {
        m_pressed = false;
        m_hot = false;
        if (m_application.UiFocus() != nullptr && m_application.UiFocus()->UiDescendantOf(this))
        {
            static_cast<void>(m_application.Focus(nullptr));
        }
    }
    for (UInt32 i{}; i < m_childCount; ++i)
    {
        m_children[i]->UiFlagChanged();
    }
    UiInvalidate();
    if (m_parent != nullptr)
    {
        m_parent->UiLayout();
    }
}
Status Control::SetVisible(bool visible) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            UiInvalidate();
            m_visible = visible;
            UiFlagChanged();
            return Status::Ok;
        });
}
Status Control::SetEnabled(bool enabled) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            m_enabled = enabled;
            UiFlagChanged();
            return Status::Ok;
        });
}
Status Control::SetTabOrder(UInt32 order, bool stop) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            m_tabOrder = order;
            m_tabStop = stop;
            return Status::Ok;
        });
}
Status Control::SetLayout(Anchor anchor, Dock dock, bool autoSize) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (static_cast<UInt8>(anchor) > 15U || static_cast<UInt8>(dock) > static_cast<UInt8>(Dock::Fill))
            {
                return Status::Invalid;
            }
            m_anchor = anchor;
            m_dock = dock;
            m_autoSize = autoSize;
            m_baseline = m_bounds;
            const Rect client{m_parent != nullptr ? m_parent->UiClient() : m_bounds};
            m_parentSize = {static_cast<Int32>(client.Width), static_cast<Int32>(client.Height)};
            if (m_parent != nullptr)
            {
                m_parent->UiLayout();
            }
            return Status::Ok;
        });
}
Status Control::SetSpacing(Insets padding, Insets margin) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (padding.Left > MaximumAxis || padding.Right > MaximumAxis || padding.Top > MaximumAxis ||
                padding.Bottom > MaximumAxis || margin.Left > MaximumAxis || margin.Right > MaximumAxis ||
                margin.Top > MaximumAxis || margin.Bottom > MaximumAxis)
            {
                return Status::Invalid;
            }
            m_padding = padding;
            m_margin = margin;
            UiLayout();
            if (m_parent != nullptr)
            {
                m_parent->UiLayout();
            }
            UiInvalidate();
            return Status::Ok;
        });
}
Status Control::SetMnemonic(UInt32 scalar) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!User::Text::IsScalar(scalar))
            {
                return Status::Invalid;
            }
            m_mnemonic = scalar;
            return Status::Ok;
        });
}
Status Control::SetHandler(EventKind kind, Handler handler) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (kind >= EventKind::Count)
            {
                return Status::Invalid;
            }
            m_handlers[static_cast<UInt32>(kind)] = handler;
            return Status::Ok;
        });
}
Status Control::Invalidate() noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            UiInvalidate();
            return Status::Ok;
        });
}
ControlState Control::State() noexcept
{
    ControlState result{};
    static_cast<void>(m_application.Invoke(
        [&]() noexcept
        {
            result = {m_bounds,  m_visible,  m_enabled,   m_application.UiFocus() == this, m_hot, m_pressed,
                      m_checked, m_selected, m_textLength};
        }));
    return result;
}
bool Control::UiDescendantOf(const Control* parent) const noexcept
{
    const Control* at{this};
    for (UInt32 depth{}; at != nullptr && depth < MaximumDepth; ++depth, at = at->m_parent)
    {
        if (at == parent)
        {
            return true;
        }
    }
    return false;
}
bool Control::UiChildVisible(const Control& child) const noexcept
{
    return child.m_visible;
}
bool Control::UiEffective() const noexcept
{
    const Control* at{this};
    for (UInt32 depth{}; at != nullptr && depth < MaximumDepth; ++depth, at = at->m_parent)
    {
        if (!at->m_visible || !at->m_enabled || !at->m_registered ||
            (at->m_parent != nullptr && !at->m_parent->UiChildVisible(*at)))
        {
            return false;
        }
    }
    return true;
}
Form* Control::UiForm() noexcept
{
    Control* at{this};
    for (UInt32 depth{}; at != nullptr && depth < MaximumDepth; ++depth, at = at->m_parent)
    {
        if (at->m_kind == Kind::Form)
        {
            return static_cast<Form*>(at);
        }
    }
    return nullptr;
}
const Form* Control::UiForm() const noexcept
{
    const Control* at{this};
    for (UInt32 depth{}; at != nullptr && depth < MaximumDepth; ++depth, at = at->m_parent)
    {
        if (at->m_kind == Kind::Form)
        {
            return static_cast<const Form*>(at);
        }
    }
    return nullptr;
}
Rect Control::UiBounds() const noexcept
{
    Rect result{m_bounds};
    const Control* at{m_parent};
    for (UInt32 depth{}; at != nullptr && depth < MaximumDepth; ++depth, at = at->m_parent)
    {
        const Point scroll{at->UiScroll()};
        result.X += at->m_bounds.X - scroll.X;
        result.Y += at->m_bounds.Y - scroll.Y;
    }
    return result;
}
Rect Control::UiClip() const noexcept
{
    Rect clip{UiBounds()};
    const Control* at{m_parent};
    for (UInt32 depth{}; at != nullptr && depth < MaximumDepth; ++depth, at = at->m_parent)
    {
        Rect client{at->UiClient()};
        const Rect bounds{at->UiBounds()};
        client.X += bounds.X;
        client.Y += bounds.Y;
        clip = User::Graphics::Intersect(clip, client);
    }
    return clip;
}
Control* Control::UiHit(Point point) noexcept
{
    if (!UiEffective() || !Contains(UiClip(), point))
    {
        return nullptr;
    }
    for (UInt32 i{m_childCount}; i != 0U; --i)
    {
        if (!UiChildVisible(*m_children[i - 1U]))
        {
            continue;
        }
        Control* const hit{m_children[i - 1U]->UiHit(point)};
        if (hit != nullptr)
        {
            return hit;
        }
    }
    return this;
}
void Control::UiInvalidate() noexcept
{
    Form* const form{UiForm()};
    if (form != nullptr)
    {
        form->UiDamage(UiClip());
    }
}
void Control::UiEmit(Event& event) noexcept
{
    if (event.Kind < EventKind::Count)
    {
        Handler handler{m_handlers[static_cast<UInt32>(event.Kind)]};
        handler(*this, event);
    }
}
void Control::UiPaint(User::Graphics::MemorySurface surface, Rect clip) noexcept
{
    if (!m_visible)
    {
        return;
    }
    clip = User::Graphics::Intersect(clip, UiClip());
    if (User::Graphics::IsEmpty(clip))
    {
        return;
    }
    Graphics graphics{surface, clip, m_application.UiTheme()};
    UiDraw(graphics, UiBounds());
    Event paint{EventKind::Paint};
    paint.Drawing = &graphics;
    UiEmit(paint);
    // Reentrant painting may add/remove children; ownership remains with caller.
    for (UInt32 i{}; i < m_childCount; ++i)
    {
        if (UiChildVisible(*m_children[i]))
        {
            m_children[i]->UiPaint(surface, clip);
        }
    }
}
void Control::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    graphics.FillRect(bounds, graphics.Palette().Face);
}
void Control::UiActivate(Event& event) noexcept
{
    Event click{event};
    click.Kind = EventKind::Click;
    UiEmit(click);
}
void Control::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseEnter)
    {
        m_hot = true;
        UiInvalidate();
    }
    if (event.Kind == EventKind::MouseLeave)
    {
        m_hot = false;
        UiInvalidate();
    }
    if (event.Kind == EventKind::MouseDown)
    {
        m_pressed = true;
        UiInvalidate();
    }
    if (event.Kind == EventKind::MouseUp)
    {
        const bool activate{m_pressed && Contains(UiClip(), event.Position)};
        m_pressed = false;
        UiInvalidate();
        if (activate)
        {
            UiActivate(event);
        }
    }
    if (event.Kind == EventKind::KeyDown && (event.Code == Key::Space || event.Code == Key::Enter) && m_tabStop)
    {
        m_pressed = true;
        UiInvalidate();
        event.Handled = true;
    }
    if (event.Kind == EventKind::KeyUp && m_pressed && (event.Code == Key::Space || event.Code == Key::Enter))
    {
        m_pressed = false;
        UiInvalidate();
        UiActivate(event);
        event.Handled = true;
    }
    UiEmit(event);
}
Rect Control::UiClient() const noexcept
{
    const UInt32 left{Min(m_padding.Left, m_bounds.Width)};
    const UInt32 top{Min(m_padding.Top, m_bounds.Height)};
    return {static_cast<Int32>(left), static_cast<Int32>(top),
            m_bounds.Width - Min(m_bounds.Width, left + m_padding.Right),
            m_bounds.Height - Min(m_bounds.Height, top + m_padding.Bottom)};
}
Point Control::UiPreferred() const noexcept
{
    // Measuring only needs the font context; the empty surface is never written.
    Graphics graphics{{}, {}, m_application.UiTheme()};
    return {static_cast<Int32>(graphics.Measure(UiText()) + 2U * m_application.UiTheme().Padding),
            static_cast<Int32>(m_application.UiTheme().DefaultHeight)};
}
void Control::UiArrange() noexcept
{
    Rect available{UiClient()};
    const Rect client{available};
    for (UInt32 i{}; i < m_childCount; ++i)
    {
        Control& child{*m_children[i]};
        if (!UiChildVisible(child))
        {
            continue;
        }
        Rect bounds{child.m_baseline};
        if (child.m_autoSize)
        {
            const Point size{child.UiPreferred()};
            bounds.Width = static_cast<UInt32>(Clamp(size.X, 0, static_cast<Int32>(MaximumAxis)));
            bounds.Height = static_cast<UInt32>(Clamp(size.Y, 0, static_cast<Int32>(MaximumAxis)));
        }
        if (child.m_dock == Dock::None)
        {
            const Int32 dx{static_cast<Int32>(client.Width) - child.m_parentSize.X};
            const Int32 dy{static_cast<Int32>(client.Height) - child.m_parentSize.Y};
            if (Has(child.m_anchor, Anchor::Right))
            {
                if (Has(child.m_anchor, Anchor::Left))
                {
                    bounds.Width = static_cast<UInt32>(
                        Clamp(static_cast<Int32>(bounds.Width) + dx, 0, static_cast<Int32>(MaximumAxis)));
                }
                else
                {
                    bounds.X += dx;
                }
            }
            else if (!Has(child.m_anchor, Anchor::Left))
            {
                bounds.X += dx / 2;
            }
            if (Has(child.m_anchor, Anchor::Bottom))
            {
                if (Has(child.m_anchor, Anchor::Top))
                {
                    bounds.Height = static_cast<UInt32>(
                        Clamp(static_cast<Int32>(bounds.Height) + dy, 0, static_cast<Int32>(MaximumAxis)));
                }
                else
                {
                    bounds.Y += dy;
                }
            }
            else if (!Has(child.m_anchor, Anchor::Top))
            {
                bounds.Y += dy / 2;
            }
        }
        else
        {
            const UInt32 width{Min(bounds.Width, available.Width)};
            const UInt32 height{Min(bounds.Height, available.Height)};
            bounds = available;
            switch (child.m_dock)
            {
            case Dock::Top:
                bounds.Height = height;
                available.Y += static_cast<Int32>(height);
                available.Height -= height;
                break;
            case Dock::Bottom:
                bounds.Height = height;
                bounds.Y += static_cast<Int32>(available.Height - height);
                available.Height -= height;
                break;
            case Dock::Left:
                bounds.Width = width;
                available.X += static_cast<Int32>(width);
                available.Width -= width;
                break;
            case Dock::Right:
                bounds.Width = width;
                bounds.X += static_cast<Int32>(available.Width - width);
                available.Width -= width;
                break;
            case Dock::Fill:
                available.Width = 0U;
                available.Height = 0U;
                break;
            case Dock::None:
                break;
            }
            const UInt32 left{Min(bounds.Width, child.m_margin.Left)};
            const UInt32 top{Min(bounds.Height, child.m_margin.Top)};
            bounds.X += static_cast<Int32>(left);
            bounds.Y += static_cast<Int32>(top);
            bounds.Width -= Min(bounds.Width, left + child.m_margin.Right);
            bounds.Height -= Min(bounds.Height, top + child.m_margin.Bottom);
        }
        child.UiSetBounds(bounds);
    }
}
void Control::UiLayout() noexcept
{
    UiArrange();
}
} // namespace Asteria::User::Toolkit
