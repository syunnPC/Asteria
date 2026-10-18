// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/Endpoint.hpp"

namespace Asteria::Kernel::Ipc
{
namespace
{
constexpr Foundation::Size AttachmentSelectorOffset{0U};
constexpr Foundation::Size AttachmentAuthenticatorOffset{8U};

void WriteLittleEndian64(Foundation::UInt8* const destination, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(value); ++index)
    {
        destination[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}
} // namespace

Endpoint::Endpoint(Object::CapabilitySpace& receiverSpace, const Object::ResourceDomainId chargeOwner,
                   const EndpointConfiguration configuration) noexcept
    : m_receiverSpace{receiverSpace}, m_chargeOwner{chargeOwner}, m_configuration{configuration},
      m_isConfigurationValid{receiverSpace.IsReady() && chargeOwner.IsValid() && IsConfigurationValid(configuration)}
{
}

Endpoint::~Endpoint() noexcept
{
    for (Foundation::Size entryIndex{0U}; entryIndex < MaximumQueueEntries; ++entryIndex)
    {
        ClearQueueEntry(m_entries[entryIndex]);
    }
    ClearStagingFrame();
    ClearReceiveStagingMessage();
    ClearPendingCloseIndices();
    ClearPreparedSendLocked();
    m_activeReceiveReservationIdentifier = 0U;
    m_reservedReceiveSequence = 0U;
    m_receiveInvocation = Object::CapabilityHandle{};
}

Object::ObjectType Endpoint::GetObjectType() const noexcept
{
    return Object::ObjectType::Endpoint;
}

Object::ResourceDomainId Endpoint::GetChargeOwner() const noexcept
{
    return m_chargeOwner;
}

Foundation::Result<EndpointSendReceipt, EndpointError> Endpoint::Send(Object::CapabilitySpace& sourceSpace,
                                                                      const Object::ResourceDomainId sender,
                                                                      const EndpointSendClass sendClass,
                                                                      const MessageEnvelope& message) noexcept
{
    using ResultType = Foundation::Result<EndpointSendReceipt, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }
    if (!sender.IsValid() || !IsKnownSendClass(sendClass))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidArgument));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    const EndpointError staged = StageSendLocked(sourceSpace, sender, sendClass, SendAuthority{}, message,
                                                 Object::CapabilitySpace::MovedSourceCommit::Release);
    if (staged.Code != EndpointErrorCode::None)
    {
        return ResultType::Failure(staged);
    }
    return ResultType::Success(PublishStagedSendLocked());
}

Foundation::Result<EndpointSendReceipt, EndpointError>
Endpoint::SendFromInvocation(Object::CapabilitySpace& callerSpace, const Object::CapabilityHandle invocation,
                             const Object::ResourceDomainId sender, const EndpointSendClass sendClass,
                             const MessageEnvelope& message) noexcept
{
    using ResultType = Foundation::Result<EndpointSendReceipt, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }
    if (!sender.IsValid() || !IsKnownSendClass(sendClass))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidArgument));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    SendAuthority authority{};
    const EndpointError authorized = AuthorizeSendInvocationLocked(callerSpace, invocation, authority);
    if (authorized.Code != EndpointErrorCode::None)
    {
        return ResultType::Failure(authorized);
    }
    const EndpointError staged = StageSendLocked(callerSpace, sender, sendClass, authority, message,
                                                 Object::CapabilitySpace::MovedSourceCommit::Release);
    if (staged.Code != EndpointErrorCode::None)
    {
        return ResultType::Failure(staged);
    }
    return ResultType::Success(PublishStagedSendLocked());
}

Foundation::Result<EndpointSendReservation, EndpointError>
Endpoint::PrepareSend(Object::CapabilitySpace& sourceSpace, const Object::ResourceDomainId sender,
                      const EndpointSendClass sendClass, const MessageEnvelope& message) noexcept
{
    using ResultType = Foundation::Result<EndpointSendReservation, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }
    if (!sender.IsValid() || !IsKnownSendClass(sendClass))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidArgument));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    if (m_nextReservationIdentifier == 0U)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::SequenceExhausted));
    }

    const EndpointError staged = StageSendLocked(sourceSpace, sender, sendClass, SendAuthority{}, message,
                                                 Object::CapabilitySpace::MovedSourceCommit::Hold);
    if (staged.Code != EndpointErrorCode::None)
    {
        return ResultType::Failure(staged);
    }

    m_preparedSourceSpace = &sourceSpace;
    m_preparedHeldCount = 0U;
    for (Foundation::Size index{0U}; index < message.AttachmentCount; ++index)
    {
        if (message.Attachments[index].TransferMode == CapabilityTransferMode::Move)
        {
            m_preparedHeldSources[m_preparedHeldCount] = message.Attachments[index].Handle;
            ++m_preparedHeldCount;
        }
    }

    const Foundation::UInt64 identifier = m_nextReservationIdentifier;
    if (m_nextReservationIdentifier == ~Foundation::UInt64{0U})
    {
        m_nextReservationIdentifier = 0U;
    }
    else
    {
        ++m_nextReservationIdentifier;
    }
    m_activeReservationIdentifier = identifier;
    return ResultType::Success(EndpointSendReservation{*this, identifier});
}

