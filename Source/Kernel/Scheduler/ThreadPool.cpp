// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Scheduler/ThreadPool.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Scheduler
{
namespace
{
using Object::MakeObjectPoolFailure;
using Object::ObjectPoolError;
using Object::ObjectPoolFailure;
} // Namespace

bool ThreadPool::Configure(IUserThreadBackend& backend) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_backend != nullptr)
    {
        return false;
    }
    m_backend = &backend;
    return true;
}

Foundation::Result<ThreadCreation, ObjectPoolFailure> ThreadPool::Create(const ThreadCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<ThreadCreation, ObjectPoolFailure>;

    Slot* slot{};
    Object::CapabilityHandle minted{};
    {
        Platform::Synchronization::SpinLockGuard guard{m_synchronization};
        if (m_state == ThreadPoolState::Faulted)
        {
            return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
        }
        if ((request.Space == nullptr) || !request.Space->IsReady() || !request.Space->IsFromRegistry(m_objectRegistry))
        {
            return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::InvalidConfiguration));
        }
        if (m_backend == nullptr)
        {
            return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::CapacityExceeded));
        }
        const auto authority = request.Space->Lookup(request.Authority, Object::ObjectType::ResourceDomain,
                                                     Object::CapabilityOperation::Manage);
        if (!authority.HasValue())
        {
            return ResultType::Failure(
                MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, authority.GetError()));
        }
        slot = FindFreeSlotLocked();
        if (slot == nullptr)
        {
            return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::CapacityExceeded));
        }
        // The lookup checked the type, and only ResourceDomain instances report ObjectType:
        // ResourceDomain. The pin keeps the domain registered while the slot holds its charge.
        Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
        if ((m_objectRegistry.PinObject(authority.GetValue().GetDescriptor().Object, Object::ObjectType::ResourceDomain,
                                        slot->DomainPin) != Object::ObjectPinError::Ok) ||
            (&slot->DomainPin.GetObject() != static_cast<Object::IKernelObject*>(&domain)))
        {
            slot->DomainPin.Reset();
            return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::ResourceDomainRejected));
        }
        const Resource::ResourceReservation charge{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = sizeof(Thread) + sizeof(SchedulingContext) + m_backend->GetNativeSlotBytes(),
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 1U,
        };
        const Resource::ResourceDomainError charged = domain.TryReserve(charge);
        if (charged != Resource::ResourceDomainError::None)
        {
            slot->DomainPin.Reset();
            return ResultType::Failure(
                MakeObjectPoolFailure(ObjectPoolError::ChargeRejected, Object::CapabilityError::Ok, charged));
        }
        slot->Domain = &domain;
        slot->Charge = charge;
        slot->State = SlotState::Reserved;
        const ObjectPoolFailure published = ReserveLocked(*slot, request, domain, minted);
        if (published.Error != ObjectPoolError::None)
        {
            return ResultType::Failure(published);
        }
    }
    // A home may need this pool while binding. Reserved storage and its charge survive the unlocked
    // backend call.
    const UserThreadBackendError started = m_backend->Start(
        *slot->ThreadInstance, ThreadId{m_objectRegistry, slot->ThreadIdentity}, *slot->ContextInstance,
        SchedulingContextId{m_objectRegistry, slot->ContextIdentity}, request.Start);
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (slot->State != SlotState::Reserved)
    {
        return ResultType::Failure(FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation)));
    }
    if (started != UserThreadBackendError::None)
    {
        const ObjectPoolError error =
            (started == UserThreadBackendError::Exhausted)
                ? ObjectPoolError::CapacityExceeded
                : ((started == UserThreadBackendError::Rejected) ? ObjectPoolError::InvalidRequest
                                                                 : ObjectPoolError::InvariantViolation);
        const ObjectPoolFailure abandoned = AbandonLocked(
            *slot, *request.Space, minted,
            MakeObjectPoolFailure(error, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                                  Object::ObjectRegistryError::Ok, static_cast<Foundation::UInt8>(started)));
        return ResultType::Failure((error == ObjectPoolError::InvariantViolation) ? FaultLocked(abandoned) : abandoned);
    }
    slot->State = SlotState::Active;
    ++m_created;
    return ResultType::Success(ThreadCreation{.Thread = slot->ThreadIdentity, .Capability = minted});
}

