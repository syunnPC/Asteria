// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBrokerLogic.hpp"
#include "User/Component/PointerRoute.hpp"
#include "User/Session/InputOrder.hpp"
#include "User/Session/WindowManager.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace C = User::Component;
namespace S = User::Session;
namespace I = User::Input;
using Foundation::Size;
using Foundation::UInt64;
using TestSupport::TestContext;
C::InputBrokerLogic broker;
C::PointerRoute route;
S::InputOrder order;
S::Scene scene;
S::ClientEvents clients[S::SessionMaximumClients];
S::WindowManager wm;
I::InputEvent keys[C::InputMaximumEvents];
S::SessionEvent answer[S::SessionMaximumEvents];
[[nodiscard]] I::InputEvent Event(const I::EventKind kind, const Foundation::UInt16 usage = 0U,
                                  const Foundation::UInt8 modifiers = 0U, const Foundation::UInt8 buttons = 0U)
{
    return {kind, modifiers, usage, 0U, 0, 0, buttons, {}, 0};
}
void Clear(const Size client, const UInt64 now)
{
    for (Size n{0U}; n < 32U; ++n)
    {
        if (!clients[client].Credit(S::SessionMaximumEvents, now))
        {
            return;
        }
        if (clients[client].Build(answer, now) == 0U)
        {
            clients[client].SendFailed(false, now);
            return;
        }
        clients[client].Commit(now);
    }
}
void Configure(TestContext& t)
{
    broker = {};
    route = {};
    order = {};
    ASTERIA_EXPECT(t, "seat enrolls once and legacy client remains available", broker.RegisterSource() == 1U,
                   broker.RegisterClient(true) == 1U, broker.RegisterClient(true) == 0U, broker.RegisterClient() == 2U,
                   broker.SetFocus(1U) == C::InputAnswer::Done);
    route.Configure(9U, 0U);
    order.Configure(7U, 9U);
    scene.Configure(256U, 112U, User::Graphics::PixelFormat::Rgbx);
    for (auto& client : clients)
    {
        client.Reset();
    }
    wm.Configure(scene, clients);
    ASTERIA_EXPECT(t, "two independent window clients", wm.Map(0U, 64U, 24U, 0U, 0U), wm.Map(4U, 64U, 16U, 0U, 0U),
                   wm.Focused() == 0U);
    Clear(0U, 400000U);
    Clear(1U, 400000U);
}
void Merge(TestContext& t, const bool clickFirst, const bool pointerArrivesFirst)
{
    Configure(t);
    const auto key = Event(I::EventKind::KeyDown, 4U);
    const auto pointer = Event(I::EventKind::Buttons, 0U, 0U, 1U);
    const I::InputEvent publish[2U]{clickFirst ? pointer : key, clickFirst ? key : pointer};
    ASTERIA_EXPECT(t, "mixed Publish commits atomically",
                   broker.Publish(1U, publish, 2U, 0U, true) == C::InputAnswer::Done);
    const Size pi = clickFirst ? 0U : 1U;
    route.Queue(publish[pi], 1U, broker.PointerStamp(pi));
    ASTERIA_EXPECT(t, "one key and no duplicate pointer", broker.Queued(1U) == 1U,
                   broker.Peek(1U, keys, C::InputMaximumEvents) == 1U, keys[0U].Kind == I::EventKind::KeyDown,
                   route.Peek() != nullptr, route.Peek()[7U] == (clickFirst ? 0U : 1U),
                   broker.Horizon(1U) == (clickFirst ? 1U : 0U));
    const auto addKey = [&]()
    {
        return order.Keys(broker.Horizon(1U), 1U, 0U, 400001U);
    };
    const auto addPointer = [&]()
    {
        return order.Pointer(route.Peek()[1U], route.Peek()[7U], 0U, 400001U);
    };
    ASTERIA_EXPECT(t, "first stream accepted", pointerArrivesFirst ? addPointer() : addKey());
    // The later broker action must wait even if its IPC arrived first.
    if (pointerArrivesFirst != clickFirst)
    {
        ASTERIA_EXPECT(t, "later action waits for opposite stream",
                       order.Take(400002U).Type == S::InputOrder::Kind::None);
    }
    ASTERIA_EXPECT(t, "second stream accepted", pointerArrivesFirst ? addKey() : addPointer());
    for (Size n{0U}; n < 2U; ++n)
    {
        const auto item = order.Take(400003U);
        ASTERIA_EXPECT(t, "broker order independent of IPC arrival",
                       item.Type ==
                           ((n == 0U) == clickFirst ? S::InputOrder::Kind::Pointer : S::InputOrder::Kind::Keys));
        if (item.Type == S::InputOrder::Kind::Pointer)
        {
            wm.Pointer({70, 86}, 1U, 0, 400003U);
        }
        if (item.Type == S::InputOrder::Kind::Keys)
        {
            static_cast<void>(wm.Key(keys[0U].Usage, 0U, true, 400003U));
        }
    }
    bool delivered[2U]{};
    bool released[2U]{};
    for (Size c{0U}; c < 2U; ++c)
    {
        ASTERIA_EXPECT(t, "clients retain independent credits", clients[c].Credit(S::SessionMaximumEvents, 400004U));
        const Size count = clients[c].Build(answer, 400004U);
        for (Size n{0U}; n < count; ++n)
        {
            if (answer[n].Kind != S::SessionEventKind::Key || answer[n].Words[2U] != 4U)
            {
                continue;
            }
            (answer[n].Words[1U] == S::SessionKeyDown ? delivered[c] : released[c]) = true;
        }
        if (count != 0U)
        {
            clients[c].Commit(400004U);
        }
        else
        {
            clients[c].SendFailed(false, 400004U);
        }
    }
    ASTERIA_EXPECT(t, "key has exactly its broker-time recipient", delivered[clickFirst ? 1U : 0U],
                   !delivered[clickFirst ? 0U : 1U], wm.Focused() == 4U);
    if (!clickFirst)
    {
        ASTERIA_EXPECT(t, "earlier make balanced on old focus", released[0U], !released[1U]);
    }
    static_cast<void>(wm.Key(4U, 0U, false, 400005U));
    if (!clickFirst)
    {
        ASTERIA_EXPECT(t, "physical release cannot leak to new focus", clients[1U].Queued() == 0U);
    }
}
void Boundaries(TestContext& t)
{
    Configure(t);
    const I::InputEvent mixed[4U]{Event(I::EventKind::KeyDown, 0xE2U, 4U), Event(I::EventKind::KeyDown, 4U, 4U),
                                  Event(I::EventKind::Buttons, 0U, 0U, 1U), Event(I::EventKind::KeyDown, 5U, 4U)};
    ASTERIA_EXPECT(t, "deferred modifier counted when flushed before pointer",
                   broker.Publish(1U, mixed, 4U, 0U, true) == C::InputAnswer::Done, broker.PointerStamp(2U) == 2U,
                   broker.Peek(1U, keys, 8U) == 2U, broker.Horizon(1U) == 0U);
    broker.Take(1U, 2U);
    ASTERIA_EXPECT(t, "batch split at pointer horizon", broker.Peek(1U, keys, 8U) == 1U, broker.Horizon(1U) == 1U);
    broker.SetRouteSequence(80U);
    ASTERIA_EXPECT(t, "root lock rejects manager focus and leaves queue unchanged",
                   broker.SetFocus(1U, true) == C::InputAnswer::Done,
                   broker.SetManagerFocus(2U) == C::InputAnswer::Refused, broker.Focus() == 1U,
                   broker.Queued(1U) == 1U);
    ASTERIA_EXPECT(t, "root unlock synthesizes all releases at current route horizon",
                   broker.SetFocus(2U) == C::InputAnswer::Done);
    broker.Take(1U, 1U);
    ASTERIA_EXPECT(t, "synthesized releases keep their horizon", broker.Horizon(1U) == 80U,
                   broker.Peek(1U, keys, 8U) == 3U);
    const I::InputEvent legacy[3U]{Event(I::EventKind::Buttons, 0U, 0U, 1U), Event(I::EventKind::Buttons),
                                   Event(I::EventKind::KeyDown, 6U)};
    ASTERIA_EXPECT(t, "console keeps mixed credit batches across pointer horizons",
                   broker.Publish(1U, legacy, 3U, 80U, true) == C::InputAnswer::Done, broker.Horizon(2U) == 0U,
                   broker.Peek(2U, keys, 8U) == 3U, keys[0U].Kind == I::EventKind::Buttons, keys[2U].Usage == 6U);
    ASTERIA_EXPECT(t, "missing pointer never applies waiting key to old focus", order.Keys(10U, 1U, 0U, 1U),
                   order.Take(4999999U).Type == S::InputOrder::Kind::None,
                   order.Take(5000001U).Type == S::InputOrder::Kind::None, order.Dropped() == 1U,
                   !order.Pointer(10U, 0U, 0U, 5000002U));
    // One client's stopped credits overflow its queue; WM shortcuts still run.
    wm.Focus(4U, 6000000U);
    Clear(0U, 6000000U);
    Clear(1U, 6000000U);
    for (Size n{0U}; n < 48U; ++n)
    {
        static_cast<void>(wm.Key(20U, 0U, true, 6000001U + n));
        static_cast<void>(wm.Key(20U, 0U, false, 6000001U + n));
    }
    ASTERIA_EXPECT(t, "hung client's input is counted and marked", clients[1U].Dropped() >= 65U,
                   clients[1U].NotResponding());
    ASTERIA_EXPECT(t, "AltTab works with hung client", wm.Key(0x2BU, 4U, true, 6000100U),
                   wm.Key(0x2BU, 4U, false, 6000101U), wm.Key(0xE2U, 0U, false, 6000102U), wm.Focused() == 0U);
    static_cast<void>(wm.Key(6U, 0U, true, 6000103U));
    ASTERIA_EXPECT(t, "healthy client independently receives keys", clients[0U].Credit(7U, 6000104U));
    const auto count = clients[0U].Build(answer, 6000104U);
    bool healthy{false};
    for (Size n{0U}; n < count; ++n)
    {
        healthy |= answer[n].Kind == S::SessionEventKind::Key && answer[n].Words[2U] == 6U;
    }
    ASTERIA_EXPECT(t, "healthy client queue unaffected", healthy);
}
} // namespace
int main()
{
    TestContext test{"SeatOrderTests"};
    for (Size click{0U}; click < 2U; ++click)
    {
        for (Size arrival{0U}; arrival < 2U; ++arrival)
        {
            Merge(test, click != 0U, arrival != 0U);
        }
    }
    Boundaries(test);
    return test.Finish();
}
