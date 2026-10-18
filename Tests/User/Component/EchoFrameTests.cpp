// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Component/EchoFrame.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt8;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::ObjectType;
using User::Component::EchoAttachment;

// The frames the root service and the echo component build by hand are canonical frames the
// kernel's codec accepts, with the attachments they name.
void TestRequestAndReplyDecode(TestContext& runner) noexcept
{
    const EchoAttachment attachments[2U]{
        {.Handle = {0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL},
         .Rights = User::Component::EchoInvokeRight,
         .Mode = User::Component::EchoCopy,
         .Type = User::Component::EchoEndpointType},
        {.Handle = {0x0456U, 0x1111'2222'3333'4444ULL},
         .Rights = User::Component::EchoFrameRights,
         .Mode = User::Component::EchoMove,
         .Type = User::Component::EchoFrameType},
    };
    UInt8 request[User::Component::EchoRequestBytes]{};
    User::Component::WriteEchoFrame(request, attachments, 2U);
    MessageEnvelope decoded{};
    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{request, sizeof(request)}, decoded);
    const CapabilityRights frameRights{
        CapabilityRights{CapabilityRight::Transfer}.With(CapabilityRight::Manage).With(CapabilityRight::Inspect)};
    ASTERIA_EXPECT(runner, "the echo request decodes with a copied reply Endpoint and a moved Frame",
                   error == MessageCodecError::None, decoded.AttachmentCount == 2U,
                   decoded.Attachments[0U].Handle.GetSelector() == 0x0123U,
                   decoded.Attachments[0U].Handle.GetAuthenticator() == 0xAAAA'BBBB'CCCC'DDDDULL,
                   decoded.Attachments[0U].TransferMode == CapabilityTransferMode::Copy,
                   decoded.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   decoded.Attachments[0U].RequestedRights.IsEqualTo(CapabilityRights{CapabilityRight::Invoke}),
                   decoded.Attachments[0U].RequestedDeadline.IsInfinite(),
                   decoded.Attachments[1U].TransferMode == CapabilityTransferMode::Move,
                   decoded.Attachments[1U].ExpectedType == ObjectType::Frame,
                   decoded.Attachments[1U].RequestedRights.IsEqualTo(frameRights));

    UInt8 reply[User::Component::EchoReplyBytes]{};
    User::Component::WriteEchoFrame(reply, attachments + 1U, 1U);
    MessageEnvelope answered{};
    const MessageCodecError replied = MessageCodec::Decode(ReadOnlyByteView{reply, sizeof(reply)}, answered);
    ASTERIA_EXPECT(runner, "the echo reply decodes with the moved Frame", replied == MessageCodecError::None,
                   answered.AttachmentCount == 1U, answered.Attachments[0U].Handle.GetSelector() == 0x0456U,
                   answered.Attachments[0U].TransferMode == CapabilityTransferMode::Move);

    // A delivery: the 48-byte header, then the frame as the kernel patched it.
    UInt8 delivery[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoRequestBytes]{};
    for (Foundation::Size index{0U}; index < sizeof(request); ++index)
    {
        delivery[User::Component::EchoDeliveryHeaderBytes + index] = request[index];
    }
    const auto handle = User::Component::ReadEchoHandle(delivery, 1U);
    ASTERIA_EXPECT(runner, "a delivery's attachments read back at their documented offsets",
                   User::Component::GetEchoAttachmentCount(delivery) == 2U, handle.Selector == 0x0456U,
                   handle.Authenticator == 0x1111'2222'3333'4444ULL,
                   User::Component::ReadEchoType(delivery, 0U) == User::Component::EchoEndpointType,
                   User::Component::ReadEchoType(delivery, 1U) == User::Component::EchoFrameType);
}

