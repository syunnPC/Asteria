// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Component/BlockFrame.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Object::ObjectType;
using User::Component::BlockMessage;
using User::System::CapabilityHandle;

constexpr Size DeliveryBytes{User::Component::EchoDeliveryHeaderBytes + User::Component::BlockIoRequestBytes};
constexpr CapabilityHandle ReplyHandle{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
constexpr CapabilityHandle DataHandle{0x0456U, 0x1111'2222'3333'4444ULL};

[[nodiscard]] UInt64 PayloadWord(const MessageEnvelope& envelope, const Size word) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(envelope.Payload[(word * 8U) + index]) << (index * 8U);
    }
    return value;
}

// Builds method's frame in a delivery (a 48-byte header before it) and
// returns the delivery's size. The buffers are static: the frames are large.
Size Build(UInt8* const delivery, const UInt32 method, const UInt64 transaction, const UInt64 sectors,
           const bool withData) noexcept
{
    const UInt64 words[5U]{3U, 100U, sectors, 0U, 3U};
    const UInt64 cancelWords[4U]{3U, 100U, sectors, 0U};
    const Size bytes = User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes, method,
                                                        transaction, words, &ReplyHandle, &DataHandle);
    if (!withData)
    {
        return User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes, method,
                                                transaction, cancelWords, &ReplyHandle, nullptr) +
               User::Component::EchoDeliveryHeaderBytes;
    }
    return bytes + User::Component::EchoDeliveryHeaderBytes;
}

bool Reads(const UInt8* const delivery, const Size bytes, BlockMessage& message) noexcept
{
    return User::Component::ReadBlockFrame(delivery, bytes, message);
}

// The frames are canonical frames the kernel's codec accepts, with the
// attachments in order: the reply Endpoint, then the Map-only data Frame.
void TestFramesDecode(TestContext& runner) noexcept
{
    static UInt8 request[User::Component::BlockIoRequestBytes];
    const UInt64 words[5U]{3U, 100U, 8U, 0U, 3U};
    const Size bytes = User::Component::WriteBlockFrame(request, User::Component::BlockWriteMethod, 7U, words,
                                                        &ReplyHandle, &DataHandle);
    MessageEnvelope decoded{};
    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{request, bytes}, decoded);
    ASTERIA_EXPECT(runner, "a write request decodes with its two attachments and payload", bytes == sizeof(request),
                   error == MessageCodecError::None, decoded.Interface.GetLow() == User::Component::BlockInterfaceLow,
                   decoded.Interface.GetHigh() == User::Component::BlockInterfaceHigh,
                   decoded.MethodIdentifier == User::Component::BlockWriteMethod, decoded.TransactionIdentifier == 7U,
                   decoded.AttachmentCount == 2U, decoded.Attachments[0U].Handle.GetSelector() == 0x0123U,
                   decoded.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   decoded.Attachments[1U].Handle.GetSelector() == 0x0456U,
                   decoded.Attachments[1U].ExpectedType == ObjectType::Frame,
                   decoded.Attachments[1U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[1U].RequestedRights.GetRawValue() == User::Component::BlockMapRight,
                   decoded.PayloadSizeBytes == User::Component::BlockIoPayloadBytes, PayloadWord(decoded, 0U) == 3U,
                   PayloadWord(decoded, 1U) == 100U, PayloadWord(decoded, 2U) == 8U);

    static UInt8 cancel[User::Component::BlockCancelBytes];
    const UInt64 cancelWords[4U]{3U, 7U, 0U, 0U};
    const Size cancelBytes = User::Component::WriteBlockFrame(cancel, User::Component::BlockCancelMethod, 8U,
                                                              cancelWords, &ReplyHandle, nullptr);
    MessageEnvelope cancelled{};
    ASTERIA_EXPECT(runner, "a cancel decodes with one attachment and the transaction to cancel",
                   cancelBytes == sizeof(cancel),
                   MessageCodec::Decode(ReadOnlyByteView{cancel, cancelBytes}, cancelled) == MessageCodecError::None,
                   cancelled.AttachmentCount == 1U, cancelled.MethodIdentifier == User::Component::BlockCancelMethod,
                   PayloadWord(cancelled, 1U) == 7U);

    static UInt8 reply[User::Component::BlockReplyBytes];
    const UInt64 replyWords[4U]{7U, User::Component::BlockTooLate, 0U, 0U};
    const Size replyBytes =
        User::Component::WriteBlockFrame(reply, User::Component::BlockReplyMethod, 7U, replyWords, nullptr, nullptr);
    MessageEnvelope answered{};
    ASTERIA_EXPECT(runner, "a reply decodes with no attachment", replyBytes == sizeof(reply),
                   MessageCodec::Decode(ReadOnlyByteView{reply, replyBytes}, answered) == MessageCodecError::None,
                   answered.AttachmentCount == 0U, answered.MethodIdentifier == User::Component::BlockReplyMethod,
                   PayloadWord(answered, 1U) == User::Component::BlockTooLate);
}

