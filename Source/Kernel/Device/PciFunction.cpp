// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/PciFunction.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Device
{
PciFunctionFailure PciFunction::MakeFailure(const PciFunctionError error, const Object::ObjectPinError pin,
                                            const Resource::ResourceDomainError resource,
                                            const Protection::ProtectionDomainError driver) noexcept
{
    return PciFunctionFailure{
        .Error = error,
        .Pin = pin,
        .Resource = resource,
        .Driver = driver,
        .Reserved = {},
    };
}

constexpr Resource::ResourceReservation PciFunction::GetResourceReservation() noexcept
{
    return Resource::ResourceReservation{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = sizeof(PciFunction),
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
    };
}

bool PciFunction::DescriptorIsValid(const PciFunctionDescriptor& descriptor) noexcept
{
    return descriptor.Address.IsValid() && (descriptor.VendorId != 0xFFFFU) &&
           ((descriptor.HeaderLayout & 0x80U) == 0U);
}

PciFunctionFailure PciFunction::Initialize(Object::ObjectRegistry& registry,
                                           const Resource::ResourceDomainObjectId resourceDomainIdentity,
                                           Resource::ResourceDomain& resourceDomain,
                                           const PciFunctionDescriptor& descriptor) noexcept
{
    return InitializeInternal(registry, resourceDomainIdentity, resourceDomain, descriptor, false);
}

PciFunctionFailure PciFunction::InitializeForDeviceManager(
    Object::ObjectRegistry& registry, const Resource::ResourceDomainObjectId resourceDomainIdentity,
    Resource::ResourceDomain& resourceDomain, const PciFunctionDescriptor& descriptor) noexcept
{
    return InitializeInternal(registry, resourceDomainIdentity, resourceDomain, descriptor, true);
}

PciFunctionFailure PciFunction::InitializeInternal(Object::ObjectRegistry& registry,
                                                   const Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                   Resource::ResourceDomain& resourceDomain,
                                                   const PciFunctionDescriptor& descriptor,
                                                   const bool deviceManagerOwned) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PciFunctionState::Uninitialized)
    {
        return MakeFailure(PciFunctionError::AlreadyInitialized);
    }
    if (!DescriptorIsValid(descriptor))
    {
        return MakeFailure(PciFunctionError::InvalidDescriptor);
    }
    const Object::ResourceDomainId resourceDomainIdentifier = resourceDomain.GetIdentifier();
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentifier.IsValid())
    {
        return MakeFailure(PciFunctionError::InvalidConfiguration);
    }
    if (!resourceDomainIdentity.IsFromRegistry(registry))
    {
        return MakeFailure(PciFunctionError::ResourceDomainRegistryMismatch);
    }

    const Object::ObjectPinError pinError = registry.PinObject(resourceDomainIdentity.GetObjectId(),
                                                               Object::ObjectType::ResourceDomain, m_resourceDomainPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(PciFunctionError::ResourceDomainPinRejected, pinError);
    }
    if (&m_resourceDomainPin.GetObject() != static_cast<Object::IKernelObject*>(&resourceDomain))
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(PciFunctionError::ResourceDomainInstanceMismatch);
    }

    const Resource::ResourceDomainError resourceError = resourceDomain.TryReserve(GetResourceReservation());
    if (resourceError != Resource::ResourceDomainError::None)
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(PciFunctionError::ResourceDomainRejected, Object::ObjectPinError::Ok, resourceError);
    }

    m_registry = &registry;
    m_resourceDomain = &resourceDomain;
    m_descriptor = descriptor;
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_resourceDomainIdentifier = resourceDomainIdentifier;
    m_deviceManagerOwned = deviceManagerOwned;
    m_state = PciFunctionState::Available;
    m_chargeOwnerValue.StoreRelease(resourceDomainIdentifier.GetValue());
    return MakeFailure(PciFunctionError::None);
}

bool PciFunction::ReserveFirmwareDma() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != PciFunctionState::Available) || (m_iommuOwner != nullptr))
    {
        return false;
    }
    m_firmwareDma = true;
    return true;
}

