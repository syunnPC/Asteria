// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_ENDPOINT_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_ENDPOINT_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/EndpointTypes.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/IKernelObject.hpp"

namespace Asteria::Kernel::Ipc
{
enum class EndpointErrorCode : Foundation::UInt8
{
    None = 0U,
    InvalidConfiguration,
    InvalidArgument,
    InvalidMessage,
    WouldBlock,
    CapabilityTransferFailed,
    QueueCorrupt,
    SequenceExhausted,
    ReservationActive,
    InvalidReservation,
    StaleReservation,
    /// The capability that invokes the Endpoint does not authorize the operation where it takes
    /// effect; EndpointError::Capability says why.
    InvocationRejected,
};

struct EndpointError
{
    EndpointErrorCode Code;
    MessageCodecError Message;
    Object::CapabilityError Capability;
    Foundation::UInt8 Reserved;
};

static_assert(sizeof(EndpointError) == 4U);
static_assert(alignof(EndpointError) == 1U);

struct EndpointConfiguration
{
    Foundation::UInt16 MaximumQueuedBytes;
    Foundation::UInt16 ReservedControlBytes;
    Foundation::UInt16 MaximumBytesPerSender;
    Foundation::UInt8 MaximumQueuedMessages;
    Foundation::UInt8 ReservedControlMessages;
    Foundation::UInt8 MaximumMessagesPerSender;
    Foundation::UInt8 MaximumQueuedAttachments;
    Foundation::UInt8 ReservedControlAttachments;
    Foundation::UInt8 MaximumAttachmentsPerSender;
    Foundation::UInt32 Reserved;

    [[nodiscard]] static constexpr EndpointConfiguration Default() noexcept
    {
        return EndpointConfiguration{
            .MaximumQueuedBytes = 4'032U,
            .ReservedControlBytes = 1'008U,
            .MaximumBytesPerSender = 2'016U,
            .MaximumQueuedMessages = 8U,
            .ReservedControlMessages = 2U,
            .MaximumMessagesPerSender = 4U,
            .MaximumQueuedAttachments = 32U,
            .ReservedControlAttachments = 8U,
            .MaximumAttachmentsPerSender = 16U,
            .Reserved = 0U,
        };
    }
};

static_assert(sizeof(EndpointConfiguration) == 16U);
static_assert(alignof(EndpointConfiguration) == 4U);

struct EndpointSendReceipt
{
    Foundation::UInt64 Sequence;
};

static_assert(sizeof(EndpointSendReceipt) == 8U);
static_assert(alignof(EndpointSendReceipt) == 8U);

class Endpoint;

/// Names one invisible, fully prepared send owned by one Endpoint.
/// The originating Endpoint outlives every copy and use of this token.
class EndpointSendReservation final
{
public:
    constexpr EndpointSendReservation() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetIdentifier() const noexcept
    {
        return m_identifier;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_endpoint != nullptr) && (m_identifier != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const EndpointSendReservation other) const noexcept
    {
        return (m_endpoint == other.m_endpoint) && (m_identifier == other.m_identifier);
    }

private:
    friend class Endpoint;

    constexpr EndpointSendReservation(const Endpoint& endpoint, const Foundation::UInt64 identifier) noexcept
        : m_endpoint{&endpoint}, m_identifier{identifier}
    {
    }

    const Endpoint* m_endpoint{nullptr};
    Foundation::UInt64 m_identifier{0U};
};

static_assert(sizeof(EndpointSendReservation) == 16U);
static_assert(alignof(EndpointSendReservation) == 8U);

/// Names one decoded, unconsumed FIFO head owned by one Endpoint.
/// The originating Endpoint outlives every copy and use of this token.
class EndpointReceiveReservation final
{
public:
    constexpr EndpointReceiveReservation() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetIdentifier() const noexcept
    {
        return m_identifier;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_endpoint != nullptr) && (m_identifier != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const EndpointReceiveReservation other) const noexcept
    {
        return (m_endpoint == other.m_endpoint) && (m_identifier == other.m_identifier);
    }

private:
    friend class Endpoint;

