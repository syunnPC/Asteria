// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Resource/ResourceDomainPool.hpp"
#include "Abi/SystemCallAbi.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Resource
{
bool ResourceDomainPool::Configure(ResourceDomainLedger& ledger, const Object::ResourceDomainId firstIdentity) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != ResourceDomainPoolState::Unconfigured) || !firstIdentity.IsValid())
    {
        return false;
    }
    m_ledger = &ledger;
    m_nextIdentity = firstIdentity.GetValue();
    m_state = ResourceDomainPoolState::Ready;
    return true;
}

Foundation::Result<ResourceDomainCreation, ResourceDomainPoolFailure>
ResourceDomainPool::Create(const ResourceDomainCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<ResourceDomainCreation, ResourceDomainPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ResourceDomainPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::Faulted));
    }
    if ((m_state != ResourceDomainPoolState::Ready) || (request.Space == nullptr) || !request.Space->IsReady() ||
        !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::InvalidConfiguration));
    }
    if ((request.Limits.KernelMetadataBytes != 0U) || (request.Limits.CapabilityHandles != 0U))
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::InvalidRequest));
    }
    const auto authority = request.Space->Lookup(request.Authority, Object::ObjectType::ResourceDomain,
                                                 Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::CapabilityRejected, authority.GetError()));
    }
    // Code belongs to the authorizing capability.
    if ((request.Flags & ~Abi::ObjectCreateDomainFlagCode) != 0U)
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::InvalidRequest));
    }
    if ((request.Flags & Abi::ObjectCreateDomainFlagCode) != 0U &&
        !authority.GetValue().GetDescriptor().Rights.ContainsAll(
            Object::CapabilityRights{Object::CapabilityRight::Code}))
    {
        return ResultType::Failure(
            MakeFailure(ResourceDomainPoolError::CapabilityRejected, Object::CapabilityError::RightsDenied));
    }
    Slot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::CapacityExceeded));
    }

    // The lookup checked the type, and only ResourceDomain instances report ObjectType:
    // ResourceDomain. The pin keeps the parent registered while the slot holds its charge.
    ResourceDomain& parent = static_cast<ResourceDomain&>(authority.GetValue().GetObject());
    const Object::ObjectId parentObject = authority.GetValue().GetDescriptor().Object;
    if ((m_objectRegistry.PinObject(parentObject, Object::ObjectType::ResourceDomain, slot->ParentPin) !=
         Object::ObjectPinError::Ok) ||
        (&slot->ParentPin.GetObject() != static_cast<Object::IKernelObject*>(&parent)))
    {
        slot->ParentPin.Reset();
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::ResourceDomainRejected));
    }
    const ResourceDomainError charged = parent.TryReserve(GetReservation());
    if (charged != ResourceDomainError::None)
    {
        slot->ParentPin.Reset();
        return ResultType::Failure(
            MakeFailure(ResourceDomainPoolError::ChargeRejected, Object::CapabilityError::Ok, charged));
    }
    slot->Parent = &parent;
    const ResourceDomainPoolFailure initialized = InitializeLocked(*slot, parent, parentObject, request);
    if (initialized.Error != ResourceDomainPoolError::None)
    {
        const ResourceDomainError released = parent.Release(GetReservation());
        slot->Parent = nullptr;
        slot->ParentPin.Reset();
        if (released != ResourceDomainError::None)
        {
            return ResultType::Failure(FaultLocked(
                MakeFailure(ResourceDomainPoolError::InvariantViolation, Object::CapabilityError::Ok, released)));
        }
        return ResultType::Failure(initialized);
    }
    return PublishLocked(*slot, request);
}

// The child's metadata limit is its parent's (a cap, not a guarantee:
// every ancestor's remaining quota bounds each reserve), and it takes the
// next identity, whether or not the rest of the creation succeeds, so no
// identity is ever handed out twice.
ResourceDomainPoolFailure ResourceDomainPool::InitializeLocked(Slot& slot, ResourceDomain& parent,
                                                               const Object::ObjectId parentObject,
                                                               const ResourceDomainCreateRequest& request) noexcept
{
    const auto inherited = parent.GetSnapshot().Limits;
    ResourceDomainLimits limits = request.Limits;
    limits.KernelMetadataBytes = inherited.KernelMetadataBytes;
    if (!request.ExplicitObjectLimits)
    {
        limits.Objects = inherited.Objects;
        limits.Objects.LargePageTableArenas = 0U;
    }
    ResourceDomain* const child = ::new (static_cast<void*>(slot.Storage)) ResourceDomain{*m_ledger};
    const ResourceDomainFailure initialized =
        child->InitializeChild(m_objectRegistry, Object::ResourceDomainId{m_nextIdentity},
                               ResourceDomainObjectId{m_objectRegistry, parentObject}, parent, limits);
    if (initialized.Error != ResourceDomainError::None)
    {
        child->~ResourceDomain();
        return MakeFailure(ResourceDomainPoolError::InvalidRequest, Object::CapabilityError::Ok, initialized.Error);
    }
    ++m_nextIdentity;
    slot.Instance = child;
    return MakeFailure(ResourceDomainPoolError::None);
}

