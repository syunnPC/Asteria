// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_XHCIEVENT_HPP_INCLUDED
#define ASTERIA_USER_USB_XHCIEVENT_HPP_INCLUDED

#include "User/Usb/XhciMemory.hpp"
#include "User/Input/UsbDevice.hpp"

namespace Asteria::User::Usb
{
enum class EventKind : UInt8
{
    Transfer = 32U,
    Command = 33U,
    Port = 34U,
    Controller = 37U,
};
struct XhciEvent
{
    EventKind Kind;
    UInt64 Pointer;
    UInt32 Residual;
    UInt8 Code;
    UInt8 Slot;
    UInt8 Endpoint;
    UInt8 Port;
};
/// Rejects unsupported kinds, reserved fields, Event Data, zero codes and
/// invalid IDs/pointers without changing output. Cycle ownership is checked
/// by the ring consumer, not this pure decoder. Matching is controller state.
[[nodiscard]] bool DecodeEvent(const Trb& trb, XhciEvent& output) noexcept;
[[nodiscard]] Input::TransferStatus CompletionStatus(UInt8 code) noexcept;
} // namespace Asteria::User::Usb
#endif
