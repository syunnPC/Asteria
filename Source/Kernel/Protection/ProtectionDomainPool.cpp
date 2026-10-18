// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Protection/ProtectionDomainPool.hpp"

#include "Kernel/Object/CapabilitySpacePool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Protection
{
bool ProtectionDomainPool::Configure(ICreatedDomainRecords& records) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != ProtectionDomainPoolState::Unconfigured)
    {
        return false;
    }
    m_records = &records;
    m_state = ProtectionDomainPoolState::Ready;
    return true;
}

// The leases end when this returns; the objects stay registered because the
// domain pins its AddressSpace and ResourceDomain and the slot its
// CapabilitySpace before Create releases the lock.
Object::ObjectPoolFailure ProtectionDomainPool::ResolveLocked(const ProtectionDomainCreateRequest& request,
                                                              Resolved& resolved) noexcept
{
    Object::CapabilitySpace& caller = *request.Space;
    const auto authority =
        caller.Lookup(request.Authority, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return MakeFailure(Object::ObjectPoolError::CapabilityRejected, authority.GetError());
    }
    const auto space =
        caller.Lookup(request.AddressSpace, Object::ObjectType::AddressSpace, Object::CapabilityOperation::Manage);
    if (!space.HasValue())
    {
        return MakeFailure(Object::ObjectPoolError::CapabilityRejected, space.GetError());
    }
    const auto capabilities =
        caller.Lookup(request.Capabilities, Object::ObjectType::CapabilitySpace, Object::CapabilityOperation::Manage);
    if (!capabilities.HasValue())
    {
        return MakeFailure(Object::ObjectPoolError::CapabilityRejected, capabilities.GetError());
    }
    // Each lookup checked the type, and only these classes report these
    // types.
    resolved.Domain = &static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    resolved.DomainIdentity =
        Resource::ResourceDomainObjectId{m_objectRegistry, authority.GetValue().GetDescriptor().Object};
    resolved.AddressSpace = &static_cast<Memory::AddressSpace&>(space.GetValue().GetObject());
    resolved.AddressSpaceIdentity = Memory::AddressSpaceId{m_objectRegistry, space.GetValue().GetDescriptor().Object};
    resolved.CapabilitiesObject = capabilities.GetValue().GetDescriptor().Object;
    resolved.Capabilities =
        &static_cast<Object::CapabilitySpaceObject&>(capabilities.GetValue().GetObject()).GetSpace();
    // Each lookup lease lives until these temporary pins retain the exact generation, then
    // initialization takes its pins.
    if (m_objectRegistry.PinObject(resolved.DomainIdentity.GetObjectId(), Object::ObjectType::ResourceDomain,
                                   resolved.DomainPin) != Object::ObjectPinError::Ok ||
        m_objectRegistry.PinObject(resolved.AddressSpaceIdentity.GetObjectId(), Object::ObjectType::AddressSpace,
                                   resolved.AddressSpacePin) != Object::ObjectPinError::Ok ||
        m_objectRegistry.PinObject(resolved.CapabilitiesObject, Object::ObjectType::CapabilitySpace,
                                   resolved.CapabilitiesPin) != Object::ObjectPinError::Ok)
    {
        return MakeFailure(Object::ObjectPoolError::CapabilityRejected, Object::CapabilityError::ObjectNotFound);
    }
    if (request.Channel.IsValid())
    {
        const auto channel =
            caller.Lookup(request.Channel, Object::ObjectType::FaultChannel, Object::CapabilityOperation::Manage);
        if (!channel.HasValue())
        {
            return MakeFailure(Object::ObjectPoolError::CapabilityRejected, channel.GetError());
        }
        resolved.Channel = &static_cast<Memory::FaultChannel&>(channel.GetValue().GetObject());
        if (!resolved.Domain->IsDescendantOf(resolved.Channel->GetDomain()))
        {
            return MakeFailure(Object::ObjectPoolError::InvalidRequest);
        }
        if (m_objectRegistry.PinObject(channel.GetValue().GetDescriptor().Object, Object::ObjectType::FaultChannel,
                                       resolved.ChannelPin) != Object::ObjectPinError::Ok)
        {
            return MakeFailure(Object::ObjectPoolError::CapabilityRejected, Object::CapabilityError::ObjectNotFound);
        }
    }
    return MakeFailure(Object::ObjectPoolError::None);
}

