// SPDX-License-Identifier: Apache-2.0
#include "User/Input/KeyRepeat.hpp"

#include <cstdio>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
namespace I = User::Input;
using Foundation::UInt64;

struct Model
{
    Foundation::UInt16 Usage{0U};
    Foundation::UInt32 Source{0U};
    Foundation::UInt8 Modifiers[3U]{};
    UInt64 Deadline{0U};
};

constexpr bool Repeatable(const Foundation::UInt16 usage) noexcept
{
    constexpr Foundation::UInt16 excluded[]{0x39U, 0x47U, 0x53U, 0x82U, 0x83U, 0x84U};
    for (const auto key : excluded)
    {
        if (key == usage)
        {
            return false;
        }
    }
    return (usage >= 4U) && (usage <= 223U);
}

void Run(TestSupport::TestContext& test, const UInt64 seed)
{
    TestSupport::Random random{seed};
    const UInt64 delay = seed % 2U == 0U ? 500'000U : 200'000U;
    const Foundation::UInt32 rate = seed % 2U == 0U ? 30U : 50U;
    const UInt64 period = (1'000'000U + rate - 1U) / rate;
    I::KeyRepeat repeat{delay, rate};
    Model model{};
    UInt64 elapsed{0U};
    const UInt64 origin = ~UInt64{0U} - 250'000U;
    for (Foundation::Size step{0U}; step < 2000U; ++step)
    {
        const UInt64 operation = random.Below(7U);
        I::InputEvent input{};
        input.Source = static_cast<Foundation::UInt32>(random.Below(3U) + 1U);
        constexpr Foundation::UInt16 keys[]{4U,    5U,    40U,   41U,   42U,   0x39U, 0x47U,
                                            0x53U, 0x82U, 0x83U, 0x84U, 0xE0U, 0xE1U, 0xE2U,
                                            0xE3U, 0xE4U, 0xE5U, 0xE6U, 0xE7U, 0U,    0xFFFFU};
        input.Usage = keys[random.Below(sizeof(keys) / sizeof(keys[0U]))];
        input.Modifiers = static_cast<Foundation::UInt8>(random.Below(256U));
        if (operation <= 2U)
        {
            input.Kind = I::EventKind::Motion;
            if (operation != 2U)
            {
                input.Kind = operation == 0U ? I::EventKind::KeyDown : I::EventKind::KeyUp;
            }
            repeat.Feed(input, origin + elapsed);
            if (operation != 2U)
            {
                model.Modifiers[input.Source - 1U] = input.Modifiers;
                if (operation == 1U)
                {
                    if ((model.Source == input.Source) && (model.Usage == input.Usage))
                    {
                        model.Usage = 0U;
                    }
                }
                else if (Repeatable(input.Usage) && ((model.Usage != input.Usage) || (model.Source != input.Source)))
                {
                    model.Usage = input.Usage;
                    model.Source = input.Source;
                    model.Deadline = elapsed + delay;
                }
            }
        }
        else if (operation == 3U)
        {
            repeat.SourceGone(input.Source);
            model.Modifiers[input.Source - 1U] = 0U;
            if (input.Source == model.Source)
            {
                model.Usage = 0U;
            }
        }
        else if (operation == 4U)
        {
            repeat.Reset();
            model.Usage = 0U;
            for (auto& modifiers : model.Modifiers)
            {
                modifiers = 0U;
            }
        }
        else if (operation == 5U)
        {
            // The owner listens again after being busy: a due repeat waits.
            const UInt64 settle = random.Below(200'000U);
            repeat.Settle(origin + elapsed, settle);
            if ((model.Usage != 0U) && (elapsed >= model.Deadline))
            {
                model.Deadline = elapsed + settle;
            }
        }
        else
        {
            elapsed += random.Below(2'000'000U);
        }
        const UInt64 maximum = random.Below(600'000U);
        UInt64 wait = maximum;
        if (model.Usage != 0U)
        {
            wait = elapsed >= model.Deadline ? 0U : model.Deadline - elapsed;
            if (wait > maximum)
            {
                wait = maximum;
            }
        }
        bool equal = repeat.WaitMicroseconds(origin + elapsed, maximum) == wait;
        I::InputEvent output{.Kind = I::EventKind::Wheel,
                             .Modifiers = 0U,
                             .Usage = 999U,
                             .Source = 0U,
                             .Dx = 0,
                             .Dy = 0,
                             .Buttons = 0U,
                             .Reserved = {},
                             .Wheel = 0};
        UInt64 count{0U};
        if ((model.Usage != 0U) && (elapsed >= model.Deadline))
        {
            // Advance the independent timeline through each expired slot.
            do
            {
                ++count;
                model.Deadline += period;
            } while (model.Deadline <= elapsed);
            if (count > 8U)
            {
                count = 8U;
            }
        }
        equal = equal && (repeat.TakeDue(origin + elapsed, output) == count);
        equal = equal &&
                (count == 0U ? output.Usage == 999U
                             : (output.Usage == model.Usage && output.Source == model.Source &&
                                output.Modifiers == (model.Modifiers[0U] | model.Modifiers[1U] | model.Modifiers[2U]) &&
                                output.Kind == I::EventKind::KeyDown));
        if (!equal)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu operation=%llu\n",
                                           static_cast<unsigned long long>(seed), step,
                                           static_cast<unsigned long long>(operation)));
            ASTERIA_FAIL(test, "repeat timeline model including source loss and focus reset");
            return;
        }
    }
}

