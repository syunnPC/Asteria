// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/DriverFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/NetFrame.hpp"

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
using User::Component::NicMessage;
using User::System::CapabilityHandle;
namespace C = User::Component;

constexpr Size Header{C::EchoDeliveryHeaderBytes};
constexpr Size DeliveryBytes{Header + C::NicDataRequestBytes};
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

// Builds method's frame with words in a delivery (a 48-byte header before
// it) and returns the delivery's size.
Size Build(UInt8* const delivery, const UInt32 method, const UInt64 transaction,
           const UInt64 (&words)[C::NicPayloadWords]) noexcept
{
    return C::WriteNicFrame(delivery + Header, method, transaction, words, &ReplyHandle, &DataHandle) + Header;
}

// The frames are canonical frames the kernel's codec accepts, with the
// attachments in order: the reply Endpoint (Invoke), then the Map-only data
// Frame, both copied.
void TestFramesDecode(TestContext& runner) noexcept
{
    static UInt8 request[C::NicDataRequestBytes];
    const UInt64 words[4U]{3U, 1514U, 0U, 0U};
    const Size bytes = C::WriteNicFrame(request, C::NicSendMethod, 7U, words, &ReplyHandle, &DataHandle);
    MessageEnvelope decoded{};
    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{request, bytes}, decoded);
    ASTERIA_EXPECT(runner, "a Send decodes with its two attachments and payload", bytes == sizeof(request),
                   error == MessageCodecError::None, decoded.Interface.GetLow() == C::NicInterfaceLow,
                   decoded.Interface.GetHigh() == C::NicInterfaceHigh, decoded.MethodIdentifier == C::NicSendMethod,
                   decoded.TransactionIdentifier == 7U, decoded.AttachmentCount == 2U,
                   decoded.Attachments[0U].Handle.GetSelector() == 0x0123U,
                   decoded.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   decoded.Attachments[0U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[0U].RequestedRights.GetRawValue() == C::EchoInvokeRight,
                   decoded.Attachments[1U].Handle.GetSelector() == 0x0456U,
                   decoded.Attachments[1U].ExpectedType == ObjectType::Frame,
                   decoded.Attachments[1U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[1U].RequestedRights.GetRawValue() == 0x0018U,
                   decoded.PayloadSizeBytes == C::NicPayloadBytes, PayloadWord(decoded, 0U) == 3U,
                   PayloadWord(decoded, 1U) == 1514U);

    static UInt8 query[C::NicQueryBytes];
    const UInt64 queryWords[4U]{3U, 0U, 0U, 0U};
    const Size queryBytes = C::WriteNicFrame(query, C::NicQueryMethod, 8U, queryWords, &ReplyHandle, nullptr);
    MessageEnvelope queried{};
    ASTERIA_EXPECT(runner, "a Query decodes with its reply attachment alone", queryBytes == sizeof(query),
                   MessageCodec::Decode(ReadOnlyByteView{query, queryBytes}, queried) == MessageCodecError::None,
                   queried.AttachmentCount == 1U, queried.MethodIdentifier == C::NicQueryMethod);

    static UInt8 reply[C::NicReplyBytes];
    const UInt64 replyWords[4U]{8U, C::NicDone, 0x5634'1200'5452ULL, 1U | (UInt64{9U} << 32U)};
    const Size replyBytes = C::WriteNicFrame(reply, C::NicReplyMethod, 8U, replyWords, nullptr, nullptr);
    MessageEnvelope answered{};
    ASTERIA_EXPECT(runner, "a reply decodes with no attachment and its extra word", replyBytes == sizeof(reply),
                   MessageCodec::Decode(ReadOnlyByteView{reply, replyBytes}, answered) == MessageCodecError::None,
                   answered.AttachmentCount == 0U, answered.MethodIdentifier == C::NicReplyMethod,
                   PayloadWord(answered, 2U) == 0x5634'1200'5452ULL,
                   PayloadWord(answered, 3U) == (1U | (UInt64{9U} << 32U)));
}

void TestConstants(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "credits, the reply capacity, sizes and frame bounds follow the decision",
                   C::NicMaximumSends == 4U, C::NicMaximumReceiveCredits == 4U, C::NicReplyCapacity == 8U,
                   C::NicDataRequestBytes == 184U, C::NicQueryBytes == 152U, C::NicReplyBytes == 120U,
                   C::NicMinimumFrameBytes == 14U, C::NicMaximumFrameBytes == 1514U, C::NicMaximumReceiveBytes <= 4096U,
                   (C::NicReplyCapacity * C::EchoMaximumFrameBytes) <= 0xFFFFU, C::NicMapRefused > C::NicNoDevice,
                   (C::NicMapRefused & 0xFFU) == 0U);
    ASTERIA_EXPECT(runner, "the interface differs from the block, file, display and driver protocols'",
                   (C::NicInterfaceLow != C::BlockInterfaceLow) || (C::NicInterfaceHigh != C::BlockInterfaceHigh),
                   (C::NicInterfaceLow != C::FsInterfaceLow) || (C::NicInterfaceHigh != C::FsInterfaceHigh),
                   (C::NicInterfaceLow != C::DisplayInterfaceLow) || (C::NicInterfaceHigh != C::DisplayInterfaceHigh),
                   (C::NicInterfaceLow != C::DriverInterfaceLow) || (C::NicInterfaceHigh != C::DriverInterfaceHigh),
                   C::NicInterfaceLow != C::NicInterfaceHigh);
    ASTERIA_EXPECT(runner, "the selector and tags are nonzero and differ from the other services'",
                   C::NicSealedSelector != 0U, C::NicServiceWaitTag != 0U, C::NicInterruptTag != 0U,
                   C::NicServiceWaitTag != C::NicInterruptTag, C::NicSealedSelector != C::BlockSealedSelector,
                   C::NicSealedSelector != C::FsSealedSelector, C::NicSealedSelector != C::DisplaySealedSelector,
                   C::NicSealedSelector != C::DriverSealedSelector, C::NicSealedSelector != C::EchoSealedSelector,
                   C::NicServiceWaitTag != C::BlockServiceWaitTag, C::NicServiceWaitTag != C::FsServiceWaitTag,
                   C::NicServiceWaitTag != C::DisplayServiceWaitTag, C::NicServiceWaitTag != C::DriverServiceWaitTag,
                   C::NicInterruptTag != C::BlockInterruptTag, C::NicInterruptTag != C::BlockServiceWaitTag);
}

