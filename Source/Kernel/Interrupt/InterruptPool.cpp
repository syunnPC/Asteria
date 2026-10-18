// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Interrupt/InterruptPool.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Kernel::Interrupt
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

// Why the device control would not send the function's messages, as the
// pool reports it; Detail keeps its error.
[[nodiscard]] ObjectPoolFailure MapMessages(const Device::PciFunctionControlError error) noexcept
{
    const auto detail = static_cast<Foundation::UInt8>(error);
    switch (error)
    {
    case Device::PciFunctionControlError::NotAssignee:
    case Device::PciFunctionControlError::Revoked:
        return MakeFailure(ObjectPoolError::NotOwned, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                           Object::ObjectRegistryError::Ok, detail);
    case Device::PciFunctionControlError::MessagesInUse:
        return MakeFailure(ObjectPoolError::Busy, Object::CapabilityError::Ok, Resource::ResourceDomainError::None,
                           Object::ObjectRegistryError::Ok, detail);
    case Device::PciFunctionControlError::NotConfigured:
    case Device::PciFunctionControlError::ConfigurationRejected:
        return MakeFailure(ObjectPoolError::InvalidConfiguration, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok, detail);
    default:
        return MakeFailure(ObjectPoolError::InvalidRequest, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok, detail);
    }
}
} // Namespace

bool InterruptPool::Configure(Hal::Interrupt::IMessageVectors& vectors, Device::PciFunctionControl& functions) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_vectors != nullptr)
    {
        return false;
    }
    m_vectors = &vectors;
    m_functions = &functions;
    return true;
}

bool InterruptPool::ConfigureLines(Hal::Interrupt::IInterruptLines& lines,
                                   Device::PciFunctionControl& functions) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_lines != nullptr)
    {
        return false;
    }
    m_lines = &lines;
    m_functions = &functions;
    return true;
}

Foundation::Result<InterruptCreation, ObjectPoolFailure>
InterruptPool::Create(const InterruptCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<InterruptCreation, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
    }
    if (((m_vectors == nullptr) && (m_lines == nullptr)) || (request.Space == nullptr) || !request.Space->IsReady() ||
        !request.Space->IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidConfiguration));
    }
    if ((request.Source > 1U) || ((request.Source == 1U) && (m_lines == nullptr)))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidRequest));
    }
    const auto authority = request.Space->Lookup(request.Authority, Object::ObjectType::ResourceDomain,
                                                 Object::CapabilityOperation::Manage);
    if (!authority.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, authority.GetError()));
    }
    const auto function = request.Space->Lookup(
        request.Function, (request.Source == 1U) ? Object::ObjectType::InterruptLine : Object::ObjectType::PciFunction,
        Object::CapabilityOperation::Invoke);
    if (!function.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, function.GetError()));
    }
    Slot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapacityExceeded));
    }

    // The lookups checked the types, and only ResourceDomain and PciFunction
    // instances report them. The pin keeps the domain while the slot holds
    // its charge.
    Resource::ResourceDomain& domain = static_cast<Resource::ResourceDomain&>(authority.GetValue().GetObject());
    if ((m_objectRegistry.PinObject(authority.GetValue().GetDescriptor().Object, Object::ObjectType::ResourceDomain,
                                    slot->DomainPin) != Object::ObjectPinError::Ok) ||
        (&slot->DomainPin.GetObject() != static_cast<Object::IKernelObject*>(&domain)))
    {
        slot->DomainPin.Reset();
        return ResultType::Failure(MakeFailure(ObjectPoolError::ResourceDomainRejected));
    }
    const Resource::ResourceDomainError charged = domain.TryReserve(GetReservation());
    if (charged != Resource::ResourceDomainError::None)
    {
        slot->DomainPin.Reset();
        return ResultType::Failure(MakeFailure(ObjectPoolError::ChargeRejected, Object::CapabilityError::Ok, charged));
    }
    slot->Domain = &domain;
    slot->Function = nullptr;
    if (request.Source == 1U)
    {
        const auto& line = static_cast<const InterruptLine&>(function.GetValue().GetObject());
        slot->GlobalSystemInterrupt = line.GetLine();
        if (m_objectRegistry.PinObject(function.GetValue().GetDescriptor().Object, Object::ObjectType::InterruptLine,
                                       slot->LinePin) != Object::ObjectPinError::Ok)
        {
            AbandonLocked(*slot);
            return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidRequest));
        }
    }
    else
    {
        slot->Function = &static_cast<const Device::PciFunction&>(function.GetValue().GetObject());
    }
    slot->Notification = ::new (static_cast<void*>(slot->NotificationStorage)) Ipc::Notification{};
    const auto registered = (slot->Notification->Initialize(domain.GetIdentifier()) == Ipc::NotificationError::None)
                                ? m_issuer.CreateObject(*slot->Notification)
                                : Foundation::Result<Object::ObjectId, Object::ObjectRegistryError>::Failure(
                                      Object::ObjectRegistryError::InvalidObject);
    if (!registered.HasValue())
    {
        AbandonLocked(*slot);
        return ResultType::Failure(MakeFailure(ObjectPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, registered.GetError()));
    }
    slot->NotificationIdentity = registered.GetValue();
    // A line's source exists before the Interrupt built on it: its vector is
    // reserved first, and published once the Interrupt is.
    if ((request.Source == 1U) || (m_vectors == nullptr))
    {
        const ObjectPoolFailure reserved = ReserveLineLocked(*slot);
        if (reserved.Error != ObjectPoolError::None)
        {
            AbandonLocked(*slot);
            return ResultType::Failure(reserved);
        }
    }
    Hal::Interrupt::IInterruptSource& source =
        (slot->Line != nullptr) ? *slot->Line : static_cast<Hal::Interrupt::IInterruptSource&>(slot->Source);
    slot->Instance = ::new (static_cast<void*>(slot->InterruptStorage)) Interrupt{m_objectRegistry, source};
    const InterruptFailure initialized = slot->Instance->Initialize(
        domain.GetIdentifier(), Ipc::NotificationId{m_objectRegistry, slot->NotificationIdentity}, *slot->Notification);
    if (initialized.Error != InterruptError::None)
    {
        AbandonLocked(*slot);
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidRequest, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok,
                                               static_cast<Foundation::UInt8>(initialized.Error)));
    }
    return CompleteLocked(*slot, request);
}