Foundation::Result<EndpointSendReceipt, EndpointError>
Endpoint::CommitPreparedSend(const EndpointSendReservation reservation) noexcept
{
    using ResultType = Foundation::Result<EndpointSendReceipt, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }
    if (!reservation.IsValid() || (reservation.m_endpoint != this))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidReservation));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    if ((m_activeReservationIdentifier == 0U) || (m_activeReservationIdentifier != reservation.m_identifier))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::StaleReservation));
    }
    Foundation::Size heldIndices[MessageEnvelope::MaximumCapabilityAttachments]{};
    if (!PreparedEntryIsValidLocked() || !ResolveHeldSourcesLocked(heldIndices))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::QueueCorrupt));
    }

    for (Foundation::Size index{0U}; index < m_preparedHeldCount; ++index)
    {
        m_preparedSourceSpace->ReleaseHeldLocked(heldIndices[index]);
    }
    const EndpointSendReceipt receipt = PublishStagedSendLocked();
    ClearPreparedSendLocked();
    return ResultType::Success(receipt);
}

EndpointError Endpoint::CancelPreparedSend(const EndpointSendReservation reservation) noexcept
{
    if (!m_isConfigurationValid)
    {
        return MakeError(EndpointErrorCode::InvalidConfiguration);
    }
    if (!reservation.IsValid() || (reservation.m_endpoint != this))
    {
        return MakeError(EndpointErrorCode::InvalidReservation);
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    if ((m_activeReservationIdentifier == 0U) || (m_activeReservationIdentifier != reservation.m_identifier))
    {
        return MakeError(EndpointErrorCode::StaleReservation);
    }
    return CancelPreparedSendLocked();
}

EndpointError Endpoint::MakeInvocationError(const Object::CapabilityError capabilityError) noexcept
{
    return MakeError(EndpointErrorCode::InvocationRejected, MessageCodecError::None, capabilityError);
}

EndpointError Endpoint::AuthorizeSendInvocationLocked(Object::CapabilitySpace& callerSpace,
                                                      const Object::CapabilityHandle invocation,
                                                      SendAuthority& authority) noexcept
{
    if (!callerSpace.SharesTransferDomain(m_receiverSpace))
    {
        return MakeInvocationError(Object::CapabilityError::InvalidConfiguration);
    }
    const Foundation::Result<Object::MonotonicTime, Object::CapabilityError> now =
        callerSpace.SampleTransferTimeLocked(m_receiverSpace);
    if (!now.HasValue())
    {
        return MakeInvocationError(now.GetError());
    }
    const Foundation::Result<Object::CapabilityDescriptor, Object::CapabilityError> authorized =
        callerSpace.AuthorizeInvocationLocked(invocation, Object::ObjectType::Endpoint,
                                              Object::CapabilityOperation::Invoke, *this, now.GetValue());
    if (!authorized.HasValue())
    {
        return MakeInvocationError(authorized.GetError());
    }
    authority = SendAuthority{
        .Now = now.GetValue(),
        .SenderEndpointSealedObjectSelector = authorized.GetValue().SealedObjectSelector,
        .SenderEndpointRights = authorized.GetValue().Rights,
        .IsTimeSampled = true,
    };
    return MakeError(EndpointErrorCode::None);
}

EndpointError Endpoint::AuthorizeReceiveInvocationLocked(const Object::CapabilityHandle invocation) noexcept
{
    const Foundation::Result<Object::MonotonicTime, Object::CapabilityError> now =
        m_receiverSpace.GetCurrentTimeLocked();
    if (!now.HasValue())
    {
        return MakeInvocationError(now.GetError());
    }
    const Foundation::Result<Object::CapabilityDescriptor, Object::CapabilityError> authorized =
        m_receiverSpace.AuthorizeInvocationLocked(
            invocation,
            m_receiveAuthority != nullptr ? m_receiveAuthority->GetObjectType() : Object::ObjectType::Endpoint,
            Object::CapabilityOperation::Read, m_receiveAuthority != nullptr ? *m_receiveAuthority : *this,
            now.GetValue());
    if (!authorized.HasValue())
    {
        return MakeInvocationError(authorized.GetError());
    }
    return MakeError(EndpointErrorCode::None);
}

EndpointError Endpoint::StageSendLocked(Object::CapabilitySpace& sourceSpace, const Object::ResourceDomainId sender,
                                        const EndpointSendClass sendClass, const SendAuthority& authority,
                                        const MessageEnvelope& message,
                                        const Object::CapabilitySpace::MovedSourceCommit movedSourceCommit) noexcept
{
    if (m_activeReservationIdentifier != 0U)
    {
        return MakeError(EndpointErrorCode::ReservationActive);
    }
    if (m_nextSequence == 0U)
    {
        return MakeError(EndpointErrorCode::SequenceExhausted);
    }

    const Foundation::Result<Foundation::Size, MessageCodecError> encoded =
        MessageCodec::Encode(message, WritableByteView{
                                          .Data = m_stagingFrame,
                                          .SizeBytes = sizeof(m_stagingFrame),
                                      });
    if (!encoded.HasValue())
    {
        return MakeError(EndpointErrorCode::InvalidMessage, encoded.GetError());
    }

    const Foundation::Size frameSize = encoded.GetValue();
    const Foundation::Size attachmentCount = message.AttachmentCount;
    const bool normal = sendClass == EndpointSendClass::Normal;
    const Foundation::Size messageLimit = normal
                                              ? static_cast<Foundation::Size>(m_configuration.MaximumQueuedMessages) -
                                                    m_configuration.ReservedControlMessages
                                              : m_configuration.MaximumQueuedMessages;
    const Foundation::Size byteLimit = normal ? static_cast<Foundation::Size>(m_configuration.MaximumQueuedBytes) -
                                                    m_configuration.ReservedControlBytes
                                              : m_configuration.MaximumQueuedBytes;
    const Foundation::Size attachmentLimit =
        normal ? static_cast<Foundation::Size>(m_configuration.MaximumQueuedAttachments) -
                     m_configuration.ReservedControlAttachments
               : m_configuration.MaximumQueuedAttachments;

    bool quotaExceeded = (m_queuedMessages >= messageLimit) || (m_queuedBytes > byteLimit) ||
                         (frameSize > (byteLimit - m_queuedBytes)) || (m_queuedAttachments > attachmentLimit) ||
                         (attachmentCount > (attachmentLimit - m_queuedAttachments));
    if (normal && !quotaExceeded)
    {
        const SenderUsage usage = GetSenderUsageLocked(sender);
        quotaExceeded =
            (usage.Messages >= m_configuration.MaximumMessagesPerSender) ||
            (usage.Bytes > m_configuration.MaximumBytesPerSender) ||
            (frameSize > (static_cast<Foundation::Size>(m_configuration.MaximumBytesPerSender) - usage.Bytes)) ||
            (usage.Attachments > m_configuration.MaximumAttachmentsPerSender) ||
            (attachmentCount >
             (static_cast<Foundation::Size>(m_configuration.MaximumAttachmentsPerSender) - usage.Attachments));
    }
    if (quotaExceeded)
    {
        ClearStagingFrame();
        return MakeError(EndpointErrorCode::WouldBlock);
    }

    Object::CapabilityTransferBatch transferred{};
    const EndpointError transferError =
        TransferAttachmentsLocked(sourceSpace, authority, message, movedSourceCommit, transferred);
    if (transferError.Code != EndpointErrorCode::None)
    {
        ClearStagingFrame();
        return transferError;
    }

    QueueEntry& entry = m_entries[m_tail];
    ClearQueueEntry(entry);
    CopyFrame(entry.Frame, m_stagingFrame);
    entry.Sequence = m_nextSequence;
    entry.Sender = sender;
    entry.SenderEndpointSealedObjectSelector = authority.SenderEndpointSealedObjectSelector;
    entry.SenderEndpointRights = authority.SenderEndpointRights;
    entry.FrameSizeBytes = static_cast<Foundation::UInt16>(frameSize);
    entry.AttachmentCount = static_cast<Foundation::UInt8>(attachmentCount);
    entry.SendClass = sendClass;
    for (Foundation::Size index{0U}; index < attachmentCount; ++index)
    {
        entry.PendingHandles[index] = transferred.DestinationHandles[index];
    }
    PatchTransferredHandles(entry, message.VariablePayloadCount, transferred);

    ClearStagingFrame();
    return MakeError(EndpointErrorCode::None);
}

EndpointError Endpoint::TransferAttachmentsLocked(Object::CapabilitySpace& sourceSpace, const SendAuthority& authority,
                                                  const MessageEnvelope& message,
                                                  const Object::CapabilitySpace::MovedSourceCommit movedSourceCommit,
                                                  Object::CapabilityTransferBatch& transferred) noexcept
{
    const Foundation::Size attachmentCount = message.AttachmentCount;
    if ((attachmentCount == 0U) && message.Deadline.IsInfinite())
    {
        return MakeError(EndpointErrorCode::None);
    }
    if ((attachmentCount != 0U) && !sourceSpace.SharesTransferDomain(m_receiverSpace))
    {
        return MakeError(EndpointErrorCode::CapabilityTransferFailed, MessageCodecError::None,
                         Object::CapabilityError::InvalidConfiguration);
    }

    // An attachment-free message touches only the receiver space; one clock
    // sample governs the invocation, the message deadline, and every lease.
    Object::MonotonicTime now = authority.Now;
    if (!authority.IsTimeSampled)
    {
        const Foundation::Result<Object::MonotonicTime, Object::CapabilityError> sampled =
            (attachmentCount == 0U) ? m_receiverSpace.GetCurrentTimeLocked()
                                    : sourceSpace.SampleTransferTimeLocked(m_receiverSpace);
        if (!sampled.HasValue())
        {
            return MakeError(EndpointErrorCode::CapabilityTransferFailed, MessageCodecError::None, sampled.GetError());
        }
        now = sampled.GetValue();
    }
    if (!message.Deadline.IsInfinite() && (now.GetTicks() >= message.Deadline.GetTicks()))
    {
        return MakeError(EndpointErrorCode::CapabilityTransferFailed, MessageCodecError::None,
                         Object::CapabilityError::Expired);
    }
    if (attachmentCount == 0U)
    {
        return MakeError(EndpointErrorCode::None);
    }

    Object::CapabilityTransferRequest transferRequests[MessageEnvelope::MaximumCapabilityAttachments]{};
    for (Foundation::Size index{0U}; index < attachmentCount; ++index)
    {
        const CapabilityAttachment& attachment = message.Attachments[index];
        transferRequests[index] = Object::CapabilityTransferRequest{
            .Source = attachment.Handle,
            .RequestedDeadline = attachment.RequestedDeadline,
            .RequestedRights = attachment.RequestedRights,
            .ExpectedType = attachment.ExpectedType,
            .Disposition = attachment.TransferMode == CapabilityTransferMode::Move
                               ? Object::CapabilityTransferDisposition::Move
                               : Object::CapabilityTransferDisposition::Copy,
            .Reserved = 0U,
        };
    }

    const auto result = sourceSpace.TransferBatchToLocked(m_receiverSpace,
                                                          Object::CapabilityTransferRequestView{
                                                              .Data = transferRequests,
                                                              .Count = attachmentCount,
                                                          },
                                                          now, movedSourceCommit);
    if (!result.HasValue())
    {
        return MakeError(EndpointErrorCode::CapabilityTransferFailed, MessageCodecError::None, result.GetError());
    }
    transferred = result.GetValue();
    return MakeError(EndpointErrorCode::None);
}

EndpointSendReceipt Endpoint::PublishStagedSendLocked() noexcept
{
    QueueEntry& entry = m_entries[m_tail];

    const EndpointSendReceipt receipt{.Sequence = m_nextSequence};
    if (m_nextSequence == ~Foundation::UInt64{0U})
    {
        m_nextSequence = 0U;
    }
    else
    {
        ++m_nextSequence;
    }
    ++m_queuedMessages;
    m_queuedBytes += entry.FrameSizeBytes;
    m_queuedAttachments += entry.AttachmentCount;
    m_tail = AdvanceQueueIndex(m_tail);
    SignalReadinessLocked();
    return receipt;
}

bool Endpoint::PreparedEntryIsValidLocked() const noexcept
{
    if ((m_activeReservationIdentifier == 0U) || (m_tail >= m_configuration.MaximumQueuedMessages) ||
        (m_nextSequence == 0U))
    {
        return false;
    }

    const QueueEntry& entry = m_entries[m_tail];
    return (entry.Sequence == m_nextSequence) && entry.Sender.IsValid() && entry.SenderEndpointRights.IsValid() &&
           IsKnownSendClass(entry.SendClass) && (entry.FrameSizeBytes >= MessageCodec::WireHeaderSizeBytes) &&
           (entry.FrameSizeBytes <= MessageCodec::MaximumWireSizeBytes) &&
           (entry.AttachmentCount <= MessageEnvelope::MaximumCapabilityAttachments) &&
           (m_queuedMessages < m_configuration.MaximumQueuedMessages) &&
           (m_queuedBytes <= m_configuration.MaximumQueuedBytes) &&
           (entry.FrameSizeBytes <= (m_configuration.MaximumQueuedBytes - m_queuedBytes)) &&
           (m_queuedAttachments <= m_configuration.MaximumQueuedAttachments) &&
           (entry.AttachmentCount <= (m_configuration.MaximumQueuedAttachments - m_queuedAttachments)) &&
           (m_preparedHeldCount <= entry.AttachmentCount) &&
           ((m_preparedHeldCount == 0U) || (m_preparedSourceSpace != nullptr));
}

bool Endpoint::ResolveHeldSourcesLocked(
    Foundation::Size (&indices)[MessageEnvelope::MaximumCapabilityAttachments]) const noexcept
{
    for (Foundation::Size held{0U}; held < m_preparedHeldCount; ++held)
    {
        const auto indexResult = m_preparedSourceSpace->ResolveHeldHandleLocked(m_preparedHeldSources[held]);
        if (!indexResult.HasValue())
        {
            return false;
        }
        for (Foundation::Size previous{0U}; previous < held; ++previous)
        {
            if (indices[previous] == indexResult.GetValue())
            {
                return false;
            }
        }
        indices[held] = indexResult.GetValue();
    }
    return true;
}

void Endpoint::ClearPreparedSendLocked() noexcept
{
    m_activeReservationIdentifier = 0U;
    m_preparedSourceSpace = nullptr;
    for (Object::CapabilityHandle& handle : m_preparedHeldSources)
    {
        handle = Object::CapabilityHandle{};
    }
    m_preparedHeldCount = 0U;
}

EndpointError Endpoint::CancelPreparedSendLocked() noexcept
{
    Foundation::Size heldIndices[MessageEnvelope::MaximumCapabilityAttachments]{};
    if (!PreparedEntryIsValidLocked() || !ResolveHeldSourcesLocked(heldIndices))
    {
        return MakeError(EndpointErrorCode::QueueCorrupt);
    }

    QueueEntry& entry = m_entries[m_tail];
    ClearPendingCloseIndices();
    for (Foundation::Size attachment{0U}; attachment < entry.AttachmentCount; ++attachment)
    {
        const auto indexResult = m_receiverSpace.ResolveHandleLocked(entry.PendingHandles[attachment]);
        if (!indexResult.HasValue())
        {
            ClearPendingCloseIndices();
            return MakeError(EndpointErrorCode::QueueCorrupt, MessageCodecError::None, indexResult.GetError());
        }
        for (Foundation::Size previous{0U}; previous < attachment; ++previous)
        {
            if (m_pendingCloseIndices[previous] == indexResult.GetValue())
            {
                ClearPendingCloseIndices();
                return MakeError(EndpointErrorCode::QueueCorrupt);
            }
        }
        m_pendingCloseIndices[attachment] = indexResult.GetValue();
    }

    for (Foundation::Size attachment{0U}; attachment < entry.AttachmentCount; ++attachment)
    {
        m_receiverSpace.CloseIndexLocked(m_pendingCloseIndices[attachment]);
    }
    for (Foundation::Size held{0U}; held < m_preparedHeldCount; ++held)
    {
        m_preparedSourceSpace->RestoreHeldLocked(heldIndices[held]);
    }
    ClearPendingCloseIndices();
    ClearQueueEntry(entry);
    ClearPreparedSendLocked();
    return MakeError(EndpointErrorCode::None);
}

bool Endpoint::QueueHeadIsValidLocked() const noexcept
{
    if ((m_queuedMessages == 0U) || (m_queuedMessages > m_configuration.MaximumQueuedMessages) ||
        (m_head >= m_configuration.MaximumQueuedMessages) || (m_queuedBytes > m_configuration.MaximumQueuedBytes) ||
        (m_queuedAttachments > m_configuration.MaximumQueuedAttachments))
    {
        return false;
    }

    const QueueEntry& entry = m_entries[m_head];
    return (entry.Sequence != 0U) && entry.Sender.IsValid() && entry.SenderEndpointRights.IsValid() &&
           IsKnownSendClass(entry.SendClass) && (entry.FrameSizeBytes >= MessageCodec::WireHeaderSizeBytes) &&
           (entry.FrameSizeBytes <= MessageCodec::MaximumWireSizeBytes) &&
           (entry.AttachmentCount <= MessageEnvelope::MaximumCapabilityAttachments) &&
           (m_queuedBytes >= entry.FrameSizeBytes) && (m_queuedAttachments >= entry.AttachmentCount);
}

bool Endpoint::PreparedReceiveIsValidLocked() const noexcept
{
    return (m_activeReceiveReservationIdentifier != 0U) && (m_reservedReceiveSequence != 0U) &&
           QueueHeadIsValidLocked() && (m_entries[m_head].Sequence == m_reservedReceiveSequence);
}

EndpointError Endpoint::DecodeHeadLocked(MessageEnvelope& output, EndpointReceiveMetadata& metadata) noexcept
{
    if (m_head >= m_configuration.MaximumQueuedMessages)
    {
        return MakeError(EndpointErrorCode::QueueCorrupt);
    }
    if (m_queuedMessages == 0U)
    {
        return MakeError(EndpointErrorCode::WouldBlock);
    }
    if (!QueueHeadIsValidLocked())
    {
        return MakeError(EndpointErrorCode::QueueCorrupt);
    }

    const QueueEntry& entry = m_entries[m_head];
    ClearReceiveStagingMessage();
    const MessageCodecError decodeError = MessageCodec::Decode(
        ReadOnlyByteView{
            .Data = entry.Frame,
            .SizeBytes = entry.FrameSizeBytes,
        },
        m_receiveStagingMessage);
    if (decodeError != MessageCodecError::None)
    {
        ClearReceiveStagingMessage();
        return MakeError(EndpointErrorCode::QueueCorrupt, decodeError);
    }
    bool metadataMatches = m_receiveStagingMessage.AttachmentCount == entry.AttachmentCount;
    for (Foundation::Size index{0U}; index < entry.AttachmentCount; ++index)
    {
        metadataMatches = metadataMatches &&
                          (m_receiveStagingMessage.Attachments[index].Handle.GetSelector() ==
                           entry.PendingHandles[index].GetSelector()) &&
                          (m_receiveStagingMessage.Attachments[index].Handle.GetAuthenticator() ==
                           entry.PendingHandles[index].GetAuthenticator());
    }
    if (!metadataMatches)
    {
        ClearReceiveStagingMessage();
        return MakeError(EndpointErrorCode::QueueCorrupt);
    }

    const EndpointReceiveMetadata decodedMetadata{
        .Sequence = entry.Sequence,
        .Sender = entry.Sender,
        .SenderEndpointSealedObjectSelector = entry.SenderEndpointSealedObjectSelector,
        .SenderEndpointRights = entry.SenderEndpointRights,
        .FrameSizeBytes = entry.FrameSizeBytes,
        .AttachmentCount = entry.AttachmentCount,
        .SendClass = entry.SendClass,
    };
    output = m_receiveStagingMessage;
    metadata = decodedMetadata;
    ClearReceiveStagingMessage();
    return MakeError(EndpointErrorCode::None);
}

void Endpoint::ConsumeHeadLocked() noexcept
{
    QueueEntry& entry = m_entries[m_head];
    --m_queuedMessages;
    m_queuedBytes -= entry.FrameSizeBytes;
    m_queuedAttachments -= entry.AttachmentCount;
    ClearQueueEntry(entry);
    m_head = AdvanceQueueIndex(m_head);
    // A waiter claims the signal before it receives; while messages remain
    // the next wait must complete too.
    if (m_queuedMessages != 0U)
    {
        SignalReadinessLocked();
    }
}

void Endpoint::SignalReadinessLocked() noexcept
{
    if (m_readiness != nullptr)
    {
        static_cast<void>(m_readiness->Signal());
    }
}

void Endpoint::AttachReadiness(Notification& readiness, const NotificationId identity) noexcept
{
    m_readiness = &readiness;
    m_readinessId = identity;
}

Foundation::Result<EndpointReceiveMetadata, EndpointError> Endpoint::Receive(MessageEnvelope& output) noexcept
{
    using ResultType = Foundation::Result<EndpointReceiveMetadata, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    if (m_activeReceiveReservationIdentifier != 0U)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::ReservationActive));
    }
    EndpointReceiveMetadata metadata{};
    const EndpointError decoded = DecodeHeadLocked(output, metadata);
    if (decoded.Code != EndpointErrorCode::None)
    {
        return ResultType::Failure(decoded);
    }
    ConsumeHeadLocked();
    return ResultType::Success(metadata);
}

