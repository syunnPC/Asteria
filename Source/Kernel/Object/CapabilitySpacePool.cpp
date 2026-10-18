// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Object/CapabilitySpacePool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Object
{
bool CapabilitySpacePool::Configure(RevocationRegistry& revocationRegistry, Time::IMonotonicClock& clock,
                                    const CapabilityValidationKey (&keys)[Capacity]) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != CapabilitySpacePoolState::Unconfigured)
    {
        return false;
    }
    for (const CapabilityValidationKey& key : keys)
    {
        if ((key.Word0 == 0U) && (key.Word1 == 0U))
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        m_keys[index] = keys[index];
    }
    m_revocationRegistry = &revocationRegistry;
    m_clock = &clock;
    m_state = CapabilitySpacePoolState::Ready;
    return true;
}

Foundation::Result<CapabilitySpaceCreation, ObjectPoolFailure>
CapabilitySpacePool::Create(const CapabilitySpaceCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<CapabilitySpaceCreation, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == CapabilitySpacePoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
    }
    if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidConfiguration));
    }
    CapabilitySpace& caller = *request.Space;
    const auto authority = caller.Lookup(request.Authority, ObjectType::ResourceDomain, CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, authority.GetError()));
    }
    Foundation::Size index{Capacity};
    for (Foundation::Size candidate{0U}; candidate < Capacity; ++candidate)
    {
        if ((index == Capacity) && (m_slots[candidate].State == SlotState::Free))
        {
            index = candidate;
        }
    }
    // An unconfigured pool has no keys: no space can be created.
    if ((m_state != CapabilitySpacePoolState::Ready) || (index == Capacity))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapacityExceeded));
    }
    Slot& slot = m_slots[index];

    // The lookup checked the type, and only ResourceDomain instances report
    // ObjectType::ResourceDomain. The pin keeps the domain registered while
    // the slot holds its charge.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    if ((m_objectRegistry.PinObject(authority.GetValue().GetDescriptor().Object, ObjectType::ResourceDomain,
                                    slot.DomainPin) != ObjectPinError::Ok) ||
        (&slot.DomainPin.GetObject() != static_cast<IKernelObject*>(&domain)))
    {
        slot.DomainPin.Reset();
        return ResultType::Failure(MakeFailure(ObjectPoolError::ResourceDomainRejected));
    }
    const Resource::ResourceDomainError charged = domain.TryReserve(GetReservation());
    if (charged != Resource::ResourceDomainError::None)
    {
        slot.DomainPin.Reset();
        return ResultType::Failure(MakeFailure(ObjectPoolError::ChargeRejected, CapabilityError::Ok, charged));
    }
    slot.Domain = &domain;
    slot.Space = ::new (static_cast<void*>(slot.SpaceStorage))
        CapabilitySpace{m_objectRegistry, *m_revocationRegistry, *m_clock, m_keys[index].Word0, m_keys[index].Word1};
    slot.Instance =
        ::new (static_cast<void*>(slot.ObjectStorage)) CapabilitySpaceObject{*slot.Space, domain.GetIdentifier()};
    if (!slot.Space->IsReady())
    {
        const ObjectPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != ObjectPoolError::None)
        {
            m_state = CapabilitySpacePoolState::Faulted;
            return ResultType::Failure(released);
        }
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidConfiguration));
    }
    return PublishLocked(slot, caller, request.Authority);
}

Foundation::Result<CapabilitySpaceCreation, ObjectPoolFailure>
CapabilitySpacePool::PublishLocked(Slot& slot, CapabilitySpace& caller, const CapabilityHandle authority) noexcept
{
    using ResultType = Foundation::Result<CapabilitySpaceCreation, ObjectPoolFailure>;

    const auto registered = m_issuer.CreateObject(*slot.Instance);
    if (!registered.HasValue())
    {
        const ObjectPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != ObjectPoolError::None)
        {
            m_state = CapabilitySpacePoolState::Faulted;
            return ResultType::Failure(released);
        }
        return ResultType::Failure(MakeFailure(ObjectPoolError::RegistrationRejected, CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, registered.GetError()));
    }
    slot.Identity = registered.GetValue();
    const auto minted =
        m_issuer.MintUnderAuthority(caller, slot.Identity, CreatedRights, authority, ObjectType::ResourceDomain,
                                    CapabilityOperation::Manage, *slot.Domain);
    if (!minted.HasValue())
    {
        // No capability names the space, so nothing can hold a reference and
        // it is collectable at once.
        const ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        const auto collected =
            (destroyed == ObjectRegistryError::Ok)
                ? m_issuer.CollectDestroyedObject(slot.Identity)
                : Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>::Failure(destroyed);
        const ObjectPoolFailure released = collected.HasValue()
                                               ? ReleaseSlotLocked(slot)
                                               : MakeFailure(ObjectPoolError::InvariantViolation, minted.GetError(),
                                                             Resource::ResourceDomainError::None, collected.GetError());
        if (released.Error != ObjectPoolError::None)
        {
            m_state = CapabilitySpacePoolState::Faulted;
            return ResultType::Failure(released);
        }
        return ResultType::Failure(MakeFailure(ObjectPoolError::MintRejected, minted.GetError()));
    }
    slot.State = SlotState::Active;
    ++m_created;
    return ResultType::Success(CapabilitySpaceCreation{
        .Space = slot.Identity,
        .Capability = minted.GetValue(),
    });
}

