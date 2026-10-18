// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/DeviceManager.hpp"

#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
[[nodiscard]] constexpr bool RangesOverlap(const DeviceRegionDescriptor& first,
                                           const DeviceRegionDescriptor& second) noexcept
{
    if (first.Kind != second.Kind)
    {
        return false;
    }
    const Foundation::UInt64 firstEnd = first.BaseAddress + first.LengthBytes;
    const Foundation::UInt64 secondEnd = second.BaseAddress + second.LengthBytes;
    return (first.BaseAddress < secondEnd) && (second.BaseAddress < firstEnd);
}
} // namespace

DeviceManagerFailure DeviceManager::MakeFailure(const DeviceManagerError error, const Object::ObjectId region,
                                                const DeviceRegionFailure regionFailure,
                                                const Object::ObjectRegistryError registryError,
                                                const Object::CapabilityError capabilityError) noexcept
{
    return DeviceManagerFailure{
        .Region = region,
        .RegionFailure = regionFailure,
        .RegistryError = registryError,
        .CapabilityError = capabilityError,
        .Error = error,
        .Reserved = {},
    };
}

Object::CapabilityRights DeviceManager::GetInitialRights(const DeviceRegionDescriptor& descriptor) noexcept
{
    Object::CapabilityRights rights{Object::CapabilityRight::Inspect};
    rights = rights.With(Object::CapabilityRight::Transfer);
    if (descriptor.Permissions.Contains(DeviceRegionPermission::Read))
    {
        rights = rights.With(Object::CapabilityRight::Read);
    }
    if (descriptor.Permissions.Contains(DeviceRegionPermission::Write))
    {
        rights = rights.With(Object::CapabilityRight::Write);
    }
    if (descriptor.Kind == DeviceRegionKind::MemoryMappedIo)
    {
        rights = rights.With(Object::CapabilityRight::Map);
    }
    return rights;
}

Foundation::Result<DeviceRegionCreation, DeviceManagerFailure>
DeviceManager::Create(const DeviceRegionCreateRequest& request) noexcept
{
    using ResultType = Foundation::Result<DeviceRegionCreation, DeviceManagerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceManagerState::Faulted)
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::Faulted));
    }
    if ((request.Destination == nullptr) || !request.Destination->IsReady() ||
        (request.ResourceDomainStorage == nullptr) || !request.ResourceDomainIdentity.IsValid() ||
        !request.ResourceDomainIdentity.IsFromRegistry(m_registry) || !request.RevocationDomain.IsValid())
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::InvalidConfiguration));
    }
    if (!DeviceRegion::DescriptorIsValid(request.Descriptor))
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::InvalidDescriptor));
    }
    if (OverlapsLocked(request.Descriptor))
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::RegionOverlap));
    }

    RegionSlot* const slot = FindFreeSlotLocked();
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::CapacityExceeded));
    }

    return CreateInSlotLocked(request, *slot);
}

Foundation::Result<DeviceRegionCreation, DeviceManagerFailure>
DeviceManager::CreateInSlotLocked(const DeviceRegionCreateRequest& request, RegionSlot& slot) noexcept
{
    using ResultType = Foundation::Result<DeviceRegionCreation, DeviceManagerFailure>;

    const DeviceRegionFailure regionFailure = slot.Region.Initialize(
        m_registry, request.ResourceDomainIdentity, *request.ResourceDomainStorage, request.Descriptor);
    if (regionFailure.Error != DeviceRegionError::None)
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::RegionRejected, {}, regionFailure));
    }
    slot.Descriptor = request.Descriptor;

    const auto registered = m_issuer.CreateObject(slot.Region);
    if (!registered.HasValue())
    {
        const DeviceRegionFailure quiesce = slot.Region.Quiesce();
        const DeviceRegionError recycle = (quiesce.Error == DeviceRegionError::None)
                                              ? slot.Region.ResetForDeviceManagerReuse()
                                              : DeviceRegionError::InvariantViolation;
        if ((quiesce.Error != DeviceRegionError::None) || (recycle != DeviceRegionError::None))
        {
            EnterFaultedLocked(slot);
            return ResultType::Failure(
                MakeFailure(DeviceManagerError::RecycleRejected, {}, quiesce, registered.GetError()));
        }
        slot.Descriptor = DeviceRegionDescriptor{};
        return ResultType::Failure(
            MakeFailure(DeviceManagerError::RegistrationRejected, {}, DeviceRegionFailure{}, registered.GetError()));
    }

    slot.Identity = registered.GetValue();
    slot.State = SlotState::Active;
    const auto capability = m_issuer.Mint(*request.Destination, slot.Identity, GetInitialRights(request.Descriptor),
                                          request.RevocationDomain, request.LeaseDeadline, 0U);
    if (!capability.HasValue())
    {
        const Object::ObjectRegistryError rollback = m_issuer.DestroyObject(slot.Identity);
        if (rollback != Object::ObjectRegistryError::Ok)
        {
            EnterFaultedLocked(slot);
            return ResultType::Failure(MakeFailure(DeviceManagerError::RollbackRejected, slot.Identity,
                                                   DeviceRegionFailure{}, rollback, capability.GetError()));
        }
        slot.State = SlotState::Quiescing;
        return ResultType::Failure(MakeFailure(DeviceManagerError::CapabilityRejected, slot.Identity,
                                               DeviceRegionFailure{}, Object::ObjectRegistryError::Ok,
                                               capability.GetError()));
    }

    return ResultType::Success(DeviceRegionCreation{
        .Storage = &slot.Region,
        .Region = DeviceRegionId{m_registry, slot.Identity},
        .Capability = capability.GetValue(),
    });
}

