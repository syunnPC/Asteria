// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_POWER_REGISTERS_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_POWER_REGISTERS_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot::Acpi
{
// PowerOff.md; ACPI 6.5 §§5.2.3.2, 5.2.9. Pointer-free final-map facts.
struct PowerRegister
{
    Foundation::UInt64 Address;
    Foundation::UInt8 Space;
    Foundation::UInt8 Width;
    Foundation::UInt8 Reserved[6U];
};
struct PowerRegisters
{
    PowerRegister ControlA;
    PowerRegister ControlB;
    PowerRegister Reset;
    Foundation::UInt32 Flags;
    Foundation::UInt8 TypeA;
    Foundation::UInt8 TypeB;
    Foundation::UInt8 SleepKnown;
    Foundation::UInt8 ResetValue;
    Foundation::UInt32 SmiCommand;
    Foundation::UInt8 AcpiEnable;
    Foundation::UInt8 Reserved[3U];
};
static_assert(sizeof(PowerRegister) == 16U);
static_assert(sizeof(PowerRegisters) == 64U);
static_assert(__is_trivial(PowerRegisters));
// ACPI 6.5 §4.8.3.2.1: preserve SCI_EN and unrelated controls; never
// propagate a read of write-only SLP_EN or GBL_RLS into the new command.
inline constexpr Foundation::UInt16 SleepEnable{1U << 13U};
inline constexpr Foundation::UInt16 SciEnable{1U};
inline constexpr Foundation::UInt16 SleepTypeMask{7U << 10U};
[[nodiscard]] constexpr Foundation::UInt16 EncodeSleep(const Foundation::UInt16 old, const Foundation::UInt8 type,
                                                       const bool enable) noexcept
{
    return static_cast<Foundation::UInt16>((old & ~(SleepTypeMask | SleepEnable | (1U << 2U))) |
                                           (Foundation::UInt16{type} << 10U) | (enable ? SleepEnable : 0U));
}
[[nodiscard]] constexpr bool ValidPowerRegister(const PowerRegister& reg, const Foundation::UInt8 width) noexcept
{
    return reg.Address != 0U && reg.Space <= 1U && reg.Width == width && (reg.Address & (width - 1U)) == 0U &&
           (reg.Space != 1U || reg.Address <= 0x10000U - width) &&
           reg.Address < (Foundation::UInt64{1U} << 47U) - width;
}
}
#endif
