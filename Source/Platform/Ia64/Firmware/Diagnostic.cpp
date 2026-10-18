// SPDX-License-Identifier: Apache-2.0

#include "Platform/Firmware/Diagnostic.hpp"

#include "Foundation/Types.hpp"
#include "Platform/Firmware/DiagnosticUserRecord.hpp"
#include "Platform/Ia64/Firmware/Ia64DiagnosticWriter.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

// The writer state has an identity address in firmware and in the locked
// kernel RW segment. Materialize it without gp: diagnostics also run before
// the firmware has installed the kernel's global pointer.
extern "C"
{
constinit Asteria::Platform::Firmware::Ia64DiagnosticWriterState AsteriaIa64DiagnosticWriter{
    .Exclusion = 0U,
    .TransmitAddress = Asteria::Platform::Ia64::Firmware::Ia64PlatformDefaultDebugUart.TransmitAddress,
    .LineStatusAddress = Asteria::Platform::Ia64::Firmware::Ia64PlatformDefaultDebugUart.LineStatusAddress,
    .Proven = 0U,
};
}

namespace Asteria::Platform::Firmware
{
namespace
{
[[nodiscard]] Ia64DiagnosticWriterState& WriterState() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    Foundation::UInt64 address{0U};
    __asm__ volatile("movl %0 = AsteriaIa64DiagnosticWriter" : "=r"(address));
    return *reinterpret_cast<Ia64DiagnosticWriterState*>(address);
#else
    return AsteriaIa64DiagnosticWriter;
#endif
}
constexpr Foundation::UInt8 TransmitHoldingRegisterEmpty{0x20U};
constexpr Foundation::Size MaximumReadyPolls{1'000'000U};

#if !defined(ASTERIA_TARGET_IA64) || !defined(__ia64__)
void PauseProcessor() noexcept
{
    __asm__ volatile("" ::: "memory");
}
#endif

// One bounded transmit. The caller holds the writer exclusion. A UART that
// times out before it ever accepted a byte is absent (a table naming a port
// the machine lacks, or no table on a machine without the platform default):
// it is dropped for good, so no later diagnostic waits on it. Once a byte got
// through, a timeout drops only the rest of that message; a busy host may
// stall a working UART for a whole wait.
[[nodiscard]] bool TryWriteCharacter(const Foundation::UInt8 value) noexcept
{
    Ia64DiagnosticWriterState& state = WriterState();
    if (state.TransmitAddress == 0U)
    {
        return false;
    }
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    const bool written = AsteriaIa64TryWriteDiagnosticCharacterPhysical(value, MaximumReadyPolls, state.TransmitAddress,
                                                                        state.LineStatusAddress) != 0U;
#else
    auto* const transmit = reinterpret_cast<volatile Foundation::UInt8*>(state.TransmitAddress);
    auto* const lineStatus = reinterpret_cast<volatile Foundation::UInt8*>(state.LineStatusAddress);
    bool written{false};
    for (Foundation::Size attempt{0U}; !written && (attempt < MaximumReadyPolls); ++attempt)
    {
        if ((*lineStatus & TransmitHoldingRegisterEmpty) != 0U)
        {
            *transmit = value;
            written = true;
        }
        else
        {
            PauseProcessor();
        }
    }
#endif
    if (written)
    {
        state.Proven = 1U;
        return true;
    }
    if (state.Proven == 0U)
    {
        state.TransmitAddress = 0U;
        state.LineStatusAddress = 0U;
    }
    return false;
}
} // namespace

void InstallIa64DiagnosticUart(const Ia64::Firmware::Ia64DebugUart& uart) noexcept
{
    Ia64DiagnosticWriterState& state = WriterState();
    const bool present = (uart.TransmitAddress != 0U) && (uart.LineStatusAddress != 0U);
    state.TransmitAddress = present ? uart.TransmitAddress : 0U;
    state.LineStatusAddress = present ? uart.LineStatusAddress : 0U;
    state.Proven = 0U;
}

void WriteIa64TerminalDiagnosticAscii(const char* const text) noexcept
{
    const char* cursor = text;
    while (*cursor != '\0')
    {
        if (!TryWriteCharacter(static_cast<Foundation::UInt8>(*cursor)))
        {
            return;
        }
        ++cursor;
    }
}

bool TryBeginIa64TerminalDiagnostic() noexcept
{
    Foundation::UInt64 expected{0U};
    return Synchronization::AtomicUInt64Reference{WriterState().Exclusion}.CompareExchangeStrongAcquireRelease(expected,
                                                                                                               1U);
}
void EndIa64TerminalDiagnostic() noexcept
{
    Synchronization::AtomicUInt64Reference{WriterState().Exclusion}.StoreRelease(0U);
}
void WriteDiagnosticAscii(const char* const text) noexcept
{
    if (!TryBeginIa64TerminalDiagnostic())
    {
        return;
    }
    WriteIa64TerminalDiagnosticAscii(text);
    EndIa64TerminalDiagnostic();
}

void WriteDiagnosticBytes(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    if (!TryBeginIa64TerminalDiagnostic())
    {
        return;
    }
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (!TryWriteCharacter(bytes[index]))
        {
            EndIa64TerminalDiagnostic();
            return;
        }
    }
    EndIa64TerminalDiagnostic();
}

void WriteDiagnosticUserLine(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    if (!TryBeginIa64TerminalDiagnostic())
    {
        return;
    }
    WriteDiagnosticUserRecord(TryWriteCharacter, bytes, sizeBytes);
    EndIa64TerminalDiagnostic();
}

[[noreturn]] void HaltAfterFirmwareExit() noexcept
{
#if defined(__ia64__)
    __asm__ volatile("rsm 0x4000;;\n\t"
                     "srlz.d;;\n\t"
                     "1:\n\t"
                     "hint @pause;;\n\t"
                     "br.cond.sptk 1b;;"
                     :
                     :
                     : "memory");
#else
    __builtin_trap();
#endif
    __builtin_unreachable();
}
} // namespace Asteria::Platform::Firmware
