// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/DeviceRegion.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt64 FrozenCachePolicyBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 CachePolicyMask{0xFFU};

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumPortAddress{0xFFFFU};

[[nodiscard]] constexpr bool IsKnownKind(const DeviceRegionKind kind) noexcept
{
    return (kind == DeviceRegionKind::MemoryMappedIo) || (kind == DeviceRegionKind::PortIo);
}

[[nodiscard]] constexpr bool IsKnownCachePolicy(const Memory::MemoryCachePolicy cachePolicy) noexcept
{
    switch (cachePolicy)
    {
    case Memory::MemoryCachePolicy::WriteBack:
    case Memory::MemoryCachePolicy::Uncached:
    case Memory::MemoryCachePolicy::WriteCombining:
    case Memory::MemoryCachePolicy::WriteThrough:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownWidth(const DeviceAccessWidth width) noexcept
{
    switch (width)
    {
    case DeviceAccessWidth::Byte:
    case DeviceAccessWidth::Word:
    case DeviceAccessWidth::DoubleWord:
    case DeviceAccessWidth::QuadWord:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownOperation(const DeviceAccessOperation operation) noexcept
{
    return (operation == DeviceAccessOperation::Read) || (operation == DeviceAccessOperation::Write);
}

[[nodiscard]] bool DescriptorIsValidInternal(const DeviceRegionDescriptor& descriptor) noexcept
{
    if ((descriptor.LengthBytes == 0U) || (descriptor.BaseAddress > (MaximumUInt64 - descriptor.LengthBytes)) ||
        !IsKnownKind(descriptor.Kind) || !descriptor.Permissions.HasOnlyKnownBits() ||
        descriptor.Permissions.IsEmpty() || !descriptor.Widths.HasOnlyKnownBits() || descriptor.Widths.IsEmpty() ||
        !IsKnownCachePolicy(descriptor.CachePolicy))
    {
        return false;
    }

    if (descriptor.Kind == DeviceRegionKind::MemoryMappedIo)
    {
        if ((descriptor.CachePolicy != Memory::MemoryCachePolicy::Uncached) &&
            (descriptor.CachePolicy != Memory::MemoryCachePolicy::WriteCombining))
        {
            return false;
        }
    }
    else
    {
        const Foundation::UInt64 lastAddress = descriptor.BaseAddress + descriptor.LengthBytes - 1U;
        if ((lastAddress > MaximumPortAddress) || (descriptor.CachePolicy != Memory::MemoryCachePolicy::Uncached) ||
            descriptor.Widths.Contains(DeviceAccessWidth::QuadWord))
        {
            return false;
        }
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

[[nodiscard]] constexpr Resource::ResourceReservation GetReservation() noexcept
{
    return Resource::ResourceReservation{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = sizeof(DeviceRegion),
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
    };
}
} // namespace

bool DeviceRegion::DescriptorIsValid(const DeviceRegionDescriptor& descriptor) noexcept
{
    return DescriptorIsValidInternal(descriptor);
}

DeviceRegionFailure DeviceRegion::MakeFailure(const DeviceRegionError error, const Object::ObjectPinError pin,
                                              const Resource::ResourceDomainError resource) noexcept
{
    return DeviceRegionFailure{
        .Error = error,
        .Pin = pin,
        .Resource = resource,
        .Reserved = {},
    };
}

DeviceRegionFailure DeviceRegion::Initialize(Object::ObjectRegistry& registry,
                                             const Resource::ResourceDomainObjectId resourceDomainIdentity,
                                             Resource::ResourceDomain& resourceDomain,
                                             const DeviceRegionDescriptor& descriptor,
                                             const DevicePortWritePolicy writes) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != DeviceRegionState::Uninitialized)
    {
        return MakeFailure(DeviceRegionError::AlreadyInitialized);
    }

    const Object::ResourceDomainId resourceDomainIdentifier = resourceDomain.GetIdentifier();
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry) ||
        !resourceDomainIdentifier.IsValid())
    {
        return MakeFailure(DeviceRegionError::InvalidConfiguration);
    }
    if (!DescriptorIsValidInternal(descriptor))
    {
        return MakeFailure(DeviceRegionError::InvalidDescriptor);
    }

    if ((writes.Count > 32U) || ((writes.Lists == nullptr) != (writes.Count == 0U)) ||
        ((writes.Count != 0U) && (descriptor.Kind != DeviceRegionKind::PortIo)))
    {
        return MakeFailure(DeviceRegionError::InvalidDescriptor);
    }
    for (Foundation::Size index{0U}; index < writes.Count; ++index)
    {
        const DevicePortWriteList& list = writes.Lists[index];
        const Foundation::UInt64 width = static_cast<Foundation::UInt8>(list.Width);
        if (!IsKnownWidth(list.Width) || !descriptor.Widths.Contains(list.Width) ||
            (list.Offset > descriptor.LengthBytes) || (width > descriptor.LengthBytes - list.Offset) ||
            (list.Count > 32U) || ((list.Values == nullptr) != (list.Count == 0U)))
        {
            return MakeFailure(DeviceRegionError::InvalidDescriptor);
        }
        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            if (writes.Lists[previous].Offset == list.Offset)
            {
                return MakeFailure(DeviceRegionError::InvalidDescriptor);
            }
        }
        for (Foundation::Size item{0U}; item < list.Count; ++item)
        {
            if ((width < 4U) && (list.Values[item] >= (Foundation::UInt64{1U} << (width * 8U))))
            {
                return MakeFailure(DeviceRegionError::InvalidDescriptor);
            }
        }
    }
    const Object::ObjectPinError pinError = registry.PinObject(resourceDomainIdentity.GetObjectId(),
                                                               Object::ObjectType::ResourceDomain, m_resourceDomainPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(DeviceRegionError::ResourceDomainPinRejected, pinError);
    }
    if (&m_resourceDomainPin.GetObject() != static_cast<Object::IKernelObject*>(&resourceDomain))
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(DeviceRegionError::ResourceDomainInstanceMismatch);
    }

    const Resource::ResourceDomainError resourceError = resourceDomain.TryReserve(GetReservation());
    if (resourceError != Resource::ResourceDomainError::None)
    {
        m_resourceDomainPin.Reset();
        return MakeFailure(DeviceRegionError::ResourceDomainRejected, Object::ObjectPinError::Ok, resourceError);
    }

    m_resourceDomain = &resourceDomain;
    m_descriptor = descriptor;
    m_mappingCachePolicy.StoreRelease(static_cast<Foundation::UInt64>(descriptor.CachePolicy));
    m_writes = writes;
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_resourceDomainIdentifier = resourceDomainIdentifier;
    m_state = DeviceRegionState::Ready;
    m_chargeOwnerValue.StoreRelease(resourceDomainIdentifier.GetValue());
    return MakeFailure(DeviceRegionError::None);
}

