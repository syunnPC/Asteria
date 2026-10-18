// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_USER_EXCEPTION_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_USER_EXCEPTION_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
/// The CPL3 exceptions a supervisor receives instead of the machine stopping: every architectural
/// exception vector except the page fault, which has its own path, and the non-maskable interrupt,
/// double fault and machine check, which no user instruction raises and which stay terminal.
[[nodiscard]] constexpr bool IsSupervisedUserExceptionVector(const Foundation::UInt64 vector) noexcept
{
    return (vector < 32U) && (vector != 2U) && (vector != 8U) && (vector != 14U) && (vector != 18U);
}

/// The neutral record of a supervised CPL3 exception: the faulting
/// instruction's address as both addresses, the vector in the syndrome's
/// upper half and the 32-bit error code (zero for vectors that push none) in
/// its lower half, an execute access and the Exception cause.
[[nodiscard]] constexpr Kernel::Memory::FaultRecord
DescribeUserException(const Foundation::UInt64 vector, const Foundation::UInt64 errorCode,
                      const Foundation::UInt64 instructionPointer) noexcept
{
    return Kernel::Memory::FaultRecord{
        .VirtualAddress = instructionPointer,
        .InstructionPointer = instructionPointer,
        .ArchitectureSyndrome = (vector << 32U) | (errorCode & 0xFFFF'FFFFU),
        .Access = Kernel::Memory::FaultAccess::Execute,
        .Cause = Kernel::Memory::FaultCause::Exception,
        .Privilege = Kernel::Memory::FaultPrivilege::User,
        .Reserved = {},
    };
}
} // Namespace Asteria::Platform::Amd64::Interrupt

#endif // ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_USER_EXCEPTION_HPP_INCLUDED
