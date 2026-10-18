// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/IommuDomainPool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Device
{
namespace
{
using Object::ObjectPoolError;
using Object::ObjectPoolFailure;

[[nodiscard]] constexpr ObjectPoolFailure
MakeFailure(const ObjectPoolError error, const Object::CapabilityError capability = Object::CapabilityError::Ok,
            const Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
            const Object::ObjectRegistryError registry = Object::ObjectRegistryError::Ok,
            const Foundation::UInt8 detail = 0U) noexcept
{
    return Object::MakeObjectPoolFailure(error, capability, resource, registry, detail);
}

// Why IommuDomain::Initialize refused, as the pool reports it; Detail keeps the IommuDomainError.
[[nodiscard]] ObjectPoolFailure MapInitialization(const IommuDomainFailure& failure) noexcept
{
    const auto detail = static_cast<Foundation::UInt8>(failure.Error);
    switch (failure.Error)
    {
    case IommuDomainError::ResourceDomainRejected:
        return MakeFailure(ObjectPoolError::ChargeRejected, Object::CapabilityError::Ok, failure.Resource,
                           Object::ObjectRegistryError::Ok, detail);
    case IommuDomainError::ResourceDomainPinRejected:
    case IommuDomainError::ResourceDomainInstanceMismatch:
    case IommuDomainError::ResourceDomainRegistryMismatch:
        return MakeFailure(ObjectPoolError::ResourceDomainRejected, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok, detail);
    case IommuDomainError::BackendRejected:
        return MakeFailure((failure.Ownership == Hal::Device::IommuBackendOwnershipError::UnsupportedConfiguration)
                               ? ObjectPoolError::InvalidRequest
                               : ObjectPoolError::InvalidConfiguration,
                           Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                           Object::ObjectRegistryError::Ok, detail);
    default:
        break;
    }
    return MakeFailure(ObjectPoolError::InvalidRequest, Object::CapabilityError::Ok,
                       Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok, detail);
}
} // Namespace

bool IommuDomainPool::Configure(IommuDomainSlot (&slots)[Capacity],
                                Hal::Device::IIommuDomainBackend* const (&backends)[Capacity],
                                const IDmaReservations* const reservations) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_slots != nullptr)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        if ((backends[index] == nullptr) || (slots[index].Instance != nullptr))
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        slots[index].Backend = backends[index];
        slots[index].State = static_cast<Foundation::UInt8>(SlotState::Free);
    }
    m_slots = slots;
    m_reservations = reservations;
    return true;
}

Foundation::Result<IommuDomainCreation, ObjectPoolFailure>
IommuDomainPool::Create(const IommuDomainCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<IommuDomainCreation, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
    }
    if ((m_slots == nullptr) || (request.Space == nullptr) || !request.Space->IsReady() ||
        !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidConfiguration));
    }
    const auto authority = request.Space->Lookup(request.Authority, Object::ObjectType::ResourceDomain,
                                                 Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, authority.GetError()));
    }
    IommuDomainSlot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapacityExceeded));
    }

    // The lookup checked the type, and only ResourceDomain instances report
    // it. Initialize pins and charges the exact domain generation.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    IommuDomain* const instance =
        ::new (static_cast<void*>(slot->Storage)) IommuDomain{m_objectRegistry, *slot->Backend, m_reservations};
    const IommuDomainFailure initialized = instance->Initialize(
        Resource::ResourceDomainObjectId{m_objectRegistry, authority.GetValue().GetDescriptor().Object}, domain,
        IommuDomainConfiguration{.InputAddressBits = request.InputAddressBits, .Reserved = {}});
    if (initialized.Error != IommuDomainError::None)
    {
        instance->~IommuDomain();
        return ResultType::Failure(MapInitialization(initialized));
    }
    slot->Instance = instance;
    slot->Domain = &domain;

    const auto registered = m_issuer.CreateObject(*instance);
    if (!registered.HasValue())
    {
        if (instance->Quiesce().Error != IommuDomainError::None)
        {
            return ResultType::Failure(FaultLocked(MakeFailure(ObjectPoolError::InvariantViolation)));
        }
        FreeSlotLocked(*slot);
        return ResultType::Failure(MakeFailure(ObjectPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, registered.GetError()));
    }
    slot->Identity = registered.GetValue();
    return MintLocked(*slot, request);
}

