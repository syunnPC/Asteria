// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Layout.hpp"

namespace Asteria::User::Toolkit
{
Rect GroupBox::UiClient() const noexcept
{
    Rect rect{Control::UiClient()};
    const auto& theme{m_application.UiTheme()};
    const UInt32 top{Min(rect.Height, theme.LineHeight)};
    const UInt32 side{Min(rect.Width / 2U, theme.Padding)};
    rect.X += static_cast<Int32>(side);
    rect.Y += static_cast<Int32>(top);
    rect.Width -= 2U * side;
    rect.Height -= top;
    return rect;
}
void GroupBox::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    Rect frame{bounds};
    const UInt32 top{Min(frame.Height, theme.LineHeight / 2U)};
    frame.Y += static_cast<Int32>(top);
    frame.Height -= top;
    graphics.Bevel(frame, true);
    Rect title{bounds.X + static_cast<Int32>(theme.Padding), bounds.Y,
               Min(bounds.Width, graphics.Measure(UiText()) + 2U * theme.Padding), theme.LineHeight};
    graphics.FillRect(title, theme.Face);
    graphics.Text(UiText(), title, UiEffective() ? theme.Text : theme.DisabledText);
}
void Panel::UiClampScroll() noexcept
{
    const Rect client{UiClient()};
    m_scroll.X =
        Clamp(m_scroll.X, 0, static_cast<Int32>(m_contentWidth > client.Width ? m_contentWidth - client.Width : 0U));
    m_scroll.Y = Clamp(m_scroll.Y, 0,
                       static_cast<Int32>(m_contentHeight > client.Height ? m_contentHeight - client.Height : 0U));
}
Status Panel::SetScroll(Point position) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            m_scroll = position;
            UiClampScroll();
            UiInvalidate();
            return Status::Ok;
        });
}
Status Panel::SetContentSize(UInt32 width, UInt32 height) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (width > MaximumAxis || height > MaximumAxis)
            {
                return Status::Invalid;
            }
            m_contentWidth = width;
            m_contentHeight = height;
            UiClampScroll();
            UiInvalidate();
            return Status::Ok;
        });
}
void Panel::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseWheel)
    {
        m_scroll.Y -= Clamp(event.Wheel, -1, 1) * static_cast<Int32>(m_application.UiTheme().ScrollStep);
        UiClampScroll();
        UiInvalidate();
        event.Handled = true;
    }
    Control::UiInput(event);
}
Status TabControl::AddPage(Control& page, TextView title) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!ValidText(title, MaximumItemText) || m_itemCount == MaximumItems)
            {
                return Status::Limit;
            }
            const Status status{Add(page)};
            if (status != Status::Ok)
            {
                return status;
            }
            static_cast<void>(AddItem(title));
            if (m_selected == -1)
            {
                UiSelect(0);
            }
            UiLayout();
            UiInvalidate();
            return Status::Ok;
        });
}
Rect TabControl::UiClient() const noexcept
{
    Rect rect{Inset({0, 0, m_bounds.Width, m_bounds.Height}, m_application.UiTheme().Border)};
    const UInt32 top{Min(rect.Height, m_application.UiTheme().ItemHeight)};
    rect.Y += static_cast<Int32>(top);
    rect.Height -= top;
    return rect;
}
bool TabControl::UiChildVisible(const Control& child) const noexcept
{
    if (m_selected < 0 || m_selected >= static_cast<Int32>(m_childCount))
    {
        return false;
    }
    return m_children[static_cast<UInt32>(m_selected)] == &child && Control::UiChildVisible(child);
}
void TabControl::UiArrange() noexcept
{
    const Rect client{UiClient()};
    for (UInt32 i{}; i < m_childCount; ++i)
    {
        m_children[i]->UiSetBounds(client);
    }
}
void TabControl::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    graphics.Bevel(bounds, false);
    for (UInt32 i{}; i < m_itemCount; ++i)
    {
        const Rect tab{bounds.X + static_cast<Int32>(i * theme.TabWidth), bounds.Y, theme.TabWidth, theme.ItemHeight};
        graphics.FillRect(tab, m_selected == static_cast<Int32>(i) ? theme.Light : theme.Face);
        graphics.Bevel(tab, false);
        const auto& item{m_items[i]};
        graphics.Text({item.Text, item.Length}, Inset(tab, theme.Padding),
                      UiEffective() ? theme.Text : theme.DisabledText);
        if (m_application.UiFocus() == this && m_selected == static_cast<Int32>(i))
        {
            graphics.FocusRect(tab);
        }
    }
}
void TabControl::UiSelect(Int32 index) noexcept
{
    if (m_selected == index)
    {
        return;
    }
    m_selected = index;
    UiLayout();
    Control* focused{m_application.UiFocus()};
    if (focused != nullptr && !focused->UiEffective())
    {
        static_cast<void>(Focus());
    }
    UiInvalidate();
    Event event{EventKind::SelectedIndexChanged};
    event.Item = index;
    UiEmit(event);
}
void TabControl::UiInput(Event& event) noexcept
{
    Int32 selected{m_selected};
    if (event.Kind == EventKind::MouseDown &&
        event.Position.Y < UiBounds().Y + static_cast<Int32>(m_application.UiTheme().ItemHeight))
    {
        selected = (event.Position.X - UiBounds().X) / static_cast<Int32>(m_application.UiTheme().TabWidth);
    }
    if (event.Kind == EventKind::KeyDown && (event.Code == Key::Left || event.Code == Key::Right))
    {
        selected = UiAdjacent(event.Code == Key::Right ? 1 : -1);
        event.Handled = true;
    }
    if (selected >= 0 && selected < static_cast<Int32>(m_itemCount) && selected != m_selected)
    {
        UiSelect(selected);
        UiLayout();
        Control* focused{m_application.UiFocus()};
        if (focused != nullptr && !focused->UiEffective())
        {
            static_cast<void>(Focus());
        }
    }
    Control::UiInput(event);
}
Status SplitContainer::SetSplit(UInt32 position, bool vertical) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (position > MaximumAxis)
            {
                return Status::Invalid;
            }
            m_position = position;
            m_vertical = vertical;
            UiLayout();
            UiInvalidate();
            return Status::Ok;
        });
}
void SplitContainer::UiArrange() noexcept
{
    Rect client{UiClient()};
    const UInt32 extent{m_vertical ? client.Width : client.Height};
    const UInt32 divider{Min(extent, m_application.UiTheme().Splitter)};
    const UInt32 position{Min(m_position, extent - divider)};
    for (UInt32 i{}; i < m_childCount && i < 2U; ++i)
    {
        Rect pane{client};
        if (m_vertical)
        {
            pane.Width = i == 0U ? position : extent - position - divider;
            if (i != 0U)
            {
                pane.X += static_cast<Int32>(position + divider);
            }
        }
        else
        {
            pane.Height = i == 0U ? position : extent - position - divider;
            if (i != 0U)
            {
                pane.Y += static_cast<Int32>(position + divider);
            }
        }
        m_children[i]->UiSetBounds(pane);
    }
}
void SplitContainer::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    const Rect client{UiClient()};
    const UInt32 extent{m_vertical ? client.Width : client.Height};
    const UInt32 divider{Min(extent, theme.Splitter)};
    const UInt32 position{Min(m_position, extent - divider)};
    Rect bar{bounds.X + client.X, bounds.Y + client.Y, client.Width, client.Height};
    if (m_vertical)
    {
        bar.X += static_cast<Int32>(position);
        bar.Width = divider;
    }
    else
    {
        bar.Y += static_cast<Int32>(position);
        bar.Height = divider;
    }
    graphics.Bevel(bar, m_dragging);
    if (m_application.UiFocus() == this)
    {
        graphics.FocusRect(bounds);
    }
}
void SplitContainer::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseDown)
    {
        m_dragging = true;
    }
    if (event.Kind == EventKind::MouseMove && m_dragging)
    {
        const Rect bounds{UiBounds()};
        m_position = static_cast<UInt32>(Clamp(m_vertical ? event.Position.X - bounds.X : event.Position.Y - bounds.Y,
                                               0, static_cast<Int32>(MaximumAxis)));
        UiLayout();
        UiInvalidate();
    }
    if (event.Kind == EventKind::MouseUp)
    {
        m_dragging = false;
    }
    if (event.Kind == EventKind::KeyDown &&
        (event.Code == Key::Left || event.Code == Key::Right || event.Code == Key::Up || event.Code == Key::Down))
    {
        m_position = static_cast<UInt32>(
            Clamp(static_cast<Int32>(m_position) + ((event.Code == Key::Left || event.Code == Key::Up) ? -1 : 1) *
                                                       static_cast<Int32>(m_application.UiTheme().Gap),
                  0, static_cast<Int32>(MaximumAxis)));
        UiLayout();
        UiInvalidate();
        event.Handled = true;
    }
    Control::UiInput(event);
}
Status FlowLayoutPanel::SetFlow(bool vertical, bool wrap) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            m_vertical = vertical;
            m_wrap = wrap;
            UiLayout();
            UiInvalidate();
            return Status::Ok;
        });
}
void FlowLayoutPanel::UiArrange() noexcept
{
    const Rect client{UiClient()};
    const UInt32 gap{m_application.UiTheme().Gap};
    UInt32 main{};
    UInt32 cross{};
    UInt32 lineCross{};
    const UInt32 limit{m_vertical ? client.Height : client.Width};
    for (UInt32 i{}; i < m_childCount; ++i)
    {
        Control& child{*m_children[i]};
        if (!UiChildVisible(child))
        {
            continue;
        }
        Rect rect{child.m_baseline};
        if (child.m_autoSize)
        {
            const Point size{child.UiPreferred()};
            rect.Width = static_cast<UInt32>(size.X);
            rect.Height = static_cast<UInt32>(size.Y);
        }
        const UInt32 width{rect.Width + child.m_margin.Left + child.m_margin.Right};
        const UInt32 height{rect.Height + child.m_margin.Top + child.m_margin.Bottom};
        const UInt32 advance{m_vertical ? height : width};
        const UInt32 breadth{m_vertical ? width : height};
        if (m_wrap && main != 0U && main + advance > limit)
        {
            cross += lineCross + gap;
            main = 0U;
            lineCross = 0U;
        }
        rect.X = client.X + static_cast<Int32>((m_vertical ? cross : main) + child.m_margin.Left);
        rect.Y = client.Y + static_cast<Int32>((m_vertical ? main : cross) + child.m_margin.Top);
        child.UiSetBounds(rect);
        main += advance + gap;
        if (breadth > lineCross)
        {
            lineCross = breadth;
        }
    }
    m_contentWidth = Min(MaximumAxis, m_vertical ? cross + lineCross : limit);
    m_contentHeight = Min(MaximumAxis, m_vertical ? limit : cross + lineCross);
    UiClampScroll();
}
Status TableLayoutPanel::SetGrid(UInt32 columns, UInt32 rows) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (columns == 0U || columns > MaximumCells || rows == 0U || rows > MaximumCells)
            {
                return Status::Invalid;
            }
            m_columns = columns;
            m_rows = rows;
            UiLayout();
            UiInvalidate();
            return Status::Ok;
        });
}
Status TableLayoutPanel::SetWeight(bool column, UInt32 index, UInt32 weight) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (index >= (column ? m_columns : m_rows) || weight == 0U || weight > MaximumAxis)
            {
                return Status::Invalid;
            }
            (column ? m_columnWeight : m_rowWeight)[index] = weight;
            UiLayout();
            UiInvalidate();
            return Status::Ok;
        });
}
void TableLayoutPanel::UiArrange() noexcept
{
    const Rect client{UiClient()};
    UInt32 columnTotal{};
    UInt32 rowTotal{};
    for (UInt32 i{}; i < m_columns; ++i)
    {
        columnTotal += m_columnWeight[i];
    }
    for (UInt32 i{}; i < m_rows; ++i)
    {
        rowTotal += m_rowWeight[i];
    }
    UInt32 rowPrefix{};
    for (UInt32 row{}; row < m_rows; ++row)
    {
        UInt32 columnPrefix{};
        const UInt32 top{static_cast<UInt32>(static_cast<UInt64>(client.Height) * rowPrefix / rowTotal)};
        const UInt32 bottom{
            static_cast<UInt32>(static_cast<UInt64>(client.Height) * (rowPrefix + m_rowWeight[row]) / rowTotal)};
        for (UInt32 column{}; column < m_columns; ++column)
        {
            const UInt32 index{row * m_columns + column};
            if (index >= m_childCount)
            {
                break;
            }
            const UInt32 left{static_cast<UInt32>(static_cast<UInt64>(client.Width) * columnPrefix / columnTotal)};
            const UInt32 right{static_cast<UInt32>(static_cast<UInt64>(client.Width) *
                                                   (columnPrefix + m_columnWeight[column]) / columnTotal)};
            columnPrefix += m_columnWeight[column];
            Control& child{*m_children[index]};
            Rect rect{client.X + static_cast<Int32>(left), client.Y + static_cast<Int32>(top), right - left,
                      bottom - top};
            const UInt32 ml{Min(rect.Width, child.m_margin.Left)};
            const UInt32 mt{Min(rect.Height, child.m_margin.Top)};
            rect.X += static_cast<Int32>(ml);
            rect.Y += static_cast<Int32>(mt);
            rect.Width -= Min(rect.Width, ml + child.m_margin.Right);
            rect.Height -= Min(rect.Height, mt + child.m_margin.Bottom);
            child.UiSetBounds(rect);
        }
        rowPrefix += m_rowWeight[row];
    }
}
} // namespace Asteria::User::Toolkit
