// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Protection/ProtectionDomain.hpp"

#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Protection
{
ProtectionDomainFailure ProtectionDomain::MakeFailure(const ProtectionDomainError error,
                                                      const Object::ObjectPinError pin,
                                                      const Resource::ResourceDomainError resource) noexcept
{
    return ProtectionDomainFailure{
        .Error = error,
        .Pin = pin,
        .Resource = resource,
        .Reserved = {},
    };
}

constexpr Resource::ResourceReservation ProtectionDomain::GetResourceReservation() noexcept
{
    return Resource::ResourceReservation{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = sizeof(ProtectionDomain),
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .Objects = {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

ProtectionDomainFailure ProtectionDomain::Initialize(Object::ObjectRegistry& registry,
                                                     const Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                     Resource::ResourceDomain& resourceDomain,
                                                     const Memory::AddressSpaceId addressSpace,
                                                     Object::CapabilitySpace& capabilities) noexcept
{
    return InitializeAssociation(registry, resourceDomainIdentity, resourceDomain, addressSpace, &capabilities,
                                 ProtectionDomainKind::UserCapable);
}

ProtectionDomainFailure ProtectionDomain::InitializeKernelOnly(
    Object::ObjectRegistry& registry, const Resource::ResourceDomainObjectId resourceDomainIdentity,
    Resource::ResourceDomain& resourceDomain, const Memory::AddressSpaceId addressSpace) noexcept
{
    return InitializeAssociation(registry, resourceDomainIdentity, resourceDomain, addressSpace, nullptr,
                                 ProtectionDomainKind::KernelOnly);
}

ProtectionDomainFailure ProtectionDomain::InitializeAssociation(
    Object::ObjectRegistry& registry, const Resource::ResourceDomainObjectId resourceDomainIdentity,
    Resource::ResourceDomain& resourceDomain, const Memory::AddressSpaceId addressSpace,
    Object::CapabilitySpace* const capabilities, const ProtectionDomainKind kind) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != ProtectionDomainState::Uninitialized)
    {
        return MakeFailure(ProtectionDomainError::AlreadyInitialized);
    }
    const Object::ResourceDomainId resourceDomainIdentifier = resourceDomain.GetIdentifier();
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry) ||
        !resourceDomainIdentifier.IsValid() || !addressSpace.IsValid() || !addressSpace.IsFromRegistry(registry) ||
        ((kind == ProtectionDomainKind::UserCapable) &&
         ((capabilities == nullptr) || !capabilities->IsReady() || !capabilities->IsFromRegistry(registry) ||
          !capabilities->GetIdentifier().IsValid())) ||
        ((kind == ProtectionDomainKind::KernelOnly) && (capabilities != nullptr)) ||
        ((kind != ProtectionDomainKind::UserCapable) && (kind != ProtectionDomainKind::KernelOnly)))
    {
        return MakeFailure(ProtectionDomainError::InvalidConfiguration);
    }

    const Object::ObjectPinError pinError =
        registry.PinObject(addressSpace.GetObjectId(), Object::ObjectType::AddressSpace, m_addressSpacePin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(ProtectionDomainError::AddressSpacePinRejected, pinError);
    }
    Memory::AddressSpace* const addressSpaceStorage =
        static_cast<Memory::AddressSpace*>(&m_addressSpacePin.GetObject());
    if ((addressSpaceStorage == nullptr) || (m_addressSpacePin.GetObjectType() != Object::ObjectType::AddressSpace) ||
        (m_addressSpacePin.GetObjectId().GetValue() != addressSpace.GetObjectId().GetValue()) ||
        (static_cast<Object::IKernelObject*>(addressSpaceStorage) != &m_addressSpacePin.GetObject()))
    {
        m_addressSpacePin.Reset();
        return MakeFailure(ProtectionDomainError::AddressSpacePinRejected, Object::ObjectPinError::InvalidArgument);
    }

    const Object::ObjectPinError resourcePinError = registry.PinObject(
        resourceDomainIdentity.GetObjectId(), Object::ObjectType::ResourceDomain, m_resourceDomainPin);
    if (resourcePinError != Object::ObjectPinError::Ok)
    {
        m_addressSpacePin.Reset();
        return MakeFailure(ProtectionDomainError::ResourceDomainPinRejected, resourcePinError);
    }
    if (&m_resourceDomainPin.GetObject() != static_cast<Object::IKernelObject*>(&resourceDomain))
    {
        m_resourceDomainPin.Reset();
        m_addressSpacePin.Reset();
        return MakeFailure(ProtectionDomainError::ResourceDomainInstanceMismatch);
    }

    const Resource::ResourceDomainError resourceError = resourceDomain.TryReserve(GetResourceReservation());
    if (resourceError != Resource::ResourceDomainError::None)
    {
        m_resourceDomainPin.Reset();
        m_addressSpacePin.Reset();
        return MakeFailure(ProtectionDomainError::ResourceDomainRejected, Object::ObjectPinError::Ok, resourceError);
    }

    m_registry = &registry;
    m_addressSpaceStorage = addressSpaceStorage;
    m_capabilities = capabilities;
    m_resourceDomain = &resourceDomain;
    m_identity = ProtectionDomainId{};
    m_addressSpace = addressSpace;
    m_capabilitySpace = capabilities == nullptr ? Object::CapabilitySpaceIdentifier{} : capabilities->GetIdentifier();
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_resourceDomainIdentifier = resourceDomainIdentifier;
    m_kind = kind;
    m_state = ProtectionDomainState::Unbound;
    m_chargeOwnerValue.StoreRelease(resourceDomainIdentifier.GetValue());
    return MakeFailure(ProtectionDomainError::None);
}

