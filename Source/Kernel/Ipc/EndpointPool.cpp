// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/EndpointPool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Ipc
{
namespace
{
// Construct the charge outside the publication frame's 512-byte stack budget.
[[nodiscard, gnu::noinline]] Resource::ResourceDomainError
ReserveEndpoint(Resource::ResourceDomain& domain, const EndpointConfiguration& configuration,
                Resource::ResourceReservation& charge) noexcept
{
    charge = EndpointPool::GetReservation(configuration);
    return domain.TryReserve(charge);
}
} // Namespace
Foundation::Result<EndpointCreation, EndpointPoolFailure>
EndpointPool::Create(const EndpointCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<EndpointCreation, EndpointPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == EndpointPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::Faulted));
    }
    if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry) ||
        ((request.Receiver != nullptr) &&
         (!request.Receiver->IsReady() || !request.Receiver->IsFromRegistry(m_objectRegistry) ||
          !request.ReceiverObject.IsValid())))
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::InvalidConfiguration));
    }
    Object::CapabilitySpace& space = *request.Space;

    const auto authority =
        space.Lookup(request.Authority, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::CapabilityRejected, authority.GetError()));
    }
    Slot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::CapacityExceeded));
    }

    // The lookup checked the type, and only ResourceDomain instances report ObjectType:
    // ResourceDomain. The pin keeps the domain registered while the slot holds its charge.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    if ((m_objectRegistry.PinObject(authority.GetValue().GetDescriptor().Object, Object::ObjectType::ResourceDomain,
                                    slot->DomainPin) != Object::ObjectPinError::Ok) ||
        (&slot->DomainPin.GetObject() != static_cast<Object::IKernelObject*>(&domain)))
    {
        slot->DomainPin.Reset();
        return ResultType::Failure(MakeFailure(EndpointPoolError::ResourceDomainRejected));
    }

    // A created receiver space stays registered while the Endpoint holds it
    // by reference.
    if ((request.Receiver != nullptr) &&
        (m_objectRegistry.PinObject(request.ReceiverObject, Object::ObjectType::CapabilitySpace, slot->ReceiverPin) !=
         Object::ObjectPinError::Ok))
    {
        slot->ReceiverPin.Reset();
        slot->DomainPin.Reset();
        return ResultType::Failure(
            MakeFailure(EndpointPoolError::CapabilityRejected, Object::CapabilityError::ObjectNotFound));
    }
    Endpoint* const endpoint = ::new (static_cast<void*>(slot->Storage)) Endpoint{
        (request.Receiver != nullptr) ? *request.Receiver : space, domain.GetIdentifier(), request.Configuration};
    // The readiness Notification is charged to the same domain and ready
    // before the Endpoint can publish anything.
    Notification* const readiness = ::new (static_cast<void*>(slot->ReadinessStorage)) Notification{};
    if (!endpoint->IsReady() || (readiness->Initialize(domain.GetIdentifier()) != NotificationError::None))
    {
        readiness->~Notification();
        endpoint->~Endpoint();
        slot->ReceiverPin.Reset();
        slot->DomainPin.Reset();
        return ResultType::Failure(MakeFailure(EndpointPoolError::InvalidRequest));
    }

    const Resource::ResourceDomainError charged = ReserveEndpoint(domain, request.Configuration, slot->Charge);
    if (charged != Resource::ResourceDomainError::None)
    {
        readiness->~Notification();
        endpoint->~Endpoint();
        slot->ReceiverPin.Reset();
        slot->DomainPin.Reset();
        return ResultType::Failure(
            MakeFailure(EndpointPoolError::ChargeRejected, Object::CapabilityError::Ok, charged));
    }
    slot->Instance = endpoint;
    slot->Readiness = readiness;
    slot->Domain = &domain;

    const auto registered = m_issuer.CreateObject(*endpoint);
    if (!registered.HasValue())
    {
        const EndpointPoolFailure released = ReleaseSlotLocked(*slot);
        if (released.Error != EndpointPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        return ResultType::Failure(MakeFailure(EndpointPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, registered.GetError()));
    }
    slot->Identity = registered.GetValue();
    const EndpointPoolFailure attached = RegisterReadinessLocked(*slot);
    if (attached.Error != EndpointPoolError::None)
    {
        return ResultType::Failure(attached);
    }

    const auto minted = m_issuer.MintUnderAuthority(
        space, slot->Identity, CreatedRights, request.Authority, Object::ObjectType::ResourceDomain,
        Object::CapabilityOperation::Manage, domain, request.SealedSelector);
    if (!minted.HasValue())
    {
        // No capability names the Endpoint or its Notification, so nothing
        // can hold a reference and both are collectable at once.
        Object::ObjectRegistryError unregistered = UnregisterLocked(slot->Identity);
        if (unregistered == Object::ObjectRegistryError::Ok)
        {
            unregistered = UnregisterLocked(slot->ReadinessIdentity);
        }
        if (unregistered != Object::ObjectRegistryError::Ok)
        {
            return ResultType::Failure(FaultLocked(MakeFailure(EndpointPoolError::InvariantViolation, minted.GetError(),
                                                               Resource::ResourceDomainError::None, unregistered)));
        }
        const EndpointPoolFailure released = ReleaseSlotLocked(*slot);
        if (released.Error != EndpointPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        return ResultType::Failure(MakeFailure(EndpointPoolError::MintRejected, minted.GetError()));
    }

    slot->State = SlotState::Active;
    ++m_created;
    return ResultType::Success(EndpointCreation{
        .Endpoint = slot->Identity,
        .Capability = minted.GetValue(),
    });
}

Foundation::Result<Object::ObjectId, EndpointPoolFailure>
EndpointPool::Destroy(Object::CapabilitySpace& space, const Object::CapabilityHandle endpoint) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, EndpointPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == EndpointPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::Faulted));
    }
    const auto lease = space.Lookup(endpoint, Object::ObjectType::Endpoint, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active) ||
        (slot->Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::NotOwned));
    }
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot->Identity);
    if (destroyed != Object::ObjectRegistryError::Ok)
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::DestroyRejected, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, destroyed));
    }
    // No capability names the Notification, so its destruction cannot be
    // refused once the Endpoint's was accepted.
    const Object::ObjectRegistryError readinessDestroyed = m_issuer.DestroyObject(slot->ReadinessIdentity);
    if (readinessDestroyed != Object::ObjectRegistryError::Ok)
    {
        return ResultType::Failure(
            FaultLocked(MakeFailure(EndpointPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                    Resource::ResourceDomainError::None, readinessDestroyed)));
    }
    slot->State = SlotState::Quiescing;
    m_quiescing = true;
    ++m_destroyed;
    return ResultType::Success(slot->Identity);
}