DeviceRegionFailure DeviceRegion::Quiesce() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DeviceRegionState::Uninitialized)
    {
        return MakeFailure(DeviceRegionError::NotReady);
    }
    if (m_state == DeviceRegionState::Quiesced)
    {
        return MakeFailure(DeviceRegionError::Quiesced);
    }
    if ((m_state != DeviceRegionState::Ready) || (m_resourceDomain == nullptr) || !m_resourceDomainPin.IsValid())
    {
        return MakeFailure(DeviceRegionError::InvariantViolation);
    }
    if ((m_activeMappings != 0U) || (m_activeRegisterAccesses != 0U))
    {
        return MakeFailure(DeviceRegionError::InUse);
    }

    const Resource::ResourceDomainError resourceError = m_resourceDomain->Release(GetReservation());
    if (resourceError != Resource::ResourceDomainError::None)
    {
        return MakeFailure(DeviceRegionError::ResourceDomainRejected, Object::ObjectPinError::Ok, resourceError);
    }

    m_resourceDomain = nullptr;
    m_resourceDomainPin.Reset();
    m_state = DeviceRegionState::Quiesced;
    return MakeFailure(DeviceRegionError::None);
}

DeviceRegionError DeviceRegion::RetainRegisterAccess(const Foundation::UInt64 offsetBytes,
                                                     const DeviceAccessWidth width,
                                                     const DeviceAccessOperation operation,
                                                     const Foundation::UInt32 value) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != DeviceRegionState::Ready)
    {
        return (m_state == DeviceRegionState::Quiesced) ? DeviceRegionError::Quiesced : DeviceRegionError::NotReady;
    }
    if (!IsKnownOperation(operation))
    {
        return DeviceRegionError::InvalidOperation;
    }
    if (!IsKnownWidth(width) || !m_descriptor.Widths.Contains(width))
    {
        return DeviceRegionError::UnsupportedWidth;
    }

    const DeviceRegionPermission requiredPermission =
        (operation == DeviceAccessOperation::Read) ? DeviceRegionPermission::Read : DeviceRegionPermission::Write;
    if (!m_descriptor.Permissions.Contains(requiredPermission))
    {
        return DeviceRegionError::UnsupportedAccess;
    }

    const Foundation::UInt64 widthBytes = static_cast<Foundation::UInt8>(width);
    if ((offsetBytes > m_descriptor.LengthBytes) || (widthBytes > (m_descriptor.LengthBytes - offsetBytes)))
    {
        return DeviceRegionError::RangeExceeded;
    }
    const Foundation::UInt64 address = m_descriptor.BaseAddress + offsetBytes;
    if ((address & (widthBytes - 1U)) != 0U)
    {
        return DeviceRegionError::MisalignedAccess;
    }
    if (operation == DeviceAccessOperation::Write)
    {
        for (Foundation::Size index{0U}; index < m_writes.Count; ++index)
        {
            const DevicePortWriteList& list = m_writes.Lists[index];
            const Foundation::UInt64 end = list.Offset + static_cast<Foundation::UInt8>(list.Width);
            if ((offsetBytes >= end) || (list.Offset >= offsetBytes + widthBytes))
            {
                continue;
            }
            bool permitted{false};
            for (Foundation::Size item{0U}; item < list.Count; ++item)
            {
                permitted = permitted || (list.Values[item] == value);
            }
            if ((offsetBytes != list.Offset) || (width != list.Width) || !permitted)
            {
                return DeviceRegionError::WriteValueDenied;
            }
        }
    }
    if (m_activeRegisterAccesses == MaximumUInt64)
    {
        return DeviceRegionError::ReferenceExhausted;
    }

    ++m_activeRegisterAccesses;
    return DeviceRegionError::None;
}

