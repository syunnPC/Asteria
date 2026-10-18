// SPDX-License-Identifier: Apache-2.0
#include "Executive/ObjectDirectory/ObjectDirectoryTypes.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Component/FsFrame.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Executive::ObjectDirectory::DirectoryName;
using Executive::ObjectDirectory::DirectoryNameView;
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
using User::Component::FsMessage;
using User::System::CapabilityHandle;

constexpr Size Header{User::Component::EchoDeliveryHeaderBytes};
constexpr Size DeliveryBytes{Header + User::Component::FsMaximumFrameBytes};
constexpr CapabilityHandle ReplyHandle{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
constexpr CapabilityHandle DataHandle{0x0456U, 0x1111'2222'3333'4444ULL};
constexpr UInt8 Hello[5U]{'h', 'e', 'l', 'l', 'o'};

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
// returns the delivery's size, or zero when the writer refused.
Size Build(UInt8* const delivery, const UInt32 method, const UInt64 transaction,
           const UInt64 (&words)[User::Component::FsPayloadWords], const UInt8* const name,
           const Size nameLength) noexcept
{
    const bool data = (method == User::Component::FsReadMethod) || (method == User::Component::FsWriteMethod);
    const bool reply = method != User::Component::FsReplyMethod;
    UInt64 payload[User::Component::FsPayloadWords]{};
    for (Size at{0U}; at < User::Component::FsPayloadWords; ++at)
    {
        payload[at] = words[at];
    }
    if (method == User::Component::FsReadMethod)
    {
        payload[7U] = 1U;
    }
    const Size bytes = User::Component::WriteFsFrame(delivery + Header, method, transaction, payload, name, nameLength,
                                                     reply ? &ReplyHandle : nullptr, data ? &DataHandle : nullptr);
    return (bytes == 0U) ? 0U : (bytes + Header);
}

bool Reads(const UInt8* const delivery, const Size bytes, FsMessage& message) noexcept
{
    return User::Component::ReadFsFrame(delivery, bytes, message);
}

// An Open or Create of the named text, tag 5.
Size BuildNamed(UInt8* const delivery, const UInt32 method, const char* const text) noexcept
{
    UInt8 name[65U]{};
    Size length{0U};
    while ((length < 64U) && (text[length] != '\0'))
    {
        name[length] = static_cast<UInt8>(text[length]);
        ++length;
    }
    const UInt64 words[User::Component::FsPayloadWords]{5U, 0U, 0U, 0U};
    return Build(delivery, method, 1U, words, name, length);
}

void TestFramesDecode(TestContext& runner) noexcept
{
    static UInt8 frame[User::Component::FsMaximumFrameBytes];
    MessageEnvelope decoded{};

    const UInt64 ioWords[User::Component::FsPayloadWords]{5U, 6U, 7U, 2U};
    const Size ioBytes = User::Component::WriteFsFrame(frame, User::Component::FsWriteMethod, 7U, ioWords, nullptr, 0U,
                                                       &ReplyHandle, &DataHandle);
    const MessageCodecError ioError = MessageCodec::Decode(ReadOnlyByteView{frame, ioBytes}, decoded);
    ASTERIA_EXPECT(runner, "a write request decodes with its two attachments and payload",
                   ioBytes == User::Component::FsIoRequestBytes, ioError == MessageCodecError::None,
                   decoded.Interface.GetLow() == User::Component::FsInterfaceLow,
                   decoded.Interface.GetHigh() == User::Component::FsInterfaceHigh,
                   decoded.MethodIdentifier == User::Component::FsWriteMethod, decoded.TransactionIdentifier == 7U,
                   decoded.AttachmentCount == 2U, decoded.Attachments[0U].Handle.GetSelector() == 0x0123U,
                   decoded.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   decoded.Attachments[0U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[0U].RequestedRights.GetRawValue() == User::Component::EchoInvokeRight,
                   decoded.Attachments[1U].Handle.GetSelector() == 0x0456U,
                   decoded.Attachments[1U].ExpectedType == ObjectType::Frame,
                   decoded.Attachments[1U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[1U].RequestedRights.GetRawValue() == 0x0118U,
                   decoded.PayloadSizeBytes == User::Component::FsPayloadBytes, PayloadWord(decoded, 0U) == 5U,
                   PayloadWord(decoded, 1U) == 6U, PayloadWord(decoded, 2U) == 7U, PayloadWord(decoded, 3U) == 2U);

    const UInt64 commitWords[User::Component::FsPayloadWords]{5U, 9U, User::Component::FsCrashAfterSuperblock, 0U};
    const Size commitBytes = User::Component::WriteFsFrame(frame, User::Component::FsCommitMethod, 8U, commitWords,
                                                           nullptr, 0U, &ReplyHandle, nullptr);
    MessageEnvelope commit{};
    ASTERIA_EXPECT(runner, "a commit and a query decode with one attachment",
                   commitBytes == User::Component::FsCommitRequestBytes,
                   MessageCodec::Decode(ReadOnlyByteView{frame, commitBytes}, commit) == MessageCodecError::None,
                   commit.AttachmentCount == 1U, commit.MethodIdentifier == User::Component::FsCommitMethod,
                   commit.PayloadSizeBytes == 64U, PayloadWord(commit, 2U) == 2U);
    const UInt64 queryWords[User::Component::FsPayloadWords]{5U, 9U, 0U, 0U};
    const Size queryBytes = User::Component::WriteFsFrame(frame, User::Component::FsQueryCommitMethod, 9U, queryWords,
                                                          nullptr, 0U, &ReplyHandle, nullptr);
    MessageEnvelope query{};
    ASTERIA_EXPECT(runner, "a query decodes", queryBytes == User::Component::FsCommitRequestBytes,
                   MessageCodec::Decode(ReadOnlyByteView{frame, queryBytes}, query) == MessageCodecError::None,
                   query.AttachmentCount == 1U, query.MethodIdentifier == User::Component::FsQueryCommitMethod);

    const UInt64 openWords[User::Component::FsPayloadWords]{5U, 0U, 0U, 0U};
    const Size openBytes = User::Component::WriteFsFrame(frame, User::Component::FsOpenMethod, 10U, openWords, Hello,
                                                         5U, &ReplyHandle, nullptr);
    MessageEnvelope open{};
    const MessageCodecError openError = MessageCodec::Decode(ReadOnlyByteView{frame, openBytes}, open);
    ASTERIA_EXPECT(runner, "an open decodes with its name area", openBytes == User::Component::FsNameRequestBytes,
                   openError == MessageCodecError::None, open.AttachmentCount == 1U, open.PayloadSizeBytes == 128U,
                   PayloadWord(open, 0U) == 5U, PayloadWord(open, 1U) == 5U, open.Payload[64U] == 'h',
                   open.Payload[68U] == 'o', open.Payload[69U] == 0U, open.Payload[127U] == 0U);

    const UInt64 replyWords[User::Component::FsPayloadWords]{7U, User::Component::FsDone, 3U, 4U};
    const Size replyBytes = User::Component::WriteFsFrame(frame, User::Component::FsReplyMethod, 7U, replyWords,
                                                          nullptr, 0U, nullptr, nullptr);
    MessageEnvelope answered{};
    ASTERIA_EXPECT(runner, "a reply decodes with no attachment", replyBytes == User::Component::FsReplyBytes,
                   MessageCodec::Decode(ReadOnlyByteView{frame, replyBytes}, answered) == MessageCodecError::None,
                   answered.AttachmentCount == 0U, answered.MethodIdentifier == User::Component::FsReplyMethod,
                   PayloadWord(answered, 1U) == User::Component::FsDone, PayloadWord(answered, 3U) == 4U);
}

void TestConstants(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "credit, capacity and frame sizes follow the decision",
                   User::Component::FsMaximumOutstanding == 1U, User::Component::FsReplyCapacity == 2U,
                   User::Component::FsMaximumNameBytes == 64U, User::Component::FsMaximumFileBlocks == 6U,
                   User::Component::FsIoRequestBytes == 216U, User::Component::FsCommitRequestBytes == 184U,
                   User::Component::FsNameRequestBytes == 248U, User::Component::FsReplyBytes == 152U,
                   User::Component::FsMaximumFrameBytes == 248U, User::Component::FsDataRights == 0x0118U);
    ASTERIA_EXPECT(runner, "the interface differs from the block protocol's, the tags are distinct and nonzero",
                   (User::Component::FsInterfaceLow != 0x424C'4F43'4B49'4F01ULL) ||
                       (User::Component::FsInterfaceHigh != 0x4153'5445'5249'4102ULL),
                   User::Component::FsServiceWaitTag != 0U, User::Component::FsBlockReplyWaitTag != 0U,
                   User::Component::FsServiceWaitTag != User::Component::FsBlockReplyWaitTag,
                   User::Component::FsSealedSelector != 0U);
}

// The name rule matches the Object Directory's for every byte in every
// position class: alone, first, interior and last.
void TestNameRule(TestContext& runner) noexcept
{
    const UInt8 japanese[]{0xE6U, 0x97U, 0xA5U, 0xE6U, 0x9CU, 0xACU};
    const UInt8 overlong[]{0xC0U, 0xAFU};
    const UInt8 surrogate[]{0xEDU, 0xA0U, 0x80U};
    const bool same = User::Component::Detail::IsFsName(japanese, sizeof(japanese)) &&
                      !User::Component::Detail::IsFsName(overlong, sizeof(overlong)) &&
                      !User::Component::Detail::IsFsName(surrogate, sizeof(surrogate));
    UInt8 longName[65U]{};
    for (UInt8& byte : longName)
    {
        byte = 'a';
    }
    ASTERIA_EXPECT(runner, "UTF-8 single names reject malformed scalar encodings", same,
                   User::Component::Detail::IsFsName(longName, 64U), !User::Component::Detail::IsFsName(longName, 65U),
                   !User::Component::Detail::IsFsName(longName, 0U));
}

void TestRequestsRead(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    FsMessage message{};

    const UInt64 readWords[User::Component::FsPayloadWords]{3U, 100U, 17U, 5U};
    const Size readBytes = Build(delivery, User::Component::FsReadMethod, 9U, readWords, nullptr, 0U);
    ASTERIA_EXPECT(runner, "a read request reads back with both handles", Reads(delivery, readBytes, message),
                   message.Method == User::Component::FsReadMethod, message.Transaction == 9U, message.Words[0U] == 3U,
                   message.Words[1U] == 100U, message.Words[2U] == 17U, message.Words[3U] == 5U,
                   message.NameLength == 0U, message.Reply.Selector == 0x0123U,
                   message.Reply.Authenticator == 0xAAAA'BBBB'CCCC'DDDDULL, message.Data.Selector == 0x0456U,
                   message.Data.Authenticator == 0x1111'2222'3333'4444ULL);

    FsMessage write{};
    const UInt64 writeWords[User::Component::FsPayloadWords]{3U, 100U, 17U, 0U};
    const Size writeBytes = Build(delivery, User::Component::FsWriteMethod, 10U, writeWords, nullptr, 0U);
    ASTERIA_EXPECT(runner, "a write of block 0 reads back", Reads(delivery, writeBytes, write),
                   write.Method == User::Component::FsWriteMethod, write.Words[3U] == 0U,
                   write.Data.Selector == 0x0456U);

    FsMessage open{};
    const Size openBytes = BuildNamed(delivery, User::Component::FsOpenMethod, "hello");
    ASTERIA_EXPECT(runner, "an open reads back with its name", Reads(delivery, openBytes, open),
                   open.Method == User::Component::FsOpenMethod, open.Words[0U] == 5U, open.Words[1U] == 5U,
                   open.NameLength == 5U, open.Name[0U] == 'h', open.Name[4U] == 'o', open.Name[5U] == 0U,
                   open.Name[63U] == 0U, open.Reply.Selector == 0x0123U, open.Data.Selector == 0U);

    FsMessage create{};
    const char* const longest = "a123456789-123456789.123456789_123456789a123456789b123456789c123";
    const Size createBytes = BuildNamed(delivery, User::Component::FsCreateMethod, longest);
    ASTERIA_EXPECT(runner, "a create with a 64-byte name reads back", Reads(delivery, createBytes, create),
                   create.Method == User::Component::FsCreateMethod, create.NameLength == 64U, create.Name[0U] == 'a',
                   create.Name[63U] == '3');

    FsMessage commit{};
    const UInt64 flags[3U]{0U, User::Component::FsCrashBeforeSuperblock, User::Component::FsCrashAfterSuperblock};
    bool allFlags{true};
    for (Size index{0U}; index < 3U; ++index)
    {
        const UInt64 words[User::Component::FsPayloadWords]{3U, 40U + index, flags[index], 0U};
        const Size bytes = Build(delivery, User::Component::FsCommitMethod, 11U, words, nullptr, 0U);
        allFlags = allFlags && Reads(delivery, bytes, commit) && (commit.Words[2U] == flags[index]) &&
                   (commit.Words[1U] == (40U + index)) && (commit.Reply.Selector == 0x0123U);
    }
    ASTERIA_EXPECT(runner, "a commit with no flag or either crash flag reads back", allFlags);

    FsMessage query{};
    const UInt64 queryWords[User::Component::FsPayloadWords]{3U, 40U, 0U, 0U};
    const Size queryBytes = Build(delivery, User::Component::FsQueryCommitMethod, 12U, queryWords, nullptr, 0U);
    ASTERIA_EXPECT(runner, "a query reads back", Reads(delivery, queryBytes, query),
                   query.Method == User::Component::FsQueryCommitMethod, query.Words[1U] == 40U,
                   query.Reply.Selector == 0x0123U);
}

void TestRepliesRead(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    bool all{true};
    for (UInt64 kind{User::Component::FsDone}; kind <= User::Component::FsUnsupported; ++kind)
    {
        const UInt64 words[User::Component::FsPayloadWords]{20U + kind, kind, 4U + kind, 0x1'0000'0001ULL};
        const Size bytes = Build(delivery, User::Component::FsReplyMethod, 20U + kind, words, nullptr, 0U);
        FsMessage reply{};
        all = all && Reads(delivery, bytes, reply) && (reply.Method == User::Component::FsReplyMethod) &&
              (reply.Transaction == (20U + kind)) && (reply.Words[0U] == (20U + kind)) && (reply.Words[1U] == kind) &&
              (reply.Words[2U] == (4U + kind)) && (reply.Words[3U] == 0x1'0000'0001ULL) &&
              (reply.Reply.Selector == 0U) && (reply.Data.Selector == 0U) && (reply.NameLength == 0U);
    }
    ASTERIA_EXPECT(runner, "every reply kind reads back with no capability", all, User::Component::FsUnsupported == 10U,
                   User::Component::FsDone == 1U);

    // Stat names a file by its identity and carries the reply capability.
    const UInt64 stat[User::Component::FsPayloadWords]{3U, 7U, 9U, 0U};
    const Size statBytes = Build(delivery, User::Component::FsStatMethod, 41U, stat, nullptr, 0U);
    FsMessage asked{};
    const bool statRead = Reads(delivery, statBytes, asked);
    const UInt64 nonzero[User::Component::FsPayloadWords]{3U, 7U, 9U, 1U};
    const Size badBytes = Build(delivery, User::Component::FsStatMethod, 42U, nonzero, nullptr, 0U);
    FsMessage refused{};
    ASTERIA_EXPECT(runner, "a Stat reads back with its identity and a nonzero last word is refused", statRead,
                   asked.Method == User::Component::FsStatMethod, asked.Words[1U] == 7U, asked.Words[2U] == 9U,
                   asked.Reply.Selector != 0U, asked.Data.Selector == 0U, !Reads(delivery, badBytes, refused));
}

void TestRefusals(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    FsMessage message{};

    // Sizes: every method's frame one byte short or long, or cut.
    const UInt64 words[User::Component::FsPayloadWords]{3U, 100U, 17U, 1U};
    const UInt32 methods[8U]{User::Component::FsOpenMethod,   User::Component::FsCreateMethod,
                             User::Component::FsReadMethod,   User::Component::FsWriteMethod,
                             User::Component::FsCommitMethod, User::Component::FsQueryCommitMethod,
                             User::Component::FsReplyMethod,  User::Component::FsStatMethod};
    bool sizes{true};
    for (const UInt32 method : methods)
    {
        UInt64 own[User::Component::FsPayloadWords]{3U, 100U, 0U, 0U};
        own[1U] = (method == User::Component::FsReplyMethod) ? 4U : 5U;
        if ((method == User::Component::FsReadMethod) || (method == User::Component::FsWriteMethod))
        {
            own[3U] = 1U;
        }
        const Size bytes = Build(delivery, method, 1U, own, Hello, 5U);
        sizes = sizes && (bytes != 0U) && Reads(delivery, bytes, message) && !Reads(delivery, bytes - 1U, message) &&
                !Reads(delivery, bytes + 1U, message) && !Reads(delivery, Header + 8U, message);
    }
    ASTERIA_EXPECT(runner, "a frame one byte short or long, or cut, is refused for every method", sizes);

    const Size readBytes = Build(delivery, User::Component::FsReadMethod, 9U, words, nullptr, 0U);
    delivery[Header + 16U] ^= 1U;
    ASTERIA_EXPECT(runner, "a foreign interface is refused", !Reads(delivery, readBytes, message));
    delivery[Header + 16U] ^= 1U;
    delivery[Header + 25U] ^= 1U;
    ASTERIA_EXPECT(runner, "a foreign interface's high word is refused", !Reads(delivery, readBytes, message));
    delivery[Header + 25U] ^= 1U;

    // Attachment count: a read with one, a commit with two, a reply with one.
    const Size oneBytes = Build(delivery, User::Component::FsCommitMethod, 9U, words, nullptr, 0U);
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::FsReadMethod);
    ASTERIA_EXPECT(runner, "a read with one attachment is refused", !Reads(delivery, oneBytes, message));
    const Size twoBytes = Build(delivery, User::Component::FsReadMethod, 9U, words, nullptr, 0U);
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::FsCommitMethod);
    ASTERIA_EXPECT(runner, "a commit with two attachments is refused", !Reads(delivery, twoBytes, message));
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::FsReplyMethod);
    ASTERIA_EXPECT(runner, "a reply with two attachments is refused", !Reads(delivery, twoBytes, message));

    // Types: the data attachment no Frame, the reply attachment no Endpoint.
    const Size typedBytes = Build(delivery, User::Component::FsReadMethod, 9U, words, nullptr, 0U);
    const Size dataType = Header + User::Component::EchoHeaderBytes + User::Component::EchoAttachmentBytes + 27U;
    delivery[dataType] = User::Component::EchoEndpointType;
    const bool wrongData = Reads(delivery, typedBytes, message);
    delivery[dataType] = User::Component::EchoFrameType;
    delivery[Header + User::Component::EchoHeaderBytes + 27U] = User::Component::EchoFrameType;
    const bool wrongReply = Reads(delivery, typedBytes, message);
    ASTERIA_EXPECT(runner, "attachments of the wrong object type are refused", !wrongData, !wrongReply);
    const Size openBytes = BuildNamed(delivery, User::Component::FsOpenMethod, "hello");
    delivery[Header + User::Component::EchoHeaderBytes + 27U] = User::Component::EchoFrameType;
    ASTERIA_EXPECT(runner, "an open whose reply attachment is no Endpoint is refused",
                   !Reads(delivery, openBytes, message));

    // An unknown method, and a method word with a high byte.
    const Size unknownBytes = Build(delivery, User::Component::FsReadMethod, 9U, words, nullptr, 0U);
    delivery[Header + 32U] = 0U;
    const bool zero = Reads(delivery, unknownBytes, message);
    delivery[Header + 32U] = 8U;
    const bool eight = Reads(delivery, unknownBytes, message);
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::FsReadMethod);
    delivery[Header + 34U] = 1U;
    const bool wide = Reads(delivery, unknownBytes, message);
    ASTERIA_EXPECT(runner, "methods 0 and 8 and a wide method word are refused", !zero, !eight, !wide);

    // Block index.
    const UInt64 sixth[User::Component::FsPayloadWords]{3U, 100U, 17U, 6U};
    const UInt64 fifth[User::Component::FsPayloadWords]{3U, 100U, 17U, 5U};
    ASTERIA_EXPECT(runner, "block 6 is refused and block 5 is not",
                   Reads(delivery, Build(delivery, User::Component::FsWriteMethod, 1U, sixth, nullptr, 0U), message),
                   Reads(delivery, Build(delivery, User::Component::FsWriteMethod, 1U, fifth, nullptr, 0U), message));
    const UInt64 huge[User::Component::FsPayloadWords]{3U, 100U, 17U, 0x1'0000'0000ULL};
    ASTERIA_EXPECT(runner, "a block index above 32 bits is refused",
                   !Reads(delivery, Build(delivery, User::Component::FsReadMethod, 1U, huge, nullptr, 0U), message));

    // Commit flags and the reserved word.
    const UInt64 bothFlags[User::Component::FsPayloadWords]{3U, 40U, 3U, 0U};
    const UInt64 unknownFlag[User::Component::FsPayloadWords]{3U, 40U, 4U, 0U};
    const UInt64 highFlag[User::Component::FsPayloadWords]{3U, 40U, 0x8000'0000'0000'0001ULL, 0U};
    const bool flagsRefused =
        !Reads(delivery, Build(delivery, User::Component::FsCommitMethod, 1U, bothFlags, nullptr, 0U), message) &&
        !Reads(delivery, Build(delivery, User::Component::FsCommitMethod, 1U, unknownFlag, nullptr, 0U), message) &&
        !Reads(delivery, Build(delivery, User::Component::FsCommitMethod, 1U, highFlag, nullptr, 0U), message);
    const UInt64 reserved[User::Component::FsPayloadWords]{3U, 40U, 0U, 1U};
    ASTERIA_EXPECT(
        runner, "both crash flags, an unknown flag and a reserved word are refused", flagsRefused,
        !Reads(delivery, Build(delivery, User::Component::FsCommitMethod, 1U, reserved, nullptr, 0U), message),
        !Reads(delivery, Build(delivery, User::Component::FsQueryCommitMethod, 1U, reserved, nullptr, 0U), message));
    const UInt64 queryFlag[User::Component::FsPayloadWords]{3U, 40U, 1U, 0U};
    ASTERIA_EXPECT(
        runner, "a query with a flag is refused",
        !Reads(delivery, Build(delivery, User::Component::FsQueryCommitMethod, 1U, queryFlag, nullptr, 0U), message));

    // Reply kinds.
    const UInt64 kindZero[User::Component::FsPayloadWords]{1U, 0U, 0U, 0U};
    const UInt64 kindEleven[User::Component::FsPayloadWords]{1U, 12U, 0U, 0U};
    ASTERIA_EXPECT(
        runner, "reply kinds 0 and 11 are refused",
        !Reads(delivery, Build(delivery, User::Component::FsReplyMethod, 1U, kindZero, nullptr, 0U), message),
        !Reads(delivery, Build(delivery, User::Component::FsReplyMethod, 1U, kindEleven, nullptr, 0U), message));
}

void TestNameRefusals(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    FsMessage message{};
    const Size nameAt = Header + User::Component::EchoHeaderBytes + User::Component::EchoAttachmentBytes +
                        User::Component::FsPayloadBytes;
    const Size lengthAt = Header + User::Component::EchoHeaderBytes + User::Component::EchoAttachmentBytes + 8U;

    ASTERIA_EXPECT(runner, "names with an uppercase letter, a space, a leading or trailing punctuation are refused",
                   !Reads(delivery, BuildNamed(delivery, User::Component::FsOpenMethod, "Hello"), message),
                   !Reads(delivery, BuildNamed(delivery, User::Component::FsOpenMethod, "he llo"), message),
                   !Reads(delivery, BuildNamed(delivery, User::Component::FsCreateMethod, ".hello"), message),
                   !Reads(delivery, BuildNamed(delivery, User::Component::FsCreateMethod, "hello-"), message),
                   !Reads(delivery, BuildNamed(delivery, User::Component::FsCreateMethod, "a/b"), message),
                   Reads(delivery, BuildNamed(delivery, User::Component::FsCreateMethod, "a.b-c_d"), message),
                   Reads(delivery, BuildNamed(delivery, User::Component::FsCreateMethod, "x"), message));

    // A name length of zero or 65 (the writer refuses to build them, so patch
    // a valid frame), a name with a byte after its length, a reserved word.
    const Size validBytes = BuildNamed(delivery, User::Component::FsOpenMethod, "hello");
    delivery[lengthAt] = 0U;
    const bool zeroLength = Reads(delivery, validBytes, message);
    delivery[lengthAt] = 65U;
    const bool longLength = Reads(delivery, validBytes, message);
    delivery[lengthAt] = 5U;
    delivery[lengthAt + 1U] = 1U;
    const bool wideLength = Reads(delivery, validBytes, message);
    delivery[lengthAt + 1U] = 0U;
    delivery[nameAt + 5U] = 'x';
    const bool padded = Reads(delivery, validBytes, message);
    delivery[nameAt + 5U] = 0U;
    delivery[nameAt + 63U] = 'x';
    const bool tail = Reads(delivery, validBytes, message);
    delivery[nameAt + 63U] = 0U;
    delivery[lengthAt + 8U] = 1U;
    const bool reserved = Reads(delivery, validBytes, message);
    delivery[lengthAt + 8U] = 0U;
    delivery[lengthAt + 16U] = 1U;
    const bool reservedLast = Reads(delivery, validBytes, message);
    delivery[lengthAt + 16U] = 0U;
    ASTERIA_EXPECT(runner, "a name length of 0, 65 or beyond 8 bits, nonzero padding or a reserved word is refused",
                   !zeroLength, !longLength, !wideLength, !padded, !tail, !reserved, !reservedLast,
                   Reads(delivery, validBytes, message));

    // An open frame the size of a commit's, and the other way round: the
    // payload size fields must match the method.
    const Size commitBytes = Build(delivery, User::Component::FsCommitMethod, 1U, {3U, 40U, 0U, 0U}, nullptr, 0U);
    delivery[Header + 32U] = static_cast<UInt8>(User::Component::FsOpenMethod);
    ASTERIA_EXPECT(runner, "a commit's frame under the open method is refused", !Reads(delivery, commitBytes, message));
}

// The writer's own refusals, and that a failed read leaves the message alone.
void TestQuiesce(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes]{};
    FsMessage message{};
    const UInt64 words[User::Component::FsPayloadWords]{5U};
    const auto bytes = Build(delivery, User::Component::FsQuiesceMethod, 8U, words, nullptr, 0U);
    ASTERIA_EXPECT(runner, "quiesce carries the v2 incarnation with one reply capability",
                   Reads(delivery, bytes, message), message.Method == User::Component::FsQuiesceMethod,
                   message.Words[0U] == 5U, message.Reply.Selector == ReplyHandle.Selector);
    constexpr Size payload{Header + User::Component::EchoHeaderBytes + User::Component::EchoAttachmentBytes};
    for (Size word{1U}; word < User::Component::FsPayloadWords; ++word)
    {
        delivery[payload + word * 8U] = 1U;
        ASTERIA_EXPECT(runner, "every unused v2 quiesce word must be zero", !Reads(delivery, bytes, message));
        delivery[payload + word * 8U] = 0U;
    }
}

void TestWriter(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes];
    UInt8 scratch[User::Component::FsMaximumFrameBytes]{};
    const UInt64 words[User::Component::FsPayloadWords]{};
    ASTERIA_EXPECT(runner, "an unknown method, a missing attachment or a bad name length writes nothing",
                   User::Component::WriteFsFrame(scratch, 0U, 1U, words, nullptr, 0U, &ReplyHandle, &DataHandle) == 0U,
                   User::Component::WriteFsFrame(scratch, User::Component::FsPassFileMethod, 1U, words, nullptr, 0U,
                                                 &ReplyHandle, &DataHandle) == 0U,
                   User::Component::WriteFsFrame(scratch, User::Component::FsQuiesceMethod + 1U, 1U, words, nullptr, 0U,
                                                 &ReplyHandle, &DataHandle) == 0U,
                   User::Component::WriteFsFrame(scratch, User::Component::FsReadMethod, 1U, words, nullptr, 0U,
                                                 &ReplyHandle, nullptr) == 0U,
                   User::Component::WriteFsFrame(scratch, User::Component::FsCommitMethod, 1U, words, nullptr, 0U,
                                                 nullptr, nullptr) == 0U,
                   User::Component::WriteFsFrame(scratch, User::Component::FsOpenMethod, 1U, words, nullptr, 5U,
                                                 &ReplyHandle, nullptr) == 0U,
                   User::Component::WriteFsFrame(scratch, User::Component::FsOpenMethod, 1U, words, Hello, 0U,
                                                 &ReplyHandle, nullptr) == 0U,
                   User::Component::WriteFsFrame(scratch, User::Component::FsOpenMethod, 1U, words, Hello, 65U,
                                                 &ReplyHandle, nullptr) == 0U,
                   scratch[0U] == 0U);

    FsMessage message{};
    message.Method = 99U;
    message.Words[2U] = 77U;
    const UInt64 bad[User::Component::FsPayloadWords]{1U, 4U, 0U, 0x20000000U};
    const Size bytes = Build(delivery, User::Component::FsReadMethod, 3U, bad, nullptr, 0U);
    ASTERIA_EXPECT(runner, "a refused frame leaves the message unchanged", !Reads(delivery, bytes, message),
                   message.Method == 99U, message.Words[2U] == 77U);
}
void TestReadPages(TestContext& runner) noexcept
{
    static UInt8 delivery[DeliveryBytes]{};
    UInt64 words[User::Component::FsPayloadWords]{1U, 1U, 2U, 0U, 1U, 0U, 0U, User::Component::FsReadPagesCapacity};
    FsMessage message{};
    const auto build = [&]() noexcept
    {
        return Header + User::Component::WriteFsFrame(delivery + Header, User::Component::FsReadPagesMethod, 1U, words,
                                                      nullptr, 0U, &ReplyHandle, &DataHandle);
    };
    ASTERIA_EXPECT(runner, "ReadPages accepts a bounded multi-page Frame", Reads(delivery, build(), message),
                   message.Words[7U] == User::Component::FsReadPagesCapacity);
    words[7U] = 0U;
    ASTERIA_EXPECT(runner, "zero page count is refused", !Reads(delivery, build(), message));
    words[7U] = User::Component::FsReadPagesCapacity + 1U;
    ASTERIA_EXPECT(runner, "oversized page count is refused", !Reads(delivery, build(), message));
    words[7U] = 2U;
    words[3U] = 0x1FFF'FFFFU;
    ASTERIA_EXPECT(runner, "page ranges cannot overflow the bounded index", !Reads(delivery, build(), message));
}

void TestUnusedRequestWords(TestContext& runner) noexcept
{
    namespace C = User::Component;
    struct Schema
    {
        UInt32 Method;
        UInt8 Used;
    };
    // Independent protocol-v2 table from Architecture/FilesystemProtocol.md.
    constexpr Schema schemas[]{
        {C::FsOpenMethod, 0x73U},        {C::FsCreateMethod, 0x73U},      {C::FsMakeDirectoryMethod, 0x73U},
        {C::FsReadMethod, 0x9FU},        {C::FsWriteMethod, 0x1FU},       {C::FsCommitMethod, 0x77U},
        {C::FsQueryCommitMethod, 0x73U}, {C::FsStatMethod, 0x17U},        {C::FsReadDirectoryMethod, 0x1FU},
        {C::FsCloseMethod, 0x17U},       {C::FsInstallViewMethod, 0x3FU}, {C::FsRemoveViewMethod, 0x11U},
        {C::FsResetViewsMethod, 0x01U},  {C::FsQuiesceMethod, 0x01U}};
    static UInt8 delivery[DeliveryBytes]{};
    FsMessage message{};
    for (const auto schema : schemas)
    {
        const UInt64 words[C::FsPayloadWords]{1U, 1U, 0U, 0U, 1U, 0U, 0U, schema.Method == C::FsReadMethod ? 1U : 0U};
        const auto bytes = Header + C::WriteFsFrame(delivery + Header, schema.Method, 1U, words, Hello, sizeof(Hello),
                                                    &ReplyHandle, &DataHandle);
        auto* payload = delivery + Header + C::EchoHeaderBytes +
                        C::Detail::CountFsAttachments(schema.Method) * C::EchoAttachmentBytes;
        // Establish a canonical baseline for each method before mutating it.
        for (Size word{0U}; word < C::FsPayloadWords; ++word)
        {
            if ((schema.Used & (1U << word)) == 0U)
            {
                for (Size b{0U}; b < 8U; ++b)
                {
                    payload[word * 8U + b] = 0U;
                }
            }
        }
        ASTERIA_EXPECT(runner, "UnusedRequestWords: canonical request accepted", Reads(delivery, bytes, message));
        for (Size word{0U}; word < C::FsPayloadWords; ++word)
        {
            if ((schema.Used & (1U << word)) != 0U)
            {
                continue;
            }
            for (Size b{0U}; b < 8U; ++b)
            {
                payload[word * 8U + b] = 0x80U;
                message.Method = 99U;
                ASTERIA_EXPECT(runner, "UnusedRequestWords: every unused byte is rejected without output mutation",
                               !Reads(delivery, bytes, message), message.Method == 99U);
                payload[word * 8U + b] = 0U;
            }
        }
    }
}

} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"FsFrameTests"};
    Asteria::Test::TestFramesDecode(runner);
    Asteria::Test::TestConstants(runner);
    Asteria::Test::TestNameRule(runner);
    Asteria::Test::TestRequestsRead(runner);
    Asteria::Test::TestRepliesRead(runner);
    Asteria::Test::TestRefusals(runner);
    Asteria::Test::TestNameRefusals(runner);
    Asteria::Test::TestWriter(runner);
    Asteria::Test::TestQuiesce(runner);
    Asteria::Test::TestReadPages(runner);
    Asteria::Test::TestUnusedRequestWords(runner);
    return runner.Finish();
}
