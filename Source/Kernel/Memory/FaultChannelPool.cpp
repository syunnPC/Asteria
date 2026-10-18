// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/FaultChannelPool.hpp"
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif
namespace Asteria::Kernel::Memory
{
namespace
{
using Result = Foundation::Result<Object::CapabilityHandle, Object::ObjectPoolFailure>;
[[nodiscard]] Object::ObjectPoolFailure
Failure(const Object::ObjectPoolError error, const Object::CapabilityError capability = Object::CapabilityError::Ok,
        const Resource::ResourceDomainError resource = Resource::ResourceDomainError::None) noexcept
{
    return Object::MakeObjectPoolFailure(error, capability, resource);
}
} // Namespace
bool FaultChannelPool::Configure(Object::RevocationRegistry& revocations, Time::IMonotonicClock& clock,
                                 const Object::CapabilityValidationKey* const keys,
                                 Scheduler::ProcessorSchedulerWakeFacade& wake, TlbShootdownDispatcher& shootdown,
                                 Hal::Memory::ITlbShootdownTransport& transport,
                                 IPagerShootdownProgress& progress) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_lock};
    if (m_revocations != nullptr || keys == nullptr)
    {
        return false;
    }
    for (Foundation::Size i{0U}; i < Capacity; ++i)
    {
        if (keys[i].Word0 == 0U && keys[i].Word1 == 0U)
        {
            return false;
        }
    }
    m_revocations = &revocations;
    m_clock = &clock;
    m_wake = &wake;
    m_shootdown = &shootdown;
    m_transport = &transport;
    m_progress = &progress;
    for (Foundation::Size i{0U}; i < Capacity; ++i)
    {
        m_keys[i] = keys[i];
    }
    return true;
}
Result FaultChannelPool::Create(Object::CapabilitySpace& caller, const Object::CapabilityHandle authorityHandle,
                                const Object::ObjectId receiver) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_lock};
    if (m_faulted)
    {
        return Result::Failure(Failure(Object::ObjectPoolError::Faulted));
    }
    if (!caller.IsReady() || !caller.IsFromRegistry(m_registry))
    {
        return Result::Failure(Failure(Object::ObjectPoolError::InvalidConfiguration));
    }
    if (m_revocations == nullptr)
    {
        return Result::Failure(Failure(Object::ObjectPoolError::CapacityExceeded));
    }
    const auto authority =
        caller.Lookup(authorityHandle, Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return Result::Failure(Failure(Object::ObjectPoolError::CapabilityRejected, authority.GetError()));
    }
    for (Foundation::Size i{0U}; i < Capacity; ++i)
    {
        Slot& slot = m_slots[i];
        if (slot.Status != State::Free)
        {
            continue;
        }
        auto& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
        if (m_registry.PinObject(authority.GetValue().GetDescriptor().Object, Object::ObjectType::ResourceDomain,
                                 slot.DomainPin) != Object::ObjectPinError::Ok)
        {
            return Result::Failure(Failure(Object::ObjectPoolError::ResourceDomainRejected));
        }
        if (receiver.IsValid() && m_registry.PinObject(receiver, Object::ObjectType::CapabilitySpace,
                                                       slot.ReceiverPin) != Object::ObjectPinError::Ok)
        {
            slot.DomainPin.Reset();
            return Result::Failure(
                Failure(Object::ObjectPoolError::CapabilityRejected, Object::CapabilityError::ObjectNotFound));
        }
        const auto charge = domain.TryReserve(GetReservation());
        if (charge != Resource::ResourceDomainError::None)
        {
            slot.DomainPin.Reset();
            slot.ReceiverPin.Reset();
            return Result::Failure(
                Failure(Object::ObjectPoolError::ChargeRejected, Object::CapabilityError::Ok, charge));
        }
        slot.Domain = &domain;
        slot.Instance = ::new (static_cast<void*>(slot.Storage))
            FaultChannel{m_registry, m_issuer,  *m_revocations, *m_clock,     caller,
                         domain,     m_keys[i], *m_wake,        *m_shootdown, *m_transport};
        return Publish(slot, caller, authorityHandle);
    }
    return Result::Failure(Failure(Object::ObjectPoolError::CapacityExceeded));
}
Result FaultChannelPool::Publish(Slot& slot, Object::CapabilitySpace& caller,
                                 const Object::CapabilityHandle authority) noexcept
{
    const auto root = m_revocations->CreateRoot();
    if (root.HasValue())
    {
        slot.Root = root.GetValue();
    }
    const bool ready =
        slot.Instance->GetReadiness().Initialize(slot.Domain->GetIdentifier()) == Ipc::NotificationError::None;
    const auto notification = ready ? m_issuer.CreateObject(slot.Instance->GetReadiness())
                                    : Foundation::Result<Object::ObjectId, Object::ObjectRegistryError>::Failure(
                                          Object::ObjectRegistryError::InvalidObject);
    if (notification.HasValue())
    {
        slot.Readiness = notification.GetValue();
    }
    const auto registered = m_issuer.CreateObject(*slot.Instance);
    if (registered.HasValue())
    {
        slot.Identity = registered.GetValue();
    }
    const bool configured =
        root.HasValue() && notification.HasValue() && registered.HasValue() &&
        slot.Instance->Initialize(m_registry, slot.Identity, slot.Readiness, slot.Root, *m_progress);
    const auto minted = configured ? m_issuer.MintUnderAuthority(caller, slot.Identity, CreatedRights, authority,
                                                                 Object::ObjectType::ResourceDomain,
                                                                 Object::CapabilityOperation::Manage, *slot.Domain)
                                   : Foundation::Result<Object::CapabilityHandle, Object::CapabilityError>::Failure(
                                         Object::CapabilityError::SpaceExhausted);
    if (!minted.HasValue())
    {
        const auto withdraw = [this](const Object::ObjectId identity) noexcept
        {
            return !identity.IsValid() || (m_issuer.DestroyObject(identity) == Object::ObjectRegistryError::Ok &&
                                           m_issuer.CollectDestroyedObject(identity).HasValue());
        };
        if (!withdraw(slot.Identity) || !withdraw(slot.Readiness) || !ReleaseSlot(slot))
        {
            m_faulted = true;
            return Result::Failure(Failure(Object::ObjectPoolError::InvariantViolation));
        }
        return Result::Failure(
            Failure(configured ? Object::ObjectPoolError::MintRejected : Object::ObjectPoolError::CapacityExceeded,
                    minted.GetError()));
    }
    slot.Metrics = {.Identity = slot.Identity.GetValue()};
    slot.Status = State::Active;
    ++m_created;
    return Result::Success(minted.GetValue());
}
Object::ObjectPoolFailure FaultChannelPool::DestroySlot(Slot& slot) noexcept
{
    if (!slot.Instance->BeginDestroy())
    {
        return Failure(Object::ObjectPoolError::Busy);
    }
    if (m_issuer.DestroyObject(slot.Identity) != Object::ObjectRegistryError::Ok ||
        m_issuer.DestroyObject(slot.Readiness) != Object::ObjectRegistryError::Ok)
    {
        m_faulted = true;
        return Failure(Object::ObjectPoolError::InvariantViolation);
    }
    slot.Status = State::Quiescing;
    ++m_destroyed;
    return Failure(Object::ObjectPoolError::None);
}
Object::ObjectPoolFailure FaultChannelPool::Destroy(Object::CapabilitySpace& caller,
                                                    const Object::CapabilityHandle handle) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_lock};
    if (m_faulted)
    {
        return Failure(Object::ObjectPoolError::Faulted);
    }
    const auto lease = caller.Lookup(handle, Object::ObjectType::FaultChannel, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return Failure(Object::ObjectPoolError::CapabilityRejected, lease.GetError());
    }
    for (Slot& slot : m_slots)
    {
        if (slot.Status == State::Active && slot.Instance == &lease.GetValue().GetObject())
        {
            return DestroySlot(slot);
        }
    }
    return Failure(Object::ObjectPoolError::NotOwned);
}
Object::ObjectPoolFailure FaultChannelPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_lock};
    if (m_faulted)
    {
        return Failure(Object::ObjectPoolError::Faulted);
    }
    bool busy{false};
    for (Slot& slot : m_slots)
    {
        if (slot.Status != State::Active || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        const auto result = DestroySlot(slot);
        if (result.Error == Object::ObjectPoolError::Busy)
        {
            busy = true;
        }
        else if (result.Error != Object::ObjectPoolError::None)
        {
            return result;
        }
    }
    return Failure(busy ? Object::ObjectPoolError::Busy : Object::ObjectPoolError::None);
}
bool FaultChannelPool::ReleaseSlot(Slot& slot) noexcept
{
    if (slot.Root.IsValid() && m_revocations->Destroy(slot.Root) != Object::RevocationError::Ok)
    {
        return false;
    }
    if (slot.Domain->Release(GetReservation()) != Resource::ResourceDomainError::None)
    {
        return false;
    }
    const auto channel = slot.Instance->GetChannel().GetSnapshot();
    m_deliveries += channel.Deliveries;
    m_resolutions += channel.Resolutions;
    m_terminations += channel.Terminations;
    slot.Metrics.Deliveries += channel.Deliveries;
    slot.Metrics.Resolutions += channel.Resolutions;
    slot.Metrics.Terminations += channel.Terminations;
    slot.Metrics.Busy += channel.Busy;
    slot.Instance->~FaultChannel();
    slot.Instance = nullptr;
    slot.Domain = nullptr;
    slot.DomainPin.Reset();
    slot.ReceiverPin.Reset();
    slot.Identity = {};
    slot.Readiness = {};
    slot.Root = {};
    slot.Status = State::Free;
    return true;
}
Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> FaultChannelPool::Collect() noexcept
{
    using Collected = Foundation::Result<Foundation::Size, Object::ObjectPoolFailure>;
    Platform::Synchronization::SpinLockGuard guard{m_lock};
    if (m_faulted)
    {
        return Collected::Failure(Failure(Object::ObjectPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    for (Slot& slot : m_slots)
    {
        if (slot.Status != State::Quiescing)
        {
            continue;
        }
        if (slot.Readiness.IsValid())
        {
            const auto collected = m_issuer.CollectDestroyedObject(slot.Readiness);
            if (!collected.HasValue())
            {
                if (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady)
                {
                    continue;
                }
                m_faulted = true;
                break;
            }
            slot.Readiness = {};
        }
        const auto collected = m_issuer.CollectDestroyedObject(slot.Identity);
        if (!collected.HasValue())
        {
            if (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady)
            {
                continue;
            }
            m_faulted = true;
            break;
        }
        if (!slot.Instance->GetEndpoint().DiscardUnread().HasValue() || !ReleaseSlot(slot))
        {
            m_faulted = true;
            break;
        }
        ++reclaimed;
        ++m_reclaimed;
    }
    return m_faulted ? Collected::Failure(Failure(Object::ObjectPoolError::InvariantViolation))
                     : Collected::Success(reclaimed);
}
bool FaultChannelPool::Progress() noexcept
{
    // No pool lock spans shootdown progress. The exact registry generation stays pinned across
    // Retry and observation.
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        Object::ObjectPin pin;
        {
            Platform::Synchronization::SpinLockGuard guard{m_lock};
            if (m_faulted)
            {
                return false;
            }
            Slot& slot = m_slots[index];
            if (slot.Status != State::Active)
            {
                continue;
            }
            if (m_registry.PinObject(slot.Identity, Object::ObjectType::FaultChannel, pin) !=
                Object::ObjectPinError::Ok)
            {
                m_faulted = true;
                return false;
            }
        }
        auto& channel = static_cast<FaultChannel&>(pin.GetObject()).GetChannel();
        channel.Retry();
        if (channel.GetSnapshot().State == PagerChannelState::Faulted)
        {
            return false;
        }
    }
    return true;
}
FaultChannelMetrics FaultChannelPool::GetMetrics(const Foundation::Size index) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_lock};
    if (index >= Capacity)
    {
        return {};
    }
    const Slot& slot = m_slots[index];
    FaultChannelMetrics result = slot.Metrics;
    if (slot.Status != State::Free)
    {
        const auto channel = slot.Instance->GetChannel().GetSnapshot();
        result.Deliveries += channel.Deliveries;
        result.Resolutions += channel.Resolutions;
        result.Terminations += channel.Terminations;
        result.Busy += channel.Busy;
    }
    return result;
}

FaultChannelPoolSnapshot FaultChannelPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_lock};
    FaultChannelPoolSnapshot result{};
    result.Created = m_created;
    result.Destroyed = m_destroyed;
    result.Reclaimed = m_reclaimed;
    result.Deliveries = m_deliveries;
    result.Resolutions = m_resolutions;
    result.Terminations = m_terminations;
    for (const Slot& slot : m_slots)
    {
        if (slot.Status == State::Free)
        {
            result.FreeSlots += m_revocations != nullptr ? 1U : 0U;
        }
        else
        {
            if (slot.Status == State::Active)
            {
                ++result.ActiveSlots;
            }
            else
            {
                ++result.QuiescingSlots;
            }
            const auto channel = slot.Instance->GetChannel().GetSnapshot();
            result.Deliveries += channel.Deliveries;
            result.Resolutions += channel.Resolutions;
            result.Terminations += channel.Terminations;
        }
    }
    return result;
}
} // Namespace Asteria::Kernel::Memory
