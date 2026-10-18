// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/PagerChannel.hpp"

#include "Abi/SystemCallAbi.hpp"

#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{

[[nodiscard]] constexpr PagerResolutionResult
MakeResult(const PagerResolutionError error,
           const Object::CapabilityError capability = Object::CapabilityError::Ok) noexcept
{
    return PagerResolutionResult{
        .Error = error,
        .Capability = capability,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr bool IsZero(const Foundation::UInt8 (&bytes)[7]) noexcept
{
    for (const Foundation::UInt8 byte : bytes)
    {
        if (byte != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsEmptyHandle(const Object::CapabilityHandle handle) noexcept
{
    return (handle.GetSelector() == 0U) && (handle.GetAuthenticator() == 0U);
}

/// Whether a request names nothing but its FaultToken, as a Terminate must.
[[nodiscard]] constexpr bool IsEmptyMapping(const PagerResolveRequest& request) noexcept
{
    const FaultFrameMappingRequest& mapping = request.Mapping;
    return IsEmptyHandle(request.AddressSpace) && IsEmptyHandle(request.MemoryObject) && IsEmptyHandle(request.Frame) &&
           (mapping.VirtualStart.GetValue() == 0U) && (mapping.Pages.GetValue() == 0U) &&
           (mapping.MemoryObjectOffsetPages == 0U) && (mapping.FrameOffsetPages == 0U) &&
           (mapping.Permissions.GetBits() == 0U);
}

/// A failure before any native change cancelled the claim and leaves the
/// fault pending for a corrected request; one after it is terminal.
[[nodiscard]] constexpr PagerResolutionError ClassifyFailure(const FaultResolutionDispatcherFailure& failure) noexcept
{
    if (failure.Terminal || (failure.NativeState != FaultResolutionNativeState::Unchanged))
    {
        return PagerResolutionError::Faulted;
    }
    switch (failure.Error)
    {
    case FaultResolutionDispatcherError::MappingPreparationRejected:
    case FaultResolutionDispatcherError::AddressSpaceRejected:
        return PagerResolutionError::MappingRejected;
    case FaultResolutionDispatcherError::ClaimRejected:
    case FaultResolutionDispatcherError::NotPending:
        return PagerResolutionError::NotPending;
    default:
        return PagerResolutionError::Faulted;
    }
}
} // Namespace

PagerChannel::PagerChannel(Object::ObjectRegistry& registry, Object::RootCapabilityIssuer& issuer,
                           Ipc::Endpoint& pagerEndpoint, Scheduler::ProcessorSchedulerWakeFacade& wakeFacade,
                           TlbShootdownDispatcher& shootdown, Hal::Memory::ITlbShootdownTransport& transport) noexcept
    : m_registry{registry}, m_issuer{issuer}, m_shootdown{shootdown}, m_transport{transport},
      m_factory{registry, issuer}, m_dispatcher{registry, pagerEndpoint, wakeFacade},
      m_ingress{registry, m_factory, m_dispatcher}, m_resolver{registry, shootdown, transport, m_dispatcher}
{
}

PagerChannelError PagerChannel::Configure(const PagerChannelConfiguration& configuration) noexcept
{
    if (m_transition.State() != PagerChannelState::Unconfigured)
    {
        return PagerChannelError::AlreadyConfigured;
    }
    if ((configuration.TokenSpace == nullptr) || !configuration.TokenSpace->IsReady() ||
        (configuration.Readiness == nullptr) || !configuration.ChargeOwner.IsValid() ||
        !configuration.Sender.IsValid() || !configuration.RevocationDomain.IsValid())
    {
        return PagerChannelError::InvalidConfiguration;
    }
    if (!m_transition.Claim(PagerChannelState::Unconfigured, PagerChannelState::Unconfigured))
    {
        return PagerChannelError::AlreadyConfigured;
    }
    m_configuration = configuration;
    m_transition.Publish(PagerChannelState::Idle);
    return PagerChannelError::None;
}

PagerChannelError PagerChannel::PrepareFault(const PagerFaultSource& source) noexcept
{
    if (m_transition.State() == PagerChannelState::Unconfigured)
    {
        return PagerChannelError::NotConfigured;
    }
    if (m_transition.State() == PagerChannelState::Faulted)
    {
        return PagerChannelError::Faulted;
    }
    if (!m_transition.Claim(PagerChannelState::Idle, PagerChannelState::FaultPending))
    {
        RecordBusy();
        return PagerChannelError::Busy;
    }
    if (!CollectRetiredTokens())
    {
        static_cast<void>(Fail(PagerResolutionError::Faulted));
        m_transition.Publish(PagerChannelState::Faulted);
        return PagerChannelError::Faulted;
    }
    const PageFaultIngressError prepared = m_ingress.Prepare(PageFaultIngressRequest{
        .SourceSpace = m_configuration.TokenSpace,
        .Thread = source.Thread,
        .AddressSpace = source.AddressSpace,
        .SchedulingContext = source.SchedulingContext,
        .Fault = source.Fault,
        .Trace = {},
        .ChargeOwner = m_configuration.ChargeOwner,
        .Sender = m_configuration.Sender,
        .RevocationDomain = m_configuration.RevocationDomain,
        .Deadline = Object::MonotonicTime::Infinite(),
        .TransactionIdentifier = m_transaction + 1U,
        .SupervisionTag = source.SupervisionTag,
        .ProtectionDomainIdentity = source.ProtectionDomainIdentity,
        .Resolution = this,
        .ChannelIdentity = m_configuration.ChannelIdentity,
    });
    if (prepared != PageFaultIngressError::None)
    {
        m_error = PagerChannelError::IngressRejected;
        m_transition.Publish(PagerChannelState::Idle);
        return PagerChannelError::IngressRejected;
    }
    ++m_transaction;
    m_signalled = false;
    m_error = PagerChannelError::None;
    m_transition.Publish(PagerChannelState::FaultPending);
    return PagerChannelError::None;
}

void PagerChannel::RecordBusy() noexcept
{
    m_busy.IncrementRelaxed();
}

bool PagerChannel::ClaimPending() noexcept
{
    // A Pending diagnostic reader must not consume the faulter's one block commit or a delivery
    // retry. These void operations wait only for brief Pending field access; Preparing/Resolving
    // callers of Prepare/Resolve still receive Busy.
    for (Scheduler::CrossProcessorWait wait;
         !m_transition.Claim(PagerChannelState::FaultPending, PagerChannelState::FaultPending);)
    {
        if (m_transition.State() != PagerChannelState::FaultPending)
        {
            return false;
        }
        if (!wait.Poll())
        {
            Platform::Synchronization::StopProcessorWait(
                Kernel::Diagnostics::StopCode::InvariantViolated,
                static_cast<Foundation::UInt64>(PagerChannelState::FaultPending));
        }
    }
    return true;
}

void PagerChannel::CommitBlocked(const Scheduler::ThreadBlockToken blockToken) noexcept
{
    if (!ClaimPending())
    {
        return;
    }
    m_ingress.CommitBlocked(blockToken);
    SignalIfDelivered();
    m_transition.Publish(m_terminal ? PagerChannelState::Faulted : PagerChannelState::FaultPending);
}

void PagerChannel::Retry() noexcept
{
    if (m_transition.Claim(PagerChannelState::Idle, PagerChannelState::Idle))
    {
        if (!CollectRetiredTokens())
        {
            static_cast<void>(Fail(PagerResolutionError::Faulted));
        }
        m_transition.Publish(m_terminal ? PagerChannelState::Faulted : PagerChannelState::Idle);
        return;
    }
    if (!ClaimPending())
    {
        return;
    }
    if (!CollectRetiredTokens())
    {
        static_cast<void>(Fail(PagerResolutionError::Faulted));
    }
    else
    {
        m_ingress.Retry();
        SignalIfDelivered();
    }
    m_transition.Publish(m_terminal ? PagerChannelState::Faulted : PagerChannelState::FaultPending);
}

bool PagerChannel::IsReclaimable() noexcept
{
    if (!m_transition.Claim(PagerChannelState::Idle, PagerChannelState::Idle))
    {
        return false;
    }
    const bool collected = CollectRetiredTokens();
    if (!collected)
    {
        static_cast<void>(Fail(PagerResolutionError::Faulted));
    }
    bool empty{collected};
    for (const auto token : m_retiredTokens)
    {
        empty = empty && !token.IsValid();
    }
    m_transition.Publish(m_terminal ? PagerChannelState::Faulted : PagerChannelState::Idle);
    return empty;
}

void PagerChannel::SignalIfDelivered() noexcept
{
    const PageFaultIngressSnapshot ingress = m_ingress.GetSnapshot();
    if (ingress.State == PageFaultIngressState::Faulted)
    {
        m_terminal = true;
        m_error = PagerChannelError::IngressRejected;
        return;
    }
    if ((ingress.State != PageFaultIngressState::Delivered) || m_signalled)
    {
        return;
    }
    if (m_configuration.Readiness->Signal() != Ipc::NotificationError::None)
    {
        m_terminal = true;
        m_error = PagerChannelError::Faulted;
        return;
    }
    m_signalled = true;
    ++m_deliveries;
}

PagerResolutionResult PagerChannel::Fail(const PagerResolutionError error) noexcept
{
    m_terminal = true;
    m_error = PagerChannelError::Faulted;
    return MakeResult(error);
}

PagerResolutionResult PagerChannel::Resolve(Object::CapabilitySpace& caller,
                                            const PagerResolveRequest& request) noexcept
{
    if (m_transition.State() == PagerChannelState::Faulted)
    {
        return MakeResult(PagerResolutionError::Faulted);
    }
    const bool terminate = request.Kind == FaultResolutionKind::Terminate;
    if (((request.Kind != FaultResolutionKind::Map) && !terminate) || !IsZero(request.Reserved) ||
        !IsZero(request.Mapping.Reserved) || (terminate && !IsEmptyMapping(request)))
    {
        return MakeResult(PagerResolutionError::InvalidRequest);
    }
    if (!m_transition.Claim(PagerChannelState::FaultPending, PagerChannelState::Resolving))
    {
        const PagerChannelState state = m_transition.State();
        if (state == PagerChannelState::Resolving || state == PagerChannelState::FaultPending)
        {
            RecordBusy();
            return MakeResult(PagerResolutionError::Busy);
        }
        return MakeResult(PagerResolutionError::NotPending);
    }
    const PagerResolutionResult result = ResolveClaimed(caller, request);
    m_transition.Publish(m_terminal                                   ? PagerChannelState::Faulted
                         : result.Error == PagerResolutionError::None ? PagerChannelState::Idle
                                                                      : PagerChannelState::FaultPending);
    return result;
}

PagerResolutionResult PagerChannel::ResolveClaimed(Object::CapabilitySpace& caller,
                                                   const PagerResolveRequest& request) noexcept
{
    const bool terminate = request.Kind == FaultResolutionKind::Terminate;
    const PageFaultIngressSnapshot ingress = m_ingress.GetSnapshot();
    if (ingress.State != PageFaultIngressState::Delivered)
    {
        return MakeResult(PagerResolutionError::NotPending);
    }
    if (terminate)
    {
        const PagerResolutionResult terminated = TerminateLeased(caller, request, ingress.FaultToken);
        if (terminated.Error != PagerResolutionError::None)
        {
            return terminated;
        }
        ++m_terminations;
        return FinishResolution(ingress.FaultToken);
    }

    const PagerResolutionResult resolved = ResolveLeased(caller, request, ingress.FaultToken);
    m_request = FaultResolutionMapRequest{};
    // A cancelled claim leaves the resolver reusable for a corrected request;
    // a terminal one has faulted the channel and keeps its evidence.
    if (!m_terminal && (m_resolver.Reset() != FaultResolutionDispatcherError::None))
    {
        return Fail(PagerResolutionError::Faulted);
    }
    if (resolved.Error != PagerResolutionError::None)
    {
        return resolved;
    }
    return FinishResolution(ingress.FaultToken);
}

PagerResolutionResult PagerChannel::FinishResolution(const Object::ObjectId token) noexcept
{
    // The thread is awake or exited and this resolver's leases and pins are gone:
    // retire the token, whose handle in the pager's space now resolves to
    // nothing, and end the fault.
    if (!RetireToken(token) || (m_ingress.Reset() != PageFaultIngressError::None) ||
        (m_dispatcher.Reset() != PagerFaultDispatcherError::None))
    {
        return Fail(PagerResolutionError::Faulted);
    }
    ++m_resolutions;
    return MakeResult(PagerResolutionError::None);
}

PagerResolutionResult PagerChannel::TerminateLeased(Object::CapabilitySpace& caller, const PagerResolveRequest& request,
                                                    const Object::ObjectId pendingToken) noexcept
{
    auto token = caller.Lookup(request.FaultToken, Object::ObjectType::FaultToken, Object::CapabilityOperation::Invoke);
    if (!token.HasValue())
    {
        return MakeResult(PagerResolutionError::CapabilityRejected, token.GetError());
    }
    if (token.GetValue().GetDescriptor().Object.GetValue() != pendingToken.GetValue())
    {
        return MakeResult(PagerResolutionError::NotPending);
    }
    auto& tokenStorage = static_cast<FaultToken&>(token.GetValue().GetObject());
    const auto claim = tokenStorage.ClaimResolution();
    if (!claim.HasValue())
    {
        return MakeResult(PagerResolutionError::NotPending);
    }
    // The thread is Blocked in its fault; the pager transaction exits it
    // without enqueue. A rejection after the claim is a kernel invariant
    // failure.
    const auto completed = m_dispatcher.Complete(PagerFaultCompletionRequest{
        .Claim = claim.GetValue().Token,
        .ExitCode = Abi::FaultTerminatedExitCode,
        .Resolution = FaultResolutionKind::Terminate,
        .Reserved = {},
    });
    if (!completed.HasValue())
    {
        return Fail(PagerResolutionError::Faulted);
    }
    return MakeResult(PagerResolutionError::None);
}

PagerResolutionResult PagerChannel::ResolveLeased(Object::CapabilitySpace& caller, const PagerResolveRequest& request,
                                                  const Object::ObjectId pendingToken) noexcept
{
    auto token = caller.Lookup(request.FaultToken, Object::ObjectType::FaultToken, Object::CapabilityOperation::Invoke);
    if (!token.HasValue())
    {
        return MakeResult(PagerResolutionError::CapabilityRejected, token.GetError());
    }
    if (token.GetValue().GetDescriptor().Object.GetValue() != pendingToken.GetValue())
    {
        return MakeResult(PagerResolutionError::NotPending);
    }
    auto& tokenStorage = static_cast<FaultToken&>(token.GetValue().GetObject());
    // No mapping resolves an exception; the fault stays pending for a Terminate.
    if (tokenStorage.GetSnapshot().Fault.Cause == FaultCause::Exception)
    {
        return MakeResult(PagerResolutionError::MappingRejected);
    }

    auto space =
        caller.Lookup(request.AddressSpace, Object::ObjectType::AddressSpace, Object::CapabilityOperation::Map);
    if (!space.HasValue())
    {
        return MakeResult(PagerResolutionError::CapabilityRejected, space.GetError());
    }
    // The unauthorized-mapping check: a pager maps only into the space that
    // faulted, and only with authority over that exact space.
    if (space.GetValue().GetDescriptor().Object.GetValue() !=
        tokenStorage.GetSnapshot().AddressSpace.GetObjectId().GetValue())
    {
        return MakeResult(PagerResolutionError::AddressSpaceMismatch);
    }
    m_request.FaultTokenStorage = &tokenStorage;
    m_request.AddressSpaceStorage = &static_cast<AddressSpace&>(space.GetValue().GetObject());
    m_request.FaultTokenIdentity = pendingToken;
    return ResolveWithBacking(caller, request);
}

PagerResolutionResult PagerChannel::ResolveWithBacking(Object::CapabilitySpace& caller,
                                                       const PagerResolveRequest& request) noexcept
{
    auto memory =
        caller.Lookup(request.MemoryObject, Object::ObjectType::MemoryObject, Object::CapabilityOperation::Map);
    if (!memory.HasValue())
    {
        return MakeResult(PagerResolutionError::CapabilityRejected, memory.GetError());
    }
    auto frame = caller.Lookup(request.Frame, Object::ObjectType::Frame, Object::CapabilityOperation::Map);
    if (!frame.HasValue())
    {
        return MakeResult(PagerResolutionError::CapabilityRejected, frame.GetError());
    }
    // Check CPU write authority before claiming the token, so a refusal is retryable.
    if (request.Mapping.Permissions.Contains(PagePermission::Write) &&
        !frame.GetValue().GetDescriptor().Rights.ContainsAll(Object::CapabilityRights{Object::CapabilityRight::Write}))
    {
        return MakeResult(PagerResolutionError::CapabilityRejected, Object::CapabilityError::RightsDenied);
    }
    m_request.MemoryObjectStorage = &static_cast<MemoryObject&>(memory.GetValue().GetObject());
    m_request.FrameStorage = &static_cast<Frame&>(frame.GetValue().GetObject());
    m_request.MemoryObjectIdentity = MemoryObjectId{m_registry, memory.GetValue().GetDescriptor().Object};
    m_request.FrameIdentity = FrameId{m_registry, frame.GetValue().GetDescriptor().Object};
    m_request.Mapping = request.Mapping;
    const auto claim = m_request.FaultTokenStorage->ClaimResolution();
    if (!claim.HasValue())
    {
        return MakeResult(PagerResolutionError::NotPending);
    }
    m_request.Claim = claim.GetValue().Token;
    return RunResolver();
}

PagerResolutionResult PagerChannel::RunResolver() noexcept
{
    FaultResolutionDispatcherState state{FaultResolutionDispatcherState::Fresh};
    PagerResolutionError rejection{PagerResolutionError::None};
    if (!BeginResolver(state, rejection))
    {
        return (rejection == PagerResolutionError::Faulted) ? Fail(rejection) : MakeResult(rejection);
    }
    for (Scheduler::CrossProcessorWait wait; state == FaultResolutionDispatcherState::ShootdownPending;)
    {
        if (!wait.Poll())
        {
            const auto snapshot = m_request.AddressSpaceStorage->GetSnapshot();
            Scheduler::CrossProcessorWait::Timeout(snapshot.TargetProcessorMask & ~snapshot.AcknowledgedProcessorMask);
        }
        if (m_configuration.ShootdownProgress != nullptr)
        {
            m_configuration.ShootdownProgress->Wait();
        }
        if (!ContinueResolver(state))
        {
            return Fail(PagerResolutionError::Faulted);
        }
    }
    if (state != FaultResolutionDispatcherState::Completed)
    {
        return Fail(PagerResolutionError::Faulted);
    }
    m_resolutionError = FaultResolutionDispatcherError::None;
    return MakeResult(PagerResolutionError::None);
}

bool PagerChannel::BeginResolver(FaultResolutionDispatcherState& state, PagerResolutionError& rejection) noexcept
{
    const auto begun = m_resolver.BeginMap(m_request);
    if (!begun.HasValue())
    {
        m_lastFailure = begun.GetError();
        m_resolutionError = begun.GetError().Error;
        rejection = ClassifyFailure(begun.GetError());
        return false;
    }
    state = begun.GetValue().State;
    return true;
}

bool PagerChannel::ContinueResolver(FaultResolutionDispatcherState& state) noexcept
{
    const auto continued = m_resolver.ContinueMap();
    if (!continued.HasValue())
    {
        m_lastFailure = continued.GetError();
        m_resolutionError = continued.GetError().Error;
        return false;
    }
    state = continued.GetValue().State;
    return true;
}

bool PagerChannel::RetireToken(const Object::ObjectId token) noexcept
{
    if (!m_factory.Retire(token).HasValue())
    {
        return false;
    }
    // Completion and authority withdrawal do not wait for another CPU's lease. The factory's
    // sixteen slots bound this list; an Active token always has room among at most fifteen retired
    // tokens.
    for (auto& retired : m_retiredTokens)
    {
        if (!retired.IsValid())
        {
            retired = token;
            return CollectRetiredTokens();
        }
    }
    return false;
}

bool PagerChannel::CollectRetiredTokens() noexcept
{
    for (auto& token : m_retiredTokens)
    {
        if (!token.IsValid())
        {
            continue;
        }
        const auto collected = m_issuer.CollectDestroyedObject(token);
        if (!collected.HasValue())
        {
            if (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady)
            {
                continue;
            }
            return false;
        }
        if (!m_factory.Reclaim(collected.GetValue()).HasValue())
        {
            return false;
        }
        token = {};
    }
    return true;
}

PagerChannelSnapshot PagerChannel::GetSnapshot() const noexcept
{
    const PagerChannelState state = m_transition.State();
    if (!m_transition.Claim(state, state))
    {
        PagerChannelSnapshot busy{};
        busy.State = state;
        busy.Error = PagerChannelError::Busy;
        return busy;
    }
    const PagerChannelSnapshot result{
        .Ingress = m_ingress.GetSnapshot(),
        .Deliveries = m_deliveries,
        .Resolutions = m_resolutions,
        .Terminations = m_terminations,
        .Busy = m_busy.LoadAcquire(),
        .Resolution = m_resolutionError,
        .State = state,
        .Error = m_error,
        .Reserved = {},
    };
    m_transition.Publish(state);
    return result;
}
} // Namespace Asteria::Kernel::Memory
