// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PINNED_FRAME_MAPPING_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PINNED_FRAME_MAPPING_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Kernel/Object/ObjectPin.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Memory
{
struct PinnedFrameMappingRequest
{
    VirtualAddress VirtualStart;
    PageCount Pages;
    Foundation::UInt64 MemoryObjectOffsetPages;
    Foundation::UInt64 FrameOffsetPages;
    PagePermissions Permissions;
    Foundation::UInt8 Reserved[7];
};

enum class PinnedFrameMappingError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    RegistryMismatch,
    InvalidMemoryObject,
    MemoryObjectNotFound,
    MemoryObjectTypeMismatch,
    MemoryObjectPinUnavailable,
    MemoryObjectInstanceMismatch,
    MemoryObjectRangeExceeded,
    MemoryObjectAccessDenied,
    MemoryObjectCacheMismatch,
    MemoryObjectCommitmentMismatch,
    InvalidFrame,
    FrameNotFound,
    FrameTypeMismatch,
    FramePinUnavailable,
    FrameInstanceMismatch,
    FrameRangeExceeded,
    UnsupportedFrameKind,
    InvalidRequest,
};

/// Retains exact pinned MemoryObject and RAM Frame generations and prepares one proactive mapping.
/// Supervisor and user mappings are both admitted only within the MemoryObject's maximum
/// permissions. This class validates safety and residency policy; it grants no fault or mapping
/// authority. Lifetime and concurrency: - registry, MemoryObject, Frame, and their storage outlive
/// this owner; - callers serialize this owner and do not call it from interrupt context; - a
/// successful owner remains alive until AddressSpace accepts or rejects the prepared record.
class PinnedFrameMappingPreparation final
{
public:
    constexpr PinnedFrameMappingPreparation() noexcept = default;
    ~PinnedFrameMappingPreparation() noexcept = default;

    PinnedFrameMappingPreparation(const PinnedFrameMappingPreparation&) = delete;
    PinnedFrameMappingPreparation& operator=(const PinnedFrameMappingPreparation&) = delete;
    PinnedFrameMappingPreparation(PinnedFrameMappingPreparation&&) = delete;
    PinnedFrameMappingPreparation& operator=(PinnedFrameMappingPreparation&&) = delete;

    /// Every failure before the first success leaves this owner fresh and
    /// unpinned. Later calls preserve the successful record and pins and return
    /// AlreadyPrepared.
    [[nodiscard]] PinnedFrameMappingError Prepare(Object::ObjectRegistry& registry, MemoryObjectId memoryObjectId,
                                                  MemoryObject& memoryObject, FrameId frameId, Frame& frame,
                                                  const PinnedFrameMappingRequest& request) noexcept;

    /// As Prepare, where a device Frame needs region, the DeviceRegion whose pages it holds, which
    /// the AddressSpace retains for the mapping; a RAM Frame needs none.
    [[nodiscard]] PinnedFrameMappingError Prepare(Object::ObjectRegistry& registry, MemoryObjectId memoryObjectId,
                                                  MemoryObject& memoryObject, FrameId frameId, Frame& frame,
                                                  const PinnedFrameMappingRequest& request,
                                                  Device::DeviceRegionId region) noexcept;

    [[nodiscard]] bool IsPrepared() const noexcept;

    /// Returns this owner to fresh, releasing its pins. Preconditions: no
    /// AddressSpace is still consuming the prepared record.
    void Reset() noexcept;

    /// Preconditions:
    /// - IsPrepared is true;
    /// - this owner remains alive while the returned record is consumed.
    [[nodiscard]] const AddressSpaceMappingRecord& GetMapping() const noexcept;
    [[nodiscard]] bool IsReadOnlyBootstrapFrame() const noexcept
    {
        return m_readOnlyBootstrap;
    }

private:
    Object::ObjectPin m_memoryObjectPin;
    Object::ObjectPin m_framePin;
    AddressSpaceMappingRecord m_mapping{};
    bool m_isPrepared{false};
    bool m_readOnlyBootstrap{false};
};

static_assert(sizeof(PinnedFrameMappingRequest) == 40U);
static_assert(alignof(PinnedFrameMappingRequest) == 8U);
static_assert(__is_aggregate(PinnedFrameMappingRequest));
static_assert(__is_standard_layout(PinnedFrameMappingRequest));
static_assert(__is_trivially_copyable(PinnedFrameMappingRequest));
static_assert(sizeof(PinnedFrameMappingError) == 1U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_PINNED_FRAME_MAPPING_HPP_INCLUDED
