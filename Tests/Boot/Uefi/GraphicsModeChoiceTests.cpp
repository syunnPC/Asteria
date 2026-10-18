// SPDX-License-Identifier: Apache-2.0
#include "Boot/Uefi/GraphicsModeChoice.hpp"

#include <cstring>

#include "Boot/Uefi/BootConsole.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Support/EdidFixture.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Platform::Firmware
{
void WriteDiagnosticAscii(const char*) noexcept
{
}
} // namespace Asteria::Platform::Firmware

namespace
{
using Asteria::Boot::Uefi::Status;
using Asteria::Boot::Uefi::Success;
using Asteria::Foundation::UInt32;
namespace Uefi = Asteria::Boot::Uefi;
constinit Uefi::GraphicsOutputModeInformation g_modes[5]{};
constinit Uefi::GraphicsOutputModeInformation g_query{};
constinit Uefi::GraphicsOutputProtocolMode g_mode{};
constinit Uefi::GraphicsOutputProtocol g_graphics{};
constinit Asteria::Foundation::UInt8 g_edid[128]{};
constinit Uefi::EdidProtocol g_edidProtocol{128U, g_edid};
constinit Uefi::Handle g_handles[2]{};
alignas(4096) constinit UInt32 g_pixels[1368U * 768U]{};
constinit Asteria::Foundation::Size g_handleCount{1U};
constinit bool g_skipFirstEdid{false};
constinit UInt32 g_setCount{0U};
constinit UInt32 g_queries{0U};
constinit UInt32 g_frees{0U};
constinit UInt32 g_fault{0U};
constinit bool g_active{true};
constinit bool g_discovered{true};
constinit bool g_available{true};
constinit char g_transcript[256]{};

Status ASTERIA_UEFI_API Output(Uefi::SimpleTextOutputProtocol*, Asteria::Foundation::UInt16* text)
{
    Asteria::Foundation::Size offset{std::strlen(g_transcript)};
    for (Asteria::Foundation::Size index{0U}; text[index] != 0U && offset + 1U < sizeof(g_transcript); ++index)
    {
        g_transcript[offset++] = static_cast<char>(text[index]);
    }
    return Success;
}
Status ASTERIA_UEFI_API Locate(Uefi::LocateSearchType, Uefi::Guid*, void*, Asteria::Foundation::Size* count,
                               Uefi::Handle** handles)
{
    *count = g_handleCount;
    *handles = g_handles;
    return Success;
}
Status ASTERIA_UEFI_API Protocol(const Uefi::Handle handle, Uefi::Guid* guid, void** output)
{
    *output = nullptr;
    if (guid->Data1 == 0x9042A9DEU)
    {
        *output = &g_graphics;
    }
    else if (((guid->Data1 == 0xBD8C1056U && g_active) || (guid->Data1 == 0x1C0C34F6U && g_discovered)) &&
             (!g_skipFirstEdid || handle == g_handles[1]))
    {
        *output = &g_edidProtocol;
    }
    return *output != nullptr ? Success : Uefi::DeviceError;
}
Status ASTERIA_UEFI_API Free(void*)
{
    ++g_frees;
    return Success;
}
Status ASTERIA_UEFI_API Query(Uefi::GraphicsOutputProtocol*, const UInt32 index, Asteria::Foundation::Size* size,
                              Uefi::GraphicsOutputModeInformation** info)
{
    ++g_queries;
    if (index >= 5U || (index == 4U && !g_available))
    {
        return Uefi::DeviceError;
    }
    g_query = g_modes[index];
    *size = sizeof(g_query);
    *info = &g_query;
    return Success;
}
Status ASTERIA_UEFI_API Set(Uefi::GraphicsOutputProtocol*, const UInt32 index)
{
    ++g_setCount;
    g_mode.Mode = index;
    g_mode.Information = &g_modes[index];
    if (index != 0U)
    {
        if (g_fault == 1U)
        {
            return Uefi::DeviceError;
        }
        if (g_fault == 2U)
        {
            g_mode.FrameBufferBase = 1U;
        }
        if (g_fault == 3U)
        {
            g_mode.FrameBufferSize = 1U;
        }
        if (g_fault == 4U)
        {
            g_mode.Information = &g_modes[3];
        }
    }
    else
    {
        g_mode.FrameBufferBase = reinterpret_cast<Asteria::Foundation::UInt64>(g_pixels);
        g_mode.FrameBufferSize = sizeof(g_pixels);
    }
    return Success;
}
Status ASTERIA_UEFI_API Blt(Uefi::GraphicsOutputProtocol*, Uefi::GraphicsOutputBltPixel* pixels,
                            Uefi::GraphicsOutputBltOperation operation, Asteria::Foundation::Size,
                            Asteria::Foundation::Size, Asteria::Foundation::Size, Asteria::Foundation::Size,
                            Asteria::Foundation::Size, Asteria::Foundation::Size, Asteria::Foundation::Size)
{
    if (operation != Uefi::GraphicsOutputBltOperation::BufferToVideo)
    {
        return Uefi::DeviceError;
    }
    g_pixels[0U] = pixels[0U].Blue + 1U;
    return Success;
}
void Reset()
{
    Uefi::ConfigureBootConsole(nullptr);
    g_handleCount = 1U;
    g_skipFirstEdid = false;
    g_setCount = 0U;
    g_queries = 0U;
    g_frees = 0U;
    g_fault = 0U;
    g_active = true;
    g_discovered = true;
    g_available = true;
    g_transcript[0] = '\0';
    g_modes[0] = {0U, 640U, 480U, 1U, 0U, 0U, 0U, 0U, 640U};
    g_modes[1] = {0U, 1024U, 768U, 0U, 0U, 0U, 0U, 0U, 1024U};
    g_modes[2] = {0U, 1280U, 800U, 1U, 0U, 0U, 0U, 0U, 1280U};
    g_modes[3] = {0U, 1366U, 768U, 2U, 0U, 0U, 0U, 0U, 1368U};
    g_modes[4] = {0U, 1366U, 768U, 1U, 0U, 0U, 0U, 0U, 1368U};
    g_mode = {5U,
              0U,
              &g_modes[0],
              sizeof(g_modes[0]),
              reinterpret_cast<Asteria::Foundation::UInt64>(g_pixels),
              sizeof(g_pixels)};
    g_graphics = {Query, Set, Blt, &g_mode};
    g_handles[0] = &g_graphics;
    g_handles[1] = &g_mode;
    Asteria::TestSupport::MakeEdid(g_edid);
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"GraphicsModeChoiceTests"};
    Uefi::BootServices services{};
    services.LocateHandleBufferFunction = Locate;
    services.HandleProtocolFunction = Protocol;
    services.FreePoolFunction = Free;
    Uefi::SimpleTextOutputProtocol console{};
    console.OutputString = Output;
    Uefi::SystemTable system{};
    system.BootServicesTable = &services;
    system.ConsoleOut = &console;
    Reset();
    ASTERIA_EXPECT(context, "native exact, pooled query data freed",
                   Uefi::ChooseGraphicsMode(system, {}) == &g_graphics, g_mode.Mode == 4U, g_setCount == 1U,
                   g_queries == 5U, g_frees == 6U, g_transcript[0U] == '\0');
    Uefi::AnnounceGraphicsMode(system, &g_graphics);
    ASTERIA_EXPECT(context, "console renders after switch",
                   Uefi::WriteBootConsoleAscii(console, "failure in 30 seconds\r\n"), g_pixels[0] != 0U);
    Reset();
    g_handleCount = 2U;
    g_skipFirstEdid = true;
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
    ASTERIA_EXPECT(context, "physical GOP with EDID preferred to aggregate GOP", g_mode.Mode == 4U);
    char report[4096]{};
    Asteria::Inventory::ReportText text{report, sizeof(report) - 1U};
    Uefi::FormatGraphicsModeChoice(text);
    ASTERIA_EXPECT(context, "report includes policy, EDID identity/checksum, bounded modes and final choice",
                   std::strstr(report, "Display policy: native") != nullptr,
                   std::strstr(report, "checksum=OK") != nullptr,
                   std::strstr(report, "manufacturer=AST product=0x1234") != nullptr,
                   std::strstr(report, "preferred=1366x768") != nullptr,
                   std::strstr(report, "Chosen mode: 4 1366x768 usable") != nullptr);
    Reset();
    g_available = false;
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
    ASTERIA_EXPECT(context, "largest bounded mode respects both EDID axes", g_mode.Mode == 1U, g_setCount == 1U);
    Reset();
    g_active = false;
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
    ASTERIA_EXPECT(context, "discovered EDID fallback", g_mode.Mode == 4U);
    Reset();
    g_active = false;
    g_discovered = false;
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
    ASTERIA_EXPECT(context, "no EDID keeps original", g_mode.Mode == 0U, g_setCount == 0U);
    Reset();
    g_edid[127U] ^= 1U;
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
    ASTERIA_EXPECT(context, "bad checksum keeps original", g_mode.Mode == 0U, g_setCount == 0U);
    Reset();
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {Asteria::Boot::DisplayPolicyKind::Keep, 0U, 0U}));
    ASTERIA_EXPECT(context, "keep never sets mode", g_mode.Mode == 0U, g_setCount == 0U);
    Reset();
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {Asteria::Boot::DisplayPolicyKind::Resolution, 1280U, 800U}));
    ASTERIA_EXPECT(context, "explicit mode ignores EDID bound", g_mode.Mode == 2U, g_setCount == 1U);
    Reset();
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {Asteria::Boot::DisplayPolicyKind::Resolution, 640U, 480U}));
    ASTERIA_EXPECT(context, "same mode has no SetMode", g_mode.Mode == 0U, g_setCount == 0U);
    Reset();
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {Asteria::Boot::DisplayPolicyKind::Resolution, 999U, 999U}));
    ASTERIA_EXPECT(context, "missing explicit mode keeps original", g_mode.Mode == 0U, g_setCount == 0U);
    for (UInt32 fault{1U}; fault <= 4U; ++fault)
    {
        Reset();
        g_fault = fault;
        static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
        ASTERIA_EXPECT(context, "failed or unusable result restored", g_mode.Mode == 0U, g_setCount == 2U,
                       Uefi::IsScanoutModeUsable(&g_mode));
    }
    Reset();
    g_mode.Mode = 4U;
    g_mode.Information = &g_modes[4U];
    g_modes[1U] = g_modes[4U];
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
    ASTERIA_EXPECT(context, "duplicate native resolution keeps current index", g_mode.Mode == 4U, g_setCount == 0U);
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {Asteria::Boot::DisplayPolicyKind::Resolution, 1366U, 768U}));
    ASTERIA_EXPECT(context, "duplicate explicit resolution keeps current index", g_mode.Mode == 4U, g_setCount == 0U);
    Reset();
    g_edid[19U] = 3U;
    g_edid[24U] = 0U;
    Asteria::TestSupport::FixEdidChecksum(g_edid);
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {}));
    char unflagged[4096]{};
    Asteria::Inventory::ReportText unflaggedText{unflagged, sizeof(unflagged) - 1U};
    Uefi::FormatGraphicsModeChoice(unflaggedText);
    ASTERIA_EXPECT(context, "EDID 1.3 unflagged first timing is used and reported", g_mode.Mode == 4U,
                   std::strstr(unflagged, "using first timing without preferred flag") != nullptr);
    Uefi::RestoreOriginalGraphicsMode();
    ASTERIA_EXPECT(context, "failure return restores original and disables console", g_mode.Mode == 0U,
                   g_setCount == 2U, !Uefi::IsBootConsoleConfigured());
    Uefi::RestoreOriginalGraphicsMode();
    ASTERIA_EXPECT(context, "restoration does not switch an already original mode", g_setCount == 2U);
    Reset();
    g_mode.InformationSize = 1U;
    Uefi::AnnounceGraphicsMode(system, &g_graphics);
    ASTERIA_EXPECT(context, "short current mode information is never announced", g_transcript[0U] == '\0');
    Reset();
    g_mode.MaximumMode = 1000U;
    static_cast<void>(Uefi::ChooseGraphicsMode(system, {Asteria::Boot::DisplayPolicyKind::Keep, 0U, 0U}));
    ASTERIA_EXPECT(context, "bounded enumeration", g_queries == Uefi::MaximumReportedGraphicsModes);
    g_modes[0].PixelsPerScanLine = 0xFFFF'FFFFU;
    g_modes[0].VerticalResolution = 0xFFFF'FFFFU;
    ASTERIA_EXPECT(context, "hostile geometry rejected without multiplication overflow",
                   !Uefi::IsScanoutInformationUsable(g_modes[0]));
    Uefi::ConfigureBootConsole(nullptr);
    return context.Finish();
}
