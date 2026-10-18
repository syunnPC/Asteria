// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/SynchronousCallDispatcher.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Ipc
{
namespace
{
[[nodiscard]] constexpr bool ContextCanRemainBlocked(const Scheduler::SchedulingContextState state) noexcept
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
} // namespace

void SynchronousCallDispatcher::SetFailure(SynchronousCallDispatcherFailure& output,
                                           const SynchronousCallDispatcherError error,
                                           const Foundation::UInt64 transactionIdentifier) noexcept
{
    Foundation::UInt8* const bytes = reinterpret_cast<Foundation::UInt8*>(&output);
    for (Foundation::Size index{0U}; index < sizeof(output); ++index)
    {
        bytes[index] = 0U;
    }
    output.TransactionIdentifier = transactionIdentifier;
    output.Error = error;
}

void SynchronousCallDispatcher::SetReplyFailureDetails(SynchronousCallDispatcherFailure& output,
                                                       const SynchronousReplyDispatcherFailure& reply) noexcept
{
    output.Reply = reply.Error;
    output.Token = reply.Token;
    output.Queue = reply.Queue;
    output.Pin = reply.Pin;
    output.Thread = reply.Thread;
    output.QueueTransition = reply.QueueTransition;
}

SynchronousCallDispatcherFailure
SynchronousCallDispatcher::Prepare(const SynchronousCallPrepareRequest& request) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state == SynchronousCallDispatcherState::Faulted) || IsRetryPendingState(m_state))
    {
        return m_diagnostic;
    }
    if (m_state != SynchronousCallDispatcherState::Fresh)
    {
        SynchronousCallDispatcherFailure failure{};
        SetFailure(failure,
                   m_state == SynchronousCallDispatcherState::Prepared
                       ? SynchronousCallDispatcherError::AlreadyPrepared
                       : SynchronousCallDispatcherError::AlreadyCompleted,
                   m_transactionIdentifier);
        return failure;
    }
    Object::ObjectPinError pinError{Object::ObjectPinError::Ok};
    Object::CapabilityError capabilityError{Object::CapabilityError::Ok};
    EndpointError endpointError{};
    SynchronousReplyDispatcherFailure replyFailure{};
    SynchronousCallDispatcherError error = ValidateRequestLocked(request);
    if (error == SynchronousCallDispatcherError::None)
    {
        error = PreflightReplyDispatcherLocked(replyFailure);
    }
    if (error == SynchronousCallDispatcherError::None)
    {
        error = AuthenticateCallEndpointLocked(request, capabilityError);
    }
    if (error == SynchronousCallDispatcherError::None)
    {
        error = PinAndCorrelateLocked(request, pinError);
    }
    if (error == SynchronousCallDispatcherError::None)
    {
        error = ValidateReplyTokenCapabilityLocked(request, capabilityError);
    }
    if (error == SynchronousCallDispatcherError::None)
    {
        error = StageCallLocked(request, endpointError);
    }
    if (error != SynchronousCallDispatcherError::None)
    {
        const Foundation::UInt64 transactionIdentifier =
            request.Call == nullptr ? 0U : request.Call->TransactionIdentifier;
        ResetPreparationLocked();
        SetFailure(m_diagnostic, error, transactionIdentifier);
        m_diagnostic.Pin = pinError;
        m_diagnostic.Capability = capabilityError;
        m_diagnostic.Endpoint = endpointError;
        SetReplyFailureDetails(m_diagnostic, replyFailure);
        return m_diagnostic;
    }

    m_callerSpace = request.CallerSpace;
    m_callerIdentity = request.Caller;
    m_contextIdentity = request.SchedulingContext;
    m_callEndpointCapability = request.CallEndpointCapability;
    m_replyTokenCapability = request.ReplyTokenCapability;
    m_sender = request.Sender;
    m_transactionIdentifier = request.Call->TransactionIdentifier;
    m_endpointSequence = 0U;
    m_blockToken = {};
    SetFailure(m_diagnostic, SynchronousCallDispatcherError::None, m_transactionIdentifier);
    m_state = SynchronousCallDispatcherState::Prepared;
    return m_diagnostic;
}

