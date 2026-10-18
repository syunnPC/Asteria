// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_EHCISTATUS_HPP_INCLUDED
#define ASTERIA_USER_USB_EHCISTATUS_HPP_INCLUDED

#include "User/Input/UsbDevice.hpp"
#include "User/Usb/XhciMemory.hpp"

namespace Asteria::User::Usb
{
enum class QtdState : UInt8
{
    Active,
    Success,
    Halted,
    Babble,
    TransactionError,
    BufferError,
    MissedMicroframe,
};
struct QtdResult
{
    QtdState State;
    Size ActualLength;
};
/// Pure token decoder; rejects impossible PID/page/residual without changing
/// output. The controller separately checks chain ownership and QH overlay.
[[nodiscard]] bool DecodeQtd(UInt32 token, Size submitted, QtdResult& output) noexcept;
[[nodiscard]] Input::TransferStatus QtdStatus(QtdState state) noexcept;
} // namespace Asteria::User::Usb
#endif
