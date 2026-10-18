// SPDX-License-Identifier: Apache-2.0
#include "User/Component/PointerRoute.hpp"

#include <cstdio>
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Component/DisplayPointer.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace F = Asteria::Foundation;
namespace In = Asteria::User::Input;
using Asteria::TestSupport::TestContext;
C::PointerRoute g_route{};
F::UInt64 g_model[64U][8U]{};

void Model(TestContext& test, const F::UInt64 seed)
{
    Asteria::TestSupport::Random random{seed};
    F::Size first{0U};
    F::Size count{0U};
    F::UInt64 epoch{4U};
    F::UInt64 sequence{0U};
    F::UInt8 buttons[C::InputMaximumSources]{};
    g_route.Configure(0U, 0U);
    const In::InputEvent release{.Kind = In::EventKind::Buttons,
                                 .Modifiers = 0U,
                                 .Usage = 0U,
                                 .Source = 0U,
                                 .Dx = 0,
                                 .Dy = 0,
                                 .Buttons = 0U,
                                 .Reserved = {},
                                 .Wheel = 0};
    for (F::UInt32 source{1U}; source <= C::InputMaximumSources; ++source)
    {
        g_route.Queue(release, source);
    }
    g_route.Configure(epoch, sequence);
    for (F::Size step{0U}; step < 800U; ++step)
    {
        const F::UInt64 operation = random.Below(10U);
        if (operation == 0U)
        {
            epoch = random.Below(3U) == 0U ? 0U : epoch + 1U;
            sequence = random.Below(2U) == 0U ? 0U : ~F::UInt64{0U} - 3U;
            first = 0U;
            count = 0U;
            g_route.Configure(epoch, sequence);
        }
        else if ((operation < 4U) && (count != 0U))
        {
            g_route.Take();
            first = (first + 1U) % 64U;
            --count;
        }
        else
        {
            // Source ids beyond InputMaximumSources are dropped, as are their buttons.
            const F::UInt32 source = static_cast<F::UInt32>(1U + random.Below(C::InputMaximumSources + 1U));
            In::InputEvent event{.Kind = In::EventKind::Motion,
                                 .Modifiers = 0U,
                                 .Usage = 0U,
                                 .Source = 99U,
                                 .Dx = 0,
                                 .Dy = 0,
                                 .Buttons = 0U,
                                 .Reserved = {},
                                 .Wheel = 0};
            if (operation == 4U)
            {
                event.Kind = In::EventKind::Buttons;
                event.Buttons = static_cast<F::UInt8>(random.Below(8U));
            }
            else if (operation == 5U)
            {
                event.Kind = In::EventKind::Wheel;
                event.Wheel = static_cast<F::Int32>(random.Below(7U)) - 3;
            }
            else if (operation == 6U)
            {
                event.Kind = In::EventKind::KeyDown;
                event.Usage = 4U;
            }
            else
            {
                event.Dx = static_cast<F::Int32>(random.Below(511U)) - 255;
                event.Dy = static_cast<F::Int32>(random.Below(511U)) - 255;
            }
            if (source > C::InputMaximumSources)
            {
                g_route.Queue(event, source);
            }
            else if (g_route.CanQueue(1U))
            {
                g_route.Queue(event, source);
                if (event.Kind == In::EventKind::Buttons)
                {
                    buttons[source - 1U] = event.Buttons;
                }
                if ((epoch != 0U) && (event.Kind != In::EventKind::KeyDown))
                {
                    F::UInt64* const words = g_model[(first + count++) % 64U];
                    words[0U] = epoch;
                    words[1U] = ++sequence;
                    words[2U] =
                        event.Kind == In::EventKind::Motion ? C::DisplayRelativeMotion : C::DisplayPointerSnapshot;
                    words[3U] = static_cast<F::UInt32>(event.Dx) |
                                (static_cast<F::UInt64>(static_cast<F::UInt32>(event.Dy)) << 32U);
                    words[4U] = 0U;
                    for (const auto held : buttons)
                    {
                        words[4U] |= held;
                    }
                    words[5U] = source;
                    words[6U] = static_cast<F::UInt32>(event.Wheel);
                    words[7U] = 0U;
                }
            }
        }
        const F::UInt64* const head = g_route.Peek();
        bool equal = (count == 0U) == (head == nullptr);
        for (F::Size index{0U}; equal && (head != nullptr) && (index < 8U); ++index)
        {
            equal = head[index] == g_model[first][index];
        }
        const bool allowed = (epoch == 0U) || ((count != 64U) && (sequence != ~F::UInt64{0U}));
        equal = equal && (g_route.CanQueue(1U) == allowed);
        if (!equal)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu operation=%llu\n",
                                           static_cast<unsigned long long>(seed), step,
                                           static_cast<unsigned long long>(operation)));
        }
        ASTERIA_EXPECT(test, "route queue agrees with model across credit retry, clear, rebind and exhaustion", equal);
    }
}
void Absolute(TestContext& test)
{
    C::DisplayPointer pointer{};
    pointer.Configure(130U, 81U, 9U);
    F::UInt64 words[8U]{8U, 1U, C::DisplayAbsoluteMotion, 0U, 7U, 2U, 0U, 0U};
    ASTERIA_EXPECT(test, "stale epoch changes nothing", !pointer.Apply(words), pointer.Sequence() == 0U,
                   pointer.Position().X == 65, pointer.Position().Y == 40);
    words[0U] = 9U;
    ASTERIA_EXPECT(test, "absolute minimum reaches first corner", pointer.Apply(words), pointer.Position().X == 0,
                   pointer.Position().Y == 0);
    words[1U] = 2U;
    words[3U] = 65535ULL | (65535ULL << 32U);
    ASTERIA_EXPECT(test, "absolute maximum reaches last corner", pointer.Apply(words), pointer.Position().X == 129,
                   pointer.Position().Y == 80);
    words[1U] = 3U;
    words[2U] = C::DisplayRelativeMotion;
    words[3U] = 0x80000000ULL | (0x7FFFFFFFULL << 32U);
    ASTERIA_EXPECT(test, "signed extreme deltas clamp without overflow", pointer.Apply(words),
                   pointer.Position().X == 0, pointer.Position().Y == 80);
    words[1U] = 4U;
    words[2U] = C::DisplayAbsoluteMotion;
    words[3U] = 65536U;
    ASTERIA_EXPECT(test, "out-of-range absolute coordinate refused atomically", !pointer.Apply(words),
                   pointer.Sequence() == 3U);
}
} // namespace
int main()
{
    TestContext test{"PointerRouteTests"};
    Absolute(test);
    const auto seeds = Asteria::TestSupport::GetSeedRange(281U, 8U);
    for (F::UInt64 seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        Model(test, seed);
    }
    return test.Finish();
}