ObjectPoolFailure ThreadPool::ConstructLocked(Slot& slot, Resource::ResourceDomain& domain) noexcept
{
    slot.ThreadInstance = ::new (static_cast<void*>(slot.ThreadStorage)) Thread{};
    slot.ContextInstance = ::new (static_cast<void*>(slot.ContextStorage)) SchedulingContext{m_objectRegistry};
    const UserThreadSchedule schedule = m_backend->GetSchedule();
    if ((slot.ThreadInstance->Initialize(domain.GetIdentifier()) != ThreadError::None) ||
        (slot.ContextInstance->Initialize(schedule.Configuration, domain.GetIdentifier(), schedule.Now) !=
         SchedulingContextError::None))
    {
        return MakeObjectPoolFailure(ObjectPoolError::InvalidRequest);
    }
    const auto thread = m_issuer.CreateObject(*slot.ThreadInstance);
    if (!thread.HasValue())
    {
        return MakeObjectPoolFailure(ObjectPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                     Resource::ResourceDomainError::None, thread.GetError());
    }
    slot.ThreadIdentity = thread.GetValue();
    const auto context = m_issuer.CreateObject(*slot.ContextInstance);
    if (!context.HasValue())
    {
        return MakeObjectPoolFailure(ObjectPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                     Resource::ResourceDomainError::None, context.GetError());
    }
    slot.ContextIdentity = context.GetValue();
    if (slot.ContextInstance->Bind(ThreadId{m_objectRegistry, slot.ThreadIdentity}) != SchedulingContextError::None)
    {
        return MakeObjectPoolFailure(ObjectPoolError::InvalidRequest);
    }
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

ObjectPoolFailure ThreadPool::ReserveLocked(Slot& slot, const ThreadCreateRequest& request,
                                            Resource::ResourceDomain& domain, Object::CapabilityHandle& minted) noexcept
{
    Object::CapabilitySpace& space = *request.Space;
    const ObjectPoolFailure constructed = ConstructLocked(slot, domain);
    if (constructed.Error != ObjectPoolError::None)
    {
        return AbandonLocked(slot, space, Object::CapabilityHandle{}, constructed);
    }
    const auto capability =
        m_issuer.MintUnderAuthority(space, slot.ThreadIdentity, CreatedRights, request.Authority,
                                    Object::ObjectType::ResourceDomain, Object::CapabilityOperation::Manage, domain);
    if (!capability.HasValue())
    {
        return AbandonLocked(slot, space, Object::CapabilityHandle{},
                             MakeObjectPoolFailure(ObjectPoolError::MintRejected, capability.GetError()));
    }
    const auto reserved =
        m_backend->Reserve(*slot.ThreadInstance, ThreadId{m_objectRegistry, slot.ThreadIdentity}, *slot.ContextInstance,
                           SchedulingContextId{m_objectRegistry, slot.ContextIdentity}, request.Start);
    if (reserved != UserThreadBackendError::None)
    {
        const auto error = reserved == UserThreadBackendError::Exhausted  ? ObjectPoolError::CapacityExceeded
                           : reserved == UserThreadBackendError::Rejected ? ObjectPoolError::InvalidRequest
                                                                          : ObjectPoolError::InvariantViolation;
        const auto abandoned = AbandonLocked(
            slot, space, capability.GetValue(),
            MakeObjectPoolFailure(error, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                                  Object::ObjectRegistryError::Ok, static_cast<Foundation::UInt8>(reserved)));
        return error == ObjectPoolError::InvariantViolation ? FaultLocked(abandoned) : abandoned;
    }
    minted = capability.GetValue();
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

ObjectPoolFailure ThreadPool::AbandonLocked(Slot& slot, Object::CapabilitySpace& space,
                                            const Object::CapabilityHandle minted,
                                            const ObjectPoolFailure cause) noexcept
{
    // Nothing ran the thread and nothing else can hold a reference: close the
    // only capability, release the SchedulingContext's pin, and collect both
    // registrations at once.
    if (minted.IsValid() && (space.Close(minted) != Object::CapabilityError::Ok))
    {
        return FaultLocked(cause);
    }
    if (slot.ContextIdentity.IsValid() && (slot.ContextInstance->GetSnapshot().BaseThread.IsValid()) &&
        (slot.ContextInstance->Unbind(ThreadId{m_objectRegistry, slot.ThreadIdentity}) != SchedulingContextError::None))
    {
        return FaultLocked(cause);
    }
    const Object::ObjectId registered[2U]{slot.ContextIdentity, slot.ThreadIdentity};
    for (const Object::ObjectId identity : registered)
    {
        if (!identity.IsValid())
        {
            continue;
        }
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(identity);
        const auto collected =
            (destroyed == Object::ObjectRegistryError::Ok)
                ? m_issuer.CollectDestroyedObject(identity)
                : Foundation::Result<Object::ObjectDestructionRecord, Object::ObjectRegistryError>::Failure(destroyed);
        if (!collected.HasValue())
        {
            ObjectPoolFailure faulted = cause;
            faulted.Registry = collected.GetError();
            return FaultLocked(faulted);
        }
    }
    const ObjectPoolFailure released = ReleaseSlotLocked(slot);
    return (released.Error != ObjectPoolError::None) ? FaultLocked(released) : cause;
}

Foundation::Result<Foundation::UInt64, ObjectPoolFailure>
ThreadPool::Destroy(Object::CapabilitySpace& space, const Object::CapabilityHandle thread) noexcept
{
    using ResultType = Foundation::Result<Foundation::UInt64, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ThreadPoolState::Faulted)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    const auto lease = space.Lookup(thread, Object::ObjectType::Thread, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active) ||
        (slot->ThreadIdentity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::NotOwned));
    }
    const ThreadSnapshot snapshot = slot->ThreadInstance->GetSnapshot();
    if (slot->BackendReserved || (snapshot.State != ThreadState::Exited))
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Busy));
    }
    const Object::ObjectId registered[2U]{slot->ThreadIdentity, slot->ContextIdentity};
    for (const Object::ObjectId identity : registered)
    {
        const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(identity);
        if (destroyed != Object::ObjectRegistryError::Ok)
        {
            return ResultType::Failure(
                FaultLocked(MakeObjectPoolFailure(ObjectPoolError::DestroyRejected, Object::CapabilityError::Ok,
                                                  Resource::ResourceDomainError::None, destroyed)));
        }
    }
    slot->State = SlotState::Quiescing;
    m_quiescing = true;
    ++m_destroyed;
    m_lastExitCode = snapshot.ExitCode;
    return ResultType::Success(snapshot.ExitCode);
}