DeviceRegionError DeviceRegion::ReleaseRegisterAccess() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != DeviceRegionState::Ready)
    {
        return DeviceRegionError::InvariantViolation;
    }
    if (m_activeRegisterAccesses == 0U)
    {
        return DeviceRegionError::AccessReferenceUnderflow;
    }
    --m_activeRegisterAccesses;
    return DeviceRegionError::None;
}

DeviceRegionError DeviceRegion::ValidateMappingLocked(const Memory::PhysicalAddress physicalStart,
                                                      const Memory::PageCount pages,
                                                      const Memory::PagePermissions permissions,
                                                      const Memory::MemoryCachePolicy cachePolicy) const noexcept
{
    if (m_state != DeviceRegionState::Ready)
    {
        return (m_state == DeviceRegionState::Quiesced) ? DeviceRegionError::Quiesced : DeviceRegionError::NotReady;
    }
    if (m_descriptor.Kind != DeviceRegionKind::MemoryMappedIo)
    {
        return DeviceRegionError::MappingNotSupported;
    }

    const Foundation::UInt64 pageCount = pages.GetValue();
    const Foundation::UInt64 mappingStart = physicalStart.GetValue();
    if ((pageCount == 0U) || (pageCount > (MaximumUInt64 / Memory::MappingPageSizeBytes)) ||
        ((mappingStart % Memory::MappingPageSizeBytes) != 0U))
    {
        return DeviceRegionError::RangeExceeded;
    }
    const Foundation::UInt64 sizeBytes = pageCount * Memory::MappingPageSizeBytes;
    if (mappingStart > (MaximumUInt64 - sizeBytes))
    {
        return DeviceRegionError::RangeExceeded;
    }
    const Foundation::UInt64 mappingEnd = mappingStart + sizeBytes;
    const Foundation::UInt64 regionEnd = m_descriptor.BaseAddress + m_descriptor.LengthBytes;
    if ((mappingStart < m_descriptor.BaseAddress) || (mappingEnd > regionEnd))
    {
        return DeviceRegionError::RangeExceeded;
    }

    if (!permissions.HasOnlyKnownBits() || !permissions.Contains(Memory::PagePermission::Read) ||
        permissions.Contains(Memory::PagePermission::Execute) ||
        !m_descriptor.Permissions.Contains(DeviceRegionPermission::Read) ||
        (permissions.Contains(Memory::PagePermission::Write) &&
         !m_descriptor.Permissions.Contains(DeviceRegionPermission::Write)))
    {
        return DeviceRegionError::MappingAccessDenied;
    }
    if (!Memory::IsDeviceCachePolicyCompatible(m_descriptor.CachePolicy, cachePolicy) ||
        (((m_activeMappings != 0U) || ((m_mappingCachePolicy.LoadAcquire() & FrozenCachePolicyBit) != 0U)) &&
         (cachePolicy != GetMappingCachePolicy())))
    {
        return DeviceRegionError::MappingCacheMismatch;
    }
    return DeviceRegionError::None;
}

