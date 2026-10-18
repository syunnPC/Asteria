// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Executive::ObjectDirectory::BindIfAbsentRequest;
using Executive::ObjectDirectory::BindingVersion;
using Executive::ObjectDirectory::DirectoryMessageContext;
using Executive::ObjectDirectory::DirectoryName;
using Executive::ObjectDirectory::DirectoryNameView;
using Executive::ObjectDirectory::DirectoryStatus;
using Executive::ObjectDirectory::InterfaceVersionRange;
using Executive::ObjectDirectory::MutationReply;
using Executive::ObjectDirectory::ObjectDirectoryProtocol;
using Executive::ObjectDirectory::ObjectDirectoryProtocolError;
using Executive::ObjectDirectory::RemoveIfVersionRequest;
using Executive::ObjectDirectory::ReplaceIfVersionRequest;
using Executive::ObjectDirectory::ResolveReply;
using Executive::ObjectDirectory::ResolveRequest;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt64;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Ipc::WritableByteView;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectType;

[[nodiscard]] bool AreBytesEqual(const void* const left, const void* const right, const Size sizeBytes) noexcept
{
    const auto* const leftBytes = static_cast<const UInt8*>(left);
    const auto* const rightBytes = static_cast<const UInt8*>(right);
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        if (leftBytes[index] != rightBytes[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] UInt16 ReadLittleEndian16(const UInt8* const bytes) noexcept
{
    return static_cast<UInt16>(static_cast<UInt16>(bytes[0U]) | (static_cast<UInt16>(bytes[1U]) << 8U));
}

[[nodiscard]] UInt64 ReadLittleEndian64(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] DirectoryName MakeName(const UInt8* const bytes, const Size sizeBytes) noexcept
{
    const auto result = DirectoryName::Create(DirectoryNameView{
        .Data = bytes,
        .SizeBytes = sizeBytes,
    });
    return result.GetValue();
}

[[nodiscard]] DirectoryName MakeStandardName() noexcept
{
    constexpr UInt8 bytes[]{
        static_cast<UInt8>('s'), static_cast<UInt8>('v'), static_cast<UInt8>('c'), static_cast<UInt8>('.'),
        static_cast<UInt8>('o'), static_cast<UInt8>('n'), static_cast<UInt8>('e'),
    };
    return MakeName(bytes, sizeof(bytes));
}

[[nodiscard]] DirectoryMessageContext MakeContext() noexcept
{
    return DirectoryMessageContext{
        .Trace =
            {
                .TraceIdentifierHigh = 0x1122'3344'5566'7788ULL,
                .TraceIdentifierLow = 0x99AA'BBCC'DDEE'FF00ULL,
                .SpanIdentifier = 0x8877'6655'4433'2211ULL,
            },
        .Deadline = MonotonicTime{0x1020'3040'5060'7080ULL},
        .TransactionIdentifier = 0x8070'6050'4030'2010ULL,
    };
}

[[nodiscard]] CapabilityRights PublishedRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read).With(CapabilityRight::Write);
}

[[nodiscard]] ResolveRequest MakeResolveRequest() noexcept
{
    return ResolveRequest{
        .Name = MakeStandardName(),
        .ServiceInterface = InterfaceIdentifier{0x0102'0304'0506'0708ULL, 0x1112'1314'1516'1718ULL},
        .RequestedDeadline = MonotonicTime{0x2122'2324'2526'2728ULL},
        .RequiredVersion =
            {
                .Major = 0x3132U,
                .MinimumMinor = 0x4142U,
                .MaximumMinor = 0x5152U,
                .Reserved = 0U,
            },
        .RequiredRights = PublishedRights(),
        .ReplyEndpoint = CapabilityHandle{0x6162'6364'6566'6768ULL, 0x7172'7374'7576'7778ULL},
        .Context = MakeContext(),
    };
}

[[nodiscard]] BindIfAbsentRequest MakeBindRequest() noexcept
{
    return BindIfAbsentRequest{
        .Name = MakeStandardName(),
        .ServiceInterface = InterfaceIdentifier{0x0102'0304'0506'0708ULL, 0x1112'1314'1516'1718ULL},
        .ServiceGeneration = 0x2122'2324'2526'2728ULL,
        .Version =
            {
                .Major = 3U,
                .MinimumMinor = 4U,
                .MaximumMinor = 9U,
                .Reserved = 0U,
            },
        .PublishedRights = PublishedRights(),
        .TargetEndpoint = CapabilityHandle{0x3132'3334'3536'3738ULL, 0x4142'4344'4546'4748ULL},
        .TargetDeadline = MonotonicTime{0x5152'5354'5556'5758ULL},
        .ReplyEndpoint = CapabilityHandle{0x6162'6364'6566'6768ULL, 0x7172'7374'7576'7778ULL},
        .Context = MakeContext(),
    };
}

[[nodiscard]] ReplaceIfVersionRequest MakeReplaceRequest() noexcept
{
    const BindIfAbsentRequest bind = MakeBindRequest();
    return ReplaceIfVersionRequest{
        .Name = bind.Name,
        .ExpectedVersion =
            {
                .DirectoryGeneration = 0x8192'A3B4'C5D6'E7F8ULL,
                .Revision = 0x1021'3243'5465'7687ULL,
            },
        .ServiceInterface = bind.ServiceInterface,
        .ServiceGeneration = bind.ServiceGeneration,
        .Version = bind.Version,
        .PublishedRights = bind.PublishedRights,
        .TargetEndpoint = bind.TargetEndpoint,
        .TargetDeadline = bind.TargetDeadline,
        .ReplyEndpoint = bind.ReplyEndpoint,
        .Context = bind.Context,
    };
}

[[nodiscard]] RemoveIfVersionRequest MakeRemoveRequest() noexcept
{
    return RemoveIfVersionRequest{
        .Name = MakeStandardName(),
        .ExpectedVersion =
            {
                .DirectoryGeneration = 0x8192'A3B4'C5D6'E7F8ULL,
                .Revision = 0x1021'3243'5465'7687ULL,
            },
        .ReplyEndpoint = CapabilityHandle{0x6162'6364'6566'6768ULL, 0x7172'7374'7576'7778ULL},
        .Context = MakeContext(),
    };
}

[[nodiscard]] ResolveReply MakeResolveReply() noexcept
{
    return ResolveReply{
        .Status = DirectoryStatus::Success,
        .Version =
            {
                .DirectoryGeneration = 0x0102'0304'0506'0708ULL,
                .Revision = 0x1112'1314'1516'1718ULL,
            },
        .ServiceInterface = InterfaceIdentifier{0x2122'2324'2526'2728ULL, 0x3132'3334'3536'3738ULL},
        .ServiceGeneration = 0x4142'4344'4546'4748ULL,
        .GrantedDeadline = MonotonicTime{0x5152'5354'5556'5758ULL},
        .Major = 0x6162U,
        .SelectedMinor = 0x7172U,
        .GrantedRights = PublishedRights(),
        .TargetEndpoint = CapabilityHandle{0x8182'8384'8586'8788ULL, 0x9192'9394'9596'9798ULL},
        .Context = MakeContext(),
    };
}

[[nodiscard]] MutationReply MakeMutationReply() noexcept
{
    return MutationReply{
        .Status = DirectoryStatus::Success,
        .Version =
            {
                .DirectoryGeneration = 0x0102'0304'0506'0708ULL,
                .Revision = 0x1112'1314'1516'1718ULL,
            },
        .Context = MakeContext(),
    };
}

[[nodiscard]] MessageEnvelope MakeEnvelopeSentinel() noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{3U, 5U};
    message.TransactionIdentifier = 7U;
    message.Deadline = MonotonicTime{11U};
    message.Payload[200U] = 0xA5U;
    message.Attachments[3U].Handle = CapabilityHandle{13U, 17U};
    message.Reserved = 19U;
    return message;
}

[[nodiscard]] bool ContextsAreEqual(const DirectoryMessageContext& left, const DirectoryMessageContext& right) noexcept
{
    return (left.Trace.TraceIdentifierHigh == right.Trace.TraceIdentifierHigh) &&
           (left.Trace.TraceIdentifierLow == right.Trace.TraceIdentifierLow) &&
           (left.Trace.SpanIdentifier == right.Trace.SpanIdentifier) &&
           (left.Deadline.GetTicks() == right.Deadline.GetTicks()) &&
           (left.TransactionIdentifier == right.TransactionIdentifier);
}

[[nodiscard]] bool HandlesAreEqual(const CapabilityHandle left, const CapabilityHandle right) noexcept
{
    return (left.GetSelector() == right.GetSelector()) && (left.GetAuthenticator() == right.GetAuthenticator());
}

[[nodiscard]] bool InterfacesAreEqual(const InterfaceIdentifier left, const InterfaceIdentifier right) noexcept
{
    return (left.GetLow() == right.GetLow()) && (left.GetHigh() == right.GetHigh());
}

[[nodiscard]] bool VersionsAreEqual(const InterfaceVersionRange& left, const InterfaceVersionRange& right) noexcept
{
    return (left.Major == right.Major) && (left.MinimumMinor == right.MinimumMinor) &&
           (left.MaximumMinor == right.MaximumMinor) && (left.Reserved == right.Reserved);
}

[[nodiscard]] bool BindingVersionsAreEqual(const BindingVersion& left, const BindingVersion& right) noexcept
{
    return (left.DirectoryGeneration == right.DirectoryGeneration) && (left.Revision == right.Revision);
}

[[nodiscard]] bool ResolveRequestsAreEqual(const ResolveRequest& left, const ResolveRequest& right) noexcept
{
    return left.Name.IsEqualTo(right.Name) && InterfacesAreEqual(left.ServiceInterface, right.ServiceInterface) &&
           (left.RequestedDeadline.GetTicks() == right.RequestedDeadline.GetTicks()) &&
           VersionsAreEqual(left.RequiredVersion, right.RequiredVersion) &&
           left.RequiredRights.IsEqualTo(right.RequiredRights) &&
           HandlesAreEqual(left.ReplyEndpoint, right.ReplyEndpoint) && ContextsAreEqual(left.Context, right.Context);
}

[[nodiscard]] bool BindRequestsAreEqual(const BindIfAbsentRequest& left, const BindIfAbsentRequest& right) noexcept
{
    return left.Name.IsEqualTo(right.Name) && InterfacesAreEqual(left.ServiceInterface, right.ServiceInterface) &&
           (left.ServiceGeneration == right.ServiceGeneration) && VersionsAreEqual(left.Version, right.Version) &&
           left.PublishedRights.IsEqualTo(right.PublishedRights) &&
           HandlesAreEqual(left.TargetEndpoint, right.TargetEndpoint) &&
           (left.TargetDeadline.GetTicks() == right.TargetDeadline.GetTicks()) &&
           HandlesAreEqual(left.ReplyEndpoint, right.ReplyEndpoint) && ContextsAreEqual(left.Context, right.Context);
}

[[nodiscard]] bool ReplaceRequestsAreEqual(const ReplaceIfVersionRequest& left,
                                           const ReplaceIfVersionRequest& right) noexcept
{
    return left.Name.IsEqualTo(right.Name) && BindingVersionsAreEqual(left.ExpectedVersion, right.ExpectedVersion) &&
           InterfacesAreEqual(left.ServiceInterface, right.ServiceInterface) &&
           (left.ServiceGeneration == right.ServiceGeneration) && VersionsAreEqual(left.Version, right.Version) &&
           left.PublishedRights.IsEqualTo(right.PublishedRights) &&
           HandlesAreEqual(left.TargetEndpoint, right.TargetEndpoint) &&
           (left.TargetDeadline.GetTicks() == right.TargetDeadline.GetTicks()) &&
           HandlesAreEqual(left.ReplyEndpoint, right.ReplyEndpoint) && ContextsAreEqual(left.Context, right.Context);
}

[[nodiscard]] bool RemoveRequestsAreEqual(const RemoveIfVersionRequest& left,
                                          const RemoveIfVersionRequest& right) noexcept
{
    return left.Name.IsEqualTo(right.Name) && BindingVersionsAreEqual(left.ExpectedVersion, right.ExpectedVersion) &&
           HandlesAreEqual(left.ReplyEndpoint, right.ReplyEndpoint) && ContextsAreEqual(left.Context, right.Context);
}

[[nodiscard]] bool ResolveRepliesAreEqual(const ResolveReply& left, const ResolveReply& right) noexcept
{
    return (left.Status == right.Status) && BindingVersionsAreEqual(left.Version, right.Version) &&
           InterfacesAreEqual(left.ServiceInterface, right.ServiceInterface) &&
           (left.ServiceGeneration == right.ServiceGeneration) &&
           (left.GrantedDeadline.GetTicks() == right.GrantedDeadline.GetTicks()) && (left.Major == right.Major) &&
           (left.SelectedMinor == right.SelectedMinor) && left.GrantedRights.IsEqualTo(right.GrantedRights) &&
           HandlesAreEqual(left.TargetEndpoint, right.TargetEndpoint) && ContextsAreEqual(left.Context, right.Context);
}

[[nodiscard]] bool MutationRepliesAreEqual(const MutationReply& left, const MutationReply& right) noexcept
{
    return (left.Status == right.Status) && BindingVersionsAreEqual(left.Version, right.Version) &&
           ContextsAreEqual(left.Context, right.Context);
}

[[nodiscard]] bool AttachmentIsZero(const CapabilityAttachment& attachment) noexcept
{
    return !attachment.Handle.IsValid() && (attachment.Handle.GetAuthenticator() == 0U) &&
           (attachment.RequestedDeadline.GetTicks() == 0U) && (attachment.RequestedRights.GetRawValue() == 0U) &&
           (attachment.ExpectedType == ObjectType::Invalid) &&
           (attachment.TransferMode == CapabilityTransferMode::Invalid) && (attachment.Reserved == 0U);
}

[[nodiscard]] bool CanonicalTailIsZero(const MessageEnvelope& message) noexcept
{
    for (Size index{message.PayloadSizeBytes}; index < MessageEnvelope::MaximumPayloadBytes; ++index)
    {
        if (message.Payload[index] != 0U)
        {
            return false;
        }
    }
    for (Size index{message.AttachmentCount}; index < MessageEnvelope::MaximumCapabilityAttachments; ++index)
    {
        if (!AttachmentIsZero(message.Attachments[index]))
        {
            return false;
        }
    }
    for (Size index{message.VariablePayloadCount}; index < MessageEnvelope::MaximumVariablePayloads; ++index)
    {
        const auto& descriptor = message.VariablePayloads[index];
        if ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U))
        {
            return false;
        }
    }
    return (message.Reserved == 0U) && (message.ReservedAlignment == 0U);
}

[[nodiscard]] bool RoundTripMessage(const MessageEnvelope& message, MessageEnvelope& output) noexcept
{
    UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(message, WritableByteView{
                                                           .Data = wire,
                                                           .SizeBytes = sizeof(wire),
                                                       });
    if (!encoded.HasValue())
    {
        return false;
    }
    return MessageCodec::Decode(
               ReadOnlyByteView{
                   .Data = wire,
                   .SizeBytes = encoded.GetValue(),
               },
               output) == MessageCodecError::None;
}

void CheckResolveFailureBuildIsRejected(TestContext& runner, const ResolveReply& reply,
                                        const char* const description) noexcept
{
    MessageEnvelope output = MakeEnvelopeSentinel();
    const MessageEnvelope before = output;
    ASTERIA_EXPECT(runner, description,
                   ObjectDirectoryProtocol::BuildResolveReply(reply, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));
}

void TestRequestRoundTrips(TestContext& runner) noexcept
{
    MessageEnvelope message{};
    const ResolveRequest resolve = MakeResolveRequest();
    ASTERIA_EXPECT(runner, "resolve request builds",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolve, message) ==
                       ObjectDirectoryProtocolError::None);
    ResolveRequest parsedResolve{};
    ASTERIA_EXPECT(runner, "resolve request round trips",
                   ObjectDirectoryProtocol::ParseResolveRequest(message, parsedResolve) ==
                       ObjectDirectoryProtocolError::None,
                   ResolveRequestsAreEqual(resolve, parsedResolve));
    ASTERIA_EXPECT(runner, "resolve request has exact envelope shape", message.Kind == MessageKind::Asynchronous,
                   message.Cancellation == CancellationState::None,
                   message.MethodIdentifier == ObjectDirectoryProtocol::ResolveMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes,
                   message.PayloadSizeBytes ==
                       ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes + resolve.Name.GetSizeBytes(),
                   message.VariablePayloadCount == 1U, message.AttachmentCount == 1U);

    const BindIfAbsentRequest bind = MakeBindRequest();
    ASTERIA_EXPECT(runner, "bind request builds",
                   ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, message) ==
                       ObjectDirectoryProtocolError::None);
    BindIfAbsentRequest parsedBind{};
    ASTERIA_EXPECT(runner, "bind request round trips",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(message, parsedBind) ==
                       ObjectDirectoryProtocolError::None,
                   BindRequestsAreEqual(bind, parsedBind));
    ASTERIA_EXPECT(runner, "bind request has exact envelope shape", message.Kind == MessageKind::Asynchronous,
                   message.MethodIdentifier == ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::BindIfAbsentRequestPayloadSizeBytes,
                   message.PayloadSizeBytes ==
                       ObjectDirectoryProtocol::BindIfAbsentRequestPayloadSizeBytes + bind.Name.GetSizeBytes(),
                   message.VariablePayloadCount == 1U, message.AttachmentCount == 2U);

    const ReplaceIfVersionRequest replace = MakeReplaceRequest();
    ASTERIA_EXPECT(runner, "replace request builds",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, message) ==
                       ObjectDirectoryProtocolError::None);
    ReplaceIfVersionRequest parsedReplace{};
    ASTERIA_EXPECT(runner, "replace request round trips",
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(message, parsedReplace) ==
                       ObjectDirectoryProtocolError::None,
                   ReplaceRequestsAreEqual(replace, parsedReplace));
    ASTERIA_EXPECT(runner, "replace request has exact envelope shape", message.Kind == MessageKind::Asynchronous,
                   message.MethodIdentifier == ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::ReplaceIfVersionRequestPayloadSizeBytes,
                   message.PayloadSizeBytes ==
                       ObjectDirectoryProtocol::ReplaceIfVersionRequestPayloadSizeBytes + replace.Name.GetSizeBytes(),
                   message.VariablePayloadCount == 1U, message.AttachmentCount == 2U);

    const RemoveIfVersionRequest remove = MakeRemoveRequest();
    ASTERIA_EXPECT(runner, "remove request builds",
                   ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(remove, message) ==
                       ObjectDirectoryProtocolError::None);
    RemoveIfVersionRequest parsedRemove{};
    ASTERIA_EXPECT(runner, "remove request round trips",
                   ObjectDirectoryProtocol::ParseRemoveIfVersionRequest(message, parsedRemove) ==
                       ObjectDirectoryProtocolError::None,
                   RemoveRequestsAreEqual(remove, parsedRemove));
    ASTERIA_EXPECT(runner, "remove request has exact envelope shape", message.Kind == MessageKind::Asynchronous,
                   message.MethodIdentifier == ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::RemoveIfVersionRequestPayloadSizeBytes,
                   message.PayloadSizeBytes ==
                       ObjectDirectoryProtocol::RemoveIfVersionRequestPayloadSizeBytes + remove.Name.GetSizeBytes(),
                   message.VariablePayloadCount == 1U, message.AttachmentCount == 1U);
}

