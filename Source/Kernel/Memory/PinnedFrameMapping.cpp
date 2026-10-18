// SPDX-License-Identifier: Apache-2.0

#include "PinnedFrameMapping.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

[[nodiscard]] constexpr PinnedFrameMappingError MapFramePinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return PinnedFrameMappingError::FrameNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return PinnedFrameMappingError::FrameTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return PinnedFrameMappingError::FramePinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return PinnedFrameMappingError::InvalidFrame;
    case Object::ObjectPinError::Ok:
        return PinnedFrameMappingError::None;
    }
    return PinnedFrameMappingError::InvalidFrame;
}

[[nodiscard]] constexpr PinnedFrameMappingError MapMemoryObjectPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::ObjectNotFound:
        return PinnedFrameMappingError::MemoryObjectNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return PinnedFrameMappingError::MemoryObjectTypeMismatch;
    case Object::ObjectPinError::ReferenceExhausted:
        return PinnedFrameMappingError::MemoryObjectPinUnavailable;
    case Object::ObjectPinError::InvalidArgument:
    case Object::ObjectPinError::OutputOccupied:
        return PinnedFrameMappingError::InvalidMemoryObject;
    case Object::ObjectPinError::Ok:
        return PinnedFrameMappingError::None;
    }
    return PinnedFrameMappingError::InvalidMemoryObject;
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

[[nodiscard]] bool ReservedBytesAreZero(const PinnedFrameMappingRequest& request) noexcept
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
} // namespace

// A RAM Frame's preparation names no region; the empty one lives in this
// frame rather than every caller's.
PinnedFrameMappingError PinnedFrameMappingPreparation::Prepare(Object::ObjectRegistry& registry,
                                                               const MemoryObjectId memoryObjectId,
                                                               MemoryObject& memoryObject, const FrameId frameId,
                                                               Frame& frame,
                                                               const PinnedFrameMappingRequest& request) noexcept
{
    return Prepare(registry, memoryObjectId, memoryObject, frameId, frame, request, Device::DeviceRegionId{});
}

