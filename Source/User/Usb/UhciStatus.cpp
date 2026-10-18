// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/UhciStatus.hpp"

namespace Asteria::User::Usb
{
bool DecodeUhciTd(const UInt32 status, const Size submitted, UhciTdResult& output) noexcept
{
    const Size actual = ((status & 0x7FFU) + 1U) & 0x7FFU;
    const bool active = (status & (1U << 23U)) != 0U;
    if ((submitted > 64U) || (!active && (actual > submitted)))
    {
        return false;
    }
    Input::TransferStatus result{Input::TransferStatus::Success};
    if (active)
    {
        result = Input::TransferStatus::Retryable;
    }
    else if ((status & ((1U << 20U) | (1U << 21U))) != 0U)
    {
        result = Input::TransferStatus::Error; // babble, DMA buffer
    }
    else if ((status & (1U << 22U)) != 0U)
    {
        result = Input::TransferStatus::Stall;
    }
    else if ((status & ((1U << 17U) | (1U << 18U) | (1U << 19U))) != 0U)
    {
        result = Input::TransferStatus::Retryable; // bit stuff, CRC/timeout, NAK
    }
    output = {.Active = active, .Status = result, .ActualLength = active ? 0U : actual};
    return true;
}
} // namespace Asteria::User::Usb
