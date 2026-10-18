// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_INPUT_PROTOCOL_FIXTURES_HPP_INCLUDED
#define ASTERIA_TESTS_USER_INPUT_PROTOCOL_FIXTURES_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Test::Input
{
using Foundation::UInt8;

// Project-owned synthetic USB devices for the UsbDescriptors interface.
// Vendor/product identifiers, release, power and report-description lengths
// are arbitrary test choices. The 34-byte configuration has one boot-class
// interface, one HID descriptor and one interrupt-IN endpoint. No firmware
// or emulator descriptor bytes are used as fixtures.
inline constexpr UInt8 KeyboardDevice[]{
    18U, 1U, 0U, 2U, 0U, 0U, 0U, 8U, 0xADU, 0xDEU, 1U, 0U, 0U, 1U, 0U, 0U, 0U, 1U,
};
inline constexpr UInt8 MouseDevice[]{
    18U, 1U, 0U, 2U, 0U, 0U, 0U, 8U, 0xADU, 0xDEU, 2U, 0U, 0U, 1U, 0U, 0U, 0U, 1U,
};
inline constexpr UInt8 KeyboardConfiguration[]{
    9U, 2U, 34U,   0U,    1U, 1U, 0U, 0x80U, 25U, 9U, 4U, 0U, 0U,    1U, 3U, 1U, 1U,
    0U, 9U, 0x21U, 0x11U, 1U, 0U, 1U, 0x22U, 64U, 0U, 7U, 5U, 0x81U, 3U, 8U, 0U, 10U,
};
inline constexpr UInt8 MouseConfiguration[]{
    9U, 2U, 34U,   0U,    1U, 1U, 0U, 0x80U, 25U, 9U, 4U, 0U, 0U,    1U, 3U, 1U, 2U,
    0U, 9U, 0x21U, 0x11U, 1U, 0U, 1U, 0x22U, 32U, 0U, 7U, 5U, 0x81U, 3U, 4U, 0U, 10U,
};

// HID Usage Tables 1.7 section 10: Keyboard LeftShift (E1) and a/A (04).
// HidBoot's input contract uses one modifier byte, one reserved byte and
// six keyboard usages. The mouse stimulus presses button 1 and moves (5,-3).
inline constexpr UInt8 UsbKeyboardReport[]{2U, 0U, 4U, 0U, 0U, 0U, 0U, 0U};
inline constexpr UInt8 UsbMouseReport[]{1U, 5U, 0xFDU, 0U};
// Ps2Mouse's relative input contract: positive X/Y device motion, button 1,
// no overflow. The decoder changes device Y to screen Y.
inline constexpr UInt8 Ps2Motion[]{9U, 5U, 3U};

// Microsoft Keyboard Scan Code Specification 1.3a Appendix A notes 4/5:
// ordinary Print Screen and Pause set-2 sequences, transcribed independently.
inline constexpr UInt8 PrintMake[]{0xE0U, 0x12U, 0xE0U, 0x7CU};
inline constexpr UInt8 PrintBreak[]{0xE0U, 0xF0U, 0x7CU, 0xE0U, 0xF0U, 0x12U};
inline constexpr UInt8 PauseMake[]{0xE1U, 0x14U, 0x77U, 0xE1U, 0xF0U, 0x14U, 0xF0U, 0x77U};
} // namespace Asteria::Test::Input

#endif // ASTERIA_TESTS_USER_INPUT_PROTOCOL_FIXTURES_HPP_INCLUDED