SynchronousCallDispatcherError
SynchronousCallDispatcher::ValidateRequestLocked(const SynchronousCallPrepareRequest& request) const noexcept
{
    if (!m_replyTokenObject.IsValid() || !m_callEndpoint.IsReady())
    {
        return SynchronousCallDispatcherError::InvalidConfiguration;
    }
    if ((request.CallerSpace == nullptr) || !request.CallerSpace->IsReady() ||
        !request.CallerSpace->IsFromRegistry(m_registry))
    {
        return SynchronousCallDispatcherError::InvalidCallerSpace;
    }
    if (!request.Sender.IsValid())
    {
        return SynchronousCallDispatcherError::InvalidSender;
    }
    if (!request.Caller.IsValid())
    {
        return SynchronousCallDispatcherError::InvalidCaller;
    }
    if (!request.Caller.IsFromRegistry(m_registry))
    {
        return SynchronousCallDispatcherError::CallerRegistryMismatch;
    }
    if (!request.SchedulingContext.IsValid())
    {
        return SynchronousCallDispatcherError::InvalidSchedulingContext;
    }
    if (!request.SchedulingContext.IsFromRegistry(m_registry))
    {
        return SynchronousCallDispatcherError::SchedulingContextRegistryMismatch;
    }
    if (!request.CallEndpointCapability.IsValid())
    {
        return SynchronousCallDispatcherError::InvalidCallEndpointCapability;
    }
    if (!request.ReplyTokenCapability.IsValid())
    {
        return SynchronousCallDispatcherError::InvalidReplyTokenCapability;
    }
    if (request.Call == nullptr)
    {
        return SynchronousCallDispatcherError::InvalidCall;
    }
    if (!request.Call->Deadline.IsInfinite())
    {
        return SynchronousCallDispatcherError::UnsupportedDeadline;
    }
    if (request.Call->Kind != MessageKind::SynchronousCall)
    {
        return SynchronousCallDispatcherError::InvalidCall;
    }
    if (request.Call->AttachmentCount > MaximumUserAttachments)
    {
        return SynchronousCallDispatcherError::TooManyAttachments;
    }
    for (Foundation::Size index{0U}; index < request.Call->AttachmentCount; ++index)
    {
        if (request.Call->Attachments[index].ExpectedType == Object::ObjectType::ReplyToken)
        {
            return SynchronousCallDispatcherError::ReplyTokenAttachmentPresent;
        }
    }
    return SynchronousCallDispatcherError::None;
}

SynchronousCallDispatcherError SynchronousCallDispatcher::PreflightReplyDispatcherLocked(
    SynchronousReplyDispatcherFailure& replyFailure) const noexcept
{
    replyFailure = m_replyDispatcher.PreflightInitialize();
    if (replyFailure.Error != SynchronousReplyDispatcherError::None)
    {
        return SynchronousCallDispatcherError::InvalidConfiguration;
    }
    return SynchronousCallDispatcherError::None;
}

SynchronousCallDispatcherError
SynchronousCallDispatcher::AuthenticateCallEndpointLocked(const SynchronousCallPrepareRequest& request,
                                                          Object::CapabilityError& capabilityError) noexcept
{
    Object::CapabilityLookupResult lookup = request.CallerSpace->Lookup(
        request.CallEndpointCapability, Object::ObjectType::Endpoint, Object::CapabilityOperation::Invoke);
    if (!lookup.HasValue())
    {
        capabilityError = lookup.GetError();
        return SynchronousCallDispatcherError::CallEndpointCapabilityRejected;
    }

    if (&lookup.GetValue().GetObject() != &m_callEndpoint)
    {
        return SynchronousCallDispatcherError::CallEndpointCapabilityMismatch;
    }
    return SynchronousCallDispatcherError::None;
}

