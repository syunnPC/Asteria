// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/DriverFrame.hpp"
#include "User/Component/FsFrame.hpp"

#include "Support/TestContext.hpp"

#include <initializer_list>

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
using User::Component::DisplayInfo;
using User::Component::DisplayMessage;
using User::System::CapabilityHandle;

constexpr Size Header{User::Component::EchoDeliveryHeaderBytes};
constexpr Size DeliveryBytes{Header + User::Component::DisplayMaximumFrameBytes};
constexpr CapabilityHandle ReplyHandle{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
constexpr Size PayloadAt{Header + User::Component::EchoHeaderBytes};
constexpr Size QueryPayloadAt{PayloadAt + User::Component::EchoAttachmentBytes};
constexpr Size TextAt{PayloadAt + User::Component::DisplayPayloadBytes};
constexpr DisplayInfo Info{.Version = 1U,
                           .Features = 1U,
                           .Width = 1280U,
                           .Height = 800U,
                           .Columns = 160U,
                           .Rows = 50U,
                           .Epoch = 0x1'0000'0002ULL};

[[nodiscard]] UInt64 PayloadWord(const MessageEnvelope& envelope, const Size word) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(envelope.Payload[(word * 8U) + index]) << (index * 8U);
    }
    return value;
}

bool Reads(const UInt8* const delivery, const Size bytes, DisplayMessage& message) noexcept
{
    return User::Component::ReadDisplayFrame(delivery, bytes, message);
}

// Wraps a frame already written at delivery + Header: its delivery's size.
Size BuildWrite(UInt8* const delivery, const UInt64 transaction, const UInt8* const bytes, const Size count) noexcept
{
    const Size size = User::Component::WriteDisplayConsoleWrite(delivery + Header, transaction, bytes, count);
    return (size == 0U) ? 0U : (size + Header);
}

Size BuildQuery(UInt8* const delivery, const UInt64 transaction) noexcept
{
    return User::Component::WriteDisplayQuery(delivery + Header, transaction, ReplyHandle) + Header;
}

Size BuildReply(UInt8* const delivery, const UInt64 transaction, const UInt64 kind) noexcept
{
    const Size size = User::Component::WriteDisplayReply(delivery + Header, transaction, kind, Info);
    return (size == 0U) ? 0U : (size + Header);
}

void TestFramesDecode(TestContext& runner) noexcept
{
    static UInt8 frame[User::Component::DisplayMaximumFrameBytes];
    MessageEnvelope decoded{};
    const UInt8 text[5U]{'h', 'e', 'l', 'l', 'o'};
    const Size writeBytes = User::Component::WriteDisplayConsoleWrite(frame, 7U, text, 5U);
    const MessageCodecError writeError = MessageCodec::Decode(ReadOnlyByteView{frame, writeBytes}, decoded);
    ASTERIA_EXPECT(runner, "a console write decodes with no attachment and its 224-byte payload",
                   writeBytes == User::Component::DisplayConsoleWriteBytes, writeBytes == 312U,
                   writeError == MessageCodecError::None,
                   decoded.Interface.GetLow() == User::Component::DisplayInterfaceLow,
                   decoded.Interface.GetHigh() == User::Component::DisplayInterfaceHigh,
                   decoded.MethodIdentifier == User::Component::DisplayConsoleWriteMethod,
                   decoded.TransactionIdentifier == 7U, decoded.AttachmentCount == 0U, decoded.PayloadSizeBytes == 224U,
                   PayloadWord(decoded, 0U) == 5U, PayloadWord(decoded, 1U) == 0U, decoded.Payload[32U] == 'h',
                   decoded.Payload[36U] == 'o', decoded.Payload[37U] == 0U, decoded.Payload[223U] == 0U);

    MessageEnvelope query{};
    const Size queryBytes = User::Component::WriteDisplayQuery(frame, 8U, ReplyHandle);
    const MessageCodecError queryError = MessageCodec::Decode(ReadOnlyByteView{frame, queryBytes}, query);
    ASTERIA_EXPECT(runner, "a query decodes with its Invoke copy attachment",
                   queryBytes == User::Component::DisplayQueryBytes, queryBytes == 152U,
                   queryError == MessageCodecError::None, query.MethodIdentifier == User::Component::DisplayQueryMethod,
                   query.TransactionIdentifier == 8U, query.AttachmentCount == 1U,
                   query.Attachments[0U].Handle.GetSelector() == 0x0123U,
                   query.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   query.Attachments[0U].TransferMode == CapabilityTransferMode::Copy,
                   query.Attachments[0U].RequestedRights.GetRawValue() == User::Component::EchoInvokeRight,
                   query.PayloadSizeBytes == 32U, PayloadWord(query, 0U) == 0U, PayloadWord(query, 3U) == 0U);

    MessageEnvelope reply{};
    const Size replyBytes = User::Component::WriteDisplayReply(frame, 9U, User::Component::DisplayDone, Info);
    const MessageCodecError replyError = MessageCodec::Decode(ReadOnlyByteView{frame, replyBytes}, reply);
    ASTERIA_EXPECT(runner, "a reply decodes with no attachment and its eight words",
                   replyBytes == User::Component::EchoHeaderBytes + User::Component::DisplayLegacyReplyPayloadBytes,
                   replyBytes == 168U, replyError == MessageCodecError::None,
                   reply.MethodIdentifier == User::Component::DisplayReplyMethod, reply.AttachmentCount == 0U,
                   reply.PayloadSizeBytes == 80U, PayloadWord(reply, 0U) == 9U, PayloadWord(reply, 1U) == 1U,
                   PayloadWord(reply, 2U) == 1U, PayloadWord(reply, 3U) == 1U,
                   PayloadWord(reply, 4U) == (1280ULL | (800ULL << 32U)),
                   PayloadWord(reply, 5U) == (160ULL | (50ULL << 32U)), PayloadWord(reply, 6U) == 0x1'0000'0002ULL,
                   PayloadWord(reply, 7U) == 0U);
}

