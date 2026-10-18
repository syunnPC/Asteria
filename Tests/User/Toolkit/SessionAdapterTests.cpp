// SPDX-License-Identifier: Apache-2.0
#include "User/Toolkit/SessionAdapter.hpp"
#include "User/Toolkit/Fixture.hpp"
#include <cstring>

namespace Asteria::Test
{
namespace
{
namespace Ui = User::Toolkit;
namespace S = User::Session;
using Foundation::UInt32;
using Foundation::UInt64;
using TestSupport::TestContext;
class Connection final : public S::IClientConnection
{
public:
    static constexpr UInt32 Width{800U};
    static constexpr UInt32 Height{800U};
    UInt32 Pixels[2U][Width * Height]{};
    S::SessionEvent Events[64U]{};
    UInt32 Count{};
    UInt32 Head{};
    UInt64 Time{};
    UInt64 Deadline{};
    UInt64 Words[S::SessionMaximumWords]{};
    UInt32 Presents{};
    UInt32 Destroys{};
    UInt32 LastImage{};
    UInt32 Damage{};
    UInt32 Ready{};
    UInt32 ViewWidth{Width};
    UInt32 ViewHeight{Height};
    [[nodiscard]] User::Graphics::MemorySurface Image(UInt32 index) noexcept override
    {
        return {Pixels[index], ViewWidth, ViewHeight, ViewWidth, User::Graphics::PixelFormat::Bgrx};
    }
    [[nodiscard]] bool Request(UInt32 method, const UInt64* words) noexcept override
    {
        if (method == S::SessionPresentMethod)
        {
            ++Presents;
            LastImage = static_cast<UInt32>(words[1U] - 1U);
            Damage = static_cast<UInt32>(words[5U]);
            __builtin_memcpy(Words, words, sizeof(Words));
        }
        if (method == S::SessionDestroySurfaceMethod)
        {
            ++Destroys;
        }
        return true;
    }
    [[nodiscard]] bool Poll(S::SessionEvent& event) noexcept override
    {
        if (Head == Count)
        {
            return false;
        }
        event = Events[Head++ % 64U];
        return true;
    }
    void WaitUntil(UInt64 deadline) noexcept override
    {
        Deadline = deadline;
        Time = deadline;
    }
    [[nodiscard]] UInt64 Now() noexcept override
    {
        return Time;
    }
    [[nodiscard]] bool Healthy() const noexcept override
    {
        return true;
    }
    void Event(S::SessionEventKind kind, UInt64 a, UInt64 b = 0U, UInt64 c = 0U, UInt64 d = 0U) noexcept
    {
        Events[Count++ % 64U] = {kind, {a, b, c, d}};
    }
    void Return() noexcept
    {
        Event(S::SessionEventKind::ImageReturned, 1ULL << LastImage);
    }
    void Buttons(UInt32 buttons, int x, int y) noexcept
    {
        Event(S::SessionEventKind::PointerButtons, 1U,
              S::PackSessionPair(static_cast<UInt32>(x), static_cast<UInt32>(y)), buttons);
    }
    void Key(UInt32 usage, bool down, UInt32 modifiers = 0U) noexcept
    {
        Event(S::SessionEventKind::Key, 1U, down ? S::SessionKeyDown : S::SessionKeyUp, usage, modifiers);
    }
};
void Adapter(TestContext& test) noexcept
{
    static Connection connection{};
    ToolkitFixture::Host host;
    Ui::SessionAdapter adapter{connection};
    Ui::Application app{host, adapter, Ui::Theme::Classic()};
    Ui::Form form{app};
    Ui::Button button{app};
    Ui::TextBox text{app};
    Ui::CheckBox check{app};
    Ui::ListBox list{app};
    ASTERIA_EXPECT(test, "bind native-sized disjoint pools", adapter.Bind(form, 600U, 760U) == Ui::Status::Ok);
    static_cast<void>(form.Add(button));
    static_cast<void>(button.SetBounds({10, 10, 100U, 30U}));
    static_cast<void>(form.Add(text));
    static_cast<void>(text.SetBounds({10, 60, 200U, 30U}));
    static_cast<void>(form.Add(check));
    static_cast<void>(check.SetBounds({10, 110, 120U, 30U}));
    static_cast<void>(form.Add(list));
    static_cast<void>(list.SetBounds({10, 160, 120U, 70U}));
    static_cast<void>(list.AddItem(Ui::Utf8("first")));
    static_cast<void>(list.AddItem(Ui::Utf8("second")));
    int clicks{};
    int doubles{};
    static_cast<void>(button.On(Ui::EventKind::Click,
                                [&](Ui::Control&, Ui::Event&) noexcept
                                {
                                    ++clicks;
                                }));
    static_cast<void>(button.On(Ui::EventKind::DoubleClick,
                                [&](Ui::Control&, Ui::Event&) noexcept
                                {
                                    ++doubles;
                                }));
    connection.Event(S::SessionEventKind::KeyFocus, 1U, 1U);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "first image complete", connection.Presents == 1U,
                   connection.Words[6U] == S::PackSessionDamage(0U, 0U, 600U, 760U));
    connection.Buttons(1U, 20, 20);
    connection.Buttons(0U, 20, 20);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "button receives click and focus", clicks == 1, button.State().Focused);
    connection.Time += 100U;
    connection.Buttons(1U, 20, 20);
    connection.Buttons(0U, 20, 20);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "toolkit timing emits double click", doubles == 1, clicks == 2);
    connection.Buttons(1U, 20, 20);
    connection.Buttons(0U, -5, -5);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "capture releases outside without activation", clicks == 2, !button.State().Pressed);
    connection.Time += app.UiTheme().DoubleClickPeriod + 1U;
    connection.Buttons(2U, 20, 20);
    connection.Buttons(0U, 20, 20);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "secondary button edges do not activate primary controls", clicks == 2, doubles == 1);
    connection.Buttons(1U, 20, 20);
    connection.Buttons(0U, 20, 20);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "late primary click does not become a double click", clicks == 3, doubles == 1);
    connection.Buttons(1U, 30, 70);
    connection.Buttons(0U, 30, 70);
    connection.Key(4U, true);
    connection.Key(4U, false);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "US make and release insert one scalar", text.UiText().Length == 1U,
                   text.UiText().Data[0U] == 'a');
    connection.Key(4U, true, 1U);
    connection.Key(4U, false, 1U);
    connection.Key(5U, true, 2U);
    connection.Key(5U, false, 2U);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "Ctrl A selects, shifted UTF-8 text replaces selection", text.UiText().Length == 1U,
                   text.UiText().Data[0U] == 'B');
    connection.Buttons(1U, 20, 120);
    connection.Buttons(0U, 20, 120);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "checkbox toggles through session pointer edges", check.State().Checked);
    connection.Buttons(1U, 20, 200);
    connection.Buttons(0U, 20, 200);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "list selection translated", list.State().Selected == 1);
    connection.Event(S::SessionEventKind::KeyFocus, 1U, 0U);
    connection.Return();
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "focus loss releases toolkit focus", app.UiFocus() == nullptr);
    connection.Key(4U, true);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "unfocused make cannot enter a control", text.UiText().Length == 1U);
    const UInt32 held = connection.LastImage;
    static UInt32 frozen[Connection::Width * Connection::Height]{};
    __builtin_memcpy(frozen, connection.Pixels[held], sizeof(frozen));
    connection.Event(S::SessionEventKind::Arrange, 1U, 7U, S::PackSessionPair(792U, 771U), S::SessionArrangeMaximized);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "maximize acknowledges Arrange and damages full view", connection.Words[4U] == 7U,
                   connection.Words[6U] == S::PackSessionDamage(0U, 0U, 792U, 771U),
                   adapter.ArrangeFlags() == S::SessionArrangeMaximized);
    ASTERIA_EXPECT(test, "lent old image remains byte-identical during resize",
                   std::memcmp(frozen, connection.Pixels[held], sizeof(frozen)) == 0);
    connection.Event(S::SessionEventKind::ImageReturned, 3U);
    connection.Event(S::SessionEventKind::Arrange, 1U, 8U, S::PackSessionPair(600U, 760U));
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "restore repaints and acknowledges even if size previously used", connection.Words[4U] == 8U,
                   connection.Words[6U] == S::PackSessionDamage(0U, 0U, 600U, 760U));
    bool cancel{true};
    int closing{};
    int closed{};
    static_cast<void>(form.On(Ui::EventKind::Closing,
                              [&](Ui::Control&, Ui::Event& event) noexcept
                              {
                                  ++closing;
                                  event.Cancel = cancel;
                              }));
    static_cast<void>(form.On(Ui::EventKind::Closed,
                              [&](Ui::Control&, Ui::Event&) noexcept
                              {
                                  ++closed;
                              }));
    connection.Event(S::SessionEventKind::CloseAsked, 1U);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "CloseAsked uses cancellable Form close path", closing == 1, !form.UiClosed(),
                   connection.Destroys == 0U);
    cancel = false;
    connection.Event(S::SessionEventKind::CloseAsked, 1U);
    connection.Event(S::SessionEventKind::CloseAsked, 1U);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "duplicate close destroys once and waits for settlement", closing == 2, closed == 1,
                   connection.Destroys == 1U, form.UiClosed());
    connection.Event(S::SessionEventKind::ImageReturned, 3U);
    ASTERIA_EXPECT(test, "close drains returns before stopping", app.Step() == Ui::Status::Stopped);
}
void Unicode(TestContext& test) noexcept
{
    static Connection connection{};
    ToolkitFixture::Host host;
    Ui::SessionAdapter adapter{connection, User::Input::KeyboardLayout::Jis};
    Ui::Application app{host, adapter, Ui::Theme::Classic()};
    Ui::Form form{app};
    Ui::TextBox text{app};
    static_cast<void>(adapter.Bind(form, 600U, 400U));
    static_cast<void>(form.Add(text));
    static_cast<void>(text.SetBounds({10, 10, 200U, 30U}));
    connection.Event(S::SessionEventKind::KeyFocus, 1U, 1U);
    connection.Buttons(1U, 20, 20);
    connection.Buttons(0U, 20, 20);
    connection.Key(0x89U, true);
    connection.Key(0x89U, false);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "JIS yen scalar stores validated UTF-8", text.UiText().Length == 2U,
                   static_cast<unsigned char>(text.UiText().Data[0U]) == 0xC2U,
                   static_cast<unsigned char>(text.UiText().Data[1U]) == 0xA5U);
}
void LimitsAndTimers(TestContext& test) noexcept
{
    S::ClientImagePlan plan{};
    ASTERIA_EXPECT(test, "native page derivation", plan.Configure(2560U, 1440U), plan.Pages == 3600U);
    ASTERIA_EXPECT(test, "slot capacity refusal leaves plan unchanged", !plan.Configure(4096U, 4096U),
                   plan.Pages == 3600U);
    ASTERIA_EXPECT(test, "protocol extent and zero refusal", !plan.Configure(8193U, 1U), !plan.Configure(0U, 1U));
    static Connection connection{};
    ToolkitFixture::Host host;
    Ui::SessionAdapter adapter{connection};
    Ui::Application app{host, adapter, Ui::Theme::Classic()};
    Ui::Form form{app};
    connection.ViewWidth = 4097U;
    ASTERIA_EXPECT(test, "toolkit axis limit enforced before binding",
                   adapter.Bind(form, 600U, 400U) == Ui::Status::Limit);
    connection.ViewWidth = Connection::Width;
    ASTERIA_EXPECT(test, "oversized initial image refused", adapter.Bind(form, 801U, 400U) == Ui::Status::Limit);
    ASTERIA_EXPECT(test, "valid bind after refusal", adapter.Bind(form, 600U, 400U) == Ui::Status::Ok);
    int ticks{};
    UInt32 timer{};
    static_cast<void>(app.StartTimer(timer, 7U,
                                     Ui::Action::Bind(
                                         [&]() noexcept
                                         {
                                             ++ticks;
                                         })));
    adapter.WaitUntil(7U);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "timer deadline on session wait scale", connection.Deadline == 7U, ticks == 1);
    connection.Time = 100U;
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "missed timer ticks coalesce", ticks == 2);
    connection.Event(S::SessionEventKind::Arrange, 1U, 9U, S::PackSessionPair(801U, 400U));
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "Arrange cannot exceed allocated native pool", !adapter.Healthy(),
                   form.UiBounds().Width == 600U);
}
} // namespace
int Main(int, char**) noexcept
{
    TestContext test{"ToolkitSessionAdapter"};
    Adapter(test);
    LimitsAndTimers(test);
    Unicode(test);
    return test.Finish();
}
} // namespace Asteria::Test
int main(int argc, char** argv)
{
    return Asteria::Test::Main(argc, argv);
}