Foundation::Result<ProtectionDomainCreation, Object::ObjectPoolFailure>
ProtectionDomainPool::Create(const ProtectionDomainCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<ProtectionDomainCreation, Object::ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::Faulted));
    }
    if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::InvalidConfiguration));
    }
    if (request.SupervisionTag == 0U)
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::InvalidRequest));
    }
    Resolved resolved{};
    const Object::ObjectPoolFailure resolution = ResolveLocked(request, resolved);
    if (resolution.Error != Object::ObjectPoolError::None)
    {
        return ResultType::Failure(resolution);
    }
    for (const Slot& live : m_slots)
    {
        if (live.State != SlotState::Free && live.Tag == request.SupervisionTag && live.Channel == resolved.Channel)
        {
            ++m_tagRefusals;
            return ResultType::Failure(MakeFailure(Object::ObjectPoolError::InvalidRequest));
        }
    }
    Slot* slot{nullptr};
    for (Slot& candidate : m_slots)
    {
        if ((slot == nullptr) && (candidate.State == SlotState::Free))
        {
            slot = &candidate;
        }
    }
    // An unconfigured pool has no records for a domain.
    if ((m_state != ProtectionDomainPoolState::Ready) || (slot == nullptr))
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::CapacityExceeded));
    }
    if ((m_objectRegistry.PinObject(resolved.CapabilitiesObject, Object::ObjectType::CapabilitySpace,
                                    slot->CapabilitiesPin) != Object::ObjectPinError::Ok))
    {
        // The space was destroyed after the lookup.
        slot->CapabilitiesPin.Reset();
        return ResultType::Failure(
            MakeFailure(Object::ObjectPoolError::CapabilityRejected, Object::CapabilityError::ObjectNotFound));
    }
    if (resolved.Channel != nullptr)
    {
        if (m_objectRegistry.PinObject(resolved.ChannelPin.GetObjectId(), Object::ObjectType::FaultChannel,
                                       slot->ChannelPin) != Object::ObjectPinError::Ok ||
            !resolved.Channel->BindDomain())
        {
            slot->ChannelPin.Reset();
            slot->CapabilitiesPin.Reset();
            return ResultType::Failure(MakeFailure(Object::ObjectPoolError::InvalidRequest));
        }
        slot->Channel = resolved.Channel;
    }
    slot->Tag = request.SupervisionTag;
    ProtectionDomain* const domain = ::new (static_cast<void*>(slot->Storage)) ProtectionDomain{};
    const ProtectionDomainFailure initialized =
        domain->Initialize(m_objectRegistry, resolved.DomainIdentity, *resolved.Domain, resolved.AddressSpaceIdentity,
                           *resolved.Capabilities);
    if (initialized.Error != ProtectionDomainError::None)
    {
        domain->~ProtectionDomain();
        if (slot->Channel != nullptr)
        {
            slot->Channel->ReleaseDomain();
            slot->Channel = nullptr;
        }
        slot->ChannelPin.Reset();
        slot->Tag = 0U;
        slot->CapabilitiesPin.Reset();
        return ResultType::Failure(MakeFailure((initialized.Error == ProtectionDomainError::ResourceDomainRejected)
                                                   ? Object::ObjectPoolError::ChargeRejected
                                                   : Object::ObjectPoolError::InvalidRequest,
                                               Object::CapabilityError::Ok, initialized.Resource));
    }
    slot->Instance = domain;
    slot->Domain = resolved.Domain;
    return PublishLocked(*slot, request, resolved);
}

Foundation::Result<ProtectionDomainCreation, Object::ObjectPoolFailure>
ProtectionDomainPool::PublishLocked(Slot& slot, const ProtectionDomainCreateRequest& request,
                                    const Resolved& resolved) noexcept
{
    using ResultType = Foundation::Result<ProtectionDomainCreation, Object::ObjectPoolFailure>;

    slot.Instance->SetDiagnosticComponentKind(request.DiagnosticComponentKind);
    const auto registered = m_issuer.CreateObject(*slot.Instance);
    if (!registered.HasValue())
    {
        const Object::ObjectPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != Object::ObjectPoolError::None)
        {
            m_state = ProtectionDomainPoolState::Faulted;
            return ResultType::Failure(released);
        }
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::RegistrationRejected,
                                               Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                                               registered.GetError()));
    }
    slot.Identity = ProtectionDomainId{m_objectRegistry, registered.GetValue()};
    const bool bound =
        slot.Instance->BindIdentity(m_objectRegistry, slot.Identity).Error == ProtectionDomainError::None;
    const bool recorded = bound && m_records->RecordDomain(CreatedDomain{
                                       .Protection = slot.Instance,
                                       .Identity = slot.Identity,
                                       .Capabilities = resolved.Capabilities,
                                       .AddressSpace = resolved.AddressSpace,
                                       .AddressSpaceIdentity = resolved.AddressSpaceIdentity,
                                       .SupervisionTag = request.SupervisionTag,
                                       .Channel = resolved.Channel,
                                   });
    const auto minted = recorded ? m_issuer.MintUnderAuthority(*request.Space, registered.GetValue(), CreatedRights,
                                                               request.Authority, Object::ObjectType::ResourceDomain,
                                                               Object::CapabilityOperation::Manage, *slot.Domain)
                                 : Foundation::Result<Object::CapabilityHandle, Object::CapabilityError>::Failure(
                                       Object::CapabilityError::SpaceExhausted);
    if (!minted.HasValue())
    {
        // No thread can be bound yet, so a recorded domain retires at once.
        const bool retired = !recorded || m_records->RetireDomain(*slot.Instance);
        const Object::ObjectPoolFailure withdrawn =
            retired ? WithdrawLocked(slot) : MakeFailure(Object::ObjectPoolError::InvariantViolation);
        if (withdrawn.Error != Object::ObjectPoolError::None)
        {
            m_state = ProtectionDomainPoolState::Faulted;
            return ResultType::Failure(withdrawn);
        }
        return ResultType::Failure(recorded ? MakeFailure(Object::ObjectPoolError::MintRejected, minted.GetError())
                                            : MakeFailure(bound ? Object::ObjectPoolError::CapacityExceeded
                                                                : Object::ObjectPoolError::RegistrationRejected));
    }
    slot.State = SlotState::Active;
    ++m_created;
    return ResultType::Success(ProtectionDomainCreation{
        .Domain = slot.Identity,
        .Capability = minted.GetValue(),
    });
}

