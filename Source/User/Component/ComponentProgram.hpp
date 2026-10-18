// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_COMPONENT_PROGRAM_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_COMPONENT_PROGRAM_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The component's clock: the ISA's free-running counter.
using ComponentClock = Foundation::UInt64 (*)() noexcept;

/// Crashes as kind (Abi::ComponentCrash*) says: executes an instruction the ISA refuses, or divides
/// by zero with that floating-point exception unmasked; returns only if it did not trap.
using ComponentCrash = void (*)(Foundation::UInt64 kind) noexcept;

/// The component the root service builds and restarts, from its start block (Abi:
/// ComponentBootstrap.hpp). The first incarnation spins for the block's tick count by `clock`,
/// longer than a time slice, then crashes as the block says; its supervisor terminates it. A
/// reporting one computes with floating point and integer division for as long, preempted
/// meanwhile, requires the exact results (else Abi::ComponentArithmeticFailedExitCode), sends the
/// canonical asynchronous frame on its report Endpoint and returns Abi::ComponentExitCode. An
/// account one creates and maps memory in its own ResourceDomain and window (words 24-29), is
/// refused a page beyond, and reports with Abi::ComponentAccountExitCode, or returns Abi:
/// ComponentAccountFailedExitCode. A driver maps the assignable device's registers into its window
/// (words 30-33) and drives the device, polling, at most the block's tick count per operation, then
/// reports with Abi::ComponentDriverExitCode, or returns Abi::ComponentDriverFailedExitCode; a
/// driver service serves its device until it is killed, the AHCI driver (ServeAhciDevice, AMD64
/// only) the block protocol, the NIC driver (ServeNicDevice, AMD64 only) the frame protocol, the
/// display broker (ServeDisplay) the console protocol, the filesystem service (ServeFilesystem,
/// AMD64 only) the file protocol over the block service its start block names, the network stack
/// service (ServeNetwork, AMD64 only) the socket protocol over the NIC driver its start block
/// names. A block without the magic or with another incarnation returns Abi:
/// ComponentBadStartExitCode, a refused report Abi::ComponentReportFailedExitCode, a crash that
/// returned Abi::ComponentCrashReturnedExitCode. The caller exits with the result.
[[nodiscard]] Foundation::UInt64 RunComponent(System::SystemCallTransport transport,
                                              const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                              ComponentClock clock, ComponentCrash crash) noexcept;
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_COMPONENT_PROGRAM_HPP_INCLUDED
