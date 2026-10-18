// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/PageFaultIngress.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
bool PageFaultIngress::FaultRecordIsValid(const FaultRecord& fault) noexcept
{
    const bool accessKnown = (fault.Access == FaultAccess::Read) || (fault.Access == FaultAccess::Write) ||
                             (fault.Access == FaultAccess::Execute);
    const bool causeKnown =
        (fault.Cause == FaultCause::MissingMapping) || (fault.Cause == FaultCause::ProtectionViolation) ||
        (fault.Cause == FaultCause::InvalidMappingEncoding) || (fault.Cause == FaultCause::Exception);
    if (!accessKnown || !causeKnown || (fault.Privilege != FaultPrivilege::User))
    {
        return false;
    }
    for (const Foundation::UInt8 reserved : fault.Reserved)
    {
        if (reserved != 0U)
        {
            return false;
        }
    }
    return true;
}

bool PageFaultIngress::TraceIsValid(const Ipc::TraceContext& trace) noexcept
{
    const bool hasTrace = (trace.TraceIdentifierHigh != 0U) || (trace.TraceIdentifierLow != 0U);
    return hasTrace == (trace.SpanIdentifier != 0U);
}

bool PageFaultIngress::ContextCanResume(const Scheduler::SchedulingContextState state) noexcept
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

bool PageFaultIngress::FactoryFailureIsRetryable(const FaultTokenFactoryFailure& failure) noexcept
{
    switch (failure.Error)
    {
    case FaultTokenFactoryError::CapacityExceeded:
        return true;

    case FaultTokenFactoryError::RegistrationRejected:
        return failure.RegistryError == Object::ObjectRegistryError::RegistryExhausted;

    case FaultTokenFactoryError::CapabilityRejected:
        return failure.CapabilityError == Object::CapabilityError::SpaceExhausted;

    case FaultTokenFactoryError::None:
    case FaultTokenFactoryError::InvalidConfiguration:
    case FaultTokenFactoryError::TokenRejected:
    case FaultTokenFactoryError::RollbackRejected:
    case FaultTokenFactoryError::InvalidToken:
    case FaultTokenFactoryError::TokenNotOwned:
    case FaultTokenFactoryError::TokenBusy:
    case FaultTokenFactoryError::DestroyRejected:
    case FaultTokenFactoryError::InvalidDestructionRecord:
    case FaultTokenFactoryError::RecycleRejected:
    case FaultTokenFactoryError::Faulted:
        return false;
    }
    return false;
}

bool PageFaultIngress::DeliveryFailureIsRetryable(const PagerFaultDispatcherFailure& failure) noexcept
{
    if (failure.Error != PagerFaultDispatcherError::EndpointRejected)
    {
        return false;
    }
    if ((failure.Endpoint.Code == Ipc::EndpointErrorCode::WouldBlock) ||
        (failure.Endpoint.Code == Ipc::EndpointErrorCode::ReservationActive))
    {
        return true;
    }
    return (failure.Endpoint.Code == Ipc::EndpointErrorCode::CapabilityTransferFailed) &&
           (failure.Endpoint.Capability == Object::CapabilityError::SpaceExhausted);
}

void PageFaultIngress::ClearFailureLocked() noexcept
{
    m_error = PageFaultIngressError::None;
    m_factoryError = FaultTokenFactoryError::None;
    m_tokenError = FaultTokenError::None;
    m_dispatcherError = PagerFaultDispatcherError::None;
    m_endpointError = Ipc::EndpointErrorCode::None;
    m_protocolError = PagerProtocolError::None;
    m_pinError = Object::ObjectPinError::Ok;
}

void PageFaultIngress::SetPrepareFailureLocked(const PageFaultIngressError error,
                                               const Object::ObjectPinError pin) noexcept
{
    ClearFailureLocked();
    m_error = error;
    m_pinError = pin;
}

void PageFaultIngress::SetFactoryFailureLocked(const FaultTokenFactoryFailure& failure) noexcept
{
    ClearFailureLocked();
    m_error = PageFaultIngressError::FactoryRejected;
    m_factoryError = failure.Error;
    m_tokenError = failure.TokenError;
}

