// SPDX-License-Identifier: Apache-2.0
#include "Abi/IpcCanonicalFrame.hpp"
#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/DriverFrame.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/NetFrame.hpp"
#include "User/Component/SocketFrame.hpp"

#include "Support/TestContext.hpp"

#include <cstring>

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
using User::Component::SocketMessage;
using User::System::CapabilityHandle;
namespace C = User::Component;

constexpr Size Header{C::EchoDeliveryHeaderBytes};
constexpr Size DeliveryBytes{Header + C::SocketMaximumFrameBytes};
constexpr CapabilityHandle ReplyHandle{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
constexpr UInt8 Hello[5U]{'h', 'e', 'l', 'l', 'o'};
// The first payload byte of a request and of a reply, and of their inline
// areas, in a delivery.
constexpr Size RequestPayload{Header + C::EchoHeaderBytes + C::EchoAttachmentBytes};
constexpr Size ReplyPayload{Header + C::EchoHeaderBytes};
constexpr Size RequestInline{RequestPayload + C::SocketPayloadBytes};
constexpr Size ReplyInline{ReplyPayload + C::SocketPayloadBytes};

[[nodiscard]] UInt64 PayloadWord(const MessageEnvelope& envelope, const Size word) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(envelope.Payload[(word * 8U) + index]) << (index * 8U);
    }
    return value;
}

// Builds method's frame with words and inline bytes in a delivery (a 48-byte
// header before it) and returns the delivery's size, or zero when the writer
// refused.
Size Build(UInt8* const delivery, const UInt32 method, const UInt64 transaction, const UInt64 (&words)[4U],
           const UInt8* const bytes = nullptr, const Size length = 0U) noexcept
{
    const Size written = C::WriteSocketFrame(delivery + Header, method, transaction, words, bytes, length,
                                             (method == C::SocketReplyMethod) ? nullptr : &ReplyHandle);
    return (written == 0U) ? 0U : (written + Header);
}

// A reply answering transaction 9.
Size Reply(UInt8* const delivery, const UInt64 kind, const UInt64 detail, const UInt64 extra,
           const UInt8* const bytes = nullptr, const Size length = 0U) noexcept
{
    const UInt64 words[4U]{9U, kind, detail, extra};
    return Build(delivery, C::SocketReplyMethod, 9U, words, bytes, length);
}

[[nodiscard]] bool Reads(const UInt8* const delivery, const Size bytes) noexcept
{
    static SocketMessage message;
    return C::ReadSocketFrame(delivery, bytes, message);
}

