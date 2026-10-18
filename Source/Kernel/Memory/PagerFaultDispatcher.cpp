// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/PagerFaultDispatcher.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
PagerFaultDispatcherFailure PagerFaultDispatcher::MakeFailure(
    const PagerFaultDispatcherError error, const Foundation::UInt64 transactionIdentifier, const FaultTokenError token,
    const Object::ObjectPinError pin, const Object::CapabilityError capability, const PagerProtocolError protocol,
    const Ipc::EndpointError& endpoint, const Scheduler::ReadyQueueFailure& queue, const Scheduler::ThreadError thread,
    const Scheduler::ReadyQueueError queueTransition) noexcept
{
    return PagerFaultDispatcherFailure{
        .TransactionIdentifier = transactionIdentifier,
        .Token = token,
        .Pin = pin,
        .Capability = capability,
        .Protocol = protocol,
        .Endpoint = endpoint,
        .Queue = queue,
        .Thread = thread,
        .QueueTransition = queueTransition,
        .Error = error,
        .Reserved = 0U,
    };
}

bool PagerFaultDispatcher::ContextCanResume(const Scheduler::SchedulingContextState state) noexcept
{
    switch (state)
    {
    case Scheduler::SchedulingContextState::Bound:
    case Scheduler::SchedulingContextState::Donated:
    case Scheduler::SchedulingContextState::BudgetExhausted:
        return true;

    case Scheduler::SchedulingContextState::Uninitialized:
    case Scheduler::SchedulingContextState::Unbound:
    case Scheduler::SchedulingContextState::Running:
        return false;
    }
    return false;
}

bool PagerFaultDispatcher::CorrelatedStateIsValidLocked() const noexcept
{
    if ((m_token == nullptr) || (m_thread == nullptr) || (m_context == nullptr))
    {
        return false;
    }

    const Scheduler::ThreadSnapshot thread = m_thread->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    return (thread.State == Scheduler::ThreadState::Blocked) && thread.BlockToken.IsEqualTo(m_blockToken) &&
           (thread.ExitCode == 0U) && !thread.RunningProcessor.IsValid() && ContextCanResume(context.State) &&
           context.CurrentThread.IsEqualTo(m_threadIdentity);
}

Foundation::Result<PagerFaultDispatcherReceipt, PagerFaultDispatcherFailure>
PagerFaultDispatcher::Deliver(const PagerFaultDeliveryRequest& request) noexcept
{
    using ResultType = Foundation::Result<PagerFaultDispatcherReceipt, PagerFaultDispatcherFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    PagerFaultDispatcherReceipt receipt{};
    PagerFaultDispatcherFailure failure{};
    if (!ValidateAndPinDeliveryLocked(request, failure))
    {
        if (m_state == PagerFaultDispatcherState::Idle)
        {
            ResetDeliveryLocked();
        }
        return ResultType::Failure(failure);
    }
    if (!SourceCapabilityIsExactLocked(request, failure) || !BuildDeliveryMessageLocked(request, failure) ||
        !PublishDeliveryLocked(request, receipt, failure))
    {
        ResetDeliveryLocked();
        return ResultType::Failure(failure);
    }
    return ResultType::Success(receipt);
}

