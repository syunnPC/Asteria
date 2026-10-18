// SPDX-License-Identifier: Apache-2.0
#include "User/Session/WindowManager.hpp"
#include "Support/TestContext.hpp"
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace S = User::Session;
using Foundation::UInt64;
using Foundation::Size;
S::Scene scene;
S::ClientEvents clients[4U];
S::WindowManager wm;
S::SessionEvent events[7U];
constexpr Foundation::UInt32 OutlineWidth{128U};
constexpr Foundation::UInt32 OutlineHeight{96U};
Foundation::UInt32 outlineBefore[OutlineWidth * OutlineHeight];
Foundation::UInt32 outlineAfter[OutlineWidth * OutlineHeight];
void Configure()
{
    scene.Configure(800U, 600U, User::Graphics::PixelFormat::Rgbx);
    for (auto& c : clients)
    {
        c.Reset();
    }
    wm.Configure(scene, clients);
}
[[nodiscard]] UInt64 Arrangement(Size client, UInt64 now)
{
    if (!clients[client].Credit(7U, now))
    {
        return 0U;
    }
    const Size count = clients[client].Build(events, now);
    UInt64 serial{0U};
    for (Size i{0U}; i < count; ++i)
    {
        if (events[i].Kind == S::SessionEventKind::Arrange)
        {
            serial = events[i].Words[1U];
        }
    }
    if (count != 0U)
    {
        clients[client].Commit(now);
    }
    else
    {
        clients[client].SendFailed(false, now);
    }
    return serial;
}
int main()
{
    TestSupport::TestContext t{"WindowManagerTests"};
    Configure();
    wm.UserLaunch(7U);
    ASTERIA_EXPECT(t, "launch serial first window", wm.Map(0U, 100U, 80U, 0U, 0U), wm.Map(4U, 100U, 80U, 7U, 0U),
                   wm.Focused() == 4U);
    wm.Unmap(4U, 1U);
    wm.Unmap(0U, 2U);
    ASTERIA_EXPECT(t, "oversize anchors zero", wm.Map(0U, 900U, 700U, 0U, 3U), scene.Frame(0U).X == 0,
                   scene.Frame(0U).Y == 0);
    Configure();
    ASTERIA_EXPECT(t, "initial map", wm.Map(0U, 100U, 80U, 0U, 0U));
    const UInt64 first = Arrangement(0U, 1U);
    ASTERIA_EXPECT(t, "initial Arrange issued", first != 0U);
    S::SizeHints hints;
    hints.MinWidth = 64U;
    hints.MinHeight = 32U;
    hints.MaxWidth = 160U;
    hints.MaxHeight = 128U;
    hints.IncrementWidth = 8U;
    hints.IncrementHeight = 16U;
    ASTERIA_EXPECT(t, "surface hints", wm.SetHints(0U, hints));
    // Right/bottom corner of frame (32,32,108,109).
    wm.Pointer({138, 139}, 1U, 0, 400000U);
    wm.Pointer({175, 170}, 1U, 0, 400001U);
    ASTERIA_EXPECT(t, "corner resizes both snapped dimensions", scene.Frame(0U).Width == 144U,
                   scene.Frame(0U).Height == 125U);
    ASTERIA_EXPECT(t, "resize holds pending Arrange behind first", Arrangement(0U, 400002U) == 0U);
    wm.AcceptArrange(0U, first + 999U, 400003U);
    ASTERIA_EXPECT(t, "wrong acceptance does nothing", Arrangement(0U, 400004U) == 0U);
    wm.AcceptArrange(0U, first, 400005U);
    const UInt64 second = Arrangement(0U, 400006U);
    ASTERIA_EXPECT(t, "matching Present emits latest resize", second > first);
    wm.Pointer({175, 170}, 0U, 0, 400007U);
    wm.AcceptArrange(0U, second, 400008U);
    const UInt64 finished = Arrangement(0U, 400009U);
    ASTERIA_EXPECT(t, "release emits non-resizing arrangement", finished > second);
    for (Size i{0U}; i < 7U; ++i)
    {
        if (events[i].Kind == S::SessionEventKind::Arrange)
        {
            ASTERIA_EXPECT(t, "resize bit clears on release", (events[i].Words[3U] & S::SessionArrangeResizing) == 0U);
            break;
        }
    }
    const auto restore = scene.Frame(0U);
    wm.Pointer({45, 40}, 1U, 0, 1000000U);
    wm.Pointer({45, 40}, 0U, 0, 1000001U);
    wm.Pointer({46, 41}, 1U, 0, 1499999U);
    wm.Pointer({46, 41}, 0U, 0, 1500000U);
    ASTERIA_EXPECT(t, "title double click maximizes", scene.Look(0U).Maximized, scene.Frame(0U).Width == 800U,
                   scene.Frame(0U).Height == 600U);
    wm.Pointer({20, 12}, 1U, 0, 2100000U);
    wm.Pointer({60, 50}, 1U, 0, 2100001U);
    wm.Pointer({60, 50}, 0U, 0, 2100002U);
    ASTERIA_EXPECT(t, "maximized title drag stays still", scene.Frame(0U).X == 0, scene.Frame(0U).Y == 0);
    const auto maximize = S::MaximizeButtonRect(scene.Frame(0U));
    wm.Pointer({maximize.X + 1, maximize.Y + 1}, 1U, 0, 2200000U);
    wm.Pointer({maximize.X + 1, maximize.Y + 1}, 0U, 0, 2200001U);
    ASTERIA_EXPECT(t, "maximize button restores", !scene.Look(0U).Maximized, scene.Frame(0U).X == restore.X,
                   scene.Frame(0U).Width == restore.Width);
    wm.LastPass(50001U);
    wm.Pointer({45, 40}, 1U, 0, 3000000U);
    wm.Pointer({145, 100}, 1U, 0, 3000001U);
    ASTERIA_EXPECT(t, "slow-pass drag moves outline only", scene.Frame(0U).X == restore.X);
    wm.Pointer({145, 100}, 0U, 0, 3000002U);
    ASTERIA_EXPECT(t, "outline commits on release", scene.Frame(0U).X == restore.X + 100,
                   scene.Frame(0U).Y == restore.Y + 60);
    const auto content = S::ContentRect(scene.Frame(0U));
    wm.Pointer({content.X + 1, content.Y + 1}, 1U, 0, 4000000U);
    wm.Pointer({0, 0}, 1U, -1, 4000001U);
    ASTERIA_EXPECT(t, "implicit grab sends outside motion in surface coordinates", clients[0U].Credit(7U, 4000002U));
    const Size count = clients[0U].Build(events, 4000002U);
    bool motion{false};
    bool wheel{false};
    for (Size i{0U}; i < count; ++i)
    {
        if (events[i].Kind == S::SessionEventKind::PointerMotion)
        {
            motion = events[i].Words[1U] == S::PackSessionPosition(-content.X, -content.Y);
        }
        if (events[i].Kind == S::SessionEventKind::Wheel)
        {
            wheel = events[i].Words[1U] == 0xFFFF'FFFFU;
        }
    }
    ASTERIA_EXPECT(t, "grab motion and wheel survive", motion, wheel);
    clients[0U].Commit(4000002U);
    wm.Pointer({0, 0}, 0U, 0, 4000003U);
    Configure();
    ASTERIA_EXPECT(t, "focus release maps", wm.Map(0U, 100U, 80U, 0U, 0U), wm.Map(4U, 100U, 80U, 0U, 0U));
    static_cast<void>(Arrangement(0U, 1U));
    ASTERIA_EXPECT(t, "ordinary key forwarded", !wm.Key(4U, 0U, true, 2U), clients[0U].Credit(7U, 3U),
                   clients[0U].Build(events, 3U) == 1U);
    clients[0U].Commit(3U);
    ASTERIA_EXPECT(t, "second undelivered make queued", !wm.Key(5U, 0U, true, 4U));
    wm.Focus(4U, 5U);
    ASTERIA_EXPECT(t, "focus loss preserves earlier make and balances both keys", clients[0U].Credit(7U, 6U),
                   clients[0U].Build(events, 6U) == 4U, events[0U].Kind == S::SessionEventKind::KeyFocus,
                   events[0U].Words[1U] == 0U, events[1U].Words[1U] == S::SessionKeyDown, events[1U].Words[2U] == 5U,
                   events[2U].Words[1U] == S::SessionKeyUp, events[2U].Words[2U] == 4U,
                   events[3U].Words[1U] == S::SessionKeyUp, events[3U].Words[2U] == 5U);
    Configure();
    ASTERIA_EXPECT(t, "Unicode scene setup", wm.Map(0U, 100U, 80U, 0U, 0U));
    const Foundation::UInt8 valid[]{0xC3U, 0xA9U};
    const Foundation::UInt8 bad[]{0xEDU, 0xA0U, 0x80U};
    ASTERIA_EXPECT(t, "scene rejects malformed UTF-8 before changing strings",
                   scene.SetTitle(0U, valid, sizeof(valid), S::ClientClass::SystemModule),
                   scene.SetSubtitle(0U, valid, sizeof(valid)),
                   !scene.SetTitle(0U, bad, sizeof(bad), S::ClientClass::SystemModule),
                   !scene.SetSubtitle(0U, bad, sizeof(bad)));
    const auto before{wm.Focused()};
    ASTERIA_EXPECT(t, "Super does not activate session shortcuts", !wm.Key(0x2BU, 0x0CU, true, 900001U),
                   !wm.Key(0x3DU, 0x84U, true, 900002U), !wm.Key(0x17U, 0x0DU, true, 900003U), !wm.TakeNewTerminal(),
                   wm.Focused() == before);
    Configure();
    ASTERIA_EXPECT(t, "outline double-click setup", wm.Map(0U, 100U, 80U, 0U, 0U));
    const auto original = scene.Frame(0U);
    wm.LastPass(S::OutlinePassUs + 1U);
    wm.Pointer({56, 44}, 1U, 0, 1000000U);
    wm.Pointer({56, 44}, 0U, 0, 1000001U);
    wm.Pointer({56, 44}, 1U, 0, 1100000U);
    wm.Pointer({56, 44}, 0U, 0, 1100001U);
    ASTERIA_EXPECT(t, "double-click maximize cannot commit the pre-maximize drag frame", scene.Look(0U).Maximized,
                   scene.Frame(0U).X == 0, scene.Frame(0U).Y == 0, scene.Frame(0U).Width == scene.Output().Width,
                   scene.Frame(0U).Height == scene.Output().Height);
    Configure();
    ASTERIA_EXPECT(t, "outline restore setup", wm.Map(0U, 100U, 80U, 0U, 0U));
    wm.LastPass(S::OutlinePassUs + 1U);
    const auto outlineButton = S::MaximizeButtonRect(scene.Frame(0U));
    wm.Pointer({outlineButton.X + 1, outlineButton.Y + 1}, 1U, 0, 1000000U);
    wm.Pointer({outlineButton.X + 1, outlineButton.Y + 1}, 0U, 0, 1000001U);
    wm.Pointer({24, 12}, 1U, 0, 2000000U);
    wm.Pointer({24, 12}, 0U, 0, 2000001U);
    wm.Pointer({24, 12}, 1U, 0, 2100000U);
    wm.Pointer({24, 12}, 0U, 0, 2100001U);
    ASTERIA_EXPECT(t, "outline double-click restore preserves the saved rectangle", !scene.Look(0U).Maximized,
                   scene.Frame(0U).X == original.X, scene.Frame(0U).Y == original.Y,
                   scene.Frame(0U).Width == original.Width, scene.Frame(0U).Height == original.Height);
    Configure();
    ASTERIA_EXPECT(t, "live restore setup", wm.Map(0U, 100U, 80U, 0U, 0U));
    auto button = S::MaximizeButtonRect(scene.Frame(0U));
    wm.Pointer({button.X + 1, button.Y + 1}, 1U, 0, 1000000U);
    wm.Pointer({button.X + 1, button.Y + 1}, 0U, 0, 1000001U);
    wm.Pointer({24, 12}, 1U, 0, 2000000U);
    wm.Pointer({24, 12}, 0U, 0, 2000001U);
    wm.Pointer({24, 12}, 1U, 0, 2100000U);
    wm.Pointer({24, 12}, 0U, 0, 2100001U);
    ASTERIA_EXPECT(t, "live restore cannot move the pre-restore maximized frame over the small image",
                   !scene.Look(0U).Maximized, scene.Frame(0U).X == original.X, scene.Frame(0U).Y == original.Y,
                   scene.Frame(0U).Width == original.Width, scene.Frame(0U).Height == original.Height);
    // Adaptive move choice belongs to a gesture, not each event.
    constexpr bool moveModes[]{false, true};
    for (const bool outline : moveModes)
    {
        Configure();
        ASTERIA_EXPECT(t, "edge drag setup", wm.Map(0U, 100U, 80U, 0U, 0U));
        wm.LastPass(outline ? S::OutlinePassUs + 1U : 0U);
        wm.Pointer({56, 44}, 1U, 0, 1000000U);
        wm.Pointer({900, 700}, 1U, 0, 1000001U);
        wm.LastPass(outline ? 0U : S::OutlinePassUs + 1U);
        wm.Pointer({900, 700}, 0U, 0, 1000002U);
        ASTERIA_EXPECT(t, "bottom-right release clamps and commits despite a changed pass time",
                       scene.Frame(0U).X == 800 - static_cast<Foundation::Int32>(S::VisibleTitleWidth),
                       scene.Frame(0U).Y == 600 - static_cast<Foundation::Int32>(S::FrameWidth + S::TitleBarHeight),
                       scene.Frame(0U).Width == original.Width, !scene.Look(0U).Maximized);
        wm.Pointer({790, 590}, 1U, 0, 2000000U);
        wm.Pointer({50, 50}, 1U, 0, 2000001U);
        wm.Pointer({50, 50}, 0U, 0, 2000002U);
        ASTERIA_EXPECT(t, "clamped title remains movable", scene.Frame(0U).X < 100, scene.Frame(0U).Y < 100);
        button = S::MaximizeButtonRect(scene.Frame(0U));
        wm.Pointer({button.X + 1, button.Y + 1}, 1U, 0, 3000000U);
        wm.Pointer({button.X + 1, button.Y + 1}, 0U, 0, 3000001U);
        ASTERIA_EXPECT(t, "edge drag retains maximize control", scene.Look(0U).Maximized,
                       scene.Frame(0U).Width == 800U);
    }
    Configure();
    ASTERIA_EXPECT(t, "drag-return click setup", wm.Map(0U, 100U, 80U, 0U, 0U));
    wm.Pointer({56, 44}, 1U, 0, 1000000U);
    wm.Pointer({156, 144}, 1U, 0, 1000001U);
    wm.Pointer({56, 44}, 1U, 0, 1000002U);
    wm.Pointer({56, 44}, 0U, 0, 1000003U);
    wm.Pointer({56, 44}, 1U, 0, 1100000U);
    wm.Pointer({56, 44}, 0U, 0, 1100001U);
    ASTERIA_EXPECT(t, "a completed drag cannot become the first click of a double-click", !scene.Look(0U).Maximized,
                   scene.Frame(0U).Width == original.Width);
    Configure();
    scene.Configure(OutlineWidth, OutlineHeight, User::Graphics::PixelFormat::Rgbx);
    ASTERIA_EXPECT(t, "outline cleanup setup", wm.Map(0U, 64U, 24U, 0U, 0U));
    wm.LastPass(S::OutlinePassUs + 1U);
    wm.Pointer({44, 42}, 1U, 0, 1000000U);
    wm.Pointer({256, 192}, 1U, 0, 1000001U);
    wm.LastPass(0U);
    wm.Pointer({256, 192}, 0U, 0, 1000002U);
    const User::Graphics::MemorySurface beforeOutline{.Pixels = outlineBefore,
                                                      .Width = OutlineWidth,
                                                      .Height = OutlineHeight,
                                                      .Stride = OutlineWidth,
                                                      .Format = User::Graphics::PixelFormat::Rgbx};
    const User::Graphics::MemorySurface afterOutline{.Pixels = outlineAfter,
                                                     .Width = OutlineWidth,
                                                     .Height = OutlineHeight,
                                                     .Stride = OutlineWidth,
                                                     .Format = User::Graphics::PixelFormat::Rgbx};
    scene.RenderAll(beforeOutline);
    scene.HideOutline();
    scene.RenderAll(afterOutline);
    bool noGhost{true};
    for (Size i{0U}; i < OutlineWidth * OutlineHeight; ++i)
    {
        if (outlineBefore[i] != outlineAfter[i])
        {
            noGhost = false;
        }
    }
    ASTERIA_EXPECT(t, "changed pass time cannot leave an unclickable outline after release", noGhost);
    return t.Finish();
}
