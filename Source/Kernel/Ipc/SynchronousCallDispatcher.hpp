// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_SYNCHRONOUS_CALL_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_SYNCHRONOUS_CALL_DISPATCHER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/ReplyToken.hpp"
#include "Kernel/Ipc/SynchronousReplyDispatcher.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Ipc
{
enum class SynchronousCallDispatcherState : Foundation::UInt8
{
    Fresh,
    Prepared,
    ArmPending,
    ReplyInitializationPending,
    Committed,
    CancellationPending,
    Cancelled,
    Faulted,
};

enum class SynchronousCallDispatcherError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidCallerSpace,
    InvalidSender,
    InvalidCaller,
    CallerRegistryMismatch,
    InvalidSchedulingContext,
    SchedulingContextRegistryMismatch,
    InvalidCallEndpointCapability,
    InvalidReplyTokenCapability,
    InvalidCall,
    UnsupportedDeadline,
    TooManyAttachments,
    ReplyTokenAttachmentPresent,
    AlreadyPrepared,
    AlreadyCompleted,
    CallerPinRejected,
    SchedulingContextPinRejected,
    ReplyTokenPinRejected,
    ReplyTokenStorageMismatch,
    ReplyTokenNotAvailable,
    ResourceOwnerMismatch,
    CorrelatedStateChanged,
    WakeProcessorMismatch,
    CallEndpointCapabilityRejected,
    CallEndpointCapabilityMismatch,
    ReplyTokenCapabilityRejected,
    ReplyTokenCapabilityMismatch,
    MessageValidationRejected,
    InvalidBlockToken,
    ReplyTokenArmRejected,
    ReplyDispatcherRejected,
    EndpointSendRejected,
    ReplyTokenCapabilityCloseRejected,
    CancellationRejected,
    CancellationPending,
    InvalidState,
    Faulted,
};

struct SynchronousCallPrepareRequest
{
    Object::CapabilitySpace* CallerSpace;
    const MessageEnvelope* Call;
    Scheduler::ThreadId Caller;
    Scheduler::SchedulingContextId SchedulingContext;
    Object::CapabilityHandle CallEndpointCapability;
    Object::CapabilityHandle ReplyTokenCapability;
    Object::ResourceDomainId Sender;
};

struct SynchronousCallDispatcherFailure
{
    Foundation::UInt64 TransactionIdentifier;
    ReplyTokenFailure Token;
    Scheduler::ReadyQueueFailure Queue;
    EndpointError Endpoint;
    Object::CapabilityError Capability;
    Object::ObjectPinError Pin;
    Scheduler::ThreadError Thread;
    Scheduler::ReadyQueueError QueueTransition;
    SynchronousReplyDispatcherError Reply;
    SynchronousCallDispatcherError Error;
    Foundation::UInt8 Reserved[2];
};

struct SynchronousCallDispatcherSnapshot
{
    Scheduler::ThreadId Caller;
    Scheduler::SchedulingContextId SchedulingContext;
    Scheduler::ThreadBlockToken BlockToken;
    Foundation::UInt64 TransactionIdentifier;
    Foundation::UInt64 RunSequence;
    Foundation::UInt64 EndpointSequence;
    SynchronousCallDispatcherFailure Diagnostic;
    SynchronousCallDispatcherState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns one bounded caller-side synchronous IPC transaction. Prepare authenticates the exact
/// server-facing Endpoint Invoke capability, a preconfigured registered ReplyToken and its
/// temporary Transfer capability, snapshots a valid infinite-deadline Call, preflights reply
/// delivery configuration, and correlates the exact Running caller and SchedulingContext. It does
/// not mutate either Endpoint, any capability slot, the token, Thread, or context. CommitBlocked
/// accepts only the block token created when that captured run finishes as Blocked. It retries
/// bounded reference pressure while arming the ReplyToken and initializing
/// SynchronousReplyDispatcher, then atomically sends the Call with the token appended as a moved,
/// Invoke-only final attachment. The send authorizes the Endpoint capability again in the critical
/// section that publishes the Call, so a Revoke, Close, or expiry while the caller was blocked
/// fails it. A failed send changes no capability slot or Endpoint entry; the dispatcher closes its
/// private source token capability and asks SynchronousReplyDispatcher to cancel and wake the
/// caller. Retryable ready-queue pressure is retained for PollCancellation. The caller-side
/// dispatcher is one-shot after block commitment. It allocates no memory, stores one complete
/// normalized Call in owned fixed storage, may spin, is forbidden in interrupt context, and
/// requires same-CPU reentry plus direct lifecycle and caller-capability-space mutation to be
/// excluded.
class SynchronousCallDispatcher final
{
public:
    static constexpr Foundation::Size MaximumUserAttachments{MessageEnvelope::MaximumCapabilityAttachments - 1U};

