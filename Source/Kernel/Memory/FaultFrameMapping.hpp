// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_FRAME_MAPPING_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_FRAME_MAPPING_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Memory
{
struct FaultFrameMappingRequest
{
    VirtualAddress VirtualStart;
    PageCount Pages;
    Foundation::UInt64 MemoryObjectOffsetPages;
    Foundation::UInt64 FrameOffsetPages;
    PagePermissions Permissions;
    Foundation::UInt8 Reserved[7];
};

struct FaultFrameMappingPlan
{
    FaultResolutionClaimToken Claim;
    AddressSpaceId AddressSpace;
    MemoryObjectId MemoryObject;
    Device::DeviceRegionId DeviceRegion;
    FrameId Frame;
    PhysicalAddress PhysicalStart;
    VirtualAddress VirtualStart;
    PageCount Pages;
    Foundation::UInt64 MemoryObjectOffsetPages;
    PagePermissions Permissions;
    MemoryCachePolicy CachePolicy;
    FrameKind Kind;
    MemoryCommitment Commitment;
    Foundation::UInt8 Reserved[4];
};

enum class FaultFrameMappingError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    InvalidClaim,
    RegistryMismatch,
    InvalidMemoryObject,
    MemoryObjectNotFound,
    MemoryObjectTypeMismatch,
    MemoryObjectPinUnavailable,
    MemoryObjectInstanceMismatch,
    MemoryObjectRangeExceeded,
    MemoryObjectAccessDenied,
    MemoryObjectCacheMismatch,
    DeviceRegionRequired,
    UnexpectedDeviceRegion,
    InvalidDeviceRegion,
    DeviceRegionNotFound,
    DeviceRegionTypeMismatch,
    DeviceRegionPinUnavailable,
    DeviceRegionInstanceMismatch,
    DeviceRegionRangeExceeded,
    DeviceRegionAccessDenied,
    DeviceRegionCacheMismatch,
    InvalidFrame,
    FrameNotFound,
    FrameTypeMismatch,
    FramePinUnavailable,
    FrameInstanceMismatch,
    InvalidRequest,
    FrameRangeExceeded,
    FaultOutsideMapping,
    AccessDenied,
    DeviceExecutionDenied,
};

/// Retains authorized MemoryObject and Frame generations, plus the exact DeviceRegion for
/// device-backed Frames, and prepares an architecture-neutral mapping plan for one active
/// fault-resolution claim. This class validates safety properties, not pager authority. Before
/// calling Prepare, the capability layer authorizes the exact AddressSpace, MemoryObject, Frame,
/// required DeviceRegion, and requested protection. The target mapper consumes the plan while this
/// owner remains alive, then commits or cancels the claim before destroying it. Lifetime and
/// concurrency: - registry, faultToken, MemoryObject, optional DeviceRegion, Frame, and their
/// storage outlive this owner. - claim remains active and exclusively owned throughout
/// preparation/mapping. - callers serialize this owner and do not call it from interrupt context.
class FaultFrameMappingPreparation final
{
public:
    constexpr FaultFrameMappingPreparation() noexcept = default;
    ~FaultFrameMappingPreparation() noexcept = default;

    FaultFrameMappingPreparation(const FaultFrameMappingPreparation&) = delete;
    FaultFrameMappingPreparation& operator=(const FaultFrameMappingPreparation&) = delete;
    FaultFrameMappingPreparation(FaultFrameMappingPreparation&&) = delete;
    FaultFrameMappingPreparation& operator=(FaultFrameMappingPreparation&&) = delete;

    /// Validates the authoritative claim and request, then retains the exact
    /// registered MemoryObject and Frame, and for device memory the exact
    /// DeviceRegion. Every failure before the first success leaves this owner
    /// fresh and unpinned; later calls preserve the prepared plan and return
    /// AlreadyPrepared.
    [[nodiscard]] FaultFrameMappingError Prepare(Object::ObjectRegistry& registry, const FaultToken& faultToken,
                                                 FaultResolutionClaimToken claim, MemoryObjectId memoryObjectId,
                                                 MemoryObject& memoryObject, Device::DeviceRegionId deviceRegionId,
                                                 Device::DeviceRegion* deviceRegion, FrameId frameId, Frame& frame,
                                                 const FaultFrameMappingRequest& request) noexcept;

    [[nodiscard]] bool IsPrepared() const noexcept;

    /// Releases every retained pin and returns to fresh. The caller has
    /// finished or abandoned the mapper that consumed the plan.
    void Reset() noexcept;

    /// Preconditions:
    /// - IsPrepared is true.
    /// - This owner remains alive while the returned plan is consumed.
    [[nodiscard]] const FaultFrameMappingPlan& GetPlan() const noexcept;

private:
    Object::ObjectPin m_memoryObjectPin;
    Object::ObjectPin m_deviceRegionPin;
    Object::ObjectPin m_framePin;
    FaultFrameMappingPlan m_plan{};
    bool m_isPrepared{false};
};

static_assert(sizeof(PagePermissions) == 1U);
static_assert(sizeof(FaultFrameMappingRequest) == 40U);
static_assert(alignof(FaultFrameMappingRequest) == 8U);
static_assert(sizeof(FaultFrameMappingPlan) == 120U);
static_assert(alignof(FaultFrameMappingPlan) == 8U);
static_assert(sizeof(FaultFrameMappingError) == 1U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FAULT_FRAME_MAPPING_HPP_INCLUDED