Foundation::Result<EndpointReceivePreparation, EndpointError> Endpoint::PrepareReceive(MessageEnvelope& output) noexcept
{
    using ResultType = Foundation::Result<EndpointReceivePreparation, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    return PrepareReceiveLocked(output);
}

Foundation::Result<EndpointReceivePreparation, EndpointError>
Endpoint::PrepareReceiveFromInvocation(Object::CapabilitySpace& callerSpace, const Object::CapabilityHandle invocation,
                                       MessageEnvelope& output) noexcept
{
    using ResultType = Foundation::Result<EndpointReceivePreparation, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }
    if (&callerSpace != &m_receiverSpace)
    {
        return ResultType::Failure(MakeInvocationError(Object::CapabilityError::RightsDenied));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    const EndpointError authorized = AuthorizeReceiveInvocationLocked(invocation);
    if (authorized.Code != EndpointErrorCode::None)
    {
        return ResultType::Failure(authorized);
    }
    const ResultType prepared = PrepareReceiveLocked(output);
    if (prepared.HasValue())
    {
        m_receiveInvocation = invocation;
    }
    return prepared;
}

Foundation::Result<EndpointReceivePreparation, EndpointError>
Endpoint::PrepareReceiveLocked(MessageEnvelope& output) noexcept
{
    using ResultType = Foundation::Result<EndpointReceivePreparation, EndpointError>;

    if (m_activeReceiveReservationIdentifier != 0U)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::ReservationActive));
    }
    if (m_nextReceiveReservationIdentifier == 0U)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::SequenceExhausted));
    }

    EndpointReceiveMetadata metadata{};
    const EndpointError decoded = DecodeHeadLocked(output, metadata);
    if (decoded.Code != EndpointErrorCode::None)
    {
        return ResultType::Failure(decoded);
    }

    const Foundation::UInt64 identifier = m_nextReceiveReservationIdentifier;
    if (m_nextReceiveReservationIdentifier == ~Foundation::UInt64{0U})
    {
        m_nextReceiveReservationIdentifier = 0U;
    }
    else
    {
        ++m_nextReceiveReservationIdentifier;
    }
    m_activeReceiveReservationIdentifier = identifier;
    m_reservedReceiveSequence = metadata.Sequence;
    m_receiveInvocation = Object::CapabilityHandle{};
    return ResultType::Success(EndpointReceivePreparation{
        .Reservation = EndpointReceiveReservation{*this, identifier},
        .Metadata = metadata,
    });
}