void TestRequestsRead(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    NicMessage send{};
    const bool sent = C::ReadNicFrame(delivery, Build(delivery, C::NicSendMethod, 9U, {3U, 60U, 0U, 0U}), send);
    ASTERIA_EXPECT(runner, "a Send reads back with both handles", sent, send.Method == C::NicSendMethod,
                   send.Transaction == 9U, send.Words[0U] == 3U, send.Words[1U] == 60U, send.Reply.Selector == 0x0123U,
                   send.Reply.Authenticator == 0xAAAA'BBBB'CCCC'DDDDULL, send.Data.Selector == 0x0456U,
                   send.Data.Authenticator == 0x1111'2222'3333'4444ULL);

    NicMessage credit{};
    const bool credited =
        C::ReadNicFrame(delivery, Build(delivery, C::NicReceiveMethod, 10U, {3U, 0U, 0U, 0U}), credit);
    NicMessage query{};
    const Size queryBytes =
        C::WriteNicFrame(delivery + Header, C::NicQueryMethod, 11U, {3U, 0U, 0U, 0U}, &ReplyHandle, &DataHandle) +
        Header;
    const bool queried = C::ReadNicFrame(delivery, queryBytes, query);
    ASTERIA_EXPECT(runner, "a ReceiveCredit reads back with its Frame, a Query with its reply alone", credited,
                   credit.Method == C::NicReceiveMethod, credit.Transaction == 10U, credit.Data.Selector == 0x0456U,
                   queried, query.Method == C::NicQueryMethod, query.Reply.Selector == 0x0123U,
                   query.Data.Selector == 0U, queryBytes == (Header + C::NicQueryBytes));

    const UInt64 kinds[5U]{C::NicDone, C::NicFailed, C::NicRefused, C::NicCancelled, C::NicDriverRestarted};
    bool all{true};
    for (Size kind{0U}; kind < 5U; ++kind)
    {
        const UInt64 words[4U]{20U + kind, kinds[kind], 4U + kind, ~UInt64{0U} - kind};
        const Size bytes =
            C::WriteNicFrame(delivery + Header, C::NicReplyMethod, 20U + kind, words, nullptr, nullptr) + Header;
        NicMessage reply{};
        all = all && C::ReadNicFrame(delivery, bytes, reply) && (reply.Method == C::NicReplyMethod) &&
              (reply.Transaction == (20U + kind)) && (reply.Words[1U] == kinds[kind]) &&
              (reply.Words[2U] == (4U + kind)) && (reply.Words[3U] == (~UInt64{0U} - kind)) &&
              (reply.Reply.Selector == 0U) && (reply.Data.Selector == 0U);
    }
    ASTERIA_EXPECT(runner, "every reply kind reads back with its detail and extra and no capability", all);
}

void TestRefusals(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    NicMessage message{};
    message.Transaction = 0x5EU;
    const Size bytes = Build(delivery, C::NicSendMethod, 9U, {3U, 60U, 0U, 0U});
    ASTERIA_EXPECT(runner, "a request one byte short or long is refused, the message unchanged",
                   !C::ReadNicFrame(delivery, bytes - 1U, message), !C::ReadNicFrame(delivery, bytes + 1U, message),
                   !C::ReadNicFrame(delivery, Header + 8U, message), message.Transaction == 0x5EU);

    delivery[Header + 16U] ^= 1U;
    const bool foreign = C::ReadNicFrame(delivery, bytes, message);
    delivery[Header + 16U] ^= 1U;
    delivery[Header + 31U] ^= 0x80U;
    const bool foreignHigh = C::ReadNicFrame(delivery, bytes, message);
    delivery[Header + 31U] ^= 0x80U;
    ASTERIA_EXPECT(runner, "a foreign interface is refused", !foreign, !foreignHigh,
                   C::ReadNicFrame(delivery, bytes, message));

    ASTERIA_EXPECT(runner, "a Send of 13 or 1515 bytes, or with a nonzero word, is refused; 14 and 1514 pass",
                   !C::ReadNicFrame(delivery, Build(delivery, C::NicSendMethod, 9U, {3U, 13U, 0U, 0U}), message),
                   !C::ReadNicFrame(delivery, Build(delivery, C::NicSendMethod, 9U, {3U, 1515U, 0U, 0U}), message),
                   !C::ReadNicFrame(delivery, Build(delivery, C::NicSendMethod, 9U, {3U, 60U, 1U, 0U}), message),
                   !C::ReadNicFrame(delivery, Build(delivery, C::NicSendMethod, 9U, {3U, 60U, 0U, 1U}), message),
                   C::ReadNicFrame(delivery, Build(delivery, C::NicSendMethod, 9U, {3U, 14U, 0U, 0U}), message),
                   C::ReadNicFrame(delivery, Build(delivery, C::NicSendMethod, 9U, {3U, 1514U, 0U, 0U}), message));
    ASTERIA_EXPECT(runner, "a ReceiveCredit or Query with a nonzero word is refused",
                   !C::ReadNicFrame(delivery, Build(delivery, C::NicReceiveMethod, 9U, {3U, 1U, 0U, 0U}), message),
                   !C::ReadNicFrame(delivery, Build(delivery, C::NicReceiveMethod, 9U, {3U, 0U, 0U, 1U}), message),
                   !C::ReadNicFrame(delivery,
                                    C::WriteNicFrame(delivery + Header, C::NicQueryMethod, 9U, {3U, 0U, 1U, 0U},
                                                     &ReplyHandle, nullptr) +
                                        Header,
                                    message));

    // A Send with one attachment, a Query with two, a data attachment that
    // is no Frame, a reply attachment that is no Endpoint.
    const Size oneBytes =
        C::WriteNicFrame(delivery + Header, C::NicQueryMethod, 9U, {3U, 60U, 0U, 0U}, &ReplyHandle, nullptr) + Header;
    delivery[Header + 32U] = static_cast<UInt8>(C::NicSendMethod);
    const bool oneAttachment = C::ReadNicFrame(delivery, oneBytes, message);
    const Size twoBytes = Build(delivery, C::NicReceiveMethod, 9U, {3U, 0U, 0U, 0U});
    delivery[Header + 32U] = static_cast<UInt8>(C::NicQueryMethod);
    const bool twoAttachments = C::ReadNicFrame(delivery, twoBytes, message);
    const Size typedBytes = Build(delivery, C::NicSendMethod, 9U, {3U, 60U, 0U, 0U});
    delivery[Header + C::EchoHeaderBytes + C::EchoAttachmentBytes + 27U] = C::EchoEndpointType;
    const bool wrongData = C::ReadNicFrame(delivery, typedBytes, message);
    delivery[Header + C::EchoHeaderBytes + C::EchoAttachmentBytes + 27U] = C::EchoFrameType;
    delivery[Header + C::EchoHeaderBytes + 27U] = C::EchoFrameType;
    const bool wrongReply = C::ReadNicFrame(delivery, typedBytes, message);
    ASTERIA_EXPECT(runner, "attachments of the wrong count or object type are refused", !oneAttachment, !twoAttachments,
                   !wrongData, !wrongReply);

    const Size unknownBytes = Build(delivery, C::NicSendMethod, 9U, {3U, 60U, 0U, 0U});
    delivery[Header + 32U] = 5U;
    const bool unknown = C::ReadNicFrame(delivery, unknownBytes, message);
    delivery[Header + 32U] = static_cast<UInt8>(C::NicSendMethod);
    delivery[Header + 34U] = 1U;
    const bool wide = C::ReadNicFrame(delivery, unknownBytes, message);
    ASTERIA_EXPECT(runner, "an unknown method, or one with high bits, is refused", !unknown, !wide);

    const UInt64 badKind[4U]{1U, 6U, 0U, 0U};
    const UInt64 zeroKind[4U]{1U, 0U, 0U, 0U};
    UInt8 scratch[C::NicDataRequestBytes]{};
    const UInt64 words[4U]{};
    ASTERIA_EXPECT(
        runner, "an unknown reply kind is refused; a frame without its attachments writes nothing",
        !C::ReadNicFrame(delivery,
                         C::WriteNicFrame(delivery + Header, C::NicReplyMethod, 1U, badKind, nullptr, nullptr) + Header,
                         message),
        !C::ReadNicFrame(
            delivery, C::WriteNicFrame(delivery + Header, C::NicReplyMethod, 1U, zeroKind, nullptr, nullptr) + Header,
            message),
        C::WriteNicFrame(scratch, C::NicSendMethod, 1U, words, &ReplyHandle, nullptr) == 0U,
        C::WriteNicFrame(scratch, C::NicQueryMethod, 1U, words, nullptr, nullptr) == 0U,
        C::WriteNicFrame(scratch, 9U, 1U, words, &ReplyHandle, &DataHandle) == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"NetFrameTests"};
    Asteria::Test::TestFramesDecode(runner);
    Asteria::Test::TestConstants(runner);
    Asteria::Test::TestRequestsRead(runner);
    Asteria::Test::TestRefusals(runner);
    return runner.Finish();
}
