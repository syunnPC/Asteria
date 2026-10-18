// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/GraphicsModeChoice.hpp"

#include "Boot/Edid.hpp"
#include "Boot/Uefi/BootConsole.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
// UEFI 2.9A sections 12.9 and 12.10. Only protocol layouts and identifiers
// are taken from the specification; this selection implementation is original.
constexpr Guid GraphicsGuid{0x9042A9DEU, 0x23DCU, 0x4A38U, {0x96U, 0xFBU, 0x7AU, 0xDEU, 0xD0U, 0x80U, 0x51U, 0x6AU}};
constexpr Guid ActiveGuid{0xBD8C1056U, 0x9F36U, 0x44ECU, {0x92U, 0xA8U, 0xA6U, 0x33U, 0x7FU, 0x81U, 0x79U, 0x86U}};
constexpr Guid DiscoveredGuid{0x1C0C34F6U, 0xD380U, 0x41FAU, {0xA0U, 0x49U, 0x8AU, 0xD0U, 0x6CU, 0x1AU, 0x66U, 0xAAU}};
constinit GraphicsOutputProtocol* g_originalGraphics{nullptr};
constinit Foundation::UInt32 g_originalMode{0U};
constinit char g_choiceReport[24U * 1024U]{};

bool HasEdid(BootServices& services, const Handle handle) noexcept
{
    Guid guid{ActiveGuid};
    for (Foundation::Size attempt{0U}; attempt < 2U; ++attempt)
    {
        void* interface{nullptr};
        if (services.HandleProtocolFunction(handle, &guid, &interface) == Success && interface != nullptr)
        {
            const auto& edid = *static_cast<const EdidProtocol*>(interface);
            if (edid.SizeOfEdid != 0U && edid.Edid != nullptr)
            {
                return true;
            }
        }
        guid = DiscoveredGuid;
    }
    return false;
}

GraphicsOutputProtocol* LocateGraphics(BootServices& services, Handle& handle, Inventory::ReportText& text) noexcept
{
    Guid guid{GraphicsGuid};
    Handle* handles{nullptr};
    Foundation::Size count{0U};
    GraphicsOutputProtocol* graphics{nullptr};
    if (services.LocateHandleBufferFunction != nullptr && services.HandleProtocolFunction != nullptr &&
        services.FreePoolFunction != nullptr &&
        services.LocateHandleBufferFunction(LocateSearchType::ByProtocol, &guid, nullptr, &count, &handles) ==
            Success &&
        handles != nullptr)
    {
        for (Foundation::Size index{0U}; index < count && index < 256U; ++index)
        {
            void* interface{nullptr};
            if (services.HandleProtocolFunction(handles[index], &guid, &interface) == Success && interface != nullptr)
            {
                // Firmware may expose an aggregate console GOP as well as
                // a physical output. Prefer a GOP handle that carries EDID.
                const bool hasEdid{HasEdid(services, handles[index])};
                if (graphics == nullptr || hasEdid)
                {
                    graphics = static_cast<GraphicsOutputProtocol*>(interface);
                    handle = handles[index];
                }
                if (hasEdid)
                {
                    break;
                }
            }
        }
        text.Text("GOP handles: ");
        text.Decimal(count);
        text.Text(" (search limit 256)\r\n");
        static_cast<void>(services.FreePoolFunction(handles));
    }
    if (graphics == nullptr && services.LocateProtocolFunction != nullptr)
    {
        void* interface{nullptr};
        if (services.LocateProtocolFunction(&guid, nullptr, &interface) == Success)
        {
            graphics = static_cast<GraphicsOutputProtocol*>(interface);
        }
    }
    return graphics;
}

