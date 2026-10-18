// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/FaultTokenFactory.hpp"

namespace Asteria::Kernel::Memory
{
FaultTokenFactoryFailure FaultTokenFactory::MakeFailure(const FaultTokenFactoryError error,
                                                        const Object::ObjectId token, const FaultTokenError tokenError,
                                                        const Object::ObjectRegistryError registryError,
                                                        const Object::CapabilityError capabilityError) noexcept
{
    return FaultTokenFactoryFailure{
        .Token = token,
        .TokenError = tokenError,
        .RegistryError = registryError,
        .CapabilityError = capabilityError,
        .Error = error,
        .Reserved = {0U, 0U, 0U, 0U},
    };
}

Foundation::Result<FaultTokenCreation, FaultTokenFactoryFailure>
FaultTokenFactory::Create(const FaultTokenCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<FaultTokenCreation, FaultTokenFactoryFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenFactoryState::Faulted)
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::Faulted));
    }
    if ((request.Destination == nullptr) || !request.Destination->IsReady() || !request.RevocationDomain.IsValid())
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::InvalidConfiguration));
    }

    Slot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::CapacityExceeded));
    }

    const FaultTokenError tokenError = slot->Token.Initialize(m_objectRegistry, request.ChargeOwner, request.Thread,
                                                              request.AddressSpace, request.Fault);
    if (tokenError != FaultTokenError::None)
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::TokenRejected, {}, tokenError));
    }

    const auto registered = m_issuer.CreateObject(slot->Token);
    if (!registered.HasValue())
    {
        const FaultTokenError resetError = slot->Token.ResetForFactoryReuse();
        if ((resetError != FaultTokenError::None) && (resetError != FaultTokenError::ClaimSequenceExhausted))
        {
            EnterFaultedLocked(*slot);
            return ResultType::Failure(
                MakeFailure(FaultTokenFactoryError::RecycleRejected, {}, resetError, registered.GetError()));
        }
        if (resetError == FaultTokenError::ClaimSequenceExhausted)
        {
            slot->State = SlotState::Retired;
        }
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::RegistrationRejected, {}, FaultTokenError::None,
                                               registered.GetError()));
    }

    slot->Identity = registered.GetValue();
    slot->State = SlotState::Active;
    slot->Token.m_issuerResolution = request.Resolution;
    if (request.ChannelIdentity.IsValid() &&
        m_objectRegistry.PinObject(request.ChannelIdentity, Object::ObjectType::FaultChannel,
                                   slot->Token.m_channelPin) != Object::ObjectPinError::Ok)
    {
        const auto withdrawn = m_issuer.DestroyObject(slot->Identity);
        if (withdrawn != Object::ObjectRegistryError::Ok)
        {
            EnterFaultedLocked(*slot);
            return ResultType::Failure(MakeFailure(FaultTokenFactoryError::RollbackRejected, slot->Identity,
                                                   FaultTokenError::None, withdrawn));
        }
        slot->State = SlotState::Quiescing;
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::InvalidConfiguration));
    }
    constexpr Object::CapabilityRights rights =
        Object::CapabilityRights{Object::CapabilityRight::Invoke}.With(Object::CapabilityRight::Transfer);
    const auto capability = m_issuer.Mint(*request.Destination, slot->Identity, rights, request.RevocationDomain,
                                          request.LeaseDeadline, 0U);
    if (!capability.HasValue())
    {
        const Object::ObjectRegistryError rollback = m_issuer.DestroyObject(slot->Identity);
        if (rollback != Object::ObjectRegistryError::Ok)
        {
            EnterFaultedLocked(*slot);
            return ResultType::Failure(MakeFailure(FaultTokenFactoryError::RollbackRejected, slot->Identity,
                                                   FaultTokenError::None, rollback, capability.GetError()));
        }
        slot->State = SlotState::Quiescing;
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::CapabilityRejected, slot->Identity,
                                               FaultTokenError::None, Object::ObjectRegistryError::Ok,
                                               capability.GetError()));
    }

    return ResultType::Success(FaultTokenCreation{
        .Storage = &slot->Token,
        .Token = slot->Identity,
        .Capability = capability.GetValue(),
    });
}