Foundation::Result<InterruptCreation, ObjectPoolFailure>
InterruptPool::CompleteLocked(Slot& slot, const InterruptCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<InterruptCreation, ObjectPoolFailure>;

    if (slot.Line != nullptr)
    {
        if (!m_lines->Publish(slot.Vector, *slot.Instance))
        {
            AbandonLocked(slot);
            return ResultType::Failure(MakeFailure(ObjectPoolError::InvariantViolation));
        }
        const Device::PciFunctionControlError enabled = (slot.Function == nullptr)
                                                            ? Device::PciFunctionControlError::None
                                                            : m_functions->EnableLine(*slot.Function, request.Caller);
        if (enabled != Device::PciFunctionControlError::None)
        {
            AbandonLocked(slot);
            return ResultType::Failure(MapMessages(enabled));
        }
    }
    else
    {
        const Hal::Interrupt::MessageTarget target = m_vectors->Bind(*slot.Instance);
        if (target.Vector == 0U)
        {
            AbandonLocked(slot);
            return ResultType::Failure(MakeFailure(ObjectPoolError::CapacityExceeded));
        }
        slot.Vector = target.Vector;
        const Device::PciFunctionControlError enabled =
            m_functions->EnableMessages(*slot.Function, request.Caller, target.Address, target.Data);
        if (enabled != Device::PciFunctionControlError::None)
        {
            AbandonLocked(slot);
            return ResultType::Failure(MapMessages(enabled));
        }
    }
    slot.MessagesEnabled = slot.Function != nullptr;
    const auto registered = m_issuer.CreateObject(*slot.Instance);
    if (!registered.HasValue())
    {
        AbandonLocked(slot);
        return ResultType::Failure(MakeFailure(ObjectPoolError::RegistrationRejected, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, registered.GetError()));
    }
    slot.Identity = registered.GetValue();
    const auto minted = m_issuer.MintUnderAuthority(*request.Space, slot.Identity, CreatedRights, request.Authority,
                                                    Object::ObjectType::ResourceDomain,
                                                    Object::CapabilityOperation::Manage, *slot.Domain);
    if (!minted.HasValue())
    {
        AbandonLocked(slot);
        return ResultType::Failure(MakeFailure(ObjectPoolError::MintRejected, minted.GetError()));
    }
    slot.State = SlotState::Active;
    ++m_created;
    return ResultType::Success(InterruptCreation{.Interrupt = slot.Identity, .Capability = minted.GetValue()});
}

