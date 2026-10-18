// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_DEVICE_INTERRUPT_POLL_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_DEVICE_INTERRUPT_POLL_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Interrupt/Ia64InterruptLines.hpp"

/// The pending vectors 0x40-0x4F in bits 0-15 (Ia64DeviceInterruptPoll.S);
/// zero on a host.
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadPendingDeviceVectors() noexcept;
/// Reads cr.ivr; 15 (spurious) on a host.
extern "C" Asteria::Foundation::UInt64 AsteriaIa64AcceptPendingInterrupt() noexcept;
/// Writes cr.eoi.
extern "C" void AsteriaIa64EndAcceptedInterrupt() noexcept;

namespace Asteria::Platform::Ia64::Interrupt
{
enum class Ia64DeviceInterruptPoll : Foundation::UInt8
{
    /// No device vector was pending.
    None,
    /// At least one was dispatched.
    Delivered,
    /// A vector that is neither a device's, the timer's nor spurious.
    Unexpected,
};

/// Dispatches every device vector pending while the BSP idles with PSR.i clear: while one is
/// pending, it reads cr.ivr in normal context, writes cr.eoi and dispatches the vector through
/// lines. A timer tick pending ahead of it, which cr.ivr returns first, is acknowledged as the PL3
/// claim acknowledges a tick that is not due: idle polls the ITC for its own deadlines, and the
/// next dispatch programs the run's. With no device vector pending a tick stays pending for the
/// next PL3 entry. On the BSP, PSR.i clear, normal context.
[[nodiscard]] Ia64DeviceInterruptPoll DeliverPendingDeviceInterrupts(Ia64InterruptLines& lines) noexcept;
} // Namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_DEVICE_INTERRUPT_POLL_HPP_INCLUDED