Foundation::Result<EndpointReceiveMetadata, EndpointError>
Endpoint::CommitPreparedReceive(const EndpointReceiveReservation reservation) noexcept
{
    using ResultType = Foundation::Result<EndpointReceiveMetadata, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }
    if (!reservation.IsValid() || (reservation.m_endpoint != this))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidReservation));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    if ((m_activeReceiveReservationIdentifier == 0U) ||
        (m_activeReceiveReservationIdentifier != reservation.m_identifier))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::StaleReservation));
    }
    if (!PreparedReceiveIsValidLocked())
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::QueueCorrupt));
    }
    if (m_receiveInvocation.IsValid())
    {
        const EndpointError authorized = AuthorizeReceiveInvocationLocked(m_receiveInvocation);
        if (authorized.Code != EndpointErrorCode::None)
        {
            return ResultType::Failure(authorized);
        }
    }

    const QueueEntry& entry = m_entries[m_head];
    const EndpointReceiveMetadata metadata{
        .Sequence = entry.Sequence,
        .Sender = entry.Sender,
        .SenderEndpointSealedObjectSelector = entry.SenderEndpointSealedObjectSelector,
        .SenderEndpointRights = entry.SenderEndpointRights,
        .FrameSizeBytes = entry.FrameSizeBytes,
        .AttachmentCount = entry.AttachmentCount,
        .SendClass = entry.SendClass,
    };
    ConsumeHeadLocked();
    m_activeReceiveReservationIdentifier = 0U;
    m_reservedReceiveSequence = 0U;
    m_receiveInvocation = Object::CapabilityHandle{};
    return ResultType::Success(metadata);
}

