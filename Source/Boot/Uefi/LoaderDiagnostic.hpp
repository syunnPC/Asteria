// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_LOADER_DIAGNOSTIC_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_LOADER_DIAGNOSTIC_HPP_INCLUDED

#include "Boot/BootDebugPort.hpp"
#include "Boot/BootKernelInput.hpp"

namespace Asteria::Boot::Uefi
{
/// Loader-owned diagnostic selection. BSP only, before the final map, no
/// allocation. AMD64 selects its debug-port and serial sinks here; on IA-64
/// it does nothing (SelectLoaderDebugPort selects the UART).
void SelectLoaderDiagnosticSinks(const void* rsdp) noexcept;
[[nodiscard]] BootDiagnosticSinks GetLoaderDiagnosticSinks() noexcept;

/// IA-64: selects the diagnostic UART the firmware's debug-port tables name
/// (Boot::Acpi::SelectDebugPort over the tables the RSDP and the DIG64 HCDP
/// configuration table reach, falling back to the platform's debug UART),
/// resolves an I/O port through the firmware's I/O port space window
/// [portSpaceBase, portSpaceBase + portSpaceBytes) and directs the loader's
/// diagnostics, and the image's shared diagnostic writer, to it. BSP only,
/// once, before the first diagnostic and while the tables are identity
/// mapped; no allocation or firmware call. Does nothing on AMD64.
void SelectLoaderDebugPort(const void* rsdp, const void* hcdpConfigurationTable, Foundation::UInt64 portSpaceBase,
                           Foundation::UInt64 portSpaceBytes) noexcept;
/// The selection BootInfo carries to the kernel; zero before selection and
/// on AMD64.
[[nodiscard]] BootDebugPort GetLoaderDebugPort() noexcept;

/// Writes readable, null-terminated ASCII with bounded polling. BSP only;
/// writes loader state and device ports, without calling UEFI or the kernel.
void WriteLoaderDiagnosticAscii(const char* text) noexcept;

/// The same for text the caller also writes to the firmware console: skipped
/// when the diagnostic UART is that console's own port (SPCR, an HCDP
/// console), where the firmware already transmits it, so the channel carries
/// each line once. Callers use it only while boot services run.
void WriteLoaderMirroredAscii(const char* text) noexcept;
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_LOADER_DIAGNOSTIC_HPP_INCLUDED
