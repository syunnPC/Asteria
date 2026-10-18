// SPDX-License-Identifier: Apache-2.0
#include "User/Input/HidRelativeMouse.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
int main()
{
    namespace I = Asteria::User::Input;
    Asteria::TestSupport::TestContext context{"HidRelativeMouse"};
    // Authored from USB HID 1.11 section 6.2.2: report ID, three buttons,
    // padding and signed 16-bit relative X/Y. No implementation reference.
    I::UInt8 descriptor[]{0x05U, 1U,    0x09U, 2U,    0xA1U, 1U,    0x85U, 7U,    0x05U, 9U,    0x19U, 1U,    0x29U,
                          3U,    0x15U, 0U,    0x25U, 1U,    0x75U, 1U,    0x95U, 3U,    0x81U, 2U,    0x75U, 5U,
                          0x95U, 1U,    0x81U, 1U,    0x05U, 1U,    0x09U, 0x30U, 0x09U, 0x31U, 0x16U, 0U,    0x80U,
                          0x26U, 0xFFU, 0x7FU, 0x75U, 16U,   0x95U, 2U,    0x81U, 6U,    0xC0U};
    I::HidRelativeMouse mouse{};
    I::EventBatch events{};
    ASTERIA_EXPECT(context, "report descriptor", mouse.Open(descriptor, sizeof(descriptor)));
    const I::UInt8 report[]{7U, 1U, 0xFEU, 0xFFU, 0x2CU, 1U};
    ASTERIA_EXPECT(context, "signed relative axes and button",
                   mouse.Mouse(report, sizeof(report), 2U, events) == I::ReportResult::Accepted, events.Count == 2U,
                   events.Events[0U].Dx == -2, events.Events[0U].Dy == 300, events.Events[0U].Source == 2U,
                   events.Events[1U].Buttons == 1U);
    const I::UInt8 released[]{7U, 0U, 0U, 0U, 0U, 0U};
    ASTERIA_EXPECT(context, "button release",
                   mouse.Mouse(released, sizeof(released), 2U, events) == I::ReportResult::Accepted, events.Count == 1U,
                   events.Events[0U].Buttons == 0U);
    ASTERIA_EXPECT(context, "truncated report", mouse.Mouse(report, 5U, 2U, events) == I::ReportResult::Short,
                   events.Count == 0U);
    const I::UInt8 unknown[]{8U};
    ASTERIA_EXPECT(context, "unknown report ID",
                   mouse.Mouse(unknown, sizeof(unknown), 2U, events) == I::ReportResult::Invalid);
    descriptor[sizeof(descriptor) - 2U] = 2U;
    ASTERIA_EXPECT(context, "absolute axes refused atomically", !mouse.Open(descriptor, sizeof(descriptor)),
                   mouse.Mouse(report, sizeof(report), 2U, events) == I::ReportResult::Accepted,
                   events.Events[0U].Dy == 300);
    mouse.Release(2U, events);
    ASTERIA_EXPECT(context, "disconnect releases report buttons", events.Count == 1U, events.Events[0U].Buttons == 0U,
                   events.Events[0U].Source == 2U);
    for (I::Size bytes{0U}; bytes < sizeof(descriptor); ++bytes)
    {
        ASTERIA_EXPECT(context, "truncated descriptors refused", !mouse.Open(descriptor, bytes));
    }
    return context.Finish();
}
