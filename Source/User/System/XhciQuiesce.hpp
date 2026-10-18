// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_XHCI_QUIESCE_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_XHCI_QUIESCE_HPP_INCLUDED

#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::System
{
enum class XhciQuietPhase : Foundation::UInt8
{
    Invalid,
    Halting,
    Resetting,
    Quiet,
    WaitingReady,
};
struct XhciQuiesceState
{
    Foundation::UInt32 Operational{0U};
    Foundation::UInt32 BarBytes{0U};
    XhciQuietPhase Phase{XhciQuietPhase::Invalid};
};
/// Read-only ownership check over the bounded extended-capability chain.
/// A failed child handoff cannot authorize a manager reset against the BIOS.
[[nodiscard]] inline bool XhciFirmwareReleased(volatile Foundation::UInt32* registers,
                                               const Foundation::UInt32 barBytes) noexcept
{
    if (barBytes < 32U)
    {
        return false;
    }
    Foundation::UInt32 offset = (ReadRegister(registers, 4U) >> 16U) * 4U;
    for (Foundation::Size count{0U}; (count < 64U) && (offset != 0U); ++count)
    {
        if ((offset < 32U) || (offset > barBytes - 4U))
        {
            return false;
        }
        const Foundation::UInt32 header = ReadRegister(registers, offset / 4U);
        if (header == 0xFFFF'FFFFU)
        {
            return false;
        }
        const bool legacy = (header & 0xFFU) == 1U;
        const Foundation::UInt32 next = ((header >> 8U) & 0xFFU) * 4U;
        if (legacy && ((offset > barBytes - 8U) || ((header & 0x10000U) != 0U) || ((next != 0U) && (next < 8U))))
        {
            return false;
        }
        if (next == 0U)
        {
            return true;
        }
        if (next > barBytes - offset)
        {
            return false;
        }
        offset += next;
    }
    return offset == 0U;
}
/// After domain drain, DMA translations and MSI are revoked. Mask controller
/// interrupts and clear Run/Stop, then wait for HCHalted before HCRST. Quiet
/// requires HCRST and Controller Not Ready clear and HCHalted still set
/// (xHCI sections 4.2 and 5.4). The supervisor supplies the finite poll budget;
/// an invalid mapping or timeout retains quarantine and every parked DMA pin.
[[nodiscard]] inline bool BeginXhciQuiesce(volatile Foundation::UInt32* registers, XhciQuiesceState& state,
                                           const Foundation::UInt32 barBytes = 4096U) noexcept
{
    const Foundation::UInt32 cap = ReadRegister(registers, 0U);
    const Foundation::UInt32 offset = cap & 0xFFU;
    const Foundation::UInt32 version = cap >> 16U;
    state.Phase = XhciQuietPhase::Invalid;
    if ((barBytes < 32U) || (offset < 32U) || (offset > barBytes - 8U) || ((offset & 3U) != 0U) ||
        ((version != 0x96U) && (version < 0x100U)) || (version > 0x120U))
    {
        return false;
    }
    if (!XhciFirmwareReleased(registers, barBytes))
    {
        return false;
    }
    state.BarBytes = barBytes;
    state.Operational = offset / 4U;
    const Foundation::UInt32 status = ReadRegister(registers, state.Operational + 1U);
    if (status == 0xFFFF'FFFFU)
    {
        return false;
    }
    if ((status & 0x800U) != 0U)
    {
        state.Phase = XhciQuietPhase::WaitingReady;
        return false;
    }
    const Foundation::UInt32 command = ReadRegister(registers, state.Operational);
    registers[state.Operational] = command & ~5U;
    state.Phase = XhciQuietPhase::Halting;
    return (command & 1U) != 0U;
}
[[nodiscard]] inline bool PollXhciQuiesce(volatile Foundation::UInt32* registers, XhciQuiesceState& state) noexcept
{
    if ((state.Phase == XhciQuietPhase::Invalid) || !XhciFirmwareReleased(registers, state.BarBytes))
    {
        return false;
    }
    const Foundation::UInt32 status = ReadRegister(registers, state.Operational + 1U);
    if (status == 0xFFFF'FFFFU)
    {
        return false;
    }
    if (state.Phase == XhciQuietPhase::WaitingReady)
    {
        if ((status & 0x800U) != 0U)
        {
            return false;
        }
        registers[state.Operational] = ReadRegister(registers, state.Operational) & ~5U;
        state.Phase = XhciQuietPhase::Halting;
        return false;
    }
    if (state.Phase == XhciQuietPhase::Halting)
    {
        if (((status & 1U) == 0U) || ((status & 0x800U) != 0U))
        {
            return false;
        }
        registers[state.Operational] = 2U;
        state.Phase = XhciQuietPhase::Resetting;
        return false;
    }
    if (((ReadRegister(registers, state.Operational) & 3U) != 0U) || ((status & 0x801U) != 1U))
    {
        return false;
    }
    OrderDmaStorage();
    state.Phase = XhciQuietPhase::Quiet;
    return true;
}
} // namespace Asteria::User::System
#endif
