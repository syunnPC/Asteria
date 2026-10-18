// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/XhciEvent.hpp"

namespace Asteria::User::Usb
{
bool DecodeEvent(const Trb& trb, XhciEvent& output) noexcept
{
    const UInt32 type = (trb.Control >> 10U) & 0x3FU;
    const UInt8 code = static_cast<UInt8>(trb.Status >> 24U);
    const UInt8 slot = static_cast<UInt8>(trb.Control >> 24U);
    const UInt8 endpoint = static_cast<UInt8>((trb.Control >> 16U) & 0x1FU);
    if (code == 0U)
    {
        return false;
    }
    EventKind kind{EventKind::Controller};
    UInt8 port{0U};
    switch (type)
    {
    case 32U:
        if ((slot == 0U) || (endpoint == 0U) || ((trb.Control & 0x00E003FEU) != 0U) || (trb.Parameter == 0U) ||
            ((trb.Parameter & 15U) != 0U))
        {
            return false;
        }
        kind = EventKind::Transfer;
        break;
    case 33U:
        if ((code == 13U) || ((trb.Control & 0x00FF03FEU) != 0U) || ((trb.Status & 0xFFFFFFU) != 0U) ||
            (trb.Parameter == 0U) || ((trb.Parameter & 15U) != 0U))
        {
            return false;
        }
        kind = EventKind::Command;
        break;
    case 34U:
        if (((trb.Control & 0xFFFF03FEU) != 0U) || ((trb.Status & 0xFFFFFFU) != 0U) ||
            ((trb.Parameter & ~UInt64{0xFF000000U}) != 0U) || (code != 1U))
        {
            return false;
        }
        port = static_cast<UInt8>(trb.Parameter >> 24U);
        if (port == 0U)
        {
            return false;
        }
        kind = EventKind::Port;
        break;
    case 37U:
        if ((trb.Parameter != 0U) || ((trb.Control & 0xFFFF03FEU) != 0U) || ((trb.Status & 0xFFFFFFU) != 0U))
        {
            return false;
        }
        break;
    default:
        return false;
    }
    output = {.Kind = kind,
              .Pointer = trb.Parameter,
              .Residual = trb.Status & 0xFFFFFFU,
              .Code = code,
              .Slot = slot,
              .Endpoint = endpoint,
              .Port = port};
    return true;
}
Input::TransferStatus CompletionStatus(const UInt8 code) noexcept
{
    switch (code)
    {
    case 1U:  // Success
    case 13U: // Short Packet
        return Input::TransferStatus::Success;
    case 6U:
        return Input::TransferStatus::Stall;
    case 4U: // USB Transaction Error; endpoint recovery precedes retry.
        return Input::TransferStatus::Retryable;
    case 2U:  // Data Buffer Error
    case 3U:  // Babble
    case 5U:  // TRB Error
    case 7U:  // Resource Error
    case 8U:  // Bandwidth Error
    case 9U:  // No Slots
    case 10U: // Invalid Stream
    case 11U: // Slot Not Enabled
    case 12U: // Endpoint Not Enabled
    case 14U: // Ring Underrun
    case 15U: // Ring Overrun
    case 17U: // Parameter Error
    case 19U: // Context State Error
    case 21U: // Event Ring Full
    case 22U: // Incompatible Device
    case 24U: // Command Ring Stopped
    case 25U: // Command Aborted
    case 26U: // Stopped
    case 27U: // Stopped Length Invalid
    case 28U: // Stopped Short Packet
    case 29U: // Max Exit Latency Too Large
    case 31U: // Isoch Buffer Overrun
    case 32U: // Event Lost
    case 33U: // Undefined Error
    case 34U: // Invalid Stream ID
    case 35U: // Secondary Bandwidth
    case 36U: // Split Transaction
    default:
        return Input::TransferStatus::Error;
    }
}
} // namespace Asteria::User::Usb