void PageFaultIngress::SetDeliveryFailureLocked(const PagerFaultDispatcherFailure& failure) noexcept
{
    ClearFailureLocked();
    m_error = PageFaultIngressError::DeliveryRejected;
    m_tokenError = failure.Token;
    m_dispatcherError = failure.Error;
    m_endpointError = failure.Endpoint.Code;
    m_protocolError = failure.Protocol;
    m_pinError = failure.Pin;
}

bool PageFaultIngress::RunningStateIsCorrelatedLocked() const noexcept
{
    if ((m_thread == nullptr) || (m_context == nullptr) || !m_threadPin.IsValid() || !m_addressSpacePin.IsValid() ||
        !m_contextPin.IsValid())
    {
        return false;
    }

    const Scheduler::ThreadSnapshot thread = m_thread->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    return (thread.State == Scheduler::ThreadState::Running) && thread.RunningProcessor.IsValid() &&
           !thread.BlockToken.IsValid() && (thread.ExitCode == 0U) &&
           (context.State == Scheduler::SchedulingContextState::Running) && context.RunningProcessor.IsValid() &&
           (context.RunningProcessor.GetValue() == thread.RunningProcessor.GetValue()) &&
           context.CurrentThread.IsEqualTo(m_request.Thread);
}

bool PageFaultIngress::BlockedStateIsCorrelatedLocked() const noexcept
{
    if ((m_thread == nullptr) || (m_context == nullptr) || !m_threadPin.IsValid() || !m_addressSpacePin.IsValid() ||
        !m_contextPin.IsValid())
    {
        return false;
    }

    const Scheduler::ThreadSnapshot thread = m_thread->GetSnapshot();
    const Scheduler::SchedulingContextSnapshot context = m_context->GetSnapshot();
    return (thread.State == Scheduler::ThreadState::Blocked) && thread.BlockToken.IsEqualTo(m_blockToken) &&
           !thread.RunningProcessor.IsValid() && (thread.ExitCode == 0U) && ContextCanResume(context.State) &&
           !context.RunningProcessor.IsValid() && context.CurrentThread.IsEqualTo(m_request.Thread);
}

void PageFaultIngress::ReleaseIngressPinsLocked() noexcept
{
    m_thread = nullptr;
    m_context = nullptr;
    m_contextPin.Reset();
    m_addressSpacePin.Reset();
    m_threadPin.Reset();
}

bool PageFaultIngress::RollbackCreationLocked() noexcept
{
    if (!m_creation.Token.IsValid())
    {
        return true;
    }
    if ((m_request.SourceSpace == nullptr) || !m_creation.Capability.IsValid() ||
        (m_dispatcher.GetSnapshot().State != PagerFaultDispatcherState::Idle))
    {
        return false;
    }

    const Object::CapabilityError closeError = m_request.SourceSpace->Close(m_creation.Capability);
    if (closeError != Object::CapabilityError::Ok)
    {
        return false;
    }
    m_creation.Capability = Object::CapabilityHandle{};

    const auto retired = m_factory.Retire(m_creation.Token);
    if (!retired.HasValue())
    {
        const FaultTokenFactoryFailure failure = retired.GetError();
        m_factoryError = failure.Error;
        m_tokenError = failure.TokenError;
        return false;
    }
    return true;
}

