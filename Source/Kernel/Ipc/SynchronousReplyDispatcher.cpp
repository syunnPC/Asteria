// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/SynchronousReplyDispatcher.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Ipc
{
SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::MakeFailure(
    const SynchronousReplyDispatcherError error, const Foundation::UInt64 transactionIdentifier,
    const Object::ObjectPinError pin, const ReplyTokenFailure& token, const Scheduler::ReadyQueueFailure& queue,
    const EndpointError& endpoint, const Scheduler::ThreadError thread,
    const Scheduler::ReadyQueueError queueTransition, const Object::CapabilityError capability) noexcept
{
    return SynchronousReplyDispatcherFailure{
        .TransactionIdentifier = transactionIdentifier,
        .Token = token,
        .Queue = queue,
        .Endpoint = endpoint,
        .Pin = pin,
        .Thread = thread,
        .QueueTransition = queueTransition,
        .Error = error,
        .Capability = capability,
        .Reserved = {0U, 0U, 0U},
    };
}

SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::AuthenticateReplyCapabilityLocked(
    Object::CapabilitySpace& serverSpace, const Object::CapabilityHandle replyCapability) const noexcept
{
    const Object::CapabilityLookupResult authority =
        serverSpace.Lookup(replyCapability, Object::ObjectType::ReplyToken, Object::CapabilityOperation::Invoke);
    if (!authority.HasValue())
    {
        return MakeFailure(SynchronousReplyDispatcherError::ReplyCapabilityRejected, m_transactionIdentifier,
                           Object::ObjectPinError::Ok, {}, {}, {}, Scheduler::ThreadError::None,
                           Scheduler::ReadyQueueError::None, authority.GetError());
    }
    if (&authority.GetValue().GetObject() != m_token)
    {
        return MakeFailure(SynchronousReplyDispatcherError::ReplyCapabilityMismatch, m_transactionIdentifier);
    }
    return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
}

SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::Initialize(const Object::ObjectId replyToken) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const SynchronousReplyDispatcherFailure preflight = PreflightInitializeLocked();
    if (preflight.Error != SynchronousReplyDispatcherError::None)
    {
        return preflight;
    }
    if (!replyToken.IsValid())
    {
        return MakeFailure(SynchronousReplyDispatcherError::InvalidToken);
    }

    const Object::ObjectPinError tokenPinError =
        m_registry.PinObject(replyToken, Object::ObjectType::ReplyToken, m_tokenPin);
    if (tokenPinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(SynchronousReplyDispatcherError::TokenPinRejected, 0U, tokenPinError);
    }
    m_token = static_cast<ReplyToken*>(&m_tokenPin.GetObject());
    {
        const ReplyTokenSnapshot tokenSnapshot = m_token->GetSnapshot();
        if ((tokenSnapshot.State != ReplyTokenState::Pending) || !tokenSnapshot.Caller.IsValid() ||
            !tokenSnapshot.SchedulingContext.IsValid() || !tokenSnapshot.BlockToken.IsValid() ||
            (tokenSnapshot.TransactionIdentifier == 0U))
        {
            const Foundation::UInt64 diagnosticTransaction = tokenSnapshot.TransactionIdentifier;
            ResetInitializationLocked();
            return MakeFailure(SynchronousReplyDispatcherError::TokenNotPending, diagnosticTransaction);
        }
        if (!tokenSnapshot.Caller.IsFromRegistry(m_registry) ||
            !tokenSnapshot.SchedulingContext.IsFromRegistry(m_registry))
        {
            const Foundation::UInt64 diagnosticTransaction = tokenSnapshot.TransactionIdentifier;
            ResetInitializationLocked();
            return MakeFailure(SynchronousReplyDispatcherError::CorrelatedStateChanged, diagnosticTransaction);
        }

        m_callerIdentity = tokenSnapshot.Caller;
        m_contextIdentity = tokenSnapshot.SchedulingContext;
        m_blockToken = tokenSnapshot.BlockToken;
        m_transactionIdentifier = tokenSnapshot.TransactionIdentifier;
    }

    const Object::ObjectPinError callerPinError =
        m_registry.PinObject(m_callerIdentity.GetObjectId(), Object::ObjectType::Thread, m_callerPin);
    if (callerPinError != Object::ObjectPinError::Ok)
    {
        const Foundation::UInt64 diagnosticTransaction = m_transactionIdentifier;
        ResetInitializationLocked();
        return MakeFailure(SynchronousReplyDispatcherError::CallerPinRejected, diagnosticTransaction, callerPinError);
    }
    const Object::ObjectPinError contextPinError =
        m_registry.PinObject(m_contextIdentity.GetObjectId(), Object::ObjectType::SchedulingContext, m_contextPin);
    if (contextPinError != Object::ObjectPinError::Ok)
    {
        const Foundation::UInt64 diagnosticTransaction = m_transactionIdentifier;
        ResetInitializationLocked();
        return MakeFailure(SynchronousReplyDispatcherError::ContextPinRejected, diagnosticTransaction, contextPinError);
    }

    m_caller = static_cast<Scheduler::Thread*>(&m_callerPin.GetObject());
    m_context = static_cast<Scheduler::SchedulingContext*>(&m_contextPin.GetObject());
    {
        const Scheduler::ThreadSnapshot callerSnapshot = m_caller->GetSnapshot();
        if ((callerSnapshot.State != Scheduler::ThreadState::Blocked) ||
            !callerSnapshot.BlockToken.IsEqualTo(m_blockToken))
        {
            const Foundation::UInt64 diagnosticTransaction = m_transactionIdentifier;
            ResetInitializationLocked();
            return MakeFailure(SynchronousReplyDispatcherError::CorrelatedStateChanged, diagnosticTransaction);
        }
    }
    {
        const Scheduler::SchedulingContextSnapshot contextSnapshot = m_context->GetSnapshot();
        if (!contextSnapshot.CurrentThread.IsEqualTo(m_callerIdentity) ||
            (contextSnapshot.State == Scheduler::SchedulingContextState::Running))
        {
            const Foundation::UInt64 diagnosticTransaction = m_transactionIdentifier;
            ResetInitializationLocked();
            return MakeFailure(SynchronousReplyDispatcherError::CorrelatedStateChanged, diagnosticTransaction);
        }
    }

    m_state = SynchronousReplyDispatcherState::Pending;
    return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
}

SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::PreflightInitialize() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PreflightInitializeLocked();
}

SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::PreflightInitializeLocked() const noexcept
{
    if (m_state != SynchronousReplyDispatcherState::Uninitialized)
    {
        return MakeFailure(SynchronousReplyDispatcherError::AlreadyInitialized, m_transactionIdentifier);
    }
    if (!m_replyEndpoint.IsReady() || !m_wakeQueue.GetState().Processor.IsValid())
    {
        return MakeFailure(SynchronousReplyDispatcherError::InvalidConfiguration);
    }
    return MakeFailure(SynchronousReplyDispatcherError::None);
}

SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::Accept(const Scheduler::ThreadId server,
                                                                     Object::CapabilitySpace& serverSpace,
                                                                     const Object::CapabilityHandle replyCapability,
                                                                     const Object::ResourceDomainId sender) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == SynchronousReplyDispatcherState::Faulted)
    {
        return MakeFailure(SynchronousReplyDispatcherError::Faulted, m_transactionIdentifier);
    }
    if (m_state == SynchronousReplyDispatcherState::Uninitialized)
    {
        return MakeFailure(SynchronousReplyDispatcherError::NotInitialized);
    }
    if ((m_state == SynchronousReplyDispatcherState::Replied) ||
        (m_state == SynchronousReplyDispatcherState::Cancelled))
    {
        return MakeFailure(SynchronousReplyDispatcherError::AlreadyCompleted, m_transactionIdentifier);
    }
    if (m_state == SynchronousReplyDispatcherState::Accepted)
    {
        return MakeFailure(SynchronousReplyDispatcherError::AlreadyAccepted, m_transactionIdentifier);
    }
    if (!server.IsValid())
    {
        return MakeFailure(SynchronousReplyDispatcherError::InvalidServer, m_transactionIdentifier);
    }
    if (!server.IsFromRegistry(m_registry))
    {
        return MakeFailure(SynchronousReplyDispatcherError::ServerRegistryMismatch, m_transactionIdentifier);
    }
    if (!sender.IsValid() || !serverSpace.IsReady())
    {
        return MakeFailure(SynchronousReplyDispatcherError::InvalidSender, m_transactionIdentifier);
    }
    const SynchronousReplyDispatcherFailure authenticated =
        AuthenticateReplyCapabilityLocked(serverSpace, replyCapability);
    if (authenticated.Error != SynchronousReplyDispatcherError::None)
    {
        return authenticated;
    }

    const Object::ObjectPinError serverPinError =
        m_registry.PinObject(server.GetObjectId(), Object::ObjectType::Thread, m_serverPin);
    if (serverPinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(SynchronousReplyDispatcherError::ServerPinRejected, m_transactionIdentifier, serverPinError);
    }
    Scheduler::Thread& serverThread = static_cast<Scheduler::Thread&>(m_serverPin.GetObject());
    if (serverThread.GetChargeOwner().GetValue() != sender.GetValue())
    {
        m_serverPin.Reset();
        return MakeFailure(SynchronousReplyDispatcherError::ServerOwnerMismatch, m_transactionIdentifier);
    }

    const ReplyTokenFailure accepted = m_token->Accept(server);
    if (accepted.Error != ReplyTokenError::None)
    {
        m_serverPin.Reset();
        return MakeFailure(SynchronousReplyDispatcherError::TokenRejected, m_transactionIdentifier,
                           Object::ObjectPinError::Ok, accepted);
    }

    // Acceptance consumes the one-shot reply authority. Only a concurrent
    // Close by the server itself can make this fail, and then the capability
    // is equally gone, so the result carries no information.
    static_cast<void>(serverSpace.Close(replyCapability));
    m_serverIdentity = server;
    m_serverSpace = &serverSpace;
    m_sender = sender;
    m_state = SynchronousReplyDispatcherState::Accepted;
    return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
}

Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>
SynchronousReplyDispatcher::CompleteReply(const MessageEnvelope& reply) noexcept
{
    using ResultType = Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == SynchronousReplyDispatcherState::Faulted)
    {
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::Faulted, m_transactionIdentifier));
    }
    if (m_state == SynchronousReplyDispatcherState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::NotInitialized));
    }
    if ((m_state == SynchronousReplyDispatcherState::Replied) ||
        (m_state == SynchronousReplyDispatcherState::Cancelled))
    {
        return ResultType::Failure(
            MakeFailure(SynchronousReplyDispatcherError::AlreadyCompleted, m_transactionIdentifier));
    }
    if (m_state != SynchronousReplyDispatcherState::Accepted)
    {
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::NotAccepted, m_transactionIdentifier));
    }
    if (!ServerIsStoppedLocked())
    {
        return ResultType::Failure(
            MakeFailure(SynchronousReplyDispatcherError::ServerStillRunning, m_transactionIdentifier));
    }
    if ((reply.Kind != MessageKind::Reply) || (reply.Cancellation != CancellationState::None) ||
        (reply.TransactionIdentifier != m_transactionIdentifier))
    {
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::InvalidReply, m_transactionIdentifier));
    }

    Scheduler::ReadyQueueEnqueueReservation queueReservation{};
    SynchronousReplyDispatcherFailure failure = ReserveCallerLocked(queueReservation);
    if (failure.Error != SynchronousReplyDispatcherError::None)
    {
        return ResultType::Failure(failure);
    }

    EndpointSendReservation endpointReservation{};
    failure = PrepareEndpointLocked(reply, queueReservation, endpointReservation);
    if (failure.Error != SynchronousReplyDispatcherError::None)
    {
        return ResultType::Failure(failure);
    }

    failure = ConsumeReplyTokenLocked(endpointReservation, queueReservation);
    if (failure.Error != SynchronousReplyDispatcherError::None)
    {
        return ResultType::Failure(failure);
    }

    Foundation::UInt64 endpointSequence{0U};
    failure = CommitEndpointLocked(endpointReservation, endpointSequence);
    if (failure.Error != SynchronousReplyDispatcherError::None)
    {
        return ResultType::Failure(failure);
    }
    return WakeAndPublishLocked(queueReservation, endpointSequence, SynchronousReplyDispatcherState::Replied);
}

Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>
SynchronousReplyDispatcher::Cancel() noexcept
{
    using ResultType = Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == SynchronousReplyDispatcherState::Faulted)
    {
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::Faulted, m_transactionIdentifier));
    }
    if (m_state == SynchronousReplyDispatcherState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::NotInitialized));
    }
    if ((m_state == SynchronousReplyDispatcherState::Replied) ||
        (m_state == SynchronousReplyDispatcherState::Cancelled))
    {
        return ResultType::Failure(
            MakeFailure(SynchronousReplyDispatcherError::AlreadyCompleted, m_transactionIdentifier));
    }
    if ((m_state == SynchronousReplyDispatcherState::Accepted) && !ServerIsStoppedLocked())
    {
        return ResultType::Failure(
            MakeFailure(SynchronousReplyDispatcherError::ServerStillRunning, m_transactionIdentifier));
    }

    Scheduler::ReadyQueueEnqueueReservation queueReservation{};
    SynchronousReplyDispatcherFailure failure = ReserveCallerLocked(queueReservation);
    if (failure.Error != SynchronousReplyDispatcherError::None)
    {
        return ResultType::Failure(failure);
    }

    failure = ConsumeCancellationTokenLocked(queueReservation);
    if (failure.Error != SynchronousReplyDispatcherError::None)
    {
        return ResultType::Failure(failure);
    }
    return WakeAndPublishLocked(queueReservation, 0U, SynchronousReplyDispatcherState::Cancelled);
}

