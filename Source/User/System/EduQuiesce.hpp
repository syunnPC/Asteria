// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_EDU_QUIESCE_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_EDU_QUIESCE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::System
{
/// When QEMU's edu is quiet after a driver died with it: its DMA command and its factorial no
/// longer run, and a factorial that will raise its interrupt has raised it. edu clears the
/// factorial's computing bit before it raises the interrupt, so a device whose computing bit is
/// clear may still owe it: while the status register asks for that interrupt (bit 7) and the
/// interrupt status lacks it, the device is not quiet yet.
inline constexpr Foundation::Size EduStatusWord{0x20U / 4U};
inline constexpr Foundation::Size EduInterruptStatusWord{0x24U / 4U};
inline constexpr Foundation::Size EduInterruptAcknowledgeWord{0x64U / 4U};
inline constexpr Foundation::Size EduDmaCommandWord{0x98U / 4U};
inline constexpr Foundation::UInt32 EduComputing{0x01U};
inline constexpr Foundation::UInt32 EduFactorialInterruptEnable{0x80U};
inline constexpr Foundation::UInt32 EduFactorialRaised{0x01U};
inline constexpr Foundation::UInt32 EduDmaRun{0x01U};

[[nodiscard]] inline bool IsEduQuiet(const volatile Foundation::UInt32* const registers) noexcept
{
    if ((ReadRegister(registers, EduDmaCommandWord) & EduDmaRun) != 0U)
    {
        return false;
    }
    const Foundation::UInt32 status = ReadRegister(registers, EduStatusWord);
    return ((status & EduComputing) == 0U) &&
           (((status & EduFactorialInterruptEnable) == 0U) ||
            ((ReadRegister(registers, EduInterruptStatusWord) & EduFactorialRaised) != 0U));
}

/// Acknowledges the interrupt status of a quiet edu and stops it asking for
/// a factorial's interrupt, in that order (clearing the request first would
/// drop an interrupt still owed): the status it acknowledged.
[[nodiscard]] inline Foundation::UInt32 AcknowledgeEdu(volatile Foundation::UInt32* const registers) noexcept
{
    const Foundation::UInt32 found = ReadRegister(registers, EduInterruptStatusWord);
    registers[EduInterruptAcknowledgeWord] = found;
    registers[EduStatusWord] = 0U;
    return found;
}
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_EDU_QUIESCE_HPP_INCLUDED
