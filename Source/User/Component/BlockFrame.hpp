// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_BLOCK_FRAME_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_BLOCK_FRAME_HPP_INCLUDED

#include "Abi/IpcCanonicalFrame.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The block protocol between a client and the AHCI driver: asynchronous canonical frames of
/// BlockInterface built by hand, as the driver protocol's are (no codec in the component image).
/// Every frame is its 88-byte header, its attachments of 32 bytes, then a payload of four words
/// (five for Read/Write). A Read or Write request (BlockReadMethod, BlockWriteMethod) carries two
/// attachments, in this order: a copy of a send capability on the client's reply Endpoint and a
/// copy of the client's data Frame with the Map right only (one contiguous run of at most
/// BlockMaximumSectors sectors, one PRDT entry). Its payload is the generation the client addressed
/// (the driver instance's tag), the first sector (LBA), the sector count and a flags word: zero, or
/// BlockCrashAfterIssue, the fault the recovery's demonstration injects, followed by the nonzero
/// medium generation from Describe. A Cancel (BlockCancelMethod) carries one attachment, the reply
/// capability, and the payload: the generation, the transaction of the request to cancel and two
/// zero words. A reply (BlockReplyMethod) carries no attachment and the payload: the transaction it
/// answers, its kind, its detail and a zero word. Every frame carries its own transaction
/// identifier in the header; a reply's payload repeats the transaction of the request (or Cancel)
/// it answers, which is also its header's, so a client matches either. Describe carries one reply
/// Endpoint and {instance, 0, 0, 0}; DescribeReply has no attachments and carries {block bytes,
/// block count, removable/read-only/present flags, medium generation}. Its header transaction
/// matches the request; its payload has no reply kind. Exactly one final reply answers each Read or
/// Write, kind BlockDone (detail: the sectors transferred), BlockFailed (detail: the device's task
/// file or the driver's refusal, BlockNoDevice and the codes after it), BlockCancelled (the request
/// was cancelled before it was issued, detail zero) or BlockDriverRestarted (the instance addressed
/// died; the request was not executed or its outcome is unknown, detail zero), or
/// BlockMediumChanged (the medium generation is stale, no I/O, detail zero). A request beyond the
/// client's credit (BlockMaximumOutstanding) is answered BlockRefused with the credit as its
/// detail, and is not executed. A Cancel is answered on its own, by a reply whose transaction is
/// the Cancel's and whose detail is the transaction of the request it named: BlockCancelled when
/// that request was cancelled at once, BlockTooLate when it was issued, already answered or never
/// known. A Cancel takes no credit, but a client keeps at most one Cancel unanswered per request it
/// may have outstanding, so that the reply Endpoint holds every answer; the driver answers each
/// Cancel it receives. Both attachments are copies, so the client's handles need the Duplicate
/// right as well.
inline constexpr Foundation::UInt64 BlockInterfaceLow{0x424C'4F43'4B49'4F01ULL};
inline constexpr Foundation::UInt64 BlockInterfaceHigh{0x4153'5445'5249'4102ULL};
inline constexpr Foundation::UInt32 BlockReadMethod{1U};
inline constexpr Foundation::UInt32 BlockWriteMethod{2U};
inline constexpr Foundation::UInt32 BlockCancelMethod{3U};
inline constexpr Foundation::UInt32 BlockReplyMethod{4U};
inline constexpr Foundation::UInt32 BlockDescribeMethod{5U};
inline constexpr Foundation::UInt32 BlockDescribeReplyMethod{6U};
/// PowerOff.md: {generation, 0, 0, 0}, reply attachment only; Done detail 0
/// follows a completed device cache synchronization command.
inline constexpr Foundation::UInt32 BlockFlushMethod{7U};
inline constexpr Foundation::UInt64 BlockRemovable{1U};
inline constexpr Foundation::UInt64 BlockReadOnly{2U};
inline constexpr Foundation::UInt64 BlockMediumPresent{4U};
inline constexpr Foundation::UInt64 BlockDescribeFlags{BlockRemovable | BlockReadOnly | BlockMediumPresent};
/// A request's flag: the driver crashes once the request's command is
/// issued, so it dies with the device transferring into the client's Frame
///, as the driver protocol's DriverCrashAfterStart.
inline constexpr Foundation::UInt64 BlockCrashAfterIssue{1U};
/// A reply's kind.
inline constexpr Foundation::UInt64 BlockDone{1U};
inline constexpr Foundation::UInt64 BlockFailed{2U};
inline constexpr Foundation::UInt64 BlockCancelled{3U};
inline constexpr Foundation::UInt64 BlockTooLate{4U};
inline constexpr Foundation::UInt64 BlockDriverRestarted{5U};
inline constexpr Foundation::UInt64 BlockRefused{6U};
inline constexpr Foundation::UInt64 BlockMediumChanged{7U};
/// The credit of a client: the requests it may have outstanding,
/// and the per-sender capacity of its reply Endpoint: a final
/// reply and a Cancel's answer per request, and one refusal.
inline constexpr Foundation::Size BlockMaximumOutstanding{2U};
inline constexpr Foundation::Size BlockReplyCapacity{(BlockMaximumOutstanding << 1U) + 1U};
inline constexpr Foundation::Size BlockSectorBytes{512U};
/// A data Frame is at most 16 pages of 4 KiB.
inline constexpr Foundation::Size BlockMaximumSectors{128U};
inline constexpr Foundation::Size BlockPayloadWords{4U};
inline constexpr Foundation::Size BlockPayloadBytes{BlockPayloadWords << 3U};
inline constexpr Foundation::Size BlockIoPayloadWords{5U};
inline constexpr Foundation::Size BlockIoPayloadBytes{BlockIoPayloadWords << 3U};
inline constexpr Foundation::Size BlockIoRequestBytes{EchoHeaderBytes + (2U * EchoAttachmentBytes) +
                                                      BlockIoPayloadBytes};
inline constexpr Foundation::Size BlockCancelBytes{EchoHeaderBytes + EchoAttachmentBytes + BlockPayloadBytes};
inline constexpr Foundation::Size BlockReplyBytes{EchoHeaderBytes + BlockPayloadBytes};
/// Map|Write permits CPU copies between client Frames and 32-bit AHCI bounce storage.
inline constexpr Foundation::UInt16 BlockMapRight{0x0018U};
/// The sealed selector of the driver's block Endpoint and the tag of that Endpoint in the driver's
/// WaitSet.
inline constexpr Foundation::UInt64 BlockSealedSelector{0x424C'4F43'4B53'454CULL};
inline constexpr Foundation::UInt64 BlockServiceWaitTag{0x424C'4B53U};
/// The tag of the driver's Interrupt (its controller's MSI) in that same
/// WaitSet.
inline constexpr Foundation::UInt64 BlockInterruptTag{0x424C'4B49U};
/// A BlockFailed reply's detail. A command the device failed carries the
/// Port's task file (PxTFD: the ATA status in bits 0-7, the error in bits 8-15), so at most 0xFFFF;
/// the driver's own refusals lie above it: no port with a disk (or no IOMMU to confine one), a
/// first sector and count beyond the capacity IDENTIFY DEVICE reported, and an IommuMap of the data
/// Frame the kernel refused, with the SystemCallError in the low byte.
inline constexpr Foundation::UInt64 BlockNoDevice{0x1'0000'0001ULL};
inline constexpr Foundation::UInt64 BlockOutOfRange{0x1'0000'0002ULL};
inline constexpr Foundation::UInt64 BlockUnsupportedSectorSize{0x1'0000'0003ULL};
inline constexpr Foundation::UInt64 BlockMapRefused{0x1'0000'0100ULL};

/// One decoded block frame.
struct BlockMessage
{
    Foundation::UInt64 Method;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Words[BlockIoPayloadWords];
    /// A request's or Cancel's reply capability, as the receiver's space
    /// holds it.
    System::CapabilityHandle Reply;
    /// A Read's or Write's data Frame, as the receiver's space holds it.
    System::CapabilityHandle Data;
};

namespace Detail
{
inline void StoreBlockWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] inline Foundation::UInt64 LoadBlockWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}

/// The attachments a method carries: two for a Read or Write, one for a
/// Cancel/Describe, none for either reply; 3 for an unknown method.
[[nodiscard]] inline Foundation::Size CountBlockAttachments(const Foundation::UInt64 method) noexcept
{
    if ((method == BlockReadMethod) || (method == BlockWriteMethod))
    {
        return 2U;
    }
    if ((method == BlockCancelMethod) || (method == BlockDescribeMethod) || (method == BlockFlushMethod))
    {
        return 1U;
    }
    return ((method == BlockReplyMethod) || (method == BlockDescribeReplyMethod)) ? 0U : 3U;
}
} // Namespace Detail

/// Writes a block frame of method into frame (at most BlockIoRequestBytes):
/// a Read or Write with its reply Endpoint attachment and data Frame
/// attachment (the Frame's rights are BlockMapRight, copied), a Cancel with
/// its reply attachment alone (data is ignored), a reply with neither.
/// Returns its size, or zero, writing nothing, for an unknown method or a
/// method whose attachments are not given.
template <Foundation::Size Words>
inline Foundation::Size WriteBlockFrame(Foundation::UInt8* const frame, const Foundation::UInt32 method,
                                        const Foundation::UInt64 transaction, const Foundation::UInt64 (&words)[Words],
                                        const System::CapabilityHandle* const reply,
                                        const System::CapabilityHandle* const data) noexcept
{
    const Foundation::Size attachments{Detail::CountBlockAttachments(method)};
    const Foundation::Size payloadWords = (attachments == 2U) ? BlockIoPayloadWords : BlockPayloadWords;
    const Foundation::Size payloadBytes = payloadWords << 3U;
    if ((Words != payloadWords) || (attachments > 2U) || ((attachments >= 1U) && (reply == nullptr)) ||
        ((attachments == 2U) && (data == nullptr)))
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
        records[1U] = EchoAttachment{.Handle = *data, .Rights = BlockMapRight, .Mode = EchoCopy, .Type = EchoFrameType};
    }
    WriteEchoFrame(frame, records, attachments);
    const Foundation::Size total = EchoHeaderBytes + (attachments << 5U) + payloadBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[8U + index] = static_cast<Foundation::UInt8>(total >> (index << 3U));
    }
    frame[12U] = static_cast<Foundation::UInt8>(payloadBytes);
    frame[13U] = 0U;
    frame[14U] = static_cast<Foundation::UInt8>(payloadBytes);
    frame[15U] = 0U;
    Detail::StoreBlockWord(frame + 16U, BlockInterfaceLow);
    Detail::StoreBlockWord(frame + 24U, BlockInterfaceHigh);
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        frame[32U + index] = static_cast<Foundation::UInt8>(method >> (index << 3U));
    }
    Detail::StoreBlockWord(frame + 40U, transaction);
    Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    for (Foundation::Size word{0U}; word < payloadWords; ++word)
    {
        Detail::StoreBlockWord(payload + (word << 3U), words[word]);
    }
    return total;
}

