// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_DISPLAY_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_DISPLAY_FRAME_HPP_INCLUDED

#include "Abi/IpcCanonicalFrame.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// Display composition v2: console and pointer share an Endpoint with split rights. Manage permits
/// ConsoleWrite and reserved SetMode; Invoke permits Pointer and Query. Canonical asynchronous
/// frames are encoded field by field; failure is atomic. Surface methods remain reserved on
/// separate per-session Endpoints. ConsoleWrite: {count,0,0,0}, 192 zero-padded CP437 bytes, no
/// attachments. Query: {0,0,0,0}, one Invoke-copy reply Endpoint. Pointer:
/// {epoch,sequence,kind,x|y<<32,buttons,source,wheel,0}, no attachment. kind is relative (signed
/// 32-bit deltas), absolute (unsigned normalized 0..65535 coordinates), or snapshot (zero motion).
/// Wheel is signed 32 bits, high half zero. Sequence and epoch are nonzero; sequence orders the
/// complete stream. Reply: {transaction,kind,version,features,width|height<<32,columns|rows<<32,
/// epoch,pointerX|pointerY<<32,lastAppliedSequence,0}. With DisplayFeatureMetrics, append
/// {scanoutPolicy,fillBytes,fillUs,latestPixels,latestUs,slowestPixels,slowestUs}.
/// Version/interface and requests stay v2. The decoder accepts both reply sizes; absent extension
/// fields are zero. Query fences composition.
inline constexpr Foundation::UInt64 DisplayInterfaceLow{0x4153'5452'4449'5302ULL};  // "ASTRDIS", version 2
inline constexpr Foundation::UInt64 DisplayInterfaceHigh{0x504F'494E'5445'5202ULL}; // "POINTER", 2
inline constexpr Foundation::UInt32 DisplayConsoleWriteMethod{1U};
inline constexpr Foundation::UInt32 DisplayQueryMethod{2U};
inline constexpr Foundation::UInt32 DisplayReplyMethod{3U};
/// Reserved: the console Endpoint's mode setting.
inline constexpr Foundation::UInt32 DisplaySetModeMethod{4U};
/// Reserved: the surface Endpoint's registration of a client Frame.
inline constexpr Foundation::UInt32 DisplayCreateSurfaceMethod{5U};
/// Reserved: the surface Endpoint's {surface, damage, believed epoch}.
inline constexpr Foundation::UInt32 DisplayPresentMethod{6U};
/// Reserved: the surface Endpoint's release of a surface.
inline constexpr Foundation::UInt32 DisplayDestroySurfaceMethod{7U};
/// Reserved: the broker's notice that the epoch changed.
inline constexpr Foundation::UInt32 DisplayEpochChangedMethod{8U};
/// A reply's kind.
inline constexpr Foundation::UInt64 DisplayDone{1U};
inline constexpr Foundation::UInt64 DisplayRefused{2U};
inline constexpr Foundation::UInt32 DisplayPointerMethod{9U};
inline constexpr Foundation::UInt64 DisplayRelativeMotion{1U};
inline constexpr Foundation::UInt64 DisplayAbsoluteMotion{2U};
inline constexpr Foundation::UInt64 DisplayPointerSnapshot{3U};
inline constexpr Foundation::UInt64 DisplayProtocolVersion{2U};
inline constexpr Foundation::UInt64 DisplayFeaturePointer{2U};
/// Optional extension: scanout policy, startup fill and full-console pass metrics.
inline constexpr Foundation::UInt64 DisplayFeatureMetrics{4U};
/// Durations are valid microseconds (counter and start-block scale available).
inline constexpr Foundation::UInt64 DisplayFeatureTiming{8U};
inline constexpr Foundation::UInt64 DisplayScanoutWc{1U};
inline constexpr Foundation::UInt64 DisplayScanoutUc{2U};
/// Console support; pointer support is the separate DisplayFeaturePointer bit.
inline constexpr Foundation::UInt64 DisplayFeatureConsole{1U};
inline constexpr Foundation::Size DisplayMaximumWriteBytes{192U};
inline constexpr Foundation::Size DisplayPayloadWords{4U};
/// Session pass metrics follow the existing metrics.
inline constexpr Foundation::UInt64 DisplayFeatureSession{16U};
inline constexpr Foundation::Size DisplayMetricsReplyWords{17U};
inline constexpr Foundation::Size DisplayMetricsReplyPayloadBytes{DisplayMetricsReplyWords << 3U};
inline constexpr Foundation::Size DisplayReplyWords{19U};
inline constexpr Foundation::Size DisplayPayloadBytes{DisplayPayloadWords << 3U};
inline constexpr Foundation::Size DisplayLegacyReplyWords{10U};
inline constexpr Foundation::Size DisplayLegacyReplyPayloadBytes{DisplayLegacyReplyWords << 3U};
inline constexpr Foundation::Size DisplayReplyPayloadBytes{DisplayReplyWords << 3U};
inline constexpr Foundation::Size DisplayConsoleWritePayloadBytes{DisplayPayloadBytes + DisplayMaximumWriteBytes};
/// ConsoleWrite: header, four words and the text area.
inline constexpr Foundation::Size DisplayConsoleWriteBytes{EchoHeaderBytes + DisplayConsoleWritePayloadBytes};
/// Query: header, reply attachment, four words.
inline constexpr Foundation::Size DisplayQueryBytes{EchoHeaderBytes + EchoAttachmentBytes + DisplayPayloadBytes};
/// Reply capacity: header, seventeen words (legacy replies use ten).
inline constexpr Foundation::Size DisplayReplyBytes{EchoHeaderBytes + DisplayReplyPayloadBytes};
/// The largest frame of the protocol.
inline constexpr Foundation::Size DisplayMaximumFrameBytes{DisplayConsoleWriteBytes};
/// The kernel's payload limit (MessageCodec MaximumPayloadBytes) and the
/// pager's delivery buffer (DeliveryCapacityBytes).
inline constexpr Foundation::Size DisplayKernelPayloadLimit{256U};
inline constexpr Foundation::Size DisplayDeliveryCapacity{552U};
static_assert(DisplayConsoleWritePayloadBytes <= DisplayKernelPayloadLimit);
static_assert(DisplayReplyPayloadBytes <= DisplayKernelPayloadLimit);
static_assert((DisplayConsoleWritePayloadBytes % 8U) == 0U);
static_assert((DisplayMaximumFrameBytes + EchoDeliveryHeaderBytes) <= DisplayDeliveryCapacity);
static_assert(DisplayConsoleWritePayloadBytes <= 255U);
/// The sealed selector of the broker's console Endpoint and its tag in the broker's WaitSet.
inline constexpr Foundation::UInt64 DisplaySealedSelector{0x4453'5053'4552'5643ULL};
inline constexpr Foundation::UInt64 DisplayServiceWaitTag{0x4453'5053U};

/// What a Query's answer reports.
struct DisplayInfo
{
    Foundation::UInt64 Version;
    Foundation::UInt64 Features;
    Foundation::UInt32 Width;
    Foundation::UInt32 Height;
    Foundation::UInt32 Columns;
    Foundation::UInt32 Rows;
    Foundation::UInt64 Epoch;
    Foundation::UInt32 PointerX{0U};
    Foundation::UInt32 PointerY{0U};
    Foundation::UInt64 PointerSequence{0U};
    Foundation::UInt64 ScanoutPolicy{0U};
    Foundation::UInt64 FillBytes{0U};
    Foundation::UInt64 FillMicroseconds{0U};
    Foundation::UInt64 LatestPixels{0U};
    Foundation::UInt64 LatestMicroseconds{0U};
    Foundation::UInt64 SlowestPixels{0U};
    Foundation::UInt64 SlowestMicroseconds{0U};
    Foundation::UInt64 SessionPixels{0U};
    Foundation::UInt64 SessionMicroseconds{0U};
};

/// One decoded display frame.
struct DisplayMessage
{
    Foundation::UInt64 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[DisplayReplyWords];
    /// A ConsoleWrite's byte count (also Words[0]).
    Foundation::Size Count;
    /// A ConsoleWrite's bytes: Count of them, in the delivery's text area, which
    /// the message does not own; null for the other frames.
    const Foundation::UInt8* Text;
    /// A Query's reply capability, as the receiver's space holds it.
    System::CapabilityHandle Reply;
};

namespace Detail
{
inline void StoreDisplayWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] inline Foundation::UInt64 LoadDisplayWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}

/// The attachments a method carries: one for a Query, none for the others
/// (a ConsoleWrite, a reply, a pointer); 3 for an unknown or reserved method.
[[nodiscard]] inline Foundation::Size CountDisplayAttachments(const Foundation::UInt64 method) noexcept
{
    if (method == DisplayQueryMethod)
    {
        return 1U;
    }
    return ((method == DisplayConsoleWriteMethod) || (method == DisplayReplyMethod) || (method == DisplayPointerMethod))
               ? 0U
               : 3U;
}

[[nodiscard]] inline Foundation::Size DisplayPayloadSize(const Foundation::UInt64 method) noexcept
{
    if (method == DisplayConsoleWriteMethod)
    {
        return DisplayConsoleWritePayloadBytes;
    }
    if (method == DisplayPointerMethod)
    {
        return 64U;
    }
    return (method == DisplayReplyMethod) ? DisplayReplyPayloadBytes : DisplayPayloadBytes;
}

/// Writes the header of a frame of method with payloadBytes, and the
/// attachment if reply is given; returns the frame's size.
inline Foundation::Size WriteDisplayHeader(Foundation::UInt8* const frame, const Foundation::UInt32 method,
                                           const Foundation::UInt64 transaction,
                                           const System::CapabilityHandle* const reply,
                                           const Foundation::Size payloadBytes) noexcept
{
    const Foundation::Size attachments = (reply != nullptr) ? 1U : 0U;
    EchoAttachment record{};
    if (reply != nullptr)
    {
        record =
            EchoAttachment{.Handle = *reply, .Rights = EchoInvokeRight, .Mode = EchoCopy, .Type = EchoEndpointType};
    }
    WriteEchoFrame(frame, &record, attachments);
    const Foundation::Size total = EchoHeaderBytes + (attachments << 5U) + payloadBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(total >> (index << 3U));
    }
    frame[12U] = static_cast<Foundation::UInt8>(payloadBytes);
    frame[13U] = 0U;
    frame[14U] = static_cast<Foundation::UInt8>(payloadBytes);
    frame[15U] = 0U;
    StoreDisplayWord(frame + 16U, DisplayInterfaceLow);
    StoreDisplayWord(frame + 24U, DisplayInterfaceHigh);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[32U + index] = static_cast<Foundation::UInt8>(method >> (index << 3U));
    }
    StoreDisplayWord(frame + 40U, transaction);
    return total;
}
} // Namespace Detail

