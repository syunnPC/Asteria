// SPDX-License-Identifier: Apache-2.0
#include "User/Toolkit/Controls.hpp"
#include "User/Toolkit/Layout.hpp"
#include "User/Toolkit/Extended.hpp"
#include "User/Toolkit/Dialogs.hpp"
#include "User/Toolkit/Demo.hpp"
#include "User/Toolkit/Graphics.hpp"

#include "User/Toolkit/Fixture.hpp"

namespace Asteria::Test
{
namespace
{
namespace F = ToolkitFixture;
namespace Ui = User::Toolkit;
using Foundation::UInt32;
using TestSupport::TestContext;
void Input(TestContext& test) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic()};
    Ui::Form form{app};
    Ui::Button button{app};
    Ui::CheckBox check{app};
    Ui::RadioButton radioA{app};
    Ui::RadioButton radioB{app};
    Ui::TextBox text{app};
    Ui::DropDownList combo{app};
    Ui::ToolBar toolbar{app};
    Ui::Panel panel{app};
    static UInt32 first[400U * 240U]{};
    static UInt32 second[400U * 240U]{};
    ASTERIA_EXPECT(test, "bind valid disjoint images",
                   form.Bind(F::Surface(first, 400U, 240U), F::Surface(second, 400U, 240U), host) == Ui::Status::Ok);
    Ui::Control* controls[]{&button, &check, &radioA, &radioB, &text, &combo, &toolbar, &panel};
    for (UInt32 i{}; i < 8U; ++i)
    {
        ASTERIA_EXPECT(test, "attach each control", form.Add(*controls[i]) == Ui::Status::Ok);
        static_cast<void>(controls[i]->SetBounds(
            {static_cast<int>((i % 2U) * 180U + 10U), static_cast<int>((i / 2U) * 50U + 10U), 160U, 30U}));
        static_cast<void>(controls[i]->SetTabOrder(i, i != 7U));
    }
    int clicks{};
    int changes{};
    int enters{};
    int leaves{};
    int resize{};
    int paint{};
    bool uiEvents{true};
    static_cast<void>(button.On(Ui::EventKind::Click,
                                [&](Ui::Control&, Ui::Event&) noexcept
                                {
                                    ++clicks;
                                    uiEvents &= app.IsUiThread();
                                }));
    static_cast<void>(check.On(Ui::EventKind::CheckedChanged,
                               [&](Ui::Control&, Ui::Event&) noexcept
                               {
                                   ++changes;
                               }));
    static_cast<void>(button.On(Ui::EventKind::MouseEnter,
                                [&](Ui::Control&, Ui::Event&) noexcept
                                {
                                    ++enters;
                                }));
    static_cast<void>(button.On(Ui::EventKind::MouseLeave,
                                [&](Ui::Control&, Ui::Event&) noexcept
                                {
                                    ++leaves;
                                }));
    static_cast<void>(button.On(Ui::EventKind::Resize,
                                [&](Ui::Control&, Ui::Event&) noexcept
                                {
                                    ++resize;
                                }));
    static_cast<void>(button.On(Ui::EventKind::Paint,
                                [&](Ui::Control&, Ui::Event& event) noexcept
                                {
                                    if (event.Drawing != nullptr)
                                    {
                                        ++paint;
                                    }
                                }));
    F::Mouse(app, form, Ui::EventKind::MouseMove, 20, 20);
    F::Mouse(app, form, Ui::EventKind::MouseMove, 390, 230);
    F::Click(app, form, 20, 20);
    ASTERIA_EXPECT(test, "mouse activation and enter/leave events", clicks == 1, enters == 1, leaves == 1,
                   button.State().Focused, uiEvents);
    F::Mouse(app, form, Ui::EventKind::MouseDown, 20, 20);
    F::Mouse(app, form, Ui::EventKind::MouseUp, 390, 230);
    ASTERIA_EXPECT(test, "release outside cancels activation", clicks == 1, !button.State().Pressed);
    F::Key(app, form, Ui::Key::Tab);
    F::Key(app, form, Ui::Key::Space);
    F::Key(app, form, Ui::Key::Space, false, false, Ui::EventKind::KeyUp);
    ASTERIA_EXPECT(test, "Tab and Space toggle checkbox once", check.State().Focused, check.State().Checked,
                   changes == 1);
    F::Key(app, form, Ui::Key::Tab, true);
    ASTERIA_EXPECT(test, "Shift Tab reverses", button.State().Focused);
    F::Click(app, form, 20, 70);
    F::Key(app, form, Ui::Key::Right);
    ASTERIA_EXPECT(test, "radio arrows focus and select exclusive sibling", radioB.State().Focused,
                   radioB.State().Checked, !radioA.State().Checked);
    static_cast<void>(radioA.SetChecked(true));
    ASTERIA_EXPECT(test, "programmatic radio selection is exclusive", radioA.State().Checked, !radioB.State().Checked);
    static_cast<void>(panel.Add(button)); // Already attached: rejected, remains in form.
    ASTERIA_EXPECT(test, "duplicate parents are rejected", panel.Add(button) == Ui::Status::Invalid,
                   button.UiParent() == &form);
    static_cast<void>(text.Focus());
    F::Character(app, form, 'A');
    F::Character(app, form, 0xE9U);
    F::Character(app, form, 0x1F600U);
    char copied[Ui::MaximumText + 1U]{};
    UInt32 length{};
    static_cast<void>(text.CopyText(copied, sizeof(copied), length));
    ASTERIA_EXPECT(test, "Unicode keypress inserts shortest UTF8", length == 7U,
                   std::strcmp(copied, "A\xC3\xA9\xF0\x9F\x98\x80") == 0);
    ASTERIA_EXPECT(test, "selection cannot split scalar", text.Select(2U, 3U) == Ui::Status::Invalid);
    F::Key(app, form, Ui::Key::Left, true);
    F::Key(app, form, Ui::Key::Backspace);
    static_cast<void>(text.CopyText(copied, sizeof(copied), length));
    ASTERIA_EXPECT(test, "backspace deletes scalar selection", std::strcmp(copied, "A\xC3\xA9") == 0);
    F::Key(app, form, Ui::Key::Home);
    F::Key(app, form, Ui::Key::Delete);
    F::Key(app, form, Ui::Key::A, false, true);
    F::Character(app, form, 'Z');
    static_cast<void>(text.CopyText(copied, sizeof(copied), length));
    ASTERIA_EXPECT(test, "home delete and Ctrl A replacement", std::strcmp(copied, "Z") == 0);
    ASTERIA_EXPECT(test, "single-line rejects LF and malformed UTF8 atomically",
                   text.SetText(Ui::Utf8("bad\n")) == Ui::Status::Invalid,
                   text.SetText(Ui::Utf8("\xC0\xAF")) == Ui::Status::Invalid);
    static_cast<void>(text.SetMultiline(true));
    F::Key(app, form, Ui::Key::End);
    F::Key(app, form, Ui::Key::Enter);
    F::Character(app, form, 'Q');
    F::Key(app, form, Ui::Key::Up);
    F::Key(app, form, Ui::Key::Home);
    F::Character(app, form, 'X');
    static_cast<void>(text.CopyText(copied, sizeof(copied), length));
    ASTERIA_EXPECT(test, "multiline enter and vertical navigation", std::strcmp(copied, "XZ\nQ") == 0);
    static_cast<void>(combo.AddItem(Ui::Utf8("Alpha")));
    static_cast<void>(combo.AddItem(Ui::Utf8("Beta")));
    static_cast<void>(combo.Focus());
    F::Key(app, form, Ui::Key::Down);
    ASTERIA_EXPECT(test, "combo keyboard selects first item", combo.State().Selected == 0);
    ASTERIA_EXPECT(test, "open combo overlay", combo.SetDropDown(true) == Ui::Status::Ok);
    F::Click(app, form, 200, 176);
    ASTERIA_EXPECT(test, "popup chooses second item", combo.State().Selected == 1);
    static_cast<void>(combo.SetDropDown(true));
    F::Key(app, form, Ui::Key::Escape);
    ASTERIA_EXPECT(test, "Escape dismisses popup and preserves combo focus", combo.State().Focused);
    static_cast<void>(toolbar.AddItem(Ui::Utf8("Run")));
    static_cast<void>(toolbar.AddItem(Ui::Utf8("Lock"), Ui::Item::Style::Toggle));
    static_cast<void>(toolbar.AddItem(Ui::Utf8(""), Ui::Item::Style::Separator));
    static_cast<void>(toolbar.AddItem(Ui::Utf8("More"), Ui::Item::Style::DropDown));
    int toggles{};
    int dropItem{-1};
    static_cast<void>(toolbar.On(Ui::EventKind::CheckedChanged,
                                 [&](Ui::Control&, Ui::Event&) noexcept
                                 {
                                     ++toggles;
                                 }));
    static_cast<void>(toolbar.On(Ui::EventKind::Click,
                                 [&](Ui::Control&, Ui::Event& e) noexcept
                                 {
                                     dropItem = e.Item;
                                 }));
    static_cast<void>(toolbar.Focus());
    F::Key(app, form, Ui::Key::Right);
    F::Key(app, form, Ui::Key::Right);
    F::Key(app, form, Ui::Key::Space);
    F::Key(app, form, Ui::Key::Space, false, false, Ui::EventKind::KeyUp);
    ASTERIA_EXPECT(test, "toolbar toggles its selected item", toggles == 1, dropItem == 1);
    F::Key(app, form, Ui::Key::Right);
    F::Key(app, form, Ui::Key::Enter);
    F::Key(app, form, Ui::Key::Enter, false, false, Ui::EventKind::KeyUp);
    ASTERIA_EXPECT(test, "toolbar skips separators and exposes dropdown item", dropItem == 3);
    static_cast<void>(button.SetBounds({10, 10, 155U, 32U}));
    static_cast<void>(form.Render());
    ASTERIA_EXPECT(test, "resize and clipped paint callbacks fire", resize == 1, paint > 0);
    static_cast<void>(button.SetMnemonic('b'));
    Ui::Event mnemonic{Ui::EventKind::KeyDown};
    mnemonic.Alt = true;
    mnemonic.Scalar = 'b';
    static_cast<void>(app.Dispatch(form, mnemonic));
    ASTERIA_EXPECT(test, "Alt mnemonic activates", clicks == 2);
    static_cast<void>(form.SetDefaultButtons(&button, &button));
    static_cast<void>(text.Focus());
    F::Key(app, form, Ui::Key::Escape);
    ASTERIA_EXPECT(test, "Escape activates cancel button", clicks == 3);
    static_cast<void>(form.SetEnabled(false));
    ASTERIA_EXPECT(test, "ancestor disable clears focus", !button.State().Focused, app.UiFocus() == nullptr,
                   app.Invariants());
    host.Release(form, 0U);
    host.Release(form, 1U);
}
void Layout(TestContext& test) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic()};
    Ui::Form form{app};
    Ui::Button anchored{app};
    Ui::Button docked{app};
    Ui::Button fill{app};
    static_cast<void>(form.SetBounds({0, 0, 200U, 150U}));
    static_cast<void>(form.Add(anchored));
    static_cast<void>(anchored.SetBounds({10, 20, 80U, 30U}));
    static_cast<void>(anchored.SetLayout(Ui::Anchor::Left | Ui::Anchor::Right | Ui::Anchor::Top, Ui::Dock::None));
    static_cast<void>(form.Add(docked));
    static_cast<void>(docked.SetBounds({0, 0, 50U, 25U}));
    static_cast<void>(docked.SetLayout(Ui::Anchor::None, Ui::Dock::Top));
    static_cast<void>(form.Add(fill));
    static_cast<void>(fill.SetLayout(Ui::Anchor::None, Ui::Dock::Fill));
    for (UInt32 width : {100U, 200U, 333U})
    {
        static_cast<void>(form.SetBounds({0, 0, width, 200U}));
        ASTERIA_EXPECT(test, "Anchor stretches relative to original parent and Dock consumes client",
                       anchored.State().Bounds.Width == (width < 120U ? 0U : width - 120U),
                       docked.State().Bounds.Width == width, fill.State().Bounds.Y == 25,
                       fill.State().Bounds.Height == 175U);
    }
    Ui::FlowLayoutPanel flow{app};
    Ui::Label a{app};
    Ui::Label b{app};
    Ui::Label c{app};
    static_cast<void>(flow.SetBounds({0, 0, 150U, 100U}));
    Ui::Control* children[]{&a, &b, &c};
    for (auto* child : children)
    {
        static_cast<void>(child->SetBounds({0, 0, 60U, 20U}));
        static_cast<void>(flow.Add(*child));
    }
    ASTERIA_EXPECT(test, "flow places two cells then wraps", a.State().Bounds.X == 0, b.State().Bounds.X == 64,
                   c.State().Bounds.Y == 24);
    static_cast<void>(flow.SetBounds({0, 0, 70U, 100U}));
    ASTERIA_EXPECT(test, "flow recomputes at narrow size", b.State().Bounds.Y == 24, c.State().Bounds.Y == 48);
    static_cast<void>(flow.SetFlow(true));
    ASTERIA_EXPECT(test, "vertical flow uses height", b.State().Bounds.Y == 24, c.State().Bounds.Y == 48);
    Ui::TableLayoutPanel table{app};
    Ui::Label d{app};
    Ui::Label e{app};
    Ui::Label f{app};
    Ui::Label g{app};
    static_cast<void>(table.SetGrid(2U, 2U));
    static_cast<void>(table.SetWeight(true, 1U, 2U));
    Ui::Control* cells[]{&d, &e, &f, &g};
    for (auto* cell : cells)
    {
        static_cast<void>(table.Add(*cell));
    }
    for (UInt32 width : {90U, 201U, 333U})
    {
        static_cast<void>(table.SetBounds({0, 0, width, 101U}));
        ASTERIA_EXPECT(test, "weighted table assigns all pixels without cumulative rounding",
                       d.State().Bounds.Width == width / 3U, e.State().Bounds.Width == width - width / 3U,
                       f.State().Bounds.Y == 50, g.State().Bounds.Height == 51U);
    }
    static_cast<void>(a.SetText(Ui::Utf8("123")));
    static_cast<void>(a.SetLayout(Ui::Anchor::Left, Ui::Dock::None, true));
    ASTERIA_EXPECT(test, "AutoSize measures through theme font", a.State().Bounds.Width == 32U);
    Ui::TabControl tabs{app};
    Ui::Panel p0{app};
    Ui::Panel p1{app};
    Ui::Button nested{app};
    static_cast<void>(form.Remove(fill));
    static_cast<void>(form.Add(tabs));
    static_cast<void>(tabs.SetBounds({0, 30, 200U, 120U}));
    static_cast<void>(tabs.AddPage(p0, Ui::Utf8("One")));
    static_cast<void>(tabs.AddPage(p1, Ui::Utf8("Two")));
    static_cast<void>(p0.Add(nested));
    static_cast<void>(nested.Focus());
    static_cast<void>(tabs.Focus());
    F::Key(app, form, Ui::Key::Right);
    ASTERIA_EXPECT(test, "tab page visibility excludes inactive descendants", !nested.UiEffective(),
                   tabs.State().Selected == 1, app.Invariants());
    Ui::SplitContainer split{app};
    Ui::Panel left{app};
    Ui::Panel right{app};
    static_cast<void>(split.SetBounds({0, 0, 200U, 100U}));
    static_cast<void>(split.Add(left));
    static_cast<void>(split.Add(right));
    static_cast<void>(split.SetSplit(70U));
    ASTERIA_EXPECT(test, "splitter reserves theme divider", left.State().Bounds.Width == 70U,
                   right.State().Bounds.X == 75, right.State().Bounds.Width == 125U);
    static_cast<void>(split.SetSplit(40U, false));
    ASTERIA_EXPECT(test, "horizontal split lays out vertically", right.State().Bounds.Y == 45,
                   right.State().Bounds.Height == 55U);
    static_cast<void>(tabs.SetSelectedIndex(0));
    static_cast<void>(nested.Focus());
    static_cast<void>(tabs.SetSelectedIndex(1));
    ASTERIA_EXPECT(test, "programmatic tab change repairs focus", tabs.State().Focused, app.Invariants());
    ASTERIA_EXPECT(test, "tree rejects cycles", p0.Add(form) == Ui::Status::Invalid);
    Ui::Panel depth[Ui::MaximumDepth + 1U]{
        Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app},
        Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app},
        Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}, Ui::Panel{app}};
    for (UInt32 i{}; i + 1U < Ui::MaximumDepth; ++i)
    {
        ASTERIA_EXPECT(test, "bounded depth attach", depth[i].Add(depth[i + 1U]) == Ui::Status::Ok);
    }
    ASTERIA_EXPECT(test, "depth overflow is explicit",
                   depth[Ui::MaximumDepth - 1U].Add(depth[Ui::MaximumDepth]) == Ui::Status::Limit);
}
void Buffers(TestContext& test) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic()};
    Ui::Form form{app};
    Ui::Button button{app};
    static UInt32 first[80U * 60U]{};
    static UInt32 second[80U * 60U]{};
    ASTERIA_EXPECT(test, "aliased images rejected",
                   form.Bind(F::Surface(first, 80U, 60U), F::Surface(first, 80U, 60U), host) == Ui::Status::Invalid);
    static_cast<void>(form.Bind(F::Surface(first, 80U, 60U), F::Surface(second, 80U, 60U), host));
    static_cast<void>(form.Add(button));
    static_cast<void>(button.SetBounds({10, 10, 30U, 24U}));
    static_cast<void>(button.SetText(Ui::Utf8("Go")));
    ASTERIA_EXPECT(test, "first image presented complete", form.Render() == Ui::Status::Ok, host.Presents == 1U);
    UInt32 saved[80U * 60U]{};
    __builtin_memcpy(saved, first, sizeof(saved));
    static_cast<void>(button.SetText(Ui::Utf8("No")));
    ASTERIA_EXPECT(test, "other image receives carry and new damage", form.Render() == Ui::Status::Ok,
                   host.Presents == 2U, std::memcmp(saved, first, sizeof(saved)) == 0);
    static_cast<void>(button.SetText(Ui::Utf8("Yes")));
    ASTERIA_EXPECT(test, "both lent images remain unwritable", form.Render() == Ui::Status::Busy,
                   form.Bind(F::Surface(first, 80U, 60U), F::Surface(second, 80U, 60U), host) == Ui::Status::Busy);
    host.Release(form, 0U);
    host.Busy = true;
    ASTERIA_EXPECT(test, "presenter refusal retains pending damage", form.Render() == Ui::Status::Busy,
                   host.Presents == 2U);
    host.Busy = false;
    ASTERIA_EXPECT(test, "retry presents damage", form.Render() == Ui::Status::Ok, host.Presents == 3U,
                   host.DamageCount != 0U);
    ASTERIA_EXPECT(test, "undamaged pixels copied forward exactly", first[0U] == second[0U],
                   first[79U + 59U * 80U] == second[79U + 59U * 80U]);
    ASTERIA_EXPECT(test, "only invalidated control rectangle is presented", host.DamageCount == 1U,
                   host.Rectangles[0U].X == 10, host.Rectangles[0U].Y == 10, host.Rectangles[0U].Width == 30U,
                   host.Rectangles[0U].Height == 24U);
    static UInt32 reference[80U * 60U]{};
    for (UInt32 iteration{}; iteration < 20U; ++iteration)
    {
        host.Release(form, 0U);
        host.Release(form, 1U);
        static_cast<void>(button.SetText(iteration % 2U == 0U ? Ui::Utf8("Go") : Ui::Utf8("No")));
        static_cast<void>(form.Render());
        form.UiPaint(F::Surface(reference, 80U, 60U), {0, 0, 80U, 60U});
        ASTERIA_EXPECT(test, "every presented image is complete after repeated damage carry",
                       std::memcmp(reference, host.Last == 0U ? first : second, sizeof(reference)) == 0);
    }
    ASTERIA_EXPECT(test, "foreign release rejected", form.ReturnImage(8U) == Ui::Status::Invalid);
    host.Release(form, 0U);
    host.Release(form, 1U);
    Ui::Damage damage;
    for (UInt32 i{}; i < Ui::MaximumDamage + 1U; ++i)
    {
        damage.Add({static_cast<int>(i), 1, 1U, 1U});
    }
    ASTERIA_EXPECT(test, "damage overflow unions conservatively", damage.Count() == 1U,
                   damage.Rectangles()[0U].Width == Ui::MaximumDamage + 1U);
    UInt32 ticks{};
    UInt32 timer{};
    static_cast<void>(app.StartTimer(timer, 10U,
                                     Ui::Action::Bind(
                                         [&]() noexcept
                                         {
                                             ++ticks;
                                         })));
    host.Time = 9U;
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "timer not early", ticks == 0U);
    host.Time = 1000U;
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "missed timer intervals coalesce", ticks == 1U);
    static_cast<void>(app.StopTimer(timer));
    host.Time = 2000U;
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "stopped timer does not fire", ticks == 1U);
    UInt32 timers[Ui::MaximumTimers]{};
    for (auto& id : timers)
    {
        ASTERIA_EXPECT(test, "timer pool capacity accepted", app.StartTimer(id, 20U, {}) == Ui::Status::Ok);
    }
    UInt32 extra{};
    ASTERIA_EXPECT(test, "timer pool refuses overflow", app.StartTimer(extra, 20U, {}) == Ui::Status::Limit);
    for (auto id : timers)
    {
        static_cast<void>(app.StopTimer(id));
    }
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Toolkit"};
    Asteria::Test::Input(test);
    Asteria::Test::Layout(test);
    Asteria::Test::Buffers(test);
    return test.Finish();
}