EndpointError Endpoint::CancelPreparedReceive(const EndpointReceiveReservation reservation) noexcept
{
    if (!m_isConfigurationValid)
    {
        return MakeError(EndpointErrorCode::InvalidConfiguration);
    }
    if (!reservation.IsValid() || (reservation.m_endpoint != this))
    {
        return MakeError(EndpointErrorCode::InvalidReservation);
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    if ((m_activeReceiveReservationIdentifier == 0U) ||
        (m_activeReceiveReservationIdentifier != reservation.m_identifier))
    {
        return MakeError(EndpointErrorCode::StaleReservation);
    }
    if (!PreparedReceiveIsValidLocked())
    {
        return MakeError(EndpointErrorCode::QueueCorrupt);
    }

    m_activeReceiveReservationIdentifier = 0U;
    m_reservedReceiveSequence = 0U;
    m_receiveInvocation = Object::CapabilityHandle{};
    return MakeError(EndpointErrorCode::None);
}

Foundation::Result<EndpointQueueState, EndpointError> Endpoint::DiscardUnread() noexcept
{
    using ResultType = Foundation::Result<EndpointQueueState, EndpointError>;

    if (!m_isConfigurationValid)
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::InvalidConfiguration));
    }

    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    if ((m_activeReservationIdentifier != 0U) || (m_activeReceiveReservationIdentifier != 0U))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::ReservationActive));
    }
    const EndpointQueueState discarded{
        .QueuedBytes = m_queuedBytes,
        .NextSequence = m_nextSequence,
        .QueuedMessages = static_cast<Foundation::UInt8>(m_queuedMessages),
        .QueuedAttachments = static_cast<Foundation::UInt8>(m_queuedAttachments),
        .Reserved = {},
    };

    if ((m_queuedMessages > m_configuration.MaximumQueuedMessages) ||
        (m_queuedBytes > m_configuration.MaximumQueuedBytes) ||
        (m_queuedAttachments > m_configuration.MaximumQueuedAttachments) ||
        (m_head >= m_configuration.MaximumQueuedMessages) || (m_tail >= m_configuration.MaximumQueuedMessages))
    {
        return ResultType::Failure(MakeError(EndpointErrorCode::QueueCorrupt));
    }

    // Every pending handle is visited and counted; one that no longer
    // resolves was already closed by the receiver and is skipped.
    ClearPendingCloseIndices();
    Foundation::Size pendingCount{0U};
    Foundation::Size closeCount{0U};
    Foundation::Size entryIndex = m_head;
    for (Foundation::Size queued{0U}; queued < m_queuedMessages; ++queued)
    {
        const QueueEntry& entry = m_entries[entryIndex];
        if ((entry.Sequence == 0U) || !entry.Sender.IsValid() || !entry.SenderEndpointRights.IsValid() ||
            !IsKnownSendClass(entry.SendClass) || (entry.FrameSizeBytes < MessageCodec::WireHeaderSizeBytes) ||
            (entry.FrameSizeBytes > MessageCodec::MaximumWireSizeBytes) ||
            (entry.AttachmentCount > MessageEnvelope::MaximumCapabilityAttachments) ||
            (entry.AttachmentCount > (MaximumQueuedAttachments - pendingCount)))
        {
            ClearPendingCloseIndices();
            return ResultType::Failure(MakeError(EndpointErrorCode::QueueCorrupt));
        }
        for (Foundation::Size attachment{0U}; attachment < entry.AttachmentCount; ++attachment)
        {
            ++pendingCount;
            const auto indexResult = m_receiverSpace.ResolveHandleLocked(entry.PendingHandles[attachment]);
            if (!indexResult.HasValue())
            {
                continue;
            }
            for (Foundation::Size previous{0U}; previous < closeCount; ++previous)
            {
                if (m_pendingCloseIndices[previous] == indexResult.GetValue())
                {
                    ClearPendingCloseIndices();
                    return ResultType::Failure(MakeError(EndpointErrorCode::QueueCorrupt));
                }
            }
            m_pendingCloseIndices[closeCount] = indexResult.GetValue();
            ++closeCount;
        }
        entryIndex = AdvanceQueueIndex(entryIndex);
    }
    if (pendingCount != m_queuedAttachments)
    {
        ClearPendingCloseIndices();
        return ResultType::Failure(MakeError(EndpointErrorCode::QueueCorrupt));
    }

    for (Foundation::Size index{0U}; index < closeCount; ++index)
    {
        m_receiverSpace.CloseIndexLocked(m_pendingCloseIndices[index]);
    }
    ClearPendingCloseIndices();
    for (QueueEntry& entry : m_entries)
    {
        ClearQueueEntry(entry);
    }
    m_head = 0U;
    m_tail = 0U;
    m_queuedMessages = 0U;
    m_queuedBytes = 0U;
    m_queuedAttachments = 0U;

    return ResultType::Success(discarded);
}

