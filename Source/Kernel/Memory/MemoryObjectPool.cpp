// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/MemoryObjectPool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Memory
{
namespace
{
using Object::MakeObjectPoolFailure;
using Object::ObjectPoolError;
using Object::ObjectPoolFailure;
} // Namespace

bool MemoryObjectPool::AttachSlab(void* const slab) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_slots != nullptr) || (slab == nullptr) ||
        ((reinterpret_cast<Foundation::UIntPointer>(slab) % SlabAlignment) != 0U))
    {
        return false;
    }
    auto* const slots = static_cast<Slot*>(slab);
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        ::new (static_cast<void*>(&slots[index])) Slot{};
        m_sets.AddFree(index);
    }
    m_slots = slots;
    return true;
}

Foundation::Result<MemoryObjectCreation, ObjectPoolFailure>
MemoryObjectPool::Create(const MemoryObjectCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<MemoryObjectCreation, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == MemoryObjectPoolState::Faulted)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::InvalidConfiguration));
    }
    if ((request.Pages.GetValue() == 0U) || (request.Pages.GetValue() > MaximumPages) ||
        ((request.CachePolicy != MemoryCachePolicy::WriteBack) &&
         (request.CachePolicy != MemoryCachePolicy::Uncached) &&
         (request.CachePolicy != MemoryCachePolicy::WriteCombining)))
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::InvalidRequest));
    }
    Object::CapabilitySpace& space = *request.Space;

    const auto authority =
        space.Lookup(request.Authority, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, authority.GetError()));
    }
    Slot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::CapacityExceeded));
    }

    // The lookup checked the type, and only ResourceDomain instances report ObjectType:
    // ResourceDomain. The MemoryObject pins and charges the domain itself.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    MemoryObject* const memory = ::new (static_cast<void*>(slot->Storage)) MemoryObject{};
    const MemoryObjectFailure initialized = memory->Initialize(
        m_objectRegistry,
        Resource::ResourceDomainObjectId{m_objectRegistry, authority.GetValue().GetDescriptor().Object}, domain,
        MemoryObjectDescriptor{
            .Pages = request.Pages,
            .CachePolicy = request.CachePolicy,
            .MaximumPermissions = request.MaximumPermissions,
            .Commitment = MemoryCommitment::Pinned,
            .Reserved = {},
        },
        // The Frame that backs a mapping carries the pages.
        MemoryObjectCharge::MetadataOnly);
    if (initialized.Error != MemoryObjectError::None)
    {
        memory->~MemoryObject();
        if (initialized.Error == MemoryObjectError::ResourceDomainRejected)
        {
            return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::ChargeRejected,
                                                             Object::CapabilityError::Ok, initialized.Resource));
        }
        return ResultType::Failure(MakeObjectPoolFailure(
            (initialized.Error == MemoryObjectError::InvalidDescriptor) ? ObjectPoolError::InvalidRequest
                                                                        : ObjectPoolError::ResourceDomainRejected,
            Object::CapabilityError::Ok, Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok,
            static_cast<Foundation::UInt8>(initialized.Error)));
    }
    slot->Instance = memory;
    slot->Domain = &domain;

    const auto registered = m_issuer.CreateObject(*memory);
    if (!registered.HasValue())
    {
        const ObjectPoolFailure released = ReleaseSlotLocked(*slot);
        return ResultType::Failure(
            released.Error != ObjectPoolError::None
                ? FaultLocked(released)
                : MakeObjectPoolFailure(ObjectPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                        Resource::ResourceDomainError::None, registered.GetError()));
    }
    slot->Identity = registered.GetValue();

    const auto minted =
        m_issuer.MintUnderAuthority(space, slot->Identity, CreatedRights, request.Authority,
                                    Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage, domain);
    if (!minted.HasValue())
    {
        // No capability names the MemoryObject, so nothing can hold a
        // reference and it is collectable at once.
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot->Identity);
        const auto collected =
            (destroyed == Object::ObjectRegistryError::Ok)
                ? m_issuer.CollectDestroyedObject(slot->Identity)
                : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
        if (!collected.HasValue())
        {
            return ResultType::Failure(
                FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, minted.GetError(),
                                                  Resource::ResourceDomainError::None, collected.GetError())));
        }
        const ObjectPoolFailure released = ReleaseSlotLocked(*slot);
        return ResultType::Failure(released.Error != ObjectPoolError::None
                                       ? FaultLocked(released)
                                       : MakeObjectPoolFailure(ObjectPoolError::MintRejected, minted.GetError()));
    }

    SetStateLocked(*slot, SlotState::Active);
    ++m_created;
    return ResultType::Success(MemoryObjectCreation{
        .MemoryObject = slot->Identity,
        .Capability = minted.GetValue(),
    });
}