PageFaultIngressError PageFaultIngress::Prepare(const PageFaultIngressRequest& request) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == PageFaultIngressState::Faulted)
    {
        return PageFaultIngressError::Faulted;
    }
    if (m_state != PageFaultIngressState::Idle)
    {
        SetPrepareFailureLocked(PageFaultIngressError::AlreadyPrepared);
        return m_error;
    }
    if ((request.SourceSpace == nullptr) || !request.SourceSpace->IsReady() || !request.Thread.IsValid() ||
        !request.AddressSpace.IsValid() || !request.SchedulingContext.IsValid() ||
        !request.Thread.IsFromRegistry(m_objectRegistry) || !request.AddressSpace.IsFromRegistry(m_objectRegistry) ||
        !request.SchedulingContext.IsFromRegistry(m_objectRegistry) || !request.ChargeOwner.IsValid() ||
        !request.Sender.IsValid() || !request.RevocationDomain.IsValid() || (request.TransactionIdentifier == 0U) ||
        !FaultRecordIsValid(request.Fault) || !TraceIsValid(request.Trace))
    {
        SetPrepareFailureLocked(PageFaultIngressError::InvalidConfiguration);
        return m_error;
    }
    if (m_threadPin.IsValid() || m_addressSpacePin.IsValid() || m_contextPin.IsValid())
    {
        m_state = PageFaultIngressState::Faulted;
        SetPrepareFailureLocked(PageFaultIngressError::Faulted);
        ReleaseIngressPinsLocked();
        return m_error;
    }

    Object::ObjectPinError pin =
        m_objectRegistry.PinObject(request.Thread.GetObjectId(), Object::ObjectType::Thread, m_threadPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        SetPrepareFailureLocked(PageFaultIngressError::ThreadPinRejected, pin);
        return m_error;
    }
    pin = m_objectRegistry.PinObject(request.AddressSpace.GetObjectId(), Object::ObjectType::AddressSpace,
                                     m_addressSpacePin);
    if (pin != Object::ObjectPinError::Ok)
    {
        m_threadPin.Reset();
        SetPrepareFailureLocked(PageFaultIngressError::AddressSpacePinRejected, pin);
        return m_error;
    }
    pin = m_objectRegistry.PinObject(request.SchedulingContext.GetObjectId(), Object::ObjectType::SchedulingContext,
                                     m_contextPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        m_addressSpacePin.Reset();
        m_threadPin.Reset();
        SetPrepareFailureLocked(PageFaultIngressError::SchedulingContextPinRejected, pin);
        return m_error;
    }

    m_thread = static_cast<Scheduler::Thread*>(&m_threadPin.GetObject());
    m_context = static_cast<Scheduler::SchedulingContext*>(&m_contextPin.GetObject());
    m_request = request;
    if (!RunningStateIsCorrelatedLocked())
    {
        ReleaseIngressPinsLocked();
        m_request = PageFaultIngressRequest{};
        SetPrepareFailureLocked(PageFaultIngressError::CorrelatedStateChanged);
        return m_error;
    }

    m_creation = FaultTokenCreation{};
    m_blockToken = Scheduler::ThreadBlockToken{};
    m_endpointSequence = 0U;
    m_state = PageFaultIngressState::Prepared;
    ClearFailureLocked();
    return PageFaultIngressError::None;
}

void PageFaultIngress::CommitBlocked(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == PageFaultIngressState::Faulted)
    {
        return;
    }
    if (m_state != PageFaultIngressState::Prepared)
    {
        SetPrepareFailureLocked(PageFaultIngressError::InvalidState);
        return;
    }
    if (!blockToken.IsValid())
    {
        m_state = PageFaultIngressState::Faulted;
        SetPrepareFailureLocked(PageFaultIngressError::InvalidBlockToken);
        ReleaseIngressPinsLocked();
        return;
    }

    m_blockToken = blockToken;
    m_state = PageFaultIngressState::BlockedPendingToken;
    if (!BlockedStateIsCorrelatedLocked())
    {
        m_state = PageFaultIngressState::Faulted;
        SetPrepareFailureLocked(PageFaultIngressError::CorrelatedStateChanged);
        if (!RollbackCreationLocked())
        {
            m_error = PageFaultIngressError::RollbackRejected;
            return;
        }
        ReleaseIngressPinsLocked();
        return;
    }
    ProgressLocked();
}

void PageFaultIngress::Retry() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != PageFaultIngressState::BlockedPendingToken) && (m_state != PageFaultIngressState::TokenReady))
    {
        return;
    }
    if (!BlockedStateIsCorrelatedLocked())
    {
        m_state = PageFaultIngressState::Faulted;
        SetPrepareFailureLocked(PageFaultIngressError::CorrelatedStateChanged);
        if (!RollbackCreationLocked())
        {
            m_error = PageFaultIngressError::RollbackRejected;
            return;
        }
        ReleaseIngressPinsLocked();
        return;
    }
    ProgressLocked();
}

