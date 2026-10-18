// SPDX-License-Identifier: Apache-2.0

#include "User/Session/SessionFrame.hpp"
#include "User/Text/Utf8.hpp"

#include "Abi/IpcCanonicalFrame.hpp"

namespace Asteria::User::Session
{
namespace
{
using Component::EchoAttachment;
using Component::EchoAttachmentBytes;
using Component::EchoDeliveryHeaderBytes;
using Component::EchoHeaderBytes;
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

constexpr Size WordCounts[SessionLastMethod + 1U]{0U,  4U,  8U, 4U, 4U, 6U, 4U, 4U, 4U, 22U, 5U,
                                                  10U, 32U, 4U, 6U, 4U, 4U, 4U, 4U, 4U, 4U,  4U};
constexpr UInt64 Low32{0xFFFF'FFFFULL};
constexpr UInt8 Copy{2U};

void Store64(UInt8* const bytes, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] UInt64 Load64(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

/// Words from an array (the encoder's) or straight from payload bytes (the
/// Decoder's), so both validate with one template and the decoder needs no copy until the frame is
/// accepted.
class ArrayWords final
{
public:
    explicit ArrayWords(const UInt64* const words) noexcept : m_words{words}
    {
    }
    [[nodiscard]] UInt64 operator[](const Size index) const noexcept
    {
        return m_words[index];
    }

private:
    const UInt64* m_words;
};

class ByteWords final
{
public:
    explicit ByteWords(const UInt8* const bytes) noexcept : m_bytes{bytes}
    {
    }
    [[nodiscard]] UInt64 operator[](const Size index) const noexcept
    {
        return Load64(m_bytes + (index * 8U));
    }

private:
    const UInt8* m_bytes;
};

/// Words [first, end) of a message or event are zero.
template <typename TWords> [[nodiscard]] bool Zero(const TWords& words, const Size first, const Size end) noexcept
{
    for (Size index{first}; index < end; ++index)
    {
        if (words[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool Within(const UInt64 value, const UInt64 minimum, const UInt64 maximum) noexcept
{
    return (value >= minimum) && (value <= maximum);
}

/// Both 32-bit halves of a pair in [minimum, maximum].
[[nodiscard]] bool PairWithin(const UInt64 pair, const UInt64 minimum, const UInt64 maximum) noexcept
{
    return Within(pair & Low32, minimum, maximum) && Within(pair >> 32U, minimum, maximum);
}

[[nodiscard]] bool Refusal(const UInt64 reason) noexcept
{
    return Within(reason, 1U, SessionLastRefusal);
}

[[nodiscard]] bool Surface(const UInt64 surface) noexcept
{
    return Within(surface, 1U, SessionMaximumSurfaces);
}

[[nodiscard]] bool Pool(const UInt64 pool) noexcept
{
    return Within(pool, 1U, SessionMaximumPools);
}

[[nodiscard]] bool Image(const UInt64 image) noexcept
{
    return Within(image, 1U, SessionMaximumImages);
}

/// Bytes [0, length) of the text in words [first, first + capacity / 8) are well-formed printable
/// UTF-8 and every byte after them is zero.
template <typename TWords>
[[nodiscard]] bool Text(const TWords& words, const Size first, const Size capacity, const UInt64 length) noexcept
{
    if (length > capacity)
    {
        return false;
    }
    Text::Utf8Decoder decoder{};
    for (Size word{0U}; word < (capacity >> 3U); ++word)
    {
        const UInt64 value = words[first + word];
        for (Size byte{0U}; byte < 8U; ++byte)
        {
            const UInt64 code = (value >> (byte * 8U)) & 0xFFU;
            const bool inside = ((word * 8U) + byte) < length;
            if (inside ? (!decoder.Feed(static_cast<UInt8>(code)) ||
                          (decoder.Complete() && !Text::IsPrintable(decoder.Point())))
                       : (code != 0U))
            {
                return false;
            }
        }
    }
    return decoder.Complete();
}

template <typename TWords>
[[nodiscard]] bool ValidEvent(const UInt64 kind, const TWords& words, const Size first) noexcept
{
    const UInt64 w0 = words[first];
    const UInt64 w1 = words[first + 1U];
    const UInt64 w2 = words[first + 2U];
    const UInt64 w3 = words[first + 3U];
    switch (kind)
    {
    case static_cast<UInt64>(SessionEventKind::Welcome):
        return (w0 == SessionProtocolVersion) && Within(w2, 1U, 2U) && PairWithin(w3, 1U, SessionMaximumOutputExtent);
    case static_cast<UInt64>(SessionEventKind::PoolReady):
        return Pool(w0) && (w1 == 0U) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::PoolRefused):
        return Pool(w0) && Refusal(w1) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::PoolReleased):
    case static_cast<UInt64>(SessionEventKind::PoolDestroyed):
        return Pool(w0) && (w1 == 0U) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::ImageReturned):
        return Within(w0, 1U, (UInt64{1U} << SessionMaximumImages) - 1U) && (w1 == 0U) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::ImageRefused):
        return Image(w0) && Refusal(w1) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::FrameShown):
        return Surface(w0) && (w1 != 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::Arrange):
        return Surface(w0) && (w1 != 0U) && PairWithin(w2, 1U, SessionMaximumExtent) &&
               (w3 <= (SessionArrangeFocused | SessionArrangeMaximized | SessionArrangeResizing));
    case static_cast<UInt64>(SessionEventKind::CloseAsked):
        return Surface(w0) && (w1 == 0U) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::KeyFocus):
        return Surface(w0) && (w1 <= 1U) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::PointerEnter):
    case static_cast<UInt64>(SessionEventKind::PointerLeave):
        return Surface(w0) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::PointerMotion):
    case static_cast<UInt64>(SessionEventKind::PointerButtons):
        return Surface(w0) && (w2 <= 7U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::Wheel):
        return Surface(w0) && Within(w1, 1U, Low32) && (w2 == 0U) && (w3 == 0U);
    case static_cast<UInt64>(SessionEventKind::Key):
        return Surface(w0) && Within(w1, SessionKeyDown, SessionKeyUp) && Within(w2, 4U, 0xE7U) && (w3 <= 0xFFU);
    case static_cast<UInt64>(SessionEventKind::RequestRefused):
        return Within(w0, SessionCreditMethod, SessionSetSubtitleMethod) && Refusal(w1) && (w2 != 0U) && (w3 == 0U);
    default:
        return false;
    }
}