ProtectionDomainFailure ProtectionDomain::BindIdentity(Object::ObjectRegistry& registry,
                                                       const ProtectionDomainId identity) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainState::Uninitialized)
    {
        return MakeFailure(ProtectionDomainError::NotInitialized);
    }
    if (m_state == ProtectionDomainState::Quiesced)
    {
        return MakeFailure(ProtectionDomainError::Quiesced);
    }
    if (m_state != ProtectionDomainState::Unbound)
    {
        return MakeFailure(ProtectionDomainError::AlreadyBound);
    }
    if (!identity.IsValid())
    {
        return MakeFailure(ProtectionDomainError::InvalidIdentity);
    }
    if ((m_registry != &registry) || !identity.IsFromRegistry(registry))
    {
        return MakeFailure(ProtectionDomainError::RegistryMismatch);
    }

    Object::ObjectPin identityPin;
    const Object::ObjectPinError pinError =
        registry.PinObject(identity.GetObjectId(), Object::ObjectType::ProtectionDomain, identityPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(ProtectionDomainError::InvalidIdentity, pinError);
    }
    if (&identityPin.GetObject() != static_cast<Object::IKernelObject*>(this))
    {
        identityPin.Reset();
        return MakeFailure(ProtectionDomainError::ObjectInstanceMismatch);
    }
    identityPin.Reset();

    m_identity = identity;
    m_state = ProtectionDomainState::Ready;
    return MakeFailure(ProtectionDomainError::None);
}

Foundation::Result<ProtectionDomainBinding, ProtectionDomainError> ProtectionDomain::GetBinding() const noexcept
{
    using ResultType = Foundation::Result<ProtectionDomainBinding, ProtectionDomainError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainState::Uninitialized)
    {
        return ResultType::Failure(ProtectionDomainError::NotInitialized);
    }
    if (m_state == ProtectionDomainState::Quiesced)
    {
        return ResultType::Failure(ProtectionDomainError::Quiesced);
    }
    if ((m_state != ProtectionDomainState::Ready) || !IsCapabilityBindingValid() || (m_resourceDomain == nullptr) ||
        !m_addressSpacePin.IsValid() || !m_resourceDomainPin.IsValid() || (m_addressSpaceStorage == nullptr) ||
        (m_addressSpacePin.GetObjectType() != Object::ObjectType::AddressSpace) ||
        (m_addressSpacePin.GetObjectId().GetValue() != m_addressSpace.GetObjectId().GetValue()) ||
        (static_cast<Object::IKernelObject*>(m_addressSpaceStorage) != &m_addressSpacePin.GetObject()))
    {
        return ResultType::Failure(ProtectionDomainError::NotReady);
    }
    return ResultType::Success(ProtectionDomainBinding{
        .Identity = m_identity,
        .AddressSpace = m_addressSpace,
        .AddressSpaceStorage = m_addressSpaceStorage,
        .Capabilities = m_capabilities,
        .CapabilitySpace = m_capabilitySpace,
        .ResourceDomainIdentity = m_resourceDomainIdentity,
        .ResourceDomainStorage = m_resourceDomain,
        .ResourceDomain = m_resourceDomainIdentifier,
    });
}

