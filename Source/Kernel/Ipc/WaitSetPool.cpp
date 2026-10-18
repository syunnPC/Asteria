// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/WaitSetPool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Ipc
{
Foundation::Result<WaitSetCreation, WaitSetPoolFailure>
WaitSetPool::Create(const WaitSetCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<WaitSetCreation, WaitSetPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::Faulted));
    }
    if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::InvalidConfiguration));
    }
    const auto authority = request.Space->Lookup(request.Authority, Object::ObjectType::ResourceDomain,
                                                 Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::CapabilityRejected, authority.GetError()));
    }
    Slot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::CapacityExceeded));
    }

    // The lookup checked the type, and only ResourceDomain instances report
    // ObjectType::ResourceDomain. The pin keeps the domain registered while
    // the slot holds its charge.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    if ((m_objectRegistry.PinObject(authority.GetValue().GetDescriptor().Object, Object::ObjectType::ResourceDomain,
                                    slot->DomainPin) != Object::ObjectPinError::Ok) ||
        (&slot->DomainPin.GetObject() != static_cast<Object::IKernelObject*>(&domain)))
    {
        slot->DomainPin.Reset();
        return ResultType::Failure(MakeFailure(WaitSetPoolError::ResourceDomainRejected));
    }
    WaitSet* const waitSet = ::new (static_cast<void*>(slot->Storage)) WaitSet{m_objectRegistry};
    if (waitSet->Initialize(domain.GetIdentifier()) != WaitSetError::None)
    {
        waitSet->~WaitSet();
        slot->DomainPin.Reset();
        return ResultType::Failure(MakeFailure(WaitSetPoolError::InvalidRequest));
    }
    const Resource::ResourceDomainError charged = domain.TryReserve(GetReservation());
    if (charged != Resource::ResourceDomainError::None)
    {
        waitSet->~WaitSet();
        slot->DomainPin.Reset();
        return ResultType::Failure(MakeFailure(WaitSetPoolError::ChargeRejected, Object::CapabilityError::Ok, charged));
    }
    slot->Instance = waitSet;
    slot->Domain = &domain;

    const auto registered = m_issuer.CreateObject(*waitSet);
    if (!registered.HasValue())
    {
        const WaitSetPoolFailure released = ReleaseSlotLocked(*slot);
        if (released.Error != WaitSetPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        return ResultType::Failure(MakeFailure(WaitSetPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, registered.GetError()));
    }
    slot->Identity = registered.GetValue();
    return MintLocked(*slot, request);
}

Foundation::Result<WaitSetCreation, WaitSetPoolFailure>
WaitSetPool::MintLocked(Slot& slot, const WaitSetCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<WaitSetCreation, WaitSetPoolFailure>;

    const auto minted = m_issuer.MintUnderAuthority(*request.Space, slot.Identity, CreatedRights, request.Authority,
                                                    Object::ObjectType::ResourceDomain,
                                                    Object::CapabilityOperation::Manage, *slot.Domain);
    if (!minted.HasValue())
    {
        // No capability names the WaitSet, so nothing can hold a reference
        // and it is collectable at once.
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        const auto collected =
            (destroyed == Object::ObjectRegistryError::Ok)
                ? m_issuer.CollectDestroyedObject(slot.Identity)
                : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
        if (!collected.HasValue())
        {
            return ResultType::Failure(
                FaultLocked(MakeFailure(WaitSetPoolError::InvariantViolation, minted.GetError(),
                                        Resource::ResourceDomainError::None, collected.GetError())));
        }
        const WaitSetPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != WaitSetPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        return ResultType::Failure(MakeFailure(WaitSetPoolError::MintRejected, minted.GetError()));
    }
    slot.State = SlotState::Active;
    ++m_created;
    return ResultType::Success(WaitSetCreation{
        .WaitSet = slot.Identity,
        .Capability = minted.GetValue(),
    });
}