bool PciFunction::GrantTrustedDma() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != PciFunctionState::Available) || (m_iommuOwner != nullptr) || m_firmwareDma ||
        (m_descriptor.HeaderLayout == 1U) || (m_descriptor.ClassCode == 3U))
    {
        return false;
    }
    m_trustedDma = true;
    return true;
}

bool PciFunction::HasTrustedDma() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_trustedDma;
}

PciFunctionError PciFunction::GetAssignmentRestriction() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_descriptor.HeaderLayout == 1U)
    {
        return PciFunctionError::BridgeReserved;
    }
    if (m_descriptor.ClassCode == 3U)
    {
        return PciFunctionError::DisplayReserved;
    }
    return m_firmwareDma ? PciFunctionError::ReservedMemory : PciFunctionError::None;
}

Foundation::Result<PciFunctionAssignmentReceipt, PciFunctionFailure>
PciFunction::Assign(const Protection::ProtectionDomainId driver, Protection::ProtectionDomain& driverStorage) noexcept
{
    using ResultType = Foundation::Result<PciFunctionAssignmentReceipt, PciFunctionFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == PciFunctionState::Assigned)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::AlreadyAssigned));
    }
    if (m_state == PciFunctionState::Quiesced)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::Quiesced));
    }
    if (m_state == PciFunctionState::Retiring)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::RetirementPending));
    }
    if ((m_state != PciFunctionState::Available) || (m_registry == nullptr))
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::NotReady));
    }
    if (m_quarantined)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::Quarantined));
    }
    if (m_descriptor.HeaderLayout == 1U)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::BridgeReserved));
    }
    if (m_descriptor.ClassCode == 3U)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::DisplayReserved));
    }
    if (m_firmwareDma)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::ReservedMemory));
    }
    if (!driver.IsValid())
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::InvalidDriver));
    }
    if (!driver.IsFromRegistry(*m_registry))
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::DriverRegistryMismatch));
    }

    Foundation::UInt64 nextSequence{0U};
    if (!PciFunctionAssignmentSequencePolicy::TryAdvance(m_lastAssignmentSequence, nextSequence))
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::AssignmentSequenceExhausted));
    }
    const Object::ObjectPinError pinError =
        m_registry->PinObject(driver.GetObjectId(), Object::ObjectType::ProtectionDomain, m_driverPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return ResultType::Failure(MakeFailure(PciFunctionError::DriverPinRejected, pinError));
    }
    if (&m_driverPin.GetObject() != static_cast<Object::IKernelObject*>(&driverStorage))
    {
        m_driverPin.Reset();
        return ResultType::Failure(MakeFailure(PciFunctionError::DriverInstanceMismatch));
    }
    const Protection::ProtectionDomainError driverError = driverStorage.RetainDeviceAssignment();
    if (driverError != Protection::ProtectionDomainError::None)
    {
        m_driverPin.Reset();
        return ResultType::Failure(MakeFailure(PciFunctionError::DriverStateRejected, Object::ObjectPinError::Ok,
                                               Resource::ResourceDomainError::None, driverError));
    }

    m_driver = driver;
    m_driverStorage = &driverStorage;
    m_lastAssignmentSequence = nextSequence;
    m_state = PciFunctionState::Assigned;
    return ResultType::Success(PciFunctionAssignmentReceipt{
        .Assignment = PciFunctionAssignmentToken{*this, nextSequence},
        .Driver = driver,
    });
}

