// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_DEMO_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_DEMO_HPP_INCLUDED

#include "User/Toolkit/Dialogs.hpp"

namespace Asteria::User::Toolkit
{
// Code-only sample; keep this object and its borrowed buffers in static storage.
class DemoForm final
{
public:
    explicit DemoForm(Application& app) noexcept;
    [[nodiscard]] Form& Window() noexcept
    {
        return m_form;
    }
    // A 38-pixel row leaves a 28-pixel cell: one Classic text line plus TextBox
    // padding, or one list item plus borders. Tall controls span two rows.
    [[nodiscard]] Status Build(UInt32 columns = 2U, UInt32 cellWidth = 300U, UInt32 rowHeight = 38U) noexcept;
    [[nodiscard]] Button& ActionButton() noexcept
    {
        return m_button;
    }
    [[nodiscard]] TextBox& Entry() noexcept
    {
        return m_text;
    }
    [[nodiscard]] CheckBox& Check() noexcept
    {
        return m_check;
    }
    [[nodiscard]] ListBox& Choices() noexcept
    {
        return m_list;
    }

private:
    [[nodiscard]] Status UiBuild(UInt32 columns, UInt32 cellWidth, UInt32 rowHeight) noexcept;
    [[nodiscard]] bool UiTall(const Control& control) const noexcept;
    void UiBasic() noexcept;
    void UiContainers() noexcept;
    void UiLists() noexcept;
    void UiExtras() noexcept;
    Form m_form;
    Button m_button;
    RadioButton m_radio;
    CheckBox m_check;
    Label m_label;
    ToolBar m_toolbar;
    DropDownList m_dropDown;
    ComboBox m_combo;
    TextBox m_text;
    PictureBox m_picture;
    GroupBox m_group;
    Panel m_panel;
    TabControl m_tabs;
    Label m_page;
    SplitContainer m_split;
    Label m_left;
    Label m_right;
    FlowLayoutPanel m_flow;
    Label m_flowA;
    Label m_flowB;
    TableLayoutPanel m_table;
    Label m_tableA;
    Label m_tableB;
    ListBox m_list;
    ListView m_details;
    TreeView m_tree;
    NumericUpDown m_numeric;
    ProgressBar m_progress;
    TrackBar m_track;
    ScrollBar m_scroll;
    LinkLabel m_link;
    MenuStrip m_menu;
    ContextMenu m_context;
    StatusBar m_status;
    ToolTip m_tip;
    TextBox m_multiline;
    TextBox m_password;
    UInt8 m_pixels[4U * 4U * 4U]{};
    // Add, tab and placement order: with four columns the two-row controls
    // fill an eight-row grid without holes.
    Control* m_controls[28U]{&m_button, &m_radio,   &m_check,     &m_label, &m_toolbar, &m_dropDown, &m_combo,
                             &m_text,   &m_picture, &m_group,     &m_panel, &m_tabs,    &m_split,    &m_flow,
                             &m_table,  &m_list,    &m_details,   &m_tree,  &m_numeric, &m_progress, &m_track,
                             &m_scroll, &m_link,    &m_multiline, &m_menu,  &m_status,  &m_tip,      &m_password};
};
} // namespace Asteria::User::Toolkit
#endif
