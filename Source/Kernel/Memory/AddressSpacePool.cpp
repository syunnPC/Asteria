// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/AddressSpacePool.hpp"

namespace Asteria::Kernel::Memory
{
bool AddressSpacePool::Configure(IAddressSpaceProvider& provider) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const Foundation::Size slots = provider.GetSlotCount();
    if ((m_state != AddressSpacePoolState::Unconfigured) || (slots == 0U) || (slots > Capacity))
    {
        return false;
    }
    m_provider = &provider;
    m_slotCount = slots;
    m_state = AddressSpacePoolState::Ready;
    return true;
}

Foundation::Result<AddressSpaceCreation, Object::ObjectPoolFailure>
AddressSpacePool::Create(const AddressSpaceCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceCreation, Object::ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpacePoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::Faulted));
    }
    if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::InvalidConfiguration));
    }
    Object::CapabilitySpace& caller = *request.Space;
    const auto authority =
        caller.Lookup(request.Authority, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::CapabilityRejected, authority.GetError()));
    }
    Foundation::Size index{m_slotCount};
    for (Foundation::Size candidate{0U}; candidate < m_slotCount; ++candidate)
    {
        if ((index == m_slotCount) && (m_slots[candidate].State == SlotState::Free))
        {
            index = candidate;
        }
    }
    // An unconfigured pool has no slots: m_slotCount is zero.
    if (index == m_slotCount)
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::CapacityExceeded));
    }
    if (!m_provider->HasAvailableArena(request.LargePageTableArena))
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::CapacityExceeded));
    }
    Slot& slot = m_slots[index];

    // The lookup checked the type, and only ResourceDomain instances report ObjectType:
    // ResourceDomain. The pin keeps the domain registered while the slot holds its charge.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    if ((m_objectRegistry.PinObject(authority.GetValue().GetDescriptor().Object, Object::ObjectType::ResourceDomain,
                                    slot.DomainPin) != Object::ObjectPinError::Ok) ||
        (&slot.DomainPin.GetObject() != static_cast<Object::IKernelObject*>(&domain)))
    {
        slot.DomainPin.Reset();
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::ResourceDomainRejected));
    }
    const Resource::ResourceReservation charge{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = m_provider->GetSlotBytes(request.LargePageTableArena),
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .Objects = {0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U},
        .LargePageTableArenas = static_cast<Foundation::UInt16>(request.LargePageTableArena ? 1U : 0U),
    };
    const Resource::ResourceDomainError charged = domain.TryReserve(charge);
    if (charged != Resource::ResourceDomainError::None)
    {
        slot.DomainPin.Reset();
        return ResultType::Failure(
            MakeFailure(Object::ObjectPoolError::ChargeRejected, Object::CapabilityError::Ok, charged));
    }
    slot.Domain = &domain;
    slot.Charge = charge;
    slot.Instance = m_provider->InitializeSlot(index, domain.GetIdentifier(), request.LargePageTableArena);
    if (slot.Instance == nullptr)
    {
        // A root the provider cannot prepare will not be prepared later
        // either: the slot is spent.
        const Object::ObjectPoolFailure released = ReleaseSlotLocked(slot, SlotState::Spent);
        if (released.Error != Object::ObjectPoolError::None)
        {
            m_state = AddressSpacePoolState::Faulted;
            return ResultType::Failure(released);
        }
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::CapacityExceeded));
    }
    const auto published = PublishLocked(slot, caller, request.Authority);
    if (!published.HasValue() && (published.GetError().Error != Object::ObjectPoolError::Faulted) &&
        (published.GetError().Error != Object::ObjectPoolError::InvariantViolation))
    {
        // Nothing names the space any more: quiesce it and release the slot.
        const Object::ObjectPoolFailure retired = RetireSlotLocked(index);
        if (retired.Error != Object::ObjectPoolError::None)
        {
            m_state = AddressSpacePoolState::Faulted;
            return ResultType::Failure(retired);
        }
    }
    return published;
}

Foundation::Result<AddressSpaceCreation, Object::ObjectPoolFailure>
AddressSpacePool::PublishLocked(Slot& slot, Object::CapabilitySpace& caller,
                                const Object::CapabilityHandle authority) noexcept
{
    using ResultType = Foundation::Result<AddressSpaceCreation, Object::ObjectPoolFailure>;

    const auto registered = m_issuer.CreateObject(*slot.Instance);
    if (!registered.HasValue())
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::RegistrationRejected,
                                               Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                                               registered.GetError()));
    }
    slot.Identity = registered.GetValue();
    const AddressSpaceId identity{m_objectRegistry, slot.Identity};
    const auto minted = (slot.Instance->BindIdentity(m_objectRegistry, identity).Error == AddressSpaceError::None)
                            ? m_issuer.MintUnderAuthority(caller, slot.Identity, CreatedRights, authority,
                                                          Object::ObjectType::ResourceDomain,
                                                          Object::CapabilityOperation::Manage, *slot.Domain)
                            : Foundation::Result<Object::CapabilityHandle, Object::CapabilityError>::Failure(
                                  Object::CapabilityError::InvalidConfiguration);
    if (!minted.HasValue())
    {
        // No capability names the space, so its registration is collectable
        // at once; Create then retires the slot.
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        const auto collected =
            (destroyed == Object::ObjectRegistryError::Ok)
                ? m_issuer.CollectDestroyedObject(slot.Identity)
                : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
        if (!collected.HasValue())
        {
            m_state = AddressSpacePoolState::Faulted;
            return ResultType::Failure(MakeFailure(Object::ObjectPoolError::InvariantViolation, minted.GetError(),
                                                   Resource::ResourceDomainError::None, collected.GetError()));
        }
        slot.Identity = Object::ObjectId{};
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::MintRejected, minted.GetError()));
    }
    slot.State = SlotState::Active;
    ++m_created;
    return ResultType::Success(AddressSpaceCreation{
        .Space = identity,
        .Capability = minted.GetValue(),
    });
}