void TestReplyRoundTrips(TestContext& runner) noexcept
{
    const ResolveReply resolve = MakeResolveReply();
    MessageEnvelope message{};
    ASTERIA_EXPECT(runner, "successful resolve reply builds",
                   ObjectDirectoryProtocol::BuildResolveReply(resolve, message) == ObjectDirectoryProtocolError::None);
    ResolveReply parsedResolve{};
    ASTERIA_EXPECT(runner, "successful resolve reply round trips",
                   ObjectDirectoryProtocol::ParseResolveReply(message, parsedResolve) ==
                       ObjectDirectoryProtocolError::None,
                   ResolveRepliesAreEqual(resolve, parsedResolve));
    ASTERIA_EXPECT(runner, "successful resolve reply carries granted endpoint semantics",
                   message.Kind == MessageKind::Reply,
                   message.MethodIdentifier == ObjectDirectoryProtocol::ResolveMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::ResolveReplyPayloadSizeBytes,
                   message.PayloadSizeBytes == ObjectDirectoryProtocol::ResolveReplyPayloadSizeBytes,
                   message.VariablePayloadCount == 0U, message.AttachmentCount == 1U,
                   message.Attachments[0U].RequestedRights.IsEqualTo(resolve.GrantedRights),
                   message.Attachments[0U].RequestedDeadline.GetTicks() == resolve.GrantedDeadline.GetTicks());

    ResolveReply failure{};
    failure.Status = DirectoryStatus::NotFound;
    failure.Context = resolve.Context;
    ASTERIA_EXPECT(runner, "failed resolve reply builds",
                   ObjectDirectoryProtocol::BuildResolveReply(failure, message) == ObjectDirectoryProtocolError::None);
    ResolveReply expectedFailure{};
    expectedFailure.Status = DirectoryStatus::NotFound;
    expectedFailure.Context = failure.Context;
    ASTERIA_EXPECT(runner, "failed resolve reply round trips canonical zero result",
                   ObjectDirectoryProtocol::ParseResolveReply(message, parsedResolve) ==
                       ObjectDirectoryProtocolError::None,
                   ResolveRepliesAreEqual(expectedFailure, parsedResolve));
    ASTERIA_EXPECT(runner, "failed resolve reply zeroes bytes 8 through 63", message.AttachmentCount == 0U,
                   AreBytesEqual(message.Payload + 8U, MessageEnvelope{}.Payload + 8U,
                                 ObjectDirectoryProtocol::ResolveReplyPayloadSizeBytes - 8U));

    const MutationReply mutation = MakeMutationReply();
    MutationReply parsedMutation{};
    ASTERIA_EXPECT(
        runner, "bind mutation reply round trips",
        ObjectDirectoryProtocol::BuildBindIfAbsentReply(mutation, message) == ObjectDirectoryProtocolError::None,
        ObjectDirectoryProtocol::ParseBindIfAbsentReply(message, parsedMutation) == ObjectDirectoryProtocolError::None,
        MutationRepliesAreEqual(mutation, parsedMutation));
    ASTERIA_EXPECT(runner, "bind mutation reply has exact envelope shape",
                   message.MethodIdentifier == ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes,
                   message.PayloadSizeBytes == ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes,
                   message.VariablePayloadCount == 0U, message.AttachmentCount == 0U);
    ASTERIA_EXPECT(runner, "replace mutation reply round trips",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionReply(mutation, message) ==
                       ObjectDirectoryProtocolError::None,
                   ObjectDirectoryProtocol::ParseReplaceIfVersionReply(message, parsedMutation) ==
                       ObjectDirectoryProtocolError::None,
                   MutationRepliesAreEqual(mutation, parsedMutation));
    ASTERIA_EXPECT(runner, "replace mutation reply has exact envelope shape",
                   message.MethodIdentifier == ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes,
                   message.PayloadSizeBytes == ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes,
                   message.VariablePayloadCount == 0U, message.AttachmentCount == 0U);
    ASTERIA_EXPECT(runner, "remove mutation reply round trips",
                   ObjectDirectoryProtocol::BuildRemoveIfVersionReply(mutation, message) ==
                       ObjectDirectoryProtocolError::None,
                   ObjectDirectoryProtocol::ParseRemoveIfVersionReply(message, parsedMutation) ==
                       ObjectDirectoryProtocolError::None,
                   MutationRepliesAreEqual(mutation, parsedMutation));
    ASTERIA_EXPECT(runner, "remove mutation reply has exact envelope shape",
                   message.MethodIdentifier == ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier,
                   message.FixedPayloadSizeBytes == ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes,
                   message.PayloadSizeBytes == ObjectDirectoryProtocol::MutationReplyPayloadSizeBytes,
                   message.VariablePayloadCount == 0U, message.AttachmentCount == 0U);
}

