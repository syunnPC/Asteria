// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_OHCISTATUS_HPP_INCLUDED
#define ASTERIA_USER_USB_OHCISTATUS_HPP_INCLUDED

#include "User/Usb/Usb1.hpp"

namespace Asteria::User::Usb
{
struct OhciTdResult
{
    bool Active;
    Input::TransferStatus Status;
    Size ActualLength;
};
/// Decode CC and CBP for a contiguous <=1024-byte submitted span. Unknown CCs
/// fail conservatively. Invalid CBP/length rejects without changing output.
[[nodiscard]] bool DecodeOhciTd(UInt32 flags, UInt32 currentBuffer, UInt32 submittedBuffer, Size submitted,
                                OhciTdResult& output) noexcept;
} // namespace Asteria::User::Usb
#endif