ObjectPoolFailure CapabilitySpacePool::Destroy(CapabilitySpace& space, const CapabilityHandle handle) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == CapabilitySpacePoolState::Faulted)
    {
        return MakeFailure(ObjectPoolError::Faulted);
    }
    const auto lease = space.Lookup(handle, ObjectType::CapabilitySpace, CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return MakeFailure(ObjectPoolError::CapabilityRejected, lease.GetError());
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) ||
            (static_cast<const IKernelObject*>(slot.Instance) != &lease.GetValue().GetObject()) ||
            (slot.Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
        {
            continue;
        }
        const ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        if (destroyed != ObjectRegistryError::Ok)
        {
            m_state = CapabilitySpacePoolState::Faulted;
            return MakeFailure(ObjectPoolError::InvariantViolation, CapabilityError::Ok,
                               Resource::ResourceDomainError::None, destroyed);
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
        return MakeFailure(ObjectPoolError::None);
    }
    return MakeFailure(ObjectPoolError::NotOwned);
}

ObjectPoolFailure CapabilitySpacePool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == CapabilitySpacePoolState::Faulted)
    {
        return MakeFailure(ObjectPoolError::Faulted);
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        const ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        if (destroyed != ObjectRegistryError::Ok)
        {
            m_state = CapabilitySpacePoolState::Faulted;
            return MakeFailure(ObjectPoolError::InvariantViolation, CapabilityError::Ok,
                               Resource::ResourceDomainError::None, destroyed);
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
    }
    return MakeFailure(ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, ObjectPoolFailure> CapabilitySpacePool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == CapabilitySpacePoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
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
        if (!collected.HasValue() && (collected.GetError() == ObjectRegistryError::NoDestructionReady))
        {
            continue;
        }
        // With every thread of its domain gone no prepared send can hold one
        // of the space's capabilities; one held now is a broken invariant.
        const ObjectPoolFailure released =
            (collected.HasValue() && (collected.GetValue().Instance == static_cast<IKernelObject*>(slot.Instance)) &&
             (slot.Space->GetHeldCount() == 0U))
                ? ReleaseSlotLocked(slot)
                : MakeFailure(ObjectPoolError::InvariantViolation, CapabilityError::Ok,
                              Resource::ResourceDomainError::None,
                              collected.HasValue() ? ObjectRegistryError::Ok : collected.GetError());
        if (released.Error != ObjectPoolError::None)
        {
            m_state = CapabilitySpacePoolState::Faulted;
            return ResultType::Failure(released);
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

ObjectId CapabilitySpacePool::FindIdentity(const CapabilitySpace& space) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (const Slot& slot : m_slots)
    {
        if ((slot.State == SlotState::Active) && (slot.Space == &space))
        {
            return slot.Identity;
        }
    }
    return ObjectId{};
}

CapabilitySpacePoolSnapshot CapabilitySpacePool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    CapabilitySpacePoolSnapshot snapshot{
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
            snapshot.FreeSlots += (m_state == CapabilitySpacePoolState::Ready) ? 1U : 0U;
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

ObjectPoolFailure CapabilitySpacePool::ReleaseSlotLocked(Slot& slot) noexcept
{
    const Resource::ResourceDomainError released = slot.Domain->Release(GetReservation());
    if (released != Resource::ResourceDomainError::None)
    {
        return MakeFailure(ObjectPoolError::InvariantViolation, CapabilityError::Ok, released);
    }
    slot.Instance->~CapabilitySpaceObject();
    slot.Space->~CapabilitySpace();
    slot.Instance = nullptr;
    slot.Space = nullptr;
    slot.Domain = nullptr;
    slot.DomainPin.Reset();
    slot.Identity = ObjectId{};
    slot.State = SlotState::Free;
    return MakeFailure(ObjectPoolError::None);
}
} // namespace Asteria::Kernel::Object
