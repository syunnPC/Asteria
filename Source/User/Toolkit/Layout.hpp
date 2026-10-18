// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_LAYOUT_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_LAYOUT_HPP_INCLUDED

#include "User/Toolkit/Controls.hpp"

namespace Asteria::User::Toolkit
{
class GroupBox final : public Control
{
public:
    explicit GroupBox(Application& app) noexcept : Control(app, Kind::GroupBox)
    {
    }

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
    [[nodiscard]] Rect UiClient() const noexcept override;
};
class Panel : public Control
{
public:
    explicit Panel(Application& app) noexcept : Control(app, Kind::Panel)
    {
    }
    [[nodiscard]] Status SetScroll(Point position) noexcept;
    [[nodiscard]] Status SetContentSize(UInt32 width, UInt32 height) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    [[nodiscard]] Point UiScroll() const noexcept override
    {
        return m_scroll;
    }
    void UiClampScroll() noexcept;
    Point m_scroll{};
    UInt32 m_contentWidth{};
    UInt32 m_contentHeight{};
};
class TabControl final : public ItemsControl
{
public:
    explicit TabControl(Application& app) noexcept : ItemsControl(app, Kind::TabControl)
    {
    }
    [[nodiscard]] Status AddPage(Control& page, TextView title) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
    [[nodiscard]] Rect UiClient() const noexcept override;
    [[nodiscard]] bool UiChildVisible(const Control& child) const noexcept override;
    void UiArrange() noexcept override;
    void UiSelect(Int32 index) noexcept override;
};
class SplitContainer final : public Control
{
public:
    explicit SplitContainer(Application& app) noexcept : Control(app, Kind::SplitContainer)
    {
        m_tabStop = true;
        m_position = app.UiTheme().DefaultWidth;
    }
    [[nodiscard]] Status SetSplit(UInt32 position, bool vertical = true) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
    void UiArrange() noexcept override;

private:
    UInt32 m_position{};
    bool m_vertical{true};
    bool m_dragging{};
};
class FlowLayoutPanel final : public Panel
{
public:
    explicit FlowLayoutPanel(Application& app) noexcept : Panel(app)
    {
        m_kind = Kind::FlowLayoutPanel;
    }
    [[nodiscard]] Status SetFlow(bool vertical, bool wrap = true) noexcept;

protected:
    void UiArrange() noexcept override;

private:
    bool m_vertical{};
    bool m_wrap{true};
};
class TableLayoutPanel final : public Panel
{
public:
    explicit TableLayoutPanel(Application& app) noexcept : Panel(app)
    {
        m_kind = Kind::TableLayoutPanel;
    }
    [[nodiscard]] Status SetGrid(UInt32 columns, UInt32 rows) noexcept;
    [[nodiscard]] Status SetWeight(bool column, UInt32 index, UInt32 weight) noexcept;

protected:
    void UiArrange() noexcept override;

private:
    UInt32 m_columns{1U};
    UInt32 m_rows{1U};
    UInt32 m_columnWeight[MaximumCells]{1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U};
    UInt32 m_rowWeight[MaximumCells]{1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U};
};
} // namespace Asteria::User::Toolkit
#endif
