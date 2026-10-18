// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_DRIVER_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_DRIVER_FRAME_HPP_INCLUDED

#include "Abi/IpcCanonicalFrame.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The driver service's protocol: asynchronous canonical frames of DriverInterface built by hand,
/// as the echo's are (no codec in the component image). A request carries one attachment, a copy of
/// a send capability on the client's reply Endpoint, then a four-word payload: the generation the
/// client addressed (the instance's tag), the request's argument, the faults to inject
/// (DriverCrashAfterStart or zero) and a zero word. Method DriverCopyMethod copies its argument, a
/// pattern, through the device's DMA; method DriverFactorialMethod has the device compute the
/// factorial of its argument and raise its interrupt when done, for a device that may not master
/// the bus. A reply, method DriverReplyMethod, carries no attachment and the payload: the request's
/// generation, its argument, the reply's kind and the device's interrupt status the instance found,
/// and cleared, as it started. Both keep the request's transaction identifier.
inline constexpr Foundation::UInt64 DriverInterfaceLow{0x4452'4956'4552'0001ULL};
inline constexpr Foundation::UInt64 DriverInterfaceHigh{0x4153'5445'5249'4101ULL};
inline constexpr Foundation::UInt32 DriverCopyMethod{1U};
inline constexpr Foundation::UInt32 DriverReplyMethod{2U};
inline constexpr Foundation::UInt32 DriverFactorialMethod{3U};
/// A reply's kind: the copy's write-back transfer or the factorial started
/// (provisional), the copy landed or the factorial came out right, the
/// request named an instance that died and was not executed, or the request
/// failed.
inline constexpr Foundation::UInt64 DriverStarted{1U};
inline constexpr Foundation::UInt64 DriverDone{2U};
inline constexpr Foundation::UInt64 DriverRestarted{3U};
inline constexpr Foundation::UInt64 DriverFailed{4U};
/// A fault the Device Manager's test injects: the instance crashes once the
/// copy's write-back transfer or the factorial runs, as a driver with a bug
/// would, so it dies with its device busy and never sees the work end.
inline constexpr Foundation::UInt64 DriverCrashAfterStart{1U};
inline constexpr Foundation::Size DriverPayloadWords{4U};
inline constexpr Foundation::Size DriverPayloadBytes{DriverPayloadWords << 3U};
inline constexpr Foundation::Size DriverRequestBytes{EchoHeaderBytes + EchoAttachmentBytes + DriverPayloadBytes};
inline constexpr Foundation::Size DriverReplyBytes{EchoHeaderBytes + DriverPayloadBytes};
/// The sealed selector of the driver service's Endpoint and the tag of that Endpoint in the
/// service's WaitSet.
inline constexpr Foundation::UInt64 DriverSealedSelector{0x4452'5652'5345'414CULL};
inline constexpr Foundation::UInt64 DriverServiceWaitTag{0x4452'5652U};

/// One decoded driver frame.
struct DriverMessage
{
    Foundation::UInt64 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[DriverPayloadWords];
    /// A request's reply capability, as the receiver's space holds it.
    System::CapabilityHandle Reply;
};

namespace Detail
{
inline void StoreDriverWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] inline Foundation::UInt64 LoadDriverWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}
} // Namespace Detail

/// Writes a driver frame into frame: a request of method when reply is
/// given, a reply otherwise. Returns its size, DriverRequestBytes or
/// DriverReplyBytes.
inline Foundation::Size WriteDriverFrame(Foundation::UInt8* const frame, const Foundation::UInt64 transaction,
                                         const Foundation::UInt64 (&words)[DriverPayloadWords],
                                         const EchoAttachment* const reply,
                                         const Foundation::UInt32 method = DriverCopyMethod) noexcept
{
    const Foundation::Size attachments{(reply != nullptr) ? 1U : 0U};
    WriteEchoFrame(frame, reply, attachments);
    const Foundation::Size total = EchoHeaderBytes + (attachments << 5U) + DriverPayloadBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(total >> (index << 3U));
    }
    frame[12U] = static_cast<Foundation::UInt8>(DriverPayloadBytes);
    frame[13U] = 0U;
    frame[14U] = static_cast<Foundation::UInt8>(DriverPayloadBytes);
    frame[15U] = 0U;
    Detail::StoreDriverWord(frame + 16U, DriverInterfaceLow);
    Detail::StoreDriverWord(frame + 24U, DriverInterfaceHigh);
    const Foundation::UInt32 written{(reply != nullptr) ? method : DriverReplyMethod};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[32U + index] = static_cast<Foundation::UInt8>(written >> (index << 3U));
    }
    Detail::StoreDriverWord(frame + 40U, transaction);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    for (Foundation::Size word{0U}; word < DriverPayloadWords; ++word)
    {
        Detail::StoreDriverWord(payload + (word << 3U), words[word]);
    }
    return total;
}

/// Decodes the frame of a delivery of deliveryBytes (its 48-byte header
/// first): false unless it is a driver frame of its exact size, a request
/// with one Endpoint attachment or a reply with none.
[[nodiscard]] inline bool ReadDriverFrame(const Foundation::UInt8* const delivery, const Foundation::Size deliveryBytes,
                                          DriverMessage& message) noexcept
{
    if (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::Size attachments = frame[81U];
    const Foundation::UInt64 method = frame[32U] | (static_cast<Foundation::UInt64>(frame[33U]) << 8U);
    const bool request = (method == DriverCopyMethod) || (method == DriverFactorialMethod);
    if ((Detail::LoadDriverWord(frame + 16U) != DriverInterfaceLow) ||
        (Detail::LoadDriverWord(frame + 24U) != DriverInterfaceHigh) || (frame[34U] != 0U) || (frame[35U] != 0U) ||
        (!request && (method != DriverReplyMethod)) || (frame[80U] != 0U) || (attachments != (request ? 1U : 0U)) ||
        (frame[12U] != DriverPayloadBytes) || (frame[13U] != 0U) ||
        (deliveryBytes != (EchoDeliveryHeaderBytes + EchoHeaderBytes + (attachments << 5U) + DriverPayloadBytes)) ||
        (request && (frame[EchoHeaderBytes + 27U] != EchoEndpointType)))
    {
        return false;
    }
    message.Method = method;
    message.Transaction = Detail::LoadDriverWord(frame + 40U);
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    for (Foundation::Size word{0U}; word < DriverPayloadWords; ++word)
    {
        message.Words[word] = Detail::LoadDriverWord(payload + (word << 3U));
    }
    message.Reply = request ? System::CapabilityHandle{Detail::LoadDriverWord(frame + EchoHeaderBytes),
                                                       Detail::LoadDriverWord(frame + EchoHeaderBytes + 8U)}
                            : System::CapabilityHandle{};
    return true;
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_DRIVER_FRAME_HPP_INCLUDED