bool PagerFaultDispatcher::ValidateAndPinDeliveryLocked(const PagerFaultDeliveryRequest& request,
                                                        PagerFaultDispatcherFailure& failure) noexcept
{
    if (m_state == PagerFaultDispatcherState::Faulted)
    {
        failure = MakeFailure(PagerFaultDispatcherError::Faulted, m_transactionIdentifier);
        return false;
    }
    if (m_state == PagerFaultDispatcherState::Delivered)
    {
        failure = MakeFailure(PagerFaultDispatcherError::AlreadyDelivered, m_transactionIdentifier);
        return false;
    }
    if (m_state == PagerFaultDispatcherState::Completed)
    {
        failure = MakeFailure(PagerFaultDispatcherError::AlreadyCompleted, m_transactionIdentifier);
        return false;
    }
    if (!m_pagerEndpoint.IsReady() || !m_wakeFacade.GetProcessor().IsValid() || (request.SourceSpace == nullptr) ||
        !request.SourceSpace->IsReady())
    {
        failure = MakeFailure(PagerFaultDispatcherError::InvalidConfiguration, request.TransactionIdentifier);
        return false;
    }
    if (!request.FaultToken.IsValid())
    {
        failure = MakeFailure(PagerFaultDispatcherError::InvalidToken, request.TransactionIdentifier);
        return false;
    }
    if (!request.SchedulingContext.IsValid())
    {
        failure = MakeFailure(PagerFaultDispatcherError::InvalidSchedulingContext, request.TransactionIdentifier);
        return false;
    }
    if (!request.SchedulingContext.IsFromRegistry(m_objectRegistry))
    {
        failure =
            MakeFailure(PagerFaultDispatcherError::SchedulingContextRegistryMismatch, request.TransactionIdentifier);
        return false;
    }
    if (!request.BlockToken.IsValid())
    {
        failure = MakeFailure(PagerFaultDispatcherError::CorrelatedStateChanged, request.TransactionIdentifier);
        return false;
    }
    if (!request.Sender.IsValid())
    {
        failure = MakeFailure(PagerFaultDispatcherError::InvalidSender, request.TransactionIdentifier);
        return false;
    }
    if (!request.FaultTokenCapability.IsValid())
    {
        failure = MakeFailure(PagerFaultDispatcherError::InvalidCapability, request.TransactionIdentifier);
        return false;
    }

    const Object::ObjectPinError tokenPinError =
        m_objectRegistry.PinObject(request.FaultToken, Object::ObjectType::FaultToken, m_tokenPin);
    if (tokenPinError != Object::ObjectPinError::Ok)
    {
        failure = MakeFailure(PagerFaultDispatcherError::TokenPinRejected, request.TransactionIdentifier,
                              FaultTokenError::None, tokenPinError);
        return false;
    }
    m_token = static_cast<FaultToken*>(&m_tokenPin.GetObject());
    const FaultTokenSnapshot tokenSnapshot = m_token->GetSnapshot();
    if ((tokenSnapshot.State != FaultTokenState::Pending) || !tokenSnapshot.Thread.IsValid())
    {
        failure = MakeFailure(PagerFaultDispatcherError::TokenNotPending, request.TransactionIdentifier);
        return false;
    }
    if (!tokenSnapshot.Thread.IsFromRegistry(m_objectRegistry))
    {
        failure = MakeFailure(PagerFaultDispatcherError::CorrelatedStateChanged, request.TransactionIdentifier);
        return false;
    }

    const Object::ObjectPinError threadPinError =
        m_objectRegistry.PinObject(tokenSnapshot.Thread.GetObjectId(), Object::ObjectType::Thread, m_threadPin);
    if (threadPinError != Object::ObjectPinError::Ok)
    {
        failure = MakeFailure(PagerFaultDispatcherError::ThreadPinRejected, request.TransactionIdentifier,
                              FaultTokenError::None, threadPinError);
        return false;
    }
    const Object::ObjectPinError contextPinError = m_objectRegistry.PinObject(
        request.SchedulingContext.GetObjectId(), Object::ObjectType::SchedulingContext, m_contextPin);
    if (contextPinError != Object::ObjectPinError::Ok)
    {
        failure = MakeFailure(PagerFaultDispatcherError::SchedulingContextPinRejected, request.TransactionIdentifier,
                              FaultTokenError::None, contextPinError);
        return false;
    }

    m_thread = static_cast<Scheduler::Thread*>(&m_threadPin.GetObject());
    m_context = static_cast<Scheduler::SchedulingContext*>(&m_contextPin.GetObject());
    m_threadIdentity = tokenSnapshot.Thread;
    m_contextIdentity = request.SchedulingContext;
    m_blockToken = request.BlockToken;
    m_tokenIdentity = request.FaultToken;
    m_fault = tokenSnapshot.Fault;
    m_transactionIdentifier = request.TransactionIdentifier;
    if (!CorrelatedStateIsValidLocked())
    {
        failure = MakeFailure(PagerFaultDispatcherError::CorrelatedStateChanged, request.TransactionIdentifier);
        return false;
    }
    return true;
}