/// Validates pointer wire words independently of broker state.
[[nodiscard]] inline bool IsDisplayPointer(const Foundation::UInt64* words) noexcept
{
    return (words[0U] != 0U) && (words[1U] != 0U) && (words[2U] >= DisplayRelativeMotion) &&
           (words[2U] <= DisplayPointerSnapshot) && (words[4U] <= 7U) && (words[5U] >= 1U) &&
           (words[5U] <= 0xFFFFFFFFU) && (words[6U] <= 0xFFFFFFFFU) &&
           ((words[2U] != DisplayAbsoluteMotion) ||
            ((static_cast<Foundation::UInt32>(words[3U]) <= 65535U) && ((words[3U] >> 32U) <= 65535U))) &&
           ((words[2U] != DisplayPointerSnapshot) || (words[3U] == 0U));
}

/// Writes eight validated pointer words, no attachment; zero on refusal.
[[nodiscard]] inline Foundation::Size WriteDisplayPointer(Foundation::UInt8* frame,
                                                          const Foundation::UInt64 transaction,
                                                          const Foundation::UInt64* words) noexcept
{
    if (!IsDisplayPointer(words) || (transaction == 0U))
    {
        return 0U;
    }
    const Foundation::Size total = Detail::WriteDisplayHeader(frame, DisplayPointerMethod, transaction, nullptr, 64U);
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        Detail::StoreDisplayWord(frame + EchoHeaderBytes + index * 8U, words[index]);
    }
    return total;
}