bool SynchronousReplyDispatcher::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state != SynchronousReplyDispatcherState::Uninitialized;
}

Scheduler::CpuId SynchronousReplyDispatcher::GetWakeProcessor() const noexcept
{
    return m_wakeQueue.GetState().Processor;
}

SynchronousReplyDispatcherSnapshot SynchronousReplyDispatcher::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return SynchronousReplyDispatcherSnapshot{
        .Caller = m_callerIdentity,
        .SchedulingContext = m_contextIdentity,
        .Server = m_serverIdentity,
        .TransactionIdentifier = m_transactionIdentifier,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

SynchronousReplyDispatcherFailure
SynchronousReplyDispatcher::ReserveCallerLocked(Scheduler::ReadyQueueEnqueueReservation& reservation) noexcept
{
    const auto result =
        m_wakeQueue.ReserveBlockedEnqueue(m_contextIdentity, *m_context, m_callerIdentity, *m_caller, m_blockToken);
    if (!result.HasValue())
    {
        return MakeFailure(SynchronousReplyDispatcherError::QueueReservationRejected, m_transactionIdentifier,
                           Object::ObjectPinError::Ok, {}, result.GetError());
    }
    reservation = result.GetValue();
    return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
}

SynchronousReplyDispatcherFailure
SynchronousReplyDispatcher::PrepareEndpointLocked(const MessageEnvelope& reply,
                                                  const Scheduler::ReadyQueueEnqueueReservation queueReservation,
                                                  EndpointSendReservation& endpointReservation) noexcept
{
    const auto result = m_replyEndpoint.PrepareSend(*m_serverSpace, m_sender, EndpointSendClass::Normal, reply);
    if (result.HasValue())
    {
        endpointReservation = result.GetValue();
        return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
    }

    SynchronousReplyDispatcherFailure failure =
        MakeFailure(SynchronousReplyDispatcherError::EndpointPreparationRejected, m_transactionIdentifier,
                    Object::ObjectPinError::Ok, {}, {}, result.GetError());
    static_cast<void>(RollBackReservationLocked({}, queueReservation, failure));
    return failure;
}

SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::ConsumeReplyTokenLocked(
    const EndpointSendReservation endpointReservation,
    const Scheduler::ReadyQueueEnqueueReservation queueReservation) noexcept
{
    const auto completion = m_token->CompleteReply(m_serverIdentity, m_transactionIdentifier);
    if (!completion.HasValue())
    {
        SynchronousReplyDispatcherFailure failure =
            MakeFailure(SynchronousReplyDispatcherError::TokenRejected, m_transactionIdentifier,
                        Object::ObjectPinError::Ok, completion.GetError());
        static_cast<void>(RollBackReservationLocked(endpointReservation, queueReservation, failure));
        EnterFaultedLocked();
        return failure;
    }
    if (!completion.GetValue().Caller.IsEqualTo(m_callerIdentity) ||
        !completion.GetValue().BlockToken.IsEqualTo(m_blockToken) ||
        (completion.GetValue().TransactionIdentifier != m_transactionIdentifier) ||
        (completion.GetValue().State != ReplyTokenState::Replied))
    {
        SynchronousReplyDispatcherFailure failure =
            MakeFailure(SynchronousReplyDispatcherError::CorrelatedStateChanged, m_transactionIdentifier);
        static_cast<void>(RollBackReservationLocked(endpointReservation, queueReservation, failure));
        EnterFaultedLocked();
        return failure;
    }
    return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
}

SynchronousReplyDispatcherFailure SynchronousReplyDispatcher::ConsumeCancellationTokenLocked(
    const Scheduler::ReadyQueueEnqueueReservation queueReservation) noexcept
{
    const auto completion = m_token->Cancel();
    if (!completion.HasValue())
    {
        SynchronousReplyDispatcherFailure failure =
            MakeFailure(SynchronousReplyDispatcherError::TokenRejected, m_transactionIdentifier,
                        Object::ObjectPinError::Ok, completion.GetError());
        static_cast<void>(RollBackReservationLocked({}, queueReservation, failure));
        return failure;
    }
    if (!completion.GetValue().Caller.IsEqualTo(m_callerIdentity) ||
        !completion.GetValue().BlockToken.IsEqualTo(m_blockToken) ||
        (completion.GetValue().TransactionIdentifier != m_transactionIdentifier) ||
        (completion.GetValue().State != ReplyTokenState::Cancelled))
    {
        EnterFaultedLocked();
        return MakeFailure(SynchronousReplyDispatcherError::CorrelatedStateChanged, m_transactionIdentifier);
    }
    return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
}

SynchronousReplyDispatcherFailure
SynchronousReplyDispatcher::CommitEndpointLocked(const EndpointSendReservation reservation,
                                                 Foundation::UInt64& sequence) noexcept
{
    const auto result = m_replyEndpoint.CommitPreparedSend(reservation);
    if (!result.HasValue())
    {
        EnterFaultedLocked();
        return MakeFailure(SynchronousReplyDispatcherError::EndpointCommitRejected, m_transactionIdentifier,
                           Object::ObjectPinError::Ok, {}, {}, result.GetError());
    }
    sequence = result.GetValue().Sequence;
    return MakeFailure(SynchronousReplyDispatcherError::None, m_transactionIdentifier);
}

Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>
SynchronousReplyDispatcher::WakeAndPublishLocked(const Scheduler::ReadyQueueEnqueueReservation reservation,
                                                 const Foundation::UInt64 endpointSequence,
                                                 const SynchronousReplyDispatcherState terminalState) noexcept
{
    using ResultType = Foundation::Result<SynchronousReplyDispatcherReceipt, SynchronousReplyDispatcherFailure>;

    const Scheduler::ThreadError wakeError = m_caller->Wake(m_blockToken);
    if (wakeError != Scheduler::ThreadError::None)
    {
        EnterFaultedLocked();
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::WakeRejected, m_transactionIdentifier,
                                               Object::ObjectPinError::Ok, {}, {}, {}, wakeError));
    }

    const auto queueCommit = m_wakeQueue.CommitReservedEnqueue(reservation);
    if (!queueCommit.HasValue())
    {
        EnterFaultedLocked();
        return ResultType::Failure(MakeFailure(SynchronousReplyDispatcherError::QueueCommitRejected,
                                               m_transactionIdentifier, Object::ObjectPinError::Ok, {},
                                               queueCommit.GetError()));
    }

    m_state = terminalState;
    const SynchronousReplyDispatcherReceipt receipt =
        MakeReceiptLocked(endpointSequence, queueCommit.GetValue().Sequence, m_state);
    ReleaseTerminalPinsLocked();
    return ResultType::Success(receipt);
}