Foundation::Result<ResourceDomainCreation, ResourceDomainPoolFailure>
ResourceDomainPool::PublishLocked(Slot& slot, const ResourceDomainCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<ResourceDomainCreation, ResourceDomainPoolFailure>;

    const auto registered = m_issuer.CreateObject(*slot.Instance);
    if (!registered.HasValue())
    {
        const ResourceDomainPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != ResourceDomainPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::RegistrationRejected,
                                               Object::CapabilityError::Ok, ResourceDomainError::None,
                                               registered.GetError()));
    }
    slot.Identity = registered.GetValue();
    const auto minted = m_issuer.MintUnderAuthority(
        *request.Space, slot.Identity,
        (request.Flags & Abi::ObjectCreateDomainFlagCode) != 0U ? CreatedRights.With(Object::CapabilityRight::Code)
                                                                : CreatedRights,
        request.Authority, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage, *slot.Parent);
    if (!minted.HasValue())
    {
        // No capability names the child, so nothing can hold a reference
        // and it is collectable at once.
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        const auto collected =
            (destroyed == Object::ObjectRegistryError::Ok)
                ? m_issuer.CollectDestroyedObject(slot.Identity)
                : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
        if (!collected.HasValue())
        {
            return ResultType::Failure(
                FaultLocked(MakeFailure(ResourceDomainPoolError::InvariantViolation, minted.GetError(),
                                        ResourceDomainError::None, collected.GetError())));
        }
        const ResourceDomainPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != ResourceDomainPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::MintRejected, minted.GetError()));
    }
    slot.State = SlotState::Active;
    ++m_created;
    return ResultType::Success(ResourceDomainCreation{
        .Domain = slot.Identity,
        .Identity = slot.Instance->GetIdentifier(),
        .Capability = minted.GetValue(),
    });
}

ResourceDomainPoolSnapshot ResourceDomainPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    ResourceDomainPoolSnapshot snapshot{
        .FreeSlots = 0U,
        .ActiveSlots = 0U,
        .QuiescingSlots = 0U,
        .Created = m_created,
        .Destroyed = m_destroyed,
        .Reclaimed = m_reclaimed,
        .NextIdentity = m_nextIdentity,
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

Foundation::Result<Object::ObjectId, ResourceDomainPoolFailure>
ResourceDomainPool::FindOwned(Object::CapabilitySpace& space, const Object::CapabilityHandle domain) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, ResourceDomainPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ResourceDomainPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::Faulted));
    }
    const auto lease = space.Lookup(domain, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::CapabilityRejected, lease.GetError()));
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State == SlotState::Active) &&
            (static_cast<const Object::IKernelObject*>(slot.Instance) == &lease.GetValue().GetObject()) &&
            (slot.Identity.GetValue() == lease.GetValue().GetDescriptor().Object.GetValue()))
        {
            return ResultType::Success(slot.Identity);
        }
    }
    return ResultType::Failure(MakeFailure(ResourceDomainPoolError::NotOwned));
}

bool ResourceDomainPool::IsInSubtree(const ResourceDomain* const candidate, const Object::ObjectId root) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return IsInSubtreeLocked(candidate, root);
}

// Walks candidate's parents through the slots: a domain this pool did not
// create (the grant, the boot root) ends the walk. Every candidate a pool
// asks about is pinned by that pool's slot, so its slot is not reused.
bool ResourceDomainPool::IsInSubtreeLocked(const ResourceDomain* candidate, const Object::ObjectId root) const noexcept
{
    for (Foundation::Size depth{0U}; (candidate != nullptr) && (depth <= MaximumResourceDomainDepth); ++depth)
    {
        const Slot* const slot = FindSlotLocked(candidate);
        if (slot == nullptr)
        {
            return false;
        }
        if (slot->Identity.GetValue() == root.GetValue())
        {
            return true;
        }
        candidate = slot->Parent;
    }
    return false;
}