void TestConstants(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "credit, reply capacity and sizes follow the decision",
                   User::Component::BlockMaximumOutstanding == 2U, User::Component::BlockReplyCapacity == 5U,
                   User::Component::BlockMaximumSectors * User::Component::BlockSectorBytes == (16U * 4096U),
                   User::Component::BlockIoRequestBytes == 192U, User::Component::BlockCancelBytes == 152U,
                   User::Component::BlockReplyBytes == 120U);
    // The driver's refusals never read as a device's task file (PxTFD's
    // status and error, at most 0xFFFF), nor as each other, and a refused
    // IommuMap keeps the kernel's error in the low byte.
    constexpr UInt64 TaskFile{0xFFFFU};
    constexpr UInt64 refused =
        User::Component::BlockMapRefused | static_cast<UInt64>(Asteria::Abi::SystemCallError::InvalidArguments);
    ASTERIA_EXPECT(runner, "the driver's failure details lie above every task file and stay distinct",
                   User::Component::BlockNoDevice > TaskFile, User::Component::BlockOutOfRange > TaskFile,
                   User::Component::BlockMapRefused > TaskFile,
                   User::Component::BlockNoDevice != User::Component::BlockOutOfRange,
                   (User::Component::BlockMapRefused & 0xFFU) == 0U, (refused & 0xFFU) == 2U,
                   (refused & ~UInt64{0xFFU}) == User::Component::BlockMapRefused,
                   (User::Component::BlockNoDevice & ~UInt64{0xFFU}) != User::Component::BlockMapRefused,
                   (User::Component::BlockOutOfRange & ~UInt64{0xFFU}) != User::Component::BlockMapRefused);
    ASTERIA_EXPECT(runner, "the driver's Interrupt and Endpoint have distinct nonzero tags in its WaitSet",
                   User::Component::BlockInterruptTag != 0U, User::Component::BlockServiceWaitTag != 0U,
                   User::Component::BlockInterruptTag != User::Component::BlockServiceWaitTag);
}

void TestRequestsRead(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    BlockMessage read{};
    const Size readBytes = Build(delivery, User::Component::BlockReadMethod, 9U, 1U, true);
    const bool readOk = Reads(delivery, readBytes, read);
    ASTERIA_EXPECT(runner, "a read request reads back with both handles", readOk,
                   read.Method == User::Component::BlockReadMethod, read.Transaction == 9U, read.Words[0U] == 3U,
                   read.Words[1U] == 100U, read.Words[2U] == 1U, read.Words[3U] == 0U, read.Reply.Selector == 0x0123U,
                   read.Reply.Authenticator == 0xAAAA'BBBB'CCCC'DDDDULL, read.Data.Selector == 0x0456U,
                   read.Data.Authenticator == 0x1111'2222'3333'4444ULL);

    BlockMessage write{};
    const Size writeBytes =
        Build(delivery, User::Component::BlockWriteMethod, 10U, User::Component::BlockMaximumSectors, true);
    ASTERIA_EXPECT(runner, "a write of the maximum sectors reads back", Reads(delivery, writeBytes, write),
                   write.Method == User::Component::BlockWriteMethod, write.Words[2U] == 128U,
                   write.Data.Selector == 0x0456U);

    BlockMessage cancel{};
    const UInt64 words[4U]{3U, 9U, 0U, 0U};
    const Size cancelBytes =
        User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                         User::Component::BlockCancelMethod, 11U, words, &ReplyHandle, nullptr) +
        User::Component::EchoDeliveryHeaderBytes;
    ASTERIA_EXPECT(runner, "a cancel reads back with its reply capability and no data",
                   Reads(delivery, cancelBytes, cancel), cancel.Method == User::Component::BlockCancelMethod,
                   cancel.Transaction == 11U, cancel.Words[1U] == 9U, cancel.Reply.Selector == 0x0123U,
                   cancel.Data.Selector == 0U);
}