SynchronousCallDispatcherError
SynchronousCallDispatcher::PinAndCorrelateLocked(const SynchronousCallPrepareRequest& request,
                                                 Object::ObjectPinError& pinError) noexcept
{
    const Object::ObjectPinError callerPinError =
        m_registry.PinObject(request.Caller.GetObjectId(), Object::ObjectType::Thread, m_callerPin);
    if (callerPinError != Object::ObjectPinError::Ok)
    {
        pinError = callerPinError;
        return SynchronousCallDispatcherError::CallerPinRejected;
    }

    const Object::ObjectPinError contextPinError = m_registry.PinObject(
        request.SchedulingContext.GetObjectId(), Object::ObjectType::SchedulingContext, m_contextPin);
    if (contextPinError != Object::ObjectPinError::Ok)
    {
        pinError = contextPinError;
        return SynchronousCallDispatcherError::SchedulingContextPinRejected;
    }

    const Object::ObjectPinError tokenPinError =
        m_registry.PinObject(m_replyTokenObject, Object::ObjectType::ReplyToken, m_tokenPin);
    if (tokenPinError != Object::ObjectPinError::Ok)
    {
        pinError = tokenPinError;
        return SynchronousCallDispatcherError::ReplyTokenPinRejected;
    }

    m_caller = static_cast<Scheduler::Thread*>(&m_callerPin.GetObject());
    m_context = static_cast<Scheduler::SchedulingContext*>(&m_contextPin.GetObject());
    if (&m_tokenPin.GetObject() != &m_replyToken)
    {
        return SynchronousCallDispatcherError::ReplyTokenStorageMismatch;
    }

    const ReplyTokenSnapshot token = m_replyToken.GetSnapshot();
    if (token.State != ReplyTokenState::Available)
    {
        return SynchronousCallDispatcherError::ReplyTokenNotAvailable;
    }
    if ((m_caller->GetChargeOwner().GetValue() != request.Sender.GetValue()) ||
        (m_context->GetChargeOwner().GetValue() != request.Sender.GetValue()) ||
        (m_replyToken.GetChargeOwner().GetValue() != request.Sender.GetValue()))
    {
        return SynchronousCallDispatcherError::ResourceOwnerMismatch;
    }

    const Scheduler::ThreadSnapshot caller = m_caller->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    const Scheduler::CpuId wakeProcessor = m_replyDispatcher.GetWakeProcessor();
    if ((caller.State != Scheduler::ThreadState::Running) || !caller.RunningProcessor.IsValid() ||
        caller.BlockToken.IsValid() || (caller.RunSequence == 0U) || (caller.ExitCode != 0U) ||
        (context.State != Scheduler::SchedulingContextState::Running) || !context.RunningProcessor.IsValid() ||
        (context.RunningProcessor.GetValue() != caller.RunningProcessor.GetValue()) ||
        !context.CurrentThread.IsEqualTo(request.Caller))
    {
        return SynchronousCallDispatcherError::CorrelatedStateChanged;
    }
    if (!wakeProcessor.IsValid() || (wakeProcessor.GetValue() != caller.RunningProcessor.GetValue()))
    {
        return SynchronousCallDispatcherError::WakeProcessorMismatch;
    }

    m_runSequence = caller.RunSequence;
    return SynchronousCallDispatcherError::None;
}

SynchronousCallDispatcherError
SynchronousCallDispatcher::ValidateReplyTokenCapabilityLocked(const SynchronousCallPrepareRequest& request,
                                                              Object::CapabilityError& capabilityError) noexcept
{
    Object::CapabilityLookupResult lookup = request.CallerSpace->Lookup(
        request.ReplyTokenCapability, Object::ObjectType::ReplyToken, Object::CapabilityOperation::Transfer);
    if (!lookup.HasValue())
    {
        capabilityError = lookup.GetError();
        return SynchronousCallDispatcherError::ReplyTokenCapabilityRejected;
    }

    const Object::CapabilityDescriptor& descriptor = lookup.GetValue().GetDescriptor();
    const Object::CapabilityRights invoke{Object::CapabilityRight::Invoke};
    if ((&lookup.GetValue().GetObject() != &m_replyToken) ||
        (descriptor.Object.GetValue() != m_replyTokenObject.GetValue()) ||
        (descriptor.ChargeOwner.GetValue() != request.Sender.GetValue()) || !descriptor.LeaseDeadline.IsInfinite() ||
        !descriptor.Rights.ContainsAll(invoke))
    {
        return SynchronousCallDispatcherError::ReplyTokenCapabilityMismatch;
    }
    return SynchronousCallDispatcherError::None;
}