// An Endpoint goes with its readiness Notification, as Destroy takes both.
Object::ObjectPoolFailure EndpointPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == EndpointPoolState::Faulted)
    {
        return MakeFailure(EndpointPoolError::Faulted);
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        const Object::ObjectId registered[2U]{slot.Identity, slot.ReadinessIdentity};
        for (const Object::ObjectId identity : registered)
        {
            const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(identity);
            if (destroyed != Object::ObjectRegistryError::Ok)
            {
                return FaultLocked(MakeFailure(EndpointPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, destroyed));
            }
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
    }
    return MakeFailure(EndpointPoolError::None);
}

Foundation::Result<Foundation::Size, EndpointPoolFailure> EndpointPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, EndpointPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == EndpointPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(EndpointPoolError::Faulted));
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
        // The Endpoint goes once its last lease is gone; its Notification
        // once no WaitSet registration pins it either.
        EndpointPoolFailure collected =
            slot.EndpointCollected ? MakeFailure(EndpointPoolError::None)
                                   : CollectRegistrationLocked(slot.Identity, *slot.Instance, slot.EndpointCollected);
        if ((collected.Error == EndpointPoolError::None) && slot.EndpointCollected && !slot.ReadinessCollected)
        {
            collected = CollectRegistrationLocked(slot.ReadinessIdentity, *slot.Readiness, slot.ReadinessCollected);
        }
        if (collected.Error != EndpointPoolError::None)
        {
            return ResultType::Failure(FaultLocked(collected));
        }
        if (!slot.EndpointCollected || !slot.ReadinessCollected)
        {
            continue;
        }
        const EndpointPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != EndpointPoolError::None)
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