    /// Preconditions:
    /// - registry owns replyTokenObject and every identity used by this call;
    /// - callEndpoint, replyToken, replyDispatcher, and registry outlive this
    /// object and remain immovable;
    /// - replyDispatcher was constructed with the caller reply Endpoint and
    /// the ReadyQueue for the processor that will run the caller.
    constexpr SynchronousCallDispatcher(Object::ObjectRegistry& registry, Endpoint& callEndpoint,
                                        ReplyToken& replyToken, Object::ObjectId replyTokenObject,
                                        SynchronousReplyDispatcher& replyDispatcher) noexcept
        : m_registry{registry}, m_callEndpoint{callEndpoint}, m_replyToken{replyToken},
          m_replyTokenObject{replyTokenObject}, m_replyDispatcher{replyDispatcher}
    {
    }

    /// Preconditions:
    /// - State is Fresh, Committed, Cancelled, or Faulted;
    /// - a Prepared transaction was cancelled explicitly;
    /// - a CancellationPending transaction was polled to completion or is
    /// retained by fatal-domain recovery;
    /// - no concurrent operation accesses this object.
    ~SynchronousCallDispatcher() noexcept = default;

    SynchronousCallDispatcher(const SynchronousCallDispatcher&) = delete;
    SynchronousCallDispatcher& operator=(const SynchronousCallDispatcher&) = delete;
    SynchronousCallDispatcher(SynchronousCallDispatcher&&) = delete;
    SynchronousCallDispatcher& operator=(SynchronousCallDispatcher&&) = delete;

    [[nodiscard]] SynchronousCallDispatcherFailure Prepare(const SynchronousCallPrepareRequest& request) noexcept;

    /// Abandons a pre-block Prepare. No capability or Endpoint mutation has
    /// occurred, so success restores Fresh and permits another Prepare.
    [[nodiscard]] SynchronousCallDispatcherFailure CancelPrepared() noexcept;

    [[nodiscard]] SynchronousCallDispatcherFailure CommitBlocked(Scheduler::ThreadBlockToken blockToken) noexcept;

    /// ProcessorKernelRunLoop completion observer. Accepted means either the
    /// Call is visible or its failed publication has already cancelled and
    /// enqueued the caller. RetryableRejected retains ArmPending,
    /// ReplyInitializationPending, or CancellationPending.
    [[nodiscard]] Scheduler::BlockedRunCommitOutcome
    CommitBlockedFromFinishedRun(Scheduler::ThreadBlockToken blockToken) noexcept;

    /// Advances retained post-block reference pressure or cancellation wake
    /// pressure without repeating any completed phase.
    [[nodiscard]] SynchronousCallDispatcherFailure Poll() noexcept;

    /// Retries only the retained caller wake after failed Call publication. A completed recovery
    /// reports EndpointSendRejected while Snapshot State is Cancelled; Error::None means that
    /// publication itself succeeded.
    [[nodiscard]] SynchronousCallDispatcherFailure PollCancellation() noexcept;