EndpointQueueState Endpoint::GetQueueState() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_receiverSpace.m_synchronization};
    return EndpointQueueState{
        .QueuedBytes = m_queuedBytes,
        .NextSequence = m_nextSequence,
        .QueuedMessages = static_cast<Foundation::UInt8>(m_queuedMessages),
        .QueuedAttachments = static_cast<Foundation::UInt8>(m_queuedAttachments),
        .Reserved = {},
    };
}

bool Endpoint::IsReady() const noexcept
{
    return m_isConfigurationValid;
}

bool Endpoint::HasReceiverCapabilitySpace(const Object::CapabilitySpace& candidate) const noexcept
{
    return &candidate == &m_receiverSpace;
}

bool Endpoint::IsConfigurationValid(const EndpointConfiguration& configuration) noexcept
{
    const Foundation::Size maximumMessages = configuration.MaximumQueuedMessages;
    const Foundation::Size reservedMessages = configuration.ReservedControlMessages;
    const Foundation::Size perSenderMessages = configuration.MaximumMessagesPerSender;
    if ((configuration.Reserved != 0U) || (maximumMessages == 0U) || (maximumMessages > MaximumQueueEntries) ||
        (reservedMessages >= maximumMessages) || (perSenderMessages == 0U) ||
        (perSenderMessages > (maximumMessages - reservedMessages)))
    {
        return false;
    }

    // Each byte budget lies between what its messages need at minimum and
    // what they can carry at most, like the attachment budgets below.
    const Foundation::Size maximumBytes = configuration.MaximumQueuedBytes;
    const Foundation::Size reservedBytes = configuration.ReservedControlBytes;
    const Foundation::Size perSenderBytes = configuration.MaximumBytesPerSender;
    const Foundation::Size minimumFrame = MessageCodec::WireHeaderSizeBytes;
    const Foundation::Size maximumFrame = MessageCodec::MaximumWireSizeBytes;
    const Foundation::Size normalMessages = maximumMessages - reservedMessages;
    if ((maximumBytes > MaximumQueueBytes) || (maximumBytes < (maximumMessages * minimumFrame)) ||
        (maximumBytes > (maximumMessages * maximumFrame)) || (reservedBytes > maximumBytes) ||
        (reservedBytes < (reservedMessages * minimumFrame)) || (reservedBytes > (reservedMessages * maximumFrame)) ||
        ((maximumBytes - reservedBytes) < (normalMessages * minimumFrame)) ||
        ((maximumBytes - reservedBytes) > (normalMessages * maximumFrame)) ||
        (perSenderBytes < (perSenderMessages * minimumFrame)) || (perSenderBytes > (maximumBytes - reservedBytes)) ||
        (perSenderBytes > (perSenderMessages * maximumFrame)))
    {
        return false;
    }

    const Foundation::Size maximumAttachments = configuration.MaximumQueuedAttachments;
    const Foundation::Size reservedAttachments = configuration.ReservedControlAttachments;
    const Foundation::Size perSenderAttachments = configuration.MaximumAttachmentsPerSender;
    if ((maximumAttachments > MaximumQueuedAttachments) ||
        (maximumAttachments > (maximumMessages * MessageEnvelope::MaximumCapabilityAttachments)) ||
        (reservedAttachments > maximumAttachments) ||
        (reservedAttachments > (reservedMessages * MessageEnvelope::MaximumCapabilityAttachments)) ||
        ((maximumAttachments - reservedAttachments) >
         ((maximumMessages - reservedMessages) * MessageEnvelope::MaximumCapabilityAttachments)) ||
        (perSenderAttachments > (perSenderMessages * MessageEnvelope::MaximumCapabilityAttachments)) ||
        (perSenderAttachments > (maximumAttachments - reservedAttachments)))
    {
        return false;
    }

    return true;
}