EndpointPoolSnapshot EndpointPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    EndpointPoolSnapshot snapshot{
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

EndpointPool::Slot* EndpointPool::FindFreeSlotLocked() noexcept
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

EndpointPool::Slot* EndpointPool::FindSlotLocked(const Object::IKernelObject& instance) noexcept
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

EndpointPoolFailure EndpointPool::CollectRegistrationLocked(const Object::ObjectId identity,
                                                            const Object::IKernelObject& instance,
                                                            bool& collected) noexcept
{
    const auto record = m_issuer.CollectDestroyedObject(identity);
    if (!record.HasValue() && (record.GetError() == Object::ObjectRegistryError::NoDestructionReady))
    {
        collected = false;
        return MakeFailure(EndpointPoolError::None);
    }
    if (!record.HasValue() || (record.GetValue().Instance != &instance))
    {
        return MakeFailure(EndpointPoolError::InvariantViolation, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None,
                           record.HasValue() ? Object::ObjectRegistryError::Ok : record.GetError());
    }
    collected = true;
    return MakeFailure(EndpointPoolError::None);
}

// The Endpoint is registered; its Notification is registered next and
// becomes its readiness, or both are undone.
EndpointPoolFailure EndpointPool::RegisterReadinessLocked(Slot& slot) noexcept
{
    const auto registered = m_issuer.CreateObject(*slot.Readiness);
    if (!registered.HasValue())
    {
        const Object::ObjectRegistryError unregistered = UnregisterLocked(slot.Identity);
        const EndpointPoolFailure released =
            (unregistered == Object::ObjectRegistryError::Ok)
                ? ReleaseSlotLocked(slot)
                : MakeFailure(EndpointPoolError::InvariantViolation, Object::CapabilityError::Ok,
                              Resource::ResourceDomainError::None, unregistered);
        if (released.Error != EndpointPoolError::None)
        {
            return FaultLocked(released);
        }
        return MakeFailure(EndpointPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None, registered.GetError());
    }
    slot.ReadinessIdentity = registered.GetValue();
    slot.Instance->AttachReadiness(*slot.Readiness, NotificationId{m_objectRegistry, slot.ReadinessIdentity});
    return MakeFailure(EndpointPoolError::None);
}

Object::ObjectRegistryError EndpointPool::UnregisterLocked(const Object::ObjectId identity) noexcept
{
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(identity);
    if (destroyed != Object::ObjectRegistryError::Ok)
    {
        return destroyed;
    }
    const auto collected = m_issuer.CollectDestroyedObject(identity);
    return collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError();
}

EndpointPoolFailure EndpointPool::ReleaseSlotLocked(Slot& slot) noexcept
{
    // A collected Endpoint has no reservation in progress, so the discard
    // succeeds; it closes the capabilities queued messages still carry.
    const auto discarded = slot.Instance->DiscardUnread();
    if (!discarded.HasValue())
    {
        return MakeFailure(EndpointPoolError::InvariantViolation, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok,
                           discarded.GetError().Code);
    }
    const Resource::ResourceDomainError released = slot.Domain->Release(slot.Charge);
    if (released != Resource::ResourceDomainError::None)
    {
        return MakeFailure(EndpointPoolError::InvariantViolation, Object::CapabilityError::Ok, released);
    }
    slot.Instance->~Endpoint();
    slot.Readiness->~Notification();
    slot.Instance = nullptr;
    slot.Readiness = nullptr;
    slot.Domain = nullptr;
    slot.DomainPin.Reset();
    slot.ReceiverPin.Reset();
    slot.Charge = Resource::ResourceReservation{};
    slot.Identity = Object::ObjectId{};
    slot.ReadinessIdentity = Object::ObjectId{};
    slot.EndpointCollected = false;
    slot.ReadinessCollected = false;
    slot.State = SlotState::Free;
    return MakeFailure(EndpointPoolError::None);
}

EndpointPoolFailure EndpointPool::FaultLocked(const EndpointPoolFailure failure) noexcept
{
    m_state = EndpointPoolState::Faulted;
    EndpointPoolFailure faulted = failure;
    faulted.Error = EndpointPoolError::InvariantViolation;
    return faulted;
}
} // Namespace Asteria::Kernel::Ipc
