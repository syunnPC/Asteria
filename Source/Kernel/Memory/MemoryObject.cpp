// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/MemoryObject.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr bool IsKnownCachePolicy(const MemoryCachePolicy policy) noexcept
{
    switch (policy)
    {
    case MemoryCachePolicy::WriteBack:
    case MemoryCachePolicy::Uncached:
    case MemoryCachePolicy::WriteCombining:
    case MemoryCachePolicy::WriteThrough:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownCommitment(const MemoryCommitment commitment) noexcept
{
    switch (commitment)
    {
    case MemoryCommitment::Committed:
    case MemoryCommitment::Discardable:
    case MemoryCommitment::Reconstructible:
    case MemoryCommitment::BestEffort:
    case MemoryCommitment::Pinned:
    case MemoryCommitment::DeviceDma:
        return true;
    }
    return false;
}

[[nodiscard]] bool DescriptorIsValid(const MemoryObjectDescriptor& descriptor) noexcept
{
    const Foundation::UInt64 pages = descriptor.Pages.GetValue();
    if ((pages == 0U) || (pages > (MaximumUInt64 / MappingPageSizeBytes)) ||
        !IsKnownCachePolicy(descriptor.CachePolicy) || !IsKnownCommitment(descriptor.Commitment) ||
        !descriptor.MaximumPermissions.HasOnlyKnownBits() ||
        !descriptor.MaximumPermissions.Contains(PagePermission::Read) ||
        ((descriptor.Commitment == MemoryCommitment::DeviceDma) &&
         (descriptor.MaximumPermissions.Contains(PagePermission::Execute) ||
          ((descriptor.CachePolicy != MemoryCachePolicy::Uncached) &&
           (descriptor.CachePolicy != MemoryCachePolicy::WriteCombining)))))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < sizeof(descriptor.Reserved); ++index)
    {
        if (descriptor.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

MemoryObjectFailure MemoryObject::MakeFailure(const MemoryObjectError error, const Object::ObjectPinError pin,
                                              const Resource::ResourceDomainError resource) noexcept
{
    return MemoryObjectFailure{
        .Error = error,
        .Pin = pin,
        .Resource = resource,
        .Reserved = {},
    };
}

Resource::ResourceReservation MemoryObject::GetReservation(const MemoryObjectDescriptor& descriptor,
                                                           const MemoryObjectCharge charge) noexcept
{
    const bool pages = charge == MemoryObjectCharge::Pages;
    const bool committed = pages && ((descriptor.Commitment == MemoryCommitment::Committed) ||
                                     (descriptor.Commitment == MemoryCommitment::Pinned) ||
                                     (descriptor.Commitment == MemoryCommitment::DeviceDma));
    const bool resident = pages && ((descriptor.Commitment == MemoryCommitment::Pinned) ||
                                    (descriptor.Commitment == MemoryCommitment::DeviceDma));
    return Resource::ResourceReservation{
        .CommittedMemoryPages = committed ? descriptor.Pages.GetValue() : 0U,
        .ResidentMemoryPages = resident ? descriptor.Pages.GetValue() : 0U,
        .KernelMetadataBytes = sizeof(MemoryObject),
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .Objects = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U},
    };
}

MemoryObjectFailure MemoryObject::Initialize(Object::ObjectRegistry& registry,
                                             const Resource::ResourceDomainObjectId resourceDomainIdentity,
                                             Resource::ResourceDomain& resourceDomain,
                                             const MemoryObjectDescriptor& descriptor,
                                             const MemoryObjectCharge charge) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != MemoryObjectState::Uninitialized)
    {
        return MakeFailure(MemoryObjectError::AlreadyInitialized);
    }
    const Object::ResourceDomainId resourceDomainIdentifier = resourceDomain.GetIdentifier();
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry) ||
        !resourceDomainIdentifier.IsValid())
    {
        return MakeFailure(MemoryObjectError::InvalidConfiguration);
    }
    if (!DescriptorIsValid(descriptor))
    {
        return MakeFailure(MemoryObjectError::InvalidDescriptor);
    }

    const Object::ObjectPinError pinError = registry.PinObject(resourceDomainIdentity.GetObjectId(),
                                                               Object::ObjectType::ResourceDomain, m_resourceDomainPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(MemoryObjectError::ResourceDomainPinRejected, pinError);
    }
    if (&m_resourceDomainPin.GetObject() != static_cast<Object::IKernelObject*>(&resourceDomain))
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(MemoryObjectError::ResourceDomainInstanceMismatch);
    }

    const Resource::ResourceDomainError resourceError = resourceDomain.TryReserve(GetReservation(descriptor, charge));
    if (resourceError != Resource::ResourceDomainError::None)
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(MemoryObjectError::ResourceDomainRejected, Object::ObjectPinError::Ok, resourceError);
    }

    m_resourceDomain = &resourceDomain;
    m_descriptor = descriptor;
    m_charge = charge;
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_resourceDomainIdentifier = resourceDomainIdentifier;
    m_state = MemoryObjectState::Ready;
    m_chargeOwnerValue.StoreRelease(resourceDomainIdentifier.GetValue());
    return MakeFailure(MemoryObjectError::None);
}