DeviceRegionError DeviceRegion::ValidateMapping(const Memory::PhysicalAddress physicalStart,
                                                const Memory::PageCount pages,
                                                const Memory::PagePermissions permissions,
                                                const Memory::MemoryCachePolicy cachePolicy) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ValidateMappingLocked(physicalStart, pages, permissions, cachePolicy);
}

DeviceRegionError DeviceRegion::RetainMapping(const Memory::PhysicalAddress physicalStart,
                                              const Memory::PageCount pages, const Memory::PagePermissions permissions,
                                              const Memory::MemoryCachePolicy cachePolicy) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const DeviceRegionError validation = ValidateMappingLocked(physicalStart, pages, permissions, cachePolicy);
    if (validation != DeviceRegionError::None)
    {
        return validation;
    }
    if (m_activeMappings == MaximumUInt64)
    {
        return DeviceRegionError::ReferenceExhausted;
    }

    Foundation::UInt64 expected = m_mappingCachePolicy.LoadAcquire();
    const Foundation::UInt64 selected = static_cast<Foundation::UInt64>(cachePolicy);
    if ((expected & CachePolicyMask) != selected)
    {
        if (((expected & FrozenCachePolicyBit) != 0U) ||
            !m_mappingCachePolicy.CompareExchangeStrongAcquireRelease(expected, selected))
        {
            return DeviceRegionError::MappingCacheMismatch;
        }
    }
    ++m_activeMappings;
    return DeviceRegionError::None;
}

Memory::MemoryCachePolicy DeviceRegion::GetMappingCachePolicy() const noexcept
{
    // Initialize/RetainMapping publish only validated enum values.
    return static_cast<Memory::MemoryCachePolicy>(m_mappingCachePolicy.LoadAcquire() & CachePolicyMask);
}

bool DeviceRegion::FreezeMappingCachePolicy(Memory::MemoryCachePolicy& output) noexcept
{
    Foundation::UInt64 expected = m_mappingCachePolicy.LoadAcquire();
    if (!m_mappingCachePolicy.CompareExchangeStrongAcquireRelease(expected, expected | FrozenCachePolicyBit))
    {
        return false;
    }
    // The policy came from Initialize or a validated RetainMapping.
    output = static_cast<Memory::MemoryCachePolicy>(expected & CachePolicyMask);
    return true;
}

DeviceRegionError DeviceRegion::ReleaseMapping() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != DeviceRegionState::Ready)
    {
        return DeviceRegionError::InvariantViolation;
    }
    if (m_activeMappings == 0U)
    {
        return DeviceRegionError::MappingReferenceUnderflow;
    }
    --m_activeMappings;
    return DeviceRegionError::None;
}

DeviceRegionError DeviceRegion::ResetForDeviceManagerReuse() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != DeviceRegionState::Quiesced) || m_resourceDomainPin.IsValid() || (m_resourceDomain != nullptr) ||
        (m_activeMappings != 0U) || (m_activeRegisterAccesses != 0U))
    {
        return DeviceRegionError::InvariantViolation;
    }

    m_descriptor = DeviceRegionDescriptor{};
    m_resourceDomainIdentity = Resource::ResourceDomainObjectId{};
    m_resourceDomainIdentifier = Object::ResourceDomainId{};
    m_chargeOwnerValue.StoreRelease(0U);
    m_state = DeviceRegionState::Uninitialized;
    return DeviceRegionError::None;
}

bool DeviceRegion::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state == DeviceRegionState::Ready;
}

DeviceRegionSnapshot DeviceRegion::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return DeviceRegionSnapshot{
        .Descriptor = m_descriptor,
        .ResourceDomainIdentity = m_resourceDomainIdentity,
        .ResourceDomain = m_resourceDomainIdentifier,
        .ActiveMappings = m_activeMappings,
        .ActiveRegisterAccesses = m_activeRegisterAccesses,
        .State = m_state,
        .Reserved = {},
    };
}

Object::ObjectType DeviceRegion::GetObjectType() const noexcept
{
    return Object::ObjectType::DeviceRegion;
}

Object::ResourceDomainId DeviceRegion::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}
} // namespace Asteria::Kernel::Device
