// SPDX-License-Identifier: Apache-2.0
#include "User/System/UsbHostQuiesce.hpp"

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::TestSupport::TestContext;
namespace S = Asteria::User::System;

// A register model: 256 bytes of memory registers, 32 bytes of I/O ports
// and 256 bytes of configuration space, counting every register write.
class Controller final : public S::IUsbHostQuietAccess
{
public:
    [[nodiscard]] UInt32 Read32(const UInt32 offset) noexcept override
    {
        return Memory[offset / 4U];
    }
    void Write32(const UInt32 offset, const UInt32 value) noexcept override
    {
        Memory[offset / 4U] = value;
        ++Writes;
    }
    [[nodiscard]] UInt16 Read16(const UInt32 offset) noexcept override
    {
        return Ports[offset / 2U];
    }
    void Write16(const UInt32 offset, const UInt16 value) noexcept override
    {
        Ports[offset / 2U] = value;
        ++Writes;
    }
    [[nodiscard]] bool ReadConfiguration(const UInt32 offset, UInt32& value) noexcept override
    {
        value = Refuse ? 0xFFFF'FFFFU : Configuration[offset / 4U];
        return !Refuse;
    }

    UInt32 Memory[64U]{};
    UInt16 Ports[16U]{};
    UInt32 Configuration[64U]{};
    Size Writes{0U};
    bool Refuse{false};
};

// EHCI 1.0: capability registers at 0, operational registers at 0x20,
// EECP 0x68 with its USB Legacy Support capability (BIOS Owned bit 16).
constexpr UInt32 EhciOperational{0x20U};
void Ehci(Controller& controller, const UInt32 legacy) noexcept
{
    controller.Memory[0U] = 0x0100'0020U;
    controller.Memory[2U] = 0x0000'6800U;
    controller.Configuration[0x68U / 4U] = legacy;
    controller.Memory[EhciOperational / 4U] = 0x0008'0031U;
    controller.Memory[EhciOperational / 4U + 1U] = 0x0000'C000U;
    controller.Memory[EhciOperational / 4U + 2U] = 0x0000'0037U;
}