SynchronousCallDispatcherError SynchronousCallDispatcher::StageCallLocked(const SynchronousCallPrepareRequest& request,
                                                                          EndpointError& endpointError) noexcept
{
    m_stagedCall = *request.Call;
    const Foundation::Size tokenIndex = m_stagedCall.AttachmentCount;
    m_stagedCall.Attachments[tokenIndex] = CapabilityAttachment{
        .Handle = request.ReplyTokenCapability,
        .RequestedDeadline = Object::MonotonicTime::Infinite(),
        .RequestedRights = Object::CapabilityRights{Object::CapabilityRight::Invoke},
        .ExpectedType = Object::ObjectType::ReplyToken,
        .TransferMode = CapabilityTransferMode::Move,
        .Reserved = 0U,
    };
    ++m_stagedCall.AttachmentCount;

    const Foundation::Result<Foundation::Size, MessageCodecError> encoded =
        MessageCodec::Encode(m_stagedCall, WritableByteView{
                                               .Data = m_validationFrame,
                                               .SizeBytes = sizeof(m_validationFrame),
                                           });
    if (!encoded.HasValue())
    {
        endpointError = EndpointError{
            .Code = EndpointErrorCode::InvalidMessage,
            .Message = encoded.GetError(),
            .Capability = Object::CapabilityError::Ok,
            .Reserved = 0U,
        };
        return SynchronousCallDispatcherError::MessageValidationRejected;
    }
    ClearValidationFrameLocked();
    return SynchronousCallDispatcherError::None;
}

SynchronousCallDispatcherFailure SynchronousCallDispatcher::CancelPrepared() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == SynchronousCallDispatcherState::Faulted)
    {
        return m_diagnostic;
    }
    if (m_state != SynchronousCallDispatcherState::Prepared)
    {
        SynchronousCallDispatcherFailure failure{};
        SetFailure(failure, SynchronousCallDispatcherError::InvalidState, m_transactionIdentifier);
        return failure;
    }

    ResetPreparationLocked();
    SetFailure(m_diagnostic, SynchronousCallDispatcherError::None);
    return m_diagnostic;
}

SynchronousCallDispatcherFailure
SynchronousCallDispatcher::CommitBlocked(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return CommitBlockedLocked(blockToken);
}

SynchronousCallDispatcherFailure
SynchronousCallDispatcher::CommitBlockedLocked(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    if (m_state == SynchronousCallDispatcherState::Faulted)
    {
        return m_diagnostic;
    }
    if ((m_state == SynchronousCallDispatcherState::Committed) ||
        (m_state == SynchronousCallDispatcherState::Cancelled))
    {
        SynchronousCallDispatcherFailure failure{};
        SetFailure(failure, SynchronousCallDispatcherError::AlreadyCompleted, m_transactionIdentifier);
        return failure;
    }
    if (IsRetryPendingState(m_state))
    {
        return m_diagnostic;
    }
    if (m_state != SynchronousCallDispatcherState::Prepared)
    {
        SynchronousCallDispatcherFailure failure{};
        SetFailure(failure, SynchronousCallDispatcherError::InvalidState, m_transactionIdentifier);
        return failure;
    }

    bool tokenMismatch{false};
    if (!CandidateBlockIsCorrelatedLocked(blockToken, tokenMismatch))
    {
        SetFailure(m_diagnostic,
                   tokenMismatch ? SynchronousCallDispatcherError::InvalidBlockToken
                                 : SynchronousCallDispatcherError::CorrelatedStateChanged,
                   m_transactionIdentifier);
        return m_diagnostic;
    }
    m_blockToken = blockToken;
    m_state = SynchronousCallDispatcherState::ArmPending;
    return ProgressBlockedCommitLocked();
}

