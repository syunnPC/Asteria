// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Memory/PagerProtocol.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::PagerFaultNotification;
using Kernel::Memory::PagerProtocol;
using Kernel::Memory::PagerProtocolError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectType;

[[nodiscard]] bool AreBytesEqual(const void* const left, const void* const right, const Size size) noexcept
{
    const auto* const leftBytes = static_cast<const UInt8*>(left);
    const auto* const rightBytes = static_cast<const UInt8*>(right);
    for (Size index{0U}; index < size; ++index)
    {
        if (leftBytes[index] != rightBytes[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] PagerFaultNotification MakeNotification() noexcept
{
    return PagerFaultNotification{
        .Fault =
            {
                .VirtualAddress = 0x0123'4567'89AB'CDEFULL,
                .InstructionPointer = 0xFEDC'BA98'7654'3210ULL,
                .ArchitectureSyndrome = 0x0F1E'2D3C'4B5A'6978ULL,
                .Access = FaultAccess::Execute,
                .Cause = FaultCause::ProtectionViolation,
                .Privilege = FaultPrivilege::Supervisor,
                .Reserved = {0U, 0U, 0U, 0U, 0U},
            },
        .SupervisionTag = 0xC0DE'0000'0000'5A61ULL,
        .ProtectionDomainIdentity = 0x0102'0304'0506'0708ULL,
        .Trace =
            {
                .TraceIdentifierHigh = 0x1122'3344'5566'7788ULL,
                .TraceIdentifierLow = 0x99AA'BBCC'DDEE'FF00ULL,
                .SpanIdentifier = 0x8877'6655'4433'2211ULL,
            },
        .FaultToken = CapabilityHandle{0x0102'0304'0506'0708ULL, 0xA1A2'A3A4'A5A6'A7A8ULL},
        .Deadline = MonotonicTime{0x1020'3040'5060'7080ULL},
        .TransactionIdentifier = 0x8070'6050'4030'2010ULL,
    };
}

[[nodiscard]] MessageEnvelope MakeEnvelopeSentinel() noexcept
{
    MessageEnvelope sentinel{};
    sentinel.Interface = InterfaceIdentifier{7U, 9U};
    sentinel.TransactionIdentifier = 11U;
    sentinel.Deadline = MonotonicTime{13U};
    sentinel.MethodIdentifier = 15U;
    sentinel.InterfaceVersionMajor = 17U;
    sentinel.Payload[200U] = 0xA5U;
    sentinel.Reserved = 19U;
    return sentinel;
}

[[nodiscard]] PagerFaultNotification MakeOutputSentinel() noexcept
{
    PagerFaultNotification sentinel = MakeNotification();
    sentinel.Fault.VirtualAddress ^= 0xFFFFU;
    sentinel.TransactionIdentifier ^= 0xAAAAU;
    return sentinel;
}

[[nodiscard]] bool NotificationIsEqual(const PagerFaultNotification& left, const PagerFaultNotification& right) noexcept
{
    return AreBytesEqual(&left, &right, sizeof(left));
}

[[nodiscard]] bool BuildFailureIsAtomic(const PagerFaultNotification& notification,
                                        const PagerProtocolError expected) noexcept
{
    MessageEnvelope output = MakeEnvelopeSentinel();
    const MessageEnvelope before = output;
    return (PagerProtocol::BuildFaultNotification(notification, output) == expected) &&
           AreBytesEqual(&output, &before, sizeof(output));
}

[[nodiscard]] bool ParseFailureIsAtomic(const MessageEnvelope& message, const PagerProtocolError expected) noexcept
{
    PagerFaultNotification output = MakeOutputSentinel();
    const PagerFaultNotification before = output;
    return (PagerProtocol::ParseFaultNotification(message, output) == expected) && NotificationIsEqual(output, before);
}

[[nodiscard]] bool PayloadHasGoldenBytes(const MessageEnvelope& message) noexcept
{
    constexpr UInt8 Expected[PagerProtocol::FaultPayloadSizeBytes]{
        0xEFU, 0xCDU, 0xABU, 0x89U, 0x67U, 0x45U, 0x23U, 0x01U, 0x10U, 0x32U, 0x54U, 0x76U, 0x98U, 0xBAU, 0xDCU, 0xFEU,
        0x78U, 0x69U, 0x5AU, 0x4BU, 0x3CU, 0x2DU, 0x1EU, 0x0FU, 0x03U, 0x02U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x61U, 0x5AU, 0x00U, 0x00U, 0x00U, 0x00U, 0xDEU, 0xC0U, 0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
    };
    return AreBytesEqual(message.Payload, Expected, sizeof(Expected));
}

[[nodiscard]] bool CanonicalTailIsZero(const MessageEnvelope& message) noexcept
{
    for (Size index{PagerProtocol::FaultPayloadSizeBytes}; index < MessageEnvelope::MaximumPayloadBytes; ++index)
    {
        if (message.Payload[index] != 0U)
        {
            return false;
        }
    }
    for (Size index{1U}; index < MessageEnvelope::MaximumCapabilityAttachments; ++index)
    {
        const CapabilityAttachment& attachment = message.Attachments[index];
        if (attachment.Handle.IsValid() || (attachment.Handle.GetAuthenticator() != 0U) ||
            (attachment.RequestedDeadline.GetTicks() != 0U) || (attachment.RequestedRights.GetRawValue() != 0U) ||
            (attachment.ExpectedType != ObjectType::Invalid) ||
            (attachment.TransferMode != CapabilityTransferMode::Invalid) || (attachment.Reserved != 0U))
        {
            return false;
        }
    }
    for (const auto& descriptor : message.VariablePayloads)
    {
        if ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U))
        {
            return false;
        }
    }
    return true;
}

void TestCanonicalRoundTrip(TestContext& runner) noexcept
{
    const PagerFaultNotification notification = MakeNotification();
    MessageEnvelope message{};
    const PagerProtocolError built = PagerProtocol::BuildFaultNotification(notification, message);
    const CapabilityAttachment& attachment = message.Attachments[0U];
    ASTERIA_EXPECT(
        runner, "builder emits the exact canonical pager-fault envelope", built == PagerProtocolError::None,
        message.Interface.GetLow() == PagerProtocol::Interface.GetLow(),
        message.Interface.GetHigh() == PagerProtocol::Interface.GetHigh(),
        message.TransactionIdentifier == notification.TransactionIdentifier,
        message.Deadline.GetTicks() == notification.Deadline.GetTicks(),
        message.MethodIdentifier == PagerProtocol::FaultMethodIdentifier,
        message.InterfaceVersionMajor == PagerProtocol::VersionMajor,
        message.InterfaceVersionMinor == PagerProtocol::VersionMinor,
        message.FixedPayloadSizeBytes == PagerProtocol::FaultPayloadSizeBytes,
        message.PayloadSizeBytes == PagerProtocol::FaultPayloadSizeBytes, message.Kind == MessageKind::Asynchronous,
        message.Cancellation == CancellationState::None, message.VariablePayloadCount == 0U,
        message.AttachmentCount == 1U, message.Reserved == 0U, message.ReservedAlignment == 0U,
        attachment.Handle.IsValid(), attachment.Handle.GetSelector() == notification.FaultToken.GetSelector(),
        attachment.Handle.GetAuthenticator() == notification.FaultToken.GetAuthenticator(),
        attachment.RequestedDeadline.GetTicks() == notification.Deadline.GetTicks(),
        attachment.RequestedRights.IsEqualTo(CapabilityRights{CapabilityRight::Invoke}),
        attachment.ExpectedType == ObjectType::FaultToken, attachment.TransferMode == CapabilityTransferMode::Move,
        attachment.Reserved == 0U, PayloadHasGoldenBytes(message), CanonicalTailIsZero(message));

    UInt8 wire[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(message, Kernel::Ipc::WritableByteView{wire, sizeof(wire)});
    MessageEnvelope decoded{};
    const MessageCodecError decodedError =
        encoded.HasValue() ? MessageCodec::Decode(Kernel::Ipc::ReadOnlyByteView{wire, encoded.GetValue()}, decoded)
                           : MessageCodecError::OutputTooSmall;
    PagerFaultNotification parsed{};
    const PagerProtocolError parsedError = decodedError == MessageCodecError::None
                                               ? PagerProtocol::ParseFaultNotification(decoded, parsed)
                                               : PagerProtocolError::InvalidMessageContract;
    ASTERIA_EXPECT(runner, "generic wire codec round-trips the typed pager notification exactly", encoded.HasValue(),
                   encoded.GetValue() == 168U, decodedError == MessageCodecError::None,
                   parsedError == PagerProtocolError::None, NotificationIsEqual(parsed, notification));
}

void TestBuildFailures(TestContext& runner) noexcept
{
    const PagerFaultNotification valid = MakeNotification();
    bool allRejected{true};

    PagerFaultNotification mutated = valid;
    mutated.TransactionIdentifier = 0U;
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidTransactionIdentifier);

    mutated = valid;
    mutated.Trace.SpanIdentifier = 0U;
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidTraceContext);

    mutated = valid;
    mutated.Fault.Access = static_cast<FaultAccess>(0U);
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidFaultRecord);

    mutated = valid;
    mutated.Fault.Access = static_cast<FaultAccess>(4U);
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidFaultRecord);

    mutated = valid;
    mutated.Fault.Cause = static_cast<FaultCause>(0xffU);
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidFaultRecord);

    mutated = valid;
    mutated.Fault.Privilege = static_cast<FaultPrivilege>(0U);
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidFaultRecord);

    mutated = valid;
    mutated.Fault.Reserved[0U] = 1U;
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidFaultRecord);

    mutated = valid;
    mutated.Fault.Reserved[4U] = 1U;
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidFaultRecord);

    mutated = valid;
    mutated.FaultToken = CapabilityHandle{};
    allRejected = allRejected && BuildFailureIsAtomic(mutated, PagerProtocolError::InvalidFaultToken);

    ASTERIA_EXPECT(runner, "builder rejects invalid identity, trace, fault, and token atomically", allRejected);
}