Foundation::Result<Object::ObjectId, ObjectPoolFailure>
InterruptPool::Destroy(Object::CapabilitySpace& space, const Object::CapabilityHandle interrupt) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
    }
    const auto lease = space.Lookup(interrupt, Object::ObjectType::Interrupt, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active) ||
        (slot->Identity.GetValue() != lease.GetValue().GetDescriptor().Object.GetValue()))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::NotOwned));
    }
    const ObjectPoolFailure destroyed = DestroySlotLocked(*slot);
    if (destroyed.Error != ObjectPoolError::None)
    {
        return ResultType::Failure(destroyed);
    }
    return ResultType::Success(slot->Identity);
}

ObjectPoolFailure InterruptPool::DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return MakeFailure(ObjectPoolError::Faulted);
    }
    // A slot whose messages cannot be turned off yet stays for the retry;
    // the others go now.
    bool busy{false};
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Active) || !subtree.Contains(slot.Domain))
        {
            continue;
        }
        const ObjectPoolFailure destroyed = DestroySlotLocked(slot);
        busy = busy || (destroyed.Error == ObjectPoolError::Busy);
        if ((destroyed.Error != ObjectPoolError::None) && (destroyed.Error != ObjectPoolError::Busy))
        {
            return destroyed;
        }
    }
    return MakeFailure(busy ? ObjectPoolError::Busy : ObjectPoolError::None);
}

Foundation::Result<Foundation::Size, ObjectPoolFailure> InterruptPool::Collect() noexcept
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
    for (Slot& slot : m_slots)
    {
        if (slot.State != SlotState::Quiescing)
        {
            continue;
        }
        // The Interrupt goes once its last lease is gone; its Notification
        // once no WaitSet registration pins it either.
        ObjectPoolFailure collected =
            slot.InterruptCollected ? MakeFailure(ObjectPoolError::None)
                                    : CollectRegistrationLocked(slot.Identity, *slot.Instance, slot.InterruptCollected);
        if ((collected.Error == ObjectPoolError::None) && slot.InterruptCollected && !slot.NotificationCollected)
        {
            collected =
                CollectRegistrationLocked(slot.NotificationIdentity, *slot.Notification, slot.NotificationCollected);
        }
        if (collected.Error != ObjectPoolError::None)
        {
            return ResultType::Failure(FaultLocked(collected));
        }
        if (!slot.InterruptCollected || !slot.NotificationCollected)
        {
            continue;
        }
        ReleaseSlotLocked(slot);
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

Foundation::Result<Foundation::UInt64, ObjectPoolFailure>
InterruptPool::Control(Object::CapabilitySpace& space, const Object::CapabilityHandle interrupt,
                       const Foundation::UInt64 operation) noexcept
{
    using ResultType = Foundation::Result<Foundation::UInt64, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_faulted)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::Faulted));
    }
    if ((operation != Rearm) && (operation != KeepMasked))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidRequest));
    }
    const auto lease = space.Lookup(interrupt, Object::ObjectType::Interrupt, Object::CapabilityOperation::Manage);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::NotOwned));
    }
    const InterruptSnapshot snapshot = slot->Instance->GetSnapshot();
    InterruptError error{InterruptError::None};
    if (snapshot.State == InterruptState::Pending)
    {
        const auto acknowledged = slot->Instance->Acknowledge(
            snapshot.PendingEvent, (operation == Rearm) ? InterruptCompletion::Rearm : InterruptCompletion::KeepMasked);
        error = acknowledged.HasValue() ? InterruptError::None : acknowledged.GetError().Error;
    }
    else if ((snapshot.State == InterruptState::Masked) && (operation == Rearm))
    {
        error = slot->Instance->Arm().Error;
    }
    else if ((snapshot.State == InterruptState::Armed) && (operation == KeepMasked))
    {
        error = slot->Instance->Mask().Error;
    }
    else if ((snapshot.State != InterruptState::Masked) && (snapshot.State != InterruptState::Armed))
    {
        error = InterruptError::Faulted;
    }
    if (error != InterruptError::None)
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::InvalidRequest, Object::CapabilityError::Ok,
                                               Resource::ResourceDomainError::None, Object::ObjectRegistryError::Ok,
                                               static_cast<Foundation::UInt8>(error)));
    }
    return ResultType::Success(slot->Instance->GetSnapshot().LastEventSequence);
}

