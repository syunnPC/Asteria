// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_SYNCHRONOUS_REPLY_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_SYNCHRONOUS_REPLY_DISPATCHER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/ReplyToken.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/ReadyQueue.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Ipc
{
enum class SynchronousReplyDispatcherState : Foundation::UInt8
{
    Uninitialized,
    Pending,
    Accepted,
    Replied,
    Cancelled,
    Faulted,
};

enum class SynchronousReplyDispatcherError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidToken,
    TokenPinRejected,
    TokenNotPending,
    CallerPinRejected,
    ContextPinRejected,
    CorrelatedStateChanged,
    AlreadyInitialized,
    NotInitialized,
    InvalidServer,
    ServerRegistryMismatch,
    InvalidSender,
    ServerPinRejected,
    ServerOwnerMismatch,
    AlreadyAccepted,
    NotAccepted,
    ServerStillRunning,
    InvalidReply,
    QueueReservationRejected,
    EndpointPreparationRejected,
    TokenRejected,
    EndpointCommitRejected,
    WakeRejected,
    QueueCommitRejected,
    ReservationRollbackRejected,
    AlreadyCompleted,
    Faulted,
    ReplyCapabilityRejected,
    ReplyCapabilityMismatch,
};

struct SynchronousReplyDispatcherFailure
{
    Foundation::UInt64 TransactionIdentifier;
    ReplyTokenFailure Token;
    Scheduler::ReadyQueueFailure Queue;
    EndpointError Endpoint;
    Object::ObjectPinError Pin;
    Scheduler::ThreadError Thread;
    Scheduler::ReadyQueueError QueueTransition;
    SynchronousReplyDispatcherError Error;
    Object::CapabilityError Capability;
    Foundation::UInt8 Reserved[3];
};

struct SynchronousReplyDispatcherReceipt
{
    Scheduler::ThreadId Caller;
    Foundation::UInt64 TransactionIdentifier;
    Foundation::UInt64 EndpointSequence;
    Foundation::UInt64 QueueSequence;
    SynchronousReplyDispatcherState State;
    Foundation::UInt8 Reserved[7];
};

struct SynchronousReplyDispatcherSnapshot
{
    Scheduler::ThreadId Caller;
    Scheduler::SchedulingContextId SchedulingContext;
    Scheduler::ThreadId Server;
    Foundation::UInt64 TransactionIdentifier;
    SynchronousReplyDispatcherState State;
    Foundation::UInt8 Reserved[7];
};

/// Commits one ReplyToken into one private Endpoint send, exact caller Wake,
/// and pre-reserved ReadyQueue entry.
///
/// This owner is the sole mutable access path to its ReplyToken after
/// Initialize. It serializes Accept, reply, and cancellation so a successful
/// Endpoint preparation cannot race a cancellation. The server run and its
/// donated SchedulingContext must be stopped before CompleteReply or an
/// accepted Cancel. A successful terminal operation keeps the token alive
/// through Endpoint publication, exact Wake, and ready-queue publication, then
/// releases all dispatcher pins. The token's storage owner may destroy and
/// collect the now-terminal token afterward.
///
/// The object registry, reply endpoint, wake queue, accepted server capability
/// space, ReplyToken storage, and every registered object outlive this
/// dispatcher and its active operation. Operations may spin, are forbidden in
/// interrupt context, and require same-CPU reentry to be excluded.
class SynchronousReplyDispatcher final
{
public:
    /// Preconditions:
    /// - replyEndpoint's receiver capability space and wakeQueue use registry.
    /// - All three objects remain alive and immovable for this lifetime.
    constexpr SynchronousReplyDispatcher(Object::ObjectRegistry& registry, Endpoint& replyEndpoint,
                                         Scheduler::ReadyQueue& wakeQueue) noexcept
        : m_registry{registry}, m_replyEndpoint{replyEndpoint}, m_wakeQueue{wakeQueue}
    {
    }

