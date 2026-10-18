// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_NIC_QUIESCE_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_NIC_QUIESCE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/System/E1000e.hpp"

namespace Asteria::User::System
{
/// When the e1000e (82574L) is quiet after a driver died with it: the drain already turned its bus
/// mastering off; the Device Manager masks every interrupt cause and resets the device (CTRL.RST,
/// which disables both rings), and the device is quiet once the reset bit reads back clear. The
/// device moves packet data only while it processes a descriptor, gated by bus mastering, so no
/// late write follows the reset. The first page of BAR0 holds every register used here
/// (User/System/E1000e.hpp).
inline constexpr Foundation::UInt32 NicAllCauses{0xFFFF'FFFFU};

/// Masks every interrupt cause and resets the device: whether a ring was
/// still enabled (the evidence the Device Manager reports).
[[nodiscard]] inline bool BeginNicQuiesce(volatile Foundation::UInt32* const registers) noexcept
{
    const bool running = ((ReadRegister(registers, E1000eRctlWord) & E1000eRctlEn) != 0U) ||
                         ((ReadRegister(registers, E1000eTctlWord) & E1000eTctlEn) != 0U);
    WriteE1000eRegister(registers, E1000eImcWord, NicAllCauses);
    WriteE1000eRegister(registers, E1000eCtrlWord, ReadRegister(registers, E1000eCtrlWord) | E1000eCtrlRst);
    return running;
}

[[nodiscard]] inline bool IsNicQuiet(const volatile Foundation::UInt32* const registers) noexcept
{
    return (ReadRegister(registers, E1000eCtrlWord) & E1000eCtrlRst) == 0U;
}

/// Masks every cause again (the reset unmasks none, but a value the reset
/// left is not assumed) and reads the interrupt cause, which clears it: the
/// causes it acknowledged.
[[nodiscard]] inline Foundation::UInt32 AcknowledgeNic(volatile Foundation::UInt32* const registers) noexcept
{
    WriteE1000eRegister(registers, E1000eImcWord, NicAllCauses);
    return ReadRegister(registers, E1000eIcrWord);
}
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_NIC_QUIESCE_HPP_INCLUDED