void TestConstants(TestContext& runner) noexcept
{
    namespace C = User::Component;
    ASTERIA_EXPECT(runner, "the sizes and methods follow the decision", C::DisplayMaximumWriteBytes == 192U,
                   C::DisplayConsoleWriteBytes == 312U, C::DisplayQueryBytes == 152U, C::DisplayReplyBytes == 240U,
                   C::DisplayReplyWords == 19U, C::DisplayConsoleWriteMethod == 1U, C::DisplayQueryMethod == 2U,
                   C::DisplayReplyMethod == 3U, C::DisplaySetModeMethod == 4U, C::DisplayCreateSurfaceMethod == 5U,
                   C::DisplayPresentMethod == 6U, C::DisplayDestroySurfaceMethod == 7U,
                   C::DisplayEpochChangedMethod == 8U, C::DisplayDone == 1U, C::DisplayRefused == 2U,
                   C::DisplayProtocolVersion == 2U, C::DisplayFeatureConsole == 1U,
                   C::DisplayConsoleWritePayloadBytes <= MessageEnvelope::MaximumPayloadBytes);
    ASTERIA_EXPECT(
        runner, "the interface differs from the block, file and driver protocols' and the echo's",
        (C::DisplayInterfaceLow != C::BlockInterfaceLow) || (C::DisplayInterfaceHigh != C::BlockInterfaceHigh),
        (C::DisplayInterfaceLow != C::FsInterfaceLow) || (C::DisplayInterfaceHigh != C::FsInterfaceHigh),
        (C::DisplayInterfaceLow != C::DriverInterfaceLow) || (C::DisplayInterfaceHigh != C::DriverInterfaceHigh),
        C::DisplayInterfaceLow != C::FsInterfaceLow, C::DisplayInterfaceLow != C::BlockInterfaceLow,
        C::DisplayInterfaceHigh != C::FsInterfaceHigh, C::DisplayInterfaceHigh != C::BlockInterfaceHigh,
        C::DisplayInterfaceLow != C::DisplayInterfaceHigh);
    ASTERIA_EXPECT(runner, "the selector and tag are nonzero and differ from the other services'",
                   C::DisplaySealedSelector != 0U, C::DisplayServiceWaitTag != 0U,
                   C::DisplaySealedSelector != C::FsSealedSelector, C::DisplaySealedSelector != C::BlockSealedSelector,
                   C::DisplaySealedSelector != C::DriverSealedSelector,
                   C::DisplaySealedSelector != C::EchoSealedSelector, C::DisplayServiceWaitTag != C::FsServiceWaitTag,
                   C::DisplayServiceWaitTag != C::BlockServiceWaitTag,
                   C::DisplayServiceWaitTag != C::DriverServiceWaitTag, C::DisplayServiceWaitTag != C::EchoWaitTag);
}

