// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_HPP_INCLUDED
#define ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Firmware
{
/// Writes a null-terminated ASCII diagnostic string to the target's early debug channel.
/// Preconditions:
/// - text is non-null and points to a null-terminated ASCII string.
#if defined(ASTERIA_TARGET_IA64)
/// IA-64 target preconditions:
/// - the caller executes at CPL0 outside an ISR;
/// - entry PSR.vm is zero;
/// - text remains readable and stable for the complete call; this primitive
/// neither snapshots the string nor makes concurrent string updates coherent;
/// - if entry PSR.dt is zero, text, the current memory stack, and any RSE
/// backing store reachable under entry ar.rsc are valid physical addresses;
/// - concurrent diagnostic writers are externally excluded.
/// IA-64 processor state:
/// - entry PSR.dt (clear or set), PSR.i, and the complete ar.rsc value (lazy
/// or eager) are restored exactly;
/// - the temporary ar.rsc value preserves every entry field except mode[1:0],
/// which is forced to zero.
/// During each native UART byte transaction, the RSE is synchronized and its
/// mode is forced lazy before the uncacheable physical interval starts. If the
/// caller enters with PSR.dt clear, compiler-generated memory-stack activity
/// may occur outside that interval, and caller-established eager RSE activity
/// may occur before it. Eager RSE activity may resume only after the UART
/// transaction completes, when the exact entry ar.rsc value is restored.
#endif
/// Allocation:
/// - Does not allocate.
void WriteDiagnosticAscii(const char* text) noexcept;

/// Writes `sizeBytes` bytes to the early debug channel with the same
/// preconditions and processor-state guarantees as WriteDiagnosticAscii; the
/// bytes need no terminator.
void WriteDiagnosticBytes(const Foundation::UInt8* bytes, Foundation::Size sizeBytes) noexcept;

/// Emits CRLF, USER: the bounded bytes and CRLF under one acquisition of the ordinary writer
/// exclusion. A busy writer drops the complete record; a byte failure abandons its remaining
/// segments.
void WriteDiagnosticUserLine(const Foundation::UInt8* bytes, Foundation::Size sizeBytes) noexcept;

/// Disables maskable interrupts and permanently stops post-firmware execution.
/// Firmware callbacks:
/// - Does not call firmware.
/// Returns:
/// - Never returns.
[[noreturn]] void HaltAfterFirmwareExit() noexcept;
} // Namespace Asteria::Platform::Firmware

#endif // ASTERIA_PLATFORM_FIRMWARE_DIAGNOSTIC_HPP_INCLUDED