SynchronousCallDispatcherFailure SynchronousCallDispatcher::ProgressBlockedCommitLocked() noexcept
{
    if ((m_state != SynchronousCallDispatcherState::ArmPending) &&
        (m_state != SynchronousCallDispatcherState::ReplyInitializationPending))
    {
        SynchronousCallDispatcherFailure failure{};
        SetFailure(failure, SynchronousCallDispatcherError::InvalidState, m_transactionIdentifier);
        return failure;
    }

    if (m_state == SynchronousCallDispatcherState::ArmPending)
    {
        const ReplyTokenFailure armed =
            m_replyToken.Arm(m_registry, m_callerIdentity, m_contextIdentity, m_blockToken, m_transactionIdentifier);
        if (armed.Error != ReplyTokenError::None)
        {
            SetFailure(m_diagnostic, SynchronousCallDispatcherError::ReplyTokenArmRejected, m_transactionIdentifier);
            m_diagnostic.Token = armed;
            if (!ReplyTokenArmFailureIsRetryable(armed))
            {
                m_state = SynchronousCallDispatcherState::Faulted;
            }
            return m_diagnostic;
        }
        m_state = SynchronousCallDispatcherState::ReplyInitializationPending;
    }

    if (m_state == SynchronousCallDispatcherState::ReplyInitializationPending)
    {
        const SynchronousReplyDispatcherFailure replyInitialized = m_replyDispatcher.Initialize(m_replyTokenObject);
        if (replyInitialized.Error != SynchronousReplyDispatcherError::None)
        {
            SetFailure(m_diagnostic, SynchronousCallDispatcherError::ReplyDispatcherRejected, m_transactionIdentifier);
            SetReplyFailureDetails(m_diagnostic, replyInitialized);
            if (!ReplyInitializationFailureIsRetryable(replyInitialized))
            {
                m_state = SynchronousCallDispatcherState::Faulted;
            }
            return m_diagnostic;
        }
    }

    // The Endpoint authorizes the caller's capability again where it
    // publishes the Call, so authority lost while the caller was blocked
    // fails the send instead of being used.
    const Foundation::Result<EndpointSendReceipt, EndpointError> sent = m_callEndpoint.SendFromInvocation(
        *m_callerSpace, m_callEndpointCapability, m_sender, EndpointSendClass::Normal, m_stagedCall);
    if (!sent.HasValue())
    {
        return CancelAfterSendFailureLocked(sent.GetError());
    }

    m_endpointSequence = sent.GetValue().Sequence;
    m_state = SynchronousCallDispatcherState::Committed;
    SetFailure(m_diagnostic, SynchronousCallDispatcherError::None, m_transactionIdentifier);
    ClearStagedCallLocked();
    ReleasePreparationPinsLocked();
    return m_diagnostic;
}

bool SynchronousCallDispatcher::CandidateBlockIsCorrelatedLocked(const Scheduler::ThreadBlockToken blockToken,
                                                                 bool& tokenMismatch) const noexcept
{
    tokenMismatch = false;
    if ((m_caller == nullptr) || (m_context == nullptr) || !m_callerPin.IsValid() || !m_contextPin.IsValid() ||
        !m_tokenPin.IsValid())
    {
        return false;
    }

    const Scheduler::ThreadSnapshot caller = m_caller->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    if (!blockToken.IsValid() || (caller.RunSequence != m_runSequence) ||
        ((caller.State == Scheduler::ThreadState::Blocked) && caller.BlockToken.IsValid() &&
         !caller.BlockToken.IsEqualTo(blockToken)))
    {
        tokenMismatch = true;
        return false;
    }
    return (caller.State == Scheduler::ThreadState::Blocked) && caller.BlockToken.IsEqualTo(blockToken) &&
           !caller.RunningProcessor.IsValid() && (caller.ExitCode == 0U) && ContextCanRemainBlocked(context.State) &&
           !context.RunningProcessor.IsValid() && context.CurrentThread.IsEqualTo(m_callerIdentity);
}