/// Decodes the frame of a delivery of deliveryBytes (its 48-byte header first): false unless it is
/// a block frame of its method's exact size and attachment count, a Read or Write with an Endpoint
/// then a Frame attachment, a sector count of 1 to BlockMaximumSectors and no flag but
/// BlockCrashAfterIssue, a Cancel with an Endpoint attachment and two zero words, or a reply with
/// none, a known kind and a zero last word. Describe's reserved words must be zero. DescribeReply
/// validates the block size, capacity, defined flags and nonzero medium generation. Failure leaves message unchanged; neither decoding nor
/// encoding allocates or blocks.
[[nodiscard]] inline bool ReadBlockFrame(const Foundation::UInt8* const delivery, const Foundation::Size deliveryBytes,
                                         BlockMessage& message) noexcept
{
    if (deliveryBytes < (EchoDeliveryHeaderBytes + EchoHeaderBytes))
    {
        return false;
    }
    const Foundation::UInt8* const frame = delivery + EchoDeliveryHeaderBytes;
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (frame[index] != Abi::CanonicalAsynchronousFrame[index])
        {
            return false;
        }
    }
    const Foundation::UInt64 method = frame[32U] | (static_cast<Foundation::UInt64>(frame[33U]) << 8U);
    const Foundation::Size expected = Detail::CountBlockAttachments(method);
    const Foundation::Size attachments = frame[81U];
    const Foundation::Size payloadWords = (expected == 2U) ? BlockIoPayloadWords : BlockPayloadWords;
    const Foundation::Size payloadBytes = payloadWords << 3U;
    if ((Detail::LoadBlockWord(frame + 16U) != BlockInterfaceLow) ||
        (Detail::LoadBlockWord(frame + 24U) != BlockInterfaceHigh) || (frame[34U] != 0U) || (frame[35U] != 0U) ||
        (expected > 2U) || (frame[80U] != 0U) || (attachments != expected) || (frame[12U] != payloadBytes) ||
        (frame[13U] != 0U) || (frame[14U] != payloadBytes) || (frame[15U] != 0U) || (frame[36U] != 1U) ||
        (frame[37U] != 0U) || (frame[38U] != 0U) || (frame[39U] != 0U) ||
        (deliveryBytes != (EchoDeliveryHeaderBytes + EchoHeaderBytes + (attachments << 5U) + payloadBytes)) ||
        ((attachments >= 1U) && (frame[EchoHeaderBytes + 27U] != EchoEndpointType)) ||
        ((attachments == 2U) && (frame[EchoHeaderBytes + EchoAttachmentBytes + 27U] != EchoFrameType)))
    {
        return false;
    }
    const Foundation::Size total = EchoHeaderBytes + (attachments << 5U) + payloadBytes;
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        if (frame[8U + index] != static_cast<Foundation::UInt8>(total >> (index << 3U)))
        {
            return false;
        }
    }
    for (Foundation::Size index{56U}; index < EchoHeaderBytes; ++index)
    {
        if ((index != 81U) && (frame[index] != Abi::CanonicalAsynchronousFrame[index]))
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < attachments; ++index)
    {
        const Foundation::UInt8* const record = frame + EchoHeaderBytes + (index << 5U);
        const Foundation::UInt16 rights = (index == 0U) ? EchoInvokeRight : BlockMapRight;
        if ((record[24U] != rights) || (record[25U] != 0U) || (record[26U] != EchoCopy) || (record[28U] != 0U) ||
            (record[29U] != 0U) || (record[30U] != 0U) || (record[31U] != 0U))
        {
            return false;
        }
    }
    const Foundation::UInt8* const payload = frame + EchoHeaderBytes + (attachments << 5U);
    Foundation::UInt64 words[BlockIoPayloadWords]{};
    for (Foundation::Size word{0U}; word < payloadWords; ++word)
    {
        words[word] = Detail::LoadBlockWord(payload + (word << 3U));
    }
    bool valid{false};
    if (attachments == 2U)
    {
        valid = (words[2U] >= 1U) && (words[2U] <= BlockMaximumSectors) &&
                ((words[3U] & ~BlockCrashAfterIssue) == 0U) && (words[0U] != 0U) && (words[4U] != 0U);
    }
    else if (attachments == 1U)
    {
        valid = (words[0U] != 0U) && (words[2U] == 0U) && (words[3U] == 0U) &&
                ((method == BlockCancelMethod) || (words[1U] == 0U));
    }
    else if (method == BlockDescribeReplyMethod)
    {
        const bool present = (words[2U] & BlockMediumPresent) != 0U;
        valid =
            (words[0U] >= 512U) && (words[0U] <= 65536U) && ((words[0U] & (words[0U] - 1U)) == 0U) &&
            ((words[2U] & ~BlockDescribeFlags) == 0U) && (words[3U] != 0U) &&
            (present ? ((words[1U] != 0U) && (words[1U] <= (~Foundation::UInt64{0U} / words[0U]))) : (words[1U] == 0U));
    }
    else
    {
        valid = (words[1U] >= BlockDone) && (words[1U] <= BlockMediumChanged) && (words[3U] == 0U);
    }
    if (!valid)
    {
        return false;
    }
    if (Detail::LoadBlockWord(frame + 40U) == 0U)
    {
        return false;
    }
    message = BlockMessage{};
    message.Method = method;
    message.Transaction = Detail::LoadBlockWord(frame + 40U);
    for (Foundation::Size word{0U}; word < payloadWords; ++word)
    {
        message.Words[word] = words[word];
    }
    message.Reply = (attachments >= 1U) ? System::CapabilityHandle{Detail::LoadBlockWord(frame + EchoHeaderBytes),
                                                                   Detail::LoadBlockWord(frame + EchoHeaderBytes + 8U)}
                                        : System::CapabilityHandle{};
    message.Data =
        (attachments == 2U)
            ? System::CapabilityHandle{Detail::LoadBlockWord(frame + EchoHeaderBytes + EchoAttachmentBytes),
                                       Detail::LoadBlockWord(frame + EchoHeaderBytes + EchoAttachmentBytes + 8U)}
            : System::CapabilityHandle{};
    return true;
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_BLOCK_FRAME_HPP_INCLUDED