Foundation::Result<Object::ObjectId, ObjectPoolFailure>
MemoryObjectPool::Destroy(Object::CapabilitySpace& space, const Object::CapabilityHandle memoryObject) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == MemoryObjectPoolState::Faulted)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    const auto lease =
        space.Lookup(memoryObject, Object::ObjectType::MemoryObject, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active) ||
        (slot->Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::NotOwned));
    }
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot->Identity);
    if (destroyed != Object::ObjectRegistryError::Ok)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::DestroyRejected, Object::CapabilityError::Ok,
                                                         Resource::ResourceDomainError::None, destroyed));
    }
    SetStateLocked(*slot, SlotState::Quiescing);
    ++m_destroyed;
    return ResultType::Success(slot->Identity);
}

ObjectPoolFailure MemoryObjectPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == MemoryObjectPoolState::Faulted)
    {
        return MakeObjectPoolFailure(ObjectPoolError::Faulted);
    }
    // Only the live slots: each step finds the next one in the bitmap.
    for (Foundation::Size index = m_sets.FindFirst(SlotState::Active); index < Capacity;
         index = m_sets.FindFirst(SlotState::Active, index + 1U))
    {
        Slot& slot = m_slots[index];
        if (!subtree.Contains(slot.Domain))
        {
            continue;
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            return FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                                     Resource::ResourceDomainError::None, destroyed));
        }
        SetStateLocked(slot, SlotState::Quiescing);
        ++m_destroyed;
    }
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, ObjectPoolFailure> MemoryObjectPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == MemoryObjectPoolState::Faulted)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    // Only the quiescing slots: each step finds the next one in the bitmap, so a Collect with
    // nothing to reclaim reads only its summary.
    for (Foundation::Size index = m_sets.FindFirst(SlotState::Quiescing); index < Capacity;
         index = m_sets.FindFirst(SlotState::Quiescing, index + 1U))
    {
        Slot& slot = m_slots[index];
        const auto collected = m_issuer.CollectDestroyedObject(slot.Identity);
        if (!collected.HasValue() && (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady))
        {
            continue;
        }
        if (!collected.HasValue() ||
            (collected.GetValue().Instance != static_cast<Object::IKernelObject*>(slot.Instance)))
        {
            return ResultType::Failure(FaultLocked(MakeObjectPoolFailure(
                ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError())));
        }
        const ObjectPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != ObjectPoolError::None)
        {
            return ResultType::Failure(FaultLocked(released));
        }
        ++reclaimed;
        ++m_reclaimed;
    }
    return ResultType::Success(reclaimed);
}

MemoryObjectPoolSnapshot MemoryObjectPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return MemoryObjectPoolSnapshot{
        .FreeSlots = m_sets.GetFreeCount(),
        .ActiveSlots = m_sets.GetActiveCount(),
        .QuiescingSlots = m_sets.GetQuiescingCount(),
        .Created = m_created,
        .Destroyed = m_destroyed,
        .Reclaimed = m_reclaimed,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

MemoryObjectPool::Slot* MemoryObjectPool::FindFreeSlotLocked() noexcept
{
    const Foundation::Size index = m_sets.FindFirst(SlotState::Free);
    return (index < Capacity) ? &m_slots[index] : nullptr;
}

MemoryObjectPool::Slot* MemoryObjectPool::FindSlotLocked(const Object::IKernelObject& instance) const noexcept
{
    // A MemoryObject of the slab lies in its slot's storage, so its address
    // names the slot; the slot must then hold exactly that instance.
    const auto address = reinterpret_cast<Foundation::UIntPointer>(&instance);
    const auto base = reinterpret_cast<Foundation::UIntPointer>(m_slots);
    if ((m_slots == nullptr) || (address < base) || ((address - base) >= SlabBytes))
    {
        return nullptr;
    }
    Slot& slot = m_slots[(address - base) / sizeof(Slot)];
    return ((slot.Instance != nullptr) && (static_cast<const Object::IKernelObject*>(slot.Instance) == &instance))
               ? &slot
               : nullptr;
}

void MemoryObjectPool::SetStateLocked(Slot& slot, const SlotState state) noexcept
{
    m_sets.Move(static_cast<Foundation::Size>(&slot - m_slots), slot.State, state);
    slot.State = state;
}

ObjectPoolFailure MemoryObjectPool::ReleaseSlotLocked(Slot& slot) noexcept
{
    // A collected MemoryObject retains no mapping: every mapping that
    // retained it also pinned it.
    const MemoryObjectFailure quiesced = slot.Instance->Quiesce();
    if (quiesced.Error != MemoryObjectError::None)
    {
        return MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                     quiesced.Resource, Object::ObjectRegistryError::Ok,
                                     static_cast<Foundation::UInt8>(quiesced.Error));
    }
    slot.Instance->~MemoryObject();
    slot.Instance = nullptr;
    slot.Domain = nullptr;
    slot.Identity = Object::ObjectId{};
    SetStateLocked(slot, SlotState::Free);
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

ObjectPoolFailure MemoryObjectPool::FaultLocked(const ObjectPoolFailure failure) noexcept
{
    m_state = MemoryObjectPoolState::Faulted;
    ObjectPoolFailure faulted = failure;
    faulted.Error = ObjectPoolError::InvariantViolation;
    return faulted;
}
} // Namespace Asteria::Kernel::Memory