void TestHeaderAndShapeRejection(TestContext& runner) noexcept
{
    MessageEnvelope valid{};
    static_cast<void>(PagerProtocol::BuildFaultNotification(MakeNotification(), valid));
    bool allRejected{true};
    MessageEnvelope mutated = valid;

    mutated.Interface = InterfaceIdentifier{1U, 2U};
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidInterface);
    mutated = valid;
    mutated.InterfaceVersionMajor = 2U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::UnsupportedVersion);
    mutated = valid;
    mutated.InterfaceVersionMinor = 0U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::UnsupportedVersion);
    mutated = valid;
    mutated.InterfaceVersionMinor = 3U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::UnsupportedVersion);
    mutated = valid;
    mutated.MethodIdentifier = 2U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidMethod);
    mutated = valid;
    mutated.TransactionIdentifier = 0U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidTransactionIdentifier);
    mutated = valid;
    mutated.Trace.TraceIdentifierHigh = 0U;
    mutated.Trace.TraceIdentifierLow = 0U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidTraceContext);
    mutated = valid;
    mutated.Kind = MessageKind::SynchronousCall;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.Cancellation = CancellationState::Cancellable;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.FixedPayloadSizeBytes = 32U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.PayloadSizeBytes = 41U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.VariablePayloadCount = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidMessageContract);
    mutated = valid;
    mutated.AttachmentCount = 2U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidMessageContract);

    ASTERIA_EXPECT(runner, "parser rejects every foreign header and message-shape variant atomically", allRejected);
}

