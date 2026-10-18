// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBrokerLogic.hpp"
#include "User/Identity/Support.hpp"
using Asteria::User::Identity::UInt16;
using Asteria::User::Identity::UInt8;
namespace Comp = Asteria::User::Component;
namespace In = Asteria::User::Input;
static Comp::InputBrokerLogic Broker{};
static In::InputEvent Key(UInt16 usage, In::EventKind kind, UInt8 modifiers = 0U) noexcept
{
    return {.Kind = kind,
            .Modifiers = modifiers,
            .Usage = usage,
            .Source = 0U,
            .Dx = 0,
            .Dy = 0,
            .Buttons = 0U,
            .Reserved = {},
            .Wheel = 0};
}
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"TrustedInput"};
    ASTERIA_EXPECT(t, "root desktop login clients", Broker.RegisterSource() == 1U, Broker.RegisterClient() == 1U,
                   Broker.RegisterClient(true) == 2U, Broker.RegisterClient() == 3U,
                   Broker.SetFocus(2U) == Comp::InputAnswer::Done);
    const In::InputEvent held[]{Key(0xE1U, In::EventKind::KeyDown, 2U), Key(4U, In::EventKind::KeyDown, 2U)};
    ASTERIA_EXPECT(t, "physical makes precede root focus lock", Broker.Publish(1U, held, 2U) == Comp::InputAnswer::Done,
                   Broker.SetFocus(3U, true) == Comp::InputAnswer::Done, Broker.FocusLocked(),
                   Broker.SetManagerFocus(2U) == Comp::InputAnswer::Refused);
    const auto b = Key(5U, In::EventKind::KeyDown, 2U);
    In::InputEvent out[8U]{};
    ASTERIA_EXPECT(t, "held Shift does not change password input",
                   Broker.Publish(1U, &b, 1U) == Comp::InputAnswer::Done, Broker.Peek(3U, out, 8U) == 1U,
                   out[0U].Usage == 5U, out[0U].Modifiers == 0U);
    Broker.Take(3U, 1U);
    const In::InputEvent release[]{Key(4U, In::EventKind::KeyUp, 2U), Key(0xE1U, In::EventKind::KeyUp)};
    ASTERIA_EXPECT(t, "old physical releases cannot reach new client",
                   Broker.Publish(1U, release, 2U) == Comp::InputAnswer::Done, Broker.Queued(3U) == 0U);
    const In::InputEvent pointer{.Kind = In::EventKind::Motion,
                                 .Modifiers = 0U,
                                 .Usage = 0U,
                                 .Source = 0U,
                                 .Dx = 1,
                                 .Dy = 2,
                                 .Buttons = 0U,
                                 .Reserved = {},
                                 .Wheel = 0};
    ASTERIA_EXPECT(t, "trusted pointer exclusion", Broker.Publish(1U, &pointer, 1U) == Comp::InputAnswer::Done,
                   Broker.Queued(3U) == 0U);
    const In::InputEvent sas[]{Key(0xE0U, In::EventKind::KeyDown, 1U), Key(0xE2U, In::EventKind::KeyDown, 5U),
                               Key(0x4CU, In::EventKind::KeyDown, 5U)};
    ASTERIA_EXPECT(t, "SAS remains root only while locked", Broker.Publish(1U, sas, 3U) == Comp::InputAnswer::Done,
                   Broker.AttentionSource() == 1U, Broker.Queued(3U) == 0U);
    Broker.TakeAttention();
    ASTERIA_EXPECT(t, "screen refresh cannot unlock", Broker.SetFocus(3U, true) == Comp::InputAnswer::Done,
                   Broker.FocusLocked(), Broker.SetManagerFocus(2U) == Comp::InputAnswer::Refused);
    const In::InputEvent second[]{Key(0x4CU, In::EventKind::KeyUp, 5U), Key(0x4CU, In::EventKind::KeyDown, 5U)};
    ASTERIA_EXPECT(t, "held SAS modifiers still reach root", Broker.Publish(1U, second, 2U) == Comp::InputAnswer::Done,
                   Broker.AttentionSource() == 1U, Broker.Queued(3U) == 0U);
    return t.Finish();
}