bool PagerFaultDispatcher::SourceCapabilityIsExactLocked(const PagerFaultDeliveryRequest& request,
                                                         PagerFaultDispatcherFailure& failure) noexcept
{
    auto capability = request.SourceSpace->Lookup(request.FaultTokenCapability, Object::ObjectType::FaultToken,
                                                  Object::CapabilityOperation::Transfer);
    if (!capability.HasValue())
    {
        failure = MakeFailure(PagerFaultDispatcherError::CapabilityLookupRejected, request.TransactionIdentifier,
                              FaultTokenError::None, Object::ObjectPinError::Ok, capability.GetError());
        return false;
    }
    if ((capability.GetValue().GetDescriptor().Object.GetValue() != request.FaultToken.GetValue()) ||
        (&capability.GetValue().GetObject() != static_cast<Object::IKernelObject*>(m_token)))
    {
        failure = MakeFailure(PagerFaultDispatcherError::CapabilityObjectMismatch, request.TransactionIdentifier);
        return false;
    }
    return true;
}

bool PagerFaultDispatcher::BuildDeliveryMessageLocked(const PagerFaultDeliveryRequest& request,
                                                      PagerFaultDispatcherFailure& failure) noexcept
{
    const PagerProtocolError protocolError = PagerProtocol::BuildFaultNotification(
        PagerFaultNotification{
            .Fault = m_fault,
            .SupervisionTag = request.SupervisionTag,
            .ProtectionDomainIdentity = request.ProtectionDomainIdentity,
            .Trace = request.Trace,
            .FaultToken = request.FaultTokenCapability,
            .Deadline = request.Deadline,
            .TransactionIdentifier = request.TransactionIdentifier,
        },
        m_message);
    if (protocolError != PagerProtocolError::None)
    {
        failure =
            MakeFailure(PagerFaultDispatcherError::ProtocolRejected, request.TransactionIdentifier,
                        FaultTokenError::None, Object::ObjectPinError::Ok, Object::CapabilityError::Ok, protocolError);
        return false;
    }
    return true;
}

bool PagerFaultDispatcher::PublishDeliveryLocked(const PagerFaultDeliveryRequest& request,
                                                 PagerFaultDispatcherReceipt& receipt,
                                                 PagerFaultDispatcherFailure& failure) noexcept
{
    const auto sent =
        m_pagerEndpoint.Send(*request.SourceSpace, request.Sender, Ipc::EndpointSendClass::ReservedControl, m_message);
    if (!sent.HasValue())
    {
        failure = MakeFailure(PagerFaultDispatcherError::EndpointRejected, request.TransactionIdentifier,
                              FaultTokenError::None, Object::ObjectPinError::Ok, Object::CapabilityError::Ok,
                              PagerProtocolError::None, sent.GetError());
        return false;
    }

    m_endpointSequence = sent.GetValue().Sequence;
    m_resolution = FaultResolutionKind::None;
    m_state = PagerFaultDispatcherState::Delivered;
    ClearMessageLocked();
    receipt = MakeReceiptLocked(0U, FaultResolutionKind::None, m_state);
    return true;
}