Foundation::Result<InterruptReadiness, ObjectPoolFailure>
InterruptPool::FindReadiness(Object::CapabilitySpace& space, const Object::CapabilityHandle interrupt) noexcept
{
    using ResultType = Foundation::Result<InterruptReadiness, ObjectPoolFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const auto lease = space.Lookup(interrupt, Object::ObjectType::Interrupt, Object::CapabilityOperation::Wait);
    if (!lease.HasValue())
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::CapabilityRejected, lease.GetError()));
    }
    Slot* const slot = FindSlotLocked(lease.GetValue().GetObject());
    if ((slot == nullptr) || (slot->State != SlotState::Active))
    {
        return ResultType::Failure(MakeFailure(ObjectPoolError::NotOwned));
    }
    return ResultType::Success(InterruptReadiness{
        .Identity = Ipc::NotificationId{m_objectRegistry, slot->NotificationIdentity},
        .Notification = slot->Notification,
    });
}

InterruptPoolSnapshot InterruptPool::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    InterruptPoolSnapshot snapshot{.Slots = ((m_vectors != nullptr) || (m_lines != nullptr)) ? Capacity : 0U,
                                   .ActiveSlots = 0U,
                                   .QuiescingSlots = 0U,
                                   .Created = m_created,
                                   .Destroyed = m_destroyed,
                                   .Reclaimed = m_reclaimed,
                                   .Faulted = m_faulted,
                                   .Reserved = {}};
    for (const Slot& slot : m_slots)
    {
        snapshot.ActiveSlots += (slot.State == SlotState::Active) ? 1U : 0U;
        snapshot.QuiescingSlots += (slot.State == SlotState::Quiescing) ? 1U : 0U;
    }
    return snapshot;
}

InterruptPool::Slot* InterruptPool::FindFreeSlotLocked() noexcept
{
    for (Slot& slot : m_slots)
    {
        if ((slot.State == SlotState::Free) && (slot.Instance == nullptr) && (slot.Notification == nullptr))
        {
            return &slot;
        }
    }
    return nullptr;
}

InterruptPool::Slot* InterruptPool::FindSlotLocked(const Object::IKernelObject& instance) noexcept
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

// Messages off first, then the pending event acknowledged and the source
// masked, then the release of source and Notification, all with interrupts
// disabled on the processor messages target, so the handler cannot run in
// between (Interrupt.md). While the messages cannot be turned off the
// Interrupt is only masked, and the stop is retried: a drain turns the
// function's bus mastering off meanwhile, which keeps its messages from
// arriving.
InterruptPool::StopOutcome InterruptPool::StopLocked(Slot& slot) noexcept
{
    if (slot.MessagesEnabled && (((slot.Line != nullptr) ? m_functions->DisableLine(*slot.Function)
                                                         : m_functions->DisableMessages(*slot.Function)) ==
                                 Device::PciFunctionControlError::None))
    {
        slot.MessagesEnabled = false;
    }
    const InterruptSnapshot snapshot = slot.Instance->GetSnapshot();
    if (snapshot.State == InterruptState::Pending)
    {
        static_cast<void>(slot.Instance->Acknowledge(snapshot.PendingEvent, InterruptCompletion::KeepMasked));
    }
    else if (snapshot.State == InterruptState::Armed)
    {
        static_cast<void>(slot.Instance->Mask());
    }
    if (slot.MessagesEnabled)
    {
        return StopOutcome::Retry;
    }
    const InterruptError quiesced = slot.Instance->Quiesce().Error;
    return ((quiesced == InterruptError::None) || (quiesced == InterruptError::AlreadyQuiesced)) ? StopOutcome::Stopped
                                                                                                 : StopOutcome::Failed;
}

ObjectPoolFailure InterruptPool::DestroySlotLocked(Slot& slot) noexcept
{
    // Authority transferred after placement does not allow an AP to acknowledge, mask or reprogram
    // an interrupt source.
    const auto cpu = Platform::Synchronization::CurrentProcessor();
    if (cpu.IsValid() && cpu.GetValue() != 0U)
    {
        return MakeFailure(ObjectPoolError::InvalidConfiguration);
    }
    const StopOutcome stopped = StopLocked(slot);
    if (stopped != StopOutcome::Stopped)
    {
        return MakeFailure((stopped == StopOutcome::Retry) ? ObjectPoolError::Busy : ObjectPoolError::DestroyRejected);
    }
    const Object::ObjectRegistryError destroyed = m_issuer.DestroyObject(slot.Identity);
    const Object::ObjectRegistryError released =
        (destroyed == Object::ObjectRegistryError::Ok) ? m_issuer.DestroyObject(slot.NotificationIdentity) : destroyed;
    if (released != Object::ObjectRegistryError::Ok)
    {
        return FaultLocked(MakeFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                                       Resource::ResourceDomainError::None, released));
    }
    slot.InterruptCollected = false;
    slot.NotificationCollected = false;
    slot.State = SlotState::Quiescing;
    m_quiescing = true;
    ++m_destroyed;
    return MakeFailure(ObjectPoolError::None);
}

