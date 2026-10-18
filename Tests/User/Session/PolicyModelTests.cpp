// SPDX-License-Identifier: Apache-2.0
#include "User/Session/AddressPlan.hpp"
#include "User/Session/ClientEvents.hpp"
#include "User/Session/InputOrder.hpp"
#include "User/Session/WindowManager.hpp"
#include "User/Session/Palette.hpp"
#include "User/Console/TextConsole.hpp"
#include "Support/TestContext.hpp"
#include "Support/Random.hpp"
#include <cstdio>
#include <cstring>
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace S = User::Session;
using TestSupport::TestContext;
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt32;
using Foundation::UInt8;
S::Scene scene;
S::ClientEvents clients[4U];
S::ClientEvents events;
S::WindowManager manager;
S::AddressPlan address;
S::InputOrder order;
S::SessionEvent batch[7U];
UInt32 outputPixels[128U * 96U];
UInt32 referencePixels[128U * 96U];
UInt32 imagePixels[16U][64U * 64U];
void Budgets(TestContext& t)
{
    ASTERIA_EXPECT(t, "region base alignment and overflow", S::IsAddressRegionBase(1ULL << 30U),
                   !S::IsAddressRegionBase(1U), !S::IsAddressRegionBase(0xFFFF'FFFF'FFFF'FFFFULL));
    address.Configure(256U, 7U, 16U, false);
    ASTERIA_EXPECT(t, "output maps first", address.Map(4U, 1U, 3600U, 8U), address.Tables() == 16U);
    for (Size slot{0U}; slot < 4U; ++slot)
    {
        for (UInt32 pool{1U}; pool <= 2U; ++pool)
        {
            ASTERIA_EXPECT(t, "two-image pool", address.Map(slot, pool, 3600U, 8U));
        }
    }
    ASTERIA_EXPECT(t, "address plan uses 80 tables", address.Tables() == 80U, address.Records() == 88U);
    for (Size slot{0U}; slot < 4U; ++slot)
    {
        for (UInt32 pool{1U}; pool <= 2U; ++pool)
        {
            ASTERIA_EXPECT(t, "slot reuse never frees tables", address.Unmap(slot, pool),
                           address.Map(slot, pool, 3600U, 8U), address.Tables() == 80U);
        }
    }
    address.Configure(64U, 7U, 16U, false);
    ASTERIA_EXPECT(t, "old arena refuses worst case", address.Map(4U, 1U, 8192U, 16U), address.Map(0U, 1U, 8192U, 16U),
                   address.Map(0U, 2U, 8192U, 16U), !address.Map(1U, 1U, 8192U, 16U), address.Tables() == 56U);
    address.Configure(256U, 7U, 16U, false);
    ASTERIA_EXPECT(t, "worst-case output", address.Map(4U, 1U, 8192U, 16U));
    for (Size slot{0U}; slot < 4U; ++slot)
    {
        for (UInt32 pool{1U}; pool <= 2U; ++pool)
        {
            ASTERIA_EXPECT(t, "worst-case pools", address.Map(slot, pool, 8192U, 16U));
        }
    }
    ASTERIA_EXPECT(t, "worst-case tables and live records", address.Tables() == 152U, address.Records() == 160U);
    address.Configure(256U, 7U, 16U, true);
    for (Size i{0U}; i < 7U; ++i)
    {
        ASTERIA_EXPECT(t, "IA64 lifetime ledger appends maps and unmaps", address.Map(0U, 1U, 8192U, 16U),
                       address.Unmap(0U, 1U));
    }
    ASTERIA_EXPECT(t, "ledger refuses atomically", address.Map(0U, 1U, 8192U, 16U), !address.Unmap(0U, 1U),
                   address.Records() == 256U);
    const auto tables = address.Tables();
    const auto records = address.Records();
    ASTERIA_EXPECT(t, "all address validation bounds", !address.Map(5U, 1U, 1U, 1U), !address.Map(0U, 0U, 1U, 1U),
                   !address.Map(0U, 3U, 1U, 1U), !address.Map(4U, 2U, 1U, 1U), !address.Map(0U, 1U, 0U, 1U),
                   !address.Map(0U, 1U, 8193U, 16U), !address.Map(0U, 1U, 513U, 1U), !address.Map(0U, 1U, 1U, 2U),
                   !address.Map(0U, 1U, 1U, 0U), !address.Map(0U, 1U, 17U, 17U), address.Tables() == tables,
                   address.Records() == records);
}
void Delivery(TestContext& t)
{
    using K = S::SessionEventKind;
    events.Reset();
    ASTERIA_EXPECT(t, "input and state queued", events.Queue({K::Key, {1U, 1U, 4U, 0U}}, 1U),
                   events.Queue({K::Arrange, {1U, 9U, S::PackSessionPair(80U, 25U), 0U}}, 1U), events.Credit(7U, 1U),
                   !events.Credit(7U, 1U));
    ASTERIA_EXPECT(t, "state delivered before keys", events.Build(batch, 1U) == 2U, batch[0U].Kind == K::Arrange,
                   batch[1U].Kind == K::Key);
    events.SendFailed(true, 1U);
    ASTERIA_EXPECT(t, "blocked answer retains credit and snapshot", events.Build(batch, 10000U) == 0U,
                   events.Build(batch, 10001U) == 2U);
    events.Commit(10001U);
    for (Size i{0U}; i < 64U; ++i)
    {
        ASTERIA_EXPECT(t, "wheel fills queue", events.Queue({K::Wheel, {1U, 1U, 0U, 0U}}, 20000U));
    }
    ASTERIA_EXPECT(t, "overflow discards incoming and queue, retains release",
                   events.Queue({K::Key, {1U, 1U, 5U, 0U}}, 20001U), events.Dropped() == 65U, events.NotResponding(),
                   events.Queued() == 1U, events.Credit(7U, 20002U), events.Build(batch, 20002U) == 1U,
                   batch[0U].Kind == K::Key, batch[0U].Words[2U] == 4U, batch[0U].Words[1U] == 2U);
    events.Commit(20002U);
    ASTERIA_EXPECT(t, "credit and empty queue clear mark", !events.NotResponding(), events.Dropped() == 0U);
    ASTERIA_EXPECT(t, "latest arrange and returned image union",
                   events.Queue({K::Arrange, {1U, 10U, S::PackSessionPair(90U, 30U), 0U}}, 1U),
                   events.Queue({K::Arrange, {1U, 11U, S::PackSessionPair(100U, 30U), 0U}}, 1U),
                   events.Queue({K::ImageReturned, {1U, 0U, 0U, 0U}}, 1U),
                   events.Queue({K::ImageReturned, {2U, 0U, 0U, 0U}}, 1U), events.Credit(1U, 1U),
                   events.Build(batch, 1U) == 1U, batch[0U].Words[0U] == 3U, events.MorePending());
    ASTERIA_EXPECT(t, "new state during retry retained", events.Queue({K::ImageReturned, {4U, 0U, 0U, 0U}}, 1U));
    events.Commit(1U);
    ASTERIA_EXPECT(t, "only committed bitmap removed", events.Credit(7U, 2U), events.Build(batch, 2U) == 2U,
                   batch[0U].Words[0U] == 4U, batch[1U].Words[1U] == 11U);
    events.Commit(2U);
    ASTERIA_EXPECT(t, "close starts timer", events.Queue({K::CloseAsked, {1U, 0U, 0U, 0U}}, 10U));
    events.Tick(5000010U);
    ASTERIA_EXPECT(t, "close timeout", events.NotResponding());
    events.DestroySurface(1U);
    ASTERIA_EXPECT(t, "fresh credit clears empty input mark", events.Credit(7U, 5000011U));
    events.Commit(5000011U);
    events.Reset();
    ASTERIA_EXPECT(t, "motion merges only adjacent same-surface motion",
                   events.Queue({K::PointerMotion, {1U, 1U, 0U, 0U}}, 1U),
                   events.Queue({K::PointerMotion, {1U, 2U, 0U, 0U}}, 1U), events.Queued() == 1U,
                   events.Queue({K::PointerButtons, {1U, 2U, 1U, 0U}}, 1U),
                   events.Queue({K::PointerMotion, {1U, 3U, 1U, 0U}}, 1U), events.Queued() == 3U);
    events.Tick(5000001U);
    ASTERIA_EXPECT(t, "no credit timeout", events.NotResponding());
}
void Management(TestContext& t)
{
    scene.Configure(800U, 600U, User::Graphics::PixelFormat::Rgbx);
    for (auto& c : clients)
    {
        c.Reset();
    }
    manager.Configure(scene, clients);
    manager.UserLaunch(9U);
    ASTERIA_EXPECT(t, "cascade and first focus", manager.Map(0U, 100U, 80U, 0U, 0U), manager.Focused() == 0U,
                   scene.Frame(0U).X == 32, manager.Map(4U, 100U, 80U, 8U, 0U), scene.Frame(4U).X == 56,
                   manager.Focused() == 0U, scene.Look(4U).State == S::TitleState::Attention);
    manager.Pointer({65, 90}, 1U, 100, 1U);
    ASTERIA_EXPECT(t, "system map ignores presses", manager.Focused() == 0U);
    manager.Pointer({65, 90}, 0U, 0, 2U);
    manager.Pointer({65, 90}, 1U, 0, 300001U);
    ASTERIA_EXPECT(t, "click focuses and raises", manager.Focused() == 4U, scene.At(scene.Count() - 1U) == 4U);
    manager.Pointer({65, 90}, 0U, 0, 300002U);
    manager.Pointer({70, 65}, 1U, 0, 400000U);
    manager.Pointer({-2000, -2000}, 1U, 0, 400001U);
    manager.Pointer({-2000, -2000}, 0U, 0, 400002U);
    ASTERIA_EXPECT(t, "drag retains title", scene.Frame(4U).X == -76, scene.Frame(4U).Y == -4);
    manager.Raise(0U, false, 500000U);
    manager.Pointer({45, 65}, 1U, 0, 500001U);
    ASTERIA_EXPECT(t, "system raise press ignore", manager.Focused() == 4U);
    manager.Pointer({45, 65}, 0U, 0, 500002U);
    ASTERIA_EXPECT(t, "AltTab consumed", manager.Key(0x2BU, 4U, true, 900000U), manager.Key(0x2BU, 4U, false, 900001U),
                   manager.Key(0xE2U, 0U, false, 900002U), manager.Focused() == 0U);
    ASTERIA_EXPECT(t, "shortcuts and releases consumed", manager.Key(0x3DU, 4U, true, 900010U),
                   manager.Key(0x3DU, 0U, false, 900011U), manager.Key(0x17U, 5U, true, 900012U),
                   manager.TakeNewTerminal(), !manager.TakeNewTerminal(), manager.Key(0x17U, 0U, false, 900013U));
    manager.Unmap(0U, 900020U);
    ASTERIA_EXPECT(t, "close focuses MRU survivor", manager.Focused() == 4U);
    manager.Unmap(4U, 900021U);
    ASTERIA_EXPECT(t, "empty focus", manager.Focused() == S::Scene::NoWindow);
    ASTERIA_EXPECT(t, "size snapping", S::SnapSize(137, 64U, 160U, 8U, 0U) == 136U,
                   S::SnapSize(-100, 64U, 160U, 8U, 0U) == 64U, S::SnapSize(900, 64U, 160U, 8U, 0U) == 160U);
}
void Models(TestContext& t)
{
    const auto seeds = TestSupport::GetSeedRange(1U, 200U);
    for (UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        TestSupport::Random random{seed};
        bool same{true};
        scene.Configure(128U, 96U, User::Graphics::PixelFormat::Rgbx);
        const User::Graphics::MemorySurface out{outputPixels, 128U, 96U, 128U, User::Graphics::PixelFormat::Rgbx};
        const User::Graphics::MemorySurface ref{referencePixels, 128U, 96U, 128U, User::Graphics::PixelFormat::Rgbx};
        for (Size step{0U}; same && (step < 300U); ++step)
        {
            const Size slot = static_cast<Size>(random.Below(16U));
            const UInt64 op = random.Below(6U);
            const User::Graphics::Rect frame{static_cast<Foundation::Int32>(random.Below(170U)) - 50,
                                             static_cast<Foundation::Int32>(random.Below(120U)) - 40,
                                             static_cast<UInt32>(random.Below(120U) + 1U),
                                             static_cast<UInt32>(random.Below(100U) + 1U)};
            if (op == 0U)
            {
                if (!scene.IsShown(slot))
                {
                    static_cast<void>(scene.Show(slot, frame));
                }
            }
            else if (op == 1U)
            {
                scene.Hide(slot);
            }
            else if (op == 2U)
            {
                scene.SetFrame(slot, frame);
            }
            else if (op == 3U)
            {
                scene.Raise(slot);
            }
            else if (op == 4U)
            {
                auto look = scene.Look(slot);
                look.State = static_cast<S::TitleState>(random.Below(3U));
                scene.SetLook(slot, look);
            }
            else
            {
                for (Size present{0U}; present < 2U; ++present)
                {
                    const auto x = static_cast<UInt32>(random.Below(48U));
                    const auto y = static_cast<UInt32>(random.Below(48U));
                    const User::Graphics::Rect damage{static_cast<Foundation::Int32>(x),
                                                      static_cast<Foundation::Int32>(y), 16U, 16U};
                    const UInt32 value = static_cast<UInt32>(random.Next()) & 0x00FF'FFFFU;
                    for (UInt32 row{y}; row < y + 16U; ++row)
                    {
                        for (UInt32 col{x}; col < x + 16U; ++col)
                        {
                            imagePixels[slot][row * 64U + col] = value;
                        }
                    }
                    scene.Present(slot, {imagePixels[slot], 64U, 64U, 64U, out.Format}, &damage, 1U);
                }
            }
            static_cast<void>(scene.Compose(out));
            scene.RenderAll(ref);
            same = std::memcmp(outputPixels, referencePixels, sizeof(outputPixels)) == 0;
        }
        order.Configure(seed + 1U, seed + 1U);
        UInt64 keyCount{0U};
        UInt64 pointerCount{0U};
        UInt64 expected[100U]{};
        Size keyNext{0U};
        Size pointerNext{0U};
        struct Entry
        {
            bool Key;
            UInt64 Dependency;
            UInt64 Sequence;
        } entries[100U]{};
        for (Size i{0U}; i < 100U; ++i)
        {
            const bool key = random.Chance(1U, 2U);
            entries[i] = {key, key ? pointerCount : keyCount, key ? ++keyCount : ++pointerCount};
            expected[i] = i + 1U;
        }
        Size applied{0U};
        for (Size step{0U}; same && (step < 1000U) && (applied < 100U); ++step)
        {
            const bool key = random.Chance(1U, 2U);
            Size& next = key ? keyNext : pointerNext;
            while ((next < 100U) && (entries[next].Key != key))
            {
                ++next;
            }
            if (next < 100U)
            {
                const auto& e = entries[next];
                same = key ? order.Keys(e.Dependency, 1U, next + 1U, 0U)
                           : order.Pointer(e.Sequence, e.Dependency, next + 1U, 0U);
                ++next;
            }
            for (Size i{0U}; i < 100U; ++i)
            {
                const auto item = order.Take(1U);
                if (item.Type == S::InputOrder::Kind::None)
                {
                    break;
                }
                same = same && (item.Token == expected[applied++]);
            }
        }
        same =
            same && (applied == 100U) && (order.AppliedKeys() == keyCount) && (order.AppliedPointer() == pointerCount);
        if (!ASTERIA_EXPECT(t, "seeded damage and two-way key/click order models", same))
        {
            std::fprintf(stderr, "seed=%llu\n", static_cast<unsigned long long>(seed));
        }
    }
    order.Configure(1000U, 1000U);
    ASTERIA_EXPECT(t, "waiting key batch cannot use old focus", order.Keys(1U, 2U, 1U, 1U),
                   order.Take(5000000U).Type == S::InputOrder::Kind::None,
                   order.Take(5000001U).Type == S::InputOrder::Kind::None, order.Dropped() == 2U,
                   !order.Keys(0U, 1U, 1U, 5000002U));
}
int main()
{
    TestContext t{"PolicyModelTests"};
    Budgets(t);
    Delivery(t);
    Management(t);
    Models(t);
    ASTERIA_EXPECT(t, "default terminal pair matches console",
                   S::TerminalDefaultForeground.Red == User::Console::ConsoleForeground.Red,
                   S::TerminalDefaultForeground.Green == User::Console::ConsoleForeground.Green,
                   S::TerminalDefaultForeground.Blue == User::Console::ConsoleForeground.Blue,
                   S::TerminalDefaultBackground.Red == User::Console::ConsoleBackground.Red,
                   S::TerminalDefaultBackground.Green == User::Console::ConsoleBackground.Green,
                   S::TerminalDefaultBackground.Blue == User::Console::ConsoleBackground.Blue);
    return t.Finish();
}
