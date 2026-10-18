// SPDX-License-Identifier: Apache-2.0
#include "User/Input/HidBoot.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Input/ProtocolFixtures.hpp"

int main()
{
    namespace In = Asteria::User::Input;
    Asteria::TestSupport::TestContext context{"HidBoot"};
    In::HidBoot hid{12U};
    In::EventBatch output{};
    ASTERIA_EXPECT(context, "synthetic keyboard",
                   hid.Keyboard(Asteria::Test::Input::UsbKeyboardReport, 8U, output) == In::ReportResult::Accepted,
                   output.Count == 2U, output.Events[0U].Usage == 0xE1U, output.Events[1U].Usage == 4U,
                   output.Events[1U].Modifiers == 2U, output.Events[1U].Source == 12U);
    ASTERIA_EXPECT(context, "repeat report",
                   hid.Keyboard(Asteria::Test::Input::UsbKeyboardReport, 8U, output) == In::ReportResult::Accepted,
                   output.Count == 0U);
    In::UInt8 report[8U]{0xFFU, 0U, 1U, 1U, 1U, 1U, 1U, 1U};
    ASTERIA_EXPECT(context, "rollover ignored including modifiers",
                   hid.Keyboard(report, 8U, output) == In::ReportResult::Rollover, output.Count == 0U);
    ASTERIA_EXPECT(context, "short and null reports", hid.Keyboard(report, 7U, output) == In::ReportResult::Short,
                   hid.Keyboard(nullptr, 100U, output) == In::ReportResult::Short,
                   hid.Mouse(report, 2U, output) == In::ReportResult::Short);
    std::memset(report, 0, sizeof(report));
    ASTERIA_EXPECT(context, "release after rollover", hid.Keyboard(report, 8U, output) == In::ReportResult::Accepted,
                   output.Count == 2U, output.Events[0U].Kind == In::EventKind::KeyUp, output.Events[0U].Usage == 0xE1U,
                   output.Events[1U].Usage == 4U);
    ASTERIA_EXPECT(context, "synthetic mouse screen Y",
                   hid.Mouse(Asteria::Test::Input::UsbMouseReport, 4U, output) == In::ReportResult::Accepted,
                   output.Count == 2U, output.Events[0U].Dx == 5, output.Events[0U].Dy == -3);
    const In::UInt8 mouse[4U]{1U, 0x80U, 0x7FU, 0xFFU};
    ASTERIA_EXPECT(context, "signed mouse and wheel", hid.Mouse(mouse, 4U, output) == In::ReportResult::Accepted,
                   output.Count == 2U, output.Events[0U].Dx == -128, output.Events[0U].Dy == 127,
                   output.Events[1U].Wheel == -1);
    const In::UInt8 wheelUp[]{0U, 0U, 0U, 1U};
    hid.Reset();
    ASTERIA_EXPECT(context, "synthetic USB wheel up", hid.Mouse(wheelUp, 4U, output) == In::ReportResult::Accepted,
                   output.Count == 1U, output.Events[0U].Wheel == 1);
    const In::UInt8 heldButton[]{1U, 0U, 0U};
    ASTERIA_EXPECT(context, "boot mouse holds a button before disconnect",
                   hid.Mouse(heldButton, sizeof(heldButton), output) == In::ReportResult::Accepted, output.Count == 1U);
    hid.Release(output);
    ASTERIA_EXPECT(context, "device retirement publishes a balancing button release", output.Count == 1U,
                   output.Events[0U].Kind == In::EventKind::Buttons, output.Events[0U].Buttons == 0U,
                   output.Events[0U].Source == 12U);
    hid.Release(output);
    ASTERIA_EXPECT(context, "repeated retirement has no duplicate release", output.Count == 0U);
    const In::UInt8 oldKeys[]{0U, 0U, 4U, 5U, 6U, 7U, 8U, 9U};
    const In::UInt8 newKeys[]{0xFFU, 0U, 10U, 11U, 12U, 13U, 14U, 15U};
    static_cast<void>(hid.Keyboard(oldKeys, 8U, output));
    ASTERIA_EXPECT(context, "maximum keyboard diff is lossless",
                   hid.Keyboard(newKeys, 8U, output) == In::ReportResult::Accepted, output.Count == 20U);
    const auto seeds = Asteria::TestSupport::GetSeedRange(270U, 8U);
    for (In::Size seedAt = 0U; seedAt < seeds.Count; ++seedAt)
    {
        const auto seed = seeds.First + seedAt;
        Asteria::TestSupport::Random random{seed};
        bool previous[256U]{};
        hid.Reset();
        for (In::Size step = 0U; step < 2000U; ++step)
        {
            bool current[256U]{};
            report[0U] = static_cast<In::UInt8>(random.Below(256U));
            report[1U] = 0U;
            for (In::Size i = 2U; i < 8U; ++i)
            {
                report[i] = static_cast<In::UInt8>(random.Below(40U));
                if (report[i] < 4U)
                {
                    report[i] = 0U;
                }
                current[report[i]] = report[i] != 0U;
            }
            for (In::Size bit = 0U; bit < 8U; ++bit)
            {
                current[0xE0U + bit] = (report[0U] & (1U << bit)) != 0U;
            }
            const bool gone = random.Chance(1U, 20U);
            if (gone)
            {
                for (auto& byte : report)
                {
                    byte = 0U;
                }
                for (auto& held : current)
                {
                    held = false;
                }
            }
            const bool reject = !gone && random.Chance(1U, 10U);
            const bool rollover = random.Chance(1U, 2U);
            if (reject)
            {
                if (rollover)
                {
                    report[2U] = 1U;
                }
                else
                {
                    report[1U] = 1U;
                }
            }
            In::ReportResult result{In::ReportResult::Accepted};
            if (gone)
            {
                hid.Release(output);
            }
            else
            {
                result = hid.Keyboard(report, 8U, output);
            }
            bool good = result == (reject ? (rollover ? In::ReportResult::Rollover : In::ReportResult::Invalid)
                                          : In::ReportResult::Accepted);
            In::Size count = 0U;
            if (!reject)
            {
                for (In::Size key = 0U; key < 256U; ++key)
                {
                    if (current[key] != previous[key])
                    {
                        ++count;
                        In::Size occurrences = 0U;
                        for (In::Size i = 0U; i < output.Count; ++i)
                        {
                            if (output.Events[i].Usage == key)
                            {
                                ++occurrences;
                                good = good && (output.Events[i].Modifiers == report[0U]) &&
                                       (output.Events[i].Kind ==
                                        (current[key] ? In::EventKind::KeyDown : In::EventKind::KeyUp));
                            }
                        }
                        good = good && (occurrences == 1U);
                    }
                    previous[key] = current[key];
                }
            }
            if (!good || (output.Count != count))
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu reject=%u\n",
                                               static_cast<unsigned long long>(seed), step, reject ? 1U : 0U));
                ASTERIA_FAIL(context, "HID pressed-set diff model");
                break;
            }
        }
    }
    return context.Finish();
}