bool Endpoint::IsKnownSendClass(const EndpointSendClass sendClass) noexcept
{
    switch (sendClass)
    {
    case EndpointSendClass::Normal:
    case EndpointSendClass::ReservedControl:
        return true;

    case EndpointSendClass::Invalid:
        return false;
    }
    return false;
}

EndpointError Endpoint::MakeError(const EndpointErrorCode code, const MessageCodecError messageError,
                                  const Object::CapabilityError capabilityError) noexcept
{
    return EndpointError{
        .Code = code,
        .Message = messageError,
        .Capability = capabilityError,
        .Reserved = 0U,
    };
}

Endpoint::SenderUsage Endpoint::GetSenderUsageLocked(const Object::ResourceDomainId sender) const noexcept
{
    SenderUsage usage{};
    Foundation::Size entryIndex = m_head;
    for (Foundation::Size queued{0U}; queued < m_queuedMessages; ++queued)
    {
        const QueueEntry& entry = m_entries[entryIndex];
        if ((entry.SendClass == EndpointSendClass::Normal) && (entry.Sender.GetValue() == sender.GetValue()))
        {
            ++usage.Messages;
            usage.Bytes += entry.FrameSizeBytes;
            usage.Attachments += entry.AttachmentCount;
        }
        entryIndex = AdvanceQueueIndex(entryIndex);
    }
    return usage;
}