template <typename TWords> [[nodiscard]] bool ValidPresent(const TWords& words) noexcept
{
    const UInt64 image = words[1U];
    const UInt64 flags = words[3U];
    const UInt64 count = words[5U];
    if (!Surface(words[0U]) || (image > SessionMaximumImages) || (words[2U] == 0U) || (flags > SessionWantFrameShown) ||
        (count > SessionMaximumDamage) || ((image == 0U) && ((count != 0U) || (flags != 0U))))
    {
        return false;
    }
    for (Size index{0U}; index < SessionMaximumDamage; ++index)
    {
        const UInt64 rect = words[6U + index];
        if (index >= count)
        {
            if (rect != 0U)
            {
                return false;
            }
            continue;
        }
        const UInt64 x = rect & 0xFFFFU;
        const UInt64 y = (rect >> 16U) & 0xFFFFU;
        const UInt64 width = (rect >> 32U) & 0xFFFFU;
        const UInt64 height = rect >> 48U;
        if ((width == 0U) || (height == 0U) || ((x + width) > SessionMaximumExtent) ||
            ((y + height) > SessionMaximumExtent))
        {
            return false;
        }
    }
    return true;
}

template <typename TWords> [[nodiscard]] bool ValidCreatePool(const TWords& words, const Size attachments) noexcept
{
    const UInt64 frames = words[2U];
    if (!Pool(words[0U]) || !Within(frames, 1U, SessionMaximumPoolFrames) || !Within(attachments, 1U, 4U) ||
        (words[3U] >= frames) || ((words[3U] + attachments) > frames))
    {
        return false;
    }
    UInt64 total{0U};
    for (Size index{0U}; index < SessionMaximumPoolFrames; ++index)
    {
        const UInt64 count = (words[4U + (index >> 2U)] >> ((index & 3U) * 16U)) & 0xFFFFU;
        if (index < frames ? !Within(count, 1U, SessionMaximumFramePages) : (count != 0U))
        {
            return false;
        }
        total += count;
    }
    if (words[1U] != total)
    {
        return false;
    }
    return true;
}

