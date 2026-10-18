// SPDX-License-Identifier: Apache-2.0
#include "User/Toolkit/Fixture.hpp"

namespace Asteria::Test
{
namespace
{
namespace F = ToolkitFixture;
namespace Ui = User::Toolkit;
using Foundation::UInt32;
using TestSupport::TestContext;
class Files final : public Ui::IFileDialogService
{
public:
    [[nodiscard]] Ui::Status Enumerate(Ui::FileCapability directory, Ui::FileEntry* entries, UInt32 capacity,
                                       UInt32& count) noexcept override
    {
        if (directory.Token != 7U && directory.Token != 8U)
        {
            return Ui::Status::Invalid;
        }
        if (capacity < 2U)
        {
            return Ui::Status::Limit;
        }
        entries[0U] = {};
        entries[1U] = {};
        __builtin_memcpy(entries[0U].Name, "folder", 6U);
        entries[0U].Length = 6U;
        entries[0U].Directory = true;
        entries[0U].Handle = {8U};
        __builtin_memcpy(entries[1U].Name, "file.txt", 8U);
        entries[1U].Length = 8U;
        count = 2U;
        ++Lists;
        return Ui::Status::Ok;
    }
    [[nodiscard]] Ui::Status Select(Ui::FileCapability directory, Ui::TextView name, bool save,
                                    Ui::FileCapability& selected) noexcept override
    {
        Directory = directory.Token;
        Saved = save;
        ++Selects;
        if (name.Length != 8U || std::memcmp(name.Data, "file.txt", 8U) != 0)
        {
            return Ui::Status::Invalid;
        }
        selected = {99U};
        return Ui::Status::Ok;
    }
    UInt32 Lists{};
    UInt32 Selects{};
    Foundation::UInt64 Directory{};
    bool Saved{};
};
void Widgets(TestContext& test) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic()};
    Ui::Form form{app};
    static_cast<void>(form.SetBounds({0, 0, 400U, 300U}));
    Ui::ListView details{app};
    static_cast<void>(form.Add(details));
    static_cast<void>(details.SetBounds({0, 0, 180U, 120U}));
    static_cast<void>(details.AddColumn(Ui::Utf8("Name"), 80U));
    static_cast<void>(details.AddColumn(Ui::Utf8("Size"), 80U));
    const Ui::TextView row[]{Ui::Utf8("one"), Ui::Utf8("10")};
    static_cast<void>(details.AddRow(row, 2U));
    static_cast<void>(details.AddRow(row, 2U));
    F::Click(app, form, 10, 30);
    ASTERIA_EXPECT(test, "details rows select below header", details.State().Selected == 0);
    F::Key(app, form, Ui::Key::Down);
    ASTERIA_EXPECT(test, "details keyboard advances row", details.State().Selected == 1);
    Ui::TreeView tree{app};
    static_cast<void>(form.Add(tree));
    static_cast<void>(tree.SetBounds({200, 0, 180U, 120U}));
    UInt32 index{};
    static_cast<void>(tree.AddNode(-1, Ui::Utf8("Root"), index));
    static_cast<void>(tree.AddNode(0, Ui::Utf8("Child"), index));
    static_cast<void>(tree.AddNode(1, Ui::Utf8("Leaf"), index));
    static_cast<void>(tree.SetSelectedIndex(0));
    static_cast<void>(tree.Focus());
    F::Key(app, form, Ui::Key::Right);
    F::Key(app, form, Ui::Key::Down);
    ASTERIA_EXPECT(test, "tree right expands and down visits child", tree.State().Selected == 1);
    F::Key(app, form, Ui::Key::Right);
    F::Key(app, form, Ui::Key::Down);
    F::Key(app, form, Ui::Key::Left);
    ASTERIA_EXPECT(test, "tree left navigates to parent", tree.State().Selected == 1);
    static_cast<void>(tree.SetExpanded(0U, false));
    ASTERIA_EXPECT(test, "collapse repairs hidden selection", tree.State().Selected == 0);
    ASTERIA_EXPECT(test, "tree rejects forward parent",
                   tree.AddNode(99, Ui::Utf8("bad"), index) == Ui::Status::Invalid);
    Ui::NumericUpDown numeric{app};
    static_cast<void>(form.Add(numeric));
    static_cast<void>(numeric.SetBounds({0, 140, 160U, 28U}));
    static_cast<void>(numeric.SetRange(-20, 20));
    static_cast<void>(numeric.SetValue(19));
    static_cast<void>(numeric.Focus());
    F::Key(app, form, Ui::Key::Up);
    F::Key(app, form, Ui::Key::Up);
    ASTERIA_EXPECT(test, "numeric clamps arrows to range", numeric.Value() == 20);
    F::Key(app, form, Ui::Key::A, false, true);
    F::Character(app, form, '-');
    F::Character(app, form, '5');
    F::Key(app, form, Ui::Key::Enter);
    ASTERIA_EXPECT(test, "numeric input commits signed value", numeric.Value() == -5);
    Ui::TrackBar track{app};
    Ui::ProgressBar progress{app};
    Ui::ScrollBar scroll{app};
    static_cast<void>(form.Add(track));
    static_cast<void>(track.SetBounds({180, 140, 180U, 28U}));
    static_cast<void>(track.SetRange(-2147483647 - 1, 2147483647));
    static_cast<void>(track.SetValue(2147483647));
    static_cast<void>(track.Focus());
    F::Key(app, form, Ui::Key::Right);
    ASTERIA_EXPECT(test, "value controls handle signed extrema without overflow", track.Value() == 2147483647);
    F::Key(app, form, Ui::Key::Home);
    ASTERIA_EXPECT(test, "track home selects minimum", track.Value() == -2147483647 - 1);
    static_cast<void>(progress.SetRange(5, 5));
    ASTERIA_EXPECT(test, "zero-length progress range is defined", progress.SetValue(90) == Ui::Status::Ok,
                   progress.Value() == 5);
    ASTERIA_EXPECT(test, "invalid range refuses", scroll.SetRange(5, 4) == Ui::Status::Invalid);
    static_cast<void>(form.Add(scroll));
    static_cast<void>(scroll.SetBounds({0, 190, 180U, 22U}));
    static_cast<void>(scroll.SetPage(10U, false));
    F::Click(app, form, 175, 200);
    ASTERIA_EXPECT(test, "scroll arrow increments", scroll.Value() == 1);
    Ui::MenuStrip menu{app};
    Ui::ContextMenu context{app};
    static_cast<void>(form.Add(menu));
    static_cast<void>(menu.SetBounds({200, 190, 180U, 26U}));
    static_cast<void>(menu.AddItem(Ui::Utf8("File"), Ui::Item::Style::DropDown));
    static_cast<void>(context.AddItem(Ui::Utf8("Open")));
    static_cast<void>(context.AddItem(Ui::Utf8(""), Ui::Item::Style::Separator));
    static_cast<void>(context.AddItem(Ui::Utf8("Save")));
    static_cast<void>(menu.SetMenu(0U, context));
    static_cast<void>(menu.SetSelectedIndex(0));
    static_cast<void>(menu.Focus());
    F::Key(app, form, Ui::Key::Enter);
    F::Key(app, form, Ui::Key::Enter, false, false, Ui::EventKind::KeyUp);
    ASTERIA_EXPECT(test, "menu keyboard opens focused popup", context.State().Visible, context.State().Focused);
    F::Key(app, form, Ui::Key::Down);
    ASTERIA_EXPECT(test, "menu navigation skips separators", context.State().Selected == 2);
    F::Key(app, form, Ui::Key::Escape);
    ASTERIA_EXPECT(test, "Escape closes menu and restores focus", !context.State().Visible, menu.State().Focused);
    int selected{-1};
    static_cast<void>(context.On(Ui::EventKind::Click,
                                 [&](Ui::Control&, Ui::Event& e) noexcept
                                 {
                                     selected = e.Item;
                                 }));
    static_cast<void>(context.Show(form, {10, 30}));
    F::Key(app, form, Ui::Key::Enter);
    F::Key(app, form, Ui::Key::Enter, false, false, Ui::EventKind::KeyUp);
    ASTERIA_EXPECT(test, "menu commits item on UI thread", selected >= 0, !context.State().Visible);
    Ui::ToolTip tip{app};
    static_cast<void>(tip.ShowLater(form, {10, 220}, Ui::Utf8("Hint")));
    host.Time = app.UiTheme().ToolTipDelay - 1U;
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "tooltip delay is themed", !tip.State().Visible);
    ++host.Time;
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "tooltip timer displays", tip.State().Visible);
    static_cast<void>(tip.Hide());
    ASTERIA_EXPECT(test, "tooltip hide cancels timer", !tip.State().Visible);
    static_cast<void>(form.Remove(details));
    static_cast<void>(form.Remove(tree));
    Ui::Panel panel{app};
    Ui::Button child{app};
    static_cast<void>(form.Add(panel));
    static_cast<void>(panel.SetBounds({0, 0, 100U, 100U}));
    static_cast<void>(panel.Add(child));
    static_cast<void>(child.SetBounds({10, 150, 70U, 30U}));
    static_cast<void>(panel.SetContentSize(100U, 200U));
    static_cast<void>(panel.SetScroll({0, 100}));
    ASTERIA_EXPECT(test, "scrolling translates child bounds and clips hit tests", child.UiBounds().Y == 50,
                   form.UiHit({20, 60}) == &child, form.UiHit({20, 120}) != &child);
    static_cast<void>(panel.SetScroll({0, 80}));
    Ui::Event wheel{Ui::EventKind::MouseWheel};
    wheel.Position = {20, 80};
    wheel.Wheel = 1;
    static_cast<void>(app.Dispatch(form, wheel));
    ASTERIA_EXPECT(test, "unhandled child wheel scrolls ancestor panel", child.UiBounds().Y == 90);
}
void Dialogs(TestContext& test, bool record) noexcept
{
    F::Host host;
    Files service;
    Ui::Application app{host, host, Ui::Theme::Classic()};
    Ui::Form owner{app};
    Ui::Button other{app};
    static_cast<void>(owner.Add(other));
    static_cast<void>(other.Focus());
    Ui::MessageBox message{app};
    static UInt32 first[240U * 160U]{};
    static UInt32 second[240U * 160U]{};
    static_cast<void>(message.Window().Bind(F::Surface(first, 240U, 160U), F::Surface(second, 240U, 160U), host));
    const Ui::DialogStrings strings{Ui::Utf8("OK"), Ui::Utf8("Cancel"), Ui::Utf8("File name")};
    int completed{};
    ASTERIA_EXPECT(test, "message dialog shows with caller-supplied labels",
                   message.Show(Ui::Utf8("MessageBox"), strings,
                                Ui::Handler::Bind(
                                    [&](Ui::Control&, Ui::Event&) noexcept
                                    {
                                        ++completed;
                                    })) == Ui::Status::Ok);
    ASTERIA_EXPECT(test, "modal window rejects outside focus", other.Focus() == Ui::Status::Invalid);
    static_cast<void>(message.Window().Render());
    ASTERIA_EXPECT(test, "message dialog reviewed pixels",
                   F::Golden("MessageBox", F::Surface(first, 240U, 160U), record));
    F::Key(app, message.Window(), Ui::Key::Escape);
    ASTERIA_EXPECT(test, "message cancel callback and result", message.Result() == Ui::DialogResult::Cancelled,
                   completed == 1);
    host.Release(message.Window(), 0U);
    host.Release(message.Window(), 1U);
    Ui::FileDialog dialog{app, service};
    static_cast<void>(dialog.Window().Bind(F::Surface(first, 240U, 160U), F::Surface(second, 240U, 160U), host));
    ASTERIA_EXPECT(test, "open dialog enumerates only injected capability",
                   dialog.Show({7U}, false, strings) == Ui::Status::Ok, service.Lists == 1U);
    for (const char ch : "file.txt")
    {
        if (ch != '\0')
        {
            F::Character(app, dialog.Window(), static_cast<UInt32>(ch));
        }
    }
    static_cast<void>(dialog.Window().Render());
    ASTERIA_EXPECT(test, "file dialog reviewed pixels", F::Golden("FileDialog", F::Surface(first, 240U, 160U), record));
    F::Key(app, dialog.Window(), Ui::Key::Enter);
    Ui::FileCapability selected{};
    Ui::DialogResult result{};
    static_cast<void>(dialog.Selected(selected, result));
    ASTERIA_EXPECT(test, "open selection returns opaque grant", result == Ui::DialogResult::Accepted,
                   selected.Token == 99U, service.Directory == 7U, !service.Saved);
    host.Release(dialog.Window(), 0U);
    host.Release(dialog.Window(), 1U);
    static_cast<void>(dialog.Show({7U}, true, strings));
    F::Click(app, dialog.Window(), 20, 32);
    F::Key(app, dialog.Window(), Ui::Key::Enter);
    ASTERIA_EXPECT(test, "directory traversal exchanges opaque directory token", service.Lists == 3U);
    for (const char ch : "file.txt")
    {
        if (ch != '\0')
        {
            F::Character(app, dialog.Window(), static_cast<UInt32>(ch));
        }
    }
    F::Key(app, dialog.Window(), Ui::Key::Enter);
    static_cast<void>(dialog.Selected(selected, result));
    ASTERIA_EXPECT(test, "save selection carries directory capability and save intent", service.Saved,
                   service.Directory == 8U, result == Ui::DialogResult::Accepted);
    host.Release(dialog.Window(), 0U);
    host.Release(dialog.Window(), 1U);
    ASTERIA_EXPECT(test, "service refusal reported without global filesystem access",
                   dialog.Show({999U}, false, strings) == Ui::Status::Invalid);
}
} // namespace
} // namespace Asteria::Test
int main(int argc, char** argv)
{
    Asteria::TestSupport::TestContext test{"Toolkit extended"};
    const bool record{argc == 2 && std::strcmp(argv[1U], "--record") == 0};
    Asteria::Test::Widgets(test);
    Asteria::Test::Dialogs(test, record);
    return test.Finish();
}