void TestRepliesRead(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    const UInt64 kinds[7U]{
        User::Component::BlockDone,         User::Component::BlockFailed,          User::Component::BlockCancelled,
        User::Component::BlockTooLate,      User::Component::BlockDriverRestarted, User::Component::BlockRefused,
        User::Component::BlockMediumChanged};
    bool all{true};
    for (Size kind{0U}; kind < 7U; ++kind)
    {
        const UInt64 words[4U]{20U + kind, kinds[kind], 4U + kind, 0U};
        const Size bytes =
            User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                             User::Component::BlockReplyMethod, 20U + kind, words, nullptr, nullptr) +
            User::Component::EchoDeliveryHeaderBytes;
        BlockMessage reply{};
        all = all && Reads(delivery, bytes, reply) && (reply.Method == User::Component::BlockReplyMethod) &&
              (reply.Transaction == (20U + kind)) && (reply.Words[0U] == (20U + kind)) &&
              (reply.Words[1U] == kinds[kind]) && (reply.Words[2U] == (4U + kind)) && (reply.Reply.Selector == 0U) &&
              (reply.Data.Selector == 0U);
    }
    ASTERIA_EXPECT(runner, "every reply kind reads back with no capability", all);

    // A cancel's answer names the cancel's own transaction and, as its
    // detail, the request it answered.
    const UInt64 words[4U]{12U, User::Component::BlockCancelled, 9U, 0U};
    const Size bytes =
        User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                         User::Component::BlockReplyMethod, 12U, words, nullptr, nullptr) +
        User::Component::EchoDeliveryHeaderBytes;
    BlockMessage answer{};
    ASTERIA_EXPECT(runner, "a cancel's answer carries the cancelled request as its detail",
                   Reads(delivery, bytes, answer), answer.Transaction == 12U, answer.Words[2U] == 9U);

    const UInt64 failedWords[4U]{13U, User::Component::BlockFailed, User::Component::BlockMapRefused | 6U, 0U};
    const Size failedBytes =
        User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                         User::Component::BlockReplyMethod, 13U, failedWords, nullptr, nullptr) +
        User::Component::EchoDeliveryHeaderBytes;
    BlockMessage failed{};
    ASTERIA_EXPECT(runner, "a failure the driver refused carries its whole detail word",
                   Reads(delivery, failedBytes, failed), failed.Words[1U] == User::Component::BlockFailed,
                   failed.Words[2U] == 0x1'0000'0106ULL);
}