Object::ObjectPoolFailure ProtectionDomainPool::Destroy(Object::CapabilitySpace& space,
                                                        const Object::CapabilityHandle domain) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainPoolState::Faulted)
    {
        return MakeFailure(Object::ObjectPoolError::Faulted);
    }
    const auto lease = space.Lookup(domain, Object::ObjectType::ProtectionDomain, Object::CapabilityOperation::Manage);
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
        if (!m_records->RetireDomain(*slot.Instance))
        {
            return MakeFailure(Object::ObjectPoolError::Busy);
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity.GetObjectId());
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            m_state = ProtectionDomainPoolState::Faulted;
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

// A domain whose record the platform cannot retire yet (a thread still
// bound) stays: Busy, and a later drain takes it.
Object::ObjectPoolFailure ProtectionDomainPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainPoolState::Faulted)
    {
        return MakeFailure(Object::ObjectPoolError::Faulted);
    }
    bool busy{false};
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        if (!m_records->RetireDomain(*slot.Instance))
        {
            busy = true;
            continue;
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity.GetObjectId());
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            m_state = ProtectionDomainPoolState::Faulted;
            return MakeFailure(Object::ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                               Resource::ResourceDomainError::None, destroyed);
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
    }
    return MakeFailure(busy ? Object::ObjectPoolError::Busy : Object::ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> ProtectionDomainPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, Object::ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainPoolState::Faulted)
    {
        return ResultType::Failure(MakeFailure(Object::ObjectPoolError::Faulted));
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
        const auto collected = m_issuer.CollectDestroyedObject(slot.Identity.GetObjectId());
        if (!collected.HasValue() && (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady))
        {
            continue;
        }
        const Object::ObjectPoolFailure released =
            (collected.HasValue() &&
             (collected.GetValue().Instance == static_cast<Object::IKernelObject*>(slot.Instance)))
                ? ReleaseSlotLocked(slot)
                : MakeFailure(Object::ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                              Resource::ResourceDomainError::None,
                              collected.HasValue() ? Object::ObjectRegistryError::Ok : collected.GetError());
        if (released.Error != Object::ObjectPoolError::None)
        {
            m_state = ProtectionDomainPoolState::Faulted;
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

Foundation::UInt64 ProtectionDomainPool::GetTagRefusals() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_tagRefusals;
}

bool ProtectionDomainPool::IsCreated(const ProtectionDomainId domain) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (const Slot& slot : m_slots)
    {
        if (slot.State != SlotState::Free && slot.Identity.IsEqualTo(domain))
        {
            return true;
        }
    }
    return false;
}

ProtectionDomainPoolSnapshot ProtectionDomainPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    ProtectionDomainPoolSnapshot snapshot{
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
            snapshot.FreeSlots += (m_state == ProtectionDomainPoolState::Ready) ? 1U : 0U;
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

Object::ObjectPoolFailure ProtectionDomainPool::WithdrawLocked(Slot& slot) noexcept
{
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity.GetObjectId());
    const auto collected =
        (destroyed == Object::ObjectRegistryError::Ok)
            ? m_issuer.CollectDestroyedObject(slot.Identity.GetObjectId())
            : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
    if (!collected.HasValue())
    {
        return MakeFailure(Object::ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None, collected.GetError());
    }
    return ReleaseSlotLocked(slot);
}

Object::ObjectPoolFailure ProtectionDomainPool::ReleaseSlotLocked(Slot& slot) noexcept
{
    // Quiesce releases the domain's charge and its AddressSpace and
    // ResourceDomain pins.
    if (slot.Instance->Quiesce() != ProtectionDomainError::None)
    {
        return MakeFailure(Object::ObjectPoolError::InvariantViolation);
    }
    slot.Instance->~ProtectionDomain();
    slot.Instance = nullptr;
    slot.Domain = nullptr;
    slot.CapabilitiesPin.Reset();
    if (slot.Channel != nullptr)
    {
        slot.Channel->ReleaseDomain();
        slot.Channel = nullptr;
    }
    slot.ChannelPin.Reset();
    slot.Tag = 0U;
    slot.Identity = ProtectionDomainId{};
    slot.State = SlotState::Free;
    return MakeFailure(Object::ObjectPoolError::None);
}
} // Namespace Asteria::Kernel::Protection
