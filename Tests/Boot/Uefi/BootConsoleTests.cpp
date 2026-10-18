// SPDX-License-Identifier: Apache-2.0
#include "Boot/Uefi/BootConsole.hpp"

#include <cstdio>
#include <cstring>

#include "Platform/Firmware/Diagnostic.hpp"
#include "Support/TestContext.hpp"
#include "Support/Random.hpp"
#include "User/Console/TextConsole.hpp"

namespace Asteria::Platform::Firmware
{
void WriteDiagnosticAscii(const char*) noexcept
{
}
} // namespace Asteria::Platform::Firmware

namespace
{
namespace Uefi = Asteria::Boot::Uefi;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
constinit UInt64 g_stores{0U};
constinit UInt32 g_calls{0U};
constinit bool g_valid{true};
constinit bool g_fail{false};
constinit UInt32 g_firmwareCalls{0U};
constinit UInt32 g_expectedY{0U};
constinit UInt32 g_width{1280U};
constinit UInt32 g_height{1800U};
constinit Uefi::GraphicsOutputBltPixel g_last[1280U * 16U]{};
constinit UInt32 g_legacyPixels[2880U * 1800U]{};
constinit Asteria::User::Console::TextConsole g_legacy{};
constinit UInt32 g_expectedPixels[1280U * 16U]{};

Uefi::Status ASTERIA_UEFI_API Output(Uefi::SimpleTextOutputProtocol*, Asteria::Foundation::UInt16*)
{
    ++g_firmwareCalls;
    return Uefi::Success;
}

Uefi::Status ASTERIA_UEFI_API Blt(Uefi::GraphicsOutputProtocol*, Uefi::GraphicsOutputBltPixel* source,
                                  const Uefi::GraphicsOutputBltOperation operation, const Size sourceX,
                                  const Size sourceY, const Size x, const Size y, const Size width, const Size height,
                                  const Size delta)
{
    ++g_calls;
    g_valid = g_valid && operation == Uefi::GraphicsOutputBltOperation::BufferToVideo && sourceX == 0U &&
              sourceY == 0U && x == 0U && y == g_expectedY && width == g_width && height == 16U &&
              delta == width * 4U && y + height <= g_height;
    if (g_fail)
    {
        return Uefi::DeviceError;
    }
    for (Size index{0U}; index < width * height; ++index)
    {
        g_last[index] = source[index];
        ++g_stores;
    }
    g_expectedY = y + 32U <= g_height ? static_cast<UInt32>(y + 16U) : 0U;
    return Uefi::Success;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"BootConsoleTests"};
    Uefi::GraphicsOutputModeInformation information{0U, 2880U, 1800U, 1U, 0U, 0U, 0U, 0U, 2880U};
    // Deliberately inaccessible physical address: only firmware Blt may touch
    // scanout, including on IA-64, so a direct store/read is a test failure.
    Uefi::GraphicsOutputProtocolMode mode{1U, 0U, &information, sizeof(information), 0x1000U, 2880U * 1800U * 4U};
    Uefi::GraphicsOutputProtocol graphics{nullptr, nullptr, Blt, &mode};
    Uefi::SimpleTextOutputProtocol console{};
    console.OutputString = Output;
    Uefi::ConfigureBootConsole(&graphics);
    char line[152U]{};
    for (Size index{0U}; index < 150U; ++index)
    {
        line[index] = 'A';
    }
    line[150U] = '\n';
    for (UInt32 index{0U}; index < 200U; ++index)
    {
        const UInt64 before{g_stores};
        ASTERIA_EXPECT(context, "one line despite firmware-sized chunks", Uefi::WriteBootConsoleAscii(console, line),
                       g_stores - before == 20'480U);
    }
    ASTERIA_EXPECT(context, "200 lines wrap without scrolling or framebuffer reads", g_valid, g_calls == 200U,
                   g_stores == 4'096'000U, g_firmwareCalls == 0U);
    // Compare the boot console with TextConsole rendering each 95-character chunk
    // on a 160 x 50 grid. Count one store per damaged cell pixel, including scrolls.
    g_legacy.Configure(1280U, 800U, Asteria::User::Graphics::Bold8x16);
    Asteria::User::Graphics::Surface surface{g_legacyPixels, 2880U, 1800U, 2880U,
                                             Asteria::User::Graphics::PixelFormat::Bgrx};
    UInt64 legacyStores{0U};
    UInt64 legacyMaximum{0U};
    for (UInt32 index{0U}; index < 200U; ++index)
    {
        const UInt64 before{legacyStores};
        for (Size offset{0U}; offset < 151U; offset += 95U)
        {
            const Size count{151U - offset < 95U ? 151U - offset : 95U};
            // Text and pixels are separate objects: no framebuffer inspection.
            Asteria::Foundation::UInt8 chunk[95U]{};
            for (Size byte{0U}; byte < count; ++byte)
            {
                chunk[byte] = static_cast<Asteria::Foundation::UInt8>(line[offset + byte]);
            }
            g_legacy.Write(chunk, count);
            const auto damage = g_legacy.Damage();
            legacyStores += UInt64{damage.End - damage.First} * 160U * 8U * 16U;
            g_legacy.Render(surface, {0, 0});
        }
        if (legacyStores - before > legacyMaximum)
        {
            legacyMaximum = legacyStores - before;
        }
    }
    ASTERIA_EXPECT(context, "legacy store count pins redraw/scroll regression", legacyStores == 160'727'040U,
                   legacyMaximum == 1'044'480U);
    std::printf("2880x1800, 200 lines: old=%llu max/line=%llu; new=%llu max/line=20480\n",
                static_cast<unsigned long long>(legacyStores), static_cast<unsigned long long>(legacyMaximum),
                static_cast<unsigned long long>(g_stores));
    // Split a line between independent callers: pending text must not cause a
    // device update until its line is complete.
    const UInt32 calls{g_calls};
    ASTERIA_EXPECT(context, "partial text stays in RAM", Uefi::WriteBootConsoleAscii(console, "failure:"),
                   g_calls == calls, Uefi::WriteBootConsoleAscii(console, " reason\r\n"), g_calls == calls + 1U);
    // A seeded reference model checks content and fragmented callers, while
    // the fixed 200-line measurement above pins the costly scroll boundary.
    const auto seeds = Asteria::TestSupport::GetSeedRange(1U, 4U);
    Asteria::User::Graphics::Surface expectedSurface{g_expectedPixels, 1280U, 16U, 1280U,
                                                     Asteria::User::Graphics::PixelFormat::Bgrx};
    for (UInt64 seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::TestSupport::Random random{seed};
        for (UInt32 step{0U}; step < 64U; ++step)
        {
            g_legacy.Configure(1280U, 16U, Asteria::User::Graphics::Bold8x16);
            Asteria::Foundation::UInt8 input[120U]{};
            for (Size index{0U}; index < sizeof(input); ++index)
            {
                input[index] = static_cast<Asteria::Foundation::UInt8>(33U + random.Below(94U));
                if (random.Chance(1U, 10U))
                {
                    input[index] = '\r';
                }
                else if (random.Chance(1U, 10U))
                {
                    input[index] = '\b';
                }
            }
            g_legacy.Write(input, sizeof(input));
            g_legacy.Render(expectedSurface, {0, 0});
            const UInt32 beforeCalls{g_calls};
            Size offset{0U};
            while (offset < sizeof(input))
            {
                char fragment[96U]{};
                Size count{1U + static_cast<Size>(random.Below(95U))};
                if (count > sizeof(input) - offset)
                {
                    count = sizeof(input) - offset;
                }
                for (Size index{0U}; index < count; ++index)
                {
                    fragment[index] = static_cast<char>(input[offset + index]);
                }
                ASTERIA_EXPECT(context, "fragmented line is pending", Uefi::WriteBootConsoleAscii(console, fragment),
                               g_calls == beforeCalls);
                offset += count;
            }
            const bool matches{Uefi::WriteBootConsoleAscii(console, "\n") && g_calls == beforeCalls + 1U &&
                               std::memcmp(g_last, g_expectedPixels, sizeof(g_expectedPixels)) == 0};
            if (!matches)
            {
                std::fprintf(stderr, "seed=%llu step=%u\n", static_cast<unsigned long long>(seed), step);
            }
            ASTERIA_EXPECT(context, "fragmented line matches independent console/font model", matches, g_valid);
        }
    }
    g_fail = true;
    ASTERIA_EXPECT(context, "Blt failure falls back to firmware text",
                   Uefi::WriteBootConsoleAscii(console, "failure\n"), g_firmwareCalls == 1U,
                   !Uefi::IsBootConsoleConfigured());
    Uefi::ConfigureBootConsole(nullptr);
    return context.Finish();
}
