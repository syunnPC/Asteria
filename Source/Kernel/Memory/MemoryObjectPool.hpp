// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_MEMORY_OBJECT_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_MEMORY_OBJECT_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Object/SlotBitmap.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
enum class MemoryObjectPoolState : Foundation::UInt8
{
    Ready,
    Faulted,
};

/// One ObjectCreate of a MemoryObject.
struct MemoryObjectCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
    PageCount Pages;
    PagePermissions MaximumPermissions;
    /// Write-back for RAM, uncached or write-combining for a device Frame's registers.
    MemoryCachePolicy CachePolicy;
};

struct MemoryObjectCreation
{
    Object::ObjectId MemoryObject;
    Object::CapabilityHandle Capability;
};

struct MemoryObjectPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    MemoryObjectPoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns the MemoryObjects user code creates through the management ABI: Pinned, write-back logical
/// ranges whose Initialize charges committed and resident pages and sizeof(MemoryObject) to the
/// ResourceDomain a Manage capability names, as boot's do. Create, Destroy and Collect follow
/// EndpointPool; collecting a MemoryObject quiesces it, which releases its charge and its domain
/// pin. An AddressSpace pins a MemoryObject per mapping, so a mapped one is collected only after it
/// is unmapped.
/// Like FrameObjectPool's, the slots live in a slab the platform attaches once; a free slot is
/// found in a bitmap and an object's slot from its address, so no operation scans the slab.
class MemoryObjectPool final
{
    struct Slot;

public:
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::MemoryObject)]};
    /// A MemoryObject charges metadata only (the Frames behind its mappings carry the pages), so
    /// the bound is on the range one maps: 16 MiB, room for a scanout up to 2560x1600.
    static constexpr Foundation::UInt64 MaximumPages{8192U};

    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Map)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr MemoryObjectPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    MemoryObjectPool(const MemoryObjectPool&) = delete;
    MemoryObjectPool& operator=(const MemoryObjectPool&) = delete;
    MemoryObjectPool(MemoryObjectPool&&) = delete;
    MemoryObjectPool& operator=(MemoryObjectPool&&) = delete;

    /// Builds the free slots in slab, SlabBytes bytes aligned to
    /// SlabAlignment that outlive the pool and nothing else uses; once
    /// only, before the first creation. False leaves the pool unattached.
    [[nodiscard]] bool AttachSlab(void* slab) noexcept;

    [[nodiscard]] Foundation::Result<MemoryObjectCreation, Object::ObjectPoolFailure>
    Create(const MemoryObjectCreateRequest& request) noexcept;

    [[nodiscard]] Foundation::Result<Object::ObjectId, Object::ObjectPoolFailure>
    Destroy(Object::CapabilitySpace& space, Object::CapabilityHandle memoryObject) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;

    [[nodiscard]] MemoryObjectPoolSnapshot GetSnapshot() const noexcept;

private:
    using SlotState = Object::PoolSlotState;

    struct Slot
    {
        alignas(MemoryObject) Foundation::UInt8 Storage[sizeof(MemoryObject)];
        MemoryObject* Instance;
        /// The domain the MemoryObject charges (it pins it itself).
        Resource::ResourceDomain* Domain;
        Object::ObjectId Identity;
        SlotState State;
    };

public:
    /// The slab AttachSlab takes: Capacity slots.
    static constexpr Foundation::Size SlabBytes{Capacity * sizeof(Slot)};
    static constexpr Foundation::Size SlabAlignment{alignof(Slot)};

private:
    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    /// The slot whose MemoryObject instance is, from its address; null for
    /// an object outside the slab or a slot without that instance.
    [[nodiscard]] Slot* FindSlotLocked(const Object::IKernelObject& instance) const noexcept;
    /// Moves a slot between the free, active and quiescing sets.
    void SetStateLocked(Slot& slot, SlotState state) noexcept;
    /// Quiesces and ends the slot's MemoryObject and frees the slot.
    [[nodiscard]] Object::ObjectPoolFailure ReleaseSlotLocked(Slot& slot) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure FaultLocked(Object::ObjectPoolFailure failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot* m_slots{nullptr};
    Object::PoolSlotSets<Capacity> m_sets{};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    MemoryObjectPoolState m_state{MemoryObjectPoolState::Ready};
};

static_assert(sizeof(MemoryObjectPoolState) == 1U);
static_assert(sizeof(MemoryObjectPoolSnapshot) == 56U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_MEMORY_OBJECT_POOL_HPP_INCLUDED