SynchronousCallDispatcherFailure
SynchronousCallDispatcher::CancelAfterSendFailureLocked(const EndpointError& endpointFailure) noexcept
{
    const Object::CapabilityError closeError = m_callerSpace->Close(m_replyTokenCapability);
    if (closeError != Object::CapabilityError::Ok)
    {
        static_cast<void>(m_replyDispatcher.Cancel());
        SetFailure(m_diagnostic, SynchronousCallDispatcherError::ReplyTokenCapabilityCloseRejected,
                   m_transactionIdentifier);
        m_diagnostic.Endpoint = endpointFailure;
        m_diagnostic.Capability = closeError;
        m_state = SynchronousCallDispatcherState::Faulted;
        return m_diagnostic;
    }

    SetFailure(m_diagnostic, SynchronousCallDispatcherError::EndpointSendRejected, m_transactionIdentifier);
    m_diagnostic.Endpoint = endpointFailure;
    m_endpointSequence = 0U;
    m_state = SynchronousCallDispatcherState::CancellationPending;
    ClearStagedCallLocked();
    ReleasePreparationPinsLocked();
    return PollCancellationLocked();
}

Scheduler::BlockedRunCommitOutcome
SynchronousCallDispatcher::CommitBlockedFromFinishedRun(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const SynchronousCallDispatcherState startingState = m_state;
    const SynchronousCallDispatcherFailure result = CommitBlockedLocked(blockToken);
    if ((m_state == SynchronousCallDispatcherState::Committed) ||
        (m_state == SynchronousCallDispatcherState::Cancelled))
    {
        return Scheduler::BlockedRunCommitOutcome::Accepted;
    }
    if (IsRetryPendingState(m_state))
    {
        return Scheduler::BlockedRunCommitOutcome::RetryableRejected;
    }
    if ((startingState == SynchronousCallDispatcherState::Prepared) &&
        (m_state != SynchronousCallDispatcherState::Faulted))
    {
        EnterFaultedLocked(result);
    }
    return Scheduler::BlockedRunCommitOutcome::TerminalRejected;
}

SynchronousCallDispatcherFailure SynchronousCallDispatcher::Poll() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == SynchronousCallDispatcherState::Faulted)
    {
        return m_diagnostic;
    }
    if ((m_state == SynchronousCallDispatcherState::ArmPending) ||
        (m_state == SynchronousCallDispatcherState::ReplyInitializationPending))
    {
        return ProgressBlockedCommitLocked();
    }
    if (m_state == SynchronousCallDispatcherState::CancellationPending)
    {
        return PollCancellationLocked();
    }

    SynchronousCallDispatcherFailure failure{};
    SetFailure(failure, SynchronousCallDispatcherError::InvalidState, m_transactionIdentifier);
    return failure;
}

SynchronousCallDispatcherFailure SynchronousCallDispatcher::PollCancellation() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PollCancellationLocked();
}

SynchronousCallDispatcherFailure SynchronousCallDispatcher::PollCancellationLocked() noexcept
{
    if (m_state == SynchronousCallDispatcherState::Faulted)
    {
        return m_diagnostic;
    }
    if (m_state != SynchronousCallDispatcherState::CancellationPending)
    {
        SynchronousCallDispatcherFailure failure{};
        SetFailure(failure, SynchronousCallDispatcherError::InvalidState, m_transactionIdentifier);
        return failure;
    }

    const auto cancelled = m_replyDispatcher.Cancel();
    if (cancelled.HasValue())
    {
        m_state = SynchronousCallDispatcherState::Cancelled;
        const EndpointError endpointFailure = m_diagnostic.Endpoint;
        SetFailure(m_diagnostic, SynchronousCallDispatcherError::EndpointSendRejected, m_transactionIdentifier);
        m_diagnostic.Endpoint = endpointFailure;
        return m_diagnostic;
    }

    const SynchronousReplyDispatcherFailure replyFailure = cancelled.GetError();
    if (CancellationFailureIsRetryable(replyFailure))
    {
        m_diagnostic.Error = SynchronousCallDispatcherError::CancellationPending;
        SetReplyFailureDetails(m_diagnostic, replyFailure);
        return m_diagnostic;
    }

    const EndpointError endpointFailure = m_diagnostic.Endpoint;
    SetFailure(m_diagnostic, SynchronousCallDispatcherError::CancellationRejected, m_transactionIdentifier);
    SetReplyFailureDetails(m_diagnostic, replyFailure);
    m_diagnostic.Endpoint = endpointFailure;
    m_state = SynchronousCallDispatcherState::Faulted;
    return m_diagnostic;
}