PciFunctionFailure PciFunction::ReleaseAssignment(const PciFunctionAssignmentToken assignment,
                                                  const bool quarantine) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!assignment.IsValid() || !assignment.IsFromFunction(*this))
    {
        return MakeFailure(PciFunctionError::InvalidAssignment);
    }
    if ((m_state != PciFunctionState::Assigned) || (assignment.GetSequence() != m_lastAssignmentSequence))
    {
        return MakeFailure(PciFunctionError::StaleAssignment);
    }
    if (m_iommuOwner != nullptr)
    {
        return MakeFailure(PciFunctionError::IommuStillClaimed);
    }
    if (!m_driverPin.IsValid() || !m_driver.IsValid() || (m_driverStorage == nullptr))
    {
        return MakeFailure(PciFunctionError::CorruptState);
    }

    const Protection::ProtectionDomainError driverError = m_driverStorage->ReleaseDeviceAssignment();
    if (driverError != Protection::ProtectionDomainError::None)
    {
        return MakeFailure(PciFunctionError::DriverStateRejected, Object::ObjectPinError::Ok,
                           Resource::ResourceDomainError::None, driverError);
    }

    m_driverStorage = nullptr;
    m_driverPin.Reset();
    m_driver = Protection::ProtectionDomainId{};
    m_state = PciFunctionState::Available;
    m_quarantined = quarantine;
    return MakeFailure(PciFunctionError::None);
}

PciFunctionFailure PciFunction::LiftQuarantine(const Foundation::UInt64 sequence) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_quarantined)
    {
        return MakeFailure(PciFunctionError::NotQuarantined);
    }
    if ((sequence == 0U) || (sequence != m_lastAssignmentSequence))
    {
        return MakeFailure(PciFunctionError::StaleAssignment);
    }
    m_quarantined = false;
    return MakeFailure(PciFunctionError::None);
}

PciFunctionFailure PciFunction::Quiesce() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_deviceManagerOwned)
    {
        return MakeFailure(PciFunctionError::ManagerOwned);
    }
    return QuiesceLocked(PciFunctionState::Available);
}

PciFunctionFailure PciFunction::QuiesceAvailableForDeviceManager() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_deviceManagerOwned)
    {
        return MakeFailure(PciFunctionError::CorruptState);
    }
    return QuiesceLocked(PciFunctionState::Available);
}

PciFunctionFailure PciFunction::QuiesceRetiringForDeviceManager() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_deviceManagerOwned)
    {
        return MakeFailure(PciFunctionError::CorruptState);
    }
    return QuiesceLocked(PciFunctionState::Retiring);
}

PciFunctionFailure PciFunction::QuiesceLocked(const PciFunctionState expectedState) noexcept
{
    if (m_state == PciFunctionState::Uninitialized)
    {
        return MakeFailure(PciFunctionError::NotReady);
    }
    if (m_state == PciFunctionState::Quiesced)
    {
        return MakeFailure(PciFunctionError::Quiesced);
    }
    if (m_state == PciFunctionState::Assigned)
    {
        return MakeFailure(PciFunctionError::AssignmentBusy);
    }
    if ((m_state != expectedState) || (m_registry == nullptr) || (m_resourceDomain == nullptr) ||
        !m_resourceDomainPin.IsValid() || m_driverPin.IsValid() || (m_driverStorage != nullptr) ||
        (m_iommuOwner != nullptr))
    {
        return MakeFailure(PciFunctionError::CorruptState);
    }

    const Resource::ResourceDomainError resourceError = m_resourceDomain->Release(GetResourceReservation());
    if (resourceError != Resource::ResourceDomainError::None)
    {
        return MakeFailure(PciFunctionError::ResourceDomainRejected, Object::ObjectPinError::Ok, resourceError);
    }

    m_resourceDomain = nullptr;
    m_resourceDomainPin.Reset();
    m_chargeOwnerValue.StoreRelease(0U);
    m_state = PciFunctionState::Quiesced;
    return MakeFailure(PciFunctionError::None);
}

PciFunctionError PciFunction::BeginDeviceManagerRetirement() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_deviceManagerOwned)
    {
        return PciFunctionError::CorruptState;
    }
    if (m_state == PciFunctionState::Assigned)
    {
        return PciFunctionError::AssignmentBusy;
    }
    if (m_state == PciFunctionState::Quiesced)
    {
        return PciFunctionError::Quiesced;
    }
    if ((m_state != PciFunctionState::Available) || (m_registry == nullptr) || !m_resourceDomainPin.IsValid() ||
        (m_resourceDomain == nullptr) || m_driverPin.IsValid() || (m_driverStorage != nullptr) ||
        (m_iommuOwner != nullptr))
    {
        return PciFunctionError::CorruptState;
    }

    m_state = PciFunctionState::Retiring;
    return PciFunctionError::None;
}

