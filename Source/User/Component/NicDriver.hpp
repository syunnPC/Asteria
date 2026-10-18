// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_NIC_DRIVER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_NIC_DRIVER_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/System/E1000e.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// What the NIC driver takes from its image besides the start block: the ISA's clock, its crash (as
/// RunComponent's; v0 injects none), and the reader and writer of a device register, System:
/// ReadE1000eRegister and System::WriteE1000eRegister (a hosted test passes its model's: reading
/// ICR clears it, so the reader is the platform's too).
struct NicDriverPlatform
{
    ComponentClock Clock;
    ComponentCrash Crash;
    System::E1000eRegisterReader ReadRegister;
    System::E1000eRegisterWriter WriteRegister;
};

/// The NIC driver (Source/User/Network/NetworkStack.hpp; Abi::ComponentBehaviorNicDriver), a
/// service its Device Manager restarts. Its window (words 28 and 29) must hold Abi::NicWindowPages
/// pages. It maps the e1000e's first Abi::NicRegisterPages register pages there and resets the
/// device (interrupts masked, CTRL.RST until it clears, the causes read), sets the link up and
/// waits for it within a bounded budget (a link still down is not a failure), and reads the MAC
/// address; then confines the function in an IommuDomain with its descriptor, receive and transmit
/// pages mapped at the window's and the device's fixed addresses, lets it master the bus, programs
/// the two legacy descriptor rings and enables receive and transmit, and binds its MSI's Interrupt
/// to its service WaitSet with NicInterruptTag before it unmasks the device's causes. Then it
/// serves the frame protocol (NetFrame.hpp) on its Endpoint until it is killed. Every wake (the
/// Interrupt, the Endpoint, or the 10 ms backstop of its wait) reads the interrupt cause, answers
/// the Sends whose descriptors the device wrote back, takes every request the Endpoint holds,
/// copies the received frames into the oldest credits (through the window's last page) or drops
/// them, copies queued Sends into free transmit buffers, and rearms the Interrupt after an
/// interrupt wake. Without an IOMMU it serves every Send and ReceiveCredit NicFailed with
/// NicNoDevice. Returns only a failure's exit code (Abi::ComponentDriverFailedExitCode with its
/// step and error); a refused Interrupt is one (receive is interrupt-driven, never a silent poll).
/// Blocks in WaitSetWait; polls the device with the clock during bring-up.
[[nodiscard]] Foundation::UInt64 ServeNicDevice(const System::SystemCallClient& client,
                                                const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                                const NicDriverPlatform& platform) noexcept;
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_NIC_DRIVER_HPP_INCLUDED
