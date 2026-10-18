// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Component/DriverFrame.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Object::ObjectType;
using User::Component::DriverMessage;

[[nodiscard]] UInt64 PayloadWord(const MessageEnvelope& envelope, const Size word) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(envelope.Payload[(word * 8U) + index]) << (index * 8U);
    }
    return value;
}

// Copies frame after a 48-byte delivery header, as the kernel delivers it.
template <Size Bytes> void Deliver(const UInt8 (&frame)[Bytes], UInt8* const delivery) noexcept
{
    for (Size index{0U}; index < Bytes; ++index)
    {
        delivery[User::Component::EchoDeliveryHeaderBytes + index] = frame[index];
    }
}

// The driver service's frames are canonical frames the kernel's codec accepts: a request with a
// copied reply Endpoint and its four payload words, a reply with none and its own four.
void TestFramesDecode(TestContext& runner) noexcept
{
    const User::Component::EchoAttachment reply{.Handle = {0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL},
                                                .Rights = User::Component::EchoInvokeRight,
                                                .Mode = User::Component::EchoCopy,
                                                .Type = User::Component::EchoEndpointType};
    UInt8 request[User::Component::DriverRequestBytes]{};
    const Size requestBytes = User::Component::WriteDriverFrame(request, 7U, {11U, 0x5A5AU, 0U, 0U}, &reply);
    MessageEnvelope decoded{};
    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{request, sizeof(request)}, decoded);
    ASTERIA_EXPECT(runner, "a request decodes with its reply Endpoint, method, transaction and payload",
                   requestBytes == sizeof(request), error == MessageCodecError::None,
                   decoded.Interface.GetLow() == User::Component::DriverInterfaceLow,
                   decoded.Interface.GetHigh() == User::Component::DriverInterfaceHigh,
                   decoded.MethodIdentifier == User::Component::DriverCopyMethod, decoded.TransactionIdentifier == 7U,
                   decoded.AttachmentCount == 1U, decoded.Attachments[0U].Handle.GetSelector() == 0x0123U,
                   decoded.Attachments[0U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   decoded.PayloadSizeBytes == User::Component::DriverPayloadBytes, PayloadWord(decoded, 0U) == 11U,
                   PayloadWord(decoded, 1U) == 0x5A5AU);

    UInt8 answer[User::Component::DriverReplyBytes]{};
    const Size answerBytes = User::Component::WriteDriverFrame(
        answer, 7U, {11U, 0x5A5AU, User::Component::DriverRestarted, 0x100U}, nullptr);
    MessageEnvelope answered{};
    const MessageCodecError replied = MessageCodec::Decode(ReadOnlyByteView{answer, sizeof(answer)}, answered);
    ASTERIA_EXPECT(runner, "a reply decodes with no attachment and its kind and found status",
                   answerBytes == sizeof(answer), replied == MessageCodecError::None, answered.AttachmentCount == 0U,
                   answered.MethodIdentifier == User::Component::DriverReplyMethod,
                   answered.TransactionIdentifier == 7U, PayloadWord(answered, 2U) == User::Component::DriverRestarted,
                   PayloadWord(answered, 3U) == 0x100U);
}

// Deliveries read back; anything else is refused.
void TestDeliveriesRead(TestContext& runner) noexcept
{
    const User::Component::EchoAttachment reply{.Handle = {0x0456U, 0x1111'2222'3333'4444ULL},
                                                .Rights = User::Component::EchoInvokeRight,
                                                .Mode = User::Component::EchoCopy,
                                                .Type = User::Component::EchoEndpointType};
    UInt8 request[User::Component::DriverRequestBytes]{};
    static_cast<void>(User::Component::WriteDriverFrame(request, 9U, {12U, 0x77U, 0U, 0U}, &reply));
    UInt8 delivery[User::Component::EchoDeliveryHeaderBytes + User::Component::DriverRequestBytes]{};
    Deliver(request, delivery);
    DriverMessage message{};
    const bool read = User::Component::ReadDriverFrame(delivery, sizeof(delivery), message);
    DriverMessage shorter{};
    const bool truncated = User::Component::ReadDriverFrame(delivery, sizeof(delivery) - 1U, shorter);
    delivery[User::Component::EchoDeliveryHeaderBytes + 16U] ^= 1U;
    DriverMessage foreign{};
    const bool otherInterface = User::Component::ReadDriverFrame(delivery, sizeof(delivery), foreign);
    ASTERIA_EXPECT(runner, "a request's delivery reads back with its reply capability", read,
                   message.Method == User::Component::DriverCopyMethod, message.Transaction == 9U,
                   message.Words[0U] == 12U, message.Words[1U] == 0x77U, message.Reply.Selector == 0x0456U,
                   message.Reply.Authenticator == 0x1111'2222'3333'4444ULL, !truncated, !otherInterface);

    UInt8 answer[User::Component::DriverReplyBytes]{};
    static_cast<void>(
        User::Component::WriteDriverFrame(answer, 9U, {12U, 0x77U, User::Component::DriverDone, 0U}, nullptr));
    UInt8 answered[User::Component::EchoDeliveryHeaderBytes + User::Component::DriverReplyBytes]{};
    Deliver(answer, answered);
    DriverMessage done{};
    ASTERIA_EXPECT(runner, "a reply's delivery reads back with no capability",
                   User::Component::ReadDriverFrame(answered, sizeof(answered), done),
                   done.Method == User::Component::DriverReplyMethod, done.Words[2U] == User::Component::DriverDone,
                   done.Reply.Selector == 0U);

    // A factorial request reads back as one; another method
    // with an attachment is refused.
    UInt8 factorial[User::Component::DriverRequestBytes]{};
    static_cast<void>(User::Component::WriteDriverFrame(factorial, 10U, {12U, 10U, 0U, 0U}, &reply,
                                                        User::Component::DriverFactorialMethod));
    UInt8 asked[User::Component::EchoDeliveryHeaderBytes + User::Component::DriverRequestBytes]{};
    Deliver(factorial, asked);
    DriverMessage computed{};
    const bool readFactorial = User::Component::ReadDriverFrame(asked, sizeof(asked), computed);
    UInt8 unknown[User::Component::DriverRequestBytes]{};
    static_cast<void>(User::Component::WriteDriverFrame(unknown, 11U, {12U, 10U, 0U, 0U}, &reply, 4U));
    UInt8 refused[User::Component::EchoDeliveryHeaderBytes + User::Component::DriverRequestBytes]{};
    Deliver(unknown, refused);
    DriverMessage other{};
    ASTERIA_EXPECT(runner, "a factorial request reads back; a request of another method does not", readFactorial,
                   computed.Method == User::Component::DriverFactorialMethod, computed.Words[1U] == 10U,
                   computed.Reply.Selector == 0x0456U,
                   !User::Component::ReadDriverFrame(refused, sizeof(refused), other));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DriverFrameTests"};
    Asteria::Test::TestFramesDecode(runner);
    Asteria::Test::TestDeliveriesRead(runner);
    return runner.Finish();
}