void TestGoldenPayloads(TestContext& runner) noexcept
{
    MessageEnvelope message{};
    const ResolveRequest resolve = MakeResolveRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(resolve, message));
    ASTERIA_EXPECT(runner, "resolve payload has golden little-endian offsets",
                   ReadLittleEndian64(message.Payload + 0U) == resolve.ServiceInterface.GetLow(),
                   ReadLittleEndian64(message.Payload + 8U) == resolve.ServiceInterface.GetHigh(),
                   ReadLittleEndian64(message.Payload + 16U) == resolve.RequestedDeadline.GetTicks(),
                   ReadLittleEndian16(message.Payload + 24U) == resolve.RequiredVersion.Major,
                   ReadLittleEndian16(message.Payload + 26U) == resolve.RequiredVersion.MinimumMinor,
                   ReadLittleEndian16(message.Payload + 28U) == resolve.RequiredVersion.MaximumMinor,
                   ReadLittleEndian16(message.Payload + 30U) == resolve.RequiredRights.GetRawValue());

    const BindIfAbsentRequest bind = MakeBindRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, message));
    ASTERIA_EXPECT(runner, "bind payload has golden little-endian offsets and zero controls",
                   ReadLittleEndian64(message.Payload + 0U) == bind.ServiceInterface.GetLow(),
                   ReadLittleEndian64(message.Payload + 8U) == bind.ServiceInterface.GetHigh(),
                   ReadLittleEndian64(message.Payload + 16U) == bind.ServiceGeneration,
                   ReadLittleEndian16(message.Payload + 24U) == bind.Version.Major,
                   ReadLittleEndian16(message.Payload + 26U) == bind.Version.MinimumMinor,
                   ReadLittleEndian16(message.Payload + 28U) == bind.Version.MaximumMinor,
                   ReadLittleEndian16(message.Payload + 30U) == bind.PublishedRights.GetRawValue(),
                   ReadLittleEndian64(message.Payload + 32U) == 0U);

    const ReplaceIfVersionRequest replace = MakeReplaceRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, message));
    ASTERIA_EXPECT(runner, "replace payload has golden little-endian offsets and zero controls",
                   ReadLittleEndian64(message.Payload + 0U) == replace.ExpectedVersion.DirectoryGeneration,
                   ReadLittleEndian64(message.Payload + 8U) == replace.ExpectedVersion.Revision,
                   ReadLittleEndian64(message.Payload + 16U) == replace.ServiceInterface.GetLow(),
                   ReadLittleEndian64(message.Payload + 24U) == replace.ServiceInterface.GetHigh(),
                   ReadLittleEndian64(message.Payload + 32U) == replace.ServiceGeneration,
                   ReadLittleEndian16(message.Payload + 40U) == replace.Version.Major,
                   ReadLittleEndian16(message.Payload + 42U) == replace.Version.MinimumMinor,
                   ReadLittleEndian16(message.Payload + 44U) == replace.Version.MaximumMinor,
                   ReadLittleEndian16(message.Payload + 46U) == replace.PublishedRights.GetRawValue(),
                   ReadLittleEndian64(message.Payload + 48U) == 0U);

    const RemoveIfVersionRequest remove = MakeRemoveRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(remove, message));
    ASTERIA_EXPECT(runner, "remove payload has golden little-endian offsets and zero controls",
                   ReadLittleEndian64(message.Payload + 0U) == remove.ExpectedVersion.DirectoryGeneration,
                   ReadLittleEndian64(message.Payload + 8U) == remove.ExpectedVersion.Revision,
                   ReadLittleEndian64(message.Payload + 16U) == 0U);

    const ResolveReply resolveReply = MakeResolveReply();
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveReply(resolveReply, message));
    ASTERIA_EXPECT(runner, "resolve reply has golden little-endian offsets",
                   ReadLittleEndian16(message.Payload + 0U) == static_cast<UInt16>(DirectoryStatus::Success),
                   ReadLittleEndian64(message.Payload + 8U) == resolveReply.Version.DirectoryGeneration,
                   ReadLittleEndian64(message.Payload + 16U) == resolveReply.Version.Revision,
                   ReadLittleEndian64(message.Payload + 24U) == resolveReply.ServiceInterface.GetLow(),
                   ReadLittleEndian64(message.Payload + 32U) == resolveReply.ServiceInterface.GetHigh(),
                   ReadLittleEndian64(message.Payload + 40U) == resolveReply.ServiceGeneration,
                   ReadLittleEndian64(message.Payload + 48U) == resolveReply.GrantedDeadline.GetTicks(),
                   ReadLittleEndian16(message.Payload + 56U) == resolveReply.Major,
                   ReadLittleEndian16(message.Payload + 58U) == resolveReply.SelectedMinor,
                   ReadLittleEndian16(message.Payload + 60U) == resolveReply.GrantedRights.GetRawValue(),
                   ReadLittleEndian16(message.Payload + 62U) == 0U);

    const MutationReply mutation = MakeMutationReply();
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentReply(mutation, message));
    ASTERIA_EXPECT(runner, "mutation reply has golden little-endian offsets",
                   ReadLittleEndian16(message.Payload + 0U) == static_cast<UInt16>(mutation.Status),
                   ReadLittleEndian64(message.Payload + 8U) == mutation.Version.DirectoryGeneration,
                   ReadLittleEndian64(message.Payload + 16U) == mutation.Version.Revision);
}

