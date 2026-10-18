// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBrokerLogic.hpp"

#include <vector>
#include <cstdio>
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace In = Asteria::User::Input;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt16;
using Asteria::TestSupport::TestContext;
In::InputEvent Key(UInt16 usage, In::EventKind kind, In::UInt8 modifiers = 0U)
{
    return In::InputEvent{.Kind = kind,
                          .Modifiers = modifiers,
                          .Usage = usage,
                          .Source = 99U,
                          .Dx = 0,
                          .Dy = 0,
                          .Buttons = 0U,
                          .Reserved = {},
                          .Wheel = 0};
}
void Chord(TestContext& context)
{
    for (UInt16 ctrl : {UInt16{0xE0U}, UInt16{0xE4U}})
    {
        for (UInt16 alt : {UInt16{0xE2U}, UInt16{0xE6U}})
        {
            C::InputBrokerLogic broker{};
            ASTERIA_EXPECT(context, "bounded registration", broker.RegisterSource() == 1U,
                           broker.RegisterSource() == 2U, broker.RegisterSource() == 3U, broker.RegisterSource() == 4U,
                           broker.RegisterSource() == 5U, broker.RegisterSource() == 6U, broker.RegisterSource() == 0U,
                           broker.RegisterClient() == 1U, broker.RegisterClient() == 2U, broker.RegisterClient() == 3U,
                           broker.RegisterClient() == 0U, broker.SetFocus(1U) == C::InputAnswer::Done);
            const In::UInt8 c = static_cast<In::UInt8>(1U << (ctrl - 0xE0U));
            const In::UInt8 a = static_cast<In::UInt8>(1U << (alt - 0xE0U));
            const In::UInt8 both = static_cast<In::UInt8>(c | a);
            const In::InputEvent batch[]{
                Key(ctrl, In::EventKind::KeyDown, c),     Key(alt, In::EventKind::KeyDown, both),
                Key(0x4CU, In::EventKind::KeyDown, both), Key(0x4CU, In::EventKind::KeyUp, 0U),
                Key(alt, In::EventKind::KeyUp, c),        Key(ctrl, In::EventKind::KeyUp)};
            // Every split preserves suppression, including Delete released
            // after modifiers changed and repeats while Delete is held.
            ASTERIA_EXPECT(context, "chord presses consumed across batch boundaries",
                           broker.Publish(1U, batch, 2U) == C::InputAnswer::Done,
                           broker.Publish(1U, batch + 2U, 1U) == C::InputAnswer::Done,
                           broker.Publish(1U, batch + 2U, 1U) == C::InputAnswer::Done,
                           broker.Publish(1U, batch + 3U, 3U) == C::InputAnswer::Done, broker.Queued(1U) == 0U,
                           broker.Queued(2U) == 0U, broker.AttentionSource() == 1U);
            broker.TakeAttention();
            ASTERIA_EXPECT(context, "repeat did not report a second attention", broker.AttentionSource() == 0U);
            const In::InputEvent shortcut[]{Key(ctrl, In::EventKind::KeyDown, c), Key(4U, In::EventKind::KeyDown, c),
                                            Key(4U, In::EventKind::KeyUp, c), Key(ctrl, In::EventKind::KeyUp)};
            In::InputEvent events[8U]{};
            ASTERIA_EXPECT(context, "ordinary shortcuts preserve modifier edges and source stamp",
                           broker.Publish(2U, shortcut, 4U) == C::InputAnswer::Done, broker.Peek(1U, events, 8U) == 4U,
                           events[0U].Usage == ctrl, events[1U].Usage == 4U, events[1U].Modifiers == c,
                           events[0U].Source == 2U, events[3U].Source == 2U);
        }
    }
}
void Detach(TestContext& context)
{
    C::InputBrokerLogic broker{};
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterClient(true));
    static_cast<void>(broker.RegisterClient());
    static_cast<void>(broker.SetFocus(1U, true));
    const auto held = Key(4U, In::EventKind::KeyDown);
    ASTERIA_EXPECT(context, "queue a held key before receiver revocation",
                   broker.Publish(1U, &held, 1U) == C::InputAnswer::Done);
    broker.DetachClient(1U);
    ASTERIA_EXPECT(context, "revoked focus is retired and lost input counted", broker.Focus() == 0U,
                   broker.Queued(1U) == 0U, broker.Dropped() == 1U, !broker.BrokerPointer(1U),
                   broker.SetFocus(1U) == C::InputAnswer::Refused, broker.SetManagerFocus(2U) == C::InputAnswer::Done);
    const In::InputEvent batch[]{Key(4U, In::EventKind::KeyUp),
                                 {.Kind = In::EventKind::Motion,
                                  .Modifiers = 0U,
                                  .Usage = 0U,
                                  .Source = 1U,
                                  .Dx = 1,
                                  .Dy = 1,
                                  .Buttons = 0U,
                                  .Reserved = {},
                                  .Wheel = 0}};
    In::InputEvent answer[2U]{};
    ASTERIA_EXPECT(context, "dead consumer's release cannot leak to a new focus",
                   broker.Publish(1U, batch, 2U) == C::InputAnswer::Done, broker.Peek(2U, answer, 2U) == 1U,
                   answer[0U].Kind == In::EventKind::Motion);
    broker.DetachClient(1U);
    ASTERIA_EXPECT(context, "repeated detach preserves the living focus", broker.Focus() == 2U, broker.Dropped() == 1U);
    broker.Take(2U, 1U);
    const auto ctrl = Key(0xE0U, In::EventKind::KeyDown);
    ASTERIA_EXPECT(context, "defer a physical attention modifier before detach",
                   broker.Publish(1U, &ctrl, 1U) == C::InputAnswer::Done);
    broker.DetachClient(2U);
    const In::InputEvent attention[]{Key(0xE2U, In::EventKind::KeyDown), Key(0x4CU, In::EventKind::KeyDown)};
    ASTERIA_EXPECT(context, "secure attention survives the loss of focused input",
                   broker.Publish(1U, attention, 2U) == C::InputAnswer::Done, broker.AttentionSource() == 1U,
                   broker.Dropped() == 2U);
}
void RetireFullSession(TestContext& context)
{
    C::InputBrokerLogic broker{};
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterClient());
    static_cast<void>(broker.RegisterClient(true));
    static_cast<void>(broker.SetFocus(2U));
    const In::InputEvent pair[]{Key(4U, In::EventKind::KeyDown), Key(4U, In::EventKind::KeyUp)};
    for (Size i{0U}; i < 32U; ++i)
    {
        static_cast<void>(broker.Publish(1U, pair, 2U));
    }
    broker.DetachClient(2U);
    ASTERIA_EXPECT(context, "retiring a full abandoned session permits console focus", broker.Dropped() == 64U,
                   broker.SetFocus(1U, true) == C::InputAnswer::Done, broker.Queued(2U) == 0U);
}
void RebindHeld(TestContext& context)
{
    C::InputBrokerLogic broker{};
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterClient(true));
    static_cast<void>(broker.SetFocus(1U));
    const auto delivered = Key(4U, In::EventKind::KeyDown);
    const auto pending = Key(5U, In::EventKind::KeyDown);
    static_cast<void>(broker.Publish(1U, &delivered, 1U));
    broker.Take(1U, 1U);
    static_cast<void>(broker.Publish(1U, &pending, 1U));
    ASTERIA_EXPECT(context, "handoff retains only the held state whose make is still queued",
                   broker.RebindClient(1U) == C::InputAnswer::Done, broker.Delivered(1U) == 1U,
                   broker.Queued(1U) == 1U);
    const auto oldRelease = Key(4U, In::EventKind::KeyUp);
    const auto newRelease = Key(5U, In::EventKind::KeyUp);
    static_cast<void>(broker.Publish(1U, &oldRelease, 1U));
    static_cast<void>(broker.Publish(1U, &newRelease, 1U));
    In::InputEvent events[3U]{};
    ASTERIA_EXPECT(context, "old held release is suppressed and queued make remains balanced",
                   broker.Peek(1U, events, 3U) == 2U, events[0U].Usage == 5U, events[0U].Kind == In::EventKind::KeyDown,
                   events[1U].Usage == 5U, events[1U].Kind == In::EventKind::KeyUp);
    broker.Take(1U, 2U);
    static_cast<void>(broker.Publish(1U, &delivered, 1U));
    ASTERIA_EXPECT(context, "physical release ends the old incarnation's suppression",
                   broker.Peek(1U, events, 3U) == 1U, events[0U].Usage == 4U);
}
void CrossSource(TestContext& context)
{
    C::InputBrokerLogic broker{};
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterClient());
    static_cast<void>(broker.SetFocus(1U));
    const auto ctrl = Key(0xE4U, In::EventKind::KeyDown);
    const auto alt = Key(0xE2U, In::EventKind::KeyDown);
    const auto del = Key(0x4CU, In::EventKind::KeyDown);
    ASTERIA_EXPECT(
        context, "physical modifier edges across sources override a forged zero snapshot",
        broker.Publish(1U, &ctrl, 1U) == C::InputAnswer::Done, broker.Publish(2U, &alt, 1U) == C::InputAnswer::Done,
        broker.Publish(2U, &del, 1U) == C::InputAnswer::Done, broker.AttentionSource() == 2U, broker.Queued(1U) == 0U);
    broker.TakeAttention();
    auto release = Key(0x4CU, In::EventKind::KeyUp);
    static_cast<void>(broker.Publish(2U, &release, 1U));
    release = Key(0xE4U, In::EventKind::KeyUp);
    static_cast<void>(broker.Publish(1U, &release, 1U));
    release = Key(0xE2U, In::EventKind::KeyUp);
    ASTERIA_EXPECT(context, "all cross-source chord releases consumed",
                   broker.Publish(2U, &release, 1U) == C::InputAnswer::Done, broker.Queued(1U) == 0U);
}
void Atomicity(TestContext& context)
{
    C::InputBrokerLogic broker{};
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterClient());
    static_cast<void>(broker.RegisterClient());
    static_cast<void>(broker.SetFocus(1U));
    In::InputEvent events[2U]{Key(4U, In::EventKind::KeyDown), Key(5U, In::EventKind::KeyDown)};
    events[1U].Reserved[1U] = 1U;
    ASTERIA_EXPECT(context, "malformed batch rejects without accepting its prefix",
                   broker.Publish(1U, events, 2U) == C::InputAnswer::Refused, broker.Queued(1U) == 0U,
                   broker.Publish(1U, events, 1U) == C::InputAnswer::Done, broker.Queued(1U) == 1U);
    In::InputEvent motion{.Kind = In::EventKind::Motion,
                          .Modifiers = 0U,
                          .Usage = 0U,
                          .Source = 0U,
                          .Dx = 1,
                          .Dy = 0,
                          .Buttons = 0U,
                          .Reserved = {},
                          .Wheel = 0};
    for (Size index{1U}; index < C::InputBrokerLogic::QueueCapacity; ++index)
    {
        static_cast<void>(broker.Publish(1U, &motion, 1U));
    }
    ASTERIA_EXPECT(context, "full queue refuses input and focus change atomically",
                   broker.Publish(1U, &motion, 1U) == C::InputAnswer::Exhausted,
                   broker.SetFocus(2U) == C::InputAnswer::Exhausted, broker.Focus() == 1U);
    broker.Take(1U, 64U);
    ASTERIA_EXPECT(context, "held release retained until focus switch has capacity",
                   broker.SetFocus(2U) == C::InputAnswer::Done, broker.Peek(1U, events, 2U) == 1U,
                   events[0U].Kind == In::EventKind::KeyUp, events[0U].Source == 1U, events[0U].Usage == 4U);
}
// Registering a source again releases what its dead driver held, refuses an
// unknown source and changes nothing when a client's queue cannot take the
// releases.
void ResetSource(TestContext& context)
{
    C::InputBrokerLogic broker{};
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterClient());
    static_cast<void>(broker.SetFocus(1U));
    const auto down = Key(4U, In::EventKind::KeyDown);
    const auto other = Key(5U, In::EventKind::KeyDown);
    In::InputEvent events[4U]{};
    ASTERIA_EXPECT(context, "keys held from two sources", broker.Publish(1U, &down, 1U) == C::InputAnswer::Done,
                   broker.Publish(2U, &other, 1U) == C::InputAnswer::Done, broker.Peek(1U, events, 4U) == 2U);
    broker.Take(1U, 2U);
    ASTERIA_EXPECT(context, "an unregistered source is refused", broker.ResetSource(0U) == C::InputAnswer::Refused,
                   broker.ResetSource(3U) == C::InputAnswer::Refused, broker.Queued(1U) == 0U);
    ASTERIA_EXPECT(context, "only the reset source's key is released, with zero modifiers",
                   broker.ResetSource(1U) == C::InputAnswer::Done, broker.Peek(1U, events, 4U) == 1U,
                   events[0U].Kind == In::EventKind::KeyUp, events[0U].Usage == 4U, events[0U].Source == 1U,
                   events[0U].Modifiers == 0U);
    broker.Take(1U, 1U);
    ASTERIA_EXPECT(context, "the restarted driver's first make is delivered again",
                   broker.Publish(1U, &down, 1U) == C::InputAnswer::Done, broker.Queued(1U) == 1U);
    broker.Take(1U, 1U);
    In::InputEvent motion{.Kind = In::EventKind::Motion,
                          .Modifiers = 0U,
                          .Usage = 0U,
                          .Source = 0U,
                          .Dx = 1,
                          .Dy = 0,
                          .Buttons = 0U,
                          .Reserved = {},
                          .Wheel = 0};
    for (Size index{0U}; index < C::InputBrokerLogic::QueueCapacity; ++index)
    {
        static_cast<void>(broker.Publish(2U, &motion, 1U));
    }
    ASTERIA_EXPECT(context, "a full queue refuses the releases atomically",
                   broker.ResetSource(1U) == C::InputAnswer::Exhausted,
                   broker.Queued(1U) == C::InputBrokerLogic::QueueCapacity);
    broker.Take(1U, C::InputBrokerLogic::QueueCapacity);
    ASTERIA_EXPECT(context, "the held key survived the refused reset", broker.ResetSource(1U) == C::InputAnswer::Done,
                   broker.Peek(1U, events, 4U) == 1U, events[0U].Kind == In::EventKind::KeyUp, events[0U].Usage == 4U);
}
void ResetPointer(TestContext& context)
{
    C::InputBrokerLogic broker{};
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterSource());
    static_cast<void>(broker.RegisterClient(true));
    static_cast<void>(broker.SetFocus(1U));
    In::InputEvent button{};
    button.Kind = In::EventKind::Buttons;
    button.Buttons = 1U;
    const auto key = Key(4U, In::EventKind::KeyDown);
    ASTERIA_EXPECT(context, "source holds a key and button in the routed seat",
                   broker.Publish(1U, &key, 1U, 7U, true) == C::InputAnswer::Done,
                   broker.Publish(1U, &button, 1U, 7U, true) == C::InputAnswer::Done);
    button.Buttons = 2U;
    static_cast<void>(broker.Publish(2U, &button, 1U, 8U, true));
    ASTERIA_EXPECT(context, "reset releases its button after its key with the cumulative stamp",
                   broker.ResetSource(1U, 9U, true) == C::InputAnswer::Done, broker.SourceButtons(1U) == 0U,
                   broker.SourceButtons(2U) == 2U, broker.Buttons() == 2U, broker.PointerStamp(0U) == 2U);
    ASTERIA_EXPECT(context, "an exhausted sequence does not clear a held button",
                   broker.ResetSource(2U, ~Asteria::Foundation::UInt64{0U}, true) == C::InputAnswer::Exhausted,
                   broker.SourceButtons(2U) == 2U);
}
void Model(TestContext& context)
{
    const auto seeds = Asteria::TestSupport::GetSeedRange(2300U, 24U);
    for (auto seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::TestSupport::Random random{seed};
        C::InputBrokerLogic broker{};
        static_cast<void>(broker.RegisterSource());
        static_cast<void>(broker.RegisterSource());
        static_cast<void>(broker.RegisterSource());
        static_cast<void>(broker.RegisterClient());
        static_cast<void>(broker.RegisterClient());
        constexpr UInt32 Sources{3U};
        bool pressed[Sources][8U]{};
        bool seen[2U][Sources][8U]{};
        std::vector<In::InputEvent> queues[2U];
        UInt32 focus{0U};
        for (Size step{0U}; step < 1200U; ++step)
        {
            const UInt32 operation = static_cast<UInt32>(random.Below(6U));
            if (operation == 5U)
            {
                // A restarted driver's source registered again: every key a
                // client saw from it is released there, and its next make
                // is a make again.
                const UInt32 source = static_cast<UInt32>(random.Below(Sources));
                for (UInt32 client{0U}; client < 2U; ++client)
                {
                    for (UInt16 key{0U}; key < 8U; ++key)
                    {
                        if (seen[client][source][key])
                        {
                            auto event = Key(static_cast<UInt16>(4U + key), In::EventKind::KeyUp);
                            event.Source = source + 1U;
                            queues[client].push_back(event);
                            seen[client][source][key] = false;
                        }
                    }
                }
                for (bool& key : pressed[source])
                {
                    key = false;
                }
                ASTERIA_EXPECT(context, "registration again accepted",
                               broker.ResetSource(source + 1U) == C::InputAnswer::Done);
            }
            else if (operation == 0U)
            {
                const UInt32 next = static_cast<UInt32>(random.Below(3U));
                if ((next != focus) && (focus != 0U))
                {
                    for (UInt32 source{0U}; source < Sources; ++source)
                    {
                        for (UInt16 key{0U}; key < 8U; ++key)
                        {
                            if (seen[focus - 1U][source][key])
                            {
                                auto event = Key(static_cast<UInt16>(4U + key), In::EventKind::KeyUp);
                                event.Source = source + 1U;
                                queues[focus - 1U].push_back(event);
                                seen[focus - 1U][source][key] = false;
                            }
                        }
                    }
                }
                static_cast<void>(broker.SetFocus(next));
                focus = next;
            }
            else
            {
                const UInt32 source = static_cast<UInt32>(random.Below(Sources));
                const UInt16 key = static_cast<UInt16>(random.Below(8U));
                const bool down = random.Below(2U) == 0U;
                auto event = Key(static_cast<UInt16>(4U + key), down ? In::EventKind::KeyDown : In::EventKind::KeyUp);
                if (pressed[source][key] != down)
                {
                    pressed[source][key] = down;
                    if ((focus != 0U) && (down || seen[focus - 1U][source][key]))
                    {
                        seen[focus - 1U][source][key] = down;
                        event.Source = source + 1U;
                        queues[focus - 1U].push_back(event);
                    }
                }
                ASTERIA_EXPECT(context, "model operation accepted",
                               broker.Publish(source + 1U, &event, 1U) == C::InputAnswer::Done);
            }
            bool same = broker.Focus() == focus;
            for (UInt32 client{0U}; client < 2U; ++client)
            {
                In::InputEvent actual[32U]{};
                const Size count = broker.Peek(client + 1U, actual, 32U);
                same = same && (count == queues[client].size());
                // Releases on focus loss may be in any order. Compare the
                // multiset, while single input operations remain exact.
                for (Size index{0U}; index < count; ++index)
                {
                    bool found{false};
                    for (auto it = queues[client].begin(); it != queues[client].end(); ++it)
                    {
                        if ((it->Kind == actual[index].Kind) && (it->Usage == actual[index].Usage) &&
                            (it->Source == actual[index].Source))
                        {
                            queues[client].erase(it);
                            found = true;
                            break;
                        }
                    }
                    same = same && found;
                }
                broker.Take(client + 1U, count);
            }
            if (!same)
            {
                std::fprintf(stderr, "seed=%llu step=%zu operation=%u\n", static_cast<unsigned long long>(seed), step,
                             operation);
            }
            ASTERIA_EXPECT(context, "reference focus, pressed sets, queue and source model", same);
        }
    }
}
} // namespace
int main()
{
    for (UInt16 usage{66U}; usage <= 68U; ++usage)
    {
        C::InputBrokerLogic broker{};
        static_cast<void>(broker.RegisterSource());
        static_cast<void>(broker.RegisterClient());
        static_cast<void>(broker.RegisterClient(true));
        static_cast<void>(broker.SetFocus(2U));
        const In::InputEvent batch[]{Key(0xE0U, In::EventKind::KeyDown, 1U), Key(0xE2U, In::EventKind::KeyDown, 5U),
                                     Key(usage, In::EventKind::KeyDown, 5U), Key(usage, In::EventKind::KeyUp),
                                     Key(0xE2U, In::EventKind::KeyUp),       Key(0xE0U, In::EventKind::KeyUp)};
        Asteria::TestSupport::TestContext power{"Root power shortcuts"};
        ASTERIA_EXPECT(power, "root receives reserved chord, client receives none",
                       broker.Publish(1U, batch, 6U) == C::InputAnswer::Done, broker.AttentionSource() == 1U,
                       broker.AttentionUsage() == usage, broker.Queued(1U) == 0U, broker.Queued(2U) == 0U);
        if (power.Finish() != 0)
        {
            return 1;
        }
    }

    TestContext context{"InputBrokerLogicTests"};
    Chord(context);
    Atomicity(context);
    Detach(context);
    RetireFullSession(context);
    RebindHeld(context);
    CrossSource(context);
    ResetSource(context);
    ResetPointer(context);
    Model(context);
    return context.Finish();
}
