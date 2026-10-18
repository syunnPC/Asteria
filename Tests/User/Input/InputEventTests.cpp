// SPDX-License-Identifier: Apache-2.0
#include "User/Input/InputEvent.hpp"

#include "Support/TestContext.hpp"

int main()
{
    Asteria::TestSupport::TestContext context{"InputEvent"};
    namespace In = Asteria::User::Input;
    const In::InputEvent event{.Kind = In::EventKind::KeyDown,
                               .Modifiers = 0x82U,
                               .Usage = 4U,
                               .Source = 7U,
                               .Dx = 0,
                               .Dy = 0,
                               .Buttons = 0U,
                               .Reserved = {},
                               .Wheel = 0};
    ASTERIA_EXPECT(context, "frame ABI", sizeof(event) == 24U, alignof(In::InputEvent) == 4U,
                   static_cast<In::UInt8>(event.Kind) == 1U, event.Modifiers == 0x82U, event.Source == 7U);
    In::EventBatch output{};
    In::UInt8 buttons = 0U;
    In::PointerEvents(output, 3U, 5, -3, 1U, -1, buttons);
    ASTERIA_EXPECT(context, "independent pointer changes", output.Count == 3U, output.Events[0U].Dy == -3,
                   output.Events[1U].Buttons == 1U, output.Events[2U].Wheel == -1, buttons == 1U);
    return context.Finish();
}