/// Writes a ConsoleWrite of count bytes (1 to DisplayMaximumWriteBytes) into
/// frame (DisplayConsoleWriteBytes): no attachment, the count, three zero
/// words, the bytes and zero padding. Returns the frame's size, or zero,
/// writing nothing, for a null bytes or a count out of range.
inline Foundation::Size WriteDisplayConsoleWrite(Foundation::UInt8* const frame, const Foundation::UInt64 transaction,
                                                 const Foundation::UInt8* const bytes,
                                                 const Foundation::Size count) noexcept
{
    if ((bytes == nullptr) || (count == 0U) || (count > DisplayMaximumWriteBytes))
    {
        return 0U;
    }
    const Foundation::Size total = Detail::WriteDisplayHeader(frame, DisplayConsoleWriteMethod, transaction, nullptr,
                                                              DisplayConsoleWritePayloadBytes);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes;
    Detail::StoreDisplayWord(payload, count);
    for (Foundation::Size word{1U}; word < DisplayPayloadWords; ++word)
    {
        Detail::StoreDisplayWord(payload + (word << 3U), 0U);
    }
    for (Foundation::Size index{0U}; index < DisplayMaximumWriteBytes; ++index)
    {
        payload[DisplayPayloadBytes + index] = (index < count) ? bytes[index] : Foundation::UInt8{0U};
    }
    return total;
}

