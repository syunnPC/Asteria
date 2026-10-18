// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_INPUT_USBCOMPOSITEFIXTURE_HPP_INCLUDED
#define ASTERIA_TESTS_USER_INPUT_USBCOMPOSITEFIXTURE_HPP_INCLUDED

#include "User/Input/UsbConfiguration.hpp"

namespace Asteria::Test::Input
{
// Project-owned USB 2.0/HID 1.11 wire fixture: a keyboard, its alternate,
// a mouse, and an 08/06/50 interface. IAD groups the two HID interfaces.
inline constexpr User::Input::UInt8 CompositeConfiguration[]{
    9U, 2U,  115U,  0U,    3U, 1U, 0U,  0x80U, 50U,   8U, 0x0BU, 0U,  2U,    3U,    1U, 0U, 0U, 9U,    4U,    0U,
    0U, 1U,  3U,    1U,    1U, 0U, 9U,  0x21U, 0x11U, 1U, 0U,    1U,  0x22U, 63U,   0U, 7U, 5U, 0x81U, 3U,    8U,
    0U, 10U, 9U,    4U,    0U, 1U, 1U,  3U,    1U,    1U, 0U,    9U,  0x21U, 0x11U, 1U, 0U, 1U, 0x22U, 63U,   0U,
    7U, 5U,  0x81U, 3U,    8U, 0U, 10U, 9U,    4U,    1U, 0U,    1U,  3U,    1U,    2U, 0U, 9U, 0x21U, 0x11U, 1U,
    0U, 1U,  0x22U, 52U,   0U, 7U, 5U,  0x82U, 3U,    4U, 0U,    10U, 9U,    4U,    2U, 0U, 2U, 8U,    6U,    0x50U,
    0U, 7U,  5U,    0x83U, 2U, 0U, 2U,  0U,    7U,    5U, 3U,    2U,  0U,    2U,    0U,
};
static_assert(sizeof(CompositeConfiguration) == 115U);
} // namespace Asteria::Test::Input

#endif