    /// Preconditions:
    /// - State is Uninitialized, Replied, or Cancelled.
    /// - No concurrent operation accesses this object.
    /// - A terminal ReplyToken is destroyed separately by its storage owner.
    ~SynchronousReplyDispatcher() noexcept = default;

    SynchronousReplyDispatcher(const SynchronousReplyDispatcher&) = delete;
    SynchronousReplyDispatcher& operator=(const SynchronousReplyDispatcher&) = delete;
    SynchronousReplyDispatcher(SynchronousReplyDispatcher&&) = delete;
    SynchronousReplyDispatcher& operator=(SynchronousReplyDispatcher&&) = delete;

    /// Claims one already initialized Pending ReplyToken and independently
    /// pins its exact caller and SchedulingContext generations.
    /// The token owner transfers exclusive mutable access before this call;
    /// no direct token operation may race Initialize or any later operation.
    [[nodiscard]] SynchronousReplyDispatcherFailure Initialize(Object::ObjectId replyToken) noexcept;

    /// Side-effect-free readiness check for a caller that has not blocked yet.
    /// Success proves that the dispatcher is unclaimed and that its immutable
    /// reply Endpoint and wake-queue processor configuration are usable.
    [[nodiscard]] SynchronousReplyDispatcherFailure PreflightInitialize() const noexcept;

    /// Accepts the token for one exact server and binds the capability source
    /// and resource-domain identity used by its eventual reply.
    /// replyCapability is the server's reply authority: the ReplyToken
    /// capability that the Call delivered into serverSpace. It must authorize
    /// Invoke on exactly this dispatcher's token (ReplyCapabilityRejected with
    /// the lookup error, or ReplyCapabilityMismatch). A successful Accept
    /// consumes it: the one-shot authority becomes this binding, which alone
    /// authorizes CompleteReply and an accepted Cancel, and no ReplyToken
    /// capability remains in serverSpace. A rejected Accept changes no token,
    /// donation, pin, or capability state.
    /// serverSpace shares the endpoint's registry, revocation registry, clock,
    /// and synchronization domain and was authorized for this server dispatch.
    [[nodiscard]] SynchronousReplyDispatcherFailure Accept(Scheduler::ThreadId server,
                                                           Object::CapabilitySpace& serverSpace,
                                                           Object::CapabilityHandle replyCapability,
                                                           Object::ResourceDomainId sender) noexcept;

    /// Reserves the caller's queue entry, privately prepares the Reply message,
    /// consumes the ReplyToken, publishes the reply, wakes the exact blocked
    /// generation, and finally exposes the runnable entry.
    [[nodiscard]] Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>
    CompleteReply(const MessageEnvelope& reply) noexcept;

    /// Cancels a Pending or stopped Accepted call without publishing a reply,
    /// then wakes and enqueues the exact caller.
    [[nodiscard]] Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>
    Cancel() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;

