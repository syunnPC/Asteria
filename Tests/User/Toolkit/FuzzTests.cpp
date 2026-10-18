// SPDX-License-Identifier: Apache-2.0
#include "User/Toolkit/Fixture.hpp"

namespace Asteria::Test
{
namespace Ui = User::Toolkit;
namespace F = ToolkitFixture;
using Foundation::UInt32;
void Fuzz(TestSupport::TestContext& test) noexcept
{
    for (unsigned int seed{1U}; seed <= 8U; ++seed)
    {
        F::Host host;
        Ui::Application app{host, host, Ui::Theme::Classic()};
        Ui::Form form{app};
        Ui::Button button{app};
        Ui::CheckBox check{app};
        Ui::TextBox text{app};
        Ui::ComboBox combo{app};
        Ui::Panel panel{app};
        static UInt32 first[320U * 160U]{};
        static UInt32 second[320U * 160U]{};
        static_cast<void>(form.Bind(F::Surface(first, 320U, 160U), F::Surface(second, 320U, 160U), host));
        Ui::Control* controls[]{&button, &check, &text, &combo, &panel};
        for (UInt32 i{}; i < 5U; ++i)
        {
            static_cast<void>(form.Add(*controls[i]));
            static_cast<void>(controls[i]->SetBounds({static_cast<int>(i * 60U), 10, 55U, 25U}));
        }
        for (UInt32 i{}; i < Ui::MaximumItems; ++i)
        {
            static_cast<void>(combo.AddItem(Ui::Utf8("Item")));
        }
        UInt32 random{seed * 0x9E3779B9U};
        bool valid{true};
        for (UInt32 iteration{}; valid && iteration < 10000U; ++iteration)
        {
            random ^= random << 13U;
            random ^= random >> 17U;
            random ^= random << 5U;
            const UInt32 which{random % 5U};
            switch ((random >> 8U) % 12U)
            {
            case 0U:
                F::Mouse(app, form, Ui::EventKind::MouseDown, static_cast<int>(random % 360U) - 20,
                         static_cast<int>((random >> 16U) % 200U) - 20);
                break;
            case 1U:
                F::Mouse(app, form, Ui::EventKind::MouseUp, static_cast<int>(random % 360U) - 20,
                         static_cast<int>((random >> 16U) % 200U) - 20);
                break;
            case 2U:
                F::Mouse(app, form, Ui::EventKind::MouseMove, static_cast<int>(random % 320U),
                         static_cast<int>((random >> 16U) % 160U));
                break;
            case 3U:
                F::Key(app, form, static_cast<Ui::Key>(random % (static_cast<UInt32>(Ui::Key::A) + 1U)),
                       (random & 1U) != 0U);
                break;
            case 4U:
                F::Key(app, form, static_cast<Ui::Key>(random % (static_cast<UInt32>(Ui::Key::A) + 1U)), false, false,
                       Ui::EventKind::KeyUp);
                break;
            case 5U:
                F::Character(app, form, random % 0x110000U);
                break;
            case 6U:
                static_cast<void>(controls[which]->SetVisible((random & 1U) != 0U));
                break;
            case 7U:
                static_cast<void>(controls[which]->SetEnabled((random & 1U) != 0U));
                break;
            case 8U:
                static_cast<void>(
                    controls[which]->SetBounds({static_cast<int>(random % 320U), 10, random % 90U, random % 40U}));
                break;
            case 9U:
                static_cast<void>(combo.SetDropDown((random & 1U) != 0U));
                break;
            case 10U:
                static_cast<void>(app.BeginInvoke(
                    []() noexcept
                    {
                    }));
                break;
            case 11U:
                static_cast<void>(
                    text.Select(random % (Ui::MaximumText + 1U), (random >> 16U) % (Ui::MaximumText + 1U)));
                break;
            default:
                break;
            }
            host.Release(form, 1U - host.Last);
            host.Time += 3U;
            static_cast<void>(app.Step());
            UInt32 start{};
            UInt32 end{};
            static_cast<void>(text.Selection(start, end));
            valid = app.Invariants() && app.Pending() <= Ui::MaximumQueue &&
                    text.State().TextLength <= Ui::MaximumText && start <= text.State().TextLength &&
                    end <= text.State().TextLength;
        }
        ASTERIA_EXPECT(test, "10000 seeded event sequences preserve bounds focus and text invariants", valid);
        host.Release(form, 0U);
        host.Release(form, 1U);
    }
}
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Toolkit seeded events"};
    Asteria::Test::Fuzz(test);
    return test.Finish();
}
