// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_USB_PROGRESS_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_USB_PROGRESS_FRAME_HPP_INCLUDED

#include "User/Component/EchoFrame.hpp"
#include "User/Input/UsbDiagnostic.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Component
{
/// One USB enumeration diagnostic, sent by a USB driver started with
/// Abi::XhciReportProgressFlag on the report Endpoint the root service also receives
/// its readiness on. The canonical frame with this method and a fixed
/// 32-byte payload, no attachments: word 0 holds kind, port, detail and code
/// in its low four bytes and the controller in byte 4 (its UsbController
/// kind in bits 7:4, its index in bits 3:0); word 1 holds Value0 and Value1;
/// word 2 the data bytes. Word 3 is zero.
inline constexpr Foundation::UInt32 XhciProgressMethod{0x5848'4350U};
/// The controller byte's kind (bits 7:4) and index (bits 3:0); an xHCI
/// driver's byte is its catalog index, kind zero.
enum class UsbController : Foundation::UInt8
{
    Xhci,
    Ehci,
    Ohci,
    Uhci,
};
[[nodiscard]] constexpr Foundation::UInt8 UsbControllerByte(const UsbController kind,
                                                            const Foundation::UInt8 index) noexcept
{
    return static_cast<Foundation::UInt8>((static_cast<Foundation::UInt8>(kind) << 4U) | (index & 15U));
}
inline constexpr Foundation::Size XhciProgressPayloadBytes{32U};
inline constexpr Foundation::Size XhciProgressFrameBytes{EchoHeaderBytes + XhciProgressPayloadBytes};

[[nodiscard]] inline Foundation::Size WriteXhciProgress(Foundation::UInt8* const frame,
                                                        const Foundation::UInt64 transaction,
                                                        const Foundation::UInt8 controller,
                                                        const Input::UsbDiagnostic& record) noexcept
{
    WriteEchoFrame(frame, nullptr, 0U);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(XhciProgressFrameBytes >> (index * 8U));
        frame[32U + index] = static_cast<Foundation::UInt8>(XhciProgressMethod >> (index * 8U));
    }
    frame[12U] = static_cast<Foundation::UInt8>(XhciProgressPayloadBytes);
    frame[14U] = static_cast<Foundation::UInt8>(XhciProgressPayloadBytes);
    System::StoreWord(frame + 40U, transaction);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes;
    System::StoreWord(payload, static_cast<Foundation::UInt64>(record.Kind) | (Foundation::UInt64{record.Port} << 8U) |
                                   (Foundation::UInt64{record.Detail} << 16U) |
                                   (Foundation::UInt64{record.Code} << 24U) | (Foundation::UInt64{controller} << 32U));
    System::StoreWord(payload + 8U, Foundation::UInt64{record.Value0} | (Foundation::UInt64{record.Value1} << 32U));
    System::StoreWord(payload + 16U, record.Data);
    System::StoreWord(payload + 24U, 0U);
    return XhciProgressFrameBytes;
}

/// Decodes a delivery of WriteXhciProgress's frame; false for any other
/// delivery, leaving the outputs unchanged.
[[nodiscard]] inline bool ReadXhciProgress(const Foundation::UInt8* const delivery, const Foundation::Size bytes,
                                           Foundation::UInt8& controller, Input::UsbDiagnostic& record) noexcept
{
    if ((delivery == nullptr) || (bytes != EchoDeliveryHeaderBytes + XhciProgressFrameBytes))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    Foundation::UInt32 method{0U};
    Foundation::UInt32 total{0U};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        method |= static_cast<Foundation::UInt32>(frame[32U + index]) << (index * 8U);
        total |= static_cast<Foundation::UInt32>(frame[8U + index]) << (index * 8U);
    }
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes;
    const Foundation::UInt64 header = System::LoadWord(payload);
    const Foundation::UInt64 kind = header & 0xFFU;
    if ((method != XhciProgressMethod) || (total != XhciProgressFrameBytes) || (frame[80U] != 0U) ||
        (frame[81U] != 0U) || (frame[12U] != XhciProgressPayloadBytes) || (frame[13U] != 0U) ||
        (frame[14U] != XhciProgressPayloadBytes) || (frame[15U] != 0U) || (kind == 0U) ||
        (kind > Input::LastUsbDiagnosticKind) || ((header >> 40U) != 0U) || (System::LoadWord(payload + 24U) != 0U))
    {
        return false;
    }
    const Foundation::UInt64 values = System::LoadWord(payload + 8U);
    controller = static_cast<Foundation::UInt8>(header >> 32U);
    record = {.Kind = static_cast<Input::UsbDiagnosticKind>(kind),
              .Port = static_cast<Foundation::UInt8>(header >> 8U),
              .Detail = static_cast<Foundation::UInt8>(header >> 16U),
              .Code = static_cast<Foundation::UInt8>(header >> 24U),
              .Value0 = static_cast<Foundation::UInt32>(values & 0xFFFF'FFFFU),
              .Value1 = static_cast<Foundation::UInt32>(values >> 32U),
              .Data = System::LoadWord(payload + 16U)};
    return true;
}
} // namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_USB_PROGRESS_FRAME_HPP_INCLUDED