    constexpr EndpointReceiveReservation(const Endpoint& endpoint, const Foundation::UInt64 identifier) noexcept
        : m_endpoint{&endpoint}, m_identifier{identifier}
    {
    }

    const Endpoint* m_endpoint{nullptr};
    Foundation::UInt64 m_identifier{0U};
};

static_assert(sizeof(EndpointReceiveReservation) == 16U);
static_assert(alignof(EndpointReceiveReservation) == 8U);

struct EndpointReceivePreparation
{
    EndpointReceiveReservation Reservation;
    EndpointReceiveMetadata Metadata;
};

static_assert(sizeof(EndpointReceivePreparation) == 48U);
static_assert(alignof(EndpointReceivePreparation) == 8U);

struct EndpointQueueState
{
    Foundation::Size QueuedBytes;
    Foundation::UInt64 NextSequence;
    Foundation::UInt8 QueuedMessages;
    Foundation::UInt8 QueuedAttachments;
    Foundation::UInt8 Reserved[6];
};

static_assert(sizeof(EndpointQueueState) == 24U);
static_assert(alignof(EndpointQueueState) == 8U);

class Endpoint final : public Object::IKernelObject
{
public:
    static constexpr Foundation::Size MaximumQueueEntries{8U};
    static constexpr Foundation::Size MaximumQueueBytes{MaximumQueueEntries * MessageCodec::MaximumWireSizeBytes};
    static constexpr Foundation::Size MaximumQueuedAttachments{MaximumQueueEntries *
                                                               MessageEnvelope::MaximumCapabilityAttachments};

    /// Creates one bounded FIFO whose transferred handles belong to receiverSpace.
    /// Preconditions:
    /// - receiverSpace and its collaborators outlive this endpoint.
    /// - The endpoint is registered only in receiverSpace's object registry.
    /// - Destruction occurs after object quiescence, after DiscardUnread or complete receive
    /// draining, after resolving every prepared send and receive, and outside the shared
    /// registry lock.
    Endpoint(Object::CapabilitySpace& receiverSpace, Object::ResourceDomainId chargeOwner,
             EndpointConfiguration configuration = EndpointConfiguration::Default()) noexcept;

    /// Clears private storage without acquiring a lock. Unread messages must already be drained.
    ~Endpoint() noexcept;

    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;
    Endpoint(Endpoint&&) = delete;
    Endpoint& operator=(Endpoint&&) = delete;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

    /// Validates, transfers, and publishes one message at a single lock linearization point.
    /// The message carries zero invoking rights and a zero sealed selector.
    /// ReservedControl is restricted to dispatchers that already proved emergency-control
    /// authority; normal callers must use Normal.
    /// Preconditions:
    /// - message remains stable and does not alias this endpoint for the call.
    /// - sender identifies a trusted caller resource domain.
    /// - The caller prevents same-CPU preemption or reentry by code using the shared lock.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    /// Allocation:
    /// - Does not allocate.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    [[nodiscard]] Foundation::Result<EndpointSendReceipt, EndpointError> Send(Object::CapabilitySpace& sourceSpace,
                                                                              Object::ResourceDomainId sender,
                                                                              EndpointSendClass sendClass,
                                                                              const MessageEnvelope& message) noexcept;

    /// Sends for the holder of invocation, a capability in callerSpace, which
    /// is also the attachment source. In the critical section that publishes
    /// the message, and against the one clock sample that also governs the
    /// message deadline and attachment leases, the capability must authorize
    /// Invoke on exactly this Endpoint: authentic, current revocation epoch,
    /// unexpired lease, live object. Otherwise the send fails with
    /// InvocationRejected before any queue or capability check, so a Revoke,
    /// Close, or expiry ordered before the publication is always observed.
    /// The queued message records that capability's rights and sealed
    /// selector as trusted metadata; message bytes never supply them.
    /// Preconditions, blocking, allocation, and interrupt rules match Send.
    [[nodiscard]] Foundation::Result<EndpointSendReceipt, EndpointError>
    SendFromInvocation(Object::CapabilitySpace& callerSpace, Object::CapabilityHandle invocation,
                       Object::ResourceDomainId sender, EndpointSendClass sendClass,
                       const MessageEnvelope& message) noexcept;