// Nothing of the slot is reachable yet: no capability names either object
// and no WaitSet registered the Notification, so both collect at once.
void InterruptPool::AbandonLocked(Slot& slot) noexcept
{
    bool clean{true};
    if (slot.Instance != nullptr)
    {
        clean = !slot.Instance->IsInitialized() || (StopLocked(slot) == StopOutcome::Stopped);
        if (slot.Identity.IsValid())
        {
            clean = (m_issuer.DestroyObject(slot.Identity) == Object::ObjectRegistryError::Ok) &&
                    m_issuer.CollectDestroyedObject(slot.Identity).HasValue() && clean;
        }
    }
    if (slot.NotificationIdentity.IsValid())
    {
        clean = (m_issuer.DestroyObject(slot.NotificationIdentity) == Object::ObjectRegistryError::Ok) &&
                m_issuer.CollectDestroyedObject(slot.NotificationIdentity).HasValue() && clean;
    }
    if (!clean)
    {
        static_cast<void>(FaultLocked(MakeFailure(ObjectPoolError::InvariantViolation)));
        return;
    }
    ReleaseSlotLocked(slot);
}

// The function's INTx line (configuration byte 0x3C), reserved with a
// vector of its own; one Interrupt per line.
ObjectPoolFailure InterruptPool::ReserveLineLocked(Slot& slot) noexcept
{
    Foundation::UInt32 line{slot.GlobalSystemInterrupt};
    const Device::PciFunctionControlError routed = (slot.Function == nullptr)
                                                       ? Device::PciFunctionControlError::None
                                                       : m_functions->GetInterruptLine(*slot.Function, line);
    if (routed != Device::PciFunctionControlError::None)
    {
        return MapMessages(routed);
    }
    const Hal::Interrupt::LineReservation reservation = m_lines->Reserve(line);
    if (reservation.Source == nullptr)
    {
        return MakeFailure(ObjectPoolError::CapacityExceeded);
    }
    slot.Line = reservation.Source;
    slot.Vector = reservation.Vector;
    return MakeFailure(ObjectPoolError::None);
}

void InterruptPool::ReleaseSlotLocked(Slot& slot) noexcept
{
    if ((slot.Line != nullptr) && (slot.Vector != 0U))
    {
        m_lines->Release(slot.Vector);
    }
    else if ((slot.Vector != 0U) && (slot.Instance != nullptr))
    {
        static_cast<void>(m_vectors->Unbind(slot.Vector, *slot.Instance));
    }
    if (slot.Instance != nullptr)
    {
        slot.Instance->~Interrupt();
    }
    if (slot.Notification != nullptr)
    {
        slot.Notification->~Notification();
    }
    static_cast<void>(slot.Domain->Release(GetReservation()));
    slot.DomainPin.Reset();
    slot.LinePin.Reset();
    slot.Notification = nullptr;
    slot.Instance = nullptr;
    slot.Function = nullptr;
    slot.Domain = nullptr;
    slot.Identity = Object::ObjectId{};
    slot.NotificationIdentity = Object::ObjectId{};
    slot.Vector = 0U;
    slot.Line = nullptr;
    slot.MessagesEnabled = false;
    slot.InterruptCollected = false;
    slot.NotificationCollected = false;
    slot.State = SlotState::Free;
}

ObjectPoolFailure InterruptPool::CollectRegistrationLocked(const Object::ObjectId identity,
                                                           const Object::IKernelObject& instance,
                                                           bool& collected) noexcept
{
    const auto record = m_issuer.CollectDestroyedObject(identity);
    if (!record.HasValue() && (record.GetError() == Object::ObjectRegistryError::NoDestructionReady))
    {
        collected = false;
        return MakeFailure(ObjectPoolError::None);
    }
    if (!record.HasValue() || (record.GetValue().Instance != &instance))
    {
        return MakeFailure(ObjectPoolError::InvariantViolation, Object::CapabilityError::Ok,
                           Resource::ResourceDomainError::None,
                           record.HasValue() ? Object::ObjectRegistryError::Ok : record.GetError());
    }
    collected = true;
    return MakeFailure(ObjectPoolError::None);
}

ObjectPoolFailure InterruptPool::FaultLocked(const ObjectPoolFailure failure) noexcept
{
    m_faulted = true;
    ObjectPoolFailure faulted = failure;
    faulted.Error = ObjectPoolError::InvariantViolation;
    return faulted;
}
} // Namespace Asteria::Kernel::Interrupt