void TestNameBoundariesAndFrameSize(TestContext& runner) noexcept
{
    constexpr UInt8 oneByteName[]{static_cast<UInt8>('a')};
    ResolveRequest resolve = MakeResolveRequest();
    resolve.Name = MakeName(oneByteName, sizeof(oneByteName));
    MessageEnvelope message{};
    ASTERIA_EXPECT(runner, "one-byte directory name builds",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolve, message) == ObjectDirectoryProtocolError::None,
                   message.PayloadSizeBytes == ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes + 1U);
    ResolveRequest parsedResolve{};
    ASTERIA_EXPECT(runner, "one-byte directory name round trips",
                   ObjectDirectoryProtocol::ParseResolveRequest(message, parsedResolve) ==
                       ObjectDirectoryProtocolError::None,
                   parsedResolve.Name.IsEqualTo(resolve.Name));

    constexpr UInt8 punctuatedName[]{
        static_cast<UInt8>('a'), static_cast<UInt8>('.'), static_cast<UInt8>('_'),
        static_cast<UInt8>('-'), static_cast<UInt8>('9'),
    };
    resolve.Name = MakeName(punctuatedName, sizeof(punctuatedName));
    ASTERIA_EXPECT(runner, "canonical interior name punctuation round trips",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolve, message) == ObjectDirectoryProtocolError::None,
                   ObjectDirectoryProtocol::ParseResolveRequest(message, parsedResolve) ==
                       ObjectDirectoryProtocolError::None,
                   parsedResolve.Name.IsEqualTo(resolve.Name));

    UInt8 maximumNameBytes[DirectoryName::MaximumSizeBytes]{};
    for (UInt8& value : maximumNameBytes)
    {
        value = static_cast<UInt8>('z');
    }
    ReplaceIfVersionRequest replace = MakeReplaceRequest();
    replace.Name = MakeName(maximumNameBytes, sizeof(maximumNameBytes));
    ASTERIA_EXPECT(runner, "64-byte directory name builds",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, message) ==
                       ObjectDirectoryProtocolError::None,
                   message.PayloadSizeBytes == ObjectDirectoryProtocol::ReplaceIfVersionRequestPayloadSizeBytes +
                                                   DirectoryName::MaximumSizeBytes);
    ReplaceIfVersionRequest parsedReplace{};
    ASTERIA_EXPECT(runner, "64-byte directory name round trips",
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(message, parsedReplace) ==
                       ObjectDirectoryProtocolError::None,
                   parsedReplace.Name.IsEqualTo(replace.Name));

    UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(message, WritableByteView{.Data = wire, .SizeBytes = sizeof(wire)});
    MessageEnvelope decoded{};
    ReplaceIfVersionRequest decodedReplace{};
    ASTERIA_EXPECT(runner, "maximum 280-byte directory frame round trips through message codec", encoded.HasValue(),
                   encoded.GetValue() == ObjectDirectoryProtocol::MaximumWireFrameSizeBytes,
                   encoded.GetValue() <= MessageCodec::MaximumWireSizeBytes,
                   MessageCodec::Decode(
                       ReadOnlyByteView{
                           .Data = wire,
                           .SizeBytes = encoded.GetValue(),
                       },
                       decoded) == MessageCodecError::None,
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(decoded, decodedReplace) ==
                       ObjectDirectoryProtocolError::None,
                   ReplaceRequestsAreEqual(replace, decodedReplace));
}