bool SynchronousCallDispatcher::CancellationFailureIsRetryable(
    const SynchronousReplyDispatcherFailure& failure) noexcept
{
    if (failure.Error != SynchronousReplyDispatcherError::QueueReservationRejected)
    {
        return false;
    }
    if (failure.Queue.Error == Scheduler::ReadyQueueError::QueueFull)
    {
        return true;
    }
    if ((failure.Queue.Error == Scheduler::ReadyQueueError::ContextPinRejected) &&
        (failure.Queue.ContextPin == Object::ObjectPinError::ReferenceExhausted))
    {
        return true;
    }
    return (failure.Queue.Error == Scheduler::ReadyQueueError::ThreadPinRejected) &&
           (failure.Queue.ThreadPin == Object::ObjectPinError::ReferenceExhausted);
}

bool SynchronousCallDispatcher::ReplyTokenArmFailureIsRetryable(const ReplyTokenFailure& failure) noexcept
{
    if (failure.PinError != Object::ObjectPinError::ReferenceExhausted)
    {
        return false;
    }
    return (failure.Error == ReplyTokenError::CallerPinUnavailable) ||
           (failure.Error == ReplyTokenError::SchedulingContextPinUnavailable);
}

bool SynchronousCallDispatcher::ReplyInitializationFailureIsRetryable(
    const SynchronousReplyDispatcherFailure& failure) noexcept
{
    if (failure.Pin != Object::ObjectPinError::ReferenceExhausted)
    {
        return false;
    }
    return (failure.Error == SynchronousReplyDispatcherError::TokenPinRejected) ||
           (failure.Error == SynchronousReplyDispatcherError::CallerPinRejected) ||
           (failure.Error == SynchronousReplyDispatcherError::ContextPinRejected);
}

bool SynchronousCallDispatcher::IsRetryPendingState(const SynchronousCallDispatcherState state) noexcept
{
    return (state == SynchronousCallDispatcherState::ArmPending) ||
           (state == SynchronousCallDispatcherState::ReplyInitializationPending) ||
           (state == SynchronousCallDispatcherState::CancellationPending);
}

SynchronousCallDispatcherSnapshot SynchronousCallDispatcher::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return SynchronousCallDispatcherSnapshot{
        .Caller = m_callerIdentity,
        .SchedulingContext = m_contextIdentity,
        .BlockToken = m_blockToken,
        .TransactionIdentifier = m_transactionIdentifier,
        .RunSequence = m_runSequence,
        .EndpointSequence = m_endpointSequence,
        .Diagnostic = m_diagnostic,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

void SynchronousCallDispatcher::ClearStagedCallLocked() noexcept
{
    Foundation::UInt8* const bytes = reinterpret_cast<Foundation::UInt8*>(&m_stagedCall);
    for (Foundation::Size index{0U}; index < sizeof(m_stagedCall); ++index)
    {
        bytes[index] = 0U;
    }
}

void SynchronousCallDispatcher::ClearValidationFrameLocked() noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(m_validationFrame); ++index)
    {
        m_validationFrame[index] = 0U;
    }
}

void SynchronousCallDispatcher::ResetPreparationLocked() noexcept
{
    ClearStagedCallLocked();
    ClearValidationFrameLocked();
    ReleasePreparationPinsLocked();
    m_callerIdentity = {};
    m_contextIdentity = {};
    m_blockToken = {};
    m_callEndpointCapability = {};
    m_replyTokenCapability = {};
    m_sender = {};
    m_transactionIdentifier = 0U;
    m_runSequence = 0U;
    m_endpointSequence = 0U;
    m_state = SynchronousCallDispatcherState::Fresh;
}

void SynchronousCallDispatcher::ReleasePreparationPinsLocked() noexcept
{
    m_tokenPin.Reset();
    m_contextPin.Reset();
    m_callerPin.Reset();
    m_callerSpace = nullptr;
    m_caller = nullptr;
    m_context = nullptr;
}

void SynchronousCallDispatcher::EnterFaultedLocked(const SynchronousCallDispatcherFailure& failure) noexcept
{
    m_diagnostic = failure;
    m_state = SynchronousCallDispatcherState::Faulted;
}
} // namespace Asteria::Kernel::Ipc
