// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_USB_HOST_QUIESCE_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_USB_HOST_QUIESCE_HPP_INCLUDED

#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::System
{
/// A USB 1.1/2.0 host controller as the Device Manager reaches it after the drain of its driver :
/// an EHCI's or OHCI's register block mapped uncached at its offset within the BAR's page, a UHCI's
/// 32 I/O ports through PortIo, and the function's configuration dwords (DeviceReadConfiguration
/// with Manage). Serialized in the root service; no allocation.
class IUsbHostQuietAccess
{
public:
    [[nodiscard]] virtual Foundation::UInt32 Read32(Foundation::UInt32 offset) noexcept = 0;
    virtual void Write32(Foundation::UInt32 offset, Foundation::UInt32 value) noexcept = 0;
    [[nodiscard]] virtual Foundation::UInt16 Read16(Foundation::UInt32 offset) noexcept = 0;
    virtual void Write16(Foundation::UInt32 offset, Foundation::UInt16 value) noexcept = 0;
    /// The configuration dword at offset; false when the kernel refused.
    [[nodiscard]] virtual bool ReadConfiguration(Foundation::UInt32 offset, Foundation::UInt32& value) noexcept = 0;

protected:
    ~IUsbHostQuietAccess() = default;
};

enum class UsbHostKind : Foundation::UInt8
{
    None,
    Ehci,
    Ohci,
    Uhci,
};

enum class UsbHostQuietPhase : Foundation::UInt8
{
    /// The registers make no sense (all ones, a revision or layout this
    /// recipe does not know): nothing was written, quarantine stays.
    Invalid,
    /// Firmware still owns the controller (an EHCI's BIOS Owned semaphore,
    /// an OHCI's InterruptRouting, a UHCI's SMI or trap enables): a reset
    /// would fight SMM, so nothing was written and quarantine stays.
    FirmwareOwned,
    /// Interrupts masked and the schedules stopped; waiting for the halt.
    Halting,
    /// Halted; the host-controller reset is in progress.
    Resetting,
    Quiet,
};

struct UsbHostQuiesceState
{
    /// An EHCI's operational register offset (CAPLENGTH).
    Foundation::UInt32 Operational{0U};
    /// The register bytes mapped from the block's start.
    Foundation::UInt32 Bytes{0U};
    /// An EHCI's USB Legacy Support capability in configuration space, zero
    /// for none.
    Foundation::UInt32 Legacy{0U};
    UsbHostKind Kind{UsbHostKind::None};
    UsbHostQuietPhase Phase{UsbHostQuietPhase::Invalid};
};

/// The controller a class dword (configuration offset 0x08) names: PCI class
/// 0C/03 with programming interface 20 (EHCI), 10 (OHCI) or 00 (UHCI).
[[nodiscard]] constexpr UsbHostKind UsbHostKindOf(const Foundation::UInt32 classDword) noexcept
{
    switch (classDword >> 8U)
    {
    case 0x0C'0320U:
        return UsbHostKind::Ehci;
    case 0x0C'0310U:
        return UsbHostKind::Ohci;
    case 0x0C'0300U:
        return UsbHostKind::Uhci;
    default:
        return UsbHostKind::None;
    }
}

namespace Detail
{
// EHCI 1.0 sections 2.3.1 (USBCMD) and 2.3.2 (USBSTS).
inline constexpr Foundation::UInt32 EhciRun{1U};
inline constexpr Foundation::UInt32 EhciReset{2U};
inline constexpr Foundation::UInt32 EhciSchedules{0x30U};
inline constexpr Foundation::UInt32 EhciHalted{1U << 12U};
inline constexpr Foundation::UInt32 EhciScheduleStatus{0xC000U};
// OpenHCI 1.0a sections 7.1.2 (HcControl) and 7.1.3 (HcCommandStatus).
inline constexpr Foundation::UInt32 OhciInterruptRouting{1U << 8U};
inline constexpr Foundation::UInt32 OhciListsAndState{0xFCU};
inline constexpr Foundation::UInt32 OhciOperational{0x80U};
inline constexpr Foundation::UInt32 OhciReset{1U};
// UHCI design guide 1.1 sections 2.1.1 (USBCMD), 2.1.2 (USBSTS) and 5.2.1
// (LEGSUP: bits 7 and 5:0 enable SMIs, traps and pass-through).
inline constexpr Foundation::UInt16 UhciRun{1U};
inline constexpr Foundation::UInt16 UhciReset{2U};
inline constexpr Foundation::UInt16 UhciKeep{0xC0U};
inline constexpr Foundation::UInt16 UhciHalted{0x20U};
inline constexpr Foundation::UInt32 UhciLegacyEnables{0xBFU};

// Finds an EHCI's USB Legacy Support capability on its EECP list (EHCI 1.0
// Section 2.1.7): bounded, a loop or a link below 0x40 is invalid.
[[nodiscard]] inline bool FindEhciLegacy(IUsbHostQuietAccess& access, Foundation::UInt32 next,
                                         Foundation::UInt32& legacy) noexcept
{
    legacy = 0U;
    Foundation::UInt64 visited{0U};
    for (Foundation::Size count{0U}; (next != 0U) && (count < 48U); ++count)
    {
        if ((next < 0x40U) || (next > 0xFCU) || ((next & 3U) != 0U))
        {
            return false;
        }
        const Foundation::UInt64 bit = Foundation::UInt64{1U} << ((next - 0x40U) / 4U);
        Foundation::UInt32 header{0U};
        if (((visited & bit) != 0U) || !access.ReadConfiguration(next, header))
        {
            return false;
        }
        visited |= bit;
        if (((header & 0xFFU) == 1U) && (legacy == 0U))
        {
            legacy = next;
        }
        next = (header >> 8U) & 0xFFU;
    }
    return next == 0U;
}

// Whether firmware has let go of the controller: an EHCI's BIOS Owned
// semaphore clear, an OHCI's InterruptRouting clear, a UHCI's SMI and trap
// enables clear. A refused read counts as owned.
[[nodiscard]] inline bool FirmwareReleased(IUsbHostQuietAccess& access, const UsbHostQuiesceState& state) noexcept
{
    Foundation::UInt32 value{0U};
    switch (state.Kind)
    {
    case UsbHostKind::Ehci:
        return (state.Legacy == 0U) || (access.ReadConfiguration(state.Legacy, value) && ((value & (1U << 16U)) == 0U));
    case UsbHostKind::Ohci:
        value = access.Read32(4U);
        return (value != 0xFFFF'FFFFU) && ((value & OhciInterruptRouting) == 0U);
    case UsbHostKind::Uhci:
        return access.ReadConfiguration(0xC0U, value) && ((value & UhciLegacyEnables) == 0U);
    case UsbHostKind::None:
        return false;
    }
    return false;
}
} // namespace Detail

/// The Device Manager's proof that a drained USB host controller is quiet
/// The drain has turned bus mastering off and revoked the IOMMU mappings; this stops the controller
/// itself, so a reassignment cannot let it fetch a dead schedule through the next instance's
/// mappings. A controller firmware still owns is never touched. EHCI: USBINTR zero, Run/Stop and
/// both schedule enables clear, HCHalted with both schedule statuses clear, then HCRESET (allowed
/// only once halted, EHCI 1.0 section 2.3.1). OHCI: every interrupt disabled, the lists off in
/// UsbReset, then HostControllerReset (OpenHCI 1.0a section 7.1.3), after which it stays out of
/// UsbOperational with its lists off. UHCI: USBINTR zero, Run/Stop clear, HCHalted, then HCRESET
/// (UHCI 1.1 section 2.1.1). Quiet requires the reset finished, Run/Stop clear and the halt (EHCI,
/// UHCI) still reported, and firmware still released. Begin answers whether the controller was
/// running; it writes nothing for an Invalid or FirmwareOwned controller. bytes is the register
/// block's mapped extent (EHCI, OHCI). The caller supplies the finite poll budget; a timeout keeps
/// the quarantine and every parked pin.
[[nodiscard]] inline bool BeginUsbHostQuiesce(IUsbHostQuietAccess& access, const UsbHostKind kind,
                                              const Foundation::UInt32 bytes, UsbHostQuiesceState& state) noexcept
{
    state = UsbHostQuiesceState{
        .Operational = 0U, .Bytes = bytes, .Legacy = 0U, .Kind = kind, .Phase = UsbHostQuietPhase::Invalid};
    switch (kind)
    {
    case UsbHostKind::Ehci:
    {
        const Foundation::UInt32 capability = access.Read32(0U);
        const Foundation::UInt32 operational = capability & 0xFFU;
        const Foundation::UInt32 version = capability >> 16U;
        if ((bytes < 12U) || (capability == 0xFFFF'FFFFU) || (operational < 0x10U) || ((operational & 3U) != 0U) ||
            (operational + 0x44U > bytes) || (version < 0x0095U) || (version > 0x0110U) ||
            !Detail::FindEhciLegacy(access, (access.Read32(8U) >> 8U) & 0xFFU, state.Legacy))
        {
            return false;
        }
        state.Operational = operational;
        if (!Detail::FirmwareReleased(access, state))
        {
            state.Phase = UsbHostQuietPhase::FirmwareOwned;
            return false;
        }
        const Foundation::UInt32 command = access.Read32(operational);
        access.Write32(operational + 8U, 0U);
        access.Write32(operational, command & ~(Detail::EhciRun | Detail::EhciSchedules));
        state.Phase = UsbHostQuietPhase::Halting;
        return (command & (Detail::EhciRun | Detail::EhciSchedules)) != 0U;
    }
    case UsbHostKind::Ohci:
    {
        const Foundation::UInt32 revision = access.Read32(0U);
        if ((bytes < 0x54U) || (revision == 0xFFFF'FFFFU) || ((revision & 0xFFU) != 0x10U))
        {
            return false;
        }
        if (!Detail::FirmwareReleased(access, state))
        {
            state.Phase = UsbHostQuietPhase::FirmwareOwned;
            return false;
        }
        const Foundation::UInt32 control = access.Read32(4U);
        access.Write32(0x14U, 0xFFFF'FFFFU);
        access.Write32(4U, control & ~Detail::OhciListsAndState);
        state.Phase = UsbHostQuietPhase::Halting;
        return (control & Detail::OhciListsAndState) != 0U;
    }
    case UsbHostKind::Uhci:
    {
        const Foundation::UInt16 command = access.Read16(0U);
        if ((command == 0xFFFFU) || (access.Read16(2U) == 0xFFFFU))
        {
            return false;
        }
        if (!Detail::FirmwareReleased(access, state))
        {
            state.Phase = UsbHostQuietPhase::FirmwareOwned;
            return false;
        }
        access.Write16(4U, 0U);
        access.Write16(0U, static_cast<Foundation::UInt16>(command & Detail::UhciKeep));
        state.Phase = UsbHostQuietPhase::Halting;
        return (command & Detail::UhciRun) != 0U;
    }
    case UsbHostKind::None:
        return false;
    }
    return false;
}

[[nodiscard]] inline bool PollUsbHostQuiesce(IUsbHostQuietAccess& access, UsbHostQuiesceState& state) noexcept
{
    if ((state.Phase != UsbHostQuietPhase::Halting) && (state.Phase != UsbHostQuietPhase::Resetting) &&
        (state.Phase != UsbHostQuietPhase::Quiet))
    {
        return false;
    }
    if (!Detail::FirmwareReleased(access, state))
    {
        // Firmware took it back meanwhile: no reset against SMM.
        state.Phase = UsbHostQuietPhase::FirmwareOwned;
        return false;
    }
    bool quiet{false};
    switch (state.Kind)
    {
    case UsbHostKind::Ehci:
    {
        const Foundation::UInt32 status = access.Read32(state.Operational + 4U);
        const Foundation::UInt32 command = access.Read32(state.Operational);
        if ((status == 0xFFFF'FFFFU) || (command == 0xFFFF'FFFFU))
        {
            return false;
        }
        const bool halted = ((status & Detail::EhciHalted) != 0U) && ((status & Detail::EhciScheduleStatus) == 0U) &&
                            ((command & (Detail::EhciRun | Detail::EhciSchedules)) == 0U);
        if (state.Phase == UsbHostQuietPhase::Halting)
        {
            if (halted)
            {
                access.Write32(state.Operational, Detail::EhciReset);
                state.Phase = UsbHostQuietPhase::Resetting;
            }
            return false;
        }
        quiet = halted && ((command & Detail::EhciReset) == 0U);
        break;
    }
    case UsbHostKind::Ohci:
    {
        const Foundation::UInt32 control = access.Read32(4U);
        const Foundation::UInt32 command = access.Read32(8U);
        if ((control == 0xFFFF'FFFFU) || (command == 0xFFFF'FFFFU))
        {
            return false;
        }
        if (state.Phase == UsbHostQuietPhase::Halting)
        {
            if ((control & Detail::OhciListsAndState) == 0U)
            {
                access.Write32(8U, Detail::OhciReset);
                state.Phase = UsbHostQuietPhase::Resetting;
            }
            return false;
        }
        // HostControllerReset leaves it in UsbSuspend, lists off.
        quiet = ((command & Detail::OhciReset) == 0U) && ((control & 0x3CU) == 0U) &&
                ((control & 0xC0U) != Detail::OhciOperational);
        break;
    }
    case UsbHostKind::Uhci:
    {
        const Foundation::UInt16 command = access.Read16(0U);
        const Foundation::UInt16 status = access.Read16(2U);
        if ((command == 0xFFFFU) || (status == 0xFFFFU))
        {
            return false;
        }
        const bool halted = ((status & Detail::UhciHalted) != 0U) && ((command & Detail::UhciRun) == 0U);
        if (state.Phase == UsbHostQuietPhase::Halting)
        {
            if (halted)
            {
                access.Write16(0U, Detail::UhciReset);
                state.Phase = UsbHostQuietPhase::Resetting;
            }
            return false;
        }
        quiet = halted && ((command & Detail::UhciReset) == 0U);
        break;
    }
    case UsbHostKind::None:
        return false;
    }
    if (!quiet)
    {
        // A controller seen running again after its halt is not quiet.
        state.Phase = (state.Phase == UsbHostQuietPhase::Quiet) ? UsbHostQuietPhase::Resetting : state.Phase;
        return false;
    }
    OrderDmaStorage();
    state.Phase = UsbHostQuietPhase::Quiet;
    return true;
}

/// Once quiet: the interrupt status the dead instance's last transfers
/// Raised, read and acknowledged (write-one-to-clear; UHCI keeps HCHalted).
[[nodiscard]] inline Foundation::UInt32 AcknowledgeUsbHost(IUsbHostQuietAccess& access,
                                                           const UsbHostQuiesceState& state) noexcept
{
    switch (state.Kind)
    {
    case UsbHostKind::Ehci:
    {
        const Foundation::UInt32 status = access.Read32(state.Operational + 4U);
        access.Write32(state.Operational + 4U, status & 0x3FU);
        return status;
    }
    case UsbHostKind::Ohci:
    {
        const Foundation::UInt32 status = access.Read32(0x0CU);
        access.Write32(0x0CU, status & 0x4000'007FU);
        return status;
    }
    case UsbHostKind::Uhci:
    {
        const Foundation::UInt16 status = access.Read16(2U);
        access.Write16(2U, static_cast<Foundation::UInt16>(status & 0x1FU));
        return status;
    }
    case UsbHostKind::None:
        return 0U;
    }
    return 0U;
}
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_USB_HOST_QUIESCE_HPP_INCLUDED
