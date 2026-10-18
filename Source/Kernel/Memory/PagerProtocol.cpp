// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/PagerProtocol.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::Size VirtualAddressOffset{0U};
constexpr Foundation::Size InstructionPointerOffset{8U};
constexpr Foundation::Size ArchitectureSyndromeOffset{16U};
constexpr Foundation::Size AccessOffset{24U};
constexpr Foundation::Size CauseOffset{25U};
constexpr Foundation::Size PrivilegeOffset{26U};
constexpr Foundation::Size FaultReservedOffset{27U};
constexpr Foundation::Size SupervisionTagOffset{PagerProtocol::FaultRecordSizeBytes};
constexpr Foundation::Size ProtectionDomainIdentityOffset{SupervisionTagOffset + 8U};

[[nodiscard]] constexpr Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

constexpr void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] constexpr bool TraceIsValid(const Ipc::TraceContext& trace) noexcept
{
    const bool hasTraceIdentifier = (trace.TraceIdentifierHigh != 0U) || (trace.TraceIdentifierLow != 0U);
    return hasTraceIdentifier == (trace.SpanIdentifier != 0U);
}

[[nodiscard]] constexpr bool FaultAccessIsKnown(const FaultAccess access) noexcept
{
    return (access == FaultAccess::Read) || (access == FaultAccess::Write) || (access == FaultAccess::Execute);
}

[[nodiscard]] constexpr bool FaultCauseIsKnown(const FaultCause cause) noexcept
{
    return (cause == FaultCause::MissingMapping) || (cause == FaultCause::ProtectionViolation) ||
           (cause == FaultCause::InvalidMappingEncoding) || (cause == FaultCause::Exception);
}

[[nodiscard]] constexpr bool FaultPrivilegeIsKnown(const FaultPrivilege privilege) noexcept
{
    return (privilege == FaultPrivilege::User) || (privilege == FaultPrivilege::Supervisor);
}

