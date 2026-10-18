// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_WIRE_HPP_INCLUDED
#define ASTERIA_USER_SESSION_WIRE_HPP_INCLUDED
#include "Abi/IpcCanonicalFrame.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/ObjectRequests.hpp"
namespace Asteria::User::Session::Wire
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using System::CapabilityHandle;
using Component::EchoAttachment;
using Component::EchoAttachmentBytes;
using Component::EchoCopy;
using Component::EchoDeliveryHeaderBytes;
using Component::EchoHeaderBytes;
using Component::ReadEchoHandle;
using Component::WriteEchoFrame;
struct ArrayWords
{
    const UInt64* Words;
    [[nodiscard]] UInt64 operator[](Size i) const noexcept
    {
        return Words[i];
    }
};
struct ByteWords
{
    const UInt8* Bytes;
    [[nodiscard]] UInt64 operator[](Size i) const noexcept
    {
        return System::LoadWord(Bytes + i * 8U);
    }
};
struct Form
{
    UInt8 Type;
    UInt16 Rights;
};
struct Message
{
    UInt32 Method;
    UInt64 Transaction;
    UInt64 Words[32U];
    Size WordCount;
    Size AttachmentCount;
    CapabilityHandle Attachments[4U];
};
[[nodiscard]] inline UInt32 Load32(const UInt8* p) noexcept
{
    return static_cast<UInt32>(p[0U]) | (static_cast<UInt32>(p[1U]) << 8U) | (static_cast<UInt32>(p[2U]) << 16U) |
           (static_cast<UInt32>(p[3U]) << 24U);
}
inline void Store32(UInt8* p, UInt32 v) noexcept
{
    for (Size i{0U}; i < 4U; ++i)
    {
        p[i] = static_cast<UInt8>(v >> (i * 8U));
    }
}
template <class Words> [[nodiscard]] bool Zero(const Words& w, Size first, Size end) noexcept
{
    for (Size i{first}; i < end; ++i)
    {
        if (w[i] != 0U)
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] inline bool Pair(UInt64 word, UInt64 low, UInt64 high) noexcept
{
    return ((word & 0xFFFF'FFFFU) >= low) && ((word & 0xFFFF'FFFFU) <= high) && ((word >> 32U) >= low) &&
           ((word >> 32U) <= high);
}
/// Field-encoded canonical AIPC (IpcWireFormat.md), shared by NEW decision
/// 9/10 codecs. Validate before any output write, never copy a full frame on
/// the target stack. Codec supplies sizes, attachment forms and word rules.
template <class Codec, class Words>
[[nodiscard]] Size Write(UInt8* frame, UInt32 method, UInt64 txn, const Words& words, const CapabilityHandle* handles,
                         Size attachments) noexcept
{
    const Size count = Codec::Count(method);
    const Size payload = count * 8U;
    const Size total = EchoHeaderBytes + attachments * EchoAttachmentBytes + payload;
    if ((frame == nullptr) || (txn == 0U) || (count == 0U) || (count > 32U) || (attachments > 4U) || (total > 504U) ||
        ((attachments != 0U) && (handles == nullptr)) || !Codec::Valid(method, words, attachments, txn))
    {
        return 0U;
    }
    for (Size i{0U}; i < attachments; ++i)
    {
        if (handles[i].Selector == 0U)
        {
            return 0U;
        }
    }
    EchoAttachment records[4U]{};
    for (Size i{0U}; i < attachments; ++i)
    {
        const Form form = Codec::Attachment(method, i);
        records[i] = {handles[i], form.Rights, EchoCopy, form.Type};
    }
    WriteEchoFrame(frame, records, attachments);
    Store32(frame + 8U, static_cast<UInt32>(total));
    Store32(frame + 32U, method);
    frame[12U] = static_cast<UInt8>(payload);
    frame[13U] = static_cast<UInt8>(payload >> 8U);
    frame[14U] = frame[12U];
    frame[15U] = frame[13U];
    System::StoreWord(frame + 16U, Codec::Low);
    System::StoreWord(frame + 24U, Codec::High);
    System::StoreWord(frame + 40U, txn);
    UInt8* const out = frame + EchoHeaderBytes + attachments * EchoAttachmentBytes;
    for (Size i{0U}; i < count; ++i)
    {
        System::StoreWord(out + i * 8U, words[i]);
    }
    return total;
}
template <class Codec> [[nodiscard]] bool Read(const UInt8* delivery, Size bytes, Message& message) noexcept
{
    if ((delivery == nullptr) || (bytes < EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return false;
    }
    const UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const UInt32 method = Load32(frame + 32U);
    const Size count = Codec::Count(method);
    const Size attachments = frame[81U];
    const Size payload = count * 8U;
    const Size total = EchoHeaderBytes + attachments * EchoAttachmentBytes + payload;
    const UInt64 txn = System::LoadWord(frame + 40U);
    if ((count == 0U) || (count > 32U) || (attachments > 4U) || (total > 504U) ||
        (bytes != EchoDeliveryHeaderBytes + total) || (txn == 0U) || (System::LoadWord(frame + 16U) != Codec::Low) ||
        (System::LoadWord(frame + 24U) != Codec::High) || (Load32(frame + 8U) != total))
    {
        return false;
    }
    for (Size i{0U}; i < EchoHeaderBytes; ++i)
    {
        if (((i >= 8U) && (i < 12U)) || ((i >= 16U) && (i < 36U)) || ((i >= 40U) && (i < 48U)) || (i == 81U))
        {
            continue;
        }
        const UInt8 expected =
            ((i == 12U) || (i == 14U))
                ? static_cast<UInt8>(payload)
                : (((i == 13U) || (i == 15U)) ? static_cast<UInt8>(payload >> 8U) : Abi::CanonicalAsynchronousFrame[i]);
        if (frame[i] != expected)
        {
            return false;
        }
    }
    for (Size i{0U}; i < attachments; ++i)
    {
        const UInt8* const r = frame + EchoHeaderBytes + i * EchoAttachmentBytes;
        const Form f = Codec::Attachment(method, i);
        if ((System::LoadWord(r) == 0U) || (System::LoadWord(r + 16U) != 0xFFFF'FFFF'FFFF'FFFFULL) ||
            (static_cast<UInt16>(r[24U] | (static_cast<UInt16>(r[25U]) << 8U)) != f.Rights) || (r[26U] != EchoCopy) ||
            (r[27U] != f.Type) || (Load32(r + 28U) != 0U))
        {
            return false;
        }
    }
    const ByteWords words{frame + EchoHeaderBytes + attachments * EchoAttachmentBytes};
    if (!Codec::Valid(method, words, attachments, txn))
    {
        return false;
    }
    message.Method = method;
    message.Transaction = txn;
    message.WordCount = count;
    message.AttachmentCount = attachments;
    for (Size i{0U}; i < 32U; ++i)
    {
        message.Words[i] = i < count ? words[i] : 0U;
    }
    for (Size i{0U}; i < 4U; ++i)
    {
        message.Attachments[i] = i < attachments ? ReadEchoHandle(delivery, i) : CapabilityHandle{};
    }
    return true;
}
} // namespace Asteria::User::Session::Wire
#endif
