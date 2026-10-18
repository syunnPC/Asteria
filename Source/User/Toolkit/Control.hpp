// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_CONTROL_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_CONTROL_HPP_INCLUDED

#include "User/Toolkit/Application.hpp"
#include "User/Toolkit/Graphics.hpp"

namespace Asteria::User::Toolkit
{
enum class Kind : UInt8
{
    Control,
    Form,
    Button,
    CheckBox,
    RadioButton,
    Label,
    TextBox,
    PictureBox,
    ToolBar,
    ComboBox,
    GroupBox,
    Panel,
    TabControl,
    SplitContainer,
    FlowLayoutPanel,
    TableLayoutPanel,
    ListBox,
    ListView,
    TreeView,
    NumericUpDown,
    ProgressBar,
    TrackBar,
    ScrollBar,
    LinkLabel,
    MenuStrip,
    ContextMenu,
    StatusBar,
    ToolTip
};
struct ControlState
{
    Rect Bounds{};
    bool Visible{};
    bool Enabled{};
    bool Focused{};
    bool Hot{};
    bool Pressed{};
    bool Checked{};
    Int32 Selected{-1};
    UInt32 TextLength{};
};
class Control
{
    friend class RadioButton;
    friend class FlowLayoutPanel;
    friend class TableLayoutPanel;
    friend class SplitContainer;
    friend class TabControl;

public:
    explicit Control(Application& application, Kind kind = Kind::Control) noexcept;
    virtual ~Control() noexcept;
    static void* operator new(Size) = delete;
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
    [[nodiscard]] Status Add(Control& child) noexcept;
    [[nodiscard]] Status Remove(Control& child) noexcept;
    [[nodiscard]] Status BringToFront() noexcept;
    [[nodiscard]] Status SetBounds(Rect bounds) noexcept;
    [[nodiscard]] Status SetText(TextView text) noexcept;
    [[nodiscard]] Status CopyText(char* buffer, UInt32 capacity, UInt32& length) noexcept;
    [[nodiscard]] Status SetVisible(bool visible) noexcept;
    [[nodiscard]] Status SetEnabled(bool enabled) noexcept;
    [[nodiscard]] Status SetTabOrder(UInt32 order, bool stop = true) noexcept;
    [[nodiscard]] Status SetLayout(Anchor anchor, Dock dock, bool autoSize = false) noexcept;
    [[nodiscard]] Status SetSpacing(Insets padding, Insets margin) noexcept;
    [[nodiscard]] Status SetMnemonic(UInt32 scalar) noexcept;
    [[nodiscard]] Status SetHandler(EventKind kind, Handler handler) noexcept;
    template <typename T> [[nodiscard]] Status On(EventKind kind, const T& handler) noexcept
    {
        return SetHandler(kind, Handler::Bind(handler));
    }
    [[nodiscard]] Status Focus() noexcept
    {
        return m_application.Focus(this);
    }
    [[nodiscard]] Status Invalidate() noexcept;
    [[nodiscard]] ControlState State() noexcept;
    [[nodiscard]] Application& App() const noexcept
    {
        return m_application;
    }
    [[nodiscard]] Kind Type() const noexcept
    {
        return m_kind;
    }
    // Ui-prefixed tree/paint primitives require the UI thread.
    [[nodiscard]] bool UiEffective() const noexcept;
    [[nodiscard]] Form* UiForm() noexcept;
    [[nodiscard]] const Form* UiForm() const noexcept;
    [[nodiscard]] Rect UiBounds() const noexcept;
    [[nodiscard]] Rect UiClip() const noexcept;
    [[nodiscard]] Control* UiHit(Point point) noexcept;
    [[nodiscard]] TextView UiText() const noexcept
    {
        return {m_text, m_textLength};
    }
    [[nodiscard]] UInt32 UiTabOrder() const noexcept
    {
        return m_tabOrder;
    }
    [[nodiscard]] bool UiTabStop() const noexcept
    {
        return m_tabStop;
    }
    [[nodiscard]] UInt32 UiMnemonic() const noexcept
    {
        return m_mnemonic;
    }
    [[nodiscard]] Control* UiParent() const noexcept
    {
        return m_parent;
    }
    [[nodiscard]] bool UiDescendantOf(const Control* parent) const noexcept;
    void UiPaint(User::Graphics::MemorySurface surface, Rect clip) noexcept;
    void UiInvalidate() noexcept;
    void UiLayout() noexcept;
    void UiSetBounds(Rect bounds, bool baseline = false) noexcept;
    void UiEmit(Event& event) noexcept;
    virtual void UiInput(Event& event) noexcept;
    virtual void UiActivate(Event& event) noexcept;

protected:
    // Controls borrow storage; deletion through a pointer is deliberately unavailable.
    static void operator delete(void*) noexcept
    {
    }
    virtual void UiDraw(Graphics& graphics, Rect bounds) noexcept;
    virtual void UiArrange() noexcept;
    [[nodiscard]] virtual Point UiScroll() const noexcept
    {
        return {};
    }
    [[nodiscard]] virtual bool UiChildVisible(const Control& child) const noexcept;
    [[nodiscard]] virtual Rect UiClient() const noexcept;
    [[nodiscard]] virtual Point UiPreferred() const noexcept;
    virtual void UiTextChanged() noexcept;
    [[nodiscard]] virtual bool UiAcceptText(TextView text) const noexcept;
    [[nodiscard]] Status UiAssignText(TextView text) noexcept;
    void UiFlagChanged() noexcept;
    Application& m_application;
    Kind m_kind{};
    Control* m_parent{};
    Control* m_children[MaximumChildren]{};
    UInt32 m_childCount{};
    Rect m_bounds{};
    Rect m_baseline{};
    Point m_parentSize{};
    Insets m_padding{};
    Insets m_margin{};
    Anchor m_anchor{Anchor::Left | Anchor::Top};
    Dock m_dock{};
    char m_text[MaximumText + 1U]{};
    UInt32 m_textLength{};
    UInt32 m_tabOrder{};
    UInt32 m_mnemonic{};
    Handler m_handlers[static_cast<UInt32>(EventKind::Count)]{};
    bool m_visible{true};
    bool m_enabled{true};
    bool m_tabStop{};
    bool m_autoSize{};
    bool m_pressed{};
    bool m_hot{};
    bool m_registered{};
    bool m_checked{};
    Int32 m_selected{-1};
};
} // namespace Asteria::User::Toolkit
#endif