template <typename TWords> [[nodiscard]] bool ValidDefineImage(const TWords& words) noexcept
{
    const UInt64 offset = words[2U];
    const UInt64 width = words[3U] & Low32;
    const UInt64 height = words[3U] >> 32U;
    const UInt64 stride = words[4U];
    return Pool(words[0U]) && Image(words[1U]) && ((offset & (SessionImageAlignment - 1U)) == 0U) &&
           Within(width, 1U, SessionMaximumExtent) && Within(height, 1U, SessionMaximumExtent) &&
           Within(stride, width, SessionMaximumExtent) && Within(words[5U], 1U, 2U) &&
           SessionImageFits(offset, static_cast<UInt32>(width), static_cast<UInt32>(height),
                            static_cast<UInt32>(stride), SessionMaximumPoolBytes);
}

template <typename TWords> [[nodiscard]] bool ValidSizeHints(const TWords& words) noexcept
{
    for (Size index{1U}; index < 5U; ++index)
    {
        if (!PairWithin(words[index], 0U, SessionMaximumExtent))
        {
            return false;
        }
    }
    const UInt64 minimum = words[1U];
    const UInt64 maximum = words[2U];
    const bool width = ((maximum & Low32) == 0U) || ((maximum & Low32) >= (minimum & Low32));
    const bool height = ((maximum >> 32U) == 0U) || ((maximum >> 32U) >= (minimum >> 32U));
    return Surface(words[0U]) && width && height;
}

template <typename TWords> [[nodiscard]] bool ValidEvents(const TWords& words, const Size attachments) noexcept
{
    const UInt64 count = words[1U] >> 56U;
    const UInt64 kinds = words[1U];
    if ((words[0U] == 0U) || !Within(count, 1U, SessionMaximumEvents) || (words[2U] > 1U) || (attachments != 0U))
    {
        return false;
    }
    for (Size index{0U}; index < SessionMaximumEvents; ++index)
    {
        const UInt64 kind = (kinds >> (index * 8U)) & 0xFFU;
        if (index >= count)
        {
            if ((kind != 0U) || ((index < SessionMaximumEvents) && !Zero(words, 4U + (index * 4U), 8U + (index * 4U))))
            {
                return false;
            }
            continue;
        }
        if (!ValidEvent(kind, words, 4U + (index * 4U)))
        {
            return false;
        }
    }
    return true;
}