PinnedFrameMappingError PinnedFrameMappingPreparation::Prepare(Object::ObjectRegistry& registry,
                                                               const MemoryObjectId memoryObjectId,
                                                               MemoryObject& memoryObject, const FrameId frameId,
                                                               Frame& frame, const PinnedFrameMappingRequest& request,
                                                               const Device::DeviceRegionId region) noexcept
{
    if (m_isPrepared)
    {
        return PinnedFrameMappingError::AlreadyPrepared;
    }
    if (!memoryObjectId.IsValid())
    {
        return PinnedFrameMappingError::InvalidMemoryObject;
    }
    if (!frameId.IsValid())
    {
        return PinnedFrameMappingError::InvalidFrame;
    }
    if (!memoryObjectId.IsFromRegistry(registry) || !frameId.IsFromRegistry(registry))
    {
        return PinnedFrameMappingError::RegistryMismatch;
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
        return PinnedFrameMappingError::InvalidRequest;
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
        return PinnedFrameMappingError::MemoryObjectInstanceMismatch;
    }

    const MemoryObjectSnapshot memoryObjectSnapshot = memoryObject.GetSnapshot();
    if (memoryObjectSnapshot.State != MemoryObjectState::Ready)
    {
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::InvalidMemoryObject;
    }
    const Foundation::UInt64 memoryObjectPages = memoryObjectSnapshot.Descriptor.Pages.GetValue();
    if ((request.MemoryObjectOffsetPages > memoryObjectPages) ||
        (requestedPages > (memoryObjectPages - request.MemoryObjectOffsetPages)))
    {
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::MemoryObjectRangeExceeded;
    }
    if ((request.Permissions.GetBits() &
         static_cast<Foundation::UInt8>(~memoryObjectSnapshot.Descriptor.MaximumPermissions.GetBits())) != 0U)
    {
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::MemoryObjectAccessDenied;
    }
    if (memoryObjectSnapshot.Descriptor.Commitment != MemoryCommitment::Pinned)
    {
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::MemoryObjectCommitmentMismatch;
    }

    const Object::ObjectPinError framePinError =
        registry.PinObject(frameId.GetObjectId(), Object::ObjectType::Frame, m_framePin);
    if (framePinError != Object::ObjectPinError::Ok)
    {
        m_memoryObjectPin.Reset();
        return MapFramePinError(framePinError);
    }
    if (&m_framePin.GetObject() != &frame)
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::FrameInstanceMismatch;
    }

    const FrameSnapshot frameSnapshot = frame.GetSnapshot();
    if (frameSnapshot.State != FrameState::Ready)
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::InvalidFrame;
    }
    // A device Frame maps only with its region, never executable; RAM never
    // with one.
    const bool device = frameSnapshot.Descriptor.Kind == FrameKind::Device;
    if ((device && (!region.IsValid() || !region.IsFromRegistry(registry) ||
                    request.Permissions.Contains(PagePermission::Execute))) ||
        (!device && ((frameSnapshot.Descriptor.Kind != FrameKind::Ram) || region.IsValid())))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::UnsupportedFrameKind;
    }

    const Foundation::UInt64 framePages = frameSnapshot.Descriptor.Pages.GetValue();
    if ((request.FrameOffsetPages > framePages) || (requestedPages > (framePages - request.FrameOffsetPages)))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::FrameRangeExceeded;
    }
    if ((frameSnapshot.Descriptor.CachePolicy != memoryObjectSnapshot.Descriptor.CachePolicy) &&
        !((frameSnapshot.Descriptor.Kind == FrameKind::Device) &&
          IsDeviceCachePolicyCompatible(frameSnapshot.Descriptor.CachePolicy,
                                        memoryObjectSnapshot.Descriptor.CachePolicy)))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::MemoryObjectCacheMismatch;
    }
    if (request.FrameOffsetPages > (MaximumUInt64 / MappingPageSizeBytes))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::FrameRangeExceeded;
    }
    const Foundation::UInt64 physicalOffset = request.FrameOffsetPages * MappingPageSizeBytes;
    const Foundation::UInt64 framePhysicalStart = frameSnapshot.Descriptor.PhysicalStart.GetValue();
    if (framePhysicalStart > (MaximumUInt64 - physicalOffset))
    {
        m_framePin.Reset();
        m_memoryObjectPin.Reset();
        return PinnedFrameMappingError::InvalidFrame;
    }

    m_mapping = AddressSpaceMappingRecord{
        .MemoryObject = memoryObjectId,
        .DeviceRegion = region,
        .Frame = frameId,
        .VirtualStart = request.VirtualStart,
        .PhysicalStart = PhysicalAddress{framePhysicalStart + physicalOffset},
        .Pages = request.Pages,
        .MemoryObjectOffsetPages = request.MemoryObjectOffsetPages,
        .Permissions = request.Permissions,
        .CachePolicy = memoryObjectSnapshot.Descriptor.CachePolicy,
        .Kind = frameSnapshot.Descriptor.Kind,
        .Commitment = MemoryCommitment::Pinned,
        .RuntimeMapping = true,
        .Reserved = {},
    };
    m_readOnlyBootstrap = frameSnapshot.ReadOnly != 0U;
    m_isPrepared = true;
    return PinnedFrameMappingError::None;
}

bool PinnedFrameMappingPreparation::IsPrepared() const noexcept
{
    return m_isPrepared;
}

void PinnedFrameMappingPreparation::Reset() noexcept
{
    m_memoryObjectPin.Reset();
    m_framePin.Reset();
    m_mapping = AddressSpaceMappingRecord{};
    m_isPrepared = false;
}

const AddressSpaceMappingRecord& PinnedFrameMappingPreparation::GetMapping() const noexcept
{
    return m_mapping;
}
} // namespace Asteria::Kernel::Memory
