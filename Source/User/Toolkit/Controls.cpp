// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Controls.hpp"

namespace Asteria::User::Toolkit
{
namespace
{
void ArrowGlyph(Graphics& graphics, Rect bounds, bool up = false) noexcept
{
    const auto& theme{graphics.Palette()};
    const UInt32 size{Min(theme.Arrow / 3U, Min(bounds.Width, bounds.Height) / 2U)};
    for (UInt32 row{}; row < size; ++row)
    {
        const UInt32 width{2U * (size - row) - 1U};
        const Int32 y{bounds.Y + static_cast<Int32>(bounds.Height / 2U) +
                      (up ? -static_cast<Int32>(row) : static_cast<Int32>(row))};
        graphics.FillRect(
            {bounds.X + static_cast<Int32>(bounds.Width / 2U) - static_cast<Int32>(width / 2U), y, width, 1U},
            theme.Text);
    }
}
} // namespace
void Button::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, m_hot && UiEffective() ? theme.Hot : theme.Face);
    graphics.Bevel(bounds, m_pressed);
    Rect text{Inset(bounds, theme.Padding)};
    if (m_pressed)
    {
        text.X += static_cast<Int32>(theme.PressOffset);
        text.Y += static_cast<Int32>(theme.PressOffset);
    }
    graphics.Text(UiText(), text, UiEffective() ? theme.Text : theme.DisabledText, Align::Center);
    if (m_application.UiFocus() == this)
    {
        graphics.FocusRect(bounds);
    }
}
void CheckBox::UiCheck(bool checked) noexcept
{
    if (m_checked == checked)
    {
        return;
    }
    m_checked = checked;
    UiInvalidate();
    Event change{EventKind::CheckedChanged};
    UiEmit(change);
}
Status CheckBox::SetChecked(bool checked) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            UiCheck(checked);
            return Status::Ok;
        });
}
void CheckBox::UiActivate(Event& event) noexcept
{
    UiCheck(!m_checked);
    Control::UiActivate(event);
}
void CheckBox::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    Rect indicator{bounds.X + static_cast<Int32>(theme.Padding),
                   bounds.Y +
                       static_cast<Int32>(bounds.Height > theme.Indicator ? bounds.Height - theme.Indicator : 0U) / 2,
                   theme.Indicator, theme.Indicator};
    graphics.FillRect(indicator, !UiEffective() || m_pressed ? theme.Face : theme.Window);
    graphics.Bevel(indicator, true);
    if (m_kind == Kind::RadioButton)
    {
        graphics.FillRect(indicator, theme.Face);
        const Int32 size{static_cast<Int32>(theme.Indicator)};
        const Int32 inner{size - static_cast<Int32>(2U * Min(theme.Border, theme.Indicator / 2U))};
        for (Int32 y{}; y < size; ++y)
        {
            for (Int32 x{}; x < size; ++x)
            {
                const Int32 dx{2 * x + 1 - size};
                const Int32 dy{2 * y + 1 - size};
                const Int32 radius{dx * dx + dy * dy};
                if (radius > size * size)
                {
                    continue;
                }
                const Color color{radius > inner * inner ? (dx + dy < 0 ? theme.Shadow : theme.Highlight)
                                                         : (m_checked && radius <= (size / 3) * (size / 3)
                                                                ? (UiEffective() ? theme.Text : theme.DisabledText)
                                                                : (UiEffective() ? theme.Window : theme.Face))};
                graphics.FillRect({indicator.X + x, indicator.Y + y, 1U, 1U}, color);
            }
        }
    }
    else if (m_checked)
    {
        const Rect inner{Inset(indicator, theme.Border + theme.PressOffset)};
        const Color ink{UiEffective() ? theme.Text : theme.DisabledText};
        graphics.Line({inner.X, inner.Y + static_cast<Int32>(inner.Height / 2U)},
                      {inner.X + static_cast<Int32>(inner.Width / 3U), inner.Y + static_cast<Int32>(inner.Height) - 1},
                      ink);
        graphics.Line({inner.X + static_cast<Int32>(inner.Width / 3U), inner.Y + static_cast<Int32>(inner.Height) - 1},
                      {inner.X + static_cast<Int32>(inner.Width) - 1, inner.Y}, ink);
    }
    const UInt32 offset{Min(bounds.Width, theme.Padding + theme.Indicator + theme.Gap)};
    graphics.Text(UiText(), {bounds.X + static_cast<Int32>(offset), bounds.Y, bounds.Width - offset, bounds.Height},
                  UiEffective() ? theme.Text : theme.DisabledText);
    if (m_application.UiFocus() == this)
    {
        graphics.FocusRect(bounds);
    }
}
void RadioButton::UiSelect() noexcept
{
    if (m_parent != nullptr)
    {
        // Siblings share a radio group. No RTTI; Kind is fixed at construction.
        for (UInt32 i{}; i < m_parent->m_childCount; ++i)
        {
            Control* sibling{m_parent->m_children[i]};
            if (sibling != this && sibling->Type() == Kind::RadioButton)
            {
                static_cast<RadioButton*>(sibling)->UiCheck(false);
            }
        }
    }
    UiCheck(true);
}
Status RadioButton::SetChecked(bool checked) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (checked)
            {
                UiSelect();
            }
            else
            {
                UiCheck(false);
            }
            return Status::Ok;
        });
}
void RadioButton::UiActivate(Event& event) noexcept
{
    UiSelect();
    Control::UiActivate(event);
}
void RadioButton::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::KeyDown && m_parent != nullptr &&
        (event.Code == Key::Left || event.Code == Key::Right || event.Code == Key::Up || event.Code == Key::Down))
    {
        UInt32 own{};
        for (UInt32 i{}; i < m_parent->m_childCount; ++i)
        {
            if (m_parent->m_children[i] == this)
            {
                own = i;
            }
        }
        const bool backwards{event.Code == Key::Left || event.Code == Key::Up};
        for (UInt32 step{1U}; step <= m_parent->m_childCount; ++step)
        {
            const UInt32 index{(own + (backwards ? m_parent->m_childCount - step : step)) % m_parent->m_childCount};
            Control* sibling{m_parent->m_children[index]};
            if (sibling->Type() == Kind::RadioButton && sibling->UiEffective())
            {
                static_cast<void>(sibling->Focus());
                sibling->UiActivate(event);
                event.Handled = true;
                break;
            }
        }
    }
    if (!event.Handled)
    {
        CheckBox::UiInput(event);
    }
    else
    {
        UiEmit(event);
    }
}
Status Label::SetAlignment(Align alignment) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (alignment > Align::End)
            {
                return Status::Invalid;
            }
            m_alignment = alignment;
            UiInvalidate();
            return Status::Ok;
        });
}
void Label::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    graphics.FillRect(bounds, graphics.Palette().Face);
    graphics.Text(UiText(), bounds, UiEffective() ? graphics.Palette().Text : graphics.Palette().DisabledText,
                  m_alignment);
}
Status PictureBox::SetImage(const Image& image, bool scale) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!image.Valid() && image.Pixels != nullptr)
            {
                return Status::Invalid;
            }
            m_image = image;
            m_scale = scale;
            UiInvalidate();
            return Status::Ok;
        });
}
void PictureBox::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    graphics.FillRect(bounds, graphics.Palette().Face);
    graphics.DrawImage(m_image, bounds, m_scale);
}
Status ItemsControl::AddItem(TextView text, Item::Style style) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!ValidText(text, MaximumItemText) || style > Item::Style::DropDown)
            {
                return Status::Invalid;
            }
            if (m_itemCount == MaximumItems)
            {
                return Status::Limit;
            }
            Item& item{m_items[m_itemCount++]};
            item = {};
            if (text.Length != 0U)
            {
                __builtin_memcpy(item.Text, text.Data, text.Length);
            }
            item.Length = text.Length;
            item.Type = style;
            UiInvalidate();
            return Status::Ok;
        });
}
Status ItemsControl::ClearItems() noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            m_itemCount = 0U;
            m_first = 0U;
            UiSelect(-1);
            UiInvalidate();
            return Status::Ok;
        });
}
Status ItemsControl::SetItemEnabled(UInt32 index, bool enabled) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (index >= m_itemCount)
            {
                return Status::Invalid;
            }
            m_items[index].Enabled = enabled;
            UiInvalidate();
            return Status::Ok;
        });
}
Status ItemsControl::SetItemChecked(UInt32 index, bool checked) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (index >= m_itemCount)
            {
                return Status::Invalid;
            }
            m_items[index].Checked = checked;
            UiInvalidate();
            return Status::Ok;
        });
}
void ItemsControl::UiSelect(Int32 index) noexcept
{
    if (m_selected == index)
    {
        return;
    }
    m_selected = index;
    if (index >= 0)
    {
        const UInt32 rows{m_bounds.Height / m_application.UiTheme().ItemHeight};
        const UInt32 selected{static_cast<UInt32>(index)};
        if (selected < m_first)
        {
            m_first = selected;
        }
        else if (rows != 0U && selected >= m_first + rows)
        {
            m_first = selected - rows + 1U;
        }
    }
    UiInvalidate();
    Event event{EventKind::SelectedIndexChanged};
    event.Item = index;
    UiEmit(event);
}
Status ItemsControl::SetSelectedIndex(Int32 index) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (index < -1 || index >= static_cast<Int32>(m_itemCount))
            {
                return Status::Invalid;
            }
            UiSelect(index);
            return Status::Ok;
        });
}
Int32 ItemsControl::UiAdjacent(Int32 direction) const noexcept
{
    Int32 index{m_selected};
    for (UInt32 work{}; work < m_itemCount; ++work)
    {
        index += direction;
        if (index < 0)
        {
            index = static_cast<Int32>(m_itemCount) - 1;
        }
        if (index >= static_cast<Int32>(m_itemCount))
        {
            index = 0;
        }
        const Item& item{m_items[static_cast<UInt32>(index)]};
        if (item.Enabled && item.Type != Item::Style::Separator)
        {
            return index;
        }
    }
    return m_selected;
}
void ItemsControl::UiListDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, UiEffective() ? theme.Window : theme.Face);
    graphics.Bevel(bounds, true);
    const Rect inner{Inset(bounds, theme.Border)};
    for (UInt32 index{m_first}; index < m_itemCount; ++index)
    {
        const UInt32 offset{(index - m_first) * theme.ItemHeight};
        if (offset >= inner.Height)
        {
            break;
        }
        Rect row{inner.X, inner.Y + static_cast<Int32>(offset), inner.Width,
                 Min(theme.ItemHeight, inner.Height - offset)};
        const bool selected{m_selected == static_cast<Int32>(index)};
        const auto& item{m_items[index]};
        if (selected)
        {
            graphics.FillRect(row, theme.Selection);
        }
        graphics.Text({item.Text, item.Length}, Inset(row, theme.Padding),
                      !UiEffective() || !item.Enabled ? theme.DisabledText
                                                      : (selected ? theme.SelectedText : theme.Text));
        if (selected && m_application.UiFocus() == this)
        {
            graphics.FocusRect(row);
        }
    }
}
void ListBox::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    UiListDraw(graphics, bounds);
}
void ListBox::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseDown)
    {
        const Rect inner{Inset(UiBounds(), m_application.UiTheme().Border)};
        if (Contains(inner, event.Position))
        {
            const UInt32 index{m_first +
                               static_cast<UInt32>(event.Position.Y - inner.Y) / m_application.UiTheme().ItemHeight};
            if (index < m_itemCount && m_items[index].Enabled)
            {
                UiSelect(static_cast<Int32>(index));
            }
        }
    }
    if (event.Kind == EventKind::KeyDown)
    {
        if (event.Code == Key::Down || event.Code == Key::Up)
        {
            UiSelect(UiAdjacent(event.Code == Key::Down ? 1 : -1));
            event.Handled = true;
        }
        if (event.Code == Key::Home || event.Code == Key::End)
        {
            m_selected = event.Code == Key::Home ? -1 : 0;
            UiSelect(UiAdjacent(event.Code == Key::Home ? 1 : -1));
            event.Handled = true;
        }
    }
    if (event.Kind == EventKind::MouseWheel)
    {
        m_first = static_cast<UInt32>(Clamp(static_cast<Int32>(m_first) - Clamp(event.Wheel, -1, 1), 0,
                                            m_itemCount == 0U ? 0 : static_cast<Int32>(m_itemCount - 1U)));
        UiInvalidate();
    }
    if (!event.Handled)
    {
        Control::UiInput(event);
    }
    else
    {
        UiEmit(event);
    }
}
ComboBox::ComboBox(Application& app, bool editable) noexcept : TextBox(app), m_editable{editable}, m_popup{*this}
{
    m_kind = Kind::ComboBox;
    m_readOnly = !editable;
    static_cast<void>(m_popup.SetVisible(false));
}
Status ComboBox::AddItem(TextView text) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!ValidText(text, MaximumItemText))
            {
                return Status::Invalid;
            }
            if (m_itemCount == MaximumItems)
            {
                return Status::Limit;
            }
            auto& item{m_items[m_itemCount++]};
            if (text.Length != 0U)
            {
                __builtin_memcpy(item.Text, text.Data, text.Length);
            }
            item.Length = text.Length;
            UiInvalidate();
            return Status::Ok;
        });
}
void ComboBox::UiSelect(Int32 index) noexcept
{
    if (index < -1 || index >= static_cast<Int32>(m_itemCount) || m_selected == index)
    {
        return;
    }
    m_selected = index;
    if (index >= 0)
    {
        const auto& item{m_items[static_cast<UInt32>(index)]};
        static_cast<void>(UiAssignText({item.Text, item.Length}));
    }
    UiInvalidate();
    Event event{EventKind::SelectedIndexChanged};
    event.Item = index;
    UiEmit(event);
}
Status ComboBox::SetSelectedIndex(Int32 index) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (index < -1 || index >= static_cast<Int32>(m_itemCount))
            {
                return Status::Invalid;
            }
            UiSelect(index);
            return Status::Ok;
        });
}
void ComboBox::UiOpen(bool open) noexcept
{
    if (m_open == open)
    {
        return;
    }
    Form* form{UiForm()};
    if (form == nullptr)
    {
        return;
    }
    if (open)
    {
        if (m_popup.UiParent() != nullptr)
        {
            static_cast<void>(m_popup.UiParent()->Remove(m_popup));
        }
        if (form->Add(m_popup) != Status::Ok)
        {
            return;
        }
        const Rect bounds{UiBounds()};
        const Rect root{form->UiBounds()};
        const auto& theme{m_application.UiTheme()};
        const UInt32 height{Min(m_itemCount, theme.PopupRows) * theme.ItemHeight + 2U * theme.Border};
        static_cast<void>(m_popup.SetBounds(
            {bounds.X - root.X, bounds.Y - root.Y + static_cast<Int32>(bounds.Height), bounds.Width, height}));
        static_cast<void>(m_popup.SetVisible(true));
        static_cast<void>(m_popup.BringToFront());
        m_first = m_selected >= 0 ? static_cast<UInt32>(m_selected) : 0U;
    }
    else
    {
        static_cast<void>(m_popup.SetVisible(false));
        if (m_popup.UiParent() != nullptr)
        {
            static_cast<void>(m_popup.UiParent()->Remove(m_popup));
        }
    }
    m_open = open;
    UiInvalidate();
}
Status ComboBox::SetDropDown(bool open) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            UiOpen(open);
            return m_open == open ? Status::Ok : Status::Invalid;
        });
}
void ComboBox::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    Rect text{bounds};
    text.Width -= Min(text.Width, theme.Arrow);
    if (m_editable)
    {
        TextBox::UiDraw(graphics, text);
    }
    else
    {
        graphics.FillRect(bounds, UiEffective() ? theme.Window : theme.Face);
        graphics.Bevel(bounds, true);
        graphics.Text(UiText(), Inset(text, theme.Padding), UiEffective() ? theme.Text : theme.DisabledText);
        if (m_application.UiFocus() == this)
        {
            graphics.FocusRect(text);
        }
    }
    Rect arrow{bounds.X + static_cast<Int32>(text.Width), bounds.Y, bounds.Width - text.Width, bounds.Height};
    graphics.FillRect(arrow, theme.Face);
    graphics.Bevel(arrow, m_open);
    ArrowGlyph(graphics, arrow);
}
void ComboBox::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::LostFocus)
    {
        UiOpen(false);
    }
    if (event.Kind == EventKind::MouseDown &&
        (!m_editable ||
         event.Position.X >=
             UiBounds().X + static_cast<Int32>(m_bounds.Width - Min(m_bounds.Width, m_application.UiTheme().Arrow))))
    {
        UiOpen(!m_open);
        event.Handled = true;
    }
    if (event.Kind == EventKind::KeyDown)
    {
        if (event.Code == Key::Escape)
        {
            UiOpen(false);
            event.Handled = true;
        }
        else if (event.Code == Key::Enter && m_open)
        {
            UiOpen(false);
            event.Handled = true;
        }
        else if (event.Code == Key::Down || event.Code == Key::Up)
        {
            if (event.Alt)
            {
                UiOpen(!m_open);
            }
            else if (m_itemCount != 0U)
            {
                UiSelect(
                    Clamp(m_selected + (event.Code == Key::Down ? 1 : -1), 0, static_cast<Int32>(m_itemCount) - 1));
                m_first = static_cast<UInt32>(m_selected);
                m_popup.UiInvalidate();
            }
            event.Handled = true;
        }
    }
    if (!event.Handled && m_editable)
    {
        TextBox::UiInput(event);
    }
    else
    {
        UiEmit(event);
    }
}
void ComboBox::Popup::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Window);
    graphics.Bevel(bounds, true);
    const Rect inner{Inset(bounds, theme.Border)};
    for (UInt32 index{m_owner.m_first}; index < m_owner.m_itemCount; ++index)
    {
        const UInt32 offset{(index - m_owner.m_first) * theme.ItemHeight};
        if (offset >= inner.Height)
        {
            break;
        }
        const Rect row{inner.X, inner.Y + static_cast<Int32>(offset), inner.Width,
                       Min(theme.ItemHeight, inner.Height - offset)};
        const auto& item{m_owner.m_items[index]};
        const bool selected{m_owner.m_selected == static_cast<Int32>(index)};
        if (selected)
        {
            graphics.FillRect(row, theme.Selection);
        }
        graphics.Text({item.Text, item.Length}, Inset(row, theme.Padding), selected ? theme.SelectedText : theme.Text);
    }
}
void ComboBox::Popup::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseUp)
    {
        const Rect inner{Inset(UiBounds(), m_application.UiTheme().Border)};
        if (Contains(inner, event.Position))
        {
            const UInt32 index{m_owner.m_first +
                               static_cast<UInt32>(event.Position.Y - inner.Y) / m_application.UiTheme().ItemHeight};
            if (index < m_owner.m_itemCount)
            {
                m_owner.UiSelect(static_cast<Int32>(index));
            }
        }
        m_owner.UiOpen(false);
    }
    if (event.Kind == EventKind::MouseWheel)
    {
        m_owner.m_first =
            static_cast<UInt32>(Clamp(static_cast<Int32>(m_owner.m_first) - Clamp(event.Wheel, -1, 1), 0,
                                      m_owner.m_itemCount == 0U ? 0 : static_cast<Int32>(m_owner.m_itemCount) - 1));
        UiInvalidate();
    }
}
Rect ToolBar::UiItemBounds(UInt32 index, Rect bounds) noexcept
{
    const auto& theme{m_application.UiTheme()};
    Graphics measure{{}, {}, theme};
    UInt32 offset{};
    for (UInt32 i{}; i <= index && i < m_itemCount; ++i)
    {
        const auto& item{m_items[i]};
        const UInt32 width{item.Type == Item::Style::Separator
                               ? theme.Separator
                               : measure.Measure({item.Text, item.Length}) + 2U * theme.Padding +
                                     (item.Type == Item::Style::DropDown ? theme.Arrow : 0U)};
        if (i == index)
        {
            return {bounds.X + static_cast<Int32>(offset), bounds.Y, width, bounds.Height};
        }
        offset += width;
    }
    return {};
}
void ToolBar::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    for (UInt32 i{}; i < m_itemCount; ++i)
    {
        const Rect rect{UiItemBounds(i, bounds)};
        const auto& item{m_items[i]};
        if (item.Type == Item::Style::Separator)
        {
            graphics.Bevel({rect.X + static_cast<Int32>(rect.Width / 2U), rect.Y, theme.Border, rect.Height}, true);
            continue;
        }
        const bool pressed{item.Checked || (m_pressed && m_selected == static_cast<Int32>(i))};
        if (m_hotItem == static_cast<Int32>(i))
        {
            graphics.FillRect(rect, theme.Hot);
        }
        graphics.Bevel(rect, pressed);
        Rect text{Inset(rect, theme.Padding)};
        if (item.Type == Item::Style::DropDown)
        {
            text.Width -= Min(text.Width, theme.Arrow);
            ArrowGlyph(graphics, {text.X + static_cast<Int32>(text.Width), rect.Y, theme.Arrow, rect.Height});
        }
        graphics.Text({item.Text, item.Length}, text, UiEffective() && item.Enabled ? theme.Text : theme.DisabledText,
                      Align::Center);
        if (m_application.UiFocus() == this && m_selected == static_cast<Int32>(i))
        {
            graphics.FocusRect(rect);
        }
    }
}
void ToolBar::UiActivate(Event& event) noexcept
{
    if (m_selected < 0 || m_selected >= static_cast<Int32>(m_itemCount))
    {
        return;
    }
    auto& item{m_items[static_cast<UInt32>(m_selected)]};
    if (!item.Enabled || item.Type == Item::Style::Separator)
    {
        return;
    }
    if (item.Type == Item::Style::Toggle)
    {
        item.Checked = !item.Checked;
        Event changed{EventKind::CheckedChanged};
        changed.Item = m_selected;
        UiEmit(changed);
    }
    event.Item = m_selected;
    Control::UiActivate(event);
    UiInvalidate();
}
void ToolBar::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseDown || event.Kind == EventKind::MouseMove)
    {
        m_hotItem = -1;
        for (UInt32 i{}; i < m_itemCount; ++i)
        {
            if (Contains(UiItemBounds(i, UiBounds()), event.Position))
            {
                m_hotItem = static_cast<Int32>(i);
                if (event.Kind == EventKind::MouseDown)
                {
                    UiSelect(m_hotItem);
                }
                break;
            }
        }
        UiInvalidate();
    }
    if (event.Kind == EventKind::MouseLeave)
    {
        m_hotItem = -1;
    }
    if (event.Kind == EventKind::KeyDown && (event.Code == Key::Left || event.Code == Key::Right))
    {
        UiSelect(UiAdjacent(event.Code == Key::Right ? 1 : -1));
        event.Handled = true;
    }
    if (!event.Handled)
    {
        Control::UiInput(event);
    }
    else
    {
        UiEmit(event);
    }
}
} // namespace Asteria::User::Toolkit
