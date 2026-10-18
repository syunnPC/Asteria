// SPDX-License-Identifier: Apache-2.0

#include "FaultToken.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
[[nodiscard]] constexpr bool IsKnownAccess(const FaultAccess access) noexcept
{
    switch (access)
    {
    case FaultAccess::Read:
    case FaultAccess::Write:
    case FaultAccess::Execute:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownCause(const FaultCause cause) noexcept
{
    switch (cause)
    {
    case FaultCause::MissingMapping:
    case FaultCause::ProtectionViolation:
    case FaultCause::InvalidMappingEncoding:
    case FaultCause::Exception:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownPrivilege(const FaultPrivilege privilege) noexcept
{
    switch (privilege)
    {
    case FaultPrivilege::User:
    case FaultPrivilege::Supervisor:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownResolution(const FaultResolutionKind resolution) noexcept
{
    switch (resolution)
    {
    case FaultResolutionKind::Map:
    case FaultResolutionKind::Terminate:
    case FaultResolutionKind::Redirect:
        return true;

    case FaultResolutionKind::None:
        return false;
    }
    return false;
}

[[nodiscard]] bool FaultRecordIsValid(const FaultRecord& fault) noexcept
{
    if (!IsKnownAccess(fault.Access) || !IsKnownCause(fault.Cause) || !IsKnownPrivilege(fault.Privilege))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < sizeof(fault.Reserved); ++index)
    {
        if (fault.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr FaultTokenError MapThreadPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return FaultTokenError::ThreadNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return FaultTokenError::ThreadTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return FaultTokenError::ThreadPinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return FaultTokenError::InvalidThread;
    case Object::ObjectPinError::Ok:
        return FaultTokenError::None;
    }
    return FaultTokenError::InvalidThread;
}

[[nodiscard]] constexpr FaultTokenError MapAddressSpacePinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return FaultTokenError::AddressSpaceNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return FaultTokenError::AddressSpaceTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return FaultTokenError::AddressSpacePinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return FaultTokenError::InvalidAddressSpace;
    case Object::ObjectPinError::Ok:
        return FaultTokenError::None;
    }
    return FaultTokenError::InvalidAddressSpace;
}
} // namespace

FaultTokenError FaultToken::Initialize(Object::ObjectRegistry& registry, const Object::ResourceDomainId chargeOwner,
                                       const Scheduler::ThreadId thread, const AddressSpaceId addressSpace,
                                       const FaultRecord& fault) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != FaultTokenState::Uninitialized)
    {
        return FaultTokenError::AlreadyInitialized;
    }
    if (!chargeOwner.IsValid())
    {
        return FaultTokenError::InvalidConfiguration;
    }
    if (!FaultRecordIsValid(fault))
    {
        return FaultTokenError::InvalidFaultRecord;
    }
    if (!thread.IsValid())
    {
        return FaultTokenError::InvalidThread;
    }
    if (!addressSpace.IsValid())
    {
        return FaultTokenError::InvalidAddressSpace;
    }
    if (!thread.IsFromRegistry(registry))
    {
        return FaultTokenError::ThreadRegistryMismatch;
    }
    if (!addressSpace.IsFromRegistry(registry))
    {
        return FaultTokenError::AddressSpaceRegistryMismatch;
    }

    const Object::ObjectPinError threadPinError =
        registry.PinObject(thread.GetObjectId(), Object::ObjectType::Thread, m_threadPin);
    if (threadPinError != Object::ObjectPinError::Ok)
    {
        return MapThreadPinError(threadPinError);
    }

    const Object::ObjectPinError addressSpacePinError =
        registry.PinObject(addressSpace.GetObjectId(), Object::ObjectType::AddressSpace, m_addressSpacePin);
    if (addressSpacePinError != Object::ObjectPinError::Ok)
    {
        m_issuerResolution = nullptr;
        m_channelPin.Reset();
        m_threadPin.Reset();
        return MapAddressSpacePinError(addressSpacePinError);
    }

    m_thread = thread;
    m_addressSpace = addressSpace;
    m_fault = fault;
    m_resolution = FaultResolutionKind::None;
    m_state = FaultTokenState::Pending;
    m_registry = &registry;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return FaultTokenError::None;
}

Foundation::Result<FaultResolutionClaim, FaultTokenError> FaultToken::ClaimResolution() noexcept
{
    using ResultType = Foundation::Result<FaultResolutionClaim, FaultTokenError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenState::Uninitialized)
    {
        return ResultType::Failure(FaultTokenError::NotInitialized);
    }
    if (m_state == FaultTokenState::Resolved)
    {
        return ResultType::Failure(FaultTokenError::AlreadyResolved);
    }
    if (m_state == FaultTokenState::Claimed)
    {
        return ResultType::Failure(FaultTokenError::AlreadyClaimed);
    }

    Foundation::UInt64 nextClaimSequence{0U};
    if (!FaultClaimSequencePolicy::TryAdvance(m_claimSequence, nextClaimSequence))
    {
        return ResultType::Failure(FaultTokenError::ClaimSequenceExhausted);
    }

    m_claimSequence = nextClaimSequence;
    m_state = FaultTokenState::Claimed;
    return ResultType::Success(FaultResolutionClaim{
        .Token = FaultResolutionClaimToken{*this, m_claimSequence},
        .Thread = m_thread,
        .AddressSpace = m_addressSpace,
        .Fault = m_fault,
    });
}

