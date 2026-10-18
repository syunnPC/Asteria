// SPDX-License-Identifier: Apache-2.0

#include "FaultFrameMapping.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr FaultFrameMappingError MapFramePinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return FaultFrameMappingError::FrameNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return FaultFrameMappingError::FrameTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return FaultFrameMappingError::FramePinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return FaultFrameMappingError::InvalidFrame;
    case Object::ObjectPinError::Ok:
        return FaultFrameMappingError::None;
    }
    return FaultFrameMappingError::InvalidFrame;
}

[[nodiscard]] constexpr FaultFrameMappingError MapMemoryObjectPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return FaultFrameMappingError::MemoryObjectNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return FaultFrameMappingError::MemoryObjectTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return FaultFrameMappingError::MemoryObjectPinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return FaultFrameMappingError::InvalidMemoryObject;
    case Object::ObjectPinError::Ok:
        return FaultFrameMappingError::None;
    }
    return FaultFrameMappingError::InvalidMemoryObject;
}

[[nodiscard]] constexpr FaultFrameMappingError MapDeviceRegionPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return FaultFrameMappingError::DeviceRegionNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return FaultFrameMappingError::DeviceRegionTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return FaultFrameMappingError::DeviceRegionPinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return FaultFrameMappingError::InvalidDeviceRegion;
    case Object::ObjectPinError::Ok:
        return FaultFrameMappingError::None;
    }
    return FaultFrameMappingError::InvalidDeviceRegion;
}

[[nodiscard]] constexpr FaultFrameMappingError MapDeviceRegionError(const Device::DeviceRegionError error) noexcept
{
    switch (error)
    {
    case Device::DeviceRegionError::RangeExceeded:
        return FaultFrameMappingError::DeviceRegionRangeExceeded;
    case Device::DeviceRegionError::MappingAccessDenied:
        return FaultFrameMappingError::DeviceRegionAccessDenied;
    case Device::DeviceRegionError::MappingCacheMismatch:
        return FaultFrameMappingError::DeviceRegionCacheMismatch;
    case Device::DeviceRegionError::None:
        return FaultFrameMappingError::None;
    case Device::DeviceRegionError::InvalidConfiguration:
    case Device::DeviceRegionError::InvalidDescriptor:
    case Device::DeviceRegionError::AlreadyInitialized:
    case Device::DeviceRegionError::ResourceDomainPinRejected:
    case Device::DeviceRegionError::ResourceDomainInstanceMismatch:
    case Device::DeviceRegionError::ResourceDomainRejected:
    case Device::DeviceRegionError::NotReady:
    case Device::DeviceRegionError::Quiesced:
    case Device::DeviceRegionError::WriteValueDenied:
    case Device::DeviceRegionError::InvalidOperation:
    case Device::DeviceRegionError::UnsupportedAccess:
    case Device::DeviceRegionError::UnsupportedWidth:
    case Device::DeviceRegionError::MisalignedAccess:
    case Device::DeviceRegionError::MappingNotSupported:
    case Device::DeviceRegionError::ReferenceExhausted:
    case Device::DeviceRegionError::AccessReferenceUnderflow:
    case Device::DeviceRegionError::MappingReferenceUnderflow:
    case Device::DeviceRegionError::InUse:
    case Device::DeviceRegionError::InvariantViolation:
        return FaultFrameMappingError::InvalidDeviceRegion;
    }
    return FaultFrameMappingError::InvalidDeviceRegion;
}

