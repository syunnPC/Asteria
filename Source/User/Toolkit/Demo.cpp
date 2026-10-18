// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Demo.hpp"

#include "Foundation/Compiler.hpp"

namespace Asteria::User::Toolkit
{
namespace
{
// Relocatable const pointers stay in the component's read-only segment.
[[gnu::section(".data.rel.ro.DemoCells")]] constexpr TextView Cells[]{Utf8("ListView"), Utf8("details")};
ASTERIA_NOINLINE_NOIPA const TextView* DemoCells() noexcept
{
    return Cells;
}
} // namespace
DemoForm::DemoForm(Application& app) noexcept
    : m_form{app}, m_button{app}, m_radio{app}, m_check{app}, m_label{app}, m_toolbar{app}, m_dropDown{app},
      m_combo{app}, m_text{app}, m_picture{app}, m_group{app}, m_panel{app}, m_tabs{app}, m_page{app}, m_split{app},
      m_left{app}, m_right{app}, m_flow{app}, m_flowA{app}, m_flowB{app}, m_table{app}, m_tableA{app}, m_tableB{app},
      m_list{app}, m_details{app}, m_tree{app}, m_numeric{app}, m_progress{app}, m_track{app}, m_scroll{app},
      m_link{app}, m_menu{app}, m_context{app}, m_status{app}, m_tip{app}, m_multiline{app}, m_password{app}
{
}
Status DemoForm::Build(const UInt32 columns, const UInt32 cellWidth, const UInt32 rowHeight) noexcept
{
    // 28 controls, four of them two rows tall, occupy 32 cells. Shortest-column
    // placement keeps every column within two rows of the lowest one, so no
    // column exceeds 32 / columns + 2 rows.
    if (columns == 0U || columns > 28U || cellWidth < 40U || cellWidth > MaximumAxis / columns || rowHeight < 32U ||
        rowHeight > MaximumAxis / (32U / columns + 2U))
    {
        return Status::Limit;
    }
    return m_form.App().Call(
        [this, columns, cellWidth, rowHeight]() noexcept
        {
            return UiBuild(columns, cellWidth, rowHeight);
        });
}
ASTERIA_NOINLINE_NOIPA Status DemoForm::UiBuild(const UInt32 columns, const UInt32 cellWidth,
                                                const UInt32 rowHeight) noexcept
{
    // Sample strings are program data.
    constexpr UInt32 count{sizeof(m_controls) / sizeof(m_controls[0U])};
    UInt8 used[28U]{};
    for (UInt32 i{}; i < count; ++i)
    {
        if (m_form.Add(*m_controls[i]) != Status::Ok)
        {
            return Status::Limit;
        }
        // Each control takes the leftmost shortest column.
        UInt32 column{};
        for (UInt32 candidate{1U}; candidate < columns; ++candidate)
        {
            column = used[candidate] < used[column] ? candidate : column;
        }
        const UInt32 rows{UiTall(*m_controls[i]) ? 2U : 1U};
        static_cast<void>(m_controls[i]->SetBounds({static_cast<Int32>(column * cellWidth + 10U),
                                                    static_cast<Int32>(used[column] * rowHeight + 10U), cellWidth - 20U,
                                                    rows * rowHeight - 10U}));
        used[column] = static_cast<UInt8>(used[column] + rows);
    }
    // Centre the divider so both panes show their labels.
    static_cast<void>(m_split.SetSplit((cellWidth - 20U - m_form.App().UiTheme().Splitter) / 2U));
    UiBasic();
    UiContainers();
    UiLists();
    UiExtras();
    return Status::Ok;
}
bool DemoForm::UiTall(const Control& control) const noexcept
{
    // A tab header plus its page, a list header plus a row, two list items or
    // two text lines do not fit one 28-pixel cell.
    return &control == &m_tabs || &control == &m_list || &control == &m_details || &control == &m_multiline;
}
ASTERIA_NOINLINE_NOIPA void DemoForm::UiBasic() noexcept
{
    static_cast<void>(m_button.SetText(Utf8("Button")));
    static_cast<void>(m_check.SetText(Utf8("CheckBox")));
    static_cast<void>(m_radio.SetText(Utf8("RadioButton")));
    static_cast<void>(m_label.SetText(Utf8("Vegur UTF-8: caf\xC3\xA9")));
    static_cast<void>(m_toolbar.AddItem(Utf8("Run")));
    static_cast<void>(m_toolbar.AddItem(Utf8("Lock"), Item::Style::Toggle));
    static_cast<void>(m_toolbar.AddItem(Utf8(""), Item::Style::Separator));
    static_cast<void>(m_toolbar.AddItem(Utf8("More"), Item::Style::DropDown));
    static_cast<void>(m_dropDown.AddItem(Utf8("DropDownList")));
    static_cast<void>(m_dropDown.SetSelectedIndex(0));
    static_cast<void>(m_combo.AddItem(Utf8("Editable combo")));
    static_cast<void>(m_combo.SetSelectedIndex(0));
    static_cast<void>(m_text.SetText(Utf8("Single-line TextBox")));
    const auto& theme{m_form.App().UiTheme()};
    for (UInt32 i{}; i < 16U; ++i)
    {
        const Color color{i % 2U == 0U ? theme.Selection : theme.Highlight};
        m_pixels[4U * i] = color.Red;
        m_pixels[4U * i + 1U] = color.Green;
        m_pixels[4U * i + 2U] = color.Blue;
        m_pixels[4U * i + 3U] = 255U;
    }
    static_cast<void>(m_picture.SetImage({m_pixels, sizeof(m_pixels), 4U, 4U, 16U, ImageFormat::Rgba}, true));
}
ASTERIA_NOINLINE_NOIPA void DemoForm::UiContainers() noexcept
{
    static_cast<void>(m_group.SetText(Utf8("GroupBox")));
    static_cast<void>(m_panel.SetContentSize(400U, 100U));
    static_cast<void>(m_page.SetText(Utf8("Tab page")));
    static_cast<void>(m_tabs.AddPage(m_page, Utf8("TabControl")));
    static_cast<void>(m_left.SetText(Utf8("Left")));
    static_cast<void>(m_right.SetText(Utf8("Right")));
    static_cast<void>(m_split.Add(m_left));
    static_cast<void>(m_split.Add(m_right));
    static_cast<void>(m_flowA.SetText(Utf8("Flow")));
    static_cast<void>(m_flowB.SetText(Utf8("panel")));
    static_cast<void>(m_flow.Add(m_flowA));
    static_cast<void>(m_flow.Add(m_flowB));
    static_cast<void>(m_table.SetGrid(2U, 1U));
    static_cast<void>(m_tableA.SetText(Utf8("Table")));
    static_cast<void>(m_tableB.SetText(Utf8("panel")));
    static_cast<void>(m_table.Add(m_tableA));
    static_cast<void>(m_table.Add(m_tableB));
}
ASTERIA_NOINLINE_NOIPA void DemoForm::UiLists() noexcept
{
    static_cast<void>(m_list.AddItem(Utf8("ListBox")));
    static_cast<void>(m_list.AddItem(Utf8("Second row")));
    static_cast<void>(m_details.AddColumn(Utf8("Name"), 140U));
    static_cast<void>(m_details.AddColumn(Utf8("Value"), 120U));
    static_cast<void>(m_details.AddRow(DemoCells(), 2U));
    UInt32 node{};
    static_cast<void>(m_tree.AddNode(-1, Utf8("TreeView"), node));
    static_cast<void>(m_tree.AddNode(0, Utf8("Child"), node));
    static_cast<void>(m_tree.SetExpanded(0U, true));
    static_cast<void>(m_numeric.SetValue(42));
    static_cast<void>(m_progress.SetValue(60));
    static_cast<void>(m_track.SetValue(35));
    static_cast<void>(m_scroll.SetPage(10U, false));
}
ASTERIA_NOINLINE_NOIPA void DemoForm::UiExtras() noexcept
{
    static_cast<void>(m_link.SetText(Utf8("LinkLabel")));
    static_cast<void>(m_menu.AddItem(Utf8("Menu"), Item::Style::DropDown));
    static_cast<void>(m_context.AddItem(Utf8("Context item")));
    static_cast<void>(m_menu.SetMenu(0U, m_context));
    static_cast<void>(m_status.SetText(Utf8("StatusBar: ready")));
    static_cast<void>(m_tip.SetText(Utf8("ToolTip")));
    static_cast<void>(m_tip.SetVisible(true));
    static_cast<void>(m_multiline.SetMultiline(true));
    static_cast<void>(m_multiline.SetText(Utf8("Multi-line\nTextBox")));
    static_cast<void>(m_password.SetText(Utf8("password")));
    static_cast<void>(m_password.SetPassword(true));
}
} // namespace Asteria::User::Toolkit