Foundation::Result<FaultResolutionClaim, FaultTokenError>
FaultToken::GetResolutionClaim(const FaultResolutionClaimToken claim) const noexcept
{
    using ResultType = Foundation::Result<FaultResolutionClaim, FaultTokenError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenState::Uninitialized)
    {
        return ResultType::Failure(FaultTokenError::NotInitialized);
    }
    if (m_state == FaultTokenState::Resolved)
    {
        return ResultType::Failure(FaultTokenError::AlreadyResolved);
    }
    if (m_state != FaultTokenState::Claimed)
    {
        return ResultType::Failure(FaultTokenError::NotClaimed);
    }
    if (!claim.IsValid() || (claim.m_faultToken != this) || (claim.GetSequence() != m_claimSequence))
    {
        return ResultType::Failure(FaultTokenError::InvalidClaim);
    }

    return ResultType::Success(FaultResolutionClaim{
        .Token = FaultResolutionClaimToken{*this, m_claimSequence},
        .Thread = m_thread,
        .AddressSpace = m_addressSpace,
        .Fault = m_fault,
    });
}

FaultTokenError FaultToken::CancelResolution(const FaultResolutionClaimToken claim) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenState::Uninitialized)
    {
        return FaultTokenError::NotInitialized;
    }
    if (m_state == FaultTokenState::Resolved)
    {
        return FaultTokenError::AlreadyResolved;
    }
    if (m_state != FaultTokenState::Claimed)
    {
        return FaultTokenError::NotClaimed;
    }
    if (!claim.IsValid() || (claim.m_faultToken != this) || (claim.GetSequence() != m_claimSequence))
    {
        return FaultTokenError::InvalidClaim;
    }

    m_state = FaultTokenState::Pending;
    return FaultTokenError::None;
}

FaultTokenError FaultToken::CommitResolution(const FaultResolutionClaimToken claim,
                                             const FaultResolutionKind resolution) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenState::Uninitialized)
    {
        return FaultTokenError::NotInitialized;
    }
    if (!IsKnownResolution(resolution))
    {
        return FaultTokenError::InvalidResolution;
    }
    if (m_state == FaultTokenState::Resolved)
    {
        return FaultTokenError::AlreadyResolved;
    }
    if (m_state != FaultTokenState::Claimed)
    {
        return FaultTokenError::NotClaimed;
    }
    if (!claim.IsValid() || (claim.m_faultToken != this) || (claim.GetSequence() != m_claimSequence))
    {
        return FaultTokenError::InvalidClaim;
    }

    m_resolution = resolution;
    m_state = FaultTokenState::Resolved;
    return FaultTokenError::None;
}

bool FaultToken::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state != FaultTokenState::Uninitialized;
}

FaultTokenSnapshot FaultToken::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetSnapshotLocked();
}

Object::ObjectType FaultToken::GetObjectType() const noexcept
{
    return Object::ObjectType::FaultToken;
}

Object::ResourceDomainId FaultToken::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

Object::ObjectPinError FaultToken::PinResolutionChannel(Object::ObjectPin& pin) const noexcept
{
    return m_channelPin.IsValid()
               ? m_registry->PinObject(m_channelPin.GetObjectId(), Object::ObjectType::FaultChannel, pin)
               : Object::ObjectPinError::Ok;
}

FaultTokenError FaultToken::ResetForFactoryReuse() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenState::Uninitialized)
    {
        return FaultTokenError::NotInitialized;
    }
    if (m_state == FaultTokenState::Claimed)
    {
        return FaultTokenError::AlreadyClaimed;
    }

    m_thread = Scheduler::ThreadId{};
    m_addressSpace = AddressSpaceId{};
    m_fault = FaultRecord{};
    m_resolution = FaultResolutionKind::None;
    m_state = FaultTokenState::Uninitialized;
    m_chargeOwnerValue.StoreRelease(0U);
    m_addressSpacePin.Reset();
    m_issuerResolution = nullptr;
    m_channelPin.Reset();
    m_threadPin.Reset();
    return (m_claimSequence == FaultClaimSequencePolicy::Maximum) ? FaultTokenError::ClaimSequenceExhausted
                                                                  : FaultTokenError::None;
}

FaultTokenSnapshot FaultToken::GetSnapshotLocked() const noexcept
{
    return FaultTokenSnapshot{
        .Thread = m_thread,
        .AddressSpace = m_addressSpace,
        .Fault = m_fault,
        .ClaimSequence = m_claimSequence,
        .State = m_state,
        .Resolution = m_resolution,
        .Reserved = {},
    };
}
} // namespace Asteria::Kernel::Memory