Foundation::Result<Object::ObjectId, WaitSetPoolFailure>
WaitSetPool::Destroy(Object::CapabilitySpace& space, const Object::CapabilityHandle waitSet) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, WaitSetPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::Faulted));
    }
    const auto lease = space.Lookup(waitSet, Object::ObjectType::WaitSet, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active) ||
        (slot->Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::NotOwned));
    }
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot->Identity);
    if (destroyed != Object::ObjectRegistryError::Ok)
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::DestroyRejected, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, destroyed));
    }
    slot->State = SlotState::Quiescing;
    m_quiescing = true;
    ++m_destroyed;
    return ResultType::Success(slot->Identity);
}

Object::ObjectPoolFailure WaitSetPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetPoolState::Faulted)
    {
        return MakeFailure(WaitSetPoolError::Faulted);
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            return FaultLocked(MakeFailure(WaitSetPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                           Resource::ResourceDomainError::None, destroyed));
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
    }
    return MakeFailure(WaitSetPoolError::None);
}

Foundation::Result<Foundation::Size, WaitSetPoolFailure> WaitSetPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, WaitSetPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(WaitSetPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    // No slot became Quiescing since a Collect left none: nothing to scan.
    if (!m_quiescing)
    {
        return ResultType::Success(0U);
    }
    for (Slot& slot : m_slots)
    {
        if (slot.State != SlotState::Quiescing)
        {
            continue;
        }
        const auto collected = m_issuer.CollectDestroyedObject(slot.Identity);
        if (!collected.HasValue() && (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady))
        {
            continue;
        }
        if (!collected.HasValue() ||
            (collected.GetValue().Instance != static_cast<Object::IKernelObject*>(slot.Instance)))
        {
            return ResultType::Failure(FaultLocked(MakeFailure(
                WaitSetPoolError::InvariantViolation, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError())));
        }
        const WaitSetPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != WaitSetPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        ++reclaimed;
        ++m_reclaimed;
    }
    m_quiescing = false;
    for (const Slot& slot : m_slots)
    {
        m_quiescing = m_quiescing || (slot.State == SlotState::Quiescing);
    }
    return ResultType::Success(reclaimed);
}

WaitSetPoolSnapshot WaitSetPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    WaitSetPoolSnapshot snapshot{
        .FreeSlots = 0U,
        .ActiveSlots = 0U,
        .QuiescingSlots = 0U,
        .Created = m_created,
        .Destroyed = m_destroyed,
        .Reclaimed = m_reclaimed,
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
        }
    }
    return snapshot;
}

WaitSetPool::Slot* WaitSetPool::FindFreeSlotLocked() noexcept
{
    for (Slot& slot : m_slots)
    {
        if ((slot.State == SlotState::Free) && (slot.Instance == nullptr))
        {
            return &slot;
        }
    }
    return nullptr;
}

WaitSetPool::Slot* WaitSetPool::FindSlotLocked(const Object::IKernelObject& instance) noexcept
{
    for (Slot& slot : m_slots)
    {
        if ((slot.Instance != nullptr) && (static_cast<const Object::IKernelObject*>(slot.Instance) == &instance))
        {
            return &slot;
        }
    }
    return nullptr;
}

WaitSetPoolFailure WaitSetPool::ReleaseSlotLocked(Slot& slot) noexcept
{
    const Resource::ResourceDomainError released = slot.Domain->Release(GetReservation());
    if (released != Resource::ResourceDomainError::None)
    {
        return MakeFailure(WaitSetPoolError::InvariantViolation, Object::CapabilityError::Ok, released);
    }
    // Ending the WaitSet detaches every registered Notification and drops
    // its pin, so a destroyed Endpoint's readiness becomes collectable.
    slot.Instance->~WaitSet();
    slot.Instance = nullptr;
    slot.Domain = nullptr;
    slot.DomainPin.Reset();
    slot.Identity = Object::ObjectId{};
    slot.State = SlotState::Free;
    return MakeFailure(WaitSetPoolError::None);
}

WaitSetPoolFailure WaitSetPool::FaultLocked(const WaitSetPoolFailure failure) noexcept
{
    m_state = WaitSetPoolState::Faulted;
    WaitSetPoolFailure faulted = failure;
    faulted.Error = WaitSetPoolError::InvariantViolation;
    return faulted;
}
} // namespace Asteria::Kernel::Ipc