Foundation::Result<PagerFaultDispatcherReceipt, PagerFaultDispatcherFailure>
PagerFaultDispatcher::Complete(const PagerFaultCompletionRequest& request) noexcept
{
    using ResultType = Foundation::Result<PagerFaultDispatcherReceipt, PagerFaultDispatcherFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    PagerFaultDispatcherReceipt receipt{};
    PagerFaultDispatcherFailure failure{};
    if (!ValidateCompletionLocked(request, failure))
    {
        return ResultType::Failure(failure);
    }
    if (request.Resolution == FaultResolutionKind::Terminate)
    {
        if (!CompleteTerminationLocked(request, receipt, failure))
        {
            return ResultType::Failure(failure);
        }
        return ResultType::Success(receipt);
    }

    Scheduler::ReadyQueueEnqueueReservation reservation{};
    if (!ReserveWakeLocked(reservation, failure) || !CommitWakeLocked(request, reservation, receipt, failure))
    {
        return ResultType::Failure(failure);
    }
    return ResultType::Success(receipt);
}

bool PagerFaultDispatcher::ValidateCompletionLocked(const PagerFaultCompletionRequest& request,
                                                    PagerFaultDispatcherFailure& failure) noexcept
{
    if (m_state == PagerFaultDispatcherState::Faulted)
    {
        failure = MakeFailure(PagerFaultDispatcherError::Faulted, m_transactionIdentifier);
        return false;
    }
    if (m_state == PagerFaultDispatcherState::Idle)
    {
        failure = MakeFailure(PagerFaultDispatcherError::NotDelivered);
        return false;
    }
    if (m_state == PagerFaultDispatcherState::Completed)
    {
        failure = MakeFailure(PagerFaultDispatcherError::AlreadyCompleted, m_transactionIdentifier);
        return false;
    }
    for (const Foundation::UInt8 reserved : request.Reserved)
    {
        if (reserved != 0U)
        {
            failure = MakeFailure(PagerFaultDispatcherError::InvalidResolution, m_transactionIdentifier,
                                  FaultTokenError::InvalidResolution);
            return false;
        }
    }
    if (((request.Resolution != FaultResolutionKind::Map) && (request.Resolution != FaultResolutionKind::Terminate) &&
         (request.Resolution != FaultResolutionKind::Redirect)) ||
        (((request.Resolution == FaultResolutionKind::Map) || (request.Resolution == FaultResolutionKind::Redirect)) &&
         (request.ExitCode != 0U)))
    {
        failure = MakeFailure(PagerFaultDispatcherError::InvalidResolution, m_transactionIdentifier,
                              FaultTokenError::InvalidResolution);
        return false;
    }
    if (!CorrelatedStateIsValidLocked())
    {
        EnterFaultedLocked();
        failure = MakeFailure(PagerFaultDispatcherError::CorrelatedStateChanged, m_transactionIdentifier);
        return false;
    }

    const auto authoritative = m_token->GetResolutionClaim(request.Claim);
    if (!authoritative.HasValue())
    {
        failure =
            MakeFailure(PagerFaultDispatcherError::TokenRejected, m_transactionIdentifier, authoritative.GetError());
        return false;
    }
    if (!authoritative.GetValue().Thread.IsEqualTo(m_threadIdentity))
    {
        EnterFaultedLocked();
        failure = MakeFailure(PagerFaultDispatcherError::CorrelatedStateChanged, m_transactionIdentifier);
        return false;
    }
    return true;
}

bool PagerFaultDispatcher::CompleteTerminationLocked(const PagerFaultCompletionRequest& request,
                                                     PagerFaultDispatcherReceipt& receipt,
                                                     PagerFaultDispatcherFailure& failure) noexcept
{
    const FaultTokenError tokenError = m_token->CommitResolution(request.Claim, request.Resolution);
    if (tokenError != FaultTokenError::None)
    {
        failure = MakeFailure(PagerFaultDispatcherError::TokenRejected, m_transactionIdentifier, tokenError);
        return false;
    }
    const Scheduler::ThreadError threadError = m_thread->Terminate(request.ExitCode);
    if (threadError != Scheduler::ThreadError::None)
    {
        EnterFaultedLocked();
        failure = MakeFailure(PagerFaultDispatcherError::ThreadRejected, m_transactionIdentifier, FaultTokenError::None,
                              Object::ObjectPinError::Ok, Object::CapabilityError::Ok, PagerProtocolError::None, {}, {},
                              threadError);
        return false;
    }

    m_resolution = request.Resolution;
    m_state = PagerFaultDispatcherState::Completed;
    receipt = MakeReceiptLocked(0U, request.Resolution, m_state);
    ReleaseTerminalPinsLocked();
    return true;
}

