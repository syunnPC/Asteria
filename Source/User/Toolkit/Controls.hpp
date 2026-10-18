// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_CONTROLS_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_CONTROLS_HPP_INCLUDED

#include "User/Toolkit/Form.hpp"

namespace Asteria::User::Imaging
{
struct Image;
}

namespace Asteria::User::Toolkit
{
class Button : public Control
{
public:
    explicit Button(Application& app) noexcept : Control(app, Kind::Button)
    {
        m_tabStop = true;
    }

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
};
class CheckBox : public Control
{
public:
    explicit CheckBox(Application& app) noexcept : Control(app, Kind::CheckBox)
    {
        m_tabStop = true;
    }
    [[nodiscard]] virtual Status SetChecked(bool checked) noexcept;
    void UiActivate(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
    void UiCheck(bool checked) noexcept;
};
class RadioButton final : public CheckBox
{
public:
    explicit RadioButton(Application& app) noexcept : CheckBox(app)
    {
        m_kind = Kind::RadioButton;
    }
    void UiActivate(Event& event) noexcept override;
    void UiInput(Event& event) noexcept override;
    [[nodiscard]] Status SetChecked(bool checked) noexcept override;

private:
    void UiSelect() noexcept;
};
class Label : public Control
{
public:
    explicit Label(Application& app) noexcept : Control(app, Kind::Label)
    {
    }
    [[nodiscard]] Status SetAlignment(Align alignment) noexcept;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
    Align m_alignment{};
};
class TextBox : public Control
{
public:
    explicit TextBox(Application& app) noexcept : Control(app, Kind::TextBox)
    {
        m_tabStop = true;
    }
    ~TextBox() noexcept;
    [[nodiscard]] Status Select(UInt32 start, UInt32 end) noexcept;
    [[nodiscard]] Status SetPassword(bool password) noexcept;
    [[nodiscard]] Status SetMultiline(bool multiline) noexcept;
    [[nodiscard]] Status SetReadOnly(bool readOnly) noexcept;
    [[nodiscard]] Status Selection(UInt32& start, UInt32& end) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
    void UiTextChanged() noexcept override;
    [[nodiscard]] bool UiAcceptText(TextView text) const noexcept override;
    [[nodiscard]] bool UiBoundary(UInt32 index) const noexcept;
    [[nodiscard]] UInt32 UiPrevious(UInt32 index) const noexcept;
    [[nodiscard]] UInt32 UiNext(UInt32 index) const noexcept;
    void UiEdit(UInt32 scalar, bool erase = false) noexcept;
    void UiMove(UInt32 index, bool shift) noexcept;
    [[nodiscard]] UInt32 UiIndexAt(Point point) noexcept;
    [[nodiscard]] UInt32 UiLineStart(UInt32 index) const noexcept;
    [[nodiscard]] UInt32 UiWidth(UInt32 start, UInt32 end) noexcept;
    UInt32 m_caret{};
    UInt32 m_selection{};
    UInt32 m_scrollX{};
    UInt32 m_scrollY{};
    bool m_password{};
    bool m_multiline{};
    bool m_readOnly{};
    bool m_dragging{};
    bool m_caretOn{true};
    UInt32 m_caretTimer{};
};
class PictureBox final : public Control
{
public:
    explicit PictureBox(Application& app) noexcept : Control(app, Kind::PictureBox)
    {
    }
    [[nodiscard]] Status SetImage(const Image& image, bool scale = false) noexcept;
    [[nodiscard]] Status SetDecodedImage(const Imaging::Image& image, bool scale = false) noexcept;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    Image m_image{};
    bool m_scale{};
};
struct Item
{
    char Text[MaximumItemText + 1U]{};
    UInt32 Length{};
    bool Enabled{true};
    bool Checked{};
    enum class Style : UInt8
    {
        Button,
        Toggle,
        Separator,
        DropDown
    } Type{};
};
class ItemsControl : public Control
{
public:
    explicit ItemsControl(Application& app, Kind kind) noexcept : Control(app, kind)
    {
        m_tabStop = true;
    }
    [[nodiscard]] Status AddItem(TextView text, Item::Style style = Item::Style::Button) noexcept;
    [[nodiscard]] Status ClearItems() noexcept;
    [[nodiscard]] Status SetItemEnabled(UInt32 index, bool enabled) noexcept;
    [[nodiscard]] Status SetItemChecked(UInt32 index, bool checked) noexcept;
    [[nodiscard]] Status SetSelectedIndex(Int32 index) noexcept;

protected:
    virtual void UiSelect(Int32 index) noexcept;
    [[nodiscard]] Int32 UiAdjacent(Int32 direction) const noexcept;
    void UiListDraw(Graphics& graphics, Rect bounds) noexcept;
    Item m_items[MaximumItems]{};
    UInt32 m_itemCount{};
    UInt32 m_first{};
};
class ListBox : public ItemsControl
{
public:
    explicit ListBox(Application& app) noexcept : ItemsControl(app, Kind::ListBox)
    {
    }
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
};
class ComboBox : public TextBox
{
public:
    explicit ComboBox(Application& app, bool editable = true) noexcept;
    [[nodiscard]] Status AddItem(TextView text) noexcept;
    [[nodiscard]] Status SetSelectedIndex(Int32 index) noexcept;
    [[nodiscard]] Status SetDropDown(bool open) noexcept;
    void UiInput(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

private:
    class Popup final : public Control
    {
    public:
        explicit Popup(ComboBox& owner) noexcept : Control(owner.App()), m_owner{owner}
        {
        }
        void UiInput(Event& event) noexcept override;

    protected:
        void UiDraw(Graphics& graphics, Rect bounds) noexcept override;

    private:
        ComboBox& m_owner;
    };
    void UiSelect(Int32 index) noexcept;
    void UiOpen(bool open) noexcept;
    Item m_items[MaximumItems]{};
    UInt32 m_itemCount{};
    UInt32 m_first{};
    bool m_editable{};
    bool m_open{};
    Popup m_popup;
};
class DropDownList final : public ComboBox
{
public:
    explicit DropDownList(Application& app) noexcept : ComboBox(app, false)
    {
    }
};
class ToolBar : public ItemsControl
{
public:
    explicit ToolBar(Application& app) noexcept : ItemsControl(app, Kind::ToolBar)
    {
    }
    void UiInput(Event& event) noexcept override;
    void UiActivate(Event& event) noexcept override;

protected:
    void UiDraw(Graphics& graphics, Rect bounds) noexcept override;
    [[nodiscard]] Rect UiItemBounds(UInt32 index, Rect bounds) noexcept;
    Int32 m_hotItem{-1};
};
} // namespace Asteria::User::Toolkit
#endif