    /// Fully validates and privately stages one message without making it
    /// receivable. Copies and destination handles are installed now but stay
    /// private until CommitPreparedSend; each moved source is held: its slot
    /// and handle are kept but unusable, and it is removed only when
    /// CommitPreparedSend publishes the message. CancelPreparedSend closes
    /// the destinations and restores every held source under its unchanged
    /// handle, so a failed or abandoned send removes nothing.
    /// A successful reservation temporarily excludes later sends, but already
    /// queued messages remain receivable. Exactly one CommitPreparedSend or
    /// CancelPreparedSend must resolve the returned token, and sourceSpace
    /// must outlive it.
    /// Preconditions, blocking, allocation, and interrupt rules match Send.
    [[nodiscard]] Foundation::Result<EndpointSendReservation, EndpointError>
    PrepareSend(Object::CapabilitySpace& sourceSpace, Object::ResourceDomainId sender, EndpointSendClass sendClass,
                const MessageEnvelope& message) noexcept;

    /// Publishes the exact prepared message and removes its held sources.
    /// With an unmodified Endpoint and its matching live reservation, this
    /// performs no validation, allocation, clock read, capability lookup, or
    /// operation that can normally fail.
    [[nodiscard]] Foundation::Result<EndpointSendReceipt, EndpointError>
    CommitPreparedSend(EndpointSendReservation reservation) noexcept;

    /// Closes every private destination handle, restores every held source
    /// handle, and removes the prepared send.
    [[nodiscard]] EndpointError CancelPreparedSend(EndpointSendReservation reservation) noexcept;

    /// Removes the oldest message after decoding it into output.
    /// Every failure leaves output and the queue unchanged.
    /// Preconditions:
    /// - output does not alias this endpoint.
    /// - The caller prevents same-CPU preemption or reentry by code using the shared lock.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    /// Allocation:
    /// - Does not allocate.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    [[nodiscard]] Foundation::Result<EndpointReceiveMetadata, EndpointError> Receive(MessageEnvelope& output) noexcept;

    /// Decodes the FIFO head into output while reserving that exact sequence.
    /// The message remains queued and sends may enqueue behind it. Every
    /// failure leaves output and the FIFO unchanged. Exactly one successful
    /// CommitPreparedReceive or CancelPreparedReceive must resolve the token.
    [[nodiscard]] Foundation::Result<EndpointReceivePreparation, EndpointError>
    PrepareReceive(MessageEnvelope& output) noexcept;

    /// PrepareReceive for the holder of invocation in callerSpace, which must
    /// be this Endpoint's receiver space (InvocationRejected with RightsDenied
    /// otherwise). The capability must authorize Read on exactly this
    /// Endpoint before the reservation, and CommitPreparedReceive authorizes
    /// it again in the critical section that consumes the head.
    [[nodiscard]] Foundation::Result<EndpointReceivePreparation, EndpointError>
    PrepareReceiveFromInvocation(Object::CapabilitySpace& callerSpace, Object::CapabilityHandle invocation,
                                 MessageEnvelope& output) noexcept;

    /// Consumes the exact FIFO head decoded by PrepareReceive once. For a
    /// reservation made through an invocation, a capability that no longer
    /// authorizes Read fails with InvocationRejected and leaves the
    /// reservation and FIFO unchanged; the caller cancels it.
    [[nodiscard]] Foundation::Result<EndpointReceiveMetadata, EndpointError>
    CommitPreparedReceive(EndpointReceiveReservation reservation) noexcept;

    /// Releases a prepared receive without consuming or changing the FIFO.
    [[nodiscard]] EndpointError CancelPreparedReceive(EndpointReceiveReservation reservation) noexcept;

    /// Atomically closes every capability held by unread messages and empties the FIFO.
    /// A pending handle that no longer resolves was already closed by the receiver, which can
    /// learn a queued handle from a cancelled PrepareReceive; it is skipped. Success carries the
    /// queue state immediately before the discard. The sequence counter is preserved. Every
    /// failure leaves the queue and receiver capability space unchanged.
    /// Preconditions:
    /// - The caller prevents same-CPU preemption or reentry by code using the shared lock.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    /// Allocation:
    /// - Does not allocate.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    [[nodiscard]] Foundation::Result<EndpointQueueState, EndpointError> DiscardUnread() noexcept;

