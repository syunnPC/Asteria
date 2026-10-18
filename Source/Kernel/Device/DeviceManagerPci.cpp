// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/DeviceManager.hpp"

#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
[[nodiscard]] constexpr PciFunctionFailure MakeFunctionFailure(const PciFunctionError error) noexcept
{
    return PciFunctionFailure{
        .Error = error,
        .Pin = Object::ObjectPinError::Ok,
        .Resource = Resource::ResourceDomainError::None,
        .Driver = Protection::ProtectionDomainError::None,
        .Reserved = {},
    };
}
}

PciFunctionManagerFailure DeviceManager::MakePciFunctionFailure(const PciFunctionManagerError error,
                                                                const Object::ObjectId function,
                                                                const PciFunctionCatalogFailure catalogFailure,
                                                                const PciFunctionFailure functionFailure,
                                                                const Object::ObjectRegistryError registryError,
                                                                const Object::CapabilityError capabilityError) noexcept
{
    return PciFunctionManagerFailure{
        .Function = function,
        .CatalogFailure = catalogFailure,
        .FunctionFailure = functionFailure,
        .RegistryError = registryError,
        .CapabilityError = capabilityError,
        .Error = error,
        .Reserved = {},
    };
}

constexpr Object::CapabilityRights DeviceManager::GetPciFunctionInitialRights() noexcept
{
    return Object::CapabilityRights{Object::CapabilityRight::Inspect}
        .With(Object::CapabilityRight::Manage)
        .With(Object::CapabilityRight::Transfer);
}