void TestDescribe(TestContext& runner) noexcept
{
    namespace Component = User::Component;
    static UInt8 delivery[DeliveryBytes]{};
    const UInt64 request[4U]{3U, 0U, 0U, 0U};
    const Size bytes = Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes,
                                                  Component::BlockDescribeMethod, 25U, request, &ReplyHandle, nullptr) +
                       Component::EchoDeliveryHeaderBytes;
    BlockMessage message{};
    ASTERIA_EXPECT(runner, "Describe carries only the reply endpoint and instance", Reads(delivery, bytes, message),
                   message.Method == Component::BlockDescribeMethod, message.Reply.Selector == ReplyHandle.Selector,
                   message.Data.Selector == 0U);
    const Size payload = Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes;
    const UInt64 cases[][4U]{
        {512U, 4096U, Component::BlockMediumPresent, 9U},
        {4096U, 4096U, Component::BlockMediumPresent | Component::BlockReadOnly | Component::BlockRemovable, 9U},
        {512U, 0U, Component::BlockRemovable, 9U},
        {0U, 4096U, Component::BlockMediumPresent, 9U},
        {513U, 4096U, Component::BlockMediumPresent, 9U},
        {131072U, 4096U, Component::BlockMediumPresent, 9U},
        {512U, 0U, Component::BlockMediumPresent, 9U},
        {512U, 1U, 0U, 9U},
        {512U, ~UInt64{0U}, Component::BlockMediumPresent, 9U},
        {512U, 4096U, 8U | Component::BlockMediumPresent, 9U},
        {512U, 4096U, Component::BlockMediumPresent, 0U},
    };
    for (Size index{0U}; index < 11U; ++index)
    {
        const Size size =
            Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes,
                                       Component::BlockDescribeReplyMethod, 25U, cases[index], nullptr, nullptr) +
            Component::EchoDeliveryHeaderBytes;
        ASTERIA_EXPECT(runner, "Describe validates size, count, flags and generation",
                       Reads(delivery, size, message) == (index < 3U));
    }
    Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes, Component::BlockDescribeMethod, 25U,
                               request, &ReplyHandle, nullptr);
    for (Size word{1U}; word < 4U; ++word)
    {
        delivery[payload + Component::EchoAttachmentBytes + (word * 8U)] = 1U;
        ASTERIA_EXPECT(runner, "Describe reserved request words are refused", !Reads(delivery, bytes, message));
        delivery[payload + Component::EchoAttachmentBytes + (word * 8U)] = 0U;
    }
    const UInt64 io[5U]{3U, 0U, 1U, 0U, 0U};
    const Size ioBytes = Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes,
                                                    Component::BlockReadMethod, 26U, io, &ReplyHandle, &DataHandle) +
                         Component::EchoDeliveryHeaderBytes;
    ASTERIA_EXPECT(runner, "zero medium generation is refused", !Reads(delivery, ioBytes, message));
    const UInt64 goodIo[5U]{3U, 0U, 1U, 0U, 9U};
    Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes, Component::BlockReadMethod, 26U, goodIo,
                               &ReplyHandle, &DataHandle);
    const Size corrupt[]{0U,
                         4U,
                         6U,
                         8U,
                         12U,
                         14U,
                         34U,
                         36U,
                         56U,
                         80U,
                         82U,
                         Component::EchoHeaderBytes + 24U,
                         Component::EchoHeaderBytes + 26U,
                         Component::EchoHeaderBytes + 28U};
    for (const Size offset : corrupt)
    {
        delivery[Component::EchoDeliveryHeaderBytes + offset] ^= 1U;
        const BlockMessage before = message;
        ASTERIA_EXPECT(runner, "malformed headers and attachment fields are refused atomically",
                       !Reads(delivery, ioBytes, message), message.Transaction == before.Transaction,
                       message.Method == before.Method, message.Words[3U] == before.Words[3U]);
        delivery[Component::EchoDeliveryHeaderBytes + offset] ^= 1U;
    }
    const UInt64 reserved[4U]{26U, Component::BlockMediumChanged, 0U, 1U};
    const Size reservedBytes =
        Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes, Component::BlockReplyMethod, 26U,
                                   reserved, nullptr, nullptr) +
        Component::EchoDeliveryHeaderBytes;
    ASTERIA_EXPECT(runner, "the new kind preserves Reply's zero reserved word",
                   !Reads(delivery, reservedBytes, message));
    const UInt64 legacy[4U]{3U, 0U, 1U, 0U};
    ASTERIA_EXPECT(
        runner, "legacy I/O cannot omit the medium generation",
        Component::WriteBlockFrame(delivery, Component::BlockReadMethod, 26U, legacy, &ReplyHandle, &DataHandle) == 0U);
}