[[nodiscard]] bool FaultRecordIsValid(const FaultRecord& fault) noexcept
{
    if (!FaultAccessIsKnown(fault.Access) || !FaultCauseIsKnown(fault.Cause) || !FaultPrivilegeIsKnown(fault.Privilege))
    {
        return false;
    }
    for (const Foundation::UInt8 value : fault.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool InterfaceIsPager(const Ipc::InterfaceIdentifier interface) noexcept
{
    return (interface.GetLow() == PagerProtocol::Interface.GetLow()) &&
           (interface.GetHigh() == PagerProtocol::Interface.GetHigh());
}

[[nodiscard]] constexpr Object::CapabilityRights FaultTokenRights() noexcept
{
    return Object::CapabilityRights{Object::CapabilityRight::Invoke};
}

[[nodiscard]] bool VariableDescriptorsAreZero(const Ipc::MessageEnvelope& message) noexcept
{
    for (const Ipc::VariablePayloadDescriptor& descriptor : message.VariablePayloads)
    {
        if ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AttachmentIsZero(const Ipc::CapabilityAttachment& attachment) noexcept
{
    return !attachment.Handle.IsValid() && (attachment.Handle.GetAuthenticator() == 0U) &&
           (attachment.RequestedDeadline.GetTicks() == 0U) && (attachment.RequestedRights.GetRawValue() == 0U) &&
           (attachment.ExpectedType == Object::ObjectType::Invalid) &&
           (attachment.TransferMode == Ipc::CapabilityTransferMode::Invalid) && (attachment.Reserved == 0U);
}

[[nodiscard]] bool UnusedAttachmentsAreZero(const Ipc::MessageEnvelope& message) noexcept
{
    for (Foundation::Size index{1U}; index < Ipc::MessageEnvelope::MaximumCapabilityAttachments; ++index)
    {
        if (!AttachmentIsZero(message.Attachments[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool UnusedPayloadIsZero(const Ipc::MessageEnvelope& message) noexcept
{
    for (Foundation::Size index{PagerProtocol::FaultPayloadSizeBytes};
         index < Ipc::MessageEnvelope::MaximumPayloadBytes; ++index)
    {
        if (message.Payload[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

void ClearMessageEnvelope(Ipc::MessageEnvelope& message) noexcept
{
    message.Interface = Ipc::InterfaceIdentifier{};
    message.TransactionIdentifier = 0U;
    message.Deadline = Object::MonotonicTime{};
    message.Trace = Ipc::TraceContext{};
    message.MethodIdentifier = 0U;
    message.InterfaceVersionMajor = 0U;
    message.InterfaceVersionMinor = 0U;
    message.FixedPayloadSizeBytes = 0U;
    message.PayloadSizeBytes = 0U;
    message.Kind = Ipc::MessageKind::Invalid;
    message.Cancellation = Ipc::CancellationState::None;
    message.VariablePayloadCount = 0U;
    message.AttachmentCount = 0U;
    message.Reserved = 0U;
    message.ReservedAlignment = 0U;
    for (Ipc::CapabilityAttachment& attachment : message.Attachments)
    {
        attachment = Ipc::CapabilityAttachment{};
    }
    for (Ipc::VariablePayloadDescriptor& descriptor : message.VariablePayloads)
    {
        descriptor = Ipc::VariablePayloadDescriptor{};
    }
    for (Foundation::UInt8& value : message.Payload)
    {
        value = 0U;
    }
}

void EncodeFault(const FaultRecord& fault, Foundation::UInt8* const payload) noexcept
{
    WriteLittleEndian64(payload + VirtualAddressOffset, fault.VirtualAddress);
    WriteLittleEndian64(payload + InstructionPointerOffset, fault.InstructionPointer);
    WriteLittleEndian64(payload + ArchitectureSyndromeOffset, fault.ArchitectureSyndrome);
    payload[AccessOffset] = static_cast<Foundation::UInt8>(fault.Access);
    payload[CauseOffset] = static_cast<Foundation::UInt8>(fault.Cause);
    payload[PrivilegeOffset] = static_cast<Foundation::UInt8>(fault.Privilege);
}

[[nodiscard]] FaultRecord DecodeFault(const Foundation::UInt8* const payload) noexcept
{
    return FaultRecord{
        .VirtualAddress = ReadLittleEndian64(payload + VirtualAddressOffset),
        .InstructionPointer = ReadLittleEndian64(payload + InstructionPointerOffset),
        .ArchitectureSyndrome = ReadLittleEndian64(payload + ArchitectureSyndromeOffset),
        .Access = static_cast<FaultAccess>(payload[AccessOffset]),
        .Cause = static_cast<FaultCause>(payload[CauseOffset]),
        .Privilege = static_cast<FaultPrivilege>(payload[PrivilegeOffset]),
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace

PagerProtocolError PagerProtocol::BuildFaultNotification(const PagerFaultNotification& notification,
                                                         Ipc::MessageEnvelope& output) noexcept
{
    if (notification.TransactionIdentifier == 0U)
    {
        return PagerProtocolError::InvalidTransactionIdentifier;
    }
    if (!TraceIsValid(notification.Trace))
    {
        return PagerProtocolError::InvalidTraceContext;
    }
    if (!FaultRecordIsValid(notification.Fault))
    {
        return PagerProtocolError::InvalidFaultRecord;
    }
    if (!notification.FaultToken.IsValid())
    {
        return PagerProtocolError::InvalidFaultToken;
    }

    ClearMessageEnvelope(output);
    output.Interface = Interface;
    output.TransactionIdentifier = notification.TransactionIdentifier;
    output.Deadline = notification.Deadline;
    output.Trace = notification.Trace;
    output.MethodIdentifier = FaultMethodIdentifier;
    output.InterfaceVersionMajor = VersionMajor;
    output.InterfaceVersionMinor = VersionMinor;
    output.FixedPayloadSizeBytes = FaultPayloadSizeBytes;
    output.PayloadSizeBytes = FaultPayloadSizeBytes;
    output.Kind = Ipc::MessageKind::Asynchronous;
    output.Cancellation = Ipc::CancellationState::None;
    output.AttachmentCount = 1U;
    output.Attachments[0U] = Ipc::CapabilityAttachment{
        .Handle = notification.FaultToken,
        .RequestedDeadline = notification.Deadline,
        .RequestedRights = FaultTokenRights(),
        .ExpectedType = Object::ObjectType::FaultToken,
        .TransferMode = Ipc::CapabilityTransferMode::Move,
        .Reserved = 0U,
    };
    EncodeFault(notification.Fault, output.Payload);
    WriteLittleEndian64(output.Payload + SupervisionTagOffset, notification.SupervisionTag);
    WriteLittleEndian64(output.Payload + ProtectionDomainIdentityOffset, notification.ProtectionDomainIdentity);

    return PagerProtocolError::None;
}

PagerProtocolError PagerProtocol::ParseFaultNotification(const Ipc::MessageEnvelope& message,
                                                         PagerFaultNotification& output) noexcept
{
    if (!InterfaceIsPager(message.Interface))
    {
        return PagerProtocolError::InvalidInterface;
    }
    if ((message.InterfaceVersionMajor != VersionMajor) || (message.InterfaceVersionMinor != VersionMinor))
    {
        return PagerProtocolError::UnsupportedVersion;
    }
    if (message.MethodIdentifier != FaultMethodIdentifier)
    {
        return PagerProtocolError::InvalidMethod;
    }
    if (message.TransactionIdentifier == 0U)
    {
        return PagerProtocolError::InvalidTransactionIdentifier;
    }
    if (!TraceIsValid(message.Trace))
    {
        return PagerProtocolError::InvalidTraceContext;
    }
    if ((message.Kind != Ipc::MessageKind::Asynchronous) || (message.Cancellation != Ipc::CancellationState::None) ||
        (message.FixedPayloadSizeBytes != FaultPayloadSizeBytes) ||
        (message.PayloadSizeBytes != FaultPayloadSizeBytes) || (message.VariablePayloadCount != 0U) ||
        (message.AttachmentCount != 1U))
    {
        return PagerProtocolError::InvalidMessageContract;
    }
    if ((message.Reserved != 0U) || (message.ReservedAlignment != 0U))
    {
        return PagerProtocolError::NoncanonicalMessage;
    }
    if (!VariableDescriptorsAreZero(message) || !UnusedAttachmentsAreZero(message) || !UnusedPayloadIsZero(message))
    {
        return PagerProtocolError::NoncanonicalMessage;
    }

    for (Foundation::Size index{FaultReservedOffset}; index < FaultRecordSizeBytes; ++index)
    {
        if (message.Payload[index] != 0U)
        {
            return PagerProtocolError::InvalidPayload;
        }
    }
    const FaultRecord fault = DecodeFault(message.Payload);
    if (!FaultRecordIsValid(fault))
    {
        return PagerProtocolError::InvalidPayload;
    }

    const Ipc::CapabilityAttachment& attachment = message.Attachments[0U];
    if (!attachment.Handle.IsValid() || (attachment.RequestedDeadline.GetTicks() != message.Deadline.GetTicks()) ||
        !attachment.RequestedRights.IsEqualTo(FaultTokenRights()) ||
        (attachment.ExpectedType != Object::ObjectType::FaultToken) ||
        (attachment.TransferMode != Ipc::CapabilityTransferMode::Move) || (attachment.Reserved != 0U))
    {
        return PagerProtocolError::InvalidAttachment;
    }

    const PagerFaultNotification candidate{
        .Fault = fault,
        .SupervisionTag = ReadLittleEndian64(message.Payload + SupervisionTagOffset),
        .ProtectionDomainIdentity = ReadLittleEndian64(message.Payload + ProtectionDomainIdentityOffset),
        .Trace = message.Trace,
        .FaultToken = attachment.Handle,
        .Deadline = message.Deadline,
        .TransactionIdentifier = message.TransactionIdentifier,
    };
    output = candidate;
    return PagerProtocolError::None;
}
} // namespace Asteria::Kernel::Memory
