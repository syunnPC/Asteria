// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/FaultResolutionDispatcher.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
[[nodiscard]] bool ReservedBytesAreZero(const FaultResolutionMapRequest& request) noexcept
{
    for (const Foundation::UInt8 value : request.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool NativeCommitMayBeUncertain(const AddressSpaceFailure& failure) noexcept
{
    return (failure.PageTableError == Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown) ||
           (failure.Error == AddressSpaceError::PageTableProtocolViolation);
}
} // namespace

FaultResolutionDispatcherFailure FaultResolutionDispatcher::MakeFailure(const FaultResolutionDispatcherError error,
                                                                        const FaultResolutionDispatcherState state,
                                                                        const FaultResolutionNativeState nativeState,
                                                                        const Foundation::UInt64 claimSequence) noexcept
{
    return FaultResolutionDispatcherFailure{
        .ClaimSequence = claimSequence,
        .AddressSpace = {},
        .Shootdown = {},
        .Pager = {},
        .Token = FaultTokenError::None,
        .Pin = Object::ObjectPinError::Ok,
        .Preparation = FaultFrameMappingError::None,
        .Error = error,
        .State = state,
        .NativeState = nativeState,
        .Terminal = false,
        .ClaimCancelled = false,
        .RemoteWorkPublished = false,
        .Reserved = 0U,
    };
}

bool FaultResolutionDispatcher::ValidateRequest(const FaultResolutionMapRequest& request,
                                                FaultResolutionDispatcherFailure& failure) noexcept
{
    return AuthenticateClaim(request, failure) && ValidateConfiguration(request, failure) &&
           PinFaultToken(request, failure) && ValidateAddressSpaceCorrelation(failure) &&
           ValidatePagerCorrelation(request, failure) && PrepareMapping(request, failure);
}

bool FaultResolutionDispatcher::AuthenticateClaim(const FaultResolutionMapRequest& request,
                                                  FaultResolutionDispatcherFailure& failure) noexcept
{
    if (request.FaultTokenStorage == nullptr)
    {
        failure = MakeFailure(FaultResolutionDispatcherError::InvalidConfiguration, m_state, m_nativeState);
        return false;
    }

    const auto authoritative = request.FaultTokenStorage->GetResolutionClaim(request.Claim);
    if (!authoritative.HasValue())
    {
        failure = MakeFailure(FaultResolutionDispatcherError::ClaimRejected, m_state, m_nativeState,
                              request.Claim.GetSequence());
        failure.Token = authoritative.GetError();
        return false;
    }

    m_faultToken = request.FaultTokenStorage;
    m_addressSpace = request.AddressSpaceStorage;
    m_claim = authoritative.GetValue().Token;
    m_claimSequence = m_claim.GetSequence();
    m_threadIdentity = authoritative.GetValue().Thread;
    m_addressSpaceIdentity = authoritative.GetValue().AddressSpace;
    return true;
}

bool FaultResolutionDispatcher::ValidateConfiguration(const FaultResolutionMapRequest& request,
                                                      FaultResolutionDispatcherFailure& failure) const noexcept
{
    if ((request.AddressSpaceStorage == nullptr) || (request.MemoryObjectStorage == nullptr) ||
        (request.FrameStorage == nullptr) || !request.FaultTokenIdentity.IsValid() || !ReservedBytesAreZero(request) ||
        !m_shootdownDispatcher.GetProcessor().IsValid())
    {
        failure =
            MakeFailure(FaultResolutionDispatcherError::InvalidConfiguration, m_state, m_nativeState, m_claimSequence);
        return false;
    }
    return true;
}

bool FaultResolutionDispatcher::PinFaultToken(const FaultResolutionMapRequest& request,
                                              FaultResolutionDispatcherFailure& failure) noexcept
{
    const Object::ObjectPinError pinError =
        m_objectRegistry.PinObject(request.FaultTokenIdentity, Object::ObjectType::FaultToken, m_faultTokenPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        failure =
            MakeFailure(FaultResolutionDispatcherError::FaultTokenPinRejected, m_state, m_nativeState, m_claimSequence);
        failure.Pin = pinError;
        return false;
    }
    if (&m_faultTokenPin.GetObject() != static_cast<Object::IKernelObject*>(m_faultToken))
    {
        failure = MakeFailure(FaultResolutionDispatcherError::FaultTokenInstanceMismatch, m_state, m_nativeState,
                              m_claimSequence);
        return false;
    }
    return true;
}

bool FaultResolutionDispatcher::ValidateAddressSpaceCorrelation(
    FaultResolutionDispatcherFailure& failure) const noexcept
{
    const AddressSpaceSnapshot addressSpace = m_addressSpace->GetSnapshot();
    if (!addressSpace.Identity.IsValid() || !addressSpace.Identity.IsFromRegistry(m_objectRegistry) ||
        !addressSpace.Identity.IsEqualTo(m_addressSpaceIdentity))
    {
        failure = MakeFailure(FaultResolutionDispatcherError::CorrelatedStateChanged, m_state, m_nativeState,
                              m_claimSequence);
        return false;
    }
    return true;
}

bool FaultResolutionDispatcher::ValidatePagerCorrelation(const FaultResolutionMapRequest& request,
                                                         FaultResolutionDispatcherFailure& failure) const noexcept
{
    const PagerFaultDispatcherSnapshot pager = m_pagerDispatcher.GetSnapshot();
    if ((pager.State != PagerFaultDispatcherState::Delivered) ||
        (pager.FaultToken.GetValue() != request.FaultTokenIdentity.GetValue()) ||
        !pager.Thread.IsEqualTo(m_threadIdentity))
    {
        failure = MakeFailure(FaultResolutionDispatcherError::CorrelatedStateChanged, m_state, m_nativeState,
                              m_claimSequence);
        return false;
    }
    return true;
}

bool FaultResolutionDispatcher::PrepareMapping(const FaultResolutionMapRequest& request,
                                               FaultResolutionDispatcherFailure& failure) noexcept
{
    const FaultFrameMappingError preparationError =
        m_preparation.Prepare(m_objectRegistry, *m_faultToken, m_claim, request.MemoryObjectIdentity,
                              *request.MemoryObjectStorage, request.DeviceRegionIdentity, request.DeviceRegionStorage,
                              request.FrameIdentity, *request.FrameStorage, request.Mapping);
    if (preparationError != FaultFrameMappingError::None)
    {
        failure = MakeFailure(FaultResolutionDispatcherError::MappingPreparationRejected, m_state, m_nativeState,
                              m_claimSequence);
        failure.Preparation = preparationError;
        return false;
    }
    return true;
}

bool FaultResolutionDispatcher::CancelBeforeNative(FaultResolutionDispatcherFailure& failure,
                                                   const bool ownerIsTerminal) noexcept
{
    const FaultTokenError cancelError = m_faultToken->CancelResolution(m_claim);
    if (cancelError != FaultTokenError::None)
    {
        m_state = FaultResolutionDispatcherState::Terminal;
        m_nativeState = FaultResolutionNativeState::Unchanged;
        failure.Token = cancelError;
        failure.Error = FaultResolutionDispatcherError::ClaimCancellationRejected;
        failure.State = m_state;
        failure.NativeState = m_nativeState;
        failure.Terminal = true;
        return false;
    }

    m_state = ownerIsTerminal ? FaultResolutionDispatcherState::Terminal : FaultResolutionDispatcherState::Cancelled;
    m_nativeState = FaultResolutionNativeState::Unchanged;
    failure.State = m_state;
    failure.NativeState = m_nativeState;
    failure.Terminal = ownerIsTerminal;
    failure.ClaimCancelled = true;
    return true;
}

Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
FaultResolutionDispatcher::BeginMap(const FaultResolutionMapRequest& request) noexcept
{
    using ResultType = Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>;

    if (m_state != FaultResolutionDispatcherState::Fresh)
    {
        FaultResolutionDispatcherFailure failure =
            MakeFailure((m_state == FaultResolutionDispatcherState::Completed)
                            ? FaultResolutionDispatcherError::AlreadyCompleted
                            : ((m_state == FaultResolutionDispatcherState::Terminal)
                                   ? FaultResolutionDispatcherError::Terminal
                                   : FaultResolutionDispatcherError::AlreadyStarted),
                        m_state, m_nativeState, m_claimSequence);
        failure.Terminal = m_state == FaultResolutionDispatcherState::Terminal;
        failure.RemoteWorkPublished = m_remoteWorkPublished;
        return ResultType::Failure(failure);
    }

    FaultResolutionDispatcherFailure failure{};
    if (!ValidateRequest(request, failure))
    {
        if ((m_faultToken != nullptr) && m_claim.IsValid())
        {
            static_cast<void>(CancelBeforeNative(failure, false));
        }
        return ResultType::Failure(failure);
    }

    const auto begun = m_addressSpace->WaitForFaultMapping(m_preparation);
    if (!begun.HasValue())
    {
        failure =
            MakeFailure(FaultResolutionDispatcherError::AddressSpaceRejected, m_state, m_nativeState, m_claimSequence);
        failure.AddressSpace = begun.GetError();
        if (NativeCommitMayBeUncertain(begun.GetError()))
        {
            EnterTerminal(FaultResolutionNativeState::StateUncertain);
            failure.State = m_state;
            failure.NativeState = m_nativeState;
            failure.Terminal = true;
        }
        else
        {
            const bool ownerIsTerminal = m_addressSpace->GetState() == AddressSpaceState::Faulted;
            static_cast<void>(CancelBeforeNative(failure, ownerIsTerminal));
        }
        return ResultType::Failure(failure);
    }

    m_mappingToken = begun.GetValue().GetToken();
    m_nativeState = FaultResolutionNativeState::InstalledUnexposed;
    const auto dispatched = m_shootdownDispatcher.Dispatch(*m_addressSpace, begun.GetValue(), m_transport);
    if (!dispatched.HasValue())
    {
        EnterTerminal(FaultResolutionNativeState::InstalledUnexposed);
        failure =
            MakeFailure(FaultResolutionDispatcherError::ShootdownRejected, m_state, m_nativeState, m_claimSequence);
        failure.Shootdown = dispatched.GetError();
        failure.Terminal = true;
        return ResultType::Failure(failure);
    }

    m_shootdownReceipt = dispatched.GetValue();
    m_remoteWorkPublished = m_shootdownReceipt.QueuedProcessorMask != 0U;
    m_state = FaultResolutionDispatcherState::ShootdownPending;
    return CompleteMappingAndPager();
}

Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
FaultResolutionDispatcher::ContinueMap() noexcept
{
    using ResultType = Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>;

    if (m_state == FaultResolutionDispatcherState::ShootdownPending)
    {
        return CompleteMappingAndPager();
    }
    if (m_state == FaultResolutionDispatcherState::MappingCommitted)
    {
        return CompletePager();
    }

    FaultResolutionDispatcherFailure failure = MakeFailure((m_state == FaultResolutionDispatcherState::Completed)
                                                               ? FaultResolutionDispatcherError::AlreadyCompleted
                                                               : ((m_state == FaultResolutionDispatcherState::Terminal)
                                                                      ? FaultResolutionDispatcherError::Terminal
                                                                      : FaultResolutionDispatcherError::NotPending),
                                                           m_state, m_nativeState, m_claimSequence);
    failure.Terminal = m_state == FaultResolutionDispatcherState::Terminal;
    failure.RemoteWorkPublished = m_remoteWorkPublished;
    return ResultType::Failure(failure);
}

Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
FaultResolutionDispatcher::CompleteMappingAndPager() noexcept
{
    using ResultType = Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>;

    const auto completed = m_addressSpace->CompleteFaultMapping(m_mappingToken);
    if (!completed.HasValue())
    {
        if (completed.GetError().Error == AddressSpaceError::ShootdownIncomplete)
        {
            return ResultType::Success(MakeReceipt());
        }

        EnterTerminal(FaultResolutionNativeState::InstalledUnexposed);
        FaultResolutionDispatcherFailure failure = MakeFailure(
            FaultResolutionDispatcherError::MappingCompletionRejected, m_state, m_nativeState, m_claimSequence);
        failure.AddressSpace = completed.GetError();
        failure.Terminal = true;
        failure.RemoteWorkPublished = m_remoteWorkPublished;
        return ResultType::Failure(failure);
    }

    const FaultFrameMappingPlan& plan = m_preparation.GetPlan();
    if ((completed.GetValue().MappingSequence != m_mappingToken.GetSequence()) ||
        (completed.GetValue().VirtualStart.GetValue() != plan.VirtualStart.GetValue()) ||
        (completed.GetValue().Pages.GetValue() != plan.Pages.GetValue()))
    {
        EnterTerminal(FaultResolutionNativeState::Committed);
        FaultResolutionDispatcherFailure failure =
            MakeFailure(FaultResolutionDispatcherError::ProtocolViolation, m_state, m_nativeState, m_claimSequence);
        failure.Terminal = true;
        failure.RemoteWorkPublished = m_remoteWorkPublished;
        return ResultType::Failure(failure);
    }

    m_mappingReceipt = completed.GetValue();
    m_nativeState = FaultResolutionNativeState::Committed;
    m_state = FaultResolutionDispatcherState::MappingCommitted;
    return CompletePager();
}

Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
FaultResolutionDispatcher::CompletePager() noexcept
{
    using ResultType = Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>;

    PagerFaultDispatcherFailure pagerFailure{};
    if (!RequestPagerCompletion(pagerFailure))
    {
        FaultResolutionDispatcherFailure failure = MakeFailure(FaultResolutionDispatcherError::PagerCompletionRejected,
                                                               m_state, m_nativeState, m_claimSequence);
        failure.Pager = pagerFailure;
        failure.RemoteWorkPublished = m_remoteWorkPublished;

        if (PagerCompletionFailureIsTerminal())
        {
            EnterTerminal(FaultResolutionNativeState::Committed);
            failure.State = m_state;
            failure.Terminal = true;
        }
        return ResultType::Failure(failure);
    }

    if (!PagerCompletionIsValid())
    {
        EnterTerminal(FaultResolutionNativeState::Committed);
        FaultResolutionDispatcherFailure failure =
            MakeFailure(FaultResolutionDispatcherError::ProtocolViolation, m_state, m_nativeState, m_claimSequence);
        failure.Terminal = true;
        failure.RemoteWorkPublished = m_remoteWorkPublished;
        return ResultType::Failure(failure);
    }

    m_state = FaultResolutionDispatcherState::Completed;
    return ResultType::Success(MakeReceipt());
}

bool FaultResolutionDispatcher::RequestPagerCompletion(PagerFaultDispatcherFailure& failure) noexcept
{
    const auto completed = m_pagerDispatcher.Complete(PagerFaultCompletionRequest{
        .Claim = m_claim,
        .ExitCode = 0U,
        .Resolution = FaultResolutionKind::Map,
        .Reserved = {},
    });
    if (!completed.HasValue())
    {
        failure = completed.GetError();
        return false;
    }
    m_pagerReceipt = completed.GetValue();
    return true;
}

bool FaultResolutionDispatcher::PagerCompletionFailureIsTerminal() const noexcept
{
    {
        const auto authoritative = m_faultToken->GetResolutionClaim(m_claim);
        if (!authoritative.HasValue())
        {
            return true;
        }
    }
    if (m_faultToken->GetSnapshot().State == FaultTokenState::Resolved)
    {
        return true;
    }
    const PagerFaultDispatcherState pagerState = m_pagerDispatcher.GetSnapshot().State;
    return (pagerState == PagerFaultDispatcherState::Faulted) || (pagerState == PagerFaultDispatcherState::Completed);
}

bool FaultResolutionDispatcher::PagerCompletionIsValid() const noexcept
{
    return (m_pagerReceipt.State == PagerFaultDispatcherState::Completed) &&
           (m_pagerReceipt.Resolution == FaultResolutionKind::Map) &&
           (m_faultToken->GetSnapshot().State == FaultTokenState::Resolved);
}

FaultResolutionDispatcherReceipt FaultResolutionDispatcher::MakeReceipt() const noexcept
{
    return FaultResolutionDispatcherReceipt{
        .Mapping = m_mappingReceipt,
        .Shootdown = m_shootdownReceipt,
        .Pager = m_pagerReceipt,
        .ClaimSequence = m_claimSequence,
        .State = m_state,
        .NativeState = m_nativeState,
        .RemoteWorkPublished = m_remoteWorkPublished,
        .Reserved = {},
    };
}

void FaultResolutionDispatcher::EnterTerminal(const FaultResolutionNativeState nativeState) noexcept
{
    m_state = FaultResolutionDispatcherState::Terminal;
    m_nativeState = nativeState;
}
FaultResolutionDispatcherError FaultResolutionDispatcher::Reset() noexcept
{
    if ((m_state != FaultResolutionDispatcherState::Fresh) && (m_state != FaultResolutionDispatcherState::Completed) &&
        (m_state != FaultResolutionDispatcherState::Cancelled))
    {
        return FaultResolutionDispatcherError::AlreadyStarted;
    }
    m_preparation.Reset();
    m_faultTokenPin.Reset();
    m_faultToken = nullptr;
    m_addressSpace = nullptr;
    m_claim = FaultResolutionClaimToken{};
    m_threadIdentity = Scheduler::ThreadId{};
    m_addressSpaceIdentity = AddressSpaceId{};
    m_mappingToken = AddressSpaceMappingToken{};
    m_mappingReceipt = AddressSpaceMappingReceipt{};
    m_shootdownReceipt = TlbShootdownReceipt{};
    m_pagerReceipt = PagerFaultDispatcherReceipt{};
    m_claimSequence = 0U;
    m_state = FaultResolutionDispatcherState::Fresh;
    m_nativeState = FaultResolutionNativeState::Unchanged;
    m_remoteWorkPublished = false;
    return FaultResolutionDispatcherError::None;
}
} // namespace Asteria::Kernel::Memory