void TestWritesRead(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    DisplayMessage message{};

    // Every byte value survives, in 192-byte writes and shorter ones.
    UInt8 all[User::Component::DisplayMaximumWriteBytes]{};
    bool survive{true};
    for (Size base{0U}; base < 256U; base += 64U)
    {
        for (Size index{0U}; index < sizeof(all); ++index)
        {
            all[index] = static_cast<UInt8>(base + index);
        }
        for (const Size count : {Size{1U}, Size{64U}, Size{191U}, Size{192U}})
        {
            const Size bytes = BuildWrite(delivery, 100U + base, all, count);
            survive = survive && (bytes == (Header + User::Component::DisplayConsoleWriteBytes)) &&
                      Reads(delivery, bytes, message) &&
                      (message.Method == User::Component::DisplayConsoleWriteMethod) &&
                      (message.Transaction == (100U + base)) && (message.Count == count) &&
                      (message.Words[0U] == count) && (message.Reply.Selector == 0U);
            for (Size index{0U}; survive && (index < count); ++index)
            {
                survive = message.Text[index] == all[index];
            }
            for (Size index{count}; survive && (index < sizeof(all)); ++index)
            {
                survive = message.Text[index] == 0U;
            }
        }
    }
    ASTERIA_EXPECT(runner, "text bytes of all 256 values read back, in writes of 1 to 192 bytes", survive);

    const UInt8 special[6U]{'\n', 0x00U, 0xFFU, '\r', 0x0DU, 0x00U};
    const Size specialBytes = BuildWrite(delivery, 5U, special, 6U);
    ASTERIA_EXPECT(runner, "a newline, zero bytes and 0xFF survive; the text points into the delivery",
                   Reads(delivery, specialBytes, message), message.Count == 6U, message.Text == (delivery + TextAt),
                   message.Text[0U] == '\n', message.Text[1U] == 0U, message.Text[2U] == 0xFFU, message.Text[5U] == 0U);

    DisplayMessage query{};
    const Size queryBytes = BuildQuery(delivery, 0xFEDC'BA98'7654'3210ULL);
    ASTERIA_EXPECT(runner, "a query reads back with its reply handle", Reads(delivery, queryBytes, query),
                   query.Method == User::Component::DisplayQueryMethod, query.Transaction == 0xFEDC'BA98'7654'3210ULL,
                   query.Count == 0U, query.Text == nullptr, query.Reply.Selector == 0x0123U,
                   query.Reply.Authenticator == 0xAAAA'BBBB'CCCC'DDDDULL, query.Words[0U] == 0U, query.Words[3U] == 0U);

    DisplayMessage reply{};
    const Size replyBytes = BuildReply(delivery, 31U, User::Component::DisplayDone);
    const bool replyRead = Reads(delivery, replyBytes, reply);
    const DisplayInfo unpacked = User::Component::UnpackDisplayInfo(reply);
    ASTERIA_EXPECT(
        runner, "a reply reads back and unpacks its fields", replyRead,
        reply.Method == User::Component::DisplayReplyMethod, reply.Transaction == 31U, reply.Words[0U] == 31U,
        reply.Words[1U] == User::Component::DisplayDone, reply.Reply.Selector == 0U, reply.Text == nullptr,
        unpacked.Version == 1U, unpacked.Features == User::Component::DisplayFeatureConsole, unpacked.Width == 1280U,
        unpacked.Height == 800U, unpacked.Columns == 160U, unpacked.Rows == 50U, unpacked.Epoch == 0x1'0000'0002ULL);

    DisplayMessage refused{};
    const Size refusedBytes = BuildReply(delivery, 32U, User::Component::DisplayRefused);
    ASTERIA_EXPECT(runner, "a refusal reads back", Reads(delivery, refusedBytes, refused),
                   refused.Words[1U] == User::Component::DisplayRefused, refused.Words[0U] == 32U);
}

void TestMetricsExtension(TestContext& runner) noexcept
{
    namespace C = User::Component;
    static UInt8 delivery[DeliveryBytes]{};
    DisplayInfo info = Info;
    info.Features |= C::DisplayFeatureMetrics | C::DisplayFeatureTiming;
    info.ScanoutPolicy = C::DisplayScanoutWc;
    info.FillBytes = 1228800U;
    info.FillMicroseconds = 3200U;
    info.LatestPixels = 307200U;
    info.LatestMicroseconds = 41000U;
    info.SlowestPixels = 307200U;
    info.SlowestMicroseconds = 52000U;
    const Size bytes = C::WriteDisplayReply(delivery + Header, 10U, C::DisplayDone, info) + Header;
    DisplayMessage message{};
    ASTERIA_EXPECT(runner, "metrics reply extends v2 canonically and retains pointer fields",
                   bytes == Header + C::EchoHeaderBytes + C::DisplayMetricsReplyPayloadBytes,
                   Reads(delivery, bytes, message));
    const DisplayInfo unpacked = C::UnpackDisplayInfo(message);
    ASTERIA_EXPECT(runner, "scanout and both scroll measurements survive the wire",
                   unpacked.ScanoutPolicy == info.ScanoutPolicy, unpacked.FillBytes == info.FillBytes,
                   unpacked.FillMicroseconds == info.FillMicroseconds, unpacked.LatestPixels == info.LatestPixels,
                   unpacked.LatestMicroseconds == info.LatestMicroseconds, unpacked.SlowestPixels == info.SlowestPixels,
                   unpacked.SlowestMicroseconds == info.SlowestMicroseconds);
    delivery[PayloadAt + 24U] ^= static_cast<UInt8>(C::DisplayFeatureMetrics);
    ASTERIA_EXPECT(runner, "extension length requires its feature bit", !Reads(delivery, bytes, message));
    const Size legacy = BuildReply(delivery, 11U, C::DisplayDone);
    ASTERIA_EXPECT(runner, "legacy v2 reply still decodes with zero extension words", Reads(delivery, legacy, message),
                   C::UnpackDisplayInfo(message).FillBytes == 0U,
                   C::UnpackDisplayInfo(message).SlowestMicroseconds == 0U);
    delivery[PayloadAt + 24U] |= static_cast<UInt8>(C::DisplayFeatureMetrics);
    ASTERIA_EXPECT(runner, "truncated extension advertised in legacy reply is refused",
                   !Reads(delivery, legacy, message));
}

void TestRefusals(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    DisplayMessage message{};
    const UInt8 text[5U]{'h', 'e', 'l', 'l', 'o'};

    // Sizes: every frame one byte short or long, or cut.
    const Size writeBytes = BuildWrite(delivery, 1U, text, 5U);
    ASTERIA_EXPECT(runner, "a console write one byte short or long, or cut, is refused",
                   Reads(delivery, writeBytes, message), !Reads(delivery, writeBytes - 1U, message),
                   !Reads(delivery, writeBytes + 1U, message), !Reads(delivery, Header + 8U, message),
                   !Reads(delivery, 0U, message));
    const Size queryBytes = BuildQuery(delivery, 1U);
    ASTERIA_EXPECT(runner, "a query one byte short or long is refused", Reads(delivery, queryBytes, message),
                   !Reads(delivery, queryBytes - 1U, message), !Reads(delivery, queryBytes + 1U, message));
    const Size replyBytes = BuildReply(delivery, 1U, User::Component::DisplayDone);
    ASTERIA_EXPECT(runner, "a reply one byte short or long is refused", Reads(delivery, replyBytes, message),
                   !Reads(delivery, replyBytes - 1U, message), !Reads(delivery, replyBytes + 1U, message));

    // Interface words.
    delivery[Header + 16U] ^= 1U;
    const bool low = Reads(delivery, replyBytes, message);
    delivery[Header + 16U] ^= 1U;
    delivery[Header + 31U] ^= 1U;
    const bool high = Reads(delivery, replyBytes, message);
    delivery[Header + 31U] ^= 1U;
    ASTERIA_EXPECT(runner, "a foreign interface, either word, is refused", !low, !high,
                   Reads(delivery, replyBytes, message));
    // A file protocol reply is no display frame.
    UInt8 foreign[User::Component::FsReplyBytes]{};
    const UInt64 fsWords[User::Component::FsPayloadWords]{1U, User::Component::FsDone, 0U, 0U};
    const Size foreignBytes = User::Component::WriteFsFrame(foreign, User::Component::FsReplyMethod, 1U, fsWords,
                                                            nullptr, 0U, nullptr, nullptr);
    for (Size index{0U}; index < foreignBytes; ++index)
    {
        delivery[Header + index] = foreign[index];
    }
    ASTERIA_EXPECT(runner, "a file protocol frame is refused", !Reads(delivery, Header + foreignBytes, message));
    static_cast<void>(BuildReply(delivery, 1U, User::Component::DisplayDone));

    // Methods: 0 and 4 to 9 refused, and a wide method word.
    const Size methodBytes = BuildReply(delivery, 1U, User::Component::DisplayDone);
    bool methods{true};
    for (const UInt8 method :
         {UInt8{0U}, UInt8{4U}, UInt8{5U}, UInt8{6U}, UInt8{7U}, UInt8{8U}, UInt8{9U}, UInt8{255U}})
    {
        delivery[Header + 32U] = method;
        methods = methods && !Reads(delivery, methodBytes, message);
    }
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::DisplayReplyMethod);
    delivery[Header + 33U] = 1U;
    const bool wide = Reads(delivery, methodBytes, message);
    delivery[Header + 33U] = 0U;
    delivery[Header + 34U] = 1U;
    const bool wider = Reads(delivery, methodBytes, message);
    delivery[Header + 34U] = 0U;
    ASTERIA_EXPECT(runner, "methods 0 and 4 to 9 and a wide method word are refused", methods, !wide, !wider,
                   Reads(delivery, methodBytes, message));
    // Reserved methods in each frame shape.
    bool reserved{true};
    for (UInt32 method{User::Component::DisplaySetModeMethod}; method <= User::Component::DisplayEpochChangedMethod;
         ++method)
    {
        const Size shapes[3U]{BuildWrite(delivery, 1U, text, 5U), BuildQuery(delivery, 1U),
                              BuildReply(delivery, 1U, User::Component::DisplayDone)};
        reserved = reserved && (shapes[0U] != 0U);
        for (Size shape{0U}; shape < 3U; ++shape)
        {
            static_cast<void>(shape == 0U ? BuildWrite(delivery, 1U, text, 5U)
                                          : (shape == 1U ? BuildQuery(delivery, 1U)
                                                         : BuildReply(delivery, 1U, User::Component::DisplayDone)));
            delivery[Header + 32U] = static_cast<UInt8>(method);
            reserved = reserved && !Reads(delivery, shapes[shape], message);
        }
    }
    ASTERIA_EXPECT(runner, "the reserved methods are refused in every frame shape", reserved);

    // Count: 0 and 193 and beyond 8 bits (the writer refuses them, so patch).
    const Size patched = BuildWrite(delivery, 1U, text, 5U);
    delivery[PayloadAt] = 0U;
    const bool zero = Reads(delivery, patched, message);
    delivery[PayloadAt] = 193U;
    const bool tooMany = Reads(delivery, patched, message);
    delivery[PayloadAt] = 255U;
    const bool maxByte = Reads(delivery, patched, message);
    delivery[PayloadAt] = 5U;
    delivery[PayloadAt + 1U] = 1U;
    const bool wideCount = Reads(delivery, patched, message);
    delivery[PayloadAt + 1U] = 0U;
    ASTERIA_EXPECT(runner, "a count of 0, 193, 255 or beyond 8 bits is refused", !zero, !tooMany, !maxByte, !wideCount,
                   Reads(delivery, patched, message));

    // Padding and reserved words.
    delivery[TextAt + 5U] = 'x';
    const bool padded = Reads(delivery, patched, message);
    delivery[TextAt + 5U] = 0U;
    delivery[TextAt + 191U] = 1U;
    const bool tail = Reads(delivery, patched, message);
    delivery[TextAt + 191U] = 0U;
    bool words{true};
    for (const Size word : {Size{1U}, Size{2U}, Size{3U}})
    {
        delivery[PayloadAt + (word * 8U)] = 1U;
        words = words && !Reads(delivery, patched, message);
        delivery[PayloadAt + (word * 8U)] = 0U;
    }
    ASTERIA_EXPECT(runner, "nonzero padding or a nonzero reserved word in a console write is refused", !padded, !tail,
                   words, Reads(delivery, patched, message));
    // A full 192-byte write has no padding to check.
    UInt8 full[User::Component::DisplayMaximumWriteBytes]{};
    for (UInt8& byte : full)
    {
        byte = 0xFFU;
    }
    ASTERIA_EXPECT(runner, "a console write of 192 bytes reads back",
                   Reads(delivery, BuildWrite(delivery, 1U, full, 192U), message), message.Count == 192U,
                   message.Text[191U] == 0xFFU);

    // Attachments: a console write or a reply carrying one, a query without.
    const Size withBytes = BuildQuery(delivery, 1U);
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::DisplayConsoleWriteMethod);
    const bool writeWithAttachment = Reads(delivery, withBytes, message);
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::DisplayReplyMethod);
    const bool replyWithAttachment = Reads(delivery, withBytes, message);
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::DisplayQueryMethod);
    delivery[Header + 81U] = 0U;
    const bool queryWithout = Reads(delivery, withBytes, message);
    delivery[Header + 81U] = 2U;
    const bool queryTwo = Reads(delivery, withBytes, message);
    delivery[Header + 81U] = 1U;
    delivery[Header + 80U] = 1U;
    const bool descriptors = Reads(delivery, withBytes, message);
    delivery[Header + 80U] = 0U;
    ASTERIA_EXPECT(runner, "an attachment on a write or a reply, or a query with none or two, is refused",
                   !writeWithAttachment, !replyWithAttachment, !queryWithout, !queryTwo, !descriptors,
                   Reads(delivery, withBytes, message));
    // The attachment's object type: a Frame is no reply Endpoint.
    delivery[Header + User::Component::EchoHeaderBytes + 27U] = User::Component::EchoFrameType;
    const bool wrongType = Reads(delivery, withBytes, message);
    delivery[Header + User::Component::EchoHeaderBytes + 27U] = User::Component::EchoEndpointType;
    ASTERIA_EXPECT(runner, "a query whose attachment is no Endpoint is refused", !wrongType,
                   Reads(delivery, withBytes, message));

    // Query words.
    bool queryWords{true};
    for (Size word{0U}; word < 4U; ++word)
    {
        delivery[QueryPayloadAt + (word * 8U) + 3U] = 1U;
        queryWords = queryWords && !Reads(delivery, withBytes, message);
        delivery[QueryPayloadAt + (word * 8U) + 3U] = 0U;
    }
    ASTERIA_EXPECT(runner, "a query with a nonzero word is refused", queryWords);

    // Reply: kinds, the transaction word, the last word.
    const Size answer = BuildReply(delivery, 41U, User::Component::DisplayDone);
    bool kinds{true};
    for (const UInt8 kind : {UInt8{0U}, UInt8{3U}, UInt8{255U}})
    {
        delivery[PayloadAt + 8U] = kind;
        kinds = kinds && !Reads(delivery, answer, message);
    }
    delivery[PayloadAt + 8U] = 2U;
    const bool two = Reads(delivery, answer, message);
    delivery[PayloadAt + 8U] = 1U;
    delivery[PayloadAt + 9U] = 1U;
    const bool wideKind = Reads(delivery, answer, message);
    delivery[PayloadAt + 9U] = 0U;
    delivery[PayloadAt] ^= 1U;
    const bool otherTransaction = Reads(delivery, answer, message);
    delivery[PayloadAt] ^= 1U;
    delivery[PayloadAt + 72U] = 1U;
    const bool lastWord = Reads(delivery, answer, message);
    delivery[PayloadAt + 72U] = 0U;
    ASTERIA_EXPECT(runner, "reply kinds 0 and 3, a wide kind, a foreign transaction and a last word are refused", kinds,
                   two, !wideKind, !otherTransaction, !lastWord, Reads(delivery, answer, message));

    // The writers' own refusals.
    UInt8 scratch[User::Component::DisplayMaximumFrameBytes]{};
    const DisplayInfo none{};
    ASTERIA_EXPECT(runner, "a count of 0 or above 192, a null text or another reply kind writes nothing",
                   User::Component::WriteDisplayConsoleWrite(scratch, 1U, text, 0U) == 0U,
                   User::Component::WriteDisplayConsoleWrite(scratch, 1U, full, 193U) == 0U,
                   User::Component::WriteDisplayConsoleWrite(scratch, 1U, nullptr, 5U) == 0U,
                   User::Component::WriteDisplayReply(scratch, 1U, 0U, none) == 0U,
                   User::Component::WriteDisplayReply(scratch, 1U, 3U, none) == 0U, scratch[0U] == 0U);

    // A refused frame leaves the message alone.
    DisplayMessage untouched{};
    untouched.Method = 99U;
    untouched.Words[2U] = 77U;
    untouched.Count = 5U;
    delivery[PayloadAt] = 0U;
    ASTERIA_EXPECT(runner, "a refused frame leaves the message unchanged", !Reads(delivery, patched, untouched),
                   untouched.Method == 99U, untouched.Words[2U] == 77U, untouched.Count == 5U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DisplayFrameTests"};
    Asteria::Test::TestFramesDecode(runner);
    Asteria::Test::TestConstants(runner);
    Asteria::Test::TestWritesRead(runner);
    Asteria::Test::TestRefusals(runner);
    Asteria::Test::TestMetricsExtension(runner);
    return runner.Finish();
}