EdidError ReadEdid(BootServices& services, const Handle handle, EdidTiming& timing,
                   Inventory::ReportText& text) noexcept
{
    void* interface{nullptr};
    Guid guid{ActiveGuid};
    const char* source{"active"};
    if (handle == nullptr || services.HandleProtocolFunction == nullptr)
    {
        text.Text("EDID: absent (no GOP handle)\r\n");
        return EdidError::Absent;
    }
    if (services.HandleProtocolFunction(handle, &guid, &interface) != Success || interface == nullptr ||
        static_cast<EdidProtocol*>(interface)->SizeOfEdid == 0U ||
        static_cast<EdidProtocol*>(interface)->Edid == nullptr)
    {
        guid = DiscoveredGuid;
        interface = nullptr;
        source = "discovered";
        if (services.HandleProtocolFunction(handle, &guid, &interface) != Success)
        {
            interface = nullptr;
        }
    }
    const auto* const edid = static_cast<const EdidProtocol*>(interface);
    const EdidError error =
        ParseEdid(edid == nullptr ? nullptr : edid->Edid, edid == nullptr ? 0U : edid->SizeOfEdid, timing);
    text.Text("EDID: ");
    text.Text(source);
    text.Text(" checksum=");
    if (edid != nullptr && edid->Edid != nullptr && edid->SizeOfEdid >= 128U)
    {
        Foundation::UInt32 sum{0U};
        for (Foundation::Size index{0U}; index < 128U; ++index)
        {
            sum += edid->Edid[index];
        }
        text.Text((sum & 255U) == 0U ? "OK" : "BAD");
    }
    else
    {
        text.Text("unavailable");
    }
    text.Text(" verdict=");
    text.Text(EdidReason(error));
    if (error == EdidError::None)
    {
        text.Text(" manufacturer=");
        text.Text(timing.Manufacturer);
        text.Text(" product=0x");
        text.Hex(timing.Product, 4U);
        text.Text(" preferred=");
        text.Decimal(timing.Width);
        text.Character('x');
        text.Decimal(timing.Height);
        text.Text(" clock-kHz=");
        text.Decimal(timing.PixelClockKhz);
        text.Text(timing.PreferredTimingFlag ? " preferred-timing-flag=set" : " preferred-timing-flag=clear");
        if (timing.Revision == 3U && !timing.PreferredTimingFlag)
        {
            text.Text(" (EDID 1.3: using first timing without preferred flag)");
        }
    }
    text.Text("\r\n");
    return error;
}

struct Choice
{
    Foundation::UInt32 Index;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
    Foundation::UInt64 Area;
    bool Exact;
};

void Consider(const GraphicsOutputModeInformation& information, const Foundation::UInt32 index,
              const DisplayPolicy& policy, const EdidTiming& preferred, const bool hasPreferred,
              Choice& choice) noexcept
{
    if (policy.Kind == DisplayPolicyKind::Keep || !IsScanoutInformationUsable(information))
    {
        return;
    }
    const Foundation::UInt32 width{policy.Kind == DisplayPolicyKind::Resolution ? policy.Width : preferred.Width};
    const Foundation::UInt32 height{policy.Kind == DisplayPolicyKind::Resolution ? policy.Height : preferred.Height};
    if (policy.Kind == DisplayPolicyKind::Native && !hasPreferred)
    {
        return;
    }
    const bool exact{information.HorizontalResolution == width && information.VerticalResolution == height};
    const Foundation::UInt64 area{Foundation::UInt64{information.HorizontalResolution} *
                                  information.VerticalResolution};
    if ((!choice.Exact && exact) ||
        (!choice.Exact && policy.Kind == DisplayPolicyKind::Native && information.HorizontalResolution <= width &&
         information.VerticalResolution <= height && area > choice.Area))
    {
        choice = {index, information.HorizontalResolution, information.VerticalResolution, area, exact};
    }
}

void ModeLine(Inventory::ReportText& text, const Foundation::UInt32 index,
              const GraphicsOutputModeInformation& information) noexcept
{
    text.Text("  mode=");
    text.Decimal(index);
    text.Character(' ');
    text.Decimal(information.HorizontalResolution);
    text.Character('x');
    text.Decimal(information.VerticalResolution);
    text.Text(" pitch=");
    text.Decimal(information.PixelsPerScanLine);
    text.Text(" format=");
    text.Decimal(information.PixelFormat);
    text.Text(IsScanoutInformationUsable(information) ? " usable\r\n" : " unusable\r\n");
}
} // namespace