MemoryObjectFailure MemoryObject::Quiesce() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == MemoryObjectState::Uninitialized)
    {
        return MakeFailure(MemoryObjectError::NotReady);
    }
    if (m_state == MemoryObjectState::Quiesced)
    {
        return MakeFailure(MemoryObjectError::Quiesced);
    }
    if ((m_state != MemoryObjectState::Ready) || (m_resourceDomain == nullptr) || !m_resourceDomainPin.IsValid())
    {
        return MakeFailure(MemoryObjectError::InvariantViolation);
    }
    if (m_activeMappings != 0U)
    {
        return MakeFailure(MemoryObjectError::InUse);
    }

    const Resource::ResourceDomainError resourceError =
        m_resourceDomain->Release(GetReservation(m_descriptor, m_charge));
    if (resourceError != Resource::ResourceDomainError::None)
    {
        return MakeFailure(MemoryObjectError::ResourceDomainRejected, Object::ObjectPinError::Ok, resourceError);
    }
    m_resourceDomain = nullptr;
    m_resourceDomainPin.Reset();
    m_state = MemoryObjectState::Quiesced;
    return MakeFailure(MemoryObjectError::None);
}

MemoryObjectError MemoryObject::RetainMapping(const Foundation::UInt64 offsetPages, const PageCount pages,
                                              const PagePermissions permissions, const MemoryCachePolicy cachePolicy,
                                              const MemoryCommitment commitment) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != MemoryObjectState::Ready)
    {
        return (m_state == MemoryObjectState::Quiesced) ? MemoryObjectError::Quiesced : MemoryObjectError::NotReady;
    }

    const Foundation::UInt64 requestedPages = pages.GetValue();
    const Foundation::UInt64 objectPages = m_descriptor.Pages.GetValue();
    if ((requestedPages == 0U) || (offsetPages > objectPages) || (requestedPages > (objectPages - offsetPages)))
    {
        return MemoryObjectError::MappingRangeExceeded;
    }
    if (!permissions.HasOnlyKnownBits() || !permissions.Contains(PagePermission::Read) ||
        (permissions.Contains(PagePermission::Write) && permissions.Contains(PagePermission::Execute)) ||
        ((permissions.GetBits() & static_cast<Foundation::UInt8>(~m_descriptor.MaximumPermissions.GetBits())) != 0U))
    {
        return MemoryObjectError::MappingAccessDenied;
    }
    if (cachePolicy != m_descriptor.CachePolicy)
    {
        return MemoryObjectError::MappingCacheMismatch;
    }
    if (commitment != m_descriptor.Commitment)
    {
        return MemoryObjectError::MappingCommitmentMismatch;
    }
    if (m_activeMappings == MaximumUInt64)
    {
        return MemoryObjectError::MappingReferenceExhausted;
    }
    ++m_activeMappings;
    return MemoryObjectError::None;
}

MemoryObjectError MemoryObject::ReleaseMapping() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != MemoryObjectState::Ready)
    {
        return MemoryObjectError::InvariantViolation;
    }
    if (m_activeMappings == 0U)
    {
        return MemoryObjectError::MappingReferenceUnderflow;
    }
    --m_activeMappings;
    return MemoryObjectError::None;
}

bool MemoryObject::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state == MemoryObjectState::Ready;
}

MemoryObjectSnapshot MemoryObject::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return MemoryObjectSnapshot{
        .Descriptor = m_descriptor,
        .ResourceDomainIdentity = m_resourceDomainIdentity,
        .ResourceDomain = m_resourceDomainIdentifier,
        .ActiveMappings = m_activeMappings,
        .State = m_state,
        .Reserved = {},
    };
}

Object::ObjectType MemoryObject::GetObjectType() const noexcept
{
    return Object::ObjectType::MemoryObject;
}

Object::ResourceDomainId MemoryObject::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}
} // namespace Asteria::Kernel::Memory