void PageFaultIngress::ProgressLocked() noexcept
{
    ClearFailureLocked();
    if (!m_creation.Token.IsValid())
    {
        const auto created = m_factory.Create(FaultTokenCreateRequest{
            .Destination = m_request.SourceSpace,
            .ChargeOwner = m_request.ChargeOwner,
            .Thread = m_request.Thread,
            .AddressSpace = m_request.AddressSpace,
            .Fault = m_request.Fault,
            .RevocationDomain = m_request.RevocationDomain,
            .LeaseDeadline = m_request.Deadline,
            .Resolution = m_request.Resolution,
            .ChannelIdentity = m_request.ChannelIdentity,
        });
        if (!created.HasValue())
        {
            const FaultTokenFactoryFailure failure = created.GetError();
            SetFactoryFailureLocked(failure);
            m_state = FactoryFailureIsRetryable(failure) ? PageFaultIngressState::BlockedPendingToken
                                                         : PageFaultIngressState::Faulted;
            if (m_state == PageFaultIngressState::Faulted)
            {
                ReleaseIngressPinsLocked();
            }
            return;
        }
        m_creation = created.GetValue();
        m_state = PageFaultIngressState::TokenReady;
    }

    const auto delivered = m_dispatcher.Deliver(PagerFaultDeliveryRequest{
        .SourceSpace = m_request.SourceSpace,
        .FaultToken = m_creation.Token,
        .SchedulingContext = m_request.SchedulingContext,
        .BlockToken = m_blockToken,
        .Sender = m_request.Sender,
        .FaultTokenCapability = m_creation.Capability,
        .Trace = m_request.Trace,
        .TransactionIdentifier = m_request.TransactionIdentifier,
        .Deadline = m_request.Deadline,
        .SupervisionTag = m_request.SupervisionTag,
        .ProtectionDomainIdentity = m_request.ProtectionDomainIdentity,
    });
    if (!delivered.HasValue())
    {
        const PagerFaultDispatcherFailure failure = delivered.GetError();
        SetDeliveryFailureLocked(failure);
        m_state =
            DeliveryFailureIsRetryable(failure) ? PageFaultIngressState::TokenReady : PageFaultIngressState::Faulted;
        if (m_state == PageFaultIngressState::Faulted)
        {
            if (!RollbackCreationLocked())
            {
                m_error = PageFaultIngressError::RollbackRejected;
                return;
            }
            ReleaseIngressPinsLocked();
        }
        return;
    }

    m_endpointSequence = delivered.GetValue().EndpointSequence;
    m_creation.Capability = Object::CapabilityHandle{};
    m_state = PageFaultIngressState::Delivered;
    ClearFailureLocked();
    ReleaseIngressPinsLocked();
}

PageFaultIngressError PageFaultIngress::Reset() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != PageFaultIngressState::Delivered) || m_threadPin.IsValid() || m_addressSpacePin.IsValid() ||
        m_contextPin.IsValid())
    {
        return PageFaultIngressError::InvalidState;
    }
    m_request = PageFaultIngressRequest{};
    m_creation = FaultTokenCreation{};
    m_blockToken = Scheduler::ThreadBlockToken{};
    m_endpointSequence = 0U;
    ClearFailureLocked();
    m_state = PageFaultIngressState::Idle;
    return PageFaultIngressError::None;
}

PageFaultIngressSnapshot PageFaultIngress::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PageFaultIngressSnapshot{
        .Thread = m_request.Thread,
        .AddressSpace = m_request.AddressSpace,
        .SchedulingContext = m_request.SchedulingContext,
        .FaultToken = m_creation.Token,
        .TransactionIdentifier = m_request.TransactionIdentifier,
        .EndpointSequence = m_endpointSequence,
        .State = m_state,
        .Error = m_error,
        .Factory = m_factoryError,
        .Token = m_tokenError,
        .Dispatcher = m_dispatcherError,
        .Endpoint = m_endpointError,
        .Protocol = m_protocolError,
        .Pin = m_pinError,
    };
}
} // namespace Asteria::Kernel::Memory