Object::ObjectPoolFailure AddressSpacePool::Destroy(Object::CapabilitySpace& space,
                                                    const Object::CapabilityHandle handle) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpacePoolState::Faulted)
    {
        return MakeFailure(Object::ObjectPoolError::Faulted);
    }
    const auto lease = space.Lookup(handle, Object::ObjectType::AddressSpace, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return MakeFailure(Object::ObjectPoolError::CapabilityRejected, lease.GetError());
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) ||
            (static_cast<const Object::IKernelObject*>(slot.Instance) != &lease.GetValue().GetObject()) ||
            (slot.Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
        {
            continue;
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            m_state = AddressSpacePoolState::Faulted;
            return MakeFailure(Object::ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                               Resource::ResourceDomainError::None, destroyed);
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
        return MakeFailure(Object::ObjectPoolError::None);
    }
    return MakeFailure(Object::ObjectPoolError::NotOwned);
}

// A drained space may keep mappings: its Quiesce releases their pins.
Object::ObjectPoolFailure AddressSpacePool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpacePoolState::Faulted)
    {
        return MakeFailure(Object::ObjectPoolError::Faulted);
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
            m_state = AddressSpacePoolState::Faulted;
            return MakeFailure(Object::ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                               Resource::ResourceDomainError::None, destroyed);
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
    }
    return MakeFailure(Object::ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> AddressSpacePool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, Object::ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == AddressSpacePoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    // No slot became Quiescing since a Collect left none: nothing to scan.
    if (!m_quiescing)
    {
        return ResultType::Success(0U);
    }
    for (Foundation::Size index{0U}; index < m_slotCount; ++index)
    {
        Slot& slot = m_slots[index];
        // A space some processor still runs is retried later; once none
        // does, its registration and then the space itself are ended, so
        // Quiesce, whose only refusal left is residency, cannot refuse.
        if ((slot.State != SlotState::Quiescing) || (slot.Instance->GetSnapshot().ResidentProcessorMask != 0U))
        {
            continue;
        }
        const auto collected = m_issuer.CollectDestroyedObject(slot.Identity);
        if (!collected.HasValue() && (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady))
        {
            continue;
        }
        const Object::ObjectPoolFailure retired =
            (collected.HasValue() &&
             (collected.GetValue().Instance == static_cast<Object::IKernelObject*>(slot.Instance)))
                ? RetireSlotLocked(index)
                : MakeFailure(Object::ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                              Resource::ResourceDomainError::None,
                              collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError());
        if (retired.Error != Object::ObjectPoolError::None)
        {
            m_state = AddressSpacePoolState::Faulted;
            return ResultType::Failure(retired);
        }
        ++reclaimed;
        ++m_reclaimed;
    }
    m_quiescing = false;
    for (Foundation::Size index{0U}; index < m_slotCount; ++index)
    {
        m_quiescing = m_quiescing || (m_slots[index].State == SlotState::Quiescing);
    }
    return ResultType::Success(reclaimed);
}

AddressSpacePoolSnapshot AddressSpacePool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    AddressSpacePoolSnapshot snapshot{
        .FreeSlots = 0U,
        .ActiveSlots = 0U,
        .QuiescingSlots = 0U,
        .SpentSlots = 0U,
        .Created = m_created,
        .Destroyed = m_destroyed,
        .Reclaimed = m_reclaimed,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
    for (Foundation::Size index{0U}; index < m_slotCount; ++index)
    {
        switch (m_slots[index].State)
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
        case SlotState::Spent:
            ++snapshot.SpentSlots;
            break;
        }
    }
    return snapshot;
}

Object::ObjectPoolFailure AddressSpacePool::ReleaseSlotLocked(Slot& slot, const SlotState state) noexcept
{
    const Resource::ResourceDomainError released = slot.Domain->Release(slot.Charge);
    if (released != Resource::ResourceDomainError::None)
    {
        return MakeFailure(Object::ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok, released);
    }
    slot.Domain = nullptr;
    slot.DomainPin.Reset();
    slot.Charge = Resource::ResourceReservation{};
    slot.State = state;
    return MakeFailure(Object::ObjectPoolError::None);
}

Object::ObjectPoolFailure AddressSpacePool::RetireSlotLocked(const Foundation::Size index) noexcept
{
    Slot& slot = m_slots[index];
    // Quiesce releases the pins and reservations of the mappings left in the
    // space; no processor holds its root, so none needs a shootdown.
    if (slot.Instance->Quiesce() != AddressSpaceError::None)
    {
        return MakeFailure(Object::ObjectPoolError::InvariantViolation);
    }
    slot.Instance = nullptr;
    slot.Identity = Object::ObjectId{};
    return ReleaseSlotLocked(slot, m_provider->ReleaseSlot(index) ? SlotState::Free : SlotState::Spent);
}
} // Namespace Asteria::Kernel::Memory