PciFunctionError PciFunction::CancelDeviceManagerRetirement() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_deviceManagerOwned || (m_state != PciFunctionState::Retiring) || (m_registry == nullptr) ||
        !m_resourceDomainPin.IsValid() || (m_resourceDomain == nullptr) || m_driverPin.IsValid() ||
        (m_driverStorage != nullptr) || (m_iommuOwner != nullptr))
    {
        return PciFunctionError::CorruptState;
    }

    m_state = PciFunctionState::Available;
    return PciFunctionError::None;
}

PciFunctionError PciFunction::ResetForDeviceManagerReuse() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_deviceManagerOwned || (m_state != PciFunctionState::Quiesced) || (m_registry == nullptr) ||
        m_resourceDomainPin.IsValid() || m_driverPin.IsValid() || (m_resourceDomain != nullptr) ||
        (m_driverStorage != nullptr) || (m_iommuOwner != nullptr) || m_driver.IsValid() ||
        (m_chargeOwnerValue.LoadAcquire() != 0U))
    {
        return PciFunctionError::CorruptState;
    }
    if (m_lastAssignmentSequence == PciFunctionAssignmentSequencePolicy::Maximum)
    {
        return PciFunctionError::AssignmentSequenceExhausted;
    }

    m_registry = nullptr;
    m_descriptor = PciFunctionDescriptor{};
    m_firmwareDma = false;
    m_trustedDma = false;
    m_resourceDomainIdentity = Resource::ResourceDomainObjectId{};
    m_resourceDomainIdentifier = Object::ResourceDomainId{};
    m_deviceManagerOwned = false;
    m_state = PciFunctionState::Uninitialized;
    return PciFunctionError::None;
}

bool PciFunction::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return (m_state == PciFunctionState::Available) || (m_state == PciFunctionState::Assigned);
}

PciFunctionSnapshot PciFunction::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return PciFunctionSnapshot{
        .Descriptor = m_descriptor,
        .Driver = m_driver,
        .LastAssignmentSequence = m_lastAssignmentSequence,
        .State = m_state,
        .IommuClaimed = m_iommuOwner != nullptr,
        .Quarantined = m_quarantined,
        .Reserved = {},
    };
}

Hal::Device::IommuDeviceError PciFunction::Claim(const IommuDomain& owner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != PciFunctionState::Assigned)
    {
        return Hal::Device::IommuDeviceError::NotReady;
    }
    if (!m_driverPin.IsValid() || !m_driver.IsValid() || (m_driverStorage == nullptr))
    {
        return Hal::Device::IommuDeviceError::CorruptState;
    }
    if (m_iommuOwner != nullptr)
    {
        return Hal::Device::IommuDeviceError::AlreadyClaimed;
    }
    m_iommuOwner = &owner;
    return Hal::Device::IommuDeviceError::None;
}

Hal::Device::IommuDeviceError PciFunction::Release(const IommuDomain& owner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_iommuOwner != &owner)
    {
        return Hal::Device::IommuDeviceError::InvalidOwner;
    }
    if ((m_state != PciFunctionState::Assigned) || !m_driverPin.IsValid() || !m_driver.IsValid() ||
        (m_driverStorage == nullptr))
    {
        return Hal::Device::IommuDeviceError::CorruptState;
    }
    m_iommuOwner = nullptr;
    return Hal::Device::IommuDeviceError::None;
}

Foundation::UInt32 PciFunction::GetRequesterId() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const PciFunctionAddress address = m_descriptor.Address;
    return (static_cast<Foundation::UInt32>(address.GetSegment()) << 16U) |
           (address.IsValid() ? address.GetRequesterId() : 0U);
}

Object::ObjectType PciFunction::GetObjectType() const noexcept
{
    return Object::ObjectType::PciFunction;
}

Object::ResourceDomainId PciFunction::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}
} // namespace Asteria::Kernel::Device
