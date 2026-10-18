// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/EhciStatus.hpp"

namespace Asteria::User::Usb
{
bool DecodeQtd(const UInt32 token, const Size submitted, QtdResult& output) noexcept
{
    const Size residual = (token >> 16U) & 0x7FFFU;
    if ((submitted > 0x5000U) || (residual > submitted) || (((token >> 8U) & 3U) == 3U) || (((token >> 12U) & 7U) > 4U))
    {
        return false;
    }
    QtdState state{QtdState::Success};
    if (((token & 0x80U) != 0U) && ((token & 0x40U) == 0U))
    {
        state = QtdState::Active;
    }
    else if ((token & 0x10U) != 0U)
    {
        state = QtdState::Babble;
    }
    else if ((token & 0x20U) != 0U)
    {
        state = QtdState::BufferError;
    }
    else if ((token & 8U) != 0U)
    {
        state = QtdState::TransactionError;
    }
    else if ((token & 4U) != 0U)
    {
        state = QtdState::MissedMicroframe;
    }
    else if ((token & 0x40U) != 0U)
    {
        state = QtdState::Halted;
    }
    else if ((token & 0x80U) != 0U)
    {
        state = QtdState::Active;
    }
    output = {.State = state, .ActualLength = submitted - residual};
    return true;
}
Input::TransferStatus QtdStatus(const QtdState state) noexcept
{
    switch (state)
    {
    case QtdState::Success:
        return Input::TransferStatus::Success;
    case QtdState::Halted:
        return Input::TransferStatus::Stall;
    case QtdState::TransactionError:
    case QtdState::MissedMicroframe:
    case QtdState::Active:
        return Input::TransferStatus::Retryable;
    case QtdState::Babble:
    case QtdState::BufferError:
        return Input::TransferStatus::Error;
    }
    return Input::TransferStatus::Error;
}
} // namespace Asteria::User::Usb