bool SynchronousReplyDispatcher::ServerIsStoppedLocked() const noexcept
{
    if ((m_state != SynchronousReplyDispatcherState::Accepted) || !m_serverPin.IsValid() || (m_context == nullptr))
    {
        return false;
    }
    const Scheduler::Thread& server = static_cast<const Scheduler::Thread&>(m_serverPin.GetObject());
    const Scheduler::ThreadSnapshot serverSnapshot = server.GetSnapshot();
    const Scheduler::SchedulingContextSnapshot contextSnapshot = m_context->GetSnapshot();
    return (serverSnapshot.State != Scheduler::ThreadState::Running) &&
           (contextSnapshot.State != Scheduler::SchedulingContextState::Running) &&
           contextSnapshot.CurrentThread.IsEqualTo(m_serverIdentity);
}

bool SynchronousReplyDispatcher::RollBackReservationLocked(
    const EndpointSendReservation endpointReservation, const Scheduler::ReadyQueueEnqueueReservation queueReservation,
    SynchronousReplyDispatcherFailure& failure) noexcept
{
    bool restored{true};
    if (endpointReservation.IsValid())
    {
        const EndpointError endpointError = m_replyEndpoint.CancelPreparedSend(endpointReservation);
        if (endpointError.Code != EndpointErrorCode::None)
        {
            failure.Endpoint = endpointError;
            restored = false;
        }
    }
    if (queueReservation.IsValid())
    {
        const Scheduler::ReadyQueueError queueError = m_wakeQueue.CancelReservedEnqueue(queueReservation);
        if (queueError != Scheduler::ReadyQueueError::None)
        {
            failure.QueueTransition = queueError;
            restored = false;
        }
    }
    if (!restored)
    {
        failure.Error = SynchronousReplyDispatcherError::ReservationRollbackRejected;
        EnterFaultedLocked();
    }
    return restored;
}