/// Writes a Query into frame (DisplayQueryBytes), with a copy of reply
/// (Invoke) as its attachment. Returns the frame's size.
inline Foundation::Size WriteDisplayQuery(Foundation::UInt8* const frame, const Foundation::UInt64 transaction,
                                          const System::CapabilityHandle& reply) noexcept
{
    const Foundation::Size total =
        Detail::WriteDisplayHeader(frame, DisplayQueryMethod, transaction, &reply, DisplayPayloadBytes);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes + EchoAttachmentBytes;
    for (Foundation::Size word{0U}; word < DisplayPayloadWords; ++word)
    {
        Detail::StoreDisplayWord(payload + (word << 3U), 0U);
    }
    return total;
}

/// Writes the reply to the request of transaction into frame
/// (DisplayReplyBytes): kind DisplayDone or DisplayRefused, with info's fields
/// (written as given). Returns the frame's size, or zero, writing nothing, for
/// another kind.
inline Foundation::Size WriteDisplayReply(Foundation::UInt8* const frame, const Foundation::UInt64 transaction,
                                          const Foundation::UInt64 kind, const DisplayInfo& info) noexcept
{
    if ((kind != DisplayDone) && (kind != DisplayRefused))
    {
        return 0U;
    }
    const bool metrics = (info.Features & DisplayFeatureMetrics) != 0U;
    const Foundation::Size payloadBytes = !metrics ? DisplayLegacyReplyPayloadBytes
                                          : (info.Features & DisplayFeatureSession) != 0U
                                              ? DisplayReplyPayloadBytes
                                              : DisplayMetricsReplyPayloadBytes;
    const Foundation::Size total =
        Detail::WriteDisplayHeader(frame, DisplayReplyMethod, transaction, nullptr, payloadBytes);
    const Foundation::UInt64 words[DisplayReplyWords]{
        transaction,
        kind,
        info.Version,
        info.Features,
        static_cast<Foundation::UInt64>(info.Width) | (static_cast<Foundation::UInt64>(info.Height) << 32U),
        static_cast<Foundation::UInt64>(info.Columns) | (static_cast<Foundation::UInt64>(info.Rows) << 32U),
        info.Epoch,
        static_cast<Foundation::UInt64>(info.PointerX) | (static_cast<Foundation::UInt64>(info.PointerY) << 32U),
        info.PointerSequence,
        0U,
        info.ScanoutPolicy,
        info.FillBytes,
        info.FillMicroseconds,
        info.LatestPixels,
        info.LatestMicroseconds,
        info.SlowestPixels,
        info.SlowestMicroseconds,
        info.SessionPixels,
        info.SessionMicroseconds};
    Foundation::UInt8* const payload = frame + EchoHeaderBytes;
    for (Foundation::Size word{0U}; word < (payloadBytes >> 3U); ++word)
    {
        Detail::StoreDisplayWord(payload + (word << 3U), words[word]);
    }
    return total;
}