ObjectPoolFailure ThreadPool::Terminate(Object::CapabilitySpace& space, const Object::CapabilityHandle thread,
                                        const Foundation::UInt64 exitCode) noexcept
{
    Slot* slot{};
    {
        Platform::Synchronization::SpinLockGuard guard{m_synchronization};
        if (m_state == ThreadPoolState::Faulted)
        {
            return MakeObjectPoolFailure(ObjectPoolError::Faulted);
        }
        const auto lease = space.Lookup(thread, Object::ObjectType::Thread, Object::CapabilityOperation::Manage);
        if (!lease.HasValue())
        {
            return MakeObjectPoolFailure(ObjectPoolError::CapabilityRejected, lease.GetError());
        }
        slot = FindSlotLocked(lease.GetValue().GetObject());
        if ((slot == nullptr) || (slot->State != SlotState::Active) ||
            (slot->ThreadIdentity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
        {
            return MakeObjectPoolFailure(ObjectPoolError::NotOwned);
        }
        if (slot->ThreadInstance->GetSnapshot().State == ThreadState::Exited)
        {
            return MakeObjectPoolFailure(ObjectPoolError::None);
        }
        if (slot->BackendReserved)
        {
            return MakeObjectPoolFailure(ObjectPoolError::Busy);
        }
        slot->BackendReserved = true;
    }
    // Post/kick outside every pool lock. Destroy and a subtree drain cannot retire this slot during
    // the backend call.
    const UserThreadTerminateError answer = m_backend->Terminate(*slot->ThreadInstance, exitCode);
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    slot->BackendReserved = false;
    switch (answer)
    {
    case UserThreadTerminateError::None:
        ++m_terminated;
        return MakeObjectPoolFailure(ObjectPoolError::None);
    case UserThreadTerminateError::Pending:
    case UserThreadTerminateError::Blocked:
        return MakeObjectPoolFailure(ObjectPoolError::Busy);
    case UserThreadTerminateError::Running:
        return MakeObjectPoolFailure(ObjectPoolError::InvalidRequest);
    case UserThreadTerminateError::Unsupported:
        return MakeObjectPoolFailure(ObjectPoolError::InvalidConfiguration);
    case UserThreadTerminateError::Faulted:
        break;
    }
    return FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation));
}