bool PagerFaultDispatcher::ReserveWakeLocked(Scheduler::ReadyQueueEnqueueReservation& reservation,
                                             PagerFaultDispatcherFailure& failure) noexcept
{
    const auto reserved =
        m_wakeFacade.ReserveBlockedEnqueue(m_contextIdentity, *m_context, m_threadIdentity, *m_thread, m_blockToken);
    if (!reserved.HasValue())
    {
        failure = MakeFailure(PagerFaultDispatcherError::QueueReservationRejected, m_transactionIdentifier,
                              FaultTokenError::None, Object::ObjectPinError::Ok, Object::CapabilityError::Ok,
                              PagerProtocolError::None, {}, reserved.GetError());
        return false;
    }
    reservation = reserved.GetValue();
    return true;
}

bool PagerFaultDispatcher::CommitWakeLocked(const PagerFaultCompletionRequest& request,
                                            const Scheduler::ReadyQueueEnqueueReservation reservation,
                                            PagerFaultDispatcherReceipt& receipt,
                                            PagerFaultDispatcherFailure& failure) noexcept
{
    const FaultTokenError tokenError = m_token->CommitResolution(request.Claim, request.Resolution);
    if (tokenError != FaultTokenError::None)
    {
        failure = MakeFailure(PagerFaultDispatcherError::TokenRejected, m_transactionIdentifier, tokenError);
        static_cast<void>(CancelReservationLocked(reservation, failure));
        return false;
    }

    const Scheduler::ThreadError threadError = m_thread->Wake(m_blockToken);
    if (threadError != Scheduler::ThreadError::None)
    {
        failure = MakeFailure(PagerFaultDispatcherError::ThreadRejected, m_transactionIdentifier, FaultTokenError::None,
                              Object::ObjectPinError::Ok, Object::CapabilityError::Ok, PagerProtocolError::None, {}, {},
                              threadError);
        static_cast<void>(CancelReservationLocked(reservation, failure));
        EnterFaultedLocked();
        return false;
    }

    const auto queued = m_wakeFacade.CommitReservedEnqueue(reservation);
    if (!queued.HasValue())
    {
        EnterFaultedLocked();
        failure = MakeFailure(PagerFaultDispatcherError::QueueCommitRejected, m_transactionIdentifier,
                              FaultTokenError::None, Object::ObjectPinError::Ok, Object::CapabilityError::Ok,
                              PagerProtocolError::None, {}, queued.GetError());
        return false;
    }

    m_resolution = request.Resolution;
    m_state = PagerFaultDispatcherState::Completed;
    receipt = MakeReceiptLocked(queued.GetValue().Sequence, request.Resolution, m_state);
    ReleaseTerminalPinsLocked();
    return true;
}

bool PagerFaultDispatcher::IsDelivered() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state == PagerFaultDispatcherState::Delivered;
}