Foundation::Size Endpoint::AdvanceQueueIndex(const Foundation::Size index) const noexcept
{
    const Foundation::Size next = index + 1U;
    return next == m_configuration.MaximumQueuedMessages ? 0U : next;
}

void Endpoint::ClearStagingFrame() noexcept
{
    for (Foundation::UInt8& value : m_stagingFrame)
    {
        value = 0U;
    }
}

void Endpoint::ClearReceiveStagingMessage() noexcept
{
    Foundation::UInt8* const bytes = reinterpret_cast<Foundation::UInt8*>(&m_receiveStagingMessage);
    for (Foundation::Size index{0U}; index < sizeof(m_receiveStagingMessage); ++index)
    {
        bytes[index] = 0U;
    }
}

void Endpoint::ClearPendingCloseIndices() noexcept
{
    for (Foundation::Size& index : m_pendingCloseIndices)
    {
        index = 0U;
    }
}

void Endpoint::ClearQueueEntry(QueueEntry& entry) noexcept
{
    for (Foundation::UInt8& value : entry.Frame)
    {
        value = 0U;
    }
    for (Object::CapabilityHandle& handle : entry.PendingHandles)
    {
        handle = Object::CapabilityHandle{};
    }
    entry.Sequence = 0U;
    entry.Sender = Object::ResourceDomainId{};
    entry.SenderEndpointSealedObjectSelector = 0U;
    entry.SenderEndpointRights = Object::CapabilityRights{};
    entry.FrameSizeBytes = 0U;
    entry.AttachmentCount = 0U;
    entry.SendClass = EndpointSendClass::Invalid;
}

void Endpoint::CopyFrame(Foundation::UInt8* const destination, const Foundation::UInt8* const source) noexcept
{
    for (Foundation::Size index{0U}; index < MessageCodec::MaximumWireSizeBytes; ++index)
    {
        destination[index] = source[index];
    }
}

void Endpoint::PatchTransferredHandles(QueueEntry& entry, const Foundation::UInt8 variablePayloadCount,
                                       const Object::CapabilityTransferBatch& transferred) noexcept
{
    const Foundation::Size attachmentsOffset =
        MessageCodec::WireHeaderSizeBytes +
        (static_cast<Foundation::Size>(variablePayloadCount) * MessageCodec::WireVariablePayloadDescriptorSizeBytes);
    for (Foundation::Size index{0U}; index < transferred.Count; ++index)
    {
        const Foundation::Size attachmentOffset =
            attachmentsOffset + (index * MessageCodec::WireCapabilityAttachmentSizeBytes);
        const Object::CapabilityHandle handle = transferred.DestinationHandles[index];
        WriteLittleEndian64(entry.Frame + attachmentOffset + AttachmentSelectorOffset, handle.GetSelector());
        WriteLittleEndian64(entry.Frame + attachmentOffset + AttachmentAuthenticatorOffset, handle.GetAuthenticator());
    }
}
} // namespace Asteria::Kernel::Ipc
