// SPDX-License-Identifier: Apache-2.0
#include "User/Toolkit/Fixture.hpp"
#include "User/Font/Font.hpp"

namespace Asteria::Test
{
namespace
{
namespace Ui = User::Toolkit;
namespace F = ToolkitFixture;
using Foundation::UInt32;
using TestSupport::TestContext;
static Ui::FontContext g_fontContext;
static User::Font::Font g_font;
static Foundation::UInt8 g_fontBytes[128U * 1024U];
static Foundation::UInt8 g_cache[128U * 1024U];
static User::Font::CacheEntry g_entries[128U];
bool LoadFont() noexcept
{
    char path[1024U]{};
    static_cast<void>(std::snprintf(path, sizeof(path), "%s/Vegur-Regular.otf", ASTERIA_FONT_DATA_DIR));
    std::FILE* file{std::fopen(path, "rb")};
    if (file == nullptr)
    {
        return false;
    }
    const auto size{std::fread(g_fontBytes, 1U, sizeof(g_fontBytes), file)};
    const bool eof{std::fgetc(file) == EOF};
    static_cast<void>(std::fclose(file));
    g_fontContext.Face = &g_font;
    return eof && g_font.Open({g_fontBytes, size}) == User::Font::Error::None &&
           g_fontContext.Cache.Configure(g_cache, sizeof(g_cache), g_entries, 128U) == User::Font::Error::None;
}
void Atlas(TestContext& test, bool record, bool scalable) noexcept
{
    F::Host host;
    const auto theme{Ui::Theme::Classic(scalable ? &g_fontContext : nullptr)};
    Ui::Application app{host, host, theme};
    Ui::Form form{app};
    Ui::Button button{app};
    Ui::RadioButton radio{app};
    Ui::CheckBox check{app};
    Ui::Label label{app};
    Ui::ToolBar toolbar{app};
    Ui::DropDownList drop{app};
    Ui::ComboBox combo{app};
    Ui::TextBox text{app};
    Ui::PictureBox picture{app};
    Ui::GroupBox group{app};
    Ui::Panel panel{app};
    Ui::TabControl tabs{app};
    Ui::Label page{app};
    Ui::SplitContainer split{app};
    Ui::Label paneA{app};
    Ui::Label paneB{app};
    Ui::FlowLayoutPanel flow{app};
    Ui::Label flowA{app};
    Ui::Label flowB{app};
    Ui::TableLayoutPanel table{app};
    Ui::Label cellA{app};
    Ui::Label cellB{app};
    Ui::ListBox list{app};
    Ui::ListView details{app};
    Ui::TreeView tree{app};
    Ui::NumericUpDown numeric{app};
    Ui::ProgressBar progress{app};
    Ui::TrackBar track{app};
    Ui::ScrollBar scroll{app};
    Ui::LinkLabel link{app};
    Ui::MenuStrip menu{app};
    Ui::ContextMenu context{app};
    Ui::StatusBar status{app};
    Ui::ToolTip tip{app};
    Ui::TextBox multiline{app};
    Ui::TextBox password{app};
    Ui::Control* controls[]{&button,  &radio,  &check,   &label,     &toolbar, &drop,   &combo, &text,
                            &picture, &group,  &panel,   &tabs,      &split,   &flow,   &table, &list,
                            &details, &tree,   &numeric, &progress,  &track,   &scroll, &link,  &menu,
                            &context, &status, &tip,     &multiline, &password};
    constexpr UInt32 count{sizeof(controls) / sizeof(controls[0U])};
    constexpr UInt32 cellWidth{184U};
    constexpr UInt32 rowHeight{80U};
    constexpr UInt32 width{cellWidth * 6U};
    constexpr UInt32 height{(count + 1U) * rowHeight};
    static UInt32 pixels[width * height]{};
    const auto surface{F::Surface(pixels, width, height)};
    static_cast<void>(form.SetBounds({0, 0, width, height}));
    Ui::Graphics graphics{surface, {0, 0, width, height}, theme};
    graphics.FillRect({0, 0, width, height}, theme.Face);
    const Ui::TextView states[]{Ui::Utf8("Normal"),  Ui::Utf8("Hot"),      Ui::Utf8("Pressed"),
                                Ui::Utf8("Focused"), Ui::Utf8("Disabled"), Ui::Utf8("Checked/selected")};
    for (UInt32 state{}; state < 6U; ++state)
    {
        graphics.Text(states[state], {static_cast<int>(state * cellWidth), 0, cellWidth, rowHeight}, theme.Text);
    }
    static_cast<void>(button.SetText(Ui::Utf8("Button")));
    static_cast<void>(radio.SetText(Ui::Utf8("RadioButton")));
    static_cast<void>(check.SetText(Ui::Utf8("CheckBox")));
    static_cast<void>(label.SetText(Ui::Utf8("Label: caf\xC3\xA9")));
    static_cast<void>(text.SetText(Ui::Utf8("TextBox: edit me")));
    static_cast<void>(toolbar.AddItem(Ui::Utf8("Go")));
    static_cast<void>(toolbar.AddItem(Ui::Utf8("Pin"), Ui::Item::Style::Toggle));
    static_cast<void>(toolbar.AddItem(Ui::Utf8(""), Ui::Item::Style::Separator));
    static_cast<void>(toolbar.AddItem(Ui::Utf8("List"), Ui::Item::Style::DropDown));
    static_cast<void>(drop.AddItem(Ui::Utf8("DropDownList")));
    static_cast<void>(drop.AddItem(Ui::Utf8("Next")));
    static_cast<void>(drop.SetSelectedIndex(0));
    static_cast<void>(combo.AddItem(Ui::Utf8("Editable combo")));
    static_cast<void>(combo.SetSelectedIndex(0));
    const Foundation::UInt8 image[]{255U, 0U, 0U, 255U, 0U, 255U, 0U, 128U, 0U, 0U, 255U, 255U, 255U, 255U, 255U, 0U};
    static_cast<void>(picture.SetImage({image, sizeof(image), 2U, 2U, 8U, Ui::ImageFormat::Rgba}, true));
    static_cast<void>(group.SetText(Ui::Utf8("GroupBox")));
    static_cast<void>(page.SetText(Ui::Utf8("Tab page")));
    static_cast<void>(tabs.AddPage(page, Ui::Utf8("One")));
    static_cast<void>(paneA.SetText(Ui::Utf8("Split")));
    static_cast<void>(paneB.SetText(Ui::Utf8("pane")));
    static_cast<void>(split.Add(paneA));
    static_cast<void>(split.Add(paneB));
    static_cast<void>(split.SetSplit(70U));
    static_cast<void>(flowA.SetText(Ui::Utf8("Flow")));
    static_cast<void>(flowB.SetText(Ui::Utf8("wrap")));
    static_cast<void>(flowA.SetBounds({0, 0, 65U, 28U}));
    static_cast<void>(flowB.SetBounds({0, 0, 65U, 28U}));
    static_cast<void>(flow.Add(flowA));
    static_cast<void>(flow.Add(flowB));
    static_cast<void>(table.SetGrid(2U, 1U));
    static_cast<void>(cellA.SetText(Ui::Utf8("Table")));
    static_cast<void>(cellB.SetText(Ui::Utf8("cell")));
    static_cast<void>(table.Add(cellA));
    static_cast<void>(table.Add(cellB));
    static_cast<void>(list.AddItem(Ui::Utf8("ListBox")));
    static_cast<void>(list.AddItem(Ui::Utf8("Second")));
    static_cast<void>(details.AddColumn(Ui::Utf8("Name"), 86U));
    static_cast<void>(details.AddColumn(Ui::Utf8("Value"), 84U));
    const Ui::TextView cells[]{Ui::Utf8("Details"), Ui::Utf8("42")};
    static_cast<void>(details.AddRow(cells, 2U));
    UInt32 node{};
    static_cast<void>(tree.AddNode(-1, Ui::Utf8("TreeView"), node));
    static_cast<void>(tree.AddNode(0, Ui::Utf8("Child"), node));
    static_cast<void>(tree.SetExpanded(0U, true));
    static_cast<void>(numeric.SetValue(42));
    static_cast<void>(progress.SetValue(60));
    static_cast<void>(track.SetValue(35));
    static_cast<void>(scroll.SetPage(10U, false));
    static_cast<void>(link.SetText(Ui::Utf8("LinkLabel")));
    static_cast<void>(menu.AddItem(Ui::Utf8("File"), Ui::Item::Style::DropDown));
    static_cast<void>(menu.AddItem(Ui::Utf8("Edit"), Ui::Item::Style::DropDown));
    static_cast<void>(context.AddItem(Ui::Utf8("Open")));
    static_cast<void>(context.AddItem(Ui::Utf8("Save")));
    static_cast<void>(status.SetText(Ui::Utf8("StatusBar")));
    static_cast<void>(tip.SetText(Ui::Utf8("ToolTip")));
    static_cast<void>(multiline.SetMultiline(true));
    static_cast<void>(multiline.SetText(Ui::Utf8("Multiline\nsecond line")));
    static_cast<void>(password.SetText(Ui::Utf8("secret")));
    static_cast<void>(password.SetPassword(true));
    for (UInt32 row{}; row < count; ++row)
    {
        Ui::Control& control{*controls[row]};
        static_cast<void>(form.Add(control));
        for (UInt32 state{}; state < 6U; ++state)
        {
            static_cast<void>(app.Focus(nullptr));
            static_cast<void>(control.SetVisible(true));
            static_cast<void>(control.SetEnabled(true));
            Ui::Event release{Ui::EventKind::MouseUp};
            release.Position = {-1, -1};
            control.UiInput(release);
            Ui::Event leave{Ui::EventKind::MouseLeave};
            control.UiInput(leave);
            const Ui::Rect rect{static_cast<int>(state * cellWidth + 5U), static_cast<int>((row + 1U) * rowHeight + 8U),
                                cellWidth - 10U, 64U};
            static_cast<void>(control.SetBounds(rect));
            if (control.Type() == Ui::Kind::CheckBox)
            {
                static_cast<void>(check.SetChecked(state == 5U));
            }
            if (control.Type() == Ui::Kind::RadioButton)
            {
                static_cast<void>(radio.SetChecked(state == 5U));
            }
            if (control.Type() == Ui::Kind::ListBox)
            {
                static_cast<void>(list.SetSelectedIndex(state == 5U ? 1 : -1));
            }
            if (control.Type() == Ui::Kind::ListView)
            {
                static_cast<void>(details.SetSelectedIndex(state == 5U ? 0 : -1));
            }
            if (control.Type() == Ui::Kind::TreeView)
            {
                static_cast<void>(tree.SetSelectedIndex(state == 5U ? 1 : -1));
            }
            static_cast<void>(toolbar.SetItemChecked(1U, state == 5U));
            if (state == 1U)
            {
                Ui::Event enter{Ui::EventKind::MouseEnter};
                enter.Position = {rect.X + 8, rect.Y + 8};
                control.UiInput(enter);
            }
            if (state == 2U)
            {
                Ui::Event down{Ui::EventKind::MouseDown};
                down.Position = {rect.X + 8, rect.Y + 8};
                control.UiInput(down);
            }
            if (state == 3U)
            {
                static_cast<void>(control.Focus());
            }
            if (state == 4U)
            {
                static_cast<void>(control.SetEnabled(false));
            }
            if (&control == &text)
            {
                static_cast<void>(text.Select(state == 3U ? 2U : 0U, state == 3U ? 7U : 0U));
            }
            control.UiPaint(surface, rect);
        }
        static_cast<void>(control.SetEnabled(true));
    }
    ASTERIA_EXPECT(test, "every control state matches reviewed pixel golden",
                   F::Golden(scalable ? "ClassicVegurStates" : "ClassicFallbackStates", surface, record));
}
void Demo(TestContext& test, bool record) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic(&g_fontContext)};
    Ui::DemoForm demo{app};
    // Two 300-pixel columns: sixteen 38-pixel rows plus the 10-pixel margin.
    static UInt32 first[600U * 618U]{};
    static UInt32 second[600U * 618U]{};
    ASTERIA_EXPECT(test, "sample binds and builds every control",
                   demo.Window().Bind(F::Surface(first, 600U, 618U), F::Surface(second, 600U, 618U), host) ==
                       Ui::Status::Ok,
                   demo.Build() == Ui::Status::Ok, demo.Window().Render() == Ui::Status::Ok);
    ASTERIA_EXPECT(test, "sample form matches reviewed golden",
                   F::Golden("DemoForm", F::Surface(first, 600U, 618U), record));
    host.Release(demo.Window(), 0U);
    host.Release(demo.Window(), 1U);
}
void Popups(TestContext& test, bool record) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic(&g_fontContext)};
    Ui::Form form{app};
    Ui::DropDownList combo{app};
    Ui::TextBox text{app};
    Ui::ContextMenu menu{app};
    static UInt32 first[420U * 220U]{};
    static UInt32 second[420U * 220U]{};
    static_cast<void>(form.Bind(F::Surface(first, 420U, 220U), F::Surface(second, 420U, 220U), host));
    static_cast<void>(form.Add(combo));
    static_cast<void>(combo.SetBounds({20, 20, 180U, 28U}));
    static_cast<void>(combo.AddItem(Ui::Utf8("Alpha")));
    static_cast<void>(combo.AddItem(Ui::Utf8("Beta")));
    static_cast<void>(combo.AddItem(Ui::Utf8("Gamma")));
    static_cast<void>(combo.SetSelectedIndex(1));
    static_cast<void>(form.Add(text));
    static_cast<void>(text.SetBounds({220, 20, 180U, 28U}));
    static_cast<void>(text.SetText(Ui::Utf8("0123456789012345678901234567890123456789")));
    static_cast<void>(text.Focus());
    F::Key(app, form, Ui::Key::End);
    static_cast<void>(form.Render());
    ASTERIA_EXPECT(test, "scrolled textbox caret and suffix are exact",
                   F::Golden("TextScroll", F::Surface(first, 420U, 220U), record));
    host.Release(form, 0U);
    host.Release(form, 1U);
    static_cast<void>(combo.Focus());
    static_cast<void>(combo.SetDropDown(true));
    static_cast<void>(form.Render());
    ASTERIA_EXPECT(test, "open combo popup overlay exact pixels",
                   F::Golden("ComboPopup", F::Surface(host.Last == 0U ? first : second, 420U, 220U), record));
    host.Release(form, 0U);
    host.Release(form, 1U);
    static_cast<void>(combo.SetDropDown(false));
    static_cast<void>(menu.AddItem(Ui::Utf8("Open")));
    static_cast<void>(menu.AddItem(Ui::Utf8(""), Ui::Item::Style::Separator));
    static_cast<void>(menu.AddItem(Ui::Utf8("Save")));
    static_cast<void>(menu.SetItemEnabled(2U, false));
    static_cast<void>(menu.Show(form, {40, 60}));
    static_cast<void>(form.Render());
    ASTERIA_EXPECT(test, "menu popup separator disabled item and focus exact pixels",
                   F::Golden("MenuPopup", F::Surface(host.Last == 0U ? first : second, 420U, 220U), record));
    static_cast<void>(menu.Close());
    host.Release(form, 0U);
    host.Release(form, 1U);
}
void Pixels(TestContext& test) noexcept
{
    UInt32 pixels[20U * 20U]{};
    const auto theme{Ui::Theme::Classic()};
    Ui::Graphics graphics{F::Surface(pixels, 20U, 20U), {2, 2, 4U, 4U}, theme};
    graphics.FillRect({0, 0, 20U, 20U}, theme.Highlight);
    graphics.Line({0, 0}, {19, 19}, theme.Text);
    ASTERIA_EXPECT(test, "drawing cannot escape clip", pixels[0U] == 0U,
                   pixels[2U * 20U + 2U] == User::Graphics::PackColor(User::Graphics::PixelFormat::Bgrx, theme.Text),
                   pixels[7U * 20U + 7U] == 0U);
    graphics.SetClip({0, 0, 20U, 20U});
    graphics.FillRect({0, 0, 20U, 20U}, theme.Highlight);
    ASTERIA_EXPECT(test, "paint callback cannot enlarge original damage clip", pixels[0U] == 0U);
    F::Host host;
    Ui::Application app{host, host, theme};
    Ui::Form form{app};
    Ui::Button themed{app};
    static_cast<void>(form.SetBounds({0, 0, 20U, 20U}));
    static_cast<void>(form.Add(themed));
    static_cast<void>(themed.SetBounds({0, 0, 20U, 20U}));
    auto custom{theme};
    custom.Face = {17U, 33U, 55U};
    custom.Highlight = {66U, 77U, 88U};
    custom.Border = 3U;
    static_cast<void>(app.SetTheme(custom));
    themed.UiPaint(F::Surface(pixels, 20U, 20U), {0, 0, 20U, 20U});
    ASTERIA_EXPECT(test, "live theme controls palette and bevel metric",
                   pixels[10U * 20U + 10U] == User::Graphics::PackColor(User::Graphics::PixelFormat::Bgrx, custom.Face),
                   pixels[0U] == User::Graphics::PackColor(User::Graphics::PixelFormat::Bgrx, custom.Highlight));
    const Foundation::UInt8 rgba[]{255U, 0U, 0U, 255U, 0U, 255U, 0U, 128U};
    const Foundation::UInt8 bgra[]{0U, 0U, 255U, 255U, 0U, 255U, 0U, 128U};
    UInt32 a[8U]{};
    UInt32 b[8U]{};
    Ui::Graphics ga{F::Surface(a, 4U, 2U), {0, 0, 4U, 2U}, theme};
    Ui::Graphics gb{F::Surface(b, 4U, 2U), {0, 0, 4U, 2U}, theme};
    ga.DrawImage({rgba, sizeof(rgba), 2U, 1U, 8U, Ui::ImageFormat::Rgba}, {0, 0, 4U, 2U}, true);
    gb.DrawImage({bgra, sizeof(bgra), 2U, 1U, 8U, Ui::ImageFormat::Bgra}, {0, 0, 4U, 2U}, true);
    ASTERIA_EXPECT(test, "RGBA/BGRA scaled drawing alpha blends identically", std::memcmp(a, b, sizeof(a)) == 0,
                   a[0U] == 0xFF0000U, a[2U] == 0x008000U);
    for (auto& pixel : a)
    {
        pixel = 0U;
    }
    ga.DrawImage({rgba, sizeof(rgba), 2U, 1U, 8U, Ui::ImageFormat::Rgba}, {0, 0, 4U, 2U}, false);
    ASTERIA_EXPECT(test, "unscaled image retains native extent", a[0U] == 0xFF0000U, a[2U] == 0U, a[4U] == 0U);
}
} // namespace
} // namespace Asteria::Test
int main(int argc, char** argv)
{
    Asteria::TestSupport::TestContext test{"Toolkit rendering"};
    const bool record{argc == 2 && std::strcmp(argv[1U], "--record") == 0};
    ASTERIA_EXPECT(test, "Vegur external data opens", Asteria::Test::LoadFont());
    Asteria::Test::Popups(test, record);
    Asteria::Test::Pixels(test);
    Asteria::Test::Atlas(test, record, false);
    Asteria::Test::Atlas(test, record, true);
    Asteria::Test::Demo(test, record);
    return test.Finish();
}