template <typename TWords>
[[nodiscard]] bool ValidMessage(const UInt32 method, const TWords& words, const Size attachments) noexcept
{
    switch (method)
    {
    case SessionCreditMethod:
        return (attachments == 1U) && (words[0U] != 0U) && Within(words[1U], 1U, SessionMaximumEvents) &&
               Zero(words, 2U, 4U);
    case SessionCreatePoolMethod:
        return ValidCreatePool(words, attachments);
    case SessionAddPoolFramesMethod:
        return Pool(words[0U]) && Within(words[1U], 0U, SessionMaximumPoolFrames - 1U) && Within(attachments, 1U, 4U) &&
               ((words[1U] + attachments) <= SessionMaximumPoolFrames) && Zero(words, 2U, 4U);
    case SessionDestroyPoolMethod:
        return (attachments == 0U) && Pool(words[0U]) && Zero(words, 1U, 4U);
    case SessionDefineImageMethod:
        return (attachments == 0U) && ValidDefineImage(words);
    case SessionForgetImageMethod:
        return (attachments == 0U) && Image(words[0U]) && Zero(words, 1U, 4U);
    case SessionCreateSurfaceMethod:
        return (attachments == 0U) && Surface(words[0U]) && (words[1U] == SessionToplevelRole) && Zero(words, 2U, 4U);
    case SessionDestroySurfaceMethod:
        return (attachments == 0U) && Surface(words[0U]) && Zero(words, 1U, 4U);
    case SessionPresentMethod:
        return (attachments == 0U) && ValidPresent(words);
    case SessionSetSizeHintsMethod:
        return (attachments == 0U) && ValidSizeHints(words);
    case SessionSetSubtitleMethod:
        return (attachments == 0U) && Surface(words[0U]) && Text(words, 2U, SessionMaximumSubtitleBytes, words[1U]);
    case SessionEventsMethod:
        return ValidEvents(words, attachments);
    case SessionEpochChangedMethod:
        return (attachments <= 1U) && (words[0U] != 0U) && PairWithin(words[1U], 1U, SessionMaximumOutputExtent) &&
               Within(words[2U], 1U, 2U);
    case SessionRegisterClientMethod:
        return (attachments == 1U) && Within(words[1U], SessionClassSystemModule, SessionClassVolumeBundle) &&
               Within(words[2U], 1U, SessionMaximumNameBytes) && (words[3U] == 0U) &&
               Text(words, 4U, SessionMaximumNameBytes, words[2U]);
    case SessionDropClientMethod:
        return (attachments == 1U) && IsSessionClient(words[0U]) && Zero(words, 1U, 4U);
    case SessionFaceChangedMethod:
        return (attachments == 1U) && (words[0U] != 0U) && Zero(words, 1U, 4U);
    case SessionQueryMethod:
        return (attachments == 1U) && (words[0U] != 0U) && Zero(words, 1U, 4U);
    case SessionUserLaunchMethod:
    case SessionReadyMethod:
    case SessionNewTerminalMethod:
        return (attachments == 0U) && (words[0U] != 0U) && Zero(words, 1U, 4U);
    case SessionReplyMethod:
        return (words[0U] != 0U) && Within(words[1U], 1U, 5U) && (words[3U] == 0U) && (attachments <= 1U) &&
               ((attachments == 0U) || ((words[1U] == 1U) && IsSessionClient(words[2U]))) &&
               ((words[1U] == 1U) || (words[2U] == 0U));
    default:
        return false;
    }
}

/// The type and rights of a method's attachment index.
struct AttachmentForm
{
    UInt8 Type;
    UInt16 Rights;
};

[[nodiscard]] AttachmentForm FormOf(const UInt32 method) noexcept
{
    if ((method == SessionCreatePoolMethod) || (method == SessionAddPoolFramesMethod))
    {
        return AttachmentForm{.Type = Component::EchoFrameType, .Rights = SessionMapRight};
    }
    if (method == SessionReplyMethod)
    {
        return AttachmentForm{.Type = Component::EchoEndpointType,
                              .Rights = static_cast<UInt16>(Component::EchoInvokeRight | SessionDuplicateRight)};
    }
    return AttachmentForm{.Type = Component::EchoEndpointType, .Rights = Component::EchoInvokeRight};
}

void WriteHeader(UInt8* const frame, const UInt32 method, const UInt64 transaction,
                 const System::CapabilityHandle* const attachments, const Size count, const Size payloadBytes) noexcept
{
    const AttachmentForm form = FormOf(method);
    EchoAttachment records[SessionMaximumAttachments]{};
    for (Size index{0U}; index < count; ++index)
    {
        records[index] =
            EchoAttachment{.Handle = attachments[index], .Rights = form.Rights, .Mode = Copy, .Type = form.Type};
    }
    Component::WriteEchoFrame(frame, records, count);
    const Size total = EchoHeaderBytes + (count * EchoAttachmentBytes) + payloadBytes;
    for (Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<UInt8>(total >> (index * 8U));
        frame[32U + index] = static_cast<UInt8>(method >> (index * 8U));
    }
    frame[12U] = static_cast<UInt8>(payloadBytes);
    frame[13U] = static_cast<UInt8>(payloadBytes >> 8U);
    frame[14U] = static_cast<UInt8>(payloadBytes);
    frame[15U] = static_cast<UInt8>(payloadBytes >> 8U);
    Store64(frame + 16U, SessionInterfaceLow);
    Store64(frame + 24U, SessionInterfaceHigh);
    Store64(frame + 40U, transaction);
}
} // Namespace

