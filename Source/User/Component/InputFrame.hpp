// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_INPUT_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_INPUT_FRAME_HPP_INCLUDED

#include "User/Component/EchoFrame.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "User/Input/InputEvent.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Component
{
inline constexpr Foundation::UInt64 InputInterfaceLow{0x4153'5449'4E50'5554ULL};
inline constexpr Foundation::UInt64 InputInterfaceHigh{0x0000'0000'0000'0001ULL};
inline constexpr Foundation::UInt64 InputSealedSelector{0x494E'5055'5453'4541ULL};
inline constexpr Foundation::UInt64 InputSourceSelector{0x494E'5053'0000'0000ULL};
inline constexpr Foundation::UInt64 InputClientSelector{0x494E'5043'0000'0000ULL};
inline constexpr Foundation::UInt64 InputServiceWaitTag{0x494E'5054U};
inline constexpr Foundation::UInt64 InputProtocolVersion{3U};
inline constexpr Foundation::UInt64 InputBrokerPointer{1U};
inline constexpr Foundation::Size InputMaximumEvents{8U};
/// One PS/2 seat, each catalog xHCI owner and the USB host group.
inline constexpr Foundation::UInt64 InputMaximumSources{6U};
static_assert(InputMaximumSources == Abi::PagerXhciControllers + 2U);
// Root terminal, desktop seat and dedicated login.
inline constexpr Foundation::UInt64 InputMaximumClients{3U};
inline constexpr Foundation::Size InputPayloadBytes{32U + InputMaximumEvents * 24U};
inline constexpr Foundation::Size InputMaximumFrameBytes{EchoHeaderBytes + 2U * EchoAttachmentBytes +
                                                         InputPayloadBytes};
inline constexpr Foundation::UInt32 InputRegisterSourceMethod{1U};
inline constexpr Foundation::UInt32 InputRegisterClientMethod{2U};
inline constexpr Foundation::UInt32 InputPublishMethod{3U};
inline constexpr Foundation::UInt32 InputCreditMethod{4U};
inline constexpr Foundation::UInt32 InputRequestFocusMethod{5U};
inline constexpr Foundation::UInt32 InputSetFocusMethod{6U};
inline constexpr Foundation::UInt32 InputAttentionCreditMethod{7U};
inline constexpr Foundation::UInt32 InputQueryMethod{8U};
inline constexpr Foundation::UInt32 InputReplyMethod{9U};
inline constexpr Foundation::UInt32 InputEventsMethod{10U};
inline constexpr Foundation::UInt32 InputAttentionMethod{11U};
inline constexpr Foundation::UInt32 InputFocusRequestMethod{12U};
/// Manage-only route: {input epoch, display epoch, last display sequence, 0}.
/// Reply attachment first, Invoke-only sink second. One attachment and zero
/// display epoch/sequence clears the route. Replaces and closes the old sink.
inline constexpr Foundation::UInt32 InputSetPointerRouteMethod{13U};
// Reply is shared on the wire; callers select the schema of their outstanding request. Driver Query
// fields are not broker ids.
enum class InputReplySchema : Foundation::UInt8
{
    Broker,
    DriverQuery
};

/// Manager replaces a session receiver; replacement obtains immutable counter bases before posting
/// credits.
inline constexpr Foundation::UInt32 InputRebindClientMethod{14U};
inline constexpr Foundation::UInt32 InputResumeMethod{15U};
inline constexpr Foundation::UInt32 InputBaselineMethod{16U};
enum class InputAnswer : Foundation::UInt64
{
    Done = 1U,
    Refused = 2U,
    Exhausted = 3U,
    Busy = 4U,
    Restarted = 5U,
};

/// All frames have four words and eight field-encoded event slots. Requests: {epoch, argument, 0,
/// 0}, plus one Invoke-copy reply Endpoint. Publish's argument is its count; Credit's is 1..8;
/// SetFocus's is a client id or 0; RegisterSource's is 0 for a new source or the id of a source to
/// register again (its driver restarted), RegisterClient's word 1 bit 0 selects brokerPointer (keys
/// only, one session client). SetFocus word 2 bit 0 locks manager focus for the root's emergency
/// console. Replies: {epoch, answer, id, count}; a successful registration alone has an
/// Invoke|Duplicate-copy Endpoint. Events: {epoch, client, count, horizon}; horizon is the last
/// routed pointer sequence preceding every event in the batch. PointerRoute word 7 is the queued
/// session key count. Attention: {epoch, source, root shortcut usage (0=secure attention), 0};
/// FocusRequest: {epoch, client, 0, 0}, no attachments. Decoder failure is atomic. The delivery
/// metadata is kernel-authenticated.
struct InputMessage
{
    Foundation::UInt32 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[4U];
    const Foundation::UInt8* Events;
    Foundation::Size Count;
    System::CapabilityHandle Reply;
    System::CapabilityHandle PointerSink;
};

namespace Detail
{
[[nodiscard]] inline Foundation::UInt32 InputLoad32(const Foundation::UInt8* bytes) noexcept
{
    Foundation::UInt32 value{0U};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        value |= static_cast<Foundation::UInt32>(bytes[index]) << (index * 8U);
    }
    return value;
}
inline void InputStore32(Foundation::UInt8* bytes, Foundation::UInt32 value) noexcept
{
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}
[[nodiscard]] inline Foundation::Int32 InputSigned(Foundation::UInt32 value) noexcept
{
    return (value <= 0x7FFF'FFFFU) ? static_cast<Foundation::Int32>(value)
                                   : -1 - static_cast<Foundation::Int32>(0xFFFF'FFFFU - value);
}
} // Namespace Detail

[[nodiscard]] inline Input::InputEvent ReadInputEvent(const Foundation::UInt8* bytes) noexcept
{
    return Input::InputEvent{
        .Kind = static_cast<Input::EventKind>(bytes[0U]),
        .Modifiers = bytes[1U],
        .Usage = static_cast<Foundation::UInt16>(bytes[2U] | (static_cast<Foundation::UInt16>(bytes[3U]) << 8U)),
        .Source = Detail::InputLoad32(bytes + 4U),
        .Dx = Detail::InputSigned(Detail::InputLoad32(bytes + 8U)),
        .Dy = Detail::InputSigned(Detail::InputLoad32(bytes + 12U)),
        .Buttons = bytes[16U],
        .Reserved = {bytes[17U], bytes[18U], bytes[19U]},
        .Wheel = Detail::InputSigned(Detail::InputLoad32(bytes + 20U))};
}

[[nodiscard]] inline bool IsInputEvent(const Input::InputEvent& event) noexcept
{
    if ((event.Reserved[0U] | event.Reserved[1U] | event.Reserved[2U]) != 0U)
    {
        return false;
    }
    switch (event.Kind)
    {
    case Input::EventKind::KeyDown:
    case Input::EventKind::KeyUp:
        return (event.Usage >= 4U) && (event.Usage <= 0xE7U) && (event.Dx == 0) && (event.Dy == 0) &&
               (event.Buttons == 0U) && (event.Wheel == 0);
    case Input::EventKind::Motion:
        return (event.Modifiers == 0U) && (event.Usage == 0U) && (event.Buttons == 0U) && (event.Wheel == 0);
    case Input::EventKind::Buttons:
        return (event.Modifiers == 0U) && (event.Usage == 0U) && (event.Dx == 0) && (event.Dy == 0) &&
               (event.Buttons <= 7U) && (event.Wheel == 0);
    case Input::EventKind::Wheel:
        return (event.Modifiers == 0U) && (event.Usage == 0U) && (event.Dx == 0) && (event.Dy == 0) &&
               (event.Buttons == 0U);
    }
    return false;
}

inline void WriteInputEvent(Foundation::UInt8* bytes, const Input::InputEvent& event) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(event.Kind);
    bytes[1U] = event.Modifiers;
    bytes[2U] = static_cast<Foundation::UInt8>(event.Usage);
    bytes[3U] = static_cast<Foundation::UInt8>(event.Usage >> 8U);
    Detail::InputStore32(bytes + 4U, event.Source);
    Detail::InputStore32(bytes + 8U, static_cast<Foundation::UInt32>(event.Dx));
    Detail::InputStore32(bytes + 12U, static_cast<Foundation::UInt32>(event.Dy));
    bytes[16U] = event.Buttons;
    for (Foundation::Size index{17U}; index < 20U; ++index)
    {
        bytes[index] = 0U;
    }
    Detail::InputStore32(bytes + 20U, static_cast<Foundation::UInt32>(event.Wheel));
}

/// Writes up to InputMaximumFrameBytes. Use the reader's method-specific
/// word contract; malformed fields are refused by the reader, including
/// unused event slots. A null attachment means no attachment.
[[nodiscard]] inline Foundation::Size WriteInputFrame(Foundation::UInt8* frame, Foundation::UInt32 method,
                                                      Foundation::UInt64 transaction,
                                                      const Foundation::UInt64 (&words)[4U],
                                                      const Input::InputEvent* events, Foundation::Size count,
                                                      const System::CapabilityHandle* attachment = nullptr) noexcept
{
    if ((method < InputRegisterSourceMethod) || (method > InputBaselineMethod) || (count > InputMaximumEvents) ||
        ((count != 0U) && (events == nullptr)))
    {
        return 0U;
    }
    const Foundation::Size attachments = (attachment == nullptr) ? 0U : 1U;
    const EchoAttachment record{
        .Handle = attachment == nullptr ? System::CapabilityHandle{} : *attachment,
        .Rights = static_cast<Foundation::UInt16>(EchoInvokeRight | ((method == InputReplyMethod) ? 0x100U : 0U)),
        .Mode = EchoCopy,
        .Type = EchoEndpointType};
    WriteEchoFrame(frame, &record, attachments);
    const Foundation::Size total = EchoHeaderBytes + attachments * EchoAttachmentBytes + InputPayloadBytes;
    Detail::InputStore32(frame + 8U, static_cast<Foundation::UInt32>(total));
    frame[12U] = static_cast<Foundation::UInt8>(InputPayloadBytes);
    frame[14U] = static_cast<Foundation::UInt8>(InputPayloadBytes);
    System::StoreWord(frame + 16U, InputInterfaceLow);
    System::StoreWord(frame + 24U, InputInterfaceHigh);
    Detail::InputStore32(frame + 32U, method);
    System::StoreWord(frame + 40U, transaction);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes + attachments * EchoAttachmentBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        System::StoreWord(payload + index * 8U, words[index]);
    }
    for (Foundation::Size index{0U}; index < InputMaximumEvents; ++index)
    {
        if (index < count)
        {
            WriteInputEvent(payload + 32U + index * 24U, events[index]);
        }
        else
        {
            for (Foundation::Size byte{0U}; byte < 24U; ++byte)
            {
                payload[32U + index * 24U + byte] = 0U;
            }
        }
    }
    return total;
}

inline constexpr Foundation::Size InputQueryPowerShift{32U};
[[nodiscard]] inline Foundation::UInt32 InputQueryInterfaces(const InputMessage& message) noexcept
{
    return static_cast<Foundation::UInt32>(message.Words[2U]);
}
[[nodiscard]] inline Foundation::UInt32 InputQueryPoweredOff(const InputMessage& message) noexcept
{
    return static_cast<Foundation::UInt32>(message.Words[2U] >> InputQueryPowerShift);
}
[[nodiscard]] inline Foundation::Size
WriteInputDriverQueryReply(Foundation::UInt8* frame, const Foundation::UInt64 transaction,
                           const Foundation::UInt64 epoch, const Foundation::UInt32 interfaces,
                           const Foundation::UInt32 poweredOff, const Foundation::UInt64 events) noexcept
{
    const Foundation::UInt64 words[4U]{
        epoch, static_cast<Foundation::UInt64>(InputAnswer::Done),
        interfaces | (static_cast<Foundation::UInt64>(poweredOff) << InputQueryPowerShift), events};
    return WriteInputFrame(frame, InputReplyMethod, transaction, words, nullptr, 0U);
}

/// Set or clear a route without exporting display management authority.
[[nodiscard]] inline Foundation::Size WriteInputPointerRoute(Foundation::UInt8* frame, Foundation::UInt64 transaction,
                                                             const Foundation::UInt64 (&words)[4U],
                                                             const System::CapabilityHandle& reply,
                                                             const System::CapabilityHandle* sink) noexcept
{
    const Foundation::Size count = sink == nullptr ? 1U : 2U;
    const EchoAttachment records[2U]{
        {.Handle = reply, .Rights = EchoInvokeRight, .Mode = EchoCopy, .Type = EchoEndpointType},
        {.Handle = sink == nullptr ? System::CapabilityHandle{} : *sink,
         .Rights = EchoInvokeRight,
         .Mode = EchoCopy,
         .Type = EchoEndpointType},
    };
    const Foundation::Size ignored =
        WriteInputFrame(frame, InputSetPointerRouteMethod, transaction, words, nullptr, 0U, &reply);
    static_cast<void>(ignored);
    // Move the fixed payload for the extra attachment, then rebuild header.
    if (count == 2U)
    {
        for (Foundation::Size index{InputPayloadBytes}; index != 0U; --index)
        {
            frame[EchoHeaderBytes + 2U * EchoAttachmentBytes + index - 1U] =
                frame[EchoHeaderBytes + EchoAttachmentBytes + index - 1U];
        }
    }
    WriteEchoFrame(frame, records, count);
    const Foundation::Size total = EchoHeaderBytes + count * EchoAttachmentBytes + InputPayloadBytes;
    Detail::InputStore32(frame + 8U, static_cast<Foundation::UInt32>(total));
    frame[12U] = static_cast<Foundation::UInt8>(InputPayloadBytes);
    frame[14U] = static_cast<Foundation::UInt8>(InputPayloadBytes);
    System::StoreWord(frame + 16U, InputInterfaceLow);
    System::StoreWord(frame + 24U, InputInterfaceHigh);
    Detail::InputStore32(frame + 32U, InputSetPointerRouteMethod);
    System::StoreWord(frame + 40U, transaction);
    return total;
}

[[nodiscard]] inline bool ReadInputFrame(const Foundation::UInt8* delivery, Foundation::Size bytes,
                                         InputMessage& message,
                                         const InputReplySchema replySchema = InputReplySchema::Broker) noexcept
{
    if ((delivery == nullptr) || (bytes < EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::UInt32 method = Detail::InputLoad32(frame + 32U);
    const Foundation::Size attachments = frame[81U];
    if ((method < InputRegisterSourceMethod) || (method > InputBaselineMethod) ||
        (attachments > ((method == InputSetPointerRouteMethod) ? 2U : 1U)) ||
        (bytes != EchoDeliveryHeaderBytes + EchoHeaderBytes + attachments * EchoAttachmentBytes + InputPayloadBytes) ||
        (Detail::InputLoad32(frame + 8U) != bytes - EchoDeliveryHeaderBytes) ||
        (System::LoadWord(frame + 16U) != InputInterfaceLow) || (System::LoadWord(frame + 24U) != InputInterfaceHigh))
    {
        return false;
    }
    // Pin every other canonical header byte (including flags, deadline,
    // descriptors, payload size and all reserved fields).
    for (Foundation::Size index{0U}; index < EchoHeaderBytes; ++index)
    {
        if (((index >= 8U) && (index < 36U)) || ((index >= 40U) && (index < 48U)) || (index == 81U))
        {
            continue;
        }
        if (frame[index] != Abi::CanonicalAsynchronousFrame[index])
        {
            return false;
        }
    }
    if ((frame[12U] != InputPayloadBytes) || (frame[13U] != 0U) || (frame[14U] != InputPayloadBytes) ||
        (frame[15U] != 0U))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < attachments; ++index)
    {
        const Foundation::UInt8* const record = frame + EchoHeaderBytes + index * EchoAttachmentBytes;
        if ((System::LoadWord(record) == 0U) || (System::LoadWord(record + 16U) != 0xFFFF'FFFF'FFFF'FFFFULL) ||
            (record[24U] != EchoInvokeRight) || (record[25U] != ((method == InputReplyMethod) ? 1U : 0U)) ||
            (record[26U] != EchoCopy) || (record[27U] != EchoEndpointType) || (Detail::InputLoad32(record + 28U) != 0U))
        {
            return false;
        }
    }
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes + attachments * EchoAttachmentBytes;
    Foundation::UInt64 words[4U]{};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        words[index] = System::LoadWord(payload + index * 8U);
    }
    Foundation::Size count{0U};
    bool valid = (words[0U] != 0U) && (System::LoadWord(frame + 40U) != 0U);
    if (method == InputSetPointerRouteMethod)
    {
        valid = valid && (words[3U] == 0U) &&
                (((attachments == 2U) && (words[1U] != 0U)) ||
                 ((attachments == 1U) && (words[1U] == 0U) && (words[2U] == 0U)));
    }
    else if (method == InputRebindClientMethod || method == InputResumeMethod)
    {
        valid = valid && attachments == 1U && words[2U] <= (method == InputRebindClientMethod ? 1U : 0U) &&
                words[3U] == 0U &&
                (method == InputResumeMethod ? words[1U] == 0U : words[1U] >= 1U && words[1U] <= InputMaximumClients);
    }
    else if (method == InputBaselineMethod)
    {
        valid = valid && attachments == 0U && words[3U] <= 7U;
    }
    else if (method <= InputQueryMethod)
    {
        valid = valid && (attachments == 1U) && (words[2U] <= (method == InputSetFocusMethod ? 1U : 0U)) &&
                (words[3U] == 0U);
        if ((method == InputPublishMethod) || (method == InputCreditMethod))
        {
            valid = valid && (words[1U] >= 1U) && (words[1U] <= InputMaximumEvents);
            count = (method == InputPublishMethod) ? static_cast<Foundation::Size>(words[1U]) : 0U;
        }
        else
        {
            // SetFocus names a client, RegisterSource a source to register
            // again: the larger bound; the broker refuses an unknown one.
            const auto maximum = method == InputSetFocusMethod         ? InputMaximumClients
                                 : method == InputRegisterSourceMethod ? InputMaximumSources
                                 : method == InputRegisterClientMethod ? InputBrokerPointer
                                                                       : 0U;
            valid = valid && (words[1U] <= maximum);
        }
    }
    else if (method == InputReplyMethod)
    {
        if (replySchema == InputReplySchema::DriverQuery)
        {
            // low 32 bits: interface count, high 32: root-port power map;
            // word 3: full u64 event count. No capabilities are imported.
            valid = valid && (attachments == 0U) && (words[1U] >= 1U) && (words[1U] <= 5U) &&
                    ((words[1U] == static_cast<Foundation::UInt64>(InputAnswer::Done)) ||
                     ((words[2U] == 0U) && (words[3U] == 0U)));
        }
        else
        {
            valid = valid && (words[1U] >= 1U) && (words[1U] <= 5U) && (words[2U] <= InputMaximumSources) &&
                    (words[3U] <= 0xFFFF'FFFFU) && ((words[1U] == 1U) || ((words[2U] == 0U) && (words[3U] == 0U))) &&
                    ((attachments == 0U) ||
                     ((words[1U] == 1U) && (words[2U] != 0U) && (words[3U] == InputProtocolVersion)));
        }
    }
    else if (method == InputEventsMethod)
    {
        valid = valid && (attachments == 0U) && (words[1U] >= 1U) && (words[1U] <= InputMaximumClients) &&
                (words[2U] >= 1U) && (words[2U] <= InputMaximumEvents);
        count = valid ? static_cast<Foundation::Size>(words[2U]) : 0U;
    }
    else
    {
        // Attention names a source, FocusRequest a client: the larger bound.
        valid = valid && (attachments == 0U) && (words[1U] >= 1U) &&
                (words[1U] <= (method == InputAttentionMethod ? InputMaximumSources : InputMaximumClients)) &&
                (words[2U] == 0U || (method == InputAttentionMethod && words[2U] >= 66U && words[2U] <= 68U)) &&
                (words[3U] == 0U);
    }
    for (Foundation::Size index{0U}; valid && (index < InputMaximumEvents); ++index)
    {
        const Foundation::UInt8* const event = payload + 32U + index * 24U;
        if (index < count)
        {
            if ((event[0U] < 1U) || (event[0U] > 5U))
            {
                return false;
            }
            const Input::InputEvent decoded = ReadInputEvent(event);
            valid = IsInputEvent(decoded) && ((method != InputEventsMethod) ||
                                              ((decoded.Source >= 1U) && (decoded.Source <= InputMaximumSources)));
        }
        else
        {
            for (Foundation::Size byte{0U}; byte < 24U; ++byte)
            {
                valid = valid && (event[byte] == 0U);
            }
        }
    }
    if (!valid)
    {
        return false;
    }
    message.Method = method;
    message.Transaction = System::LoadWord(frame + 40U);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        message.Words[index] = words[index];
    }
    message.Events = payload + 32U;
    message.Count = count;
    message.Reply = attachments == 0U ? System::CapabilityHandle{} : ReadEchoHandle(delivery, 0U);
    message.PointerSink = attachments == 2U ? ReadEchoHandle(delivery, 1U) : System::CapabilityHandle{};
    return true;
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_INPUT_FRAME_HPP_INCLUDED