Foundation::Result<IommuDomainCreation, ObjectPoolFailure>
IommuDomainPool::MintLocked(IommuDomainSlot& slot, const IommuDomainCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<IommuDomainCreation, ObjectPoolFailure>;

    const auto minted = m_issuer.MintUnderAuthority(*request.Space, slot.Identity, CreatedRights, request.Authority,
                                                    Object::ObjectType::ResourceDomain,
                                                    Object::CapabilityOperation::Manage, *slot.Domain);
    if (!minted.HasValue())
    {
        // No capability names the domain and nothing is attached to it, so
        // it is collectable at once.
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
        const auto collected =
            (destroyed == Object::ObjectRegistryError::Ok)
                ? m_issuer.CollectDestroyedObject(slot.Identity)
                : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
        if (!collected.HasValue() || (slot.Instance->Quiesce().Error != IommuDomainError::None))
        {
            return ResultType::Failure(FaultLocked(
                MakeFailure(ObjectPoolError::InvariantViolation, minted.GetError(), Resource::ResourceDomainError::None,
                            collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError())));
        }
        FreeSlotLocked(slot);
        return ResultType::Failure(MakeFailure(ObjectPoolError::MintRejected, minted.GetError()));
    }
    slot.State = static_cast<Foundation::UInt8>(SlotState::Active);
    ++m_created;
    return ResultType::Success(IommuDomainCreation{.Domain = slot.Identity, .Capability = minted.GetValue()});
}

Foundation::Result<Object::ObjectId, ObjectPoolFailure>
IommuDomainPool::Destroy(Object::CapabilitySpace& space, const Object::CapabilityHandle domain,
                         IIommuDomainQuiescer& quiescer) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
    }
    const auto lease = space.Lookup(domain, Object::ObjectType::IommuDomain, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, lease.GetError()));
    }
    IommuDomainSlot* const slot = (m_slots == nullptr) ? nullptr : FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != static_cast<Foundation::UInt8>(SlotState::Active)) ||
        (slot->Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::NotOwned));
    }
    const ObjectPoolFailure destroyed = DestroySlotLocked(*slot, quiescer);
    if (destroyed.Error != ObjectPoolError::None)
    {
        return ResultType::Failure(destroyed);
    }
    return ResultType::Success(slot->Identity);
}

ObjectPoolFailure IommuDomainPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree,
                                                  IIommuDomainQuiescer& quiescer) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return MakeFailure(ObjectPoolError::Faulted);
    }
    for (Foundation::Size index{0U}; (m_slots != nullptr) && (index < Capacity); ++index)
    {
        IommuDomainSlot& slot = m_slots[index];
        if ((slot.State != static_cast<Foundation::UInt8>(SlotState::Active)) || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        const ObjectPoolFailure destroyed = DestroySlotLocked(slot, quiescer, &subtree);
        if (destroyed.Error != ObjectPoolError::None)
        {
            return destroyed;
        }
    }
    return MakeFailure(ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, ObjectPoolFailure> IommuDomainPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    // No slot became Quiescing since a Collect left none: nothing to scan.
    if (!m_quiescing)
    {
        return ResultType::Success(0U);
    }
    for (Foundation::Size index{0U}; (m_slots != nullptr) && (index < Capacity); ++index)
    {
        IommuDomainSlot& slot = m_slots[index];
        if (slot.State != static_cast<Foundation::UInt8>(SlotState::Quiescing))
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
                ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError())));
        }
        FreeSlotLocked(slot);
        ++reclaimed;
        ++m_reclaimed;
    }
    m_quiescing = false;
    for (Foundation::Size index{0U}; (m_slots != nullptr) && (index < Capacity); ++index)
    {
        m_quiescing = m_quiescing || (m_slots[index].State == static_cast<Foundation::UInt8>(SlotState::Quiescing));
    }
    return ResultType::Success(reclaimed);
}

