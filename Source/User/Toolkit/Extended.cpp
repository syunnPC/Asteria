// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Extended.hpp"

namespace Asteria::User::Toolkit
{
namespace
{
void IntegerText(Int32 value, char* output, UInt32& length) noexcept
{
    const Foundation::Int64 wide{value};
    UInt32 magnitude{static_cast<UInt32>(wide < 0 ? -wide : wide)};
    char reverse[10U]{};
    UInt32 count{};
    do
    {
        reverse[count++] = static_cast<char>('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude != 0U);
    length = 0U;
    if (value < 0)
    {
        output[length++] = '-';
    }
    while (count != 0U)
    {
        output[length++] = reverse[--count];
    }
    output[length] = '\0';
}
} // namespace
Status ListView::AddColumn(TextView title, UInt32 width) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!ValidText(title, MaximumItemText) || width == 0U || width > MaximumAxis)
            {
                return Status::Invalid;
            }
            if (m_columnCount == MaximumCells)
            {
                return Status::Limit;
            }
            auto& column{m_columns[m_columnCount]};
            if (title.Length != 0U)
            {
                __builtin_memcpy(column.Text, title.Data, title.Length);
            }
            column.Length = title.Length;
            m_widths[m_columnCount++] = width;
            UiInvalidate();
            return Status::Ok;
        });
}
Status ListView::AddRow(const TextView* cells, UInt32 count) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (cells == nullptr || count != m_columnCount || count == 0U)
            {
                return Status::Invalid;
            }
            if (m_itemCount == MaximumItems)
            {
                return Status::Limit;
            }
            for (UInt32 i{}; i < count; ++i)
            {
                if (!ValidText(cells[i], MaximumItemText))
                {
                    return Status::Invalid;
                }
            }
            for (UInt32 i{}; i < count; ++i)
            {
                Item& cell{m_cells[m_itemCount][i]};
                if (cells[i].Length != 0U)
                {
                    __builtin_memcpy(cell.Text, cells[i].Data, cells[i].Length);
                }
                cell.Length = cells[i].Length;
            }
            return AddItem(cells[0U]);
        });
}
void ListView::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, UiEffective() ? theme.Window : theme.Face);
    graphics.Bevel(bounds, true);
    const Rect inner{Inset(bounds, theme.Border)};
    Int32 x{inner.X};
    for (UInt32 column{}; column < m_columnCount; ++column)
    {
        const Rect header{x, inner.Y, m_widths[column], theme.ItemHeight};
        graphics.FillRect(header, theme.Face);
        graphics.Bevel(header, false);
        graphics.Text({m_columns[column].Text, m_columns[column].Length}, Inset(header, theme.Padding), theme.Text);
        x += static_cast<Int32>(m_widths[column]);
    }
    for (UInt32 row{m_first}; row < m_itemCount; ++row)
    {
        const UInt32 offset{(row - m_first + 1U) * theme.ItemHeight};
        if (offset >= inner.Height)
        {
            break;
        }
        const Rect line{inner.X, inner.Y + static_cast<Int32>(offset), inner.Width,
                        Min(theme.ItemHeight, inner.Height - offset)};
        const bool selected{m_selected == static_cast<Int32>(row)};
        if (selected)
        {
            graphics.FillRect(line, theme.Selection);
        }
        x = inner.X;
        for (UInt32 column{}; column < m_columnCount; ++column)
        {
            const auto& cell{m_cells[row][column]};
            graphics.Text({cell.Text, cell.Length}, Inset({x, line.Y, m_widths[column], line.Height}, theme.Padding),
                          !UiEffective() ? theme.DisabledText : (selected ? theme.SelectedText : theme.Text));
            x += static_cast<Int32>(m_widths[column]);
        }
        if (selected && m_application.UiFocus() == this)
        {
            graphics.FocusRect(line);
        }
    }
}
void ListView::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseDown)
    {
        const Rect inner{Inset(UiBounds(), m_application.UiTheme().Border)};
        if (event.Position.Y < inner.Y + static_cast<Int32>(m_application.UiTheme().ItemHeight))
        {
            UiEmit(event);
            return;
        }
        Event shifted{event};
        shifted.Position.Y -= static_cast<Int32>(m_application.UiTheme().ItemHeight);
        ListBox::UiInput(shifted);
        event.Handled = shifted.Handled;
        return;
    }
    ListBox::UiInput(event);
}
UInt32 TreeView::UiNodeDepth(UInt32 index) const noexcept
{
    UInt32 depth{};
    for (UInt32 work{}; work < MaximumDepth && m_parents[index] >= 0; ++work)
    {
        ++depth;
        index = static_cast<UInt32>(m_parents[index]);
    }
    return depth;
}
bool TreeView::UiNodeVisible(UInt32 index) const noexcept
{
    for (UInt32 work{}; work < MaximumDepth && m_parents[index] >= 0; ++work)
    {
        index = static_cast<UInt32>(m_parents[index]);
        if (!m_expanded[index])
        {
            return false;
        }
    }
    return true;
}
bool TreeView::UiHasChildren(UInt32 index) const noexcept
{
    for (UInt32 i{}; i < m_itemCount; ++i)
    {
        if (m_parents[i] == static_cast<Int32>(index))
        {
            return true;
        }
    }
    return false;
}
Status TreeView::AddNode(Int32 parent, TextView title, UInt32& index) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (parent < -1 || parent >= static_cast<Int32>(m_itemCount))
            {
                return Status::Invalid;
            }
            if (parent >= 0 && UiNodeDepth(static_cast<UInt32>(parent)) + 1U >= MaximumDepth)
            {
                return Status::Limit;
            }
            index = m_itemCount;
            const Status result{AddItem(title)};
            if (result == Status::Ok)
            {
                m_parents[index] = parent;
                m_expanded[index] = false;
            }
            return result;
        });
}
Status TreeView::SetExpanded(UInt32 index, bool expanded) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (index >= m_itemCount)
            {
                return Status::Invalid;
            }
            m_expanded[index] = expanded;
            if (m_selected >= 0 && !UiNodeVisible(static_cast<UInt32>(m_selected)))
            {
                UiSelect(static_cast<Int32>(index));
            }
            UiInvalidate();
            return Status::Ok;
        });
}
void TreeView::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, UiEffective() ? theme.Window : theme.Face);
    graphics.Bevel(bounds, true);
    const Rect inner{Inset(bounds, theme.Border)};
    UInt32 row{};
    for (UInt32 i{m_first}; i < m_itemCount; ++i)
    {
        if (!UiNodeVisible(i))
        {
            continue;
        }
        const UInt32 offset{row++ * theme.ItemHeight};
        if (offset >= inner.Height)
        {
            break;
        }
        Rect line{inner.X, inner.Y + static_cast<Int32>(offset), inner.Width,
                  Min(theme.ItemHeight, inner.Height - offset)};
        const bool selected{m_selected == static_cast<Int32>(i)};
        if (selected)
        {
            graphics.FillRect(line, theme.Selection);
        }
        const UInt32 indent{Min(line.Width, UiNodeDepth(i) * theme.Indicator)};
        line.X += static_cast<Int32>(indent);
        line.Width -= indent;
        if (UiHasChildren(i))
        {
            const Rect toggle{line.X, line.Y + static_cast<Int32>(theme.Padding), theme.Indicator, theme.Indicator};
            graphics.FillRect(toggle, theme.Face);
            graphics.Bevel(toggle, true);
            graphics.Text(m_expanded[i] ? Utf8("-") : Utf8("+"), toggle, theme.Text, Align::Center);
        }
        const UInt32 indicator{Min(line.Width, theme.Indicator + theme.Gap)};
        line.X += static_cast<Int32>(indicator);
        line.Width -= indicator;
        const auto& item{m_items[i]};
        graphics.Text({item.Text, item.Length}, Inset(line, theme.Padding),
                      !UiEffective() ? theme.DisabledText : (selected ? theme.SelectedText : theme.Text));
        if (selected && m_application.UiFocus() == this)
        {
            graphics.FocusRect(line);
        }
    }
}
void TreeView::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::KeyDown)
    {
        if (event.Code == Key::Left && m_selected >= 0)
        {
            const UInt32 index{static_cast<UInt32>(m_selected)};
            if (m_expanded[index])
            {
                static_cast<void>(SetExpanded(index, false));
            }
            else if (m_parents[index] >= 0)
            {
                UiSelect(m_parents[index]);
            }
            event.Handled = true;
        }
        else if (event.Code == Key::Right && m_selected >= 0)
        {
            const UInt32 index{static_cast<UInt32>(m_selected)};
            if (!m_expanded[index])
            {
                static_cast<void>(SetExpanded(index, true));
            }
            else
            {
                for (UInt32 i{}; i < m_itemCount; ++i)
                {
                    if (m_parents[i] == m_selected)
                    {
                        UiSelect(static_cast<Int32>(i));
                        break;
                    }
                }
            }
            event.Handled = true;
        }
        else if (event.Code == Key::Down || event.Code == Key::Up)
        {
            const Int32 direction{event.Code == Key::Down ? 1 : -1};
            Int32 index{m_selected};
            for (UInt32 work{}; work < m_itemCount; ++work)
            {
                index += direction;
                if (index < 0 || index >= static_cast<Int32>(m_itemCount))
                {
                    break;
                }
                if (UiNodeVisible(static_cast<UInt32>(index)))
                {
                    UiSelect(index);
                    break;
                }
            }
            event.Handled = true;
        }
    }
    if (event.Kind == EventKind::MouseDown)
    {
        const Rect inner{Inset(UiBounds(), m_application.UiTheme().Border)};
        const Int32 wanted{(event.Position.Y - inner.Y) / static_cast<Int32>(m_application.UiTheme().ItemHeight)};
        Int32 row{};
        for (UInt32 i{m_first}; i < m_itemCount; ++i)
        {
            if (!UiNodeVisible(i))
            {
                continue;
            }
            if (row++ != wanted)
            {
                continue;
            }
            UiSelect(static_cast<Int32>(i));
            if (event.Position.X <
                inner.X + static_cast<Int32>((UiNodeDepth(i) + 1U) * m_application.UiTheme().Indicator))
            {
                static_cast<void>(SetExpanded(i, !m_expanded[i]));
            }
            break;
        }
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
NumericUpDown::NumericUpDown(Application& app) noexcept : TextBox(app)
{
    m_kind = Kind::NumericUpDown;
    static_cast<void>(UiAssignText(Utf8("0")));
}
void NumericUpDown::UiValue(Int32 value) noexcept
{
    value = Clamp(value, m_minimum, m_maximum);
    const bool changed{value != m_value};
    m_value = value;
    char buffer[12U]{};
    UInt32 length{};
    IntegerText(value, buffer, length);
    static_cast<void>(UiAssignText({buffer, length}));
    UiInvalidate();
    if (changed)
    {
        Event event{EventKind::ValueChanged};
        UiEmit(event);
    }
}
Status NumericUpDown::SetRange(Int32 minimum, Int32 maximum) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (minimum > maximum)
            {
                return Status::Invalid;
            }
            m_minimum = minimum;
            m_maximum = maximum;
            UiValue(m_value);
            return Status::Ok;
        });
}
Status NumericUpDown::SetValue(Int32 value) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            UiValue(value);
            return Status::Ok;
        });
}
Int32 NumericUpDown::Value() noexcept
{
    Int32 value{};
    static_cast<void>(m_application.Invoke(
        [&]() noexcept
        {
            value = m_value;
        }));
    return value;
}
void NumericUpDown::UiCommit() noexcept
{
    Foundation::Int64 value{};
    const bool negative{m_textLength != 0U && m_text[0U] == '-'};
    for (UInt32 i{negative ? 1U : 0U}; i < m_textLength; ++i)
    {
        if (m_text[i] < '0' || m_text[i] > '9')
        {
            UiValue(m_value);
            return;
        }
        value = value * 10 + (m_text[i] - '0');
        if (value > 2147483648LL)
        {
            UiValue(negative ? m_minimum : m_maximum);
            return;
        }
    }
    if (negative)
    {
        value = -value;
    }
    UiValue(static_cast<Int32>(value < m_minimum ? m_minimum : (value > m_maximum ? m_maximum : value)));
}
void NumericUpDown::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    Rect text{bounds};
    const UInt32 arrow{Min(bounds.Width, theme.Arrow)};
    text.Width -= arrow;
    TextBox::UiDraw(graphics, text);
    const Rect top{bounds.X + static_cast<Int32>(text.Width), bounds.Y, arrow, bounds.Height / 2U};
    const Rect bottom{top.X, top.Y + static_cast<Int32>(top.Height), arrow, bounds.Height - top.Height};
    graphics.FillRect(top, theme.Face);
    graphics.Bevel(top, false);
    graphics.Text(Utf8("+"), top, theme.Text, Align::Center);
    graphics.FillRect(bottom, theme.Face);
    graphics.Bevel(bottom, false);
    graphics.Text(Utf8("-"), bottom, theme.Text, Align::Center);
}
void NumericUpDown::UiInput(Event& event) noexcept
{
    Int32 direction{};
    if (event.Kind == EventKind::KeyDown && (event.Code == Key::Up || event.Code == Key::Down))
    {
        direction = event.Code == Key::Up ? 1 : -1;
    }
    if (event.Kind == EventKind::MouseDown &&
        event.Position.X >=
            UiBounds().X + static_cast<Int32>(m_bounds.Width - Min(m_bounds.Width, m_application.UiTheme().Arrow)))
    {
        direction = event.Position.Y < UiBounds().Y + static_cast<Int32>(m_bounds.Height / 2U) ? 1 : -1;
    }
    if (direction != 0)
    {
        const Foundation::Int64 value{static_cast<Foundation::Int64>(m_value) + direction};
        UiValue(static_cast<Int32>(value < m_minimum ? m_minimum : (value > m_maximum ? m_maximum : value)));
        event.Handled = true;
        UiEmit(event);
        return;
    }
    if ((event.Kind == EventKind::KeyDown && event.Code == Key::Enter) || event.Kind == EventKind::LostFocus)
    {
        UiCommit();
    }
    if (event.Kind == EventKind::KeyPress && !(event.Scalar >= '0' && event.Scalar <= '9') && event.Scalar != '-')
    {
        event.Handled = true;
        UiEmit(event);
        return;
    }
    TextBox::UiInput(event);
}
void RangeControl::UiValue(Int32 value) noexcept
{
    value = Clamp(value, m_minimum, m_maximum);
    if (value == m_value)
    {
        return;
    }
    m_value = value;
    UiInvalidate();
    Event event{EventKind::ValueChanged};
    UiEmit(event);
}
Status RangeControl::SetRange(Int32 minimum, Int32 maximum) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (minimum > maximum)
            {
                return Status::Invalid;
            }
            m_minimum = minimum;
            m_maximum = maximum;
            UiValue(m_value);
            UiInvalidate();
            return Status::Ok;
        });
}
Status RangeControl::SetValue(Int32 value) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            UiValue(value);
            return Status::Ok;
        });
}
Int32 RangeControl::Value() noexcept
{
    Int32 value{};
    static_cast<void>(m_application.Invoke(
        [&]() noexcept
        {
            value = m_value;
        }));
    return value;
}
UInt32 RangeControl::UiFraction(UInt32 extent) const noexcept
{
    const Foundation::Int64 range{static_cast<Foundation::Int64>(m_maximum) - m_minimum};
    return range == 0 ? 0U
                      : static_cast<UInt32>((static_cast<Foundation::Int64>(m_value) - m_minimum) * extent / range);
}
void RangeControl::UiFromPoint(Point point, Rect bounds) noexcept
{
    if (bounds.Width == 0U)
    {
        return;
    }
    const Int32 offset{Clamp(point.X - bounds.X, 0, static_cast<Int32>(bounds.Width))};
    const Foundation::Int64 value{m_minimum +
                                  (static_cast<Foundation::Int64>(m_maximum) - m_minimum) * offset / bounds.Width};
    UiValue(static_cast<Int32>(value));
}
void ProgressBar::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    graphics.Bevel(bounds, true);
    Rect inner{Inset(bounds, theme.Border)};
    inner.Width = UiFraction(inner.Width);
    graphics.FillRect(inner, UiEffective() ? theme.Selection : theme.Shadow);
}
void TrackBar::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    const Rect inner{Inset(bounds, theme.Padding)};
    const Rect rail{inner.X, inner.Y + static_cast<Int32>(inner.Height / 2U), inner.Width, theme.Border};
    graphics.Bevel(rail, true);
    const UInt32 width{Min(inner.Width, theme.MinimumThumb)};
    Rect thumb{inner.X + static_cast<Int32>(UiFraction(inner.Width - width)), inner.Y, width, inner.Height};
    graphics.FillRect(thumb, UiEffective() ? theme.Light : theme.Face);
    graphics.Bevel(thumb, m_pressed);
    if (m_application.UiFocus() == this)
    {
        graphics.FocusRect(bounds);
    }
}
void TrackBar::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseDown || (event.Kind == EventKind::MouseMove && m_pressed))
    {
        UiFromPoint(event.Position, Inset(UiBounds(), m_application.UiTheme().Padding));
    }
    if (event.Kind == EventKind::KeyDown || event.Kind == EventKind::MouseWheel)
    {
        Int32 direction{};
        if (event.Code == Key::Left || event.Code == Key::Down)
        {
            direction = -1;
        }
        if (event.Code == Key::Right || event.Code == Key::Up)
        {
            direction = 1;
        }
        if (event.Kind == EventKind::MouseWheel)
        {
            direction = Clamp(event.Wheel, -1, 1);
        }
        if (direction != 0)
        {
            const Foundation::Int64 value{static_cast<Foundation::Int64>(m_value) + direction};
            UiValue(static_cast<Int32>(value < m_minimum ? m_minimum : (value > m_maximum ? m_maximum : value)));
            event.Handled = true;
        }
        if (event.Code == Key::Home || event.Code == Key::End)
        {
            UiValue(event.Code == Key::Home ? m_minimum : m_maximum);
            event.Handled = true;
        }
    }
    Control::UiInput(event);
}
Status ScrollBar::SetPage(UInt32 page, bool vertical) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (page == 0U || page > MaximumAxis)
            {
                return Status::Invalid;
            }
            m_page = page;
            m_vertical = vertical;
            UiInvalidate();
            return Status::Ok;
        });
}
Rect ScrollBar::UiTrack(Rect bounds) const noexcept
{
    const UInt32 arrow{Min(m_vertical ? bounds.Height / 2U : bounds.Width / 2U, m_application.UiTheme().Arrow)};
    if (m_vertical)
    {
        bounds.Y += static_cast<Int32>(arrow);
        bounds.Height -= 2U * arrow;
    }
    else
    {
        bounds.X += static_cast<Int32>(arrow);
        bounds.Width -= 2U * arrow;
    }
    return bounds;
}
void ScrollBar::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Light);
    const Rect track{UiTrack(bounds)};
    const UInt32 extent{m_vertical ? track.Height : track.Width};
    const UInt64 range{static_cast<UInt64>(static_cast<Foundation::Int64>(m_maximum) - m_minimum)};
    const UInt32 thumbSize{
        Min(extent, static_cast<UInt32>(extent * static_cast<UInt64>(m_page) / (range + m_page)) + theme.MinimumThumb)};
    Rect thumb{track};
    const UInt32 shift{UiFraction(extent - thumbSize)};
    if (m_vertical)
    {
        thumb.Y += static_cast<Int32>(shift);
        thumb.Height = thumbSize;
    }
    else
    {
        thumb.X += static_cast<Int32>(shift);
        thumb.Width = thumbSize;
    }
    graphics.FillRect(thumb, theme.Face);
    graphics.Bevel(thumb, m_pressed);
    const UInt32 arrow{Min(m_vertical ? bounds.Height / 2U : bounds.Width / 2U, theme.Arrow)};
    Rect first{bounds};
    Rect last{bounds};
    if (m_vertical)
    {
        first.Height = arrow;
        last.Height = arrow;
        last.Y += static_cast<Int32>(bounds.Height - arrow);
    }
    else
    {
        first.Width = arrow;
        last.Width = arrow;
        last.X += static_cast<Int32>(bounds.Width - arrow);
    }
    graphics.FillRect(first, theme.Face);
    graphics.Bevel(first, false);
    graphics.Text(Utf8("-"), first, theme.Text, Align::Center);
    graphics.FillRect(last, theme.Face);
    graphics.Bevel(last, false);
    graphics.Text(Utf8("+"), last, theme.Text, Align::Center);
    if (m_application.UiFocus() == this)
    {
        graphics.FocusRect(thumb);
    }
}
void ScrollBar::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::MouseDown || (event.Kind == EventKind::MouseMove && m_pressed))
    {
        const Rect track{UiTrack(UiBounds())};
        const Int32 point{m_vertical ? event.Position.Y : event.Position.X};
        const Int32 start{m_vertical ? track.Y : track.X};
        const UInt32 extent{m_vertical ? track.Height : track.Width};
        if (point < start || point >= start + static_cast<Int32>(extent))
        {
            const Foundation::Int64 value{static_cast<Foundation::Int64>(m_value) + (point < start ? -1 : 1)};
            UiValue(static_cast<Int32>(value < m_minimum ? m_minimum : (value > m_maximum ? m_maximum : value)));
        }
        else
        {
            UiFromPoint({point, 0}, {start, 0, extent, 1U});
        }
        Control::UiInput(event);
        return;
    }
    TrackBar::UiInput(event);
}
void LinkLabel::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    const Color ink{UiEffective() ? theme.Link : theme.DisabledText};
    graphics.Text(UiText(), bounds, ink, m_alignment);
    const UInt32 width{Min(bounds.Width, graphics.Measure(UiText()))};
    graphics.FillRect(
        {bounds.X, bounds.Y + static_cast<Int32>((bounds.Height + theme.FontSize) / 2U), width, theme.CaretWidth}, ink);
    if (m_application.UiFocus() == this)
    {
        graphics.FocusRect(bounds);
    }
}
void StatusBar::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    Label::UiDraw(graphics, bounds);
    graphics.Bevel(bounds, true);
}
Status ContextMenu::Show(Form& form, Point position) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (&form.App() != &m_application)
            {
                return Status::Invalid;
            }
            if (m_parent != nullptr && m_parent != &form)
            {
                static_cast<void>(m_parent->Remove(*this));
            }
            if (m_parent == nullptr && form.Add(*this) != Status::Ok)
            {
                return Status::Limit;
            }
            const auto& theme{m_application.UiTheme()};
            Graphics measure{{}, {}, theme};
            UInt32 width{theme.DefaultWidth};
            for (UInt32 i{}; i < m_itemCount; ++i)
            {
                const UInt32 next{measure.Measure({m_items[i].Text, m_items[i].Length}) + 2U * theme.Padding};
                if (next > width)
                {
                    width = next;
                }
            }
            const UInt32 height{Min(form.State().Bounds.Height, m_itemCount * theme.ItemHeight + 2U * theme.Border)};
            const Rect root{form.State().Bounds};
            position.X = Clamp(position.X, 0, static_cast<Int32>(root.Width > width ? root.Width - width : 0U));
            position.Y = Clamp(position.Y, 0, static_cast<Int32>(root.Height > height ? root.Height - height : 0U));
            static_cast<void>(SetBounds({position.X, position.Y, width, height}));
            m_previousFocus = m_application.UiFocus();
            static_cast<void>(SetVisible(true));
            static_cast<void>(BringToFront());
            static_cast<void>(Focus());
            UiSelect(UiAdjacent(1));
            return Status::Ok;
        });
}
Status ContextMenu::Close() noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            Control* previous{m_previousFocus};
            m_previousFocus = nullptr;
            static_cast<void>(SetVisible(false));
            if (previous != nullptr)
            {
                static_cast<void>(m_application.Focus(previous));
            }
            if (m_parent != nullptr)
            {
                static_cast<void>(m_parent->Remove(*this));
            }
            return Status::Ok;
        });
}
void ContextMenu::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, theme.Face);
    graphics.Bevel(bounds, false);
    const Rect inner{Inset(bounds, theme.Border)};
    for (UInt32 i{}; i < m_itemCount; ++i)
    {
        const UInt32 offset{i * theme.ItemHeight};
        if (offset >= inner.Height)
        {
            break;
        }
        const auto& item{m_items[i]};
        const Rect row{inner.X, inner.Y + static_cast<Int32>(offset), inner.Width,
                       Min(theme.ItemHeight, inner.Height - offset)};
        if (item.Type == Item::Style::Separator)
        {
            const Rect line{Inset(row, theme.Padding)};
            graphics.Bevel({line.X, row.Y + static_cast<Int32>(row.Height / 2U), line.Width, theme.Border}, true);
            continue;
        }
        const bool selected{m_selected == static_cast<Int32>(i)};
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
void ContextMenu::UiActivate(Event& event) noexcept
{
    if (m_selected < 0 || m_selected >= static_cast<Int32>(m_itemCount))
    {
        return;
    }
    const auto& item{m_items[static_cast<UInt32>(m_selected)]};
    if (!item.Enabled || item.Type == Item::Style::Separator)
    {
        return;
    }
    event.Item = m_selected;
    Control::UiActivate(event);
    static_cast<void>(Close());
}
void ContextMenu::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::KeyDown && event.Code == Key::Escape)
    {
        static_cast<void>(Close());
        event.Handled = true;
    }
    if (event.Kind == EventKind::LostFocus && m_visible)
    {
        m_previousFocus = nullptr;
        static_cast<void>(Close());
    }
    if (event.Kind == EventKind::KeyDown && (event.Code == Key::Down || event.Code == Key::Up))
    {
        UiSelect(UiAdjacent(event.Code == Key::Down ? 1 : -1));
        event.Handled = true;
    }
    if (event.Kind == EventKind::MouseMove || event.Kind == EventKind::MouseDown)
    {
        const Rect inner{Inset(UiBounds(), m_application.UiTheme().Border)};
        if (Contains(inner, event.Position))
        {
            const UInt32 index{static_cast<UInt32>(event.Position.Y - inner.Y) / m_application.UiTheme().ItemHeight};
            if (index < m_itemCount && m_items[index].Enabled && m_items[index].Type != Item::Style::Separator)
            {
                UiSelect(static_cast<Int32>(index));
            }
        }
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
Status MenuStrip::SetMenu(UInt32 index, ContextMenu& menu) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (index >= m_itemCount || &menu.App() != &m_application)
            {
                return Status::Invalid;
            }
            m_menus[index] = &menu;
            return Status::Ok;
        });
}
void MenuStrip::UiActivate(Event& event) noexcept
{
    if (m_selected >= 0 && m_selected < static_cast<Int32>(m_itemCount) &&
        m_menus[static_cast<UInt32>(m_selected)] != nullptr && UiForm() != nullptr)
    {
        const Rect item{UiItemBounds(static_cast<UInt32>(m_selected), UiBounds())};
        static_cast<void>(m_menus[static_cast<UInt32>(m_selected)]->Show(
            *UiForm(), {item.X, item.Y + static_cast<Int32>(item.Height)}));
    }
    else
    {
        ToolBar::UiActivate(event);
    }
}
ToolTip::~ToolTip() noexcept
{
    if (m_timer != 0U)
    {
        static_cast<void>(m_application.StopTimer(m_timer));
    }
}
Status ToolTip::ShowLater(Form& form, Point position, TextView text) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!ValidText(text, MaximumText) || &form.App() != &m_application)
            {
                return Status::Invalid;
            }
            static_cast<void>(Hide());
            if (m_parent != nullptr)
            {
                static_cast<void>(m_parent->Remove(*this));
            }
            const Status status{form.Add(*this)};
            if (status != Status::Ok)
            {
                return status;
            }
            static_cast<void>(SetText(text));
            const Point size{UiPreferred()};
            static_cast<void>(
                SetBounds({position.X, position.Y, static_cast<UInt32>(size.X), static_cast<UInt32>(size.Y)}));
            return m_application.StartTimer(m_timer, m_application.UiTheme().ToolTipDelay,
                                            Action::Bind(
                                                [this]() noexcept
                                                {
                                                    static_cast<void>(m_application.StopTimer(m_timer));
                                                    m_timer = 0U;
                                                    static_cast<void>(SetVisible(true));
                                                    static_cast<void>(BringToFront());
                                                }));
        });
}
Status ToolTip::Hide() noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (m_timer != 0U)
            {
                static_cast<void>(m_application.StopTimer(m_timer));
                m_timer = 0U;
            }
            return SetVisible(false);
        });
}
void ToolTip::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    Label::UiDraw(graphics, bounds);
    graphics.Bevel(bounds, false);
}
} // namespace Asteria::User::Toolkit
