// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_AHCI_DRIVER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_AHCI_DRIVER_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/System/AhciPort.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// What the AHCI driver takes from its image besides the start block: the ISA's clock, its crash
/// (as RunComponent's), and the writer of a device register, System::WriteAhciRegister (a hosted
/// test passes its model's).
struct AhciDriverPlatform
{
    ComponentClock Clock;
    ComponentCrash Crash;
    System::AhciRegisterWriter WriteRegister;
};

/// The AHCI driver (Source/User/System/AhciPort.hpp; Abi::ComponentBehaviorBlockDriver), a service
/// its Device Manager restarts. It maps the controller's registers into the first page of its
/// window (words 28-33) and brings up the first implemented port with a disk: stops it if it runs,
/// confines its function in an IommuDomain with its DMA page (the second page of its window) at
/// device address 0x0010'0000, lets it master the bus, starts the port, reads the disk's capacity
/// with IDENTIFY DEVICE, and binds its controller's Interrupt to its service WaitSet with
/// BlockInterruptTag (polling only if the kernel refuses the Interrupt). Then it serves the block
/// protocol (BlockFrame.hpp) on its Endpoint until it is killed, one command at a time, each data
/// Frame mapped at device address 0x0020'0000 while its command runs. Without a disk, or without an
/// IOMMU, it serves every Read and Write BlockFailed with BlockNoDevice, and Describe reports an
/// absent medium. Describe otherwise reports IDENTIFY capacity, fixed writable media and its medium
/// generation. Non-512 logical sectors are refused with BlockUnsupportedSectorSize. State-losing
/// resets restart the incarnation and advance medium generation; error recovery that only restarts
/// command engines preserves it. Returns only a failure's exit code (Abi:
/// ComponentDriverFailedExitCode with its step and error). Blocks in WaitSetWait; polls the
/// controller with the clock during bring-up.
[[nodiscard]] Foundation::UInt64 ServeAhciDevice(const System::SystemCallClient& client,
                                                 const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                                 const AhciDriverPlatform& platform) noexcept;
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_AHCI_DRIVER_HPP_INCLUDED