    [[nodiscard]] EndpointQueueState GetQueueState() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;

    /// Allows an invocation adapter to enforce this endpoint's exact receiver
    /// capability-space identity without exposing mutable internals.
    [[nodiscard]] bool HasReceiverCapabilitySpace(const Object::CapabilitySpace& candidate) const noexcept;

    /// Makes readiness, registered as identity, this Endpoint's readiness Notification: it is
    /// signalled whenever a message is published for the receiver and whenever a receive leaves
    /// messages queued, so a WaitSet bound to it is level-triggered. Endpoints without one (boot
    /// Endpoints) signal nothing. Preconditions: - called once, before any capability names this
    /// Endpoint; - readiness is initialized and outlives every operation on this Endpoint.
    void AttachReadiness(Notification& readiness, NotificationId identity) noexcept;

    // An embedded fault queue reauthorizes Read on its enclosing channel at prepare and commit.
    // Fixed before publication.
    void AttachReceiveAuthority(Object::IKernelObject& authority) noexcept
    {
        m_receiveAuthority = &authority;
    }

    /// The readiness Notification and its identity; null and invalid without
    /// one. Fixed once a capability names this Endpoint.
    [[nodiscard]] Notification* GetReadiness() const noexcept
    {
        return m_readiness;
    }
    [[nodiscard]] NotificationId GetReadinessId() const noexcept
    {
        return m_readinessId;
    }

private:
    struct SenderUsage
    {
        Foundation::Size Messages;
        Foundation::Size Bytes;
        Foundation::Size Attachments;
    };

    struct QueueEntry
    {
        Foundation::UInt8 Frame[MessageCodec::MaximumWireSizeBytes];
        Object::CapabilityHandle PendingHandles[MessageEnvelope::MaximumCapabilityAttachments];
        Foundation::UInt64 Sequence;
        Object::ResourceDomainId Sender;
        Foundation::UInt64 SenderEndpointSealedObjectSelector;
        Object::CapabilityRights SenderEndpointRights;
        Foundation::UInt16 FrameSizeBytes;
        Foundation::UInt8 AttachmentCount;
        EndpointSendClass SendClass;
    };

    static_assert(sizeof(QueueEntry) == 600U);
    static_assert(alignof(QueueEntry) == 8U);

    [[nodiscard]] static bool IsConfigurationValid(const EndpointConfiguration& configuration) noexcept;
    [[nodiscard]] static bool IsKnownSendClass(EndpointSendClass sendClass) noexcept;
    [[nodiscard]] static EndpointError
    MakeError(EndpointErrorCode code, MessageCodecError messageError = MessageCodecError::None,
              Object::CapabilityError capabilityError = Object::CapabilityError::Ok) noexcept;

    /// What authorized one send: the invoking capability's trusted metadata
    /// and the clock sample taken while authorizing it, if any.
    struct SendAuthority
    {
        Object::MonotonicTime Now;
        Foundation::UInt64 SenderEndpointSealedObjectSelector;
        Object::CapabilityRights SenderEndpointRights;
        bool IsTimeSampled;
    };