void Boundaries(TestSupport::TestContext& test)
{
    I::InputEvent event{.Kind = I::EventKind::KeyDown,
                        .Modifiers = 0U,
                        .Usage = 42U,
                        .Source = 1U,
                        .Dx = 0,
                        .Dy = 0,
                        .Buttons = 0U,
                        .Reserved = {},
                        .Wheel = 0};
    I::KeyRepeat repeat{};
    constexpr UInt64 start = ~UInt64{0U} - 400'000U;
    repeat.Feed(event, start);
    ASTERIA_EXPECT(test, "delay crosses unsigned time wrap exactly", repeat.TakeDue(start + 499'999U, event) == 0U,
                   repeat.WaitMicroseconds(start + 499'999U, 10'000U) == 1U,
                   repeat.TakeDue(start + 500'000U, event) == 1U);
    ASTERIA_EXPECT(test, "rate period rounds up and due batches are bounded",
                   repeat.TakeDue(start + 533'333U, event) == 0U, repeat.TakeDue(start + 533'334U, event) == 1U,
                   repeat.TakeDue(start + 5'000'000U, event) == 8U, repeat.TakeDue(start + 5'000'000U, event) == 0U);
    repeat.Settle(start + 5'100'000U, 100'000U);
    ASTERIA_EXPECT(test, "settling postpones a repeat that fell due while busy",
                   repeat.TakeDue(start + 5'199'999U, event) == 0U,
                   repeat.WaitMicroseconds(start + 5'150'000U, 80'000U) == 50'000U,
                   repeat.TakeDue(start + 5'200'000U, event) == 1U);
    repeat.Settle(start + 5'210'000U, 100'000U);
    ASTERIA_EXPECT(test, "settling leaves a repeat that is not yet due",
                   repeat.TakeDue(start + 5'233'333U, event) == 0U, repeat.TakeDue(start + 5'233'334U, event) == 1U);
    repeat.SourceGone(1U);
    ASTERIA_EXPECT(test, "device disappearance stops a missing release",
                   repeat.TakeDue(start + 6'000'000U, event) == 0U);
    repeat.Feed(event, 0U);
    I::InputEvent modifier = event;
    modifier.Source = 2U;
    modifier.Usage = 0xE1U;
    modifier.Modifiers = 2U;
    repeat.Feed(modifier, 1U);
    ASTERIA_EXPECT(test, "another keyboard's held Shift applies", repeat.TakeDue(500'000U, event) == 1U,
                   event.Modifiers == 2U);
    repeat.SourceGone(2U);
    ASTERIA_EXPECT(test, "modifier keyboard unplug leaves the winner with cleared Shift",
                   repeat.TakeDue(533'334U, event) == 1U, event.Modifiers == 0U);
    for (Foundation::UInt32 source{2U}; source <= 5U; ++source)
    {
        modifier.Source = source;
        repeat.Feed(modifier, 533'334U);
    }
    modifier.Source = 5U;
    modifier.Usage = 5U;
    repeat.Feed(modifier, 533'334U);
    ASTERIA_EXPECT(test, "source capacity refuses takeover", repeat.TakeDue(566'668U, event) == 1U, event.Source == 1U,
                   event.Usage == 42U);
    repeat.SourceGone(3U);
    repeat.Feed(modifier, 566'668U);
    ASTERIA_EXPECT(test, "source removal frees a slot", repeat.TakeDue(1'066'668U, event) == 1U, event.Source == 5U,
                   event.Usage == 5U);

    I::KeyRepeat fastest{0U, 2000U};
    fastest.Feed(event, 0U);
    ASTERIA_EXPECT(test, "configuration is bounded", fastest.TakeDue(0U, event) == 0U, fastest.TakeDue(1U, event) == 1U,
                   fastest.WaitMicroseconds(1U, 10'000U) == 1000U);
    I::KeyRepeat slowest{~UInt64{0U}, 0U};
    slowest.Feed(event, 0U);
    ASTERIA_EXPECT(test, "maximum delay stays in modular comparison range",
                   slowest.WaitMicroseconds(0U, ~UInt64{0U}) == (UInt64{1U} << 63U) - 1U,
                   slowest.TakeDue((UInt64{1U} << 63U) - 1U, event) == 1U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext test{"KeyRepeatTests"};
    const auto seeds = Asteria::TestSupport::GetSeedRange(700U, 32U);
    for (Asteria::Foundation::UInt64 index{0U}; index < seeds.Count; ++index)
    {
        Asteria::Test::Run(test, seeds.First + index);
    }
    Asteria::Test::Boundaries(test);
    return test.Finish();
}