Size SessionWordCount(const UInt32 method) noexcept
{
    return (method <= SessionLastMethod) ? WordCounts[method] : 0U;
}

bool IsSessionMessage(const UInt32 method, const UInt64* const words, const Size attachments) noexcept
{
    return (words != nullptr) && (SessionWordCount(method) != 0U) &&
           ValidMessage(method, ArrayWords{words}, attachments);
}

bool IsSessionEvent(const SessionEvent& event) noexcept
{
    return ValidEvent(static_cast<UInt64>(event.Kind), ArrayWords{event.Words}, 0U);
}

Size WriteSessionFrame(UInt8* const frame, const UInt32 method, const UInt64 transaction, const UInt64* const words,
                       const System::CapabilityHandle* const attachments, const Size attachmentCount) noexcept
{
    const Size wordCount = SessionWordCount(method);
    if ((frame == nullptr) || (transaction == 0U) || (attachmentCount > SessionMaximumAttachments) ||
        ((attachmentCount != 0U) && (attachments == nullptr)) || !IsSessionMessage(method, words, attachmentCount) ||
        ((method == SessionReplyMethod) && (words[0U] != transaction)))
    {
        return 0U;
    }
    for (Size index{0U}; index < attachmentCount; ++index)
    {
        if (attachments[index].Selector == 0U)
        {
            return 0U;
        }
    }
    const Size payloadBytes = wordCount * 8U;
    WriteHeader(frame, method, transaction, attachments, attachmentCount, payloadBytes);
    UInt8* const payload = frame + EchoHeaderBytes + (attachmentCount * EchoAttachmentBytes);
    for (Size index{0U}; index < wordCount; ++index)
    {
        Store64(payload + (index * 8U), words[index]);
    }
    return EchoHeaderBytes + (attachmentCount * EchoAttachmentBytes) + payloadBytes;
}

bool ReadSessionFrame(const UInt8* const delivery, const Size deliveryBytes, SessionMessage& message) noexcept
{
    if ((delivery == nullptr) || (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes)))
    {
        return false;
    }
    const UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const UInt32 method = static_cast<UInt32>(frame[32U]) | (static_cast<UInt32>(frame[33U]) << 8U) |
                          (static_cast<UInt32>(frame[34U]) << 16U) | (static_cast<UInt32>(frame[35U]) << 24U);
    const Size wordCount = SessionWordCount(method);
    const Size attachments = frame[81U];
    const Size payloadBytes = wordCount * 8U;
    const Size total = EchoHeaderBytes + (attachments * EchoAttachmentBytes) + payloadBytes;
    if ((wordCount == 0U) || (attachments > SessionMaximumAttachments) ||
        (deliveryBytes != (EchoDeliveryHeaderBytes + total)) || (Load64(frame + 16U) != SessionInterfaceLow) ||
        (Load64(frame + 24U) != SessionInterfaceHigh) || (Load64(frame + 40U) == 0U))
    {
        return false;
    }
    // Every other header byte is the canonical frame's: version, sizes of
    // the header, interface version, deadline, trace, no descriptors, the
    // asynchronous kind, no cancellation, reserved zero.
    for (Size index{0U}; index < EchoHeaderBytes; ++index)
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
    for (Size index{0U}; index < 4U; ++index)
    {
        if (frame[8U + index] != static_cast<UInt8>(total >> (index * 8U)))
        {
            return false;
        }
    }
    if ((frame[12U] != static_cast<UInt8>(payloadBytes)) || (frame[13U] != static_cast<UInt8>(payloadBytes >> 8U)) ||
        (frame[14U] != static_cast<UInt8>(payloadBytes)) || (frame[15U] != static_cast<UInt8>(payloadBytes >> 8U)))
    {
        return false;
    }
    const AttachmentForm form = FormOf(method);
    for (Size index{0U}; index < attachments; ++index)
    {
        const UInt8* const record = frame + EchoHeaderBytes + (index * EchoAttachmentBytes);
        const UInt16 rights = static_cast<UInt16>(record[24U] | (static_cast<UInt16>(record[25U]) << 8U));
        if ((Load64(record) == 0U) || (Load64(record + 16U) != 0xFFFF'FFFF'FFFF'FFFFULL) || (rights != form.Rights) ||
            (record[26U] != Copy) || (record[27U] != form.Type) || (record[28U] != 0U) || (record[29U] != 0U) ||
            (record[30U] != 0U) || (record[31U] != 0U))
        {
            return false;
        }
    }
    const UInt8* const payload = frame + EchoHeaderBytes + (attachments * EchoAttachmentBytes);
    const ByteWords words{payload};
    if (!ValidMessage(method, words, attachments) ||
        ((method == SessionReplyMethod) && (words[0U] != Load64(frame + 40U))))
    {
        return false;
    }
    message.Method = method;
    message.Transaction = Load64(frame + 40U);
    for (Size index{0U}; index < SessionMaximumWords; ++index)
    {
        message.Words[index] = (index < wordCount) ? words[index] : 0U;
    }
    message.WordCount = wordCount;
    message.AttachmentCount = attachments;
    for (Size index{0U}; index < SessionMaximumAttachments; ++index)
    {
        message.Attachments[index] =
            (index < attachments) ? Component::ReadEchoHandle(delivery, index) : System::CapabilityHandle{};
    }
    return true;
}

