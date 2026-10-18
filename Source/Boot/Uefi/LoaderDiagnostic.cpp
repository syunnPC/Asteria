// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/LoaderDiagnostic.hpp"

#if defined(ASTERIA_TARGET_AMD64)
#include "Platform/Amd64/Firmware/DiagnosticSinkSelection.hpp"
#elif defined(ASTERIA_TARGET_IA64)
#include "Boot/Acpi/DebugPortTables.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Ia64/Firmware/Ia64DebugUart.hpp"
#include "Platform/Ia64/Firmware/Ia64DiagnosticWriter.hpp"
#endif

namespace Asteria::Boot::Uefi
{
namespace
{
#if defined(ASTERIA_TARGET_AMD64)
constinit BootDiagnosticSinks g_sinks{};
constinit bool g_selected{false};
#elif defined(ASTERIA_TARGET_IA64)
// Until SelectLoaderDebugPort runs, diagnostics use the platform debug UART.
constinit BootDebugPort g_debugPort{};
#endif
} // namespace

void SelectLoaderDiagnosticSinks(const void* const rsdp) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    if (!g_selected)
    {
        Platform::Amd64::Firmware::CaptureDiagnosticSinks(rsdp, g_sinks);
        g_selected = true;
    }
#else
    static_cast<void>(rsdp);
#endif
}

BootDiagnosticSinks GetLoaderDiagnosticSinks() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    return g_sinks;
#else
    return {};
#endif
}

void SelectLoaderDebugPort(const void* const rsdp, const void* const hcdpConfigurationTable,
                           const Foundation::UInt64 portSpaceBase, const Foundation::UInt64 portSpaceBytes) noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    const BootDebugPort port = Acpi::SelectDebugPort(Acpi::LocateDebugPortTables(rsdp, hcdpConfigurationTable),
                                                     Platform::Ia64::Firmware::Ia64PlatformDefaultDebugPort);
    const Platform::Ia64::Firmware::Ia64DebugUart uart = Platform::Ia64::Firmware::ResolveIa64DebugUart(
        port, Platform::Ia64::Firmware::Ia64PortSpaceWindow{portSpaceBase, portSpaceBytes});
    // A port this platform cannot reach (an I/O port without a port space
    // window) is reported as none, so the kernel drops its output too.
    g_debugPort = uart.TransmitAddress != 0U ? port : BootDebugPort{};
    Platform::Firmware::InstallIa64DiagnosticUart(uart);
#else
    static_cast<void>(rsdp);
    static_cast<void>(hcdpConfigurationTable);
    static_cast<void>(portSpaceBase);
    static_cast<void>(portSpaceBytes);
#endif
}

BootDebugPort GetLoaderDebugPort() noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    return g_debugPort;
#else
    return {};
#endif
}

void WriteLoaderDiagnosticAscii(const char* const text) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    SelectLoaderDiagnosticSinks(nullptr);
    for (const char* cursor = text; *cursor != '\0'; ++cursor)
    {
        Platform::Amd64::Firmware::WriteDiagnosticByteToPorts(static_cast<Foundation::UInt8>(g_sinks.DebugPortPresent),
                                                              g_sinks.SerialPort,
                                                              static_cast<Foundation::UInt8>(*cursor));
    }
#elif defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    // The image's own diagnostic writer: the selected UART, its bounded
    // transmit wait and its policy for a UART that never answers.
    Platform::Firmware::WriteDiagnosticAscii(text);
#else
    static_cast<void>(text);
#endif
}

void WriteLoaderMirroredAscii(const char* const text) noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    if (IsFirmwareConsole(g_debugPort))
    {
        return;
    }
#endif
    WriteLoaderDiagnosticAscii(text);
}
} // namespace Asteria::Boot::Uefi