IommuDomainPoolSnapshot IommuDomainPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    IommuDomainPoolSnapshot snapshot{.Slots = (m_slots != nullptr) ? Capacity : 0U,
                                     .ActiveSlots = 0U,
                                     .QuiescingSlots = 0U,
                                     .Created = m_created,
                                     .Destroyed = m_destroyed,
                                     .Reclaimed = m_reclaimed,
                                     .Faulted = m_faulted,
                                     .Reserved = {}};
    for (Foundation::Size index{0U}; (m_slots != nullptr) && (index < Capacity); ++index)
    {
        const auto state = static_cast<SlotState>(m_slots[index].State);
        snapshot.ActiveSlots += (state == SlotState::Active) ? 1U : 0U;
        snapshot.QuiescingSlots += (state == SlotState::Quiescing) ? 1U : 0U;
    }
    return snapshot;
}

IommuDomainSlot* IommuDomainPool::FindFreeSlotLocked() noexcept
{
    for (Foundation::Size index{0U}; (m_slots != nullptr) && (index < Capacity); ++index)
    {
        if ((m_slots[index].State == static_cast<Foundation::UInt8>(SlotState::Free)) &&
            (m_slots[index].Instance == nullptr))
        {
            return &m_slots[index];
        }
    }
    return nullptr;
}

IommuDomainSlot* IommuDomainPool::FindSlotLocked(const Object::IKernelObject& instance) noexcept
{
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        if ((m_slots[index].Instance != nullptr) &&
            (static_cast<const Object::IKernelObject*>(m_slots[index].Instance) == &instance))
        {
            return &m_slots[index];
        }
    }
    return nullptr;
}

// The quiescer stops the domain's DMA and quiesces it, in a drain parking its pins; a domain that
// cannot be quiesced keeps its resources and stays live, so the call can be retried. A drain stops
// there (Faulted) before it releases any assignment, so a parker without room leaves the dead
// domain with its pins and attachments.
ObjectPoolFailure IommuDomainPool::DestroySlotLocked(IommuDomainSlot& slot, IIommuDomainQuiescer& quiescer,
                                                     const Resource::IResourceDomainSubtree* const drained) noexcept
{
    if (!((drained != nullptr) ? quiescer.QuiesceDrainedDomain(*slot.Instance, *drained)
                               : quiescer.QuiesceDomain(*slot.Instance)))
    {
        return MakeFailure(ObjectPoolError::DestroyRejected);
    }
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
    if (destroyed != Object::ObjectRegistryError::Ok)
    {
        return FaultLocked(MakeFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                       Resource::ResourceDomainError::None, destroyed));
    }
    slot.State = static_cast<Foundation::UInt8>(SlotState::Quiescing);
    m_quiescing = true;
    ++m_destroyed;
    return MakeFailure(ObjectPoolError::None);
}

void IommuDomainPool::FreeSlotLocked(IommuDomainSlot& slot) noexcept
{
    slot.Instance->~IommuDomain();
    slot.Instance = nullptr;
    slot.Domain = nullptr;
    slot.Identity = Object::ObjectId{};
    slot.State = static_cast<Foundation::UInt8>(SlotState::Free);
}

ObjectPoolFailure IommuDomainPool::FaultLocked(const ObjectPoolFailure failure) noexcept
{
    m_faulted = true;
    ObjectPoolFailure faulted = failure;
    faulted.Error = ObjectPoolError::InvariantViolation;
    return faulted;
}
} // Namespace Asteria::Kernel::Device