    /// Returns the immutable processor identity of the caller wake queue.
    /// This permits pre-block call setup to reject a queue that cannot accept
    /// the caller's captured running processor.
    [[nodiscard]] Scheduler::CpuId GetWakeProcessor() const noexcept;
    [[nodiscard]] SynchronousReplyDispatcherSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] static SynchronousReplyDispatcherFailure
    MakeFailure(SynchronousReplyDispatcherError error, Foundation::UInt64 transactionIdentifier = 0U,
                Object::ObjectPinError pin = Object::ObjectPinError::Ok, const ReplyTokenFailure& token = {},
                const Scheduler::ReadyQueueFailure& queue = {}, const EndpointError& endpoint = {},
                Scheduler::ThreadError thread = Scheduler::ThreadError::None,
                Scheduler::ReadyQueueError queueTransition = Scheduler::ReadyQueueError::None,
                Object::CapabilityError capability = Object::CapabilityError::Ok) noexcept;
    [[nodiscard]] SynchronousReplyDispatcherFailure
    AuthenticateReplyCapabilityLocked(Object::CapabilitySpace& serverSpace,
                                      Object::CapabilityHandle replyCapability) const noexcept;
    [[nodiscard]] SynchronousReplyDispatcherFailure PreflightInitializeLocked() const noexcept;
    [[nodiscard]] bool ServerIsStoppedLocked() const noexcept;
    [[nodiscard]] SynchronousReplyDispatcherFailure
    ReserveCallerLocked(Scheduler::ReadyQueueEnqueueReservation& reservation) noexcept;
    [[nodiscard]] SynchronousReplyDispatcherFailure
    PrepareEndpointLocked(const MessageEnvelope& reply, Scheduler::ReadyQueueEnqueueReservation queueReservation,
                          EndpointSendReservation& endpointReservation) noexcept;
    [[nodiscard]] SynchronousReplyDispatcherFailure
    ConsumeReplyTokenLocked(EndpointSendReservation endpointReservation,
                            Scheduler::ReadyQueueEnqueueReservation queueReservation) noexcept;
    [[nodiscard]] SynchronousReplyDispatcherFailure
    ConsumeCancellationTokenLocked(Scheduler::ReadyQueueEnqueueReservation queueReservation) noexcept;
    [[nodiscard]] SynchronousReplyDispatcherFailure CommitEndpointLocked(EndpointSendReservation reservation,
                                                                         Foundation::UInt64& sequence) noexcept;
    [[nodiscard]] Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>
    WakeAndPublishLocked(Scheduler::ReadyQueueEnqueueReservation reservation, Foundation::UInt64 endpointSequence,
                         SynchronousReplyDispatcherState terminalState) noexcept;
    [[nodiscard]] bool RollBackReservationLocked(EndpointSendReservation endpointReservation,
                                                 Scheduler::ReadyQueueEnqueueReservation queueReservation,
                                                 SynchronousReplyDispatcherFailure& failure) noexcept;
    [[nodiscard]] SynchronousReplyDispatcherReceipt
    MakeReceiptLocked(Foundation::UInt64 endpointSequence, Foundation::UInt64 queueSequence,
                      SynchronousReplyDispatcherState state) const noexcept;
    void ResetInitializationLocked() noexcept;
    void ReleaseTerminalPinsLocked() noexcept;
    void EnterFaultedLocked() noexcept;

    Object::ObjectRegistry& m_registry;
    Endpoint& m_replyEndpoint;
    Scheduler::ReadyQueue& m_wakeQueue;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Object::ObjectPin m_tokenPin;
    Object::ObjectPin m_callerPin;
    Object::ObjectPin m_contextPin;
    Object::ObjectPin m_serverPin;
    ReplyToken* m_token{nullptr};
    Scheduler::Thread* m_caller{nullptr};
    Scheduler::SchedulingContext* m_context{nullptr};
    Object::CapabilitySpace* m_serverSpace{nullptr};
    Scheduler::ThreadId m_callerIdentity{};
    Scheduler::SchedulingContextId m_contextIdentity{};
    Scheduler::ThreadId m_serverIdentity{};
    Scheduler::ThreadBlockToken m_blockToken{};
    Object::ResourceDomainId m_sender{};
    Foundation::UInt64 m_transactionIdentifier{0U};
    SynchronousReplyDispatcherState m_state{SynchronousReplyDispatcherState::Uninitialized};
};

static_assert(sizeof(SynchronousReplyDispatcherState) == 1U);
static_assert(sizeof(SynchronousReplyDispatcherError) == 1U);
static_assert(sizeof(SynchronousReplyDispatcherFailure) == 32U);
static_assert(alignof(SynchronousReplyDispatcherFailure) == 8U);
static_assert(sizeof(SynchronousReplyDispatcherReceipt) == 48U);
static_assert(alignof(SynchronousReplyDispatcherReceipt) == 8U);
static_assert(sizeof(SynchronousReplyDispatcherSnapshot) == 64U);
static_assert(alignof(SynchronousReplyDispatcherSnapshot) == 8U);
} // namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_SYNCHRONOUS_REPLY_DISPATCHER_HPP_INCLUDED