PciFunctionManagerError DeviceManager::BindPciFunctionCatalog(const PciFunctionCatalog& catalog) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceManagerState::Faulted)
    {
        return PciFunctionManagerError::Faulted;
    }
    if (m_pciCatalog != nullptr)
    {
        return PciFunctionManagerError::CatalogAlreadyBound;
    }
    if (catalog.GetSnapshot().State != PciFunctionCatalogState::Ready)
    {
        return PciFunctionManagerError::CatalogRejected;
    }

    m_pciCatalog = &catalog;
    return PciFunctionManagerError::None;
}

Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
DeviceManager::CreatePciFunction(const PciFunctionCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceManagerState::Faulted)
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::Faulted));
    }
    if (m_pciCatalog == nullptr)
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::CatalogNotBound));
    }
    if ((request.Destination == nullptr) || !request.Destination->IsReady() ||
        (request.ResourceDomainStorage == nullptr) || !request.ResourceDomainIdentity.IsValid() ||
        !request.ResourceDomainIdentity.IsFromRegistry(m_registry) || !request.RevocationDomain.IsValid() ||
        !request.Address.IsValid() || (request.Reserved[0U] != 0U) || (request.Reserved[1U] != 0U))
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::InvalidConfiguration));
    }

    return CreatePciFunctionFromCatalogLocked(request);
}

Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
DeviceManager::CreatePciFunctionFromCatalogLocked(const PciFunctionCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>;

    const auto descriptor = m_pciCatalog->FindFunction(request.Address);
    if (!descriptor.HasValue())
    {
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::CatalogRejected, {}, descriptor.GetError()));
    }
    if (HasPciFunctionLocked(request.Address))
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::FunctionAlreadyOwned));
    }

    PciSlot* const slot = FindFreePciSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::CapacityExceeded));
    }

    return CreatePciFunctionInSlotLocked(request, descriptor.GetValue(), *slot);
}

Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
DeviceManager::CreatePciFunctionInSlotLocked(const PciFunctionCreateRequest& request,
                                             const PciFunctionDescriptor& descriptor, PciSlot& slot) noexcept
{
    using ResultType = Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>;

    const PciFunctionFailure initialized = slot.Function.InitializeForDeviceManager(
        m_registry, request.ResourceDomainIdentity, *request.ResourceDomainStorage, descriptor);
    if (initialized.Error != PciFunctionError::None)
    {
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::FunctionRejected, {}, {}, initialized));
    }
    slot.Descriptor = descriptor;

    const auto registered = m_issuer.CreateObject(slot.Function);
    if (!registered.HasValue())
    {
        return RecycleUnregisteredPciFunctionLocked(slot, registered.GetError());
    }

    slot.Identity = registered.GetValue();
    slot.State = PciSlotState::Active;
    return MintPciFunctionLocked(request, slot);
}

Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
DeviceManager::RecycleUnregisteredPciFunctionLocked(PciSlot& slot,
                                                    const Object::ObjectRegistryError registrationError) noexcept
{
    using ResultType = Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>;

    const PciFunctionFailure quiesce = slot.Function.QuiesceAvailableForDeviceManager();
    const PciFunctionError recycle = (quiesce.Error == PciFunctionError::None)
                                         ? slot.Function.ResetForDeviceManagerReuse()
                                         : PciFunctionError::CorruptState;
    if ((quiesce.Error != PciFunctionError::None) ||
        ((recycle != PciFunctionError::None) && (recycle != PciFunctionError::AssignmentSequenceExhausted)))
    {
        EnterFaultedLocked(slot);
        return ResultType::Failure(MakePciFunctionFailure(
            PciFunctionManagerError::RecycleRejected, {}, {},
            (quiesce.Error == PciFunctionError::None) ? MakeFunctionFailure(recycle) : quiesce, registrationError));
    }
    slot.Descriptor = PciFunctionDescriptor{};
    slot.State =
        (recycle == PciFunctionError::AssignmentSequenceExhausted) ? PciSlotState::Retired : PciSlotState::Free;
    return ResultType::Failure(
        MakePciFunctionFailure(PciFunctionManagerError::RegistrationRejected, {}, {}, {}, registrationError));
}

Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
DeviceManager::MintPciFunctionLocked(const PciFunctionCreateRequest& request, PciSlot& slot) noexcept
{
    using ResultType = Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>;

    const auto capability = m_issuer.Mint(*request.Destination, slot.Identity, GetPciFunctionInitialRights(),
                                          request.RevocationDomain, request.LeaseDeadline, 0U);
    if (!capability.HasValue())
    {
        const PciFunctionError retirement = slot.Function.BeginDeviceManagerRetirement();
        const Object::ObjectRegistryError rollback = (retirement == PciFunctionError::None)
                                                         ? m_issuer.DestroyObject(slot.Identity)
                                                         : Object::ObjectRegistryError::InvalidObject;
        if ((retirement != PciFunctionError::None) || (rollback != Object::ObjectRegistryError::Ok))
        {
            const PciFunctionError cancellation =
                (retirement == PciFunctionError::None) ? slot.Function.CancelDeviceManagerRetirement() : retirement;
            EnterFaultedLocked(slot);
            return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::RollbackRejected, slot.Identity,
                                                              {}, MakeFunctionFailure(cancellation), rollback,
                                                              capability.GetError()));
        }
        slot.State = PciSlotState::Quiescing;
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::CapabilityRejected, slot.Identity,
                                                          {}, {}, Object::ObjectRegistryError::Ok,
                                                          capability.GetError()));
    }

    return ResultType::Success(PciFunctionCreation{
        .Function = PciFunctionId{m_registry, slot.Identity},
        .Capability = capability.GetValue(),
    });
}

Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>
DeviceManager::RetirePciFunction(const PciFunctionId function) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceManagerState::Faulted)
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::Faulted, function.GetObjectId()));
    }
    if (!function.IsValid() || !function.IsFromRegistry(m_registry))
    {
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::InvalidFunction, function.GetObjectId()));
    }
    PciSlot* const slot = FindPciSlotLocked(function);
    if (slot == nullptr)
    {
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::FunctionNotOwned, function.GetObjectId()));
    }
    if (slot->State != PciSlotState::Active)
    {
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::FunctionBusy, function.GetObjectId()));
    }

    return RetirePciFunctionLocked(*slot);
}

Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>
DeviceManager::RetirePciFunctionLocked(PciSlot& slot) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>;

    const PciFunctionError retirement = slot.Function.BeginDeviceManagerRetirement();
    if (retirement == PciFunctionError::AssignmentBusy)
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::FunctionBusy, slot.Identity, {},
                                                          MakeFunctionFailure(retirement)));
    }
    if (retirement != PciFunctionError::None)
    {
        EnterFaultedLocked(slot);
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::RecycleRejected, slot.Identity, {},
                                                          MakeFunctionFailure(retirement)));
    }

    const Object::ObjectRegistryError destroy = m_issuer.DestroyObject(slot.Identity);
    if (destroy != Object::ObjectRegistryError::Ok)
    {
        const PciFunctionError cancellation = slot.Function.CancelDeviceManagerRetirement();
        if (cancellation != PciFunctionError::None)
        {
            EnterFaultedLocked(slot);
            return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::RecycleRejected, slot.Identity,
                                                              {}, MakeFunctionFailure(cancellation), destroy));
        }
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::DestroyRejected, slot.Identity, {}, {}, destroy));
    }

    slot.State = PciSlotState::Quiescing;
    return ResultType::Success(slot.Identity);
}

Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>
DeviceManager::ReclaimPciFunction(const Object::ObjectDestructionRecord& record) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceManagerState::Faulted)
    {
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::Faulted, record.Object));
    }
    if (!record.IsFromRegistry(m_registry) || !record.Object.IsValid() || (record.Instance == nullptr))
    {
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::InvalidDestructionRecord, record.Object));
    }
    PciSlot* const slot = FindPciSlotLocked(record.Instance);
    if ((slot == nullptr) || (slot->State != PciSlotState::Quiescing) ||
        (slot->Identity.GetValue() != record.Object.GetValue()))
    {
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::InvalidDestructionRecord, record.Object));
    }

    const PciFunctionFailure quiesce = slot->Function.QuiesceRetiringForDeviceManager();
    if (quiesce.Error != PciFunctionError::None)
    {
        EnterFaultedLocked(*slot);
        return ResultType::Failure(
            MakePciFunctionFailure(PciFunctionManagerError::RecycleRejected, record.Object, {}, quiesce));
    }
    const PciFunctionError recycle = slot->Function.ResetForDeviceManagerReuse();
    if ((recycle != PciFunctionError::None) && (recycle != PciFunctionError::AssignmentSequenceExhausted))
    {
        EnterFaultedLocked(*slot);
        return ResultType::Failure(MakePciFunctionFailure(PciFunctionManagerError::RecycleRejected, record.Object, {},
                                                          MakeFunctionFailure(recycle)));
    }

    const Object::ObjectId reclaimed = slot->Identity;
    slot->Identity = Object::ObjectId{};
    slot->Descriptor = PciFunctionDescriptor{};
    slot->State =
        (recycle == PciFunctionError::AssignmentSequenceExhausted) ? PciSlotState::Retired : PciSlotState::Free;
    return ResultType::Success(reclaimed);
}

