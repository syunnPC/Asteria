// SPDX-License-Identifier: Apache-2.0
#include "User/Session/ClientEvents.hpp"
#include "User/Session/WindowManager.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace S = User::Session;
using Foundation::Size;
using Foundation::UInt64;
S::ClientEvents state;
S::ClientEvents clientsForModel[4U];
S::SessionEvent answer[7U];
S::Scene modelScene;
S::WindowManager modelManager;
int main()
{
    TestSupport::TestContext t{"EventModelTests"};
    const auto seeds = TestSupport::GetSeedRange(500U, 200U);
    for (UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        TestSupport::Random random{seed};
        state.Reset();
        bool same{true};
        std::vector<S::SessionEvent> reference;
        std::vector<S::SessionEvent> frozen;
        UInt64 returned{0U};
        UInt64 frozenReturned{0U};
        bool credit{false};
        bool blocked{false};
        Size maximum{1U};
        for (UInt64 step{1U}; same && (step < 2000U); ++step)
        {
            const UInt64 op = random.Below(5U);
            if ((op == 0U) && (reference.size() < 55U))
            {
                // Paired wheel/button/motion entries exercise the FIFO and
                // adjacent-only motion coalescing without exceeding broker held bounds.
                const auto kind = static_cast<S::SessionEventKind>(14U + random.Below(3U));
                const UInt64 surface = random.Below(4U) + 1U;
                const S::SessionEvent event{
                    kind, {surface, step, kind == S::SessionEventKind::Wheel ? 0U : random.Below(8U), 0U}};
                same = state.Queue(event, step * 20000U);
                if (!reference.empty() && (reference.size() > frozen.size() - (frozenReturned != 0U ? 1U : 0U)) &&
                    (kind == S::SessionEventKind::PointerMotion) && (reference.back().Kind == kind) &&
                    (reference.back().Words[0U] == surface))
                {
                    reference.back() = event;
                }
                else
                {
                    reference.push_back(event);
                }
            }
            else if (op == 1U)
            {
                const UInt64 bitmap = 1ULL << random.Below(8U);
                same = state.Queue({S::SessionEventKind::ImageReturned, {bitmap, 0U, 0U, 0U}}, step * 20000U);
                returned |= bitmap;
            }
            else if ((op == 2U) && !credit)
            {
                maximum = static_cast<Size>(random.Below(7U)) + 1U;
                same = state.Credit(maximum, step * 20000U);
                credit = true;
            }
            else if ((op >= 3U) && credit)
            {
                if (frozen.empty())
                {
                    frozenReturned = returned;
                    if (returned != 0U)
                    {
                        frozen.push_back({S::SessionEventKind::ImageReturned, {returned, 0U, 0U, 0U}});
                    }
                    for (Size i{0U}; (i < reference.size()) && (frozen.size() < maximum); ++i)
                    {
                        frozen.push_back(reference[i]);
                    }
                }
                const Size size = state.Build(answer, step * 20000U);
                same = same && (size == frozen.size());
                for (Size i{0U}; same && (i < size); ++i)
                {
                    same = (answer[i].Kind == frozen[i].Kind) &&
                           (std::memcmp(answer[i].Words, frozen[i].Words, sizeof(answer[i].Words)) == 0);
                }
                if (size == 0U)
                {
                    continue;
                }
                if ((op == 3U) && !blocked)
                {
                    state.SendFailed(true, step * 20000U);
                    blocked = true;
                }
                else
                {
                    state.Commit(step * 20000U);
                    returned &= ~frozenReturned;
                    const Size consumed = frozen.size() - (frozenReturned != 0U ? 1U : 0U);
                    for (Size i{0U}; i < consumed; ++i)
                    {
                        reference.erase(reference.begin());
                    }
                    frozen.clear();
                    frozenReturned = 0U;
                    credit = false;
                    blocked = false;
                }
            }
            same = same && (reference.size() == state.Queued());
        }
        modelScene.Configure(1024U, 768U, User::Graphics::PixelFormat::Rgbx);
        for (auto& c : clientsForModel)
        {
            c.Reset();
        }
        modelManager.Configure(modelScene, clientsForModel);
        std::vector<Size> mru;
        Size focused = S::Scene::NoWindow;
        bool shown[16U]{};
        for (UInt64 step{1U}; same && (step < 1000U); ++step)
        {
            const Size slot = static_cast<Size>(random.Below(16U));
            const UInt64 op = random.Below(5U);
            if ((op == 0U) && !shown[slot])
            {
                same = modelManager.Map(slot, 80U, 64U, 0U, step * 400000U);
                shown[slot] = true;
                mru.push_back(slot);
                if (focused == S::Scene::NoWindow)
                {
                    focused = slot;
                    mru.erase(std::find(mru.begin(), mru.end(), slot));
                    mru.insert(mru.begin(), slot);
                }
            }
            else if ((op == 1U) && shown[slot])
            {
                modelManager.Focus(slot, step * 400000U);
                focused = slot;
                mru.erase(std::find(mru.begin(), mru.end(), slot));
                mru.insert(mru.begin(), slot);
            }
            else if ((op == 2U) && shown[slot])
            {
                modelManager.Unmap(slot, step * 400000U);
                shown[slot] = false;
                mru.erase(std::find(mru.begin(), mru.end(), slot));
                if (focused == slot)
                {
                    focused = mru.empty() ? S::Scene::NoWindow : mru.front();
                }
            }
            else if ((op == 3U) && shown[slot])
            {
                const auto frame = modelScene.Frame(slot);
                const User::Graphics::Point point{frame.X + 6, frame.Y + 30};
                const Size target = modelScene.TopmostAt(point);
                modelManager.Pointer(point, 1U, 0, step * 400000U);
                modelManager.Pointer(point, 0U, 0, step * 400000U + 1U);
                if (target != S::Scene::NoWindow)
                {
                    focused = target;
                    mru.erase(std::find(mru.begin(), mru.end(), target));
                    mru.insert(mru.begin(), target);
                }
            }
            else if ((op == 4U) && !mru.empty())
            {
                const Size target = mru[mru.size() > 1U ? 1U : 0U];
                same = modelManager.Key(0x2BU, 4U, true, step * 400000U) &&
                       modelManager.Key(0x2BU, 4U, false, step * 400000U + 1U) &&
                       modelManager.Key(0xE2U, 0U, false, step * 400000U + 2U);
                focused = target;
                mru.erase(std::find(mru.begin(), mru.end(), target));
                mru.insert(mru.begin(), target);
            }
            same = same && (modelManager.Focused() == focused) && (modelScene.Count() == mru.size());
        }
        if (!ASTERIA_EXPECT(t, "seeded event-credit reference and MRU reference", same))
        {
            std::fprintf(stderr, "seed=%llu\n", static_cast<unsigned long long>(seed));
        }
    }
    return t.Finish();
}