Foundation::Result<Object::ObjectId, DeviceManagerFailure> DeviceManager::Retire(const DeviceRegionId region) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, DeviceManagerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceManagerState::Faulted)
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::Faulted, region.GetObjectId()));
    }
    if (!region.IsValid() || !region.IsFromRegistry(m_registry))
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::InvalidRegion, region.GetObjectId()));
    }
    RegionSlot* const slot = FindSlotLocked(region);
    if (slot == nullptr)
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::RegionNotOwned, region.GetObjectId()));
    }
    if (slot->State != SlotState::Active)
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::RegionBusy, region.GetObjectId()));
    }

    const Object::ObjectRegistryError destroy = m_issuer.DestroyObject(slot->Identity);
    if (destroy != Object::ObjectRegistryError::Ok)
    {
        return ResultType::Failure(
            MakeFailure(DeviceManagerError::DestroyRejected, slot->Identity, DeviceRegionFailure{}, destroy));
    }
    slot->State = SlotState::Quiescing;
    return ResultType::Success(slot->Identity);
}

Foundation::Result<Object::ObjectId, DeviceManagerFailure>
DeviceManager::Reclaim(const Object::ObjectDestructionRecord& record) noexcept
{
    using ResultType = Foundation::Result<Object::ObjectId, DeviceManagerFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceManagerState::Faulted)
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::Faulted, record.Object));
    }
    if (!record.IsFromRegistry(m_registry) || !record.Object.IsValid() || (record.Instance == nullptr))
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::InvalidDestructionRecord, record.Object));
    }
    RegionSlot* const slot = FindSlotLocked(record.Instance);
    if ((slot == nullptr) || (slot->State != SlotState::Quiescing) ||
        (slot->Identity.GetValue() != record.Object.GetValue()))
    {
        return ResultType::Failure(MakeFailure(DeviceManagerError::InvalidDestructionRecord, record.Object));
    }

    const DeviceRegionFailure quiesce = slot->Region.Quiesce();
    if (quiesce.Error != DeviceRegionError::None)
    {
        EnterFaultedLocked(*slot);
        return ResultType::Failure(MakeFailure(DeviceManagerError::RecycleRejected, record.Object, quiesce));
    }
    const DeviceRegionError recycle = slot->Region.ResetForDeviceManagerReuse();
    if (recycle != DeviceRegionError::None)
    {
        EnterFaultedLocked(*slot);
        return ResultType::Failure(MakeFailure(DeviceManagerError::RecycleRejected, record.Object,
                                               DeviceRegionFailure{
                                                   .Error = recycle,
                                                   .Pin = Object::ObjectPinError::Ok,
                                                   .Resource = Resource::ResourceDomainError::None,
                                                   .Reserved = {},
                                               }));
    }

    const Object::ObjectId reclaimed = slot->Identity;
    slot->Identity = Object::ObjectId{};
    slot->Descriptor = DeviceRegionDescriptor{};
    slot->State = SlotState::Free;
    return ResultType::Success(reclaimed);
}

DeviceManagerSnapshot DeviceManager::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    DeviceManagerSnapshot snapshot{
        .FreeSlots = 0U,
        .ActiveSlots = 0U,
        .QuiescingSlots = 0U,
        .FaultedSlots = 0U,
        .State = m_state,
        .Reserved = {},
    };
    for (const RegionSlot& slot : m_regionSlots)
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
        case SlotState::Faulted:
            ++snapshot.FaultedSlots;
            break;
        }
    }
    return snapshot;
}

bool DeviceManager::OverlapsLocked(const DeviceRegionDescriptor& descriptor) const noexcept
{
    for (const RegionSlot& slot : m_regionSlots)
    {
        if ((slot.State != SlotState::Free) && RangesOverlap(descriptor, slot.Descriptor))
        {
            return true;
        }
    }
    return false;
}

DeviceManager::RegionSlot* DeviceManager::FindFreeSlotLocked() noexcept
{
    for (RegionSlot& slot : m_regionSlots)
    {
        if (slot.State == SlotState::Free)
        {
            return &slot;
        }
    }
    return nullptr;
}

DeviceManager::RegionSlot* DeviceManager::FindSlotLocked(const DeviceRegionId region) noexcept
{
    for (RegionSlot& slot : m_regionSlots)
    {
        if ((slot.State != SlotState::Free) && (slot.Identity.GetValue() == region.GetValue()))
        {
            return &slot;
        }
    }
    return nullptr;
}

DeviceManager::RegionSlot* DeviceManager::FindSlotLocked(Object::IKernelObject* const instance) noexcept
{
    for (RegionSlot& slot : m_regionSlots)
    {
        if (static_cast<Object::IKernelObject*>(&slot.Region) == instance)
        {
            return &slot;
        }
    }
    return nullptr;
}

void DeviceManager::EnterFaultedLocked(RegionSlot& slot) noexcept
{
    slot.State = SlotState::Faulted;
    m_state = DeviceManagerState::Faulted;
}
} // namespace Asteria::Kernel::Device
