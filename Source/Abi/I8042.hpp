// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_I8042_HPP_INCLUDED
#define ASTERIA_ABI_I8042_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Abi
{
/// Declared devices, not a hardware probe.
inline constexpr Foundation::UInt64 I8042FadtFlag{1U};
inline constexpr Foundation::UInt64 I8042KeyboardAml{2U};
inline constexpr Foundation::UInt64 I8042MouseAml{4U};
inline constexpr Foundation::UInt64 I8042StaUnknown{8U};
/// The published declared-device word (start blocks, inventory) carries only
/// these four bits.
inline constexpr Foundation::UInt64 I8042DeclaredMask{15U};
/// Boot-time AML evidence only, never published: the IRQ descriptor of the first qualified keyboard
/// (ISA IRQ 1) and mouse (ISA IRQ 12) declaration, ACPI 6.5 6.4.2.1 (IRQ, _HE/_LL) and 6.4.3.6
/// (Extended Interrupt). A clear level or active-low bit means edge or active-high; without the
/// declared bit the descriptor named no such IRQ.
inline constexpr Foundation::UInt64 I8042KeyboardIrqDeclared{0x10U};
inline constexpr Foundation::UInt64 I8042KeyboardIrqLevel{0x20U};
inline constexpr Foundation::UInt64 I8042KeyboardIrqActiveLow{0x40U};
inline constexpr Foundation::UInt64 I8042MouseIrqDeclared{0x80U};
inline constexpr Foundation::UInt64 I8042MouseIrqLevel{0x100U};
inline constexpr Foundation::UInt64 I8042MouseIrqActiveLow{0x200U};
inline constexpr Foundation::UInt64 I8042InterruptMask{0x3F0U};
/// Every bit the boot application may record as AML evidence.
inline constexpr Foundation::UInt64 I8042AmlEvidenceMask{(I8042DeclaredMask & ~I8042FadtFlag) | I8042InterruptMask};
}
#endif
