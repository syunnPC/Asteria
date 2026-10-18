// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_AHCI_QUIESCE_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_AHCI_QUIESCE_HPP_INCLUDED

#include "User/System/AhciPort.hpp"

namespace Asteria::User::System
{
enum class AhciQuiescePhase : Foundation::UInt8
{
    WaitingForCr,
    WaitingForFr,
    ResetAsserted,
    WaitingForQuiet,
    Quiet,
};

enum class AhciQuiesceProgress : Foundation::UInt8
{
    Running,
    Quiet,
};

/// The ports captured at begin, the next recovery step, whether a port had
/// ST set or a command issued before begin stopped it, and whether a command
/// was issued (a transfer still running: the evidence the Device Manager
/// reports). FRE alone does not mean a transfer was running. Begin replaces
/// any previous state.
struct AhciQuiesceState
{
    Foundation::UInt32 ImplementedPorts{0U};
    AhciQuiescePhase Phase{AhciQuiescePhase::WaitingForCr};
    bool Running{false};
    bool Issued{false};
};

/// After the driver's drain left its function quarantined (Documentation/
/// Source/User/System/AhciPort.hpp, AHCI block I/O ): masks HBA and port interrupts, then stops
/// every implemented command engine (AHCI 1.3.1 section 10.3.2). ST = 0 is not quiescence: QEMU
/// retains in-flight DMA targets even after CR and CI clear. COMRESET drains that I/O into the
/// still-pinned Frames before the Device Manager may confirm DeviceQuiesced and release them. The
/// caller exclusively owns an uncached ABAR mapping covering every PI port (six ports fit in one 4
/// KiB page; all 32 require 0x1100 bytes) and keeps the DMA storage pinned until Quiet and
/// acknowledgement. No clock, allocation or wait; the writer has AhciPort's synchronous MMIO
/// contract.
inline void BeginAhciQuiesce(volatile Foundation::UInt32* const registers, AhciQuiesceState& state,
                             const AhciRegisterWriter writeRegister = WriteAhciRegister) noexcept
{
    state = AhciQuiesceState{
        .ImplementedPorts = ReadRegister(registers, AhciPiWord),
        .Phase = AhciQuiescePhase::WaitingForCr,
        .Running = false,
        .Issued = false,
    };
    writeRegister(registers, AhciGhcWord, ReadRegister(registers, AhciGhcWord) & ~AhciIe);
    for (Foundation::UInt32 port{0U}; port < 32U; ++port)
    {
        if ((state.ImplementedPorts & (1U << port)) == 0U)
        {
            continue;
        }
        const Foundation::Size base = AhciPortWord(port);
        writeRegister(registers, base + AhciPortIeWord, 0U);
        const Foundation::UInt32 command = ReadRegister(registers, base + AhciCmdWord);
        const Foundation::UInt32 issued = ReadRegister(registers, base + AhciCiWord);
        state.Running = state.Running || ((command & AhciSt) != 0U) || (issued != 0U);
        state.Issued = state.Issued || (issued != 0U);
        writeRegister(registers, base + AhciCmdWord, command & ~AhciSt);
    }
}

namespace Detail
{
[[nodiscard]] inline bool AhciQuiesceEnginesClear(const volatile Foundation::UInt32* const registers,
                                                  const AhciQuiesceState& state, const Foundation::UInt32 mask) noexcept
{
    for (Foundation::UInt32 port{0U}; port < 32U; ++port)
    {
        if (((state.ImplementedPorts & (1U << port)) != 0U) &&
            ((ReadRegister(registers, AhciPortWord(port) + AhciCmdWord) & mask) != 0U))
        {
            return false;
        }
    }
    return true;
}

inline void SetAhciQuiesceDet(volatile Foundation::UInt32* const registers, const AhciQuiesceState& state,
                              const Foundation::UInt32 det, const AhciRegisterWriter writeRegister) noexcept
{
    for (Foundation::UInt32 port{0U}; port < 32U; ++port)
    {
        if ((state.ImplementedPorts & (1U << port)) != 0U)
        {
            const Foundation::Size word = AhciPortWord(port) + AhciSctlWord;
            writeRegister(registers, word, (ReadRegister(registers, word) & ~AhciDetMask) | det);
        }
    }
}
} // Namespace Detail

/// Advances through every step that needs no elapsed time: CR clear, FRE
/// stopped, FR clear, then DET = 1. AHCI 1.3.1 section 3.3.11 allows DET
/// changes only with ST clear; sections 10.3.2 and 10.4.2 require stopped
/// engines and a COMRESET pulse of at least 1 ms. The caller may poll once
/// immediately after Begin, but must pause at least 1 ms between polls and
/// bound their number (the Device Manager uses 100 polls, 10 ms apart).
/// DET = 1 and DET = 0 are always in different polls. On QEMU the latter
/// write drains in-flight DMA before returning; Quiet also requires each
/// port to report a device or none, CI zero and CR/FR clear. PxSIG is not
/// required: QEMU defers it until FIS reception starts again. A timeout
/// keeps the quarantine and DMA pins; it never authorizes DeviceQuiesced.
[[nodiscard]] inline AhciQuiesceProgress
PollAhciQuiesce(volatile Foundation::UInt32* const registers, AhciQuiesceState& state,
                const AhciRegisterWriter writeRegister = WriteAhciRegister) noexcept
{
    switch (state.Phase)
    {
    case AhciQuiescePhase::WaitingForCr:
    {
        if (!Detail::AhciQuiesceEnginesClear(registers, state, AhciSt | AhciCr))
        {
            return AhciQuiesceProgress::Running;
        }
        for (Foundation::UInt32 port{0U}; port < 32U; ++port)
        {
            if ((state.ImplementedPorts & (1U << port)) != 0U)
            {
                const Foundation::Size word = AhciPortWord(port) + AhciCmdWord;
                writeRegister(registers, word, ReadRegister(registers, word) & ~AhciFre);
            }
        }
        state.Phase = AhciQuiescePhase::WaitingForFr;
        [[fallthrough]];
    }
    case AhciQuiescePhase::WaitingForFr:
    {
        if (!Detail::AhciQuiesceEnginesClear(registers, state, AhciSt | AhciCr | AhciFr))
        {
            return AhciQuiesceProgress::Running;
        }
        Detail::SetAhciQuiesceDet(registers, state, 1U, writeRegister);
        state.Phase = AhciQuiescePhase::ResetAsserted;
        return AhciQuiesceProgress::Running;
    }
    case AhciQuiescePhase::ResetAsserted:
    {
        Detail::SetAhciQuiesceDet(registers, state, 0U, writeRegister);
        state.Phase = AhciQuiescePhase::WaitingForQuiet;
        [[fallthrough]];
    }
    case AhciQuiescePhase::WaitingForQuiet:
    {
        for (Foundation::UInt32 port{0U}; port < 32U; ++port)
        {
            if ((state.ImplementedPorts & (1U << port)) == 0U)
            {
                continue;
            }
            const Foundation::Size base = AhciPortWord(port);
            const Foundation::UInt32 det = ReadRegister(registers, base + AhciSstsWord) & AhciDetMask;
            if (((det != AhciDevicePresent) && (det != 0U)) || (ReadRegister(registers, base + AhciCiWord) != 0U) ||
                ((ReadRegister(registers, base + AhciCmdWord) & (AhciCr | AhciFr)) != 0U))
            {
                return AhciQuiesceProgress::Running;
            }
        }
        state.Phase = AhciQuiescePhase::Quiet;
        return AhciQuiesceProgress::Quiet;
    }
    case AhciQuiescePhase::Quiet:
    {
        return AhciQuiesceProgress::Quiet;
    }
    }
    return AhciQuiesceProgress::Running;
}

/// After Quiet, acknowledges every captured port's SERR and IS with ones, then the HBA IS it reads:
/// the HBA status acknowledged (AHCI 1.3.1 sections 3.1.3, 3.3.5 and 3.3.12; AHCI block I/O ).
/// GHC.IE and PxIE stay clear; the next driver instance enables the interrupts it needs.
[[nodiscard]] inline Foundation::UInt32
AcknowledgeAhci(volatile Foundation::UInt32* const registers, const AhciQuiesceState& state,
                const AhciRegisterWriter writeRegister = WriteAhciRegister) noexcept
{
    for (Foundation::UInt32 port{0U}; port < 32U; ++port)
    {
        if ((state.ImplementedPorts & (1U << port)) != 0U)
        {
            const Foundation::Size base = AhciPortWord(port);
            writeRegister(registers, base + AhciSerrWord, 0xFFFFFFFFU);
            writeRegister(registers, base + AhciPortIsWord, 0xFFFFFFFFU);
        }
    }
    const Foundation::UInt32 found = ReadRegister(registers, AhciIsWord);
    writeRegister(registers, AhciIsWord, found);
    return found;
}
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_AHCI_QUIESCE_HPP_INCLUDED
