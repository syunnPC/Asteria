// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FRAME_OBJECT_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FRAME_OBJECT_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Memory/DeviceFrameRegions.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/FramePool.hpp"
#include "Kernel/Memory/IFrameScrubber.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Object/SlotBitmap.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
enum class FrameObjectPoolState : Foundation::UInt8
{
    Ready,
    Faulted,
};

/// One ObjectCreate of a Frame.
struct FrameCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
    PageCount Pages;
    /// A device Frame's memory-mapped DeviceRegion, a capability with Map in Space, and the page of
    /// the region its first page is; zero for a Frame of RAM from the frame pool.
    Object::CapabilityHandle Region;
    Foundation::UInt64 RegionOffsetPages;
    /// A Frame of RAM's physical alignment in pages, a power of two up to
    /// FramePool::MaximumAlignmentPages; zero or one for a page. A device Frame's pages are its region's, so
    /// it takes none.
    Foundation::UInt64 AlignmentPages{0U};
    /// The legacy RAM request limits addresses to 32 bits. The all-RAM request also accepts limit 0.
    Foundation::UInt8 AddressLimitBits{32U};
};

struct FrameCreation
{
    Object::ObjectId Frame;
    Object::CapabilityHandle Capability;
    PhysicalAddress PhysicalStart;
};

struct FrameObjectPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::Size ZeroingSlots;
    Foundation::Size PeakZeroingSlots;
    Foundation::UInt64 ZeroDeferrals;
    Foundation::UInt64 ZeroFaults;
    Foundation::UInt64 CreationZeroedPages;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    FrameObjectPoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns the Frames user code creates through the management ABI: each a run of zeroed pages from a
/// FramePool, write-back RAM, charged sizeof(CreatedFrame) to the ResourceDomain a Manage
/// capability names; the Pinned MemoryObject that maps it carries the page charge, as at boot.
/// Create, Destroy and Collect follow EndpointPool: the capability is minted under the authority,
/// every failure undoes the earlier steps, Destroy makes an owned Frame quiescing, and Collect
/// reclaims it once unreferenced. An AddressSpace pins a Frame per mapping, so a mapped Frame is
/// collected only after it is unmapped. A collected Frame's pages return to the FramePool dirty.
/// The slots live in a slab the platform places (the AMD64 metadata
/// arena, IA-64's runtime page) and attaches once (AttachSlab); until then creation is exhausted. A
/// free slot is found in a bitmap (the lowest free one), a Frame's slot from its address, and
/// Collect and a drain visit only the quiescing and live slots, so no operation scans the slab.
class FrameObjectPool final : public IDeviceFrameRegions
{
    struct Slot;

public:
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::Frame)]};

    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{
        Object::CapabilityRight::Inspect} // Creators retain CPU write authority.
                                                                .With(Object::CapabilityRight::Read)
                                                                .With(Object::CapabilityRight::Write)
                                                                .With(Object::CapabilityRight::Map)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry; both and pages outlive this pool;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr FrameObjectPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer,
                              FramePool& pages) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}, m_pages{pages}
    {
    }

    FrameObjectPool(const FrameObjectPool&) = delete;
    FrameObjectPool& operator=(const FrameObjectPool&) = delete;
    FrameObjectPool(FrameObjectPool&&) = delete;
    FrameObjectPool& operator=(FrameObjectPool&&) = delete;

    /// Builds the free slots in slab, SlabBytes bytes aligned to
    /// SlabAlignment that outlive the pool and nothing else uses; once
    /// only, before the first creation. False leaves the pool unattached.
    [[nodiscard]] bool AttachSlab(void* slab) noexcept;

    [[nodiscard]] bool ConfigureScrubber(IFrameScrubber& scrubber) noexcept;

    /// Test firmware only: simulate a previous owner returning the initial
    /// clean range at the first RAM creation, after Collect has run.
    void EnableCreationZeroProbe() noexcept
    {
        m_creationZeroProbe = true;
    }

    [[nodiscard]] Foundation::Result<FrameCreation, Object::ObjectPoolFailure>
    Create(const FrameCreateRequest& request) noexcept;

    [[nodiscard]] Foundation::Result<Object::ObjectId, Object::ObjectPoolFailure>
    Destroy(Object::CapabilitySpace& space, Object::CapabilityHandle frame) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;

    [[nodiscard]] FrameObjectPoolSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Device::DeviceRegionId GetDeviceRegion(const Frame& frame) const noexcept override;

    /// A Frame of pages charges them, committed and resident, and its metadata: it owns the
    /// physical run.
    [[nodiscard]] static constexpr Resource::ResourceReservation GetReservation(const PageCount pages) noexcept
    {
        return Resource::ResourceReservation{
            .CommittedMemoryPages = pages.GetValue(),
            .ResidentMemoryPages = pages.GetValue(),
            .KernelMetadataBytes = sizeof(CreatedFrame),
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .Objects = {0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U},
        };
    }

    /// A device Frame charges its metadata only: its pages are a device's, not memory.
    [[nodiscard]] static constexpr Resource::ResourceReservation GetDeviceReservation() noexcept
    {
        return Resource::ResourceReservation{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = sizeof(CreatedFrame),
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .Objects = {0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U},
        };
    }

