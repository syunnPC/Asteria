// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/OhciStatus.hpp"

namespace Asteria::User::Usb
{
bool DecodeOhciTd(const UInt32 flags, const UInt32 currentBuffer, const UInt32 submittedBuffer, const Size submitted,
                  OhciTdResult& output) noexcept
{
    if ((submitted > 1024U) ||
        ((submitted != 0U) && ((submittedBuffer == 0U) || (submittedBuffer > 0xFFFFFFFFU - (submitted - 1U)))) ||
        ((submitted == 0U) && (currentBuffer != 0U)) ||
        ((currentBuffer != 0U) &&
         ((currentBuffer < submittedBuffer) || (UInt64{currentBuffer} > UInt64{submittedBuffer} + submitted))))
    {
        return false;
    }
    const UInt32 code = flags >> 28U;
    const Size length = currentBuffer == 0U ? submitted : Size{currentBuffer - submittedBuffer};
    Input::TransferStatus status{Input::TransferStatus::Error};
    switch (code)
    {
    case 0U:
        status = Input::TransferStatus::Success;
        break;
    case 4U:
        status = Input::TransferStatus::Stall;
        break;
    case 1U: // CRC
    case 2U: // bit stuffing
    case 3U: // toggle mismatch
    case 5U: // no response
        status = Input::TransferStatus::Retryable;
        break;
    case 9U: // data underrun; only acceptable when BufferRounding was requested
        status = (flags & (1U << 18U)) != 0U ? Input::TransferStatus::Success : Input::TransferStatus::Error;
        break;
    case 14U:
    case 15U:
        status = Input::TransferStatus::Retryable;
        break;
    default: // PID, overrun, reserved and buffer errors
        break;
    }
    output = {.Active = code >= 14U, .Status = status, .ActualLength = length};
    return true;
}
} // namespace Asteria::User::Usb