bool IsScanoutInformationUsable(const GraphicsOutputModeInformation& information) noexcept
{
    if (information.Version != 0U || information.PixelFormat > 1U || information.HorizontalResolution == 0U ||
        information.VerticalResolution == 0U || information.PixelsPerScanLine < information.HorizontalResolution)
    {
        return false;
    }
    // Division before multiplication bounds the extent and prevents overflow
    // even when firmware supplies full-width hostile dimensions and strides.
    return information.PixelsPerScanLine <= ScanoutCeilingBytes / 4U / information.VerticalResolution;
}

bool IsScanoutModeUsable(const GraphicsOutputProtocolMode* const mode) noexcept
{
    if (mode == nullptr || mode->Information == nullptr ||
        mode->InformationSize < sizeof(GraphicsOutputModeInformation) || mode->MaximumMode == 0U ||
        mode->Mode >= mode->MaximumMode || !IsScanoutInformationUsable(*mode->Information))
    {
        return false;
    }
    const Foundation::UInt64 needed{Foundation::UInt64{mode->Information->PixelsPerScanLine} *
                                    mode->Information->VerticalResolution * 4U};
    return mode->FrameBufferBase != 0U && (mode->FrameBufferBase & 4095U) == 0U &&
           mode->FrameBufferBase <= 0xFFFF'FFFF'FFFF'FFFFULL - needed && mode->FrameBufferSize >= needed;
}

GraphicsOutputProtocol* ChooseGraphicsMode(SystemTable& system, const DisplayPolicy& policy) noexcept
{
    g_originalGraphics = nullptr;
    Inventory::ReportText text{g_choiceReport, sizeof(g_choiceReport) - 1U};
    text.Text("Display policy: ");
    switch (policy.Kind)
    {
    case DisplayPolicyKind::Native:
        text.Text("native");
        break;
    case DisplayPolicyKind::Keep:
        text.Text("keep");
        break;
    case DisplayPolicyKind::Resolution:
        text.Decimal(policy.Width);
        text.Character('x');
        text.Decimal(policy.Height);
        break;
    }
    text.Text("\r\n");
    BootServices& services = *system.BootServicesTable;
    Handle handle{nullptr};
    GraphicsOutputProtocol* const graphics = LocateGraphics(services, handle, text);
    if (graphics == nullptr || graphics->Mode == nullptr || graphics->Mode->Information == nullptr ||
        graphics->Mode->InformationSize < sizeof(GraphicsOutputModeInformation))
    {
        text.Text("GOP modes: unavailable\r\nEDID: absent\r\nChosen mode: unavailable\r\n");
        g_choiceReport[text.Bytes()] = '\0';
        return graphics;
    }
    EdidTiming preferred{};
    const bool hasPreferred{ReadEdid(services, handle, preferred, text) == EdidError::None};
    const Foundation::UInt32 original{graphics->Mode->Mode};
    g_originalGraphics = graphics;
    g_originalMode = original;
    Choice choice{original, graphics->Mode->Information->HorizontalResolution,
                  graphics->Mode->Information->VerticalResolution, 0U, false};
    // Seed an exact usable current mode before enumerating duplicates. An
    // equal resolution at another index must not clear an already correct mode.
    Consider(*graphics->Mode->Information, original, policy, preferred, hasPreferred, choice);
    const Foundation::UInt32 maximum{graphics->Mode->MaximumMode < MaximumReportedGraphicsModes
                                         ? graphics->Mode->MaximumMode
                                         : MaximumReportedGraphicsModes};
    text.Text("GOP modes (limit 256):\r\n");
    if (graphics->QueryMode != nullptr && services.FreePoolFunction != nullptr)
    {
        for (Foundation::UInt32 index{0U}; index < maximum; ++index)
        {
            Foundation::Size size{0U};
            GraphicsOutputModeInformation* information{nullptr};
            const Status status{graphics->QueryMode(graphics, index, &size, &information)};
            if (status == Success && information != nullptr && size >= sizeof(GraphicsOutputModeInformation))
            {
                ModeLine(text, index, *information);
                Consider(*information, index, policy, preferred, hasPreferred, choice);
            }
            else
            {
                text.Text("  mode=");
                text.Decimal(index);
                text.Text(" query-failed\r\n");
            }
            if (information != nullptr)
            {
                static_cast<void>(services.FreePoolFunction(information));
            }
        }
    }
    if (maximum < graphics->Mode->MaximumMode)
    {
        text.Text("  mode list truncated\r\n");
    }
    char line[96]{};
    Inventory::ReportText announcement{line, sizeof(line) - 1U};
    announcement.Text("Asteria display choice: ");
    announcement.Decimal(choice.Width);
    announcement.Character('x');
    announcement.Decimal(choice.Height);
    announcement.Text(" mode=");
    announcement.Decimal(choice.Index);
    announcement.Text("\r\n");
    Platform::Firmware::WriteDiagnosticAscii(line);
    // Console text begins after the switch and desktop; retain this intention
    // in the diagnostic channel before calling firmware SetMode.
    if (choice.Index != original && graphics->SetMode != nullptr)
    {
        const Status status{graphics->SetMode(graphics, choice.Index)};
        if (status != Success || !IsScanoutModeUsable(graphics->Mode) || graphics->Mode->Mode != choice.Index ||
            graphics->Mode->Information->HorizontalResolution != choice.Width ||
            graphics->Mode->Information->VerticalResolution != choice.Height)
        {
            text.Text("SetMode rejected; restore-original status=0x");
            text.Hex(graphics->SetMode(graphics, original), 16U);
            text.Text("\r\n");
        }
        else
        {
            text.Text("SetMode: OK\r\n");
        }
        // Use our text renderer after any firmware switch attempt. ConOut's
        // cached geometry can disagree with GOP, and text SetMode may switch
        // GOP back; do not ask ConOut to reconfigure the graphics hardware.
        ConfigureBootConsole(graphics);
    }
    if (choice.Index != original && graphics->SetMode == nullptr)
    {
        text.Text("SetMode unavailable; retained current mode\r\n");
    }
    text.Text("Chosen mode: ");
    if (graphics->Mode != nullptr && graphics->Mode->Information != nullptr &&
        graphics->Mode->InformationSize >= sizeof(GraphicsOutputModeInformation))
    {
        text.Decimal(graphics->Mode->Mode);
        text.Character(' ');
        text.Decimal(graphics->Mode->Information->HorizontalResolution);
        text.Character('x');
        text.Decimal(graphics->Mode->Information->VerticalResolution);
        text.Text(IsScanoutModeUsable(graphics->Mode) ? " usable\r\n" : " unusable\r\n");
    }
    else
    {
        text.Text("unavailable\r\n");
    }
    g_choiceReport[text.Bytes()] = '\0';
    return graphics;
}