/// Decode a kernel delivery; canonical header, attachments and method payload
/// are checked before publishing any borrowed view. Rights and stale epochs are
/// broker state checks, not decoder checks.
[[nodiscard]] inline bool ReadDisplayFrame(const Foundation::UInt8* const delivery,
                                           const Foundation::Size deliveryBytes, DisplayMessage& message) noexcept
{
    if ((delivery == nullptr) || (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes)))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::UInt64 method = frame[32U] | (static_cast<Foundation::UInt64>(frame[33U]) << 8U);
    const Foundation::Size expected = Detail::CountDisplayAttachments(method);
    const Foundation::Size attachments = frame[81U];
    const Foundation::Size payloadBytes =
        method == DisplayReplyMethod ? frame[12U] : Detail::DisplayPayloadSize(method);
    if ((method == DisplayReplyMethod) && (payloadBytes != DisplayLegacyReplyPayloadBytes) &&
        (payloadBytes != DisplayReplyPayloadBytes) && (payloadBytes != DisplayMetricsReplyPayloadBytes))
    {
        return false;
    }
    if ((Detail::LoadDisplayWord(frame + 16U) != DisplayInterfaceLow) ||
        (Detail::LoadDisplayWord(frame + 24U) != DisplayInterfaceHigh) || (frame[34U] != 0U) || (frame[35U] != 0U) ||
        (method < DisplayConsoleWriteMethod) || ((method > DisplayReplyMethod) && (method != DisplayPointerMethod)) ||
        (frame[80U] != 0U) || (attachments != expected) || (frame[12U] != payloadBytes) || (frame[13U] != 0U) ||
        (deliveryBytes != (EchoDeliveryHeaderBytes + EchoHeaderBytes + (attachments << 5U) + payloadBytes)) ||
        ((attachments == 1U) && (frame[EchoHeaderBytes + 27U] != EchoEndpointType)))
    {
        return false;
    }
    if (Detail::LoadDisplayWord(frame + 40U) == 0U)
    {
        return false;
    }
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
    const Foundation::Size total = deliveryBytes - EchoDeliveryHeaderBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        if (frame[8U + index] != static_cast<Foundation::UInt8>(total >> (index * 8U)))
        {
            return false;
        }
    }
    if ((frame[14U] != payloadBytes) || (frame[15U] != 0U))
    {
        return false;
    }
    if (attachments == 1U)
    {
        const Foundation::UInt8* const record = frame + EchoHeaderBytes;
        if ((Detail::LoadDisplayWord(record) == 0U) ||
            (Detail::LoadDisplayWord(record + 16U) != 0xFFFF'FFFF'FFFF'FFFFULL) || (record[24U] != EchoInvokeRight) ||
            (record[25U] != 0U) || (record[26U] != EchoCopy) || (record[28U] != 0U) || (record[29U] != 0U) ||
            (record[30U] != 0U) || (record[31U] != 0U))
        {
            return false;
        }
    }
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    const Foundation::Size wordCount = payloadBytes >> 3U;
    Foundation::UInt64 words[DisplayReplyWords]{};
    for (Foundation::Size word{0U}; (word < wordCount) && (word < DisplayReplyWords); ++word)
    {
        words[word] = Detail::LoadDisplayWord(payload + (word << 3U));
    }
    const Foundation::UInt64 transaction = Detail::LoadDisplayWord(frame + 40U);
    bool valid{false};
    if (method == DisplayConsoleWriteMethod)
    {
        valid = (words[0U] >= 1U) && (words[0U] <= DisplayMaximumWriteBytes) && (words[1U] == 0U) &&
                (words[2U] == 0U) && (words[3U] == 0U);
        const Foundation::UInt8* const text = payload + DisplayPayloadBytes;
        for (Foundation::Size index{static_cast<Foundation::Size>(words[0U] & 0xFFU)};
             valid && (index < DisplayMaximumWriteBytes); ++index)
        {
            valid = text[index] == 0U;
        }
    }
    else if (method == DisplayQueryMethod)
    {
        valid = (words[0U] == 0U) && (words[1U] == 0U) && (words[2U] == 0U) && (words[3U] == 0U);
    }
    else if (method == DisplayPointerMethod)
    {
        valid = IsDisplayPointer(words);
    }
    else
    {
        valid = (words[0U] == transaction) && ((words[1U] == DisplayDone) || (words[1U] == DisplayRefused)) &&
                (words[9U] == 0U) &&
                (payloadBytes == ((words[3U] & DisplayFeatureMetrics) == 0U   ? DisplayLegacyReplyPayloadBytes
                                  : (words[3U] & DisplayFeatureSession) != 0U ? DisplayReplyPayloadBytes
                                                                              : DisplayMetricsReplyPayloadBytes));
    }
    if (!valid)
    {
        return false;
    }
    message.Method = method;
    message.Transaction = transaction;
    for (Foundation::Size word{0U}; word < DisplayReplyWords; ++word)
    {
        message.Words[word] = words[word];
    }
    const bool write = method == DisplayConsoleWriteMethod;
    message.Count = write ? static_cast<Foundation::Size>(words[0U]) : 0U;
    message.Text = write ? (payload + DisplayPayloadBytes) : nullptr;
    message.Reply = (attachments == 1U)
                        ? System::CapabilityHandle{Detail::LoadDisplayWord(frame + EchoHeaderBytes),
                                                   Detail::LoadDisplayWord(frame + EchoHeaderBytes + 8U)}
                        : System::CapabilityHandle{};
    return true;
}