// Each seed lets the controller keep running a different number of polls
// before it halts and before its reset completes: no reset is written before
// the halt (with both schedules stopped), and quiet comes only after it.
void EhciSchedules(TestContext& runner) noexcept
{
    for (UInt32 seed{1U}; seed <= 64U; ++seed)
    {
        Controller controller{};
        Ehci(controller, 0x0100'0001U);
        S::UsbHostQuiesceState state{};
        const bool running = S::BeginUsbHostQuiesce(controller, S::UsbHostKind::Ehci, 0x400U, state);
        ASTERIA_EXPECT(runner, "a running EHCI stops its schedules and masks its interrupts", running,
                       controller.Memory[EhciOperational / 4U] == 0x0008'0000U,
                       controller.Memory[EhciOperational / 4U + 2U] == 0U,
                       state.Phase == S::UsbHostQuietPhase::Halting);
        for (UInt32 step{0U}; step < (seed % 5U) + 1U; ++step)
        {
            // Not halted, or halted with a schedule still reported.
            controller.Memory[EhciOperational / 4U + 1U] = ((step & 1U) != 0U) ? 0x0000'5000U : 0x0000'8000U;
            ASTERIA_EXPECT(runner, "no reset before the halt with both schedules stopped",
                           !S::PollUsbHostQuiesce(controller, state),
                           controller.Memory[EhciOperational / 4U] == 0x0008'0000U);
        }
        controller.Memory[EhciOperational / 4U + 1U] = 0x0000'1000U;
        ASTERIA_EXPECT(runner, "the halt starts HCRESET but proves nothing yet",
                       !S::PollUsbHostQuiesce(controller, state), controller.Memory[EhciOperational / 4U] == 2U,
                       state.Phase == S::UsbHostQuietPhase::Resetting);
        for (UInt32 step{0U}; step < (seed % 3U); ++step)
        {
            ASTERIA_EXPECT(runner, "a reset in progress is not quiet", !S::PollUsbHostQuiesce(controller, state));
        }
        controller.Memory[EhciOperational / 4U] = 0x0008'0000U;
        controller.Memory[EhciOperational / 4U + 1U] = 0x0000'1003U;
        ASTERIA_EXPECT(runner, "a finished reset of a halted EHCI is quiet", S::PollUsbHostQuiesce(controller, state),
                       state.Phase == S::UsbHostQuietPhase::Quiet);
        ASTERIA_EXPECT(runner, "its interrupt status is acknowledged",
                       S::AcknowledgeUsbHost(controller, state) == 0x0000'1003U,
                       controller.Memory[EhciOperational / 4U + 1U] == 3U);
        controller.Memory[EhciOperational / 4U] = 1U;
        controller.Memory[EhciOperational / 4U + 1U] = 0U;
        ASTERIA_EXPECT(runner, "a controller seen running again is not quiet",
                       !S::PollUsbHostQuiesce(controller, state));
    }
}

void EhciRefusals(TestContext& runner) noexcept
{
    Controller owned{};
    Ehci(owned, 0x0001'0001U);
    S::UsbHostQuiesceState state{};
    ASTERIA_EXPECT(runner, "an EHCI firmware owns is never written",
                   !S::BeginUsbHostQuiesce(owned, S::UsbHostKind::Ehci, 0x400U, state),
                   state.Phase == S::UsbHostQuietPhase::FirmwareOwned, owned.Writes == 0U,
                   !S::PollUsbHostQuiesce(owned, state), owned.Writes == 0U);
    Controller retaken{};
    Ehci(retaken, 0x0100'0001U);
    const bool begun = S::BeginUsbHostQuiesce(retaken, S::UsbHostKind::Ehci, 0x400U, state);
    retaken.Configuration[0x68U / 4U] = 0x0101'0001U;
    retaken.Memory[EhciOperational / 4U + 1U] = 0x0000'1000U;
    const Size writes = retaken.Writes;
    ASTERIA_EXPECT(runner, "firmware taking an EHCI back during the halt stops the reset", begun,
                   !S::PollUsbHostQuiesce(retaken, state), state.Phase == S::UsbHostQuietPhase::FirmwareOwned,
                   retaken.Writes == writes);
    Controller refused{};
    Ehci(refused, 0x0100'0001U);
    refused.Refuse = true;
    ASTERIA_EXPECT(runner, "a refused EECP read is invalid, without writes",
                   !S::BeginUsbHostQuiesce(refused, S::UsbHostKind::Ehci, 0x400U, state),
                   state.Phase == S::UsbHostQuietPhase::Invalid, refused.Writes == 0U);
    Controller looped{};
    Ehci(looped, 0x0000'6801U);
    Controller low{};
    Ehci(low, 0x0000'3001U);
    Controller small{};
    Ehci(small, 0x0100'0001U);
    small.Memory[0U] = 0x0100'0008U;
    Controller beyond{};
    Ehci(beyond, 0x0100'0001U);
    Controller version{};
    Ehci(version, 0x0100'0001U);
    version.Memory[0U] = 0x0200'0020U;
    Controller absent{};
    Ehci(absent, 0x0100'0001U);
    absent.Memory[0U] = 0xFFFF'FFFFU;
    ASTERIA_EXPECT(runner, "an EHCI layout this recipe does not know is invalid, without writes",
                   !S::BeginUsbHostQuiesce(looped, S::UsbHostKind::Ehci, 0x400U, state), looped.Writes == 0U,
                   !S::BeginUsbHostQuiesce(low, S::UsbHostKind::Ehci, 0x400U, state), low.Writes == 0U,
                   !S::BeginUsbHostQuiesce(small, S::UsbHostKind::Ehci, 0x400U, state), small.Writes == 0U,
                   !S::BeginUsbHostQuiesce(beyond, S::UsbHostKind::Ehci, 0x60U, state), beyond.Writes == 0U,
                   !S::BeginUsbHostQuiesce(version, S::UsbHostKind::Ehci, 0x400U, state), version.Writes == 0U,
                   !S::BeginUsbHostQuiesce(absent, S::UsbHostKind::Ehci, 0x400U, state), absent.Writes == 0U,
                   state.Phase == S::UsbHostQuietPhase::Invalid, !S::PollUsbHostQuiesce(absent, state));
    Controller halted{};
    Ehci(halted, 0x0100'0001U);
    halted.Memory[2U] = 0U;
    halted.Memory[EhciOperational / 4U] = 0x0008'0000U;
    halted.Memory[EhciOperational / 4U + 1U] = 0x0000'1000U;
    ASTERIA_EXPECT(runner, "a halted EHCI without legacy support is reset too",
                   !S::BeginUsbHostQuiesce(halted, S::UsbHostKind::Ehci, 0x400U, state), state.Legacy == 0U,
                   state.Phase == S::UsbHostQuietPhase::Halting, !S::PollUsbHostQuiesce(halted, state),
                   halted.Memory[EhciOperational / 4U] == 2U);
}

// OpenHCI 1.0a: HcRevision 0x10, HcControl at 4 (operational, every list
// enabled), HcCommandStatus at 8.
void Ohci(Controller& controller, const UInt32 control) noexcept
{
    controller.Memory[0U] = 0x10U;
    controller.Memory[1U] = control;
}

void OhciSchedules(TestContext& runner) noexcept
{
    for (UInt32 seed{1U}; seed <= 32U; ++seed)
    {
        Controller controller{};
        Ohci(controller, 0x0000'06BFU);
        S::UsbHostQuiesceState state{};
        ASTERIA_EXPECT(runner, "a running OHCI turns its lists off in UsbReset with every interrupt disabled",
                       S::BeginUsbHostQuiesce(controller, S::UsbHostKind::Ohci, 0x100U, state),
                       controller.Memory[1U] == 0x0000'0603U, controller.Memory[0x14U / 4U] == 0xFFFF'FFFFU,
                       controller.Memory[2U] == 0U);
        ASTERIA_EXPECT(runner, "UsbReset reached starts HostControllerReset", !S::PollUsbHostQuiesce(controller, state),
                       controller.Memory[2U] == 1U, state.Phase == S::UsbHostQuietPhase::Resetting);
        for (UInt32 step{0U}; step < (seed % 4U); ++step)
        {
            ASTERIA_EXPECT(runner, "a reset in progress is not quiet", !S::PollUsbHostQuiesce(controller, state));
        }
        controller.Memory[2U] = 0U;
        controller.Memory[1U] = 0x0000'00C3U;
        ASTERIA_EXPECT(runner, "a reset OHCI in UsbSuspend with its lists off is quiet",
                       S::PollUsbHostQuiesce(controller, state));
        controller.Memory[0x0CU / 4U] = 0x4000'0045U;
        ASTERIA_EXPECT(runner, "its interrupt status is acknowledged",
                       S::AcknowledgeUsbHost(controller, state) == 0x4000'0045U);
        controller.Memory[1U] = 0x0000'0083U;
        ASTERIA_EXPECT(runner, "an OHCI back in UsbOperational is not quiet",
                       !S::PollUsbHostQuiesce(controller, state));
    }
    Controller owned{};
    Ohci(owned, 0x0000'01BFU);
    S::UsbHostQuiesceState state{};
    Controller revision{};
    Ohci(revision, 0x0000'00BFU);
    revision.Memory[0U] = 0x11U;
    ASTERIA_EXPECT(runner, "an OHCI routed to SMM, or of another revision, is never written",
                   !S::BeginUsbHostQuiesce(owned, S::UsbHostKind::Ohci, 0x100U, state),
                   state.Phase == S::UsbHostQuietPhase::FirmwareOwned, owned.Writes == 0U,
                   !S::BeginUsbHostQuiesce(revision, S::UsbHostKind::Ohci, 0x100U, state),
                   state.Phase == S::UsbHostQuietPhase::Invalid, revision.Writes == 0U,
                   !S::BeginUsbHostQuiesce(revision, S::UsbHostKind::Ohci, 0x50U, state));
}

// UHCI 1.1: USBCMD at 0, USBSTS at 2, USBINTR at 4; LEGSUP at 0xC0.
void UhciSchedules(TestContext& runner) noexcept
{
    for (UInt32 seed{1U}; seed <= 32U; ++seed)
    {
        Controller controller{};
        controller.Ports[0U] = 0x00C1U;
        controller.Ports[2U] = 0x000FU;
        controller.Configuration[0xC0U / 4U] = 0x2000U;
        S::UsbHostQuiesceState state{};
        ASTERIA_EXPECT(runner, "a running UHCI clears Run/Stop with its interrupts masked",
                       S::BeginUsbHostQuiesce(controller, S::UsbHostKind::Uhci, 0U, state),
                       controller.Ports[0U] == 0x00C0U, controller.Ports[2U] == 0U);
        for (UInt32 step{0U}; step < (seed % 4U) + 1U; ++step)
        {
            ASTERIA_EXPECT(runner, "no HCRESET before HCHalted", !S::PollUsbHostQuiesce(controller, state),
                           controller.Ports[0U] == 0x00C0U);
        }
        controller.Ports[1U] = 0x0021U;
        ASTERIA_EXPECT(runner, "HCHalted starts HCRESET", !S::PollUsbHostQuiesce(controller, state),
                       controller.Ports[0U] == 2U, !S::PollUsbHostQuiesce(controller, state));
        controller.Ports[0U] = 0U;
        ASTERIA_EXPECT(runner, "a finished reset of a halted UHCI is quiet", S::PollUsbHostQuiesce(controller, state),
                       S::AcknowledgeUsbHost(controller, state) == 0x0021U, controller.Ports[1U] == 1U);
        controller.Ports[1U] = 0U;
        ASTERIA_EXPECT(runner, "a UHCI no longer halted is not quiet", !S::PollUsbHostQuiesce(controller, state));
    }
    Controller trapped{};
    trapped.Ports[0U] = 0x0001U;
    trapped.Configuration[0xC0U / 4U] = 0x2010U;
    S::UsbHostQuiesceState state{};
    Controller refused{};
    refused.Refuse = true;
    Controller absent{};
    absent.Ports[0U] = 0xFFFFU;
    ASTERIA_EXPECT(runner, "a UHCI with SMIs enabled, or unreadable, is never written",
                   !S::BeginUsbHostQuiesce(trapped, S::UsbHostKind::Uhci, 0U, state),
                   state.Phase == S::UsbHostQuietPhase::FirmwareOwned, trapped.Writes == 0U,
                   !S::BeginUsbHostQuiesce(refused, S::UsbHostKind::Uhci, 0U, state),
                   state.Phase == S::UsbHostQuietPhase::FirmwareOwned, refused.Writes == 0U,
                   !S::BeginUsbHostQuiesce(absent, S::UsbHostKind::Uhci, 0U, state),
                   state.Phase == S::UsbHostQuietPhase::Invalid, absent.Writes == 0U,
                   !S::BeginUsbHostQuiesce(absent, S::UsbHostKind::None, 0U, state));
    ASTERIA_EXPECT(
        runner, "class dwords name the controller", S::UsbHostKindOf(0x0C03'2003U) == S::UsbHostKind::Ehci,
        S::UsbHostKindOf(0x0C03'1000U) == S::UsbHostKind::Ohci, S::UsbHostKindOf(0x0C03'0001U) == S::UsbHostKind::Uhci,
        S::UsbHostKindOf(0x0C03'3000U) == S::UsbHostKind::None, S::UsbHostKindOf(0x0C05'0000U) == S::UsbHostKind::None);
}
} // namespace

int main()
{
    TestContext runner{"USB host recovery model"};
    EhciSchedules(runner);
    EhciRefusals(runner);
    OhciSchedules(runner);
    UhciSchedules(runner);
    return runner.Finish();
}