// Every frame is a canonical frame the kernel's codec accepts: a request
// with its reply attachment (Invoke, copied) and the 224-byte payload, a
// reply with none.
void TestFramesDecode(TestContext& runner) noexcept
{
    static UInt8 frame[C::SocketMaximumFrameBytes];
    const UInt64 sendWords[4U]{3U, 7U | (UInt64{40000U} << 16U), 0x0A00'0202U, 5U};
    const Size sendBytes = C::WriteSocketFrame(frame, C::SocketSendMethod, 7U, sendWords, Hello, 5U, &ReplyHandle);
    MessageEnvelope decoded{};
    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{frame, sendBytes}, decoded);
    ASTERIA_EXPECT(runner, "a Send decodes with its reply attachment, words and inline datagram",
                   sendBytes == C::SocketRequestBytes, error == MessageCodecError::None,
                   decoded.Interface.GetLow() == C::SocketInterfaceLow,
                   decoded.Interface.GetHigh() == C::SocketInterfaceHigh,
                   decoded.MethodIdentifier == C::SocketSendMethod, decoded.TransactionIdentifier == 7U,
                   decoded.AttachmentCount == 1U, decoded.Attachments[0U].Handle.GetSelector() == 0x0123U,
                   decoded.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   decoded.Attachments[0U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[0U].RequestedRights.GetRawValue() == C::EchoInvokeRight,
                   decoded.PayloadSizeBytes == C::SocketFramePayloadBytes, PayloadWord(decoded, 0U) == 3U,
                   PayloadWord(decoded, 1U) == sendWords[1U], PayloadWord(decoded, 2U) == 0x0A00'0202U,
                   PayloadWord(decoded, 3U) == 5U, decoded.Payload[32U] == 'h', decoded.Payload[36U] == 'o',
                   decoded.Payload[37U] == 0U, decoded.Payload[223U] == 0U);

    const UInt64 replyWords[4U]{7U, C::SocketDone, 5U, 0x0A00'0202U | (UInt64{40000U} << 32U)};
    const Size replyBytes = C::WriteSocketFrame(frame, C::SocketReplyMethod, 7U, replyWords, Hello, 5U, &ReplyHandle);
    MessageEnvelope answered{};
    ASTERIA_EXPECT(runner, "a reply decodes with no attachment, even when a reply capability is given",
                   replyBytes == C::SocketReplyBytes,
                   MessageCodec::Decode(ReadOnlyByteView{frame, replyBytes}, answered) == MessageCodecError::None,
                   answered.AttachmentCount == 0U, answered.MethodIdentifier == C::SocketReplyMethod,
                   answered.PayloadSizeBytes == C::SocketFramePayloadBytes, PayloadWord(answered, 1U) == C::SocketDone,
                   answered.Payload[32U] == 'h');
}

void TestConstants(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "methods, kinds and reasons are numbered as the decision says", C::SocketRuleMethod == 1U,
                   C::SocketBindMethod == 2U, C::SocketSendMethod == 3U, C::SocketReceiveMethod == 4U,
                   C::SocketQueryMethod == 5U, C::SocketReplyMethod == 6U, C::SocketDone == 1U, C::SocketRefused == 2U,
                   C::SocketPending == 3U, C::SocketFailed == 4U, C::SocketRestarted == 5U,
                   C::SocketRefusedCredit == 1U, C::SocketRefusedNoRule == 2U, C::SocketRefusedNotBound == 3U,
                   C::SocketRefusedIdentity == 4U, C::SocketRefusedMalformed == 5U);
    ASTERIA_EXPECT(runner, "the drop counts' indices, credits, capacities and sizes follow the decision",
                   C::SocketDropNotForUs == 0U, C::SocketDropBadIcmp == 1U, C::SocketDropUnauthorized == 2U,
                   C::SocketDropNoReceive == 3U, C::SocketDropTooLong == 4U, C::SocketDropNoTransmit == 5U,
                   C::SocketDropMalformed == 6U, C::SocketDropCounts == 7U, C::SocketQueryInlineBytes == 56U,
                   C::SocketPayloadWords == 4U, C::SocketInlineBytes == 192U, C::SocketMaximumReceives == 2U,
                   C::SocketMaximumRequests == 2U, C::SocketReplyCapacity == 5U, C::SocketDriverReplyCapacity == 5U,
                   C::SocketRequestBytes == 344U, C::SocketReplyBytes == 312U, C::SocketMaximumFrameBytes == 344U,
                   C::SocketFramePayloadBytes == 224U,
                   (C::SocketMaximumFrameBytes + Header) <= C::DisplayDeliveryCapacity,
                   C::SocketFramePayloadBytes <= C::DisplayKernelPayloadLimit);
    const UInt64 echoLow{Abi::CanonicalAsynchronousFrame[16U]};
    constexpr UInt64 directoryLow{Executive::ObjectDirectory::ObjectDirectoryProtocol::Interface.GetLow()};
    constexpr UInt64 directoryHigh{Executive::ObjectDirectory::ObjectDirectoryProtocol::Interface.GetHigh()};
    ASTERIA_EXPECT(
        runner, "the interface differs from every other protocol's",
        (C::SocketInterfaceLow != C::FsInterfaceLow) || (C::SocketInterfaceHigh != C::FsInterfaceHigh),
        (C::SocketInterfaceLow != C::BlockInterfaceLow) || (C::SocketInterfaceHigh != C::BlockInterfaceHigh),
        (C::SocketInterfaceLow != C::NicInterfaceLow) || (C::SocketInterfaceHigh != C::NicInterfaceHigh),
        (C::SocketInterfaceLow != C::DisplayInterfaceLow) || (C::SocketInterfaceHigh != C::DisplayInterfaceHigh),
        (C::SocketInterfaceLow != C::DriverInterfaceLow) || (C::SocketInterfaceHigh != C::DriverInterfaceHigh),
        (C::SocketInterfaceLow != echoLow) || (C::SocketInterfaceHigh != 0U),
        (C::SocketInterfaceLow != directoryLow) || (C::SocketInterfaceHigh != directoryHigh),
        C::SocketInterfaceLow != C::SocketInterfaceHigh);
    ASTERIA_EXPECT(
        runner, "the selector and tags are nonzero and differ from the other services'", C::SocketSealedSelector != 0U,
        C::SocketServiceWaitTag != 0U, C::SocketDriverReplyWaitTag != 0U,
        C::SocketServiceWaitTag != C::SocketDriverReplyWaitTag, C::SocketSealedSelector != C::FsSealedSelector,
        C::SocketSealedSelector != C::BlockSealedSelector, C::SocketSealedSelector != C::NicSealedSelector,
        C::SocketSealedSelector != C::DisplaySealedSelector, C::SocketSealedSelector != C::DriverSealedSelector,
        C::SocketSealedSelector != C::EchoSealedSelector, C::SocketServiceWaitTag != C::FsServiceWaitTag,
        C::SocketServiceWaitTag != C::FsBlockReplyWaitTag, C::SocketServiceWaitTag != C::BlockServiceWaitTag,
        C::SocketServiceWaitTag != C::NicServiceWaitTag, C::SocketServiceWaitTag != C::NicInterruptTag,
        C::SocketServiceWaitTag != C::DisplayServiceWaitTag, C::SocketServiceWaitTag != C::DriverServiceWaitTag,
        C::SocketServiceWaitTag != C::EchoWaitTag, C::SocketDriverReplyWaitTag != C::FsServiceWaitTag,
        C::SocketDriverReplyWaitTag != C::FsBlockReplyWaitTag, C::SocketDriverReplyWaitTag != C::BlockServiceWaitTag,
        C::SocketDriverReplyWaitTag != C::NicServiceWaitTag, C::SocketDriverReplyWaitTag != C::NicInterruptTag,
        C::SocketDriverReplyWaitTag != C::DisplayServiceWaitTag, C::SocketDriverReplyWaitTag != C::DriverServiceWaitTag,
        C::SocketDriverReplyWaitTag != C::EchoWaitTag);
}

// Every method reads back with its words, its inline bytes and its length.
void TestRoundTrips(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    SocketMessage message{};
    const UInt64 ruleWords[4U]{3U, 0U, 7U | (UInt64{0xFFFFU} << 16U), 0xFFFF'FFFFU};
    ASTERIA_EXPECT(runner, "a Rule reads back with its handle and no inline bytes",
                   C::ReadSocketFrame(delivery, Build(delivery, C::SocketRuleMethod, 1U, ruleWords), message),
                   message.Method == C::SocketRuleMethod, message.Transaction == 1U, message.Words[0U] == 3U,
                   message.Words[2U] == ruleWords[2U], message.Words[3U] == 0xFFFF'FFFFU, message.InlineLength == 0U,
                   message.Reply.Selector == 0x0123U, message.Reply.Authenticator == 0xAAAA'BBBB'CCCC'DDDDULL);
    const UInt64 identityWords[4U]{3U, ~UInt64{0U}, 0U, 0U};
    const UInt64 bindWords[4U]{3U, 0xFFFFU, 0U, 0U};
    const UInt64 receiveWords[4U]{3U, 7U, 0U, 0U};
    const UInt64 queryWords[4U]{3U, 0U, 0U, 0U};
    SocketMessage bind{};
    SocketMessage receive{};
    SocketMessage query{};
    ASTERIA_EXPECT(runner, "a Rule's identity and zero ports, a Bind, a Receive and a Query read back",
                   C::ReadSocketFrame(delivery, Build(delivery, C::SocketRuleMethod, 2U, identityWords), message),
                   message.Words[1U] == ~UInt64{0U}, message.Words[2U] == 0U,
                   C::ReadSocketFrame(delivery, Build(delivery, C::SocketBindMethod, 3U, bindWords), bind),
                   bind.Method == C::SocketBindMethod, bind.Words[1U] == 0xFFFFU,
                   C::ReadSocketFrame(delivery, Build(delivery, C::SocketReceiveMethod, 4U, receiveWords), receive),
                   receive.Method == C::SocketReceiveMethod, receive.Words[1U] == 7U, receive.InlineLength == 0U,
                   C::ReadSocketFrame(delivery, Build(delivery, C::SocketQueryMethod, 5U, queryWords), query),
                   query.Method == C::SocketQueryMethod, query.Transaction == 5U);

    UInt8 full[C::SocketInlineBytes]{};
    for (Size index{0U}; index < C::SocketInlineBytes; ++index)
    {
        full[index] = static_cast<UInt8>(0xFFU - index);
    }
    const UInt64 sendWords[4U]{3U, 7U | (UInt64{9U} << 16U), 0x0A00'0202U, C::SocketInlineBytes};
    SocketMessage send{};
    const UInt64 oneWords[4U]{3U, 7U | (UInt64{9U} << 16U), 0x0A00'0202U, 1U};
    SocketMessage one{};
    ASTERIA_EXPECT(runner, "a Send of 192 bytes and one of one byte read back with their datagrams",
                   C::ReadSocketFrame(
                       delivery, Build(delivery, C::SocketSendMethod, 6U, sendWords, full, C::SocketInlineBytes), send),
                   send.InlineLength == C::SocketInlineBytes, std::memcmp(send.Inline, full, C::SocketInlineBytes) == 0,
                   send.Words[3U] == C::SocketInlineBytes,
                   C::ReadSocketFrame(delivery, Build(delivery, C::SocketSendMethod, 7U, oneWords, Hello, 1U), one),
                   one.InlineLength == 1U, one.Inline[0U] == 'h', one.Inline[1U] == 0U);

    // A reply is read whole: the request it answers decides which of its
    // inline bytes count (a Receive's datagram, a Query's counts).
    SocketMessage received{};
    SocketMessage queried{};
    SocketMessage sent{};
    SocketMessage empty{};
    SocketMessage refused{};
    UInt8 counts[C::SocketQueryInlineBytes]{};
    counts[C::SocketQueryInlineBytes - 1U] = 0x80U;
    const UInt64 source{0x0A00'0202U | (UInt64{40000U} << 32U)};
    ASTERIA_EXPECT(runner, "a Receive's, a Query's, a Send's and other replies are read with their whole area",
                   C::ReadSocketFrame(delivery, Reply(delivery, C::SocketDone, 5U, source, Hello, 5U), received),
                   received.Method == C::SocketReplyMethod, received.Words[0U] == 9U, received.Words[3U] == source,
                   received.InlineLength == C::SocketInlineBytes, received.Inline[4U] == 'o', received.Inline[5U] == 0U,
                   received.Reply.Selector == 0U,
                   C::ReadSocketFrame(delivery,
                                      Reply(delivery, C::SocketDone, 0x017E'A500'5452ULL,
                                            0x0A00'020FU | (UInt64{24U} << 32U), counts, C::SocketQueryInlineBytes),
                                      queried),
                   queried.InlineLength == C::SocketInlineBytes, queried.Inline[55U] == 0x80U,
                   C::ReadSocketFrame(delivery, Reply(delivery, C::SocketDone, 5U, 0U), sent),
                   sent.InlineLength == C::SocketInlineBytes, sent.Inline[0U] == 0U,
                   C::ReadSocketFrame(delivery, Reply(delivery, C::SocketDone, 0U, source), empty),
                   empty.InlineLength == C::SocketInlineBytes,
                   C::ReadSocketFrame(delivery, Reply(delivery, C::SocketRefused, C::SocketRefusedNoRule, 0U), refused),
                   refused.Words[1U] == C::SocketRefused, refused.Words[2U] == C::SocketRefusedNoRule,
                   Reads(delivery, Reply(delivery, C::SocketPending, 0U, 0U)),
                   Reads(delivery, Reply(delivery, C::SocketFailed, 3U, 4U)),
                   Reads(delivery, Reply(delivery, C::SocketRestarted, 0U, 0U)));
}

void TestRejections(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    SocketMessage message{};
    message.Transaction = 0x5EU;
    const UInt64 sendWords[4U]{3U, 7U | (UInt64{9U} << 16U), 0x0A00'0202U, 5U};
    const Size bytes = Build(delivery, C::SocketSendMethod, 9U, sendWords, Hello, 5U);
    ASTERIA_EXPECT(runner, "a frame one byte short or long, or without a frame header, is refused, the message kept",
                   !C::ReadSocketFrame(delivery, bytes - 1U, message),
                   !C::ReadSocketFrame(delivery, bytes + 1U, message),
                   !C::ReadSocketFrame(delivery, Header + 8U, message), message.Transaction == 0x5EU,
                   C::ReadSocketFrame(delivery, bytes, message));

    struct Flip
    {
        Size Offset;
        UInt8 Value;
    };
    // The header's interface, method and counts, the payload size, the
    // attachment's type; then the padding past the datagram.
    const Flip flips[]{
        {Header + 16U, 0x00U},
        {Header + 31U, 0x00U},
        {Header + 32U, 0x00U},
        {Header + 32U, 0x07U},
        {Header + 33U, 0x01U},
        {Header + 34U, 0x01U},
        {Header + 35U, 0x01U},
        {Header + 80U, 0x01U},
        {Header + 12U, 0xE1U},
        {Header + 13U, 0x01U},
        {Header + C::EchoHeaderBytes + 27U, C::EchoFrameType},
        {RequestInline + 5U, 0x01U},
        {RequestInline + C::SocketInlineBytes - 1U, 0x01U},
    };
    bool refused{true};
    for (const Flip& flip : flips)
    {
        const UInt8 saved = delivery[flip.Offset];
        delivery[flip.Offset] = flip.Value;
        refused = refused && !C::ReadSocketFrame(delivery, bytes, message);
        delivery[flip.Offset] = saved;
    }
    ASTERIA_EXPECT(runner, "a foreign interface, method, count, size or type, or padding past the datagram, is refused",
                   refused, message.Transaction == 9U, C::ReadSocketFrame(delivery, bytes, message));

    // Words out of their fields.
    const UInt64 badWords[][4U]{
        {3U, 0U, UInt64{1U} << 32U, 0U},
        {3U, 0U, 7U, UInt64{1U} << 32U},
    };
    const UInt64 badPorts[][4U]{
        {3U, 0x1'0000U, 0U, 0U},
        {3U, 7U, 1U, 0U},
        {3U, 7U, 0U, 1U},
    };
    const UInt64 badSends[][4U]{
        {3U, UInt64{1U} << 32U, 0x0A00'0202U, 5U},
        {3U, 7U, UInt64{1U} << 32U, 5U},
        {3U, 7U, 0x0A00'0202U, 0U},
        {3U, 7U, 0x0A00'0202U, C::SocketInlineBytes + 1U},
    };
    const UInt64 badQueries[][4U]{
        {3U, 1U, 0U, 0U},
        {3U, 0U, 1U, 0U},
        {3U, 0U, 0U, 1U},
    };
    bool words{true};
    for (const auto& rule : badWords)
    {
        words = words && !Reads(delivery, Build(delivery, C::SocketRuleMethod, 1U, rule));
    }
    for (const auto& port : badPorts)
    {
        words = words && !Reads(delivery, Build(delivery, C::SocketBindMethod, 1U, port)) &&
                !Reads(delivery, Build(delivery, C::SocketReceiveMethod, 1U, port));
    }
    for (const auto& send : badSends)
    {
        // As many bytes as the length gives, up to five: only the word is wrong.
        const Size length = (send[3U] < 5U) ? static_cast<Size>(send[3U]) : 5U;
        words = words && !Reads(delivery, Build(delivery, C::SocketSendMethod, 1U, send, Hello, length));
    }
    for (const auto& query : badQueries)
    {
        words = words && !Reads(delivery, Build(delivery, C::SocketQueryMethod, 1U, query));
    }
    ASTERIA_EXPECT(runner, "a bit beyond a port, an address or a length, or a nonzero reserved word, is refused",
                   words);

    // A request with inline bytes, a reply of an unknown kind or for another
    // transaction, or one with an attachment; a reply's padding is not
    // checked.
    const UInt64 queryWords[4U]{3U, 0U, 0U, 0U};
    const UInt64 source{0x0A00'0202U | (UInt64{40000U} << 32U)};
    UInt8 counts[C::SocketInlineBytes]{};
    counts[C::SocketQueryInlineBytes] = 1U;
    const UInt64 otherWords[4U]{8U, C::SocketDone, 0U, 0U};
    const UInt64 zeroKind[4U]{9U, 0U, 0U, 0U};
    const UInt64 sixthKind[4U]{9U, 6U, 0U, 0U};
    const Size withAttachment =
        C::WriteSocketFrame(delivery + Header, C::SocketQueryMethod, 9U, queryWords, nullptr, 0U, &ReplyHandle) +
        Header;
    delivery[Header + 32U] = static_cast<UInt8>(C::SocketReplyMethod);
    const bool attachedReply = Reads(delivery, withAttachment);
    const Size withoutAttachment = Reply(delivery, C::SocketDone, 0U, 0U);
    delivery[Header + 32U] = static_cast<UInt8>(C::SocketQueryMethod);
    const bool bareRequest = Reads(delivery, withoutAttachment);
    ASTERIA_EXPECT(
        runner,
        "a request's stray inline bytes, a stray kind or transaction, or a wrong attachment is refused; a reply's "
        "padding is read",
        !Reads(delivery, Build(delivery, C::SocketQueryMethod, 1U, queryWords, Hello, 1U)),
        Reads(delivery, Reply(delivery, C::SocketDone, 4U, source, Hello, 5U)),
        Reads(delivery,
              Reply(delivery, C::SocketDone, 0x017E'A500'5452ULL, 1U, counts, C::SocketQueryInlineBytes + 1U)),
        !Reads(delivery, Build(delivery, C::SocketReplyMethod, 9U, otherWords)),
        !Reads(delivery, Build(delivery, C::SocketReplyMethod, 9U, zeroKind)),
        !Reads(delivery, Build(delivery, C::SocketReplyMethod, 9U, sixthKind)), !attachedReply, !bareRequest);

    UInt8 scratch[C::SocketMaximumFrameBytes]{};
    ASTERIA_EXPECT(runner, "the writer refuses an unknown method, a request without reply and too many bytes",
                   C::WriteSocketFrame(scratch, 0U, 1U, queryWords, nullptr, 0U, &ReplyHandle) == 0U,
                   C::WriteSocketFrame(scratch, 7U, 1U, queryWords, nullptr, 0U, &ReplyHandle) == 0U,
                   C::WriteSocketFrame(scratch, C::SocketQueryMethod, 1U, queryWords, nullptr, 0U, nullptr) == 0U,
                   C::WriteSocketFrame(scratch, C::SocketSendMethod, 1U, sendWords, Hello, C::SocketInlineBytes + 1U,
                                       &ReplyHandle) == 0U,
                   C::WriteSocketFrame(scratch, C::SocketSendMethod, 1U, sendWords, nullptr, 5U, &ReplyHandle) == 0U,
                   scratch[0U] == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SocketFrameTests"};
    Asteria::Test::TestFramesDecode(runner);
    Asteria::Test::TestConstants(runner);
    Asteria::Test::TestRoundTrips(runner);
    Asteria::Test::TestRejections(runner);
    return runner.Finish();
}