Foundation::Result<Object::ObjectId, FaultTokenFactoryFailure>
FaultTokenFactory::Retire(const Object::ObjectId token) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, FaultTokenFactoryFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenFactoryState::Faulted)
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::Faulted, token));
    }
    if (!token.IsValid())
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::InvalidToken, token));
    }
    Slot* const slot = FindSlotLocked(token);
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::TokenNotOwned, token));
    }
    if (slot->State != SlotState::Active)
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::TokenBusy, token));
    }
    const FaultTokenState tokenState = slot->Token.GetSnapshot().State;
    if ((tokenState != FaultTokenState::Pending) && (tokenState != FaultTokenState::Resolved))
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::TokenBusy, token));
    }

    const Object::ObjectRegistryError destroyError = m_issuer.DestroyObject(token);
    if (destroyError != Object::ObjectRegistryError::Ok)
    {
        return ResultType::Failure(
            MakeFailure(FaultTokenFactoryError::DestroyRejected, token, FaultTokenError::None, destroyError));
    }
    slot->State = SlotState::Quiescing;
    return ResultType::Success(token);
}

Foundation::Result<Object::ObjectId, FaultTokenFactoryFailure>
FaultTokenFactory::Reclaim(const Object::ObjectDestructionRecord& record) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, FaultTokenFactoryFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == FaultTokenFactoryState::Faulted)
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::Faulted, record.Object));
    }
    if (!record.IsFromRegistry(m_objectRegistry) || !record.Object.IsValid() || (record.Instance == nullptr))
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::InvalidDestructionRecord, record.Object));
    }
    Slot* const slot = FindSlotLocked(record.Instance);
    if ((slot == nullptr) || (slot->State != SlotState::Quiescing) ||
        (slot->Identity.GetValue() != record.Object.GetValue()))
    {
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::InvalidDestructionRecord, record.Object));
    }

    const FaultTokenError resetError = slot->Token.ResetForFactoryReuse();
    if ((resetError != FaultTokenError::None) && (resetError != FaultTokenError::ClaimSequenceExhausted))
    {
        EnterFaultedLocked(*slot);
        return ResultType::Failure(MakeFailure(FaultTokenFactoryError::RecycleRejected, record.Object, resetError));
    }

    const Object::ObjectId reclaimed = slot->Identity;
    slot->Identity = Object::ObjectId{};
    slot->State = (resetError == FaultTokenError::ClaimSequenceExhausted) ? SlotState::Retired : SlotState::Free;
    return ResultType::Success(reclaimed);
}

FaultTokenFactorySnapshot FaultTokenFactory::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    FaultTokenFactorySnapshot snapshot{
        .FreeSlots = 0U,
        .ActiveSlots = 0U,
        .QuiescingSlots = 0U,
        .RetiredSlots = 0U,
        .FaultedSlots = 0U,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
    for (const Slot& slot : m_slots)
    {
        switch (slot.State)
        {
        case SlotState::Free:
            ++snapshot.FreeSlots;
            break;
        case SlotState::Active:
            ++snapshot.ActiveSlots;
            break;
        case SlotState::Quiescing:
            ++snapshot.QuiescingSlots;
            break;
        case SlotState::Retired:
            ++snapshot.RetiredSlots;
            break;
        case SlotState::Faulted:
            ++snapshot.FaultedSlots;
            break;
        }
    }
    return snapshot;
}

FaultTokenFactory::Slot* FaultTokenFactory::FindFreeSlotLocked() noexcept
{
    for (Slot& slot : m_slots)
    {
        if (slot.State == SlotState::Free)
        {
            return &slot;
        }
    }
    return nullptr;
}

FaultTokenFactory::Slot* FaultTokenFactory::FindSlotLocked(const Object::ObjectId token) noexcept
{
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Free) && (slot.State != SlotState::Retired) &&
            (slot.Identity.GetValue() == token.GetValue()))
        {
            return &slot;
        }
    }
    return nullptr;
}

FaultTokenFactory::Slot* FaultTokenFactory::FindSlotLocked(Object::IKernelObject* const instance) noexcept
{
    for (Slot& slot : m_slots)
    {
        if (static_cast<Object::IKernelObject*>(&slot.Token) == instance)
        {
            return &slot;
        }
    }
    return nullptr;
}

void FaultTokenFactory::EnterFaultedLocked(Slot& slot) noexcept
{
    slot.State = SlotState::Faulted;
    m_state = FaultTokenFactoryState::Faulted;
}
} // namespace Asteria::Kernel::Memory