void TestCanonicalPayloadAndAttachmentRejection(TestContext& runner) noexcept
{
    MessageEnvelope valid{};
    static_cast<void>(PagerProtocol::BuildFaultNotification(MakeNotification(), valid));
    bool allRejected{true};
    MessageEnvelope mutated = valid;

    mutated.Reserved = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.ReservedAlignment = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.VariablePayloads[0U].OffsetBytes = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.VariablePayloads[3U].Reserved = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Attachments[1U].Handle = CapabilityHandle{0U, 1U};
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Attachments[3U].Reserved = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Payload[PagerProtocol::FaultPayloadSizeBytes] = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Payload[255U] = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::NoncanonicalMessage);
    mutated = valid;
    mutated.Payload[24U] = 0U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidPayload);
    mutated = valid;
    mutated.Payload[24U] = 4U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidPayload);
    mutated = valid;
    mutated.Payload[25U] = 0xffU;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidPayload);
    mutated = valid;
    mutated.Payload[26U] = 0U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidPayload);
    mutated = valid;
    mutated.Payload[27U] = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidPayload);
    mutated = valid;
    mutated.Payload[31U] = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidPayload);

    mutated = valid;
    mutated.Attachments[0U].Handle = CapabilityHandle{};
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].RequestedDeadline = MonotonicTime{1U};
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].RequestedRights = CapabilityRights{CapabilityRight::Inspect};
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].RequestedRights = CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Inspect);
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].ExpectedType = ObjectType::Endpoint;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].TransferMode = CapabilityTransferMode::Copy;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidAttachment);
    mutated = valid;
    mutated.Attachments[0U].Reserved = 1U;
    allRejected = allRejected && ParseFailureIsAtomic(mutated, PagerProtocolError::InvalidAttachment);

    ASTERIA_EXPECT(runner, "parser rejects noncanonical bytes and attenuated-token violations atomically", allRejected);
}