PagerFaultDispatcherSnapshot PagerFaultDispatcher::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PagerFaultDispatcherSnapshot{
        .Thread = m_threadIdentity,
        .SchedulingContext = m_contextIdentity,
        .FaultToken = m_tokenIdentity,
        .TransactionIdentifier = m_transactionIdentifier,
        .EndpointSequence = m_endpointSequence,
        .Resolution = m_resolution,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

bool PagerFaultDispatcher::CancelReservationLocked(const Scheduler::ReadyQueueEnqueueReservation reservation,
                                                   PagerFaultDispatcherFailure& failure) noexcept
{
    const Scheduler::ReadyQueueError rollback = m_wakeFacade.CancelReservedEnqueue(reservation);
    if (rollback == Scheduler::ReadyQueueError::None)
    {
        return true;
    }
    failure.QueueTransition = rollback;
    failure.Error = PagerFaultDispatcherError::ReservationRollbackRejected;
    EnterFaultedLocked();
    return false;
}

PagerFaultDispatcherReceipt
PagerFaultDispatcher::MakeReceiptLocked(const Foundation::UInt64 queueSequence, const FaultResolutionKind resolution,
                                        const PagerFaultDispatcherState state) const noexcept
{
    return PagerFaultDispatcherReceipt{
        .Thread = m_threadIdentity,
        .FaultToken = m_tokenIdentity,
        .TransactionIdentifier = m_transactionIdentifier,
        .EndpointSequence = m_endpointSequence,
        .QueueSequence = queueSequence,
        .Resolution = resolution,
        .State = state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

void PagerFaultDispatcher::ClearMessageLocked() noexcept
{
    m_message.Interface = Ipc::InterfaceIdentifier{};
    m_message.TransactionIdentifier = 0U;
    m_message.Deadline = Object::MonotonicTime{};
    m_message.Trace = Ipc::TraceContext{};
    m_message.MethodIdentifier = 0U;
    m_message.InterfaceVersionMajor = 0U;
    m_message.InterfaceVersionMinor = 0U;
    m_message.FixedPayloadSizeBytes = 0U;
    m_message.PayloadSizeBytes = 0U;
    m_message.Kind = Ipc::MessageKind::Invalid;
    m_message.Cancellation = Ipc::CancellationState::None;
    m_message.VariablePayloadCount = 0U;
    m_message.AttachmentCount = 0U;
    m_message.Reserved = 0U;
    m_message.ReservedAlignment = 0U;
    for (Ipc::CapabilityAttachment& attachment : m_message.Attachments)
    {
        attachment = Ipc::CapabilityAttachment{};
    }
    for (Ipc::VariablePayloadDescriptor& descriptor : m_message.VariablePayloads)
    {
        descriptor = Ipc::VariablePayloadDescriptor{};
    }
    for (Foundation::UInt8& value : m_message.Payload)
    {
        value = 0U;
    }
}

void PagerFaultDispatcher::ResetDeliveryLocked() noexcept
{
    ClearMessageLocked();
    m_token = nullptr;
    m_thread = nullptr;
    m_context = nullptr;
    m_threadIdentity = Scheduler::ThreadId{};
    m_contextIdentity = Scheduler::SchedulingContextId{};
    m_blockToken = Scheduler::ThreadBlockToken{};
    m_tokenIdentity = Object::ObjectId{};
    m_fault = FaultRecord{};
    m_transactionIdentifier = 0U;
    m_endpointSequence = 0U;
    m_resolution = FaultResolutionKind::None;
    m_state = PagerFaultDispatcherState::Idle;
    m_contextPin.Reset();
    m_threadPin.Reset();
    m_tokenPin.Reset();
}

PagerFaultDispatcherError PagerFaultDispatcher::Reset() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PagerFaultDispatcherState::Completed)
    {
        return PagerFaultDispatcherError::NotDelivered;
    }
    ResetDeliveryLocked();
    return PagerFaultDispatcherError::None;
}

void PagerFaultDispatcher::ReleaseTerminalPinsLocked() noexcept
{
    m_token = nullptr;
    m_thread = nullptr;
    m_context = nullptr;
    m_blockToken = Scheduler::ThreadBlockToken{};
    m_contextPin.Reset();
    m_threadPin.Reset();
    m_tokenPin.Reset();
}

void PagerFaultDispatcher::EnterFaultedLocked() noexcept
{
    m_state = PagerFaultDispatcherState::Faulted;
}
} // namespace Asteria::Kernel::Memory
