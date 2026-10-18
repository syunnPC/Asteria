// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_UHCISTATUS_HPP_INCLUDED
#define ASTERIA_USER_USB_UHCISTATUS_HPP_INCLUDED

#include "User/Usb/Usb1.hpp"

namespace Asteria::User::Usb
{
struct UhciTdResult
{
    bool Active;
    Input::TransferStatus Status;
    Size ActualLength;
};
/// Pure status/actual-length decoder. 0x7ff encodes zero bytes; inactive
/// CRC/timeout/bit-stuff with exhausted C_ERR are Retryable. Active NAK and
/// hardware retries remain pending. Malformed lengths reject atomically.
[[nodiscard]] bool DecodeUhciTd(UInt32 status, Size submitted, UhciTdResult& output) noexcept;
} // namespace Asteria::User::Usb
#endif