void TestMessageCodecRoundTrips(TestContext& runner) noexcept
{
    MessageEnvelope message{};
    MessageEnvelope decoded{};

    const ResolveRequest resolveRequest = MakeResolveRequest();
    ResolveRequest parsedResolveRequest{};
    ASTERIA_EXPECT(runner, "resolve request round trips through message codec",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolveRequest, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseResolveRequest(decoded, parsedResolveRequest) ==
                       ObjectDirectoryProtocolError::None,
                   ResolveRequestsAreEqual(resolveRequest, parsedResolveRequest));

    const BindIfAbsentRequest bindRequest = MakeBindRequest();
    BindIfAbsentRequest parsedBindRequest{};
    ASTERIA_EXPECT(runner, "bind request round trips through message codec",
                   ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bindRequest, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(decoded, parsedBindRequest) ==
                       ObjectDirectoryProtocolError::None,
                   BindRequestsAreEqual(bindRequest, parsedBindRequest));

    const ReplaceIfVersionRequest replaceRequest = MakeReplaceRequest();
    ReplaceIfVersionRequest parsedReplaceRequest{};
    ASTERIA_EXPECT(runner, "replace request round trips through message codec",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replaceRequest, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(decoded, parsedReplaceRequest) ==
                       ObjectDirectoryProtocolError::None,
                   ReplaceRequestsAreEqual(replaceRequest, parsedReplaceRequest));

    const RemoveIfVersionRequest removeRequest = MakeRemoveRequest();
    RemoveIfVersionRequest parsedRemoveRequest{};
    ASTERIA_EXPECT(runner, "remove request round trips through message codec",
                   ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(removeRequest, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseRemoveIfVersionRequest(decoded, parsedRemoveRequest) ==
                       ObjectDirectoryProtocolError::None,
                   RemoveRequestsAreEqual(removeRequest, parsedRemoveRequest));

    const ResolveReply resolveReply = MakeResolveReply();
    ResolveReply parsedResolveReply{};
    ASTERIA_EXPECT(
        runner, "successful resolve reply round trips through message codec",
        ObjectDirectoryProtocol::BuildResolveReply(resolveReply, message) == ObjectDirectoryProtocolError::None,
        RoundTripMessage(message, decoded),
        ObjectDirectoryProtocol::ParseResolveReply(decoded, parsedResolveReply) == ObjectDirectoryProtocolError::None,
        ResolveRepliesAreEqual(resolveReply, parsedResolveReply));

    ResolveReply failedResolveReply{};
    failedResolveReply.Status = DirectoryStatus::NotFound;
    failedResolveReply.Context = MakeContext();
    ResolveReply parsedFailedResolveReply{};
    ASTERIA_EXPECT(runner, "failed resolve reply round trips through message codec",
                   ObjectDirectoryProtocol::BuildResolveReply(failedResolveReply, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseResolveReply(decoded, parsedFailedResolveReply) ==
                       ObjectDirectoryProtocolError::None,
                   ResolveRepliesAreEqual(failedResolveReply, parsedFailedResolveReply));

    const MutationReply mutationReply = MakeMutationReply();
    MutationReply parsedMutationReply{};
    ASTERIA_EXPECT(runner, "bind reply round trips through message codec",
                   ObjectDirectoryProtocol::BuildBindIfAbsentReply(mutationReply, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseBindIfAbsentReply(decoded, parsedMutationReply) ==
                       ObjectDirectoryProtocolError::None,
                   MutationRepliesAreEqual(mutationReply, parsedMutationReply));
    ASTERIA_EXPECT(runner, "replace reply round trips through message codec",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionReply(mutationReply, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseReplaceIfVersionReply(decoded, parsedMutationReply) ==
                       ObjectDirectoryProtocolError::None,
                   MutationRepliesAreEqual(mutationReply, parsedMutationReply));
    ASTERIA_EXPECT(runner, "remove reply round trips through message codec",
                   ObjectDirectoryProtocol::BuildRemoveIfVersionReply(mutationReply, message) ==
                       ObjectDirectoryProtocolError::None,
                   RoundTripMessage(message, decoded),
                   ObjectDirectoryProtocol::ParseRemoveIfVersionReply(decoded, parsedMutationReply) ==
                       ObjectDirectoryProtocolError::None,
                   MutationRepliesAreEqual(mutationReply, parsedMutationReply));
}

void TestCanonicalConstructionAndAttachments(TestContext& runner) noexcept
{
    MessageEnvelope message{};
    const ResolveRequest resolve = MakeResolveRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(resolve, message));
    ASTERIA_EXPECT(runner, "resolve request canonical tail is zero", CanonicalTailIsZero(message));
    ASTERIA_EXPECT(runner, "request attachment zero is copied invoke-only reply endpoint",
                   message.Attachments[0U].Handle.IsValid(),
                   message.Attachments[0U].RequestedDeadline.GetTicks() == message.Deadline.GetTicks(),
                   message.Attachments[0U].RequestedRights.IsEqualTo(CapabilityRights{CapabilityRight::Invoke}),
                   message.Attachments[0U].ExpectedType == ObjectType::Endpoint,
                   message.Attachments[0U].TransferMode == CapabilityTransferMode::Copy);

    const BindIfAbsentRequest bind = MakeBindRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, message));
    ASTERIA_EXPECT(runner, "bind request canonical tail is zero", CanonicalTailIsZero(message));
    ASTERIA_EXPECT(
        runner, "bind attachment one is copied target with duplicate publication rights",
        message.Attachments[1U].Handle.GetSelector() == bind.TargetEndpoint.GetSelector(),
        message.Attachments[1U].RequestedDeadline.GetTicks() == bind.TargetDeadline.GetTicks(),
        message.Attachments[1U].RequestedRights.IsEqualTo(bind.PublishedRights.With(CapabilityRight::Duplicate)),
        message.Attachments[1U].ExpectedType == ObjectType::Endpoint,
        message.Attachments[1U].TransferMode == CapabilityTransferMode::Copy);

    const ResolveReply reply = MakeResolveReply();
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveReply(reply, message));
    ASTERIA_EXPECT(runner, "resolve reply canonical tail is zero", CanonicalTailIsZero(message));
    const MutationReply mutation = MakeMutationReply();
    static_cast<void>(ObjectDirectoryProtocol::BuildRemoveIfVersionReply(mutation, message));
    ASTERIA_EXPECT(runner, "mutation reply canonical tail is zero", CanonicalTailIsZero(message));
}

void TestBuildFailureAtomicity(TestContext& runner) noexcept
{
    MessageEnvelope output = MakeEnvelopeSentinel();
    MessageEnvelope before = output;
    ResolveRequest resolve = MakeResolveRequest();
    resolve.Context.TransactionIdentifier = 0U;
    ASTERIA_EXPECT(runner, "resolve build transaction failure is atomic",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolve, output) ==
                       ObjectDirectoryProtocolError::InvalidTransactionIdentifier,
                   AreBytesEqual(&output, &before, sizeof(output)));

    resolve = MakeResolveRequest();
    resolve.Context.Trace.SpanIdentifier = 0U;
    ASTERIA_EXPECT(runner, "resolve build trace failure is atomic",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolve, output) ==
                       ObjectDirectoryProtocolError::InvalidTraceContext,
                   AreBytesEqual(&output, &before, sizeof(output)));

    resolve = MakeResolveRequest();
    resolve.Name = DirectoryName{};
    ASTERIA_EXPECT(runner, "resolve build name failure is atomic",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolve, output) ==
                       ObjectDirectoryProtocolError::InvalidName,
                   AreBytesEqual(&output, &before, sizeof(output)));

    resolve = MakeResolveRequest();
    resolve.RequiredRights = CapabilityRights{CapabilityRight::Read};
    ASTERIA_EXPECT(runner, "resolve build rights failure is atomic",
                   ObjectDirectoryProtocol::BuildResolveRequest(resolve, output) ==
                       ObjectDirectoryProtocolError::InvalidRights,
                   AreBytesEqual(&output, &before, sizeof(output)));

    BindIfAbsentRequest bind = MakeBindRequest();
    bind.ServiceGeneration = 0U;
    ASTERIA_EXPECT(runner, "bind build generation failure is atomic",
                   ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, output) ==
                       ObjectDirectoryProtocolError::InvalidServiceGeneration,
                   AreBytesEqual(&output, &before, sizeof(output)));

    bind = MakeBindRequest();
    bind.TargetEndpoint = CapabilityHandle{};
    ASTERIA_EXPECT(runner, "bind build target failure is atomic",
                   ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, output) ==
                       ObjectDirectoryProtocolError::InvalidTargetCapability,
                   AreBytesEqual(&output, &before, sizeof(output)));

    ReplaceIfVersionRequest replace = MakeReplaceRequest();
    replace.Version.MinimumMinor = 10U;
    ASTERIA_EXPECT(runner, "replace build version failure is atomic",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, output) ==
                       ObjectDirectoryProtocolError::InvalidVersionRange,
                   AreBytesEqual(&output, &before, sizeof(output)));

    RemoveIfVersionRequest remove = MakeRemoveRequest();
    remove.ReplyEndpoint = CapabilityHandle{};
    ASTERIA_EXPECT(runner, "remove build reply endpoint failure is atomic",
                   ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(remove, output) ==
                       ObjectDirectoryProtocolError::InvalidReplyCapability,
                   AreBytesEqual(&output, &before, sizeof(output)));

    ResolveReply reply = MakeResolveReply();
    reply.Status = static_cast<DirectoryStatus>(0xFFFFU);
    ASTERIA_EXPECT(runner, "resolve reply build status failure is atomic",
                   ObjectDirectoryProtocol::BuildResolveReply(reply, output) ==
                       ObjectDirectoryProtocolError::InvalidStatus,
                   AreBytesEqual(&output, &before, sizeof(output)));

    MutationReply mutation = MakeMutationReply();
    mutation.Context.TransactionIdentifier = 0U;
    ASTERIA_EXPECT(runner, "mutation reply build failure is atomic",
                   ObjectDirectoryProtocol::BuildBindIfAbsentReply(mutation, output) ==
                       ObjectDirectoryProtocolError::InvalidTransactionIdentifier,
                   AreBytesEqual(&output, &before, sizeof(output)));
}