private:
    using SlotState = Object::PoolSlotState;

    struct Slot
    {
        alignas(CreatedFrame) Foundation::UInt8 Storage[sizeof(CreatedFrame)];
        CreatedFrame* Instance;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin DomainPin;
        Resource::ResourceReservation Charge;
        PhysicalAddress PhysicalStart;
        PageCount Pages;
        Foundation::UInt64 DirtyMask[PhysicalPageAllocator::DirtyWords];
        Object::ObjectId Identity;
        /// A device Frame's region and the pin that keeps it registered while
        /// the Frame names its pages; invalid for RAM.
        Device::DeviceRegionId Region;
        Object::ObjectPin RegionPin;
        MemoryCachePolicy CachePolicy;
        SlotState State;
    };

public:
    /// The slab AttachSlab takes: Capacity slots.
    static constexpr Foundation::Size SlabBytes{Capacity * sizeof(Slot)};
    static constexpr Foundation::Size SlabAlignment{alignof(Slot)};

private:
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure ReserveLocked(const FrameCreateRequest& request,
                                                                         Slot*& output) noexcept;
    [[nodiscard, gnu::noinline]] FrameScrubError ZeroSlot(Slot& slot) noexcept;
    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    /// The slot whose Frame instance is, from its address; null for an
    /// object outside the slab or a slot without that instance.
    [[nodiscard]] Slot* FindSlotLocked(const Object::IKernelObject& instance) const noexcept;
    [[nodiscard]] Foundation::Size IndexOf(const Slot& slot) const noexcept;
    /// Moves a slot between the free, active and quiescing sets.
    void SetStateLocked(Slot& slot, SlotState state) noexcept;
    /// Pins the memory-mapped region request names (Map in space) and sets
    /// the slot's pages and cache policy to the requested ones of it.
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure BindRegionLocked(Slot& slot, Object::CapabilitySpace& space,
                                                                            const FrameCreateRequest& request) noexcept;
    /// Ends the slot's Frame, returns its pages (dirty unless clean), releases
    /// its charge and pin, and frees the slot.
    [[nodiscard]] Object::ObjectPoolFailure ReleaseSlotLocked(Slot& slot, bool clean) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure FaultLocked(Object::ObjectPoolFailure failure) noexcept;
    /// Initializes, registers and mints the Frame of a slot that holds its
    /// domain pin, charge and page run; on failure everything is undone.
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure PublishLocked(Slot& slot, Object::CapabilitySpace& space,
                                                                         Object::CapabilityHandle authority,
                                                                         Resource::ResourceDomain& domain,
                                                                         Object::CapabilityHandle& minted) noexcept;
    /// Undoes a registered, unminted Frame: destroys and collects its
    /// registration and releases the slot with clean pages.
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure AbandonLocked(Slot& slot,
                                                                         Object::CapabilityError mintError) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    FramePool& m_pages;
    IFrameScrubber* m_scrubber{nullptr};
    [[nodiscard, gnu::noinline]] bool PrimeCreationZeroProbe() noexcept;
    bool m_creationZeroProbe{false};
    Foundation::Size m_peakZeroing{0U};
    Foundation::UInt64 m_zeroDeferrals{0U};
    Foundation::UInt64 m_zeroFaults{0U};
    Foundation::UInt64 m_creationZeroedPages{0U};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot* m_slots{nullptr};
    Object::PoolSlotSets<Capacity> m_sets{};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    FrameObjectPoolState m_state{FrameObjectPoolState::Ready};
};

static_assert(sizeof(FrameObjectPoolState) == 1U);
static_assert(sizeof(FrameObjectPoolSnapshot) == 96U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FRAME_OBJECT_POOL_HPP_INCLUDED