bool ThreadPool::HaveChargedExited(const Resource::IResourceDomainSubtree& subtree) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (const Slot& slot : m_slots)
    {
        // A subtree cannot begin draining while creation
        // or an unlocked backend call still owns arguments in this pool.
        if ((slot.State != SlotState::Free) && subtree.Contains(slot.Domain) &&
            (slot.State == SlotState::Reserved || slot.BackendReserved ||
             slot.ThreadInstance->GetSnapshot().State != ThreadState::Exited))
        {
            return false;
        }
    }
    return true;
}

// A thread that has not exited stays; the drain asked HaveChargedExited
// first, so that is a thread created meanwhile.
ObjectPoolFailure ThreadPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ThreadPoolState::Faulted)
    {
        return MakeObjectPoolFailure(ObjectPoolError::Faulted);
    }
    bool busy{false};
    for (Slot& slot : m_slots)
    {
        if (slot.State == SlotState::Reserved && subtree.Contains(slot.Domain))
        {
            busy = true;
            continue;
        }
        if ((slot.State != SlotState::Active) || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        if (slot.BackendReserved || (slot.ThreadInstance->GetSnapshot().State != ThreadState::Exited))
        {
            busy = true;
            continue;
        }
        const Object::ObjectId registered[2U]{slot.ThreadIdentity, slot.ContextIdentity};
        for (const Object::ObjectId identity : registered)
        {
            const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(identity);
            if (destroyed != Object::ObjectRegistryError::Ok)
            {
                return FaultLocked(MakeObjectPoolFailure(ObjectPoolError::DestroyRejected, Object::CapabilityError::Ok,
                                                         Resource::ResourceDomainError::None, destroyed));
            }
        }
        slot.State = SlotState::Quiescing;
        m_quiescing = true;
        ++m_destroyed;
    }
    return MakeObjectPoolFailure(busy ? ObjectPoolError::Busy : ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, ObjectPoolFailure> ThreadPool::Collect() noexcept
{
    using ResultType = Foundation::Result<Foundation::Size, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ThreadPoolState::Faulted)
    {
        return ResultType::Failure(MakeObjectPoolFailure(ObjectPoolError::Faulted));
    }
    Foundation::Size reclaimed{0U};
    // No slot became Quiescing since a Collect left none: nothing to scan.
    if (!m_quiescing)
    {
        return ResultType::Success(0U);
    }
    for (Slot& slot : m_slots)
    {
        // The run loop still names a thread whose native slot is bound.
        if ((slot.State != SlotState::Quiescing) || !m_backend->IsReleased(*slot.ThreadInstance))
        {
            continue;
        }
        if (!slot.ContextUnbound)
        {
            if (slot.ContextInstance->Unbind(ThreadId{m_objectRegistry, slot.ThreadIdentity}) !=
                SchedulingContextError::None)
            {
                return ResultType::Failure(FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation)));
            }
            slot.ContextUnbound = true;
        }
        bool ready{true};
        const Object::ObjectId registered[2U]{slot.ContextIdentity, slot.ThreadIdentity};
        for (const Object::ObjectId identity : registered)
        {
            if (!identity.IsValid())
            {
                continue;
            }
            const auto collected = m_issuer.CollectDestroyedObject(identity);
            if (!collected.HasValue() && (collected.GetError() == Object::ObjectRegistryError::NoDestructionReady))
            {
                ready = false;
                break;
            }
            if (!collected.HasValue())
            {
                return ResultType::Failure(
                    FaultLocked(MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                                      Resource::ResourceDomainError::None, collected.GetError())));
            }
            // A collected registration is not collected again.
            if (identity.GetValue() == slot.ContextIdentity.GetValue())
            {
                slot.ContextIdentity = Object::ObjectId{};
            }
            else
            {
                slot.ThreadIdentity = Object::ObjectId{};
            }
        }
        if (!ready)
        {
            continue;
        }
        const ObjectPoolFailure released = ReleaseSlotLocked(slot);
        if (released.Error != ObjectPoolError::None)
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