SynchronousReplyDispatcherReceipt
SynchronousReplyDispatcher::MakeReceiptLocked(const Foundation::UInt64 endpointSequence,
                                              const Foundation::UInt64 queueSequence,
                                              const SynchronousReplyDispatcherState state) const noexcept
{
    return SynchronousReplyDispatcherReceipt{
        .Caller = m_callerIdentity,
        .TransactionIdentifier = m_transactionIdentifier,
        .EndpointSequence = endpointSequence,
        .QueueSequence = queueSequence,
        .State = state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

void SynchronousReplyDispatcher::ResetInitializationLocked() noexcept
{
    m_contextPin.Reset();
    m_callerPin.Reset();
    m_tokenPin.Reset();
    m_token = nullptr;
    m_caller = nullptr;
    m_context = nullptr;
    m_serverSpace = nullptr;
    m_callerIdentity = Scheduler::ThreadId{};
    m_contextIdentity = Scheduler::SchedulingContextId{};
    m_serverIdentity = Scheduler::ThreadId{};
    m_blockToken = Scheduler::ThreadBlockToken{};
    m_sender = Object::ResourceDomainId{};
    m_transactionIdentifier = 0U;
}

void SynchronousReplyDispatcher::ReleaseTerminalPinsLocked() noexcept
{
    m_serverPin.Reset();
    m_contextPin.Reset();
    m_callerPin.Reset();
    m_tokenPin.Reset();
    m_serverSpace = nullptr;
    m_context = nullptr;
    m_caller = nullptr;
    m_token = nullptr;
}

void SynchronousReplyDispatcher::EnterFaultedLocked() noexcept
{
    m_state = SynchronousReplyDispatcherState::Faulted;
}
} // namespace Asteria::Kernel::Ipc