Size WriteSessionEvents(UInt8* const frame, const UInt64 transaction, const UInt64 epoch,
                        const SessionEvent* const events, const Size count, const UInt64 flags,
                        const UInt64 dropped) noexcept
{
    if ((frame == nullptr) || (transaction == 0U) || (epoch == 0U) || (count == 0U) || (count > SessionMaximumEvents) ||
        (events == nullptr) || (flags > SessionEventsMorePending))
    {
        return 0U;
    }
    UInt64 kinds{0U};
    for (Size index{0U}; index < count; ++index)
    {
        if (!IsSessionEvent(events[index]))
        {
            return 0U;
        }
        kinds |= static_cast<UInt64>(events[index].Kind) << (index * 8U);
    }
    const Size payloadBytes = SessionMaximumWords * 8U;
    WriteHeader(frame, SessionEventsMethod, transaction, nullptr, 0U, payloadBytes);
    UInt8* const payload = frame + EchoHeaderBytes;
    Store64(payload, epoch);
    Store64(payload + 8U, kinds | (static_cast<UInt64>(count) << 56U));
    Store64(payload + 16U, flags);
    Store64(payload + 24U, dropped);
    for (Size index{0U}; index < SessionMaximumEvents; ++index)
    {
        for (Size word{0U}; word < SessionEventWords; ++word)
        {
            Store64(payload + 32U + (((index * SessionEventWords) + word) * 8U),
                    (index < count) ? events[index].Words[word] : 0U);
        }
    }
    return EchoHeaderBytes + payloadBytes;
}

Size SessionEventCount(const SessionMessage& message) noexcept
{
    return message.Method == SessionEventsMethod ? static_cast<Size>(message.Words[1U] >> 56U) : 0U;
}

bool GetSessionEvent(const SessionMessage& message, const Size index, SessionEvent& event) noexcept
{
    if ((message.Method != SessionEventsMethod) || (index >= SessionEventCount(message)) ||
        (index >= SessionMaximumEvents))
    {
        return false;
    }
    event.Kind = static_cast<SessionEventKind>((message.Words[1U] >> (index * 8U)) & 0xFFU);
    for (Size word{0U}; word < SessionEventWords; ++word)
    {
        event.Words[word] = message.Words[4U + (index * SessionEventWords) + word];
    }
    return true;
}
} // Namespace Asteria::User::Session