ResourceDomainPoolFailure ResourceDomainPool::DestroySubtree(const Object::ObjectId root) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ResourceDomainPoolState::Faulted)
    {
        return MakeFailure(ResourceDomainPoolError::Faulted);
    }
    bool found{false};
    for (const Slot& slot : m_slots)
    {
        found = found || ((slot.State == SlotState::Active) && (slot.Identity.GetValue() == root.GetValue()));
    }
    if (!found)
    {
        return MakeFailure(ResourceDomainPoolError::NotOwned);
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) || !IsInSubtreeLocked(slot.Instance, root))
        {
            continue;
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            return FaultLocked(MakeFailure(ResourceDomainPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                           ResourceDomainError::None, destroyed));
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
    }
    return MakeFailure(ResourceDomainPoolError::None);
}

ResourceDomainPoolFailure ResourceDomainPool::BeginDrain(const Object::ObjectId root) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ResourceDomainPoolState::Faulted)
    {
        return MakeFailure(ResourceDomainPoolError::Faulted);
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State == SlotState::Active) && (slot.Identity.GetValue() == root.GetValue()))
        {
            const ResourceDomainError marked = slot.Instance->BeginDrain();
            return (marked == ResourceDomainError::None)
                       ? MakeFailure(ResourceDomainPoolError::None)
                       : FaultLocked(MakeFailure(ResourceDomainPoolError::InvariantViolation,
                                                 Object::CapabilityError::Ok, marked));
        }
    }
    return MakeFailure(ResourceDomainPoolError::NotOwned);
}

// A child pins its parent, so a parent's entry becomes collectable only
// after its children were reclaimed: passes repeat while one reclaims.
Foundation::Result<Foundation::Size, ResourceDomainPoolFailure> ResourceDomainPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, ResourceDomainPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ResourceDomainPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ResourceDomainPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    // No slot became Quiescing since a Collect left none: nothing to scan.
    if (!m_quiescing)
    {
        return ResultType::Success(0U);
    }
    for (bool progressed{true}; progressed;)
    {
        progressed = false;
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
                    ResourceDomainPoolError::InvariantViolation, Object::CapabilityError::Ok, ResourceDomainError::None,
                    collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError())));
            }
            const ResourceDomainPoolFailure released = ReleaseSlotLocked(slot);
            if (released.Error != ResourceDomainPoolError::None)
            {
                return ResultType::Failure(FaultLocked(released));
            }
            ++reclaimed;
            ++m_reclaimed;
            progressed = true;
        }
    }
    m_quiescing = false;
    for (const Slot& slot : m_slots)
    {
        m_quiescing = m_quiescing || (slot.State == SlotState::Quiescing);
    }
    return ResultType::Success(reclaimed);
}

const ResourceDomainPool::Slot* ResourceDomainPool::FindSlotLocked(const ResourceDomain* const instance) const noexcept
{
    for (const Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Free) && (slot.Instance == instance))
        {
            return &slot;
        }
    }
    return nullptr;
}

ResourceDomainPool::Slot* ResourceDomainPool::FindFreeSlotLocked() noexcept
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

// Quiesced is terminal, so the slot ends the child's lifetime and the next
// creation constructs a fresh one in its storage.
ResourceDomainPoolFailure ResourceDomainPool::ReleaseSlotLocked(Slot& slot) noexcept
{
    if (slot.Instance != nullptr)
    {
        const ResourceDomainError quiesced = slot.Instance->Quiesce();
        if (quiesced != ResourceDomainError::None)
        {
            return MakeFailure(ResourceDomainPoolError::InvariantViolation, Object::CapabilityError::Ok, quiesced);
        }
        slot.Instance->~ResourceDomain();
        slot.Instance = nullptr;
    }
    if (slot.Parent != nullptr)
    {
        const ResourceDomainError released = slot.Parent->Release(GetReservation());
        if (released != ResourceDomainError::None)
        {
            return MakeFailure(ResourceDomainPoolError::InvariantViolation, Object::CapabilityError::Ok, released);
        }
        slot.Parent = nullptr;
    }
    slot.ParentPin.Reset();
    slot.Identity = Object::ObjectId{};
    slot.State = SlotState::Free;
    return MakeFailure(ResourceDomainPoolError::None);
}

ResourceDomainPoolFailure ResourceDomainPool::FaultLocked(const ResourceDomainPoolFailure failure) noexcept
{
    m_state = ResourceDomainPoolState::Faulted;
    return failure;
}
} // Namespace Asteria::Kernel::Resource