    [[nodiscard]] SynchronousCallDispatcherSnapshot GetSnapshot() const noexcept;

private:
    static void SetFailure(SynchronousCallDispatcherFailure& output, SynchronousCallDispatcherError error,
                           Foundation::UInt64 transactionIdentifier = 0U) noexcept;
    static void SetReplyFailureDetails(SynchronousCallDispatcherFailure& output,
                                       const SynchronousReplyDispatcherFailure& reply) noexcept;
    [[nodiscard]] SynchronousCallDispatcherError
    ValidateRequestLocked(const SynchronousCallPrepareRequest& request) const noexcept;
    [[nodiscard]] SynchronousCallDispatcherError
    PreflightReplyDispatcherLocked(SynchronousReplyDispatcherFailure& replyFailure) const noexcept;
    [[nodiscard]] SynchronousCallDispatcherError
    AuthenticateCallEndpointLocked(const SynchronousCallPrepareRequest& request,
                                   Object::CapabilityError& capabilityError) noexcept;
    [[nodiscard]] SynchronousCallDispatcherError PinAndCorrelateLocked(const SynchronousCallPrepareRequest& request,
                                                                       Object::ObjectPinError& pinError) noexcept;
    [[nodiscard]] SynchronousCallDispatcherError
    ValidateReplyTokenCapabilityLocked(const SynchronousCallPrepareRequest& request,
                                       Object::CapabilityError& capabilityError) noexcept;
    [[nodiscard]] SynchronousCallDispatcherError StageCallLocked(const SynchronousCallPrepareRequest& request,
                                                                 EndpointError& endpointError) noexcept;
    [[nodiscard]] bool CandidateBlockIsCorrelatedLocked(Scheduler::ThreadBlockToken blockToken,
                                                        bool& tokenMismatch) const noexcept;
    [[nodiscard]] SynchronousCallDispatcherFailure CommitBlockedLocked(Scheduler::ThreadBlockToken blockToken) noexcept;
    [[nodiscard]] SynchronousCallDispatcherFailure ProgressBlockedCommitLocked() noexcept;
    [[nodiscard]] SynchronousCallDispatcherFailure
    CancelAfterSendFailureLocked(const EndpointError& endpointFailure) noexcept;
    [[nodiscard]] SynchronousCallDispatcherFailure PollCancellationLocked() noexcept;
    [[nodiscard]] static bool CancellationFailureIsRetryable(const SynchronousReplyDispatcherFailure& failure) noexcept;
    [[nodiscard]] static bool ReplyTokenArmFailureIsRetryable(const ReplyTokenFailure& failure) noexcept;
    [[nodiscard]] static bool
    ReplyInitializationFailureIsRetryable(const SynchronousReplyDispatcherFailure& failure) noexcept;
    [[nodiscard]] static bool IsRetryPendingState(SynchronousCallDispatcherState state) noexcept;
    void ClearStagedCallLocked() noexcept;
    void ClearValidationFrameLocked() noexcept;
    void ResetPreparationLocked() noexcept;
    void ReleasePreparationPinsLocked() noexcept;
    void EnterFaultedLocked(const SynchronousCallDispatcherFailure& failure) noexcept;

    Object::ObjectRegistry& m_registry;
    Endpoint& m_callEndpoint;
    ReplyToken& m_replyToken;
    Object::ObjectId m_replyTokenObject;
    SynchronousReplyDispatcher& m_replyDispatcher;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Object::ObjectPin m_callerPin;
    Object::ObjectPin m_contextPin;
    Object::ObjectPin m_tokenPin;
    Object::CapabilitySpace* m_callerSpace{nullptr};
    Scheduler::Thread* m_caller{nullptr};
    Scheduler::SchedulingContext* m_context{nullptr};
    Scheduler::ThreadId m_callerIdentity{};
    Scheduler::SchedulingContextId m_contextIdentity{};
    Scheduler::ThreadBlockToken m_blockToken{};
    Object::CapabilityHandle m_callEndpointCapability{};
    Object::CapabilityHandle m_replyTokenCapability{};
    Object::ResourceDomainId m_sender{};
    Foundation::UInt64 m_transactionIdentifier{0U};
    Foundation::UInt64 m_runSequence{0U};
    Foundation::UInt64 m_endpointSequence{0U};
    MessageEnvelope m_stagedCall{};
    Foundation::UInt8 m_validationFrame[MessageCodec::MaximumWireSizeBytes]{};
    SynchronousCallDispatcherFailure m_diagnostic{};
    SynchronousCallDispatcherState m_state{SynchronousCallDispatcherState::Fresh};
};

static_assert(sizeof(SynchronousCallDispatcherState) == 1U);
static_assert(sizeof(SynchronousCallDispatcherError) == 1U);
static_assert(sizeof(SynchronousCallDispatcherFailure) == 32U);
static_assert(alignof(SynchronousCallDispatcherFailure) == 8U);
static_assert(sizeof(SynchronousCallDispatcherSnapshot) == 112U);
static_assert(alignof(SynchronousCallDispatcherSnapshot) == 8U);
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_SYNCHRONOUS_CALL_DISPATCHER_HPP_INCLUDED