bool ProtectionDomain::IsCapabilityBindingValid() const noexcept
{
    if (m_kind == ProtectionDomainKind::KernelOnly)
    {
        return (m_capabilities == nullptr) && !m_capabilitySpace.IsValid();
    }
    if ((m_kind != ProtectionDomainKind::UserCapable) || (m_capabilities == nullptr) || (m_registry == nullptr) ||
        !m_capabilitySpace.IsValid() || !m_capabilities->IsReady() || !m_capabilities->IsFromRegistry(*m_registry))
    {
        return false;
    }
    const Object::CapabilitySpaceIdentifier liveIdentifier = m_capabilities->GetIdentifier();
    return liveIdentifier.IsValid() && (liveIdentifier.GetValue() == m_capabilitySpace.GetValue());
}

ProtectionDomainError ProtectionDomain::RetainDeviceAssignment() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainState::Quiesced)
    {
        return ProtectionDomainError::Quiesced;
    }
    if (m_state != ProtectionDomainState::Ready)
    {
        return ProtectionDomainError::NotReady;
    }
    if (m_activeDeviceAssignments == ~Foundation::UInt64{0U})
    {
        return ProtectionDomainError::DeviceAssignmentReferenceExhausted;
    }
    ++m_activeDeviceAssignments;
    return ProtectionDomainError::None;
}

ProtectionDomainError ProtectionDomain::ReleaseDeviceAssignment() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != ProtectionDomainState::Ready)
    {
        return ProtectionDomainError::NotReady;
    }
    if (m_activeDeviceAssignments == 0U)
    {
        return ProtectionDomainError::DeviceAssignmentReferenceUnderflow;
    }
    --m_activeDeviceAssignments;
    return ProtectionDomainError::None;
}

ProtectionDomainError ProtectionDomain::Quiesce() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ProtectionDomainState::Uninitialized)
    {
        return ProtectionDomainError::NotInitialized;
    }
    if (m_state == ProtectionDomainState::Quiesced)
    {
        return ProtectionDomainError::Quiesced;
    }
    if (m_activeDeviceAssignments != 0U)
    {
        return ProtectionDomainError::InUse;
    }

    if (m_resourceDomain == nullptr)
    {
        return ProtectionDomainError::ResourceDomainRejected;
    }
    const Resource::ResourceDomainError resourceError = m_resourceDomain->Release(GetResourceReservation());
    if (resourceError != Resource::ResourceDomainError::None)
    {
        return ProtectionDomainError::ResourceDomainRejected;
    }

    m_registry = nullptr;
    m_addressSpaceStorage = nullptr;
    m_capabilities = nullptr;
    m_resourceDomain = nullptr;
    m_identity = ProtectionDomainId{};
    m_addressSpace = Memory::AddressSpaceId{};
    m_capabilitySpace = Object::CapabilitySpaceIdentifier{};
    m_resourceDomainIdentity = Resource::ResourceDomainObjectId{};
    m_resourceDomainIdentifier = Object::ResourceDomainId{};
    m_chargeOwnerValue.StoreRelease(0U);
    m_kind = ProtectionDomainKind::Uninitialized;
    m_state = ProtectionDomainState::Quiesced;
    m_resourceDomainPin.Reset();
    m_addressSpacePin.Reset();
    return ProtectionDomainError::None;
}

ProtectionDomainSnapshot ProtectionDomain::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ProtectionDomainSnapshot{
        .Identity = m_identity,
        .AddressSpace = m_addressSpace,
        .CapabilitySpace = m_capabilitySpace,
        .ResourceDomainIdentity = m_resourceDomainIdentity,
        .ResourceDomain = m_resourceDomainIdentifier,
        .Kind = m_kind,
        .State = m_state,
        .Reserved = {},
    };
}

Object::ObjectType ProtectionDomain::GetObjectType() const noexcept
{
    return Object::ObjectType::ProtectionDomain;
}

Object::ResourceDomainId ProtectionDomain::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}
} // namespace Asteria::Kernel::Protection