bool DeviceManager::ReserveFirmwareDma(const PciFunctionAddress address) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (PciSlot& slot : m_pciSlots)
    {
        if ((slot.State == PciSlotState::Active) && slot.Descriptor.Address.IsEqualTo(address))
        {
            return slot.Function.ReserveFirmwareDma();
        }
    }
    return false;
}

PciFunctionError DeviceManager::GetAssignmentRestriction(const PciFunctionAddress address) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    for (PciSlot& slot : m_pciSlots)
    {
        if ((slot.State == PciSlotState::Active) && slot.Descriptor.Address.IsEqualTo(address))
        {
            return slot.Function.GetAssignmentRestriction();
        }
    }
    return PciFunctionError::InvalidConfiguration;
}

PciFunctionManagerSnapshot DeviceManager::GetPciFunctionSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    PciFunctionManagerSnapshot snapshot{
        .FreeSlots = 0U,
        .ActiveSlots = 0U,
        .QuiescingSlots = 0U,
        .RetiredSlots = 0U,
        .FaultedSlots = 0U,
        .State = m_state,
        .CatalogBound = m_pciCatalog != nullptr,
        .Reserved = {},
    };
    for (const PciSlot& slot : m_pciSlots)
    {
        switch (slot.State)
        {
        case PciSlotState::Free:
            ++snapshot.FreeSlots;
            break;
        case PciSlotState::Active:
            ++snapshot.ActiveSlots;
            break;
        case PciSlotState::Quiescing:
            ++snapshot.QuiescingSlots;
            break;
        case PciSlotState::Retired:
            ++snapshot.RetiredSlots;
            break;
        case PciSlotState::Faulted:
            ++snapshot.FaultedSlots;
            break;
        }
    }
    return snapshot;
}

bool DeviceManager::HasPciFunctionLocked(const PciFunctionAddress address) const noexcept
{
    for (const PciSlot& slot : m_pciSlots)
    {
        if ((slot.State != PciSlotState::Free) && (slot.State != PciSlotState::Retired) &&
            slot.Descriptor.Address.IsEqualTo(address))
        {
            return true;
        }
    }
    return false;
}

DeviceManager::PciSlot* DeviceManager::FindFreePciSlotLocked() noexcept
{
    for (PciSlot& slot : m_pciSlots)
    {
        if (slot.State == PciSlotState::Free)
        {
            return &slot;
        }
    }
    return nullptr;
}

DeviceManager::PciSlot* DeviceManager::FindPciSlotLocked(const PciFunctionId function) noexcept
{
    for (PciSlot& slot : m_pciSlots)
    {
        if ((slot.State != PciSlotState::Free) && (slot.State != PciSlotState::Retired) &&
            (slot.Identity.GetValue() == function.GetValue()))
        {
            return &slot;
        }
    }
    return nullptr;
}

DeviceManager::PciSlot* DeviceManager::FindPciSlotLocked(Object::IKernelObject* const instance) noexcept
{
    for (PciSlot& slot : m_pciSlots)
    {
        if (static_cast<Object::IKernelObject*>(&slot.Function) == instance)
        {
            return &slot;
        }
    }
    return nullptr;
}

void DeviceManager::EnterFaultedLocked(PciSlot& slot) noexcept
{
    slot.State = PciSlotState::Faulted;
    m_state = DeviceManagerState::Faulted;
}
} // namespace Asteria::Kernel::Device
