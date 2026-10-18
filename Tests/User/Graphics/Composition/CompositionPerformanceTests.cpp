// SPDX-License-Identifier: Apache-2.0
#include "User/Graphics/Composition/Composition.hpp"

#include <chrono>
#include <cstdio>
#include <initializer_list>

#include "Platform/Synchronization/DeviceMemoryBarrier.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace F = Asteria::Foundation;
namespace G = Asteria::User::Graphics;
namespace C = G::Composition;
namespace Console = Asteria::User::Console;
using Asteria::TestSupport::TestContext;
alignas(8) F::UInt32 g_pixels[1366U * 768U]{};
Console::TextConsole g_console{};
C::Composer g_composer{};
C::DamageList g_damage{};
F::Size g_orders{0U};

void Order() noexcept
{
    Asteria::Platform::Synchronization::OrderScanoutStorage();
    ++g_orders;
}

void Benchmark(TestContext& test, const G::Point size)
{
    const G::Surface surface{.Pixels = g_pixels,
                             .Width = static_cast<F::UInt32>(size.X),
                             .Height = static_cast<F::UInt32>(size.Y),
                             .Stride = static_cast<F::UInt32>(size.X),
                             .Format = G::PixelFormat::Bgrx};
    g_console.Configure(surface.Width, surface.Height, G::Bold8x16);
    C::BackgroundPlane background{Console::ConsoleBackground};
    C::ConsolePlane text{g_console};
    C::CursorPlane cursor{};
    cursor.Move({320, 240});
    g_composer.Reset();
    ASTERIA_EXPECT(test, "benchmark uses the broker's three planes and output",
                   g_composer.AddOutput({.Scanout = surface, .Origin = {0, 0}, .Scale = 1U}),
                   g_composer.AddPlane(background), g_composer.AddPlane(text), g_composer.AddPlane(cursor));
    g_damage.Configure(G::Bounds(surface));
    constexpr F::UInt8 line[]{'A', 's', 't', 'e', 'r', 'i', 'a', '\n'};
    for (F::Size row{0U}; row < 60U; ++row)
    {
        g_console.Write(line, sizeof(line));
    }
    // Report time only; CI's verdict is pixel/store accounting and ordering.
    constexpr F::Size Passes{32U};
    for (const bool fullScreen : {true, false})
    {
        const F::UInt32 width = fullScreen ? surface.Width : g_console.Columns() * 8U;
        const F::UInt32 height = fullScreen ? surface.Height : g_console.Rows() * 16U;
        const G::Rect area{.X = 0, .Y = 0, .Width = width, .Height = height};
        C::RenderStatistics total{};
        const F::Size ordersBefore = g_orders;
        const auto begin = std::chrono::steady_clock::now();
        for (F::Size pass{0U}; pass < Passes; ++pass)
        {
            g_console.Write(line, sizeof(line));
            g_damage.Add(area);
            // Adjacent row damage and cursor damage must not add extra passes.
            g_damage.Add({.X = 0, .Y = 0, .Width = width, .Height = 16U});
            g_damage.Add(cursor.Bounds());
            const C::RenderStatistics stats = g_composer.Render(g_damage, Order);
            total.Pixels += stats.Pixels;
            total.Stores32 += stats.Stores32;
            total.Stores64 += stats.Stores64;
            g_console.MarkDrawn();
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        const F::UInt64 pixels = static_cast<F::UInt64>(width) * height * Passes;
        ASTERIA_EXPECT(test, "scroll writes each damaged pixel once across all planes, with one fence",
                       total.Pixels == pixels, total.Stores32 + total.Stores64 * 2U == pixels, total.Stores32 == 0U,
                       total.Stores64 == pixels / 2U, g_orders == ordersBefore + Passes);
        static_cast<void>(std::printf("%ux%u %s: %.2f us/pass, %llu px, %llu stores32, %llu stores64\n", surface.Width,
                                      surface.Height, fullScreen ? "full screen" : "console scroll",
                                      static_cast<double>(elapsed) / static_cast<double>(Passes),
                                      static_cast<unsigned long long>(total.Pixels / Passes),
                                      static_cast<unsigned long long>(total.Stores32 / Passes),
                                      static_cast<unsigned long long>(total.Stores64 / Passes)));
    }
}
} // namespace

int main()
{
    TestContext test{"CompositionPerformanceTests"};
    Benchmark(test, {640, 480});
    Benchmark(test, {1366, 768});
    return test.Finish();
}