[[nodiscard]] constexpr bool TryGetRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                            Foundation::UInt64& end) noexcept
{
    if ((pages == 0U) || (pages > (MaximumUInt64 / MappingPageSizeBytes)))
    {
        return false;
    }
    const Foundation::UInt64 sizeBytes = pages * MappingPageSizeBytes;
    if (start > (MaximumUInt64 - sizeBytes))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] bool ReservedBytesAreZero(const FaultFrameMappingRequest& request) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(request.Reserved); ++index)
    {
        if (request.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool FaultAccessIsAllowed(const FaultAccess access, const PagePermissions permissions) noexcept
{
    switch (access)
    {
    case FaultAccess::Read:
        return permissions.Contains(PagePermission::Read);
    case FaultAccess::Write:
        return permissions.Contains(PagePermission::Write);
    case FaultAccess::Execute:
        return permissions.Contains(PagePermission::Execute);
    }
    return false;
}
} // namespace

FaultFrameMappingError
FaultFrameMappingPreparation::Prepare(Object::ObjectRegistry& registry, const FaultToken& faultToken,
                                      const FaultResolutionClaimToken claim, const MemoryObjectId memoryObjectId,
                                      MemoryObject& memoryObject, const Device::DeviceRegionId deviceRegionId,
                                      Device::DeviceRegion* const deviceRegion, const FrameId frameId, Frame& frame,
                                      const FaultFrameMappingRequest& request) noexcept
{
    if (m_isPrepared)
    {
        return FaultFrameMappingError::AlreadyPrepared;
    }

    const auto claimResult = faultToken.GetResolutionClaim(claim);
    if (!claimResult.HasValue())
    {
        return FaultFrameMappingError::InvalidClaim;
    }
    const FaultResolutionClaim& resolutionClaim = claimResult.GetValue();
    if (!memoryObjectId.IsValid())
    {
        return FaultFrameMappingError::InvalidMemoryObject;
    }
    if (!frameId.IsValid())
    {
        return FaultFrameMappingError::InvalidFrame;
    }
    if (!resolutionClaim.Thread.IsFromRegistry(registry) || !resolutionClaim.AddressSpace.IsFromRegistry(registry) ||
        !memoryObjectId.IsFromRegistry(registry) || !frameId.IsFromRegistry(registry) ||
        (deviceRegionId.IsValid() && !deviceRegionId.IsFromRegistry(registry)))
    {
        return FaultFrameMappingError::RegistryMismatch;
    }

    const Foundation::UInt64 virtualStart = request.VirtualStart.GetValue();
    const Foundation::UInt64 requestedPages = request.Pages.GetValue();
    Foundation::UInt64 virtualEnd{0U};
    if (((virtualStart % MappingPageSizeBytes) != 0U) || !TryGetRangeEnd(virtualStart, requestedPages, virtualEnd) ||
        !request.Permissions.HasOnlyKnownBits() || !request.Permissions.Contains(PagePermission::Read) ||
        (request.Permissions.Contains(PagePermission::Write) &&
         request.Permissions.Contains(PagePermission::Execute)) ||
        !ReservedBytesAreZero(request))
    {
        return FaultFrameMappingError::InvalidRequest;
    }
    if ((resolutionClaim.Fault.Privilege == FaultPrivilege::User) &&
        !request.Permissions.Contains(PagePermission::User))
    {
        return FaultFrameMappingError::AccessDenied;
    }
    if (!FaultAccessIsAllowed(resolutionClaim.Fault.Access, request.Permissions))
    {
        return FaultFrameMappingError::AccessDenied;
    }
    if ((resolutionClaim.Fault.VirtualAddress < virtualStart) || (resolutionClaim.Fault.VirtualAddress >= virtualEnd))
    {
        return FaultFrameMappingError::FaultOutsideMapping;
    }

    const Object::ObjectPinError memoryObjectPinError =
        registry.PinObject(memoryObjectId.GetObjectId(), Object::ObjectType::MemoryObject, m_memoryObjectPin);
    if (memoryObjectPinError != Object::ObjectPinError::Ok)
    {
        return MapMemoryObjectPinError(memoryObjectPinError);
    }
    if (&m_memoryObjectPin.GetObject() != &memoryObject)
    {
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::MemoryObjectInstanceMismatch;
    }
    const MemoryObjectSnapshot memoryObjectSnapshot = memoryObject.GetSnapshot();
    if (memoryObjectSnapshot.State != MemoryObjectState::Ready)
    {
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::InvalidMemoryObject;
    }
    const Foundation::UInt64 memoryObjectPages = memoryObjectSnapshot.Descriptor.Pages.GetValue();
    if ((request.MemoryObjectOffsetPages > memoryObjectPages) ||
        (requestedPages > (memoryObjectPages - request.MemoryObjectOffsetPages)))
    {
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::MemoryObjectRangeExceeded;
    }
    if ((request.Permissions.GetBits() &
         static_cast<Foundation::UInt8>(~memoryObjectSnapshot.Descriptor.MaximumPermissions.GetBits())) != 0U)
    {
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::MemoryObjectAccessDenied;
    }

    const Object::ObjectPinError pinError =
        registry.PinObject(frameId.GetObjectId(), Object::ObjectType::Frame, m_framePin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        m_memoryObjectPin.Reset();
        return MapFramePinError(pinError);
    }
    if (&m_framePin.GetObject() != &frame)
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::FrameInstanceMismatch;
    }

    const FrameSnapshot frameSnapshot = frame.GetSnapshot();
    if (frameSnapshot.State != FrameState::Ready)
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::InvalidFrame;
    }

    const Foundation::UInt64 framePages = frameSnapshot.Descriptor.Pages.GetValue();
    if ((request.FrameOffsetPages > framePages) || (requestedPages > (framePages - request.FrameOffsetPages)))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::FrameRangeExceeded;
    }
    if ((frameSnapshot.Descriptor.CachePolicy != memoryObjectSnapshot.Descriptor.CachePolicy) &&
        !((frameSnapshot.Descriptor.Kind == FrameKind::Device) &&
          IsDeviceCachePolicyCompatible(frameSnapshot.Descriptor.CachePolicy,
                                        memoryObjectSnapshot.Descriptor.CachePolicy)))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::MemoryObjectCacheMismatch;
    }
    if ((frameSnapshot.Descriptor.Kind == FrameKind::Device) && request.Permissions.Contains(PagePermission::Execute))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::DeviceExecutionDenied;
    }

    const Foundation::UInt64 physicalOffset = request.FrameOffsetPages * MappingPageSizeBytes;
    const Foundation::UInt64 physicalStart = frameSnapshot.Descriptor.PhysicalStart.GetValue() + physicalOffset;

    if (frameSnapshot.Descriptor.Kind == FrameKind::Device)
    {
        if (!deviceRegionId.IsValid() || (deviceRegion == nullptr))
        {
            m_framePin.Reset();
            m_memoryObjectPin.Reset();
            return FaultFrameMappingError::DeviceRegionRequired;
        }
        const Object::ObjectPinError deviceRegionPinError =
            registry.PinObject(deviceRegionId.GetObjectId(), Object::ObjectType::DeviceRegion, m_deviceRegionPin);
        if (deviceRegionPinError != Object::ObjectPinError::Ok)
        {
            m_framePin.Reset();
            m_memoryObjectPin.Reset();
            return MapDeviceRegionPinError(deviceRegionPinError);
        }
        if (&m_deviceRegionPin.GetObject() != deviceRegion)
        {
            m_deviceRegionPin.Reset();
            m_framePin.Reset();
            m_memoryObjectPin.Reset();
            return FaultFrameMappingError::DeviceRegionInstanceMismatch;
        }
        const Device::DeviceRegionError deviceRegionError =
            deviceRegion->ValidateMapping(PhysicalAddress{physicalStart}, request.Pages, request.Permissions,
                                          memoryObjectSnapshot.Descriptor.CachePolicy);
        if (deviceRegionError != Device::DeviceRegionError::None)
        {
            m_deviceRegionPin.Reset();
            m_framePin.Reset();
            m_memoryObjectPin.Reset();
            return MapDeviceRegionError(deviceRegionError);
        }
    }
    else if (deviceRegionId.IsValid() || (deviceRegion != nullptr))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return FaultFrameMappingError::UnexpectedDeviceRegion;
    }

    m_plan = FaultFrameMappingPlan{
        .Claim = resolutionClaim.Token,
        .AddressSpace = resolutionClaim.AddressSpace,
        .MemoryObject = memoryObjectId,
        .DeviceRegion = deviceRegionId,
        .Frame = frameId,
        .PhysicalStart = PhysicalAddress{physicalStart},
        .VirtualStart = request.VirtualStart,
        .Pages = request.Pages,
        .MemoryObjectOffsetPages = request.MemoryObjectOffsetPages,
        .Permissions = request.Permissions,
        .CachePolicy = memoryObjectSnapshot.Descriptor.CachePolicy,
        .Kind = frameSnapshot.Descriptor.Kind,
        .Commitment = memoryObjectSnapshot.Descriptor.Commitment,
        .Reserved = {},
    };
    m_isPrepared = true;
    return FaultFrameMappingError::None;
}

void FaultFrameMappingPreparation::Reset() noexcept
{
    m_memoryObjectPin.Reset();
    m_deviceRegionPin.Reset();
    m_framePin.Reset();
    m_plan = FaultFrameMappingPlan{};
    m_isPrepared = false;
}

bool FaultFrameMappingPreparation::IsPrepared() const noexcept
{
    return m_isPrepared;
}

const FaultFrameMappingPlan& FaultFrameMappingPreparation::GetPlan() const noexcept
{
    return m_plan;
}
} // namespace Asteria::Kernel::Memory