// The echo answers a request with itself: header, payload and transaction
// unchanged, the reply attachment dropped and any other moved back.
void TestEchoReply(TestContext& runner) noexcept
{
    MessageEnvelope request{};
    request.Interface = Kernel::Ipc::InterfaceIdentifier{0x0123U, 0x4567U};
    request.TransactionIdentifier = 77U;
    request.Deadline = Kernel::Object::MonotonicTime::Infinite();
    request.MethodIdentifier = 3U;
    request.InterfaceVersionMajor = 1U;
    request.Kind = Kernel::Ipc::MessageKind::Asynchronous;
    request.Cancellation = Kernel::Ipc::CancellationState::None;
    request.FixedPayloadSizeBytes = 8U;
    request.PayloadSizeBytes = 8U;
    request.FixedPayloadSizeBytes = 0U;
    request.VariablePayloadCount = 1U;
    request.VariablePayloads[0U] = {0U, 8U, 0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        request.Payload[index] = static_cast<UInt8>(0xA0U + index);
    }
    request.AttachmentCount = 2U;
    request.Attachments[0U] =
        Kernel::Ipc::CapabilityAttachment{.Handle = {0x0111U, 0x2222U},
                                          .RequestedDeadline = Kernel::Object::MonotonicTime::Infinite(),
                                          .RequestedRights = CapabilityRights{CapabilityRight::Invoke},
                                          .ExpectedType = ObjectType::Endpoint,
                                          .TransferMode = CapabilityTransferMode::Copy,
                                          .Reserved = 0U};
    request.Attachments[1U] = Kernel::Ipc::CapabilityAttachment{
        .Handle = {0x0333U, 0x4444U},
        .RequestedDeadline = Kernel::Object::MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Transfer}.With(CapabilityRight::Inspect),
        .ExpectedType = ObjectType::Frame,
        .TransferMode = CapabilityTransferMode::Move,
        .Reserved = 0U};
    UInt8 delivery[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoMaximumFrameBytes]{};
    const auto encoded =
        MessageCodec::Encode(request, Kernel::Ipc::WritableByteView{delivery + User::Component::EchoDeliveryHeaderBytes,
                                                                    User::Component::EchoMaximumFrameBytes});
    UInt8 reply[User::Component::EchoMaximumFrameBytes]{};
    const Foundation::Size replied =
        encoded.HasValue() ? User::Component::WriteEchoReply(
                                 delivery, User::Component::EchoDeliveryHeaderBytes + encoded.GetValue(), reply)
                           : 0U;
    MessageEnvelope answer{};
    const MessageCodecError error = MessageCodec::Decode(ReadOnlyByteView{reply, replied}, answer);
    const auto replyHandle = User::Component::ReadEchoHandle(delivery, 0U);
    const auto frameHandle = User::Component::ReadEchoHandle(delivery, 1U);
    ASTERIA_EXPECT(runner, "descriptor-bearing deliveries read the real handles and types", encoded.HasValue(),
                   replyHandle.Selector == 0x0111U, replyHandle.Authenticator == 0x2222U,
                   frameHandle.Selector == 0x0333U, frameHandle.Authenticator == 0x4444U,
                   User::Component::ReadEchoType(delivery, 0U) == User::Component::EchoEndpointType,
                   User::Component::ReadEchoType(delivery, 1U) == User::Component::EchoFrameType);
    ASTERIA_EXPECT(runner,
                   "the echo answers with the request's payload and transaction, the other capability moved back",
                   encoded.HasValue(), replied == (encoded.HasValue() ? encoded.GetValue() - 32U : 0U),
                   error == MessageCodecError::None, answer.TransactionIdentifier == 77U, answer.PayloadSizeBytes == 8U,
                   answer.Payload[7U] == 0xA7U, answer.AttachmentCount == 1U,
                   answer.Attachments[0U].Handle.GetSelector() == 0x0333U,
                   answer.Attachments[0U].TransferMode == CapabilityTransferMode::Move,
                   answer.Attachments[0U].ExpectedType == ObjectType::Frame);

    UInt8 bad[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoMaximumFrameBytes]{};
    ASTERIA_EXPECT(runner, "a delivery that is no frame with a reply attachment gets no answer",
                   User::Component::WriteEchoReply(bad, 40U, reply) == 0U,
                   User::Component::WriteEchoReply(bad, sizeof(bad), reply) == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"EchoFrameTests"};
    Asteria::Test::TestRequestAndReplyDecode(runner);
    Asteria::Test::TestEchoReply(runner);
    return runner.Finish();
}