void TestCommonRequestMutations(TestContext& runner) noexcept
{
    const ResolveRequest request = MakeResolveRequest();
    MessageEnvelope valid{};
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(request, valid));
    ResolveRequest output = request;

    MessageEnvelope mutated = valid;
    mutated.Interface = InterfaceIdentifier{1U, 2U};
    ASTERIA_EXPECT(runner, "request rejects wrong interface",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidInterface);
    mutated = valid;
    ++mutated.InterfaceVersionMinor;
    ASTERIA_EXPECT(runner, "request rejects unsupported version",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::UnsupportedVersion);
    mutated = valid;
    ++mutated.MethodIdentifier;
    ASTERIA_EXPECT(runner, "request rejects wrong method",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMethod);
    mutated = valid;
    mutated.TransactionIdentifier = 0U;
    ASTERIA_EXPECT(runner, "request rejects zero transaction",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidTransactionIdentifier);
    mutated = valid;
    mutated.Trace.SpanIdentifier = 0U;
    ASTERIA_EXPECT(runner, "request rejects unpaired trace",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidTraceContext);
    mutated = valid;
    mutated.Kind = MessageKind::SynchronousCall;
    ASTERIA_EXPECT(runner, "request rejects wrong message kind",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.Cancellation = CancellationState::Cancellable;
    ASTERIA_EXPECT(runner, "request rejects cancellation state",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    ++mutated.FixedPayloadSizeBytes;
    ASTERIA_EXPECT(runner, "request rejects fixed payload size mutation",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.VariablePayloadCount = 0U;
    ASTERIA_EXPECT(runner, "request rejects missing name descriptor",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    ++mutated.VariablePayloads[0U].OffsetBytes;
    ASTERIA_EXPECT(runner, "request rejects name descriptor offset mutation",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    ++mutated.VariablePayloads[0U].SizeBytes;
    ASTERIA_EXPECT(runner, "request rejects name descriptor size mutation",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.PayloadSizeBytes = ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes;
    mutated.VariablePayloads[0U].SizeBytes = 0U;
    ASTERIA_EXPECT(runner, "request rejects a zero-byte name",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.PayloadSizeBytes = static_cast<UInt16>(ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes +
                                                   DirectoryName::MaximumSizeBytes + 1U);
    mutated.VariablePayloads[0U].SizeBytes = static_cast<UInt16>(DirectoryName::MaximumSizeBytes + 1U);
    for (Size index{ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes}; index < mutated.PayloadSizeBytes; ++index)
    {
        mutated.Payload[index] = static_cast<UInt8>('a');
    }
    ASTERIA_EXPECT(runner, "request rejects a 65-byte name",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.VariablePayloads[0U].Reserved = 1U;
    ASTERIA_EXPECT(runner, "request rejects name descriptor reserve",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.VariablePayloads[1U].Reserved = 1U;
    ASTERIA_EXPECT(runner, "request rejects nonzero unused descriptor",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Payload[mutated.PayloadSizeBytes] = 1U;
    ASTERIA_EXPECT(runner, "request rejects nonzero payload tail",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Attachments[1U].Handle = CapabilityHandle{1U, 2U};
    ASTERIA_EXPECT(runner, "request rejects nonzero unused attachment",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.ReservedAlignment = 1U;
    ASTERIA_EXPECT(runner, "request rejects nonzero envelope alignment reserve",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Reserved = 1U;
    ASTERIA_EXPECT(runner, "request rejects nonzero envelope reserve",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);

    mutated = valid;
    mutated.Payload[ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes] = static_cast<UInt8>('A');
    ASTERIA_EXPECT(runner, "request rejects invalid name character",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidName);
    mutated = valid;
    for (Size index{0U}; index < 16U; ++index)
    {
        mutated.Payload[index] = 0U;
    }
    ASTERIA_EXPECT(runner, "request rejects invalid service interface",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidInterface);
    mutated = valid;
    mutated.Payload[24U] = 0U;
    mutated.Payload[25U] = 0U;
    ASTERIA_EXPECT(runner, "request rejects zero major",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidVersionRange);
    mutated = valid;
    mutated.Payload[26U] = 10U;
    mutated.Payload[27U] = 0U;
    mutated.Payload[28U] = 1U;
    mutated.Payload[29U] = 0U;
    ASTERIA_EXPECT(runner, "request rejects descending minor range",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidVersionRange);
    mutated = valid;
    mutated.Payload[30U] = 0U;
    mutated.Payload[31U] = 0U;
    ASTERIA_EXPECT(runner, "request rejects empty rights",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidRights);
    mutated = valid;
    mutated.Payload[30U] = 0x02U;
    mutated.Payload[31U] = 0x80U;
    ASTERIA_EXPECT(runner, "request rejects unknown rights bit",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidRights);
}

void TestAttachmentMutations(TestContext& runner) noexcept
{
    const ResolveRequest request = MakeResolveRequest();
    MessageEnvelope valid{};
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(request, valid));
    ResolveRequest output = request;
    MessageEnvelope mutated = valid;

    mutated.Attachments[0U].Handle = CapabilityHandle{};
    ASTERIA_EXPECT(runner, "request rejects invalid reply handle",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].RequestedDeadline = MonotonicTime{1U};
    ASTERIA_EXPECT(runner, "request rejects reply deadline mismatch",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].RequestedRights = PublishedRights();
    ASTERIA_EXPECT(runner, "request rejects excessive reply rights",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].ExpectedType = ObjectType::Notification;
    ASTERIA_EXPECT(runner, "request rejects reply type mutation",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].TransferMode = CapabilityTransferMode::Move;
    ASTERIA_EXPECT(runner, "request rejects moved reply endpoint",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].Reserved = 1U;
    ASTERIA_EXPECT(runner, "request rejects reply attachment reserve",
                   ObjectDirectoryProtocol::ParseResolveRequest(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);

    const BindIfAbsentRequest bind = MakeBindRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, valid));
    BindIfAbsentRequest bindOutput = bind;
    mutated = valid;
    mutated.Attachments[1U].RequestedRights = bind.PublishedRights;
    ASTERIA_EXPECT(runner, "bind rejects target rights without duplicate",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(mutated, bindOutput) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[1U].ExpectedType = ObjectType::Notification;
    ASTERIA_EXPECT(runner, "bind rejects target type mutation",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(mutated, bindOutput) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[1U].TransferMode = CapabilityTransferMode::Move;
    ASTERIA_EXPECT(runner, "bind rejects moved target endpoint",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(mutated, bindOutput) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    const CapabilityAttachment reply = mutated.Attachments[0U];
    mutated.Attachments[0U] = mutated.Attachments[1U];
    mutated.Attachments[1U] = reply;
    ASTERIA_EXPECT(runner, "bind rejects reversed attachment order",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(mutated, bindOutput) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
}

void TestCasTokensAndReplyCanonicality(TestContext& runner) noexcept
{
    MessageEnvelope output = MakeEnvelopeSentinel();
    const MessageEnvelope before = output;

    ReplaceIfVersionRequest replace = MakeReplaceRequest();
    replace.ExpectedVersion.DirectoryGeneration = 0U;
    ASTERIA_EXPECT(runner, "replace builder rejects zero expected directory generation atomically",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));
    replace = MakeReplaceRequest();
    replace.ExpectedVersion.Revision = 0U;
    ASTERIA_EXPECT(runner, "replace builder rejects zero expected revision atomically",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));

    RemoveIfVersionRequest remove = MakeRemoveRequest();
    remove.ExpectedVersion.DirectoryGeneration = 0U;
    ASTERIA_EXPECT(runner, "remove builder rejects zero expected directory generation atomically",
                   ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(remove, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));
    remove = MakeRemoveRequest();
    remove.ExpectedVersion.Revision = 0U;
    ASTERIA_EXPECT(runner, "remove builder rejects zero expected revision atomically",
                   ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(remove, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));

    BindIfAbsentRequest bind = MakeBindRequest();
    bind.TargetEndpoint = CapabilityHandle{bind.ReplyEndpoint.GetSelector(), bind.TargetEndpoint.GetAuthenticator()};
    ASTERIA_EXPECT(runner, "bind builder rejects duplicate target and reply selectors atomically",
                   ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, output) ==
                       ObjectDirectoryProtocolError::InvalidTargetCapability,
                   AreBytesEqual(&output, &before, sizeof(output)));
    replace = MakeReplaceRequest();
    replace.TargetEndpoint =
        CapabilityHandle{replace.ReplyEndpoint.GetSelector(), replace.TargetEndpoint.GetAuthenticator()};
    ASTERIA_EXPECT(runner, "replace builder rejects duplicate target and reply selectors atomically",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, output) ==
                       ObjectDirectoryProtocolError::InvalidTargetCapability,
                   AreBytesEqual(&output, &before, sizeof(output)));

    MessageEnvelope message{};
    bind = MakeBindRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bind, message));
    message.Attachments[1U].Handle = CapabilityHandle{message.Attachments[0U].Handle.GetSelector(),
                                                      message.Attachments[1U].Handle.GetAuthenticator()};
    BindIfAbsentRequest parsedBind = bind;
    ASTERIA_EXPECT(runner, "bind parser rejects duplicate target and reply selectors atomically",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(message, parsedBind) ==
                       ObjectDirectoryProtocolError::InvalidAttachment,
                   BindRequestsAreEqual(parsedBind, bind));
    UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
    const auto duplicateEncoding = MessageCodec::Encode(message, WritableByteView{
                                                                     .Data = wire,
                                                                     .SizeBytes = sizeof(wire),
                                                                 });
    ASTERIA_EXPECT(runner, "message codec rejects duplicate directory attachments", !duplicateEncoding.HasValue(),
                   duplicateEncoding.GetError() == MessageCodecError::DuplicateAttachment);

    replace = MakeReplaceRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, message));
    message.Attachments[1U].Handle = CapabilityHandle{message.Attachments[0U].Handle.GetSelector(),
                                                      message.Attachments[1U].Handle.GetAuthenticator()};
    ReplaceIfVersionRequest parsedReplace = replace;
    ASTERIA_EXPECT(runner, "replace parser rejects duplicate target and reply selectors atomically",
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(message, parsedReplace) ==
                       ObjectDirectoryProtocolError::InvalidAttachment,
                   ReplaceRequestsAreEqual(parsedReplace, replace));

    static_cast<void>(ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replace, message));
    for (Size index{0U}; index < 8U; ++index)
    {
        message.Payload[index] = 0U;
    }
    ASTERIA_EXPECT(runner, "replace parser rejects zero expected directory generation atomically",
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(message, parsedReplace) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   ReplaceRequestsAreEqual(parsedReplace, replace));

    remove = MakeRemoveRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(remove, message));
    for (Size index{8U}; index < 16U; ++index)
    {
        message.Payload[index] = 0U;
    }
    RemoveIfVersionRequest parsedRemove = remove;
    ASTERIA_EXPECT(runner, "remove parser rejects zero expected revision atomically",
                   ObjectDirectoryProtocol::ParseRemoveIfVersionRequest(message, parsedRemove) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   RemoveRequestsAreEqual(parsedRemove, remove));

    ResolveReply resolveReply = MakeResolveReply();
    resolveReply.Version.Revision = 0U;
    ASTERIA_EXPECT(runner, "resolve reply builder rejects zero success binding version atomically",
                   ObjectDirectoryProtocol::BuildResolveReply(resolveReply, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));

    ResolveReply failedReply{};
    failedReply.Status = DirectoryStatus::NotFound;
    failedReply.Context = MakeContext();
    ResolveReply invalidFailedReply = failedReply;
    invalidFailedReply.Version.DirectoryGeneration = 1U;
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero binding version atomically");
    invalidFailedReply = failedReply;
    invalidFailedReply.ServiceInterface = InterfaceIdentifier{1U, 0U};
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero service interface atomically");
    invalidFailedReply = failedReply;
    invalidFailedReply.ServiceGeneration = 1U;
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero service generation atomically");
    invalidFailedReply = failedReply;
    invalidFailedReply.GrantedDeadline = MonotonicTime{1U};
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero granted deadline atomically");
    invalidFailedReply = failedReply;
    invalidFailedReply.Major = 1U;
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero major atomically");
    invalidFailedReply = failedReply;
    invalidFailedReply.SelectedMinor = 1U;
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero selected minor atomically");
    invalidFailedReply = failedReply;
    invalidFailedReply.GrantedRights = CapabilityRights{CapabilityRight::Invoke};
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero granted rights atomically");
    invalidFailedReply = failedReply;
    invalidFailedReply.TargetEndpoint = CapabilityHandle{0U, 1U};
    CheckResolveFailureBuildIsRejected(runner, invalidFailedReply,
                                       "failed resolve builder rejects nonzero target authenticator atomically");

    resolveReply = MakeResolveReply();
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveReply(resolveReply, message));
    for (Size index{8U}; index < 16U; ++index)
    {
        message.Payload[index] = 0U;
    }
    ResolveReply parsedResolveReply = resolveReply;
    ASTERIA_EXPECT(runner, "resolve parser rejects zero success binding version atomically",
                   ObjectDirectoryProtocol::ParseResolveReply(message, parsedResolveReply) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   ResolveRepliesAreEqual(parsedResolveReply, resolveReply));

    MutationReply mutationReply = MakeMutationReply();
    mutationReply.Version.Revision = 0U;
    ASTERIA_EXPECT(runner, "mutation builder rejects zero success binding version atomically",
                   ObjectDirectoryProtocol::BuildBindIfAbsentReply(mutationReply, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));

    MutationReply failedMutationReply{};
    failedMutationReply.Status = DirectoryStatus::BindingChanged;
    failedMutationReply.Context = MakeContext();
    MutationReply parsedMutationReply{};
    ASTERIA_EXPECT(runner, "canonical failed mutation reply round trips",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionReply(failedMutationReply, message) ==
                       ObjectDirectoryProtocolError::None,
                   ObjectDirectoryProtocol::ParseReplaceIfVersionReply(message, parsedMutationReply) ==
                       ObjectDirectoryProtocolError::None,
                   MutationRepliesAreEqual(failedMutationReply, parsedMutationReply));

    message.Payload[8U] = 1U;
    parsedMutationReply = MakeMutationReply();
    ASTERIA_EXPECT(runner, "failed mutation parser rejects nonzero binding version atomically",
                   ObjectDirectoryProtocol::ParseReplaceIfVersionReply(message, parsedMutationReply) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   MutationRepliesAreEqual(parsedMutationReply, MakeMutationReply()));
    failedMutationReply.Version.Revision = 1U;
    ASTERIA_EXPECT(runner, "failed mutation builder rejects nonzero binding version atomically",
                   ObjectDirectoryProtocol::BuildReplaceIfVersionReply(failedMutationReply, output) ==
                       ObjectDirectoryProtocolError::InvalidPayload,
                   AreBytesEqual(&output, &before, sizeof(output)));
}

void TestMethodSpecificRequestMutations(TestContext& runner) noexcept
{
    MessageEnvelope message{};
    BindIfAbsentRequest bindOutput = MakeBindRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bindOutput, message));
    message.Payload[32U] = 1U;
    ASTERIA_EXPECT(runner, "bind rejects nonzero flags",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(message, bindOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);

    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(MakeBindRequest(), message));
    for (Size index{16U}; index < 24U; ++index)
    {
        message.Payload[index] = 0U;
    }
    ASTERIA_EXPECT(runner, "bind rejects zero service generation",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(message, bindOutput) ==
                       ObjectDirectoryProtocolError::InvalidServiceGeneration);

    ReplaceIfVersionRequest replaceOutput = MakeReplaceRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replaceOutput, message));
    message.Payload[52U] = 1U;
    ASTERIA_EXPECT(runner, "replace rejects nonzero reserve",
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(message, replaceOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);

    RemoveIfVersionRequest removeOutput = MakeRemoveRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(removeOutput, message));
    message.Payload[16U] = 1U;
    ASTERIA_EXPECT(runner, "remove rejects nonzero flags",
                   ObjectDirectoryProtocol::ParseRemoveIfVersionRequest(message, removeOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
}

void TestReplyMutations(TestContext& runner) noexcept
{
    MessageEnvelope valid{};
    const ResolveReply reply = MakeResolveReply();
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveReply(reply, valid));
    const CapabilityAttachment successfulAttachment = valid.Attachments[0U];
    ResolveReply output = reply;
    MessageEnvelope mutated = valid;

    mutated.Payload[2U] = 1U;
    ASTERIA_EXPECT(runner, "resolve reply rejects status reserve",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Payload[62U] = 1U;
    ASTERIA_EXPECT(runner, "resolve reply rejects trailing reserve",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    for (Size index{24U}; index < 40U; ++index)
    {
        mutated.Payload[index] = 0U;
    }
    ASTERIA_EXPECT(runner, "resolve reply rejects invalid service interface",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidInterface);
    mutated = valid;
    for (Size index{40U}; index < 48U; ++index)
    {
        mutated.Payload[index] = 0U;
    }
    ASTERIA_EXPECT(runner, "resolve reply rejects zero service generation",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidServiceGeneration);
    mutated = valid;
    mutated.Payload[56U] = 0U;
    mutated.Payload[57U] = 0U;
    ASTERIA_EXPECT(runner, "resolve reply rejects zero major",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidVersionRange);
    mutated = valid;
    mutated.Payload[60U] = 0U;
    mutated.Payload[61U] = 0U;
    ASTERIA_EXPECT(runner, "resolve reply rejects empty granted rights",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidRights);
    mutated = valid;
    mutated.Attachments[0U].RequestedDeadline = MonotonicTime{1U};
    ASTERIA_EXPECT(runner, "resolve reply rejects granted deadline mismatch",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].RequestedRights = CapabilityRights{CapabilityRight::Invoke};
    ASTERIA_EXPECT(runner, "resolve reply rejects granted rights mismatch",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidAttachment);

    ResolveReply failed{};
    failed.Status = DirectoryStatus::NotFound;
    failed.Context = reply.Context;
    ASTERIA_EXPECT(runner, "canonical failed resolve reply builds for mutation tests",
                   ObjectDirectoryProtocol::BuildResolveReply(failed, valid) == ObjectDirectoryProtocolError::None);
    mutated = valid;
    mutated.Payload[8U] = 1U;
    ASTERIA_EXPECT(runner, "failed resolve reply rejects nonzero result fields",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.AttachmentCount = 1U;
    mutated.Attachments[0U] = successfulAttachment;
    ASTERIA_EXPECT(runner, "failed resolve reply rejects attachment count",
                   ObjectDirectoryProtocol::ParseResolveReply(mutated, output) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);

    const MutationReply mutation = MakeMutationReply();
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentReply(mutation, valid));
    MutationReply mutationOutput = mutation;
    mutated = valid;
    mutated.Payload[2U] = 1U;
    ASTERIA_EXPECT(runner, "mutation reply rejects status reserve",
                   ObjectDirectoryProtocol::ParseBindIfAbsentReply(mutated, mutationOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Payload[0U] = 0xFFU;
    mutated.Payload[1U] = 0xFFU;
    ASTERIA_EXPECT(runner, "mutation reply rejects unknown directory status",
                   ObjectDirectoryProtocol::ParseBindIfAbsentReply(mutated, mutationOutput) ==
                       ObjectDirectoryProtocolError::InvalidStatus);
    mutated = valid;
    mutated.AttachmentCount = 1U;
    ASTERIA_EXPECT(runner, "mutation reply rejects all attachments",
                   ObjectDirectoryProtocol::ParseBindIfAbsentReply(mutated, mutationOutput) ==
                       ObjectDirectoryProtocolError::InvalidMessageContract);
}

void TestParseFailureAtomicity(TestContext& runner) noexcept
{
    MessageEnvelope message{};
    const ResolveRequest resolveSentinel = MakeResolveRequest();
    ResolveRequest resolveOutput = resolveSentinel;
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(resolveSentinel, message));
    message.Payload[30U] = 0U;
    message.Payload[31U] = 0U;
    ASTERIA_EXPECT(runner, "resolve parse failure is atomic",
                   ObjectDirectoryProtocol::ParseResolveRequest(message, resolveOutput) ==
                       ObjectDirectoryProtocolError::InvalidRights,
                   ResolveRequestsAreEqual(resolveOutput, resolveSentinel));

    const BindIfAbsentRequest bindSentinel = MakeBindRequest();
    BindIfAbsentRequest bindOutput = bindSentinel;
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(bindSentinel, message));
    message.Attachments[1U].Handle = CapabilityHandle{};
    ASTERIA_EXPECT(runner, "bind parse failure is atomic",
                   ObjectDirectoryProtocol::ParseBindIfAbsentRequest(message, bindOutput) ==
                       ObjectDirectoryProtocolError::InvalidAttachment,
                   BindRequestsAreEqual(bindOutput, bindSentinel));

    const ReplaceIfVersionRequest replaceSentinel = MakeReplaceRequest();
    ReplaceIfVersionRequest replaceOutput = replaceSentinel;
    static_cast<void>(ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(replaceSentinel, message));
    message.Payload[48U] = 1U;
    ASTERIA_EXPECT(runner, "replace parse failure is atomic",
                   ObjectDirectoryProtocol::ParseReplaceIfVersionRequest(message, replaceOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage,
                   ReplaceRequestsAreEqual(replaceOutput, replaceSentinel));

    const RemoveIfVersionRequest removeSentinel = MakeRemoveRequest();
    RemoveIfVersionRequest removeOutput = removeSentinel;
    static_cast<void>(ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(removeSentinel, message));
    message.Payload[16U] = 1U;
    ASTERIA_EXPECT(runner, "remove parse failure is atomic",
                   ObjectDirectoryProtocol::ParseRemoveIfVersionRequest(message, removeOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage,
                   RemoveRequestsAreEqual(removeOutput, removeSentinel));

    const ResolveReply replySentinel = MakeResolveReply();
    ResolveReply replyOutput = replySentinel;
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveReply(replySentinel, message));
    message.Payload[62U] = 1U;
    ASTERIA_EXPECT(runner, "resolve reply parse failure is atomic",
                   ObjectDirectoryProtocol::ParseResolveReply(message, replyOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage,
                   ResolveRepliesAreEqual(replyOutput, replySentinel));

    const MutationReply mutationSentinel = MakeMutationReply();
    MutationReply mutationOutput = mutationSentinel;
    static_cast<void>(ObjectDirectoryProtocol::BuildRemoveIfVersionReply(mutationSentinel, message));
    message.Payload[2U] = 1U;
    ASTERIA_EXPECT(runner, "mutation reply parse failure is atomic",
                   ObjectDirectoryProtocol::ParseRemoveIfVersionReply(message, mutationOutput) ==
                       ObjectDirectoryProtocolError::NoncanonicalMessage,
                   MutationRepliesAreEqual(mutationOutput, mutationSentinel));
}

void TestInvalidNames(TestContext& runner) noexcept
{
    ResolveRequest request = MakeResolveRequest();
    MessageEnvelope output = MakeEnvelopeSentinel();
    const MessageEnvelope before = output;
    request.Name = DirectoryName{};
    ASTERIA_EXPECT(runner, "builder rejects empty directory name",
                   ObjectDirectoryProtocol::BuildResolveRequest(request, output) ==
                       ObjectDirectoryProtocolError::InvalidName,
                   AreBytesEqual(&output, &before, sizeof(output)));

    MessageEnvelope message{};
    request = MakeResolveRequest();
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(request, message));
    message.Payload[ObjectDirectoryProtocol::ResolveRequestPayloadSizeBytes] = static_cast<UInt8>('-');
    ResolveRequest parsed = request;
    ASTERIA_EXPECT(runner, "parser rejects punctuation at name boundary",
                   ObjectDirectoryProtocol::ParseResolveRequest(message, parsed) ==
                       ObjectDirectoryProtocolError::InvalidName,
                   ResolveRequestsAreEqual(parsed, request));
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectDirectoryProtocolTests"};
    Asteria::Test::TestRequestRoundTrips(runner);
    Asteria::Test::TestReplyRoundTrips(runner);
    Asteria::Test::TestGoldenPayloads(runner);
    Asteria::Test::TestNameBoundariesAndFrameSize(runner);
    Asteria::Test::TestMessageCodecRoundTrips(runner);
    Asteria::Test::TestCanonicalConstructionAndAttachments(runner);
    Asteria::Test::TestBuildFailureAtomicity(runner);
    Asteria::Test::TestCommonRequestMutations(runner);
    Asteria::Test::TestAttachmentMutations(runner);
    Asteria::Test::TestCasTokensAndReplyCanonicality(runner);
    Asteria::Test::TestMethodSpecificRequestMutations(runner);
    Asteria::Test::TestReplyMutations(runner);
    Asteria::Test::TestParseFailureAtomicity(runner);
    Asteria::Test::TestInvalidNames(runner);
    return runner.Finish();
}