void TestRefusals(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    BlockMessage message{};
    const Size readBytes = Build(delivery, User::Component::BlockReadMethod, 9U, 4U, true);
    ASTERIA_EXPECT(runner, "a request one byte short or long is refused", Reads(delivery, readBytes, message),
                   !Reads(delivery, readBytes - 1U, message), !Reads(delivery, readBytes + 1U, message),
                   !Reads(delivery, User::Component::EchoDeliveryHeaderBytes + 8U, message));

    delivery[User::Component::EchoDeliveryHeaderBytes + 16U] ^= 1U;
    ASTERIA_EXPECT(runner, "a foreign interface is refused", !Reads(delivery, readBytes, message));
    delivery[User::Component::EchoDeliveryHeaderBytes + 16U] ^= 1U;

    // A read with one attachment: the data Frame missing.
    const Size oneBytes = Build(delivery, User::Component::BlockCancelMethod, 9U, 0U, false);
    delivery[User::Component::EchoDeliveryHeaderBytes + 32U] = static_cast<UInt8>(User::Component::BlockReadMethod);
    ASTERIA_EXPECT(runner, "a read with one attachment is refused", !Reads(delivery, oneBytes, message));

    // A cancel with two attachments.
    const Size twoBytes = Build(delivery, User::Component::BlockReadMethod, 9U, 0U, true);
    delivery[User::Component::EchoDeliveryHeaderBytes + 32U] = static_cast<UInt8>(User::Component::BlockCancelMethod);
    ASTERIA_EXPECT(runner, "a cancel with two attachments is refused", !Reads(delivery, twoBytes, message));

    // A data attachment that is no Frame, a reply attachment that is no Endpoint.
    const Size typedBytes = Build(delivery, User::Component::BlockReadMethod, 9U, 4U, true);
    delivery[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoHeaderBytes +
             User::Component::EchoAttachmentBytes + 27U] = User::Component::EchoEndpointType;
    const bool wrongData = Reads(delivery, typedBytes, message);
    delivery[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoHeaderBytes +
             User::Component::EchoAttachmentBytes + 27U] = User::Component::EchoFrameType;
    delivery[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoHeaderBytes + 27U] =
        User::Component::EchoFrameType;
    const bool wrongReply = Reads(delivery, typedBytes, message);
    ASTERIA_EXPECT(runner, "attachments of the wrong object type are refused", !wrongData, !wrongReply);

    ASTERIA_EXPECT(runner, "sectors 0 and 129 are refused",
                   !Reads(delivery, Build(delivery, User::Component::BlockWriteMethod, 9U, 0U, true), message),
                   !Reads(delivery, Build(delivery, User::Component::BlockWriteMethod, 9U, 129U, true), message),
                   Reads(delivery, Build(delivery, User::Component::BlockWriteMethod, 9U, 128U, true), message));

    const Size unknownBytes = Build(delivery, User::Component::BlockReadMethod, 9U, 4U, true);
    delivery[User::Component::EchoDeliveryHeaderBytes + 32U] = 5U;
    const bool unknown = Reads(delivery, unknownBytes, message);
    delivery[User::Component::EchoDeliveryHeaderBytes + 32U] = 0U;
    ASTERIA_EXPECT(runner, "an unknown method is refused", !unknown, !Reads(delivery, unknownBytes, message));

    // A reply of an unknown kind, and a request with an unknown flag.
    const UInt64 badKind[4U]{1U, 8U, 0U, 0U};
    const Size kindBytes =
        User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                         User::Component::BlockReplyMethod, 1U, badKind, nullptr, nullptr) +
        User::Component::EchoDeliveryHeaderBytes;
    const bool badKindRead = Reads(delivery, kindBytes, message);
    const UInt64 badFlags[5U]{1U, 0U, 4U, 2U, 1U};
    const Size flagBytes =
        User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                         User::Component::BlockReadMethod, 1U, badFlags, &ReplyHandle, &DataHandle) +
        User::Component::EchoDeliveryHeaderBytes;
    ASTERIA_EXPECT(runner, "an unknown reply kind and an unknown flag are refused", !badKindRead,
                   !Reads(delivery, flagBytes, message),
                   User::Component::WriteBlockFrame(delivery, User::Component::BlockReadMethod, 1U, badFlags, nullptr,
                                                    nullptr) == 0U);
    const UInt64 crashing[5U]{1U, 0U, 4U, User::Component::BlockCrashAfterIssue, 1U};
    const Size crashBytes =
        User::Component::WriteBlockFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                         User::Component::BlockReadMethod, 1U, crashing, &ReplyHandle, &DataHandle) +
        User::Component::EchoDeliveryHeaderBytes;
    ASTERIA_EXPECT(runner, "a request may inject the crash after its command is issued",
                   Reads(delivery, crashBytes, message), message.Words[3U] == User::Component::BlockCrashAfterIssue);

    UInt8 scratch[User::Component::BlockReplyBytes]{};
    const UInt64 words[4U]{};
    ASTERIA_EXPECT(runner, "an unknown method writes nothing",
                   User::Component::WriteBlockFrame(scratch, 9U, 1U, words, &ReplyHandle, &DataHandle) == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"BlockFrameTests"};
    Asteria::Test::TestFramesDecode(runner);
    Asteria::Test::TestConstants(runner);
    Asteria::Test::TestRequestsRead(runner);
    Asteria::Test::TestRepliesRead(runner);
    Asteria::Test::TestRefusals(runner);
    Asteria::Test::TestDescribe(runner);
    return runner.Finish();
}
