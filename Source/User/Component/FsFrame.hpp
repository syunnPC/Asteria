// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_FS_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_FS_FRAME_HPP_INCLUDED

#include "Abi/IpcCanonicalFrame.hpp"
#include "Foundation/Types.hpp"
#include "User/Filesystem/Names.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// V2, eight words. Requests carry the instance tag in word 0 and view id in word 4.
/// Lookup/Create/MakeDirectory carry the parent slot/generation in words 5/6 and name length in
/// word 1. Other object operations carry slot/generation in words 1/2; word 3 is a page index or
/// directory cursor. ReadPages' word 7 is the page count, at most FsReadPagesCapacity and the
/// attached Frame; each page reuses the window. Commit uses transaction/flags in words 1/2,
/// QueryCommit transaction in word 1; both carry the root slot/generation in words 5/6. Replies:
/// {transaction, status, detail, extra, kind, bytes, object, generation}. Lookup/Create
/// detail/extra are slot/slot-generation. ReadDirectory also returns one UTF-8 name; detail is the
/// next cursor. Stat carries all four metadata fields. InstallView: {tag, owner (0 = sender),
/// source slot, source slot-generation, source view (0 = volume root), rights, 0, 0}; RemoveView
/// names its view in word 4; ResetViews has only the tag. The separate sealed manager face
/// authorizes Install/Remove by Invoke, Reset by Manage. No method accepts a path or a passable
/// file attachment.
inline constexpr Foundation::UInt64 FsInterfaceLow{0x4153'5452'4653'5602ULL};
inline constexpr Foundation::UInt64 FsInterfaceHigh{0x4649'4C45'5359'5303ULL};
inline constexpr Foundation::UInt32 FsOpenMethod{1U};
inline constexpr Foundation::UInt32 FsCreateMethod{2U};
inline constexpr Foundation::UInt32 FsReadMethod{3U};
inline constexpr Foundation::UInt32 FsWriteMethod{4U};
inline constexpr Foundation::UInt32 FsCommitMethod{5U};
inline constexpr Foundation::UInt32 FsQueryCommitMethod{6U};
inline constexpr Foundation::UInt32 FsReplyMethod{7U};
inline constexpr Foundation::UInt32 FsStatMethod{8U};
inline constexpr Foundation::UInt32 FsLookupMethod{FsOpenMethod};
inline constexpr Foundation::UInt32 FsReadPagesMethod{FsReadMethod};
inline constexpr Foundation::UInt32 FsReadDirectoryMethod{9U};
inline constexpr Foundation::UInt32 FsMakeDirectoryMethod{10U};
inline constexpr Foundation::UInt32 FsCloseMethod{11U};
inline constexpr Foundation::UInt32 FsInstallViewMethod{12U};
inline constexpr Foundation::UInt32 FsRemoveViewMethod{13U};
inline constexpr Foundation::UInt32 FsPassFileMethod{14U};      // Reserved
inline constexpr Foundation::UInt32 FsObserveWritesMethod{16U}; // Reserved visibility
inline constexpr Foundation::UInt8 FsFileAttachmentKind{0xFEU}; // Reserved
inline constexpr Foundation::UInt32 FsResetViewsMethod{15U};
inline constexpr Foundation::UInt64 FsReadRight{1U};
inline constexpr Foundation::UInt64 FsWriteRight{2U};
inline constexpr Foundation::UInt64 FsExecRight{4U};
inline constexpr Foundation::UInt64 FsAllRights{FsReadRight | FsWriteRight | FsExecRight};
inline constexpr Foundation::UInt64 FsRightsDenied{11U};
inline constexpr Foundation::Size FsOpenSlots{64U};
inline constexpr Foundation::Size FsViewCapacity{2U};
inline constexpr Foundation::UInt64 FsManagerSelector{0x4653'4D41'4E41'4745ULL};
inline constexpr Foundation::UInt64 FsManagerWaitTag{0x4653'4D47U};
/// PowerOff.md: {tag,0,0,0,0,0,0,0}; root-only Manage on the sealed manager face
/// flushes and closes admission.
inline constexpr Foundation::UInt32 FsQuiesceMethod{17U};
/// Commit's flags: the service crashes before it writes its superblock, or
/// just after.
inline constexpr Foundation::UInt64 FsCrashBeforeSuperblock{1U};
inline constexpr Foundation::UInt64 FsCrashAfterSuperblock{2U};
/// A reply's kind.
inline constexpr Foundation::UInt64 FsDone{1U};
inline constexpr Foundation::UInt64 FsCommitted{2U};
inline constexpr Foundation::UInt64 FsNotCommitted{3U};
inline constexpr Foundation::UInt64 FsRefused{4U};
inline constexpr Foundation::UInt64 FsCorrupt{5U};
inline constexpr Foundation::UInt64 FsStaleObject{6U};
inline constexpr Foundation::UInt64 FsNotFound{7U};
inline constexpr Foundation::UInt64 FsRestarted{8U};
inline constexpr Foundation::UInt64 FsFailed{9U};
inline constexpr Foundation::UInt64 FsUnsupported{10U};
/// The formats an FsUnsupported answer to QueryCommit names.
inline constexpr Foundation::UInt64 FsFormatFat{1U};
inline constexpr Foundation::UInt64 FsFormatExfat{2U};
/// The credit of a client (one request outstanding) and the per-sender
/// capacity of its reply Endpoint: a final reply and one refusal.
inline constexpr Foundation::Size FsMaximumOutstanding{1U};
inline constexpr Foundation::Size FsReplyCapacity{2U};
inline constexpr Foundation::Size FsMaximumNameBytes{Filesystem::NameBytes};
/// A file's blocks (direct block pointers of the object table's entry).
inline constexpr Foundation::Size FsMaximumFileBlocks{6U};
inline constexpr Foundation::Size FsReadPagesCapacity{32U}; // Bounded by current RAM Frames
inline constexpr Foundation::Size FsPayloadWords{8U};
inline constexpr Foundation::Size FsPayloadBytes{FsPayloadWords << 3U};
inline constexpr Foundation::Size FsNamePayloadBytes{FsPayloadBytes + FsMaximumNameBytes};
/// Read and Write: header, reply and data attachments, eight words.
inline constexpr Foundation::Size FsIoRequestBytes{EchoHeaderBytes + (2U * EchoAttachmentBytes) + FsPayloadBytes};
/// Commit and QueryCommit: header, reply attachment, eight words.
inline constexpr Foundation::Size FsCommitRequestBytes{EchoHeaderBytes + EchoAttachmentBytes + FsPayloadBytes};
/// Open and Create: header, reply attachment, eight words and the name area.
inline constexpr Foundation::Size FsNameRequestBytes{EchoHeaderBytes + EchoAttachmentBytes + FsNamePayloadBytes};
inline constexpr Foundation::Size FsReplyBytes{EchoHeaderBytes + FsPayloadBytes};
/// The largest frame of the protocol.
inline constexpr Foundation::Size FsMaximumFrameBytes{FsNameRequestBytes};
/// The data Frame attachment's rights: Map, Write and Duplicate.
// The service CPU-maps client data writable.
inline constexpr Foundation::UInt16 FsDataRights{0x0118U};
/// The sealed selector of the service's Endpoint and the tags of that Endpoint and of the
/// block-reply Endpoint in the service's WaitSet.
inline constexpr Foundation::UInt64 FsSealedSelector{0x4653'5345'5256'4943ULL};
inline constexpr Foundation::UInt64 FsServiceWaitTag{0x4653'5356U};
inline constexpr Foundation::UInt64 FsBlockReplyWaitTag{0x4653'424CU};

/// One decoded file frame.
struct FsMessage
{
    Foundation::UInt64 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[FsPayloadWords];
    /// Open's and Create's name, zero-padded; NameLength bytes are the name.
    Foundation::UInt8 Name[FsMaximumNameBytes];
    Foundation::Size NameLength;
    /// A request's reply capability, as the receiver's space holds it.
    System::CapabilityHandle Reply;
    /// A Read's or Write's data Frame, as the receiver's space holds it.
    System::CapabilityHandle Data;
};

namespace Detail
{
inline void StoreFsWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] inline Foundation::UInt64 LoadFsWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}

/// The attachments a method carries: two for a Read or Write, one for the
/// other requests, none for a reply; 3 for an unknown method.
[[nodiscard]] inline Foundation::Size CountFsAttachments(const Foundation::UInt64 method) noexcept
{
    if ((method == FsReadMethod) || (method == FsWriteMethod) || (method == FsReadDirectoryMethod))
    {
        return 2U;
    }
    if ((method == FsOpenMethod) || (method == FsCreateMethod) || (method == FsCommitMethod) ||
        (method == FsQueryCommitMethod) || (method == FsStatMethod) || (method == FsReadDirectoryMethod) ||
        (method == FsMakeDirectoryMethod) || (method == FsCloseMethod) || (method == FsInstallViewMethod) ||
        (method == FsRemoveViewMethod) || (method == FsResetViewsMethod) || (method == FsQuiesceMethod))
    {
        return 1U;
    }
    return (method == FsReplyMethod) ? 0U : 3U;
}

[[nodiscard]] inline bool HasFsName(const Foundation::UInt64 method) noexcept
{
    return (method == FsOpenMethod) || (method == FsCreateMethod) || (method == FsMakeDirectoryMethod);
}

/// Component launch, FilesystemProtocol.md's v2 word schema. Reply uses every word;
/// unknown/reserved methods are rejected by attachment count.
[[nodiscard]] inline Foundation::UInt8 UsedFsWords(const Foundation::UInt64 method) noexcept
{
    if (HasFsName(method) || method == FsQueryCommitMethod)
    {
        return 0x73U;
    }
    if (method == FsReadMethod)
    {
        return 0x9FU;
    }
    if (method == FsWriteMethod || method == FsReadDirectoryMethod)
    {
        return 0x1FU;
    }
    if (method == FsCommitMethod)
    {
        return 0x77U;
    }
    if (method == FsStatMethod || method == FsCloseMethod)
    {
        return 0x17U;
    }
    if (method == FsInstallViewMethod)
    {
        return 0x3FU;
    }
    if (method == FsRemoveViewMethod)
    {
        return 0x11U;
    }
    if (method == FsResetViewsMethod || method == FsQuiesceMethod)
    {
        return 0x01U;
    }
    return 0xFFU;
}

/// Strict UTF-8, single names up to 64 bytes; retain DirectoryName's ASCII grammar and allow
/// lexical ".." on Lookup.
[[nodiscard]] inline bool IsFsName(const Foundation::UInt8* const name, const Foundation::Size length) noexcept
{
    return Filesystem::ValidName(name, length);
}
} // Namespace Detail

/// Writes a file frame of method into frame (at most FsMaximumFrameBytes):
/// every request with its reply Endpoint attachment (Invoke, copied), a Read or
/// Write also with the data Frame attachment (FsDataRights, copied), a reply
/// with neither. Open and Create write the name of nameLength bytes (1 to
/// FsMaximumNameBytes) after the words and store nameLength as the second word,
/// whatever words[1] holds; the name's characters are not checked here. The
/// other methods ignore name and nameLength. Returns the frame's size, or zero,
/// writing nothing, for an unknown method, an attachment or name not given
/// that the method needs, or a name length out of range.
inline Foundation::Size WriteFsFrame(Foundation::UInt8* const frame, const Foundation::UInt32 method,
                                     const Foundation::UInt64 transaction,
                                     const Foundation::UInt64 (&words)[FsPayloadWords],
                                     const Foundation::UInt8* const name, const Foundation::Size nameLength,
                                     const System::CapabilityHandle* const reply,
                                     const System::CapabilityHandle* const data) noexcept
{
    const Foundation::Size attachments{Detail::CountFsAttachments(method)};
    const bool named{Detail::HasFsName(method)};
    if ((attachments > 2U) || ((attachments >= 1U) && (reply == nullptr)) ||
        ((attachments == 2U) && (data == nullptr)) ||
        (named && ((name == nullptr) || (nameLength == 0U) || (nameLength > FsMaximumNameBytes))))
    {
        return 0U;
    }
    EchoAttachment records[2]{};
    if (attachments >= 1U)
    {
        records[0U] =
            EchoAttachment{.Handle = *reply, .Rights = EchoInvokeRight, .Mode = EchoCopy, .Type = EchoEndpointType};
    }
    if (attachments == 2U)
    {
        records[1U] = EchoAttachment{.Handle = *data, .Rights = FsDataRights, .Mode = EchoCopy, .Type = EchoFrameType};
    }
    WriteEchoFrame(frame, records, attachments);
    const Foundation::Size payloadBytes = named ? FsNamePayloadBytes : FsPayloadBytes;
    const Foundation::Size total = EchoHeaderBytes + (attachments << 5U) + payloadBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(total >> (index << 3U));
    }
    frame[12U] = static_cast<Foundation::UInt8>(payloadBytes);
    frame[13U] = 0U;
    frame[14U] = static_cast<Foundation::UInt8>(payloadBytes);
    frame[15U] = 0U;
    Detail::StoreFsWord(frame + 16U, FsInterfaceLow);
    Detail::StoreFsWord(frame + 24U, FsInterfaceHigh);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[32U + index] = static_cast<Foundation::UInt8>(method >> (index << 3U));
    }
    Detail::StoreFsWord(frame + 40U, transaction);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    for (Foundation::Size word{0U}; word < FsPayloadWords; ++word)
    {
        Detail::StoreFsWord(payload + (word << 3U), (named && (word == 1U)) ? nameLength : words[word]);
    }
    if (named)
    {
        for (Foundation::Size index{0U}; index < FsMaximumNameBytes; ++index)
        {
            payload[FsPayloadBytes + index] = (index < nameLength) ? name[index] : Foundation::UInt8{0U};
        }
    }
    return total;
}

/// Decodes the frame of a delivery of deliveryBytes (its 48-byte header
/// first): false, leaving message unchanged, unless it is a file frame of its
/// method's exact size and attachment count: an Endpoint attachment on a
/// request and a Frame attachment after it on a Read or Write; for Open and
/// Create a name of 1 to FsMaximumNameBytes bytes that follows the Object
/// Directory's rules, zero padding and two zero words; for Read and Write a
/// bounded page index and ReadPages count; for Commit no flag but one of
/// FsCrashBeforeSuperblock and FsCrashAfterSuperblock and a zero last word; for
/// QueryCommit two zero words, a Stat a zero last word; for a reply a kind of
/// FsDone to FsRightsDenied.
[[nodiscard]] inline bool ReadFsFrame(const Foundation::UInt8* const delivery, const Foundation::Size deliveryBytes,
                                      FsMessage& message) noexcept
{
    if (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    const Foundation::UInt64 method = frame[32U] | (static_cast<Foundation::UInt64>(frame[33U]) << 8U);
    const Foundation::Size expected = Detail::CountFsAttachments(method);
    const Foundation::Size attachments = frame[81U];
    const bool named = Detail::HasFsName(method);
    const Foundation::Size payloadBytes = named ? FsNamePayloadBytes : FsPayloadBytes;
    if ((Detail::LoadFsWord(frame + 16U) != FsInterfaceLow) || (Detail::LoadFsWord(frame + 24U) != FsInterfaceHigh) ||
        (frame[34U] != 0U) || (frame[35U] != 0U) || (expected > 2U) || (frame[80U] != 0U) ||
        (attachments != expected) || (frame[12U] != payloadBytes) || (frame[13U] != 0U) ||
        (deliveryBytes != (EchoDeliveryHeaderBytes + EchoHeaderBytes + (attachments << 5U) + payloadBytes)) ||
        ((attachments >= 1U) && (frame[EchoHeaderBytes + 27U] != EchoEndpointType)) ||
        ((attachments == 2U) && (frame[EchoHeaderBytes + EchoAttachmentBytes + 27U] != EchoFrameType)))
    {
        return false;
    }
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    Foundation::UInt64 words[FsPayloadWords]{};
    for (Foundation::Size word{0U}; word < FsPayloadWords; ++word)
    {
        words[word] = Detail::LoadFsWord(payload + (word << 3U));
    }
    const auto used = Detail::UsedFsWords(method);
    for (Foundation::Size word{0U}; word < FsPayloadWords; ++word)
    {
        if ((used & (1U << word)) == 0U && words[word] != 0U)
        {
            return false;
        }
    }
    bool valid{false};
    if (named)
    {
        const Foundation::UInt8* const name = payload + FsPayloadBytes;
        valid = (words[1U] >= 1U) && (words[1U] <= FsMaximumNameBytes) &&
                Detail::IsFsName(name, static_cast<Foundation::Size>(words[1U]));
        for (Foundation::Size index{static_cast<Foundation::Size>(words[1U] & 0x7FU)};
             valid && (index < FsMaximumNameBytes); ++index)
        {
            valid = name[index] == 0U;
        }
    }
    else if ((method == FsReadMethod) || (method == FsWriteMethod))
    {
        valid = words[3U] <= 0x1FFF'FFFFU &&
                (method != FsReadMethod ||
                 (words[7U] >= 1U && words[7U] <= FsReadPagesCapacity && words[7U] - 1U <= 0x1FFF'FFFFU - words[3U]));
    }
    else if (method == FsCommitMethod)
    {
        valid = ((words[2U] & ~(FsCrashBeforeSuperblock | FsCrashAfterSuperblock)) == 0U) &&
                (words[2U] != (FsCrashBeforeSuperblock | FsCrashAfterSuperblock));
    }
    else if (method == FsQueryCommitMethod)
    {
        valid = true;
    }
    else if ((method == FsReadDirectoryMethod) || (method == FsInstallViewMethod) || (method == FsRemoveViewMethod) ||
             (method == FsResetViewsMethod) || (method == FsCloseMethod))
    {
        valid = true;
    }
    else if (method == FsQuiesceMethod)
    {
        valid = true;
    }
    else if (method == FsStatMethod)
    {
        valid = true;
    }
    else
    {
        valid = (words[1U] >= FsDone) && (words[1U] <= FsRightsDenied);
    }
    if (!valid)
    {
        return false;
    }
    message.Method = method;
    message.Transaction = Detail::LoadFsWord(frame + 40U);
    for (Foundation::Size word{0U}; word < FsPayloadWords; ++word)
    {
        message.Words[word] = words[word];
    }
    message.NameLength = named ? static_cast<Foundation::Size>(words[1U]) : 0U;
    for (Foundation::Size index{0U}; index < FsMaximumNameBytes; ++index)
    {
        message.Name[index] = named ? payload[FsPayloadBytes + index] : Foundation::UInt8{0U};
    }
    message.Reply = (attachments >= 1U) ? System::CapabilityHandle{Detail::LoadFsWord(frame + EchoHeaderBytes),
                                                                   Detail::LoadFsWord(frame + EchoHeaderBytes + 8U)}
                                        : System::CapabilityHandle{};
    message.Data =
        (attachments == 2U)
            ? System::CapabilityHandle{Detail::LoadFsWord(frame + EchoHeaderBytes + EchoAttachmentBytes),
                                       Detail::LoadFsWord(frame + EchoHeaderBytes + EchoAttachmentBytes + 8U)}
            : System::CapabilityHandle{};
    return true;
}
static_assert(FsMaximumFrameBytes <= EchoMaximumFrameBytes);
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_FS_FRAME_HPP_INCLUDED
