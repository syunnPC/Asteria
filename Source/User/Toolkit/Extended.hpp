// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_EXTENDED_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_EXTENDED_HPP_INCLUDED

#include "User/Toolkit/Layout.hpp"

namespace Asteria::User::Toolkit
{
class ListView final : public ListBox
{
public:
    explicit ListView(Application& app) noexcept : ListBox(app)
    {
        m_kind = Kind::ListView;
    }
    [[nodiscard]] Status AddColumn(TextView title, UInt32 width) noexcept;
    [[nodiscard]] Status AddRow(const TextView* cells, UInt32 count) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    Item m_columns[MaximumCells]{};
    Item m_cells[MaximumItems][MaximumCells]{};
    UInt32 m_widths[MaximumCells]{};
    UInt32 m_columnCount{};
};
class TreeView final : public ListBox
{
public:
    explicit TreeView(Application& app) noexcept : ListBox(app)
    {
        m_kind = Kind::TreeView;
    }
    [[nodiscard]] Status AddNode(Int32 parent, TextView title, UInt32& index) noexcept;
    [[nodiscard]] Status SetExpanded(UInt32 index, bool expanded) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    [[nodiscard]] bool UiNodeVisible(UInt32 index) const noexcept;
    [[nodiscard]] UInt32 UiNodeDepth(UInt32 index) const noexcept;
    [[nodiscard]] bool UiHasChildren(UInt32 index) const noexcept;
    Int32 m_parents[MaximumItems]{};
    bool m_expanded[MaximumItems]{};
};
class NumericUpDown final : public TextBox
{
public:
    explicit NumericUpDown(Application& app) noexcept;
    [[nodiscard]] Status SetRange(Int32 minimum, Int32 maximum) noexcept;
    [[nodiscard]] Status SetValue(Int32 value) noexcept;
    [[nodiscard]] Int32 Value() noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    void UiValue(Int32 value) noexcept;
    void UiCommit() noexcept;
    Int32 m_value{};
    Int32 m_minimum{};
    Int32 m_maximum{100};
};
class RangeControl : public Control
{
public:
    explicit RangeControl(Application& app, Kind kind) noexcept : Control(app, kind)
    {
    }
    [[nodiscard]] Status SetRange(Int32 minimum, Int32 maximum) noexcept;
    [[nodiscard]] Status SetValue(Int32 value) noexcept;
    [[nodiscard]] Int32 Value() noexcept;

protected:
    void UiValue(Int32 value) noexcept;
    [[nodiscard]] UInt32 UiFraction(UInt32 extent) const noexcept;
    void UiFromPoint(Point point, Rect bounds) noexcept;
    Int32 m_value{};
    Int32 m_minimum{};
    Int32 m_maximum{100};
};
class ProgressBar final : public RangeControl
{
public:
    explicit ProgressBar(Application& app) noexcept : RangeControl(app, Kind::ProgressBar)
    {
    }

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
};
class TrackBar : public RangeControl
{
public:
    explicit TrackBar(Application& app) noexcept : RangeControl(app, Kind::TrackBar)
    {
        m_tabStop = true;
    }
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
};
class ScrollBar final : public TrackBar
{
public:
    explicit ScrollBar(Application& app) noexcept : TrackBar(app)
    {
        m_kind = Kind::ScrollBar;
    }
    [[nodiscard]] Status SetPage(UInt32 page, bool vertical = true) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    [[nodiscard]] Rect UiTrack(Rect bounds) const noexcept;
    UInt32 m_page{10U};
    bool m_vertical{true};
};
class LinkLabel final : public Label
{
public:
    explicit LinkLabel(Application& app) noexcept : Label(app)
    {
        m_kind = Kind::LinkLabel;
        m_tabStop = true;
    }

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
};
class StatusBar final : public Label
{
public:
    explicit StatusBar(Application& app) noexcept : Label(app)
    {
        m_kind = Kind::StatusBar;
    }

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
};
// Menus use bounded, caller-owned popup items. No filesystem/global authority.
class ContextMenu : public ItemsControl
{
public:
    explicit ContextMenu(Application& app) noexcept : ItemsControl(app, Kind::ContextMenu)
    {
        m_visible = false;
    }
    [[nodiscard]] Status Show(Form& form, Point position) noexcept;
    [[nodiscard]] Status Close() noexcept;
    void UiInput(Event& event) noexcept override;
    void UiActivate(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    Control* m_previousFocus{};
};
class MenuStrip final : public ToolBar
{
public:
    explicit MenuStrip(Application& app) noexcept : ToolBar(app)
    {
        m_kind = Kind::MenuStrip;
    }
    [[nodiscard]] Status SetMenu(UInt32 index, ContextMenu& menu) noexcept;
    void UiActivate(Event& event) noexcept override;

private:
    ContextMenu* m_menus[MaximumItems]{};
};
class ToolTip final : public Label
{
public:
    explicit ToolTip(Application& app) noexcept : Label(app)
    {
        m_kind = Kind::ToolTip;
        m_visible = false;
    }
    ~ToolTip() noexcept override;
    [[nodiscard]] Status ShowLater(Form& form, Point position, TextView text) noexcept;
    [[nodiscard]] Status Hide() noexcept;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    UInt32 m_timer{};
};
} // namespace Asteria::User::Toolkit
#endif