/// A decoded reply's fields, unpacked.
[[nodiscard]] inline DisplayInfo UnpackDisplayInfo(const DisplayMessage& message) noexcept
{
    return DisplayInfo{.Version = message.Words[2U],
                       .Features = message.Words[3U],
                       .Width = static_cast<Foundation::UInt32>(message.Words[4U]),
                       .Height = static_cast<Foundation::UInt32>(message.Words[4U] >> 32U),
                       .Columns = static_cast<Foundation::UInt32>(message.Words[5U]),
                       .Rows = static_cast<Foundation::UInt32>(message.Words[5U] >> 32U),
                       .Epoch = message.Words[6U],
                       .PointerX = static_cast<Foundation::UInt32>(message.Words[7U]),
                       .PointerY = static_cast<Foundation::UInt32>(message.Words[7U] >> 32U),
                       .PointerSequence = message.Words[8U],
                       .ScanoutPolicy = message.Words[10U],
                       .FillBytes = message.Words[11U],
                       .FillMicroseconds = message.Words[12U],
                       .LatestPixels = message.Words[13U],
                       .LatestMicroseconds = message.Words[14U],
                       .SlowestPixels = message.Words[15U],
                       .SlowestMicroseconds = message.Words[16U],
                       .SessionPixels = message.Words[17U],
                       .SessionMicroseconds = message.Words[18U]};
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_DISPLAY_FRAME_HPP_INCLUDED