    [[nodiscard]] static EndpointError MakeInvocationError(Object::CapabilityError capabilityError) noexcept;
    [[nodiscard]] EndpointError AuthorizeSendInvocationLocked(Object::CapabilitySpace& callerSpace,
                                                              Object::CapabilityHandle invocation,
                                                              SendAuthority& authority) noexcept;
    [[nodiscard]] EndpointError AuthorizeReceiveInvocationLocked(Object::CapabilityHandle invocation) noexcept;
    [[nodiscard]] EndpointError StageSendLocked(Object::CapabilitySpace& sourceSpace, Object::ResourceDomainId sender,
                                                EndpointSendClass sendClass, const SendAuthority& authority,
                                                const MessageEnvelope& message,
                                                Object::CapabilitySpace::MovedSourceCommit movedSourceCommit) noexcept;
    [[nodiscard]] EndpointError TransferAttachmentsLocked(Object::CapabilitySpace& sourceSpace,
                                                          const SendAuthority& authority,
                                                          const MessageEnvelope& message,
                                                          Object::CapabilitySpace::MovedSourceCommit movedSourceCommit,
                                                          Object::CapabilityTransferBatch& transferred) noexcept;
    [[nodiscard]] EndpointSendReceipt PublishStagedSendLocked() noexcept;
    [[nodiscard]] bool PreparedEntryIsValidLocked() const noexcept;
    [[nodiscard]] bool ResolveHeldSourcesLocked(
        Foundation::Size (&indices)[MessageEnvelope::MaximumCapabilityAttachments]) const noexcept;
    void ClearPreparedSendLocked() noexcept;
    [[nodiscard]] EndpointError CancelPreparedSendLocked() noexcept;
    [[nodiscard]] Foundation::Result<EndpointReceivePreparation, EndpointError>
    PrepareReceiveLocked(MessageEnvelope& output) noexcept;
    [[nodiscard]] bool QueueHeadIsValidLocked() const noexcept;
    [[nodiscard]] bool PreparedReceiveIsValidLocked() const noexcept;
    [[nodiscard]] EndpointError DecodeHeadLocked(MessageEnvelope& output, EndpointReceiveMetadata& metadata) noexcept;
    void ConsumeHeadLocked() noexcept;
    /// Signals the readiness Notification, if any; lock-free.
    void SignalReadinessLocked() noexcept;
    [[nodiscard]] SenderUsage GetSenderUsageLocked(Object::ResourceDomainId sender) const noexcept;
    [[nodiscard]] Foundation::Size AdvanceQueueIndex(Foundation::Size index) const noexcept;
    void ClearStagingFrame() noexcept;
    void ClearReceiveStagingMessage() noexcept;
    void ClearPendingCloseIndices() noexcept;
    static void ClearQueueEntry(QueueEntry& entry) noexcept;
    static void CopyFrame(Foundation::UInt8* destination, const Foundation::UInt8* source) noexcept;
    static void PatchTransferredHandles(QueueEntry& entry, Foundation::UInt8 variablePayloadCount,
                                        const Object::CapabilityTransferBatch& transferred) noexcept;

    Object::IKernelObject* m_receiveAuthority{nullptr};
    Object::CapabilitySpace& m_receiverSpace;
    Object::ResourceDomainId m_chargeOwner;
    Notification* m_readiness{nullptr};
    NotificationId m_readinessId{};
    EndpointConfiguration m_configuration;
    bool m_isConfigurationValid;
    QueueEntry m_entries[MaximumQueueEntries]{};
    Foundation::UInt8 m_stagingFrame[MessageCodec::MaximumWireSizeBytes]{};
    MessageEnvelope m_receiveStagingMessage{};
    Foundation::Size m_pendingCloseIndices[MaximumQueuedAttachments]{};
    Foundation::Size m_head{0U};
    Foundation::Size m_tail{0U};
    Foundation::Size m_queuedMessages{0U};
    Foundation::Size m_queuedBytes{0U};
    Foundation::Size m_queuedAttachments{0U};
    Foundation::UInt64 m_nextSequence{1U};
    Foundation::UInt64 m_nextReservationIdentifier{1U};
    Foundation::UInt64 m_activeReservationIdentifier{0U};
    Foundation::UInt64 m_nextReceiveReservationIdentifier{1U};
    Foundation::UInt64 m_activeReceiveReservationIdentifier{0U};
    Foundation::UInt64 m_reservedReceiveSequence{0U};
    Object::CapabilitySpace* m_preparedSourceSpace{nullptr};
    Object::CapabilityHandle m_preparedHeldSources[MessageEnvelope::MaximumCapabilityAttachments]{};
    Foundation::Size m_preparedHeldCount{0U};
    Object::CapabilityHandle m_receiveInvocation{};
};
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_ENDPOINT_HPP_INCLUDED