void RestoreOriginalGraphicsMode() noexcept
{
    if (g_originalGraphics != nullptr && g_originalGraphics->SetMode != nullptr &&
        (g_originalGraphics->Mode == nullptr || g_originalGraphics->Mode->Mode != g_originalMode))
    {
        static_cast<void>(g_originalGraphics->SetMode(g_originalGraphics, g_originalMode));
    }
    ConfigureBootConsole(nullptr);
}

void AnnounceGraphicsMode(SystemTable& system, const GraphicsOutputProtocol* const graphics) noexcept
{
    if (graphics == nullptr || graphics->Mode == nullptr || graphics->Mode->Information == nullptr ||
        graphics->Mode->InformationSize < sizeof(GraphicsOutputModeInformation))
    {
        return;
    }
    char line[96]{};
    Inventory::ReportText text{line, sizeof(line) - 1U};
    text.Text("Asteria display choice: ");
    text.Decimal(graphics->Mode->Information->HorizontalResolution);
    text.Character('x');
    text.Decimal(graphics->Mode->Information->VerticalResolution);
    text.Text(" mode=");
    text.Decimal(graphics->Mode->Mode);
    text.Text("\r\n");
    static_cast<void>(WriteBootConsoleAscii(*system.ConsoleOut, line));
}

void FormatGraphicsModeChoice(Inventory::ReportText& text) noexcept
{
    text.Text(g_choiceReport);
}
} // namespace Asteria::Boot::Uefi
