// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_DIAGNOSTIC_WRITER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_DIAGNOSTIC_WRITER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/Ia64DebugUart.hpp"

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
/// Native UART leaf in Diagnostic.S. BSP physical-mode callers need no
/// exclusion before AP admission; ordinary kernel writers use the wrapper.
/// transmitAddress and lineStatusAddress are the physical addresses of the
/// UART's transmit holding and line status registers (Ia64DebugUart); the
/// leaf accesses them through the uncacheable alias with PSR.dt clear.
/// Polls at most maximumReadyPolls times, returning 0 on timeout, 1 on write.
extern "C" Asteria::Foundation::UInt64 AsteriaIa64TryWriteDiagnosticCharacterPhysical(
    Asteria::Foundation::UInt64 value, Asteria::Foundation::UInt64 maximumReadyPolls,
    Asteria::Foundation::UInt64 transmitAddress, Asteria::Foundation::UInt64 lineStatusAddress) noexcept;
#endif

namespace Asteria::Platform::Firmware
{
/// The diagnostic writer's state, the extern "C" object
/// AsteriaIa64DiagnosticWriter at an identity address: the writers'
/// exclusion word and the UART they use (zero addresses: none, output is
/// dropped). Proven is 1 once the UART accepted a byte. Fields other than
/// Exclusion change only under the exclusion or before any AP runs.
struct Ia64DiagnosticWriterState
{
    Foundation::UInt64 Exclusion;
    Foundation::UInt64 TransmitAddress;
    Foundation::UInt64 LineStatusAddress;
    Foundation::UInt64 Proven;
};

/// Directs every later diagnostic of this image to uart (both addresses
/// zero: drop them). The boot's decision (BootInfo::DebugPort): the loader
/// installs it once it read the firmware tables, the kernel when it imports
/// BootInfo. BSP only, with no other processor running and no writer active.
void InstallIa64DiagnosticUart(const Ia64::Firmware::Ia64DebugUart& uart) noexcept;

/// One attempt at exclusion shared with ordinary diagnostic writers.
/// Caller runs at CPL0 in terminal normal context with translation enabled,
/// on a validated stack/RBS. It never resumes the interrupt or fault.
[[nodiscard]] bool TryBeginIa64TerminalDiagnostic() noexcept;
/// Same physical UART contract as Diagnostic.hpp; requires successful begin.
void WriteIa64TerminalDiagnosticAscii(const char* text) noexcept;
void EndIa64TerminalDiagnostic() noexcept;
}

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_DIAGNOSTIC_WRITER_HPP_INCLUDED