ThreadPoolSnapshot ThreadPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    ThreadPoolSnapshot snapshot{
        .FreeSlots = 0U,
        .ActiveSlots = 0U,
        .QuiescingSlots = 0U,
        .Created = m_created,
        .Destroyed = m_destroyed,
        .Reclaimed = m_reclaimed,
        .LastExitCode = m_lastExitCode,
        .Terminated = m_terminated,
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
        case SlotState::Reserved:
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

ThreadPool::Slot* ThreadPool::FindFreeSlotLocked() noexcept
{
    for (Slot& slot : m_slots)
    {
        if ((slot.State == SlotState::Free) && (slot.ThreadInstance == nullptr))
        {
            return &slot;
        }
    }
    return nullptr;
}

ThreadPool::Slot* ThreadPool::FindSlotLocked(const Object::IKernelObject& instance) noexcept
{
    for (Slot& slot : m_slots)
    {
        if ((slot.ThreadInstance != nullptr) &&
            (static_cast<const Object::IKernelObject*>(slot.ThreadInstance) == &instance))
        {
            return &slot;
        }
    }
    return nullptr;
}

ObjectPoolFailure ThreadPool::ReleaseSlotLocked(Slot& slot) noexcept
{
    const Resource::ResourceDomainError released = slot.Domain->Release(slot.Charge);
    if (released != Resource::ResourceDomainError::None)
    {
        return MakeObjectPoolFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok, released);
    }
    if (slot.ContextInstance != nullptr)
    {
        slot.ContextInstance->~SchedulingContext();
    }
    if (slot.ThreadInstance != nullptr)
    {
        slot.ThreadInstance->~Thread();
    }
    slot.ThreadInstance = nullptr;
    slot.ContextInstance = nullptr;
    slot.Domain = nullptr;
    slot.DomainPin.Reset();
    slot.Charge = Resource::ResourceReservation{};
    slot.ThreadIdentity = Object::ObjectId{};
    slot.ContextIdentity = Object::ObjectId{};
    slot.ContextUnbound = false;
    slot.BackendReserved = false;
    slot.State = SlotState::Free;
    return MakeObjectPoolFailure(ObjectPoolError::None);
}

ObjectPoolFailure ThreadPool::FaultLocked(const ObjectPoolFailure failure) noexcept
{
    m_state = ThreadPoolState::Faulted;
    ObjectPoolFailure faulted = failure;
    faulted.Error = ObjectPoolError::InvariantViolation;
    return faulted;
}
} // Namespace Asteria::Kernel::Scheduler