void TestBoundaryValues(TestContext& runner) noexcept
{
    PagerFaultNotification boundary = MakeNotification();
    boundary.Fault.VirtualAddress = ~UInt64{0U};
    boundary.Fault.InstructionPointer = 0U;
    boundary.Fault.ArchitectureSyndrome = ~UInt64{0U};
    boundary.Fault.Access = FaultAccess::Read;
    boundary.Fault.Cause = FaultCause::InvalidMappingEncoding;
    boundary.Fault.Privilege = FaultPrivilege::User;
    boundary.SupervisionTag = ~UInt64{0U};
    boundary.Trace = {};
    boundary.FaultToken = CapabilityHandle{~UInt64{0U}, ~UInt64{0U}};
    boundary.Deadline = MonotonicTime::Infinite();
    boundary.TransactionIdentifier = ~UInt64{0U};

    MessageEnvelope message{};
    const PagerProtocolError built = PagerProtocol::BuildFaultNotification(boundary, message);
    PagerFaultNotification parsed{};
    const PagerProtocolError parsedError = PagerProtocol::ParseFaultNotification(message, parsed);
    ASTERIA_EXPECT(runner, "finite fields, zero trace, and all-ones opaque values round-trip exactly",
                   built == PagerProtocolError::None, parsedError == PagerProtocolError::None,
                   NotificationIsEqual(parsed, boundary));

    boundary.Trace = {
        .TraceIdentifierHigh = 0U,
        .TraceIdentifierLow = 1U,
        .SpanIdentifier = 2U,
    };
    const PagerProtocolError lowTraceBuilt = PagerProtocol::BuildFaultNotification(boundary, message);
    const PagerProtocolError lowTraceParsed = PagerProtocol::ParseFaultNotification(message, parsed);
    ASTERIA_EXPECT(runner, "a low-half-only trace identifier remains a valid complete trace",
                   lowTraceBuilt == PagerProtocolError::None, lowTraceParsed == PagerProtocolError::None,
                   NotificationIsEqual(parsed, boundary));

    // A user exception travels as its own cause.
    boundary.Fault.Access = FaultAccess::Execute;
    boundary.Fault.Cause = FaultCause::Exception;
    const PagerProtocolError exceptionBuilt = PagerProtocol::BuildFaultNotification(boundary, message);
    const PagerProtocolError exceptionParsed = PagerProtocol::ParseFaultNotification(message, parsed);
    ASTERIA_EXPECT(runner, "an exception's cause round-trips", exceptionBuilt == PagerProtocolError::None,
                   exceptionParsed == PagerProtocolError::None, NotificationIsEqual(parsed, boundary));
}
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"PagerProtocolTests"};
    Asteria::Test::TestCanonicalRoundTrip(runner);
    Asteria::Test::TestBuildFailures(runner);
    Asteria::Test::TestHeaderAndShapeRejection(runner);
    Asteria::Test::TestCanonicalPayloadAndAttachmentRejection(runner);
    Asteria::Test::TestBoundaryValues(runner);
    return runner.Finish();
}
