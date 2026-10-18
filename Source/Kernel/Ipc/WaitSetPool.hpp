// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_WAIT_SET_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_WAIT_SET_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Ipc
{
enum class WaitSetPoolState : Foundation::UInt8
{
    Ready,
    Faulted,
};

using WaitSetPoolError = Object::ObjectPoolError;
using WaitSetPoolFailure = Object::ObjectPoolFailure;

/// One ObjectCreate of a WaitSet.
struct WaitSetCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
};

struct WaitSetCreation
{
    Object::ObjectId WaitSet;
    Object::CapabilityHandle Capability;
};

struct WaitSetPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    WaitSetPoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns a fixed set of WaitSet slots that user code creates and destroys through the management
/// ABI. Create charges the ResourceDomain named by a Manage capability sizeof(WaitSet) in
/// kernel-metadata bytes, constructs and initializes the WaitSet in a free slot, registers it, and
/// mints the creator's capability under that authority. Destroy makes an owned WaitSet quiescing.
/// Collect reclaims every quiescing slot whose last lease and pin are gone (a blocked waiter pins
/// its WaitSet until its wait completes): ending the WaitSet's lifetime drops every Notification
/// registration and its pin, then the charge and the domain pin are released. A failure that leaves
/// no object behind changes nothing; a rollback that cannot complete faults the pool, which then
/// refuses every operation.
class WaitSetPool final
{
public:
    /// Sixteen: the product's device tier, storage, the Component Manager, the Directory, the
    /// session and the shell, and the applications they launch.
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::WaitSet)]};

    /// The rights of a created WaitSet's first capability: Inspect, Wait,
    /// Transfer, Duplicate, Derive and Manage, without Revoke.
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Wait)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool.
    constexpr WaitSetPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    /// Preconditions:
    /// - every slot is Free.
    ~WaitSetPool() noexcept = default;

    WaitSetPool(const WaitSetPool&) = delete;
    WaitSetPool& operator=(const WaitSetPool&) = delete;
    WaitSetPool(WaitSetPool&&) = delete;
    WaitSetPool& operator=(WaitSetPool&&) = delete;

    [[nodiscard]] Foundation::Result<WaitSetCreation, WaitSetPoolFailure>
    Create(const WaitSetCreateRequest& request) noexcept;

    /// Makes the WaitSet that waitSet, a capability with Manage in space,
    /// names quiescing: every capability to it fails ObjectNotFound from now
    /// on and stays closeable, while a waiter already blocked on it still
    /// completes. WaitSets this pool does not own (a boot WaitSet) are
    /// NotOwned.
    [[nodiscard]] Foundation::Result<Object::ObjectId, WaitSetPoolFailure>
    Destroy(Object::CapabilitySpace& space, Object::CapabilityHandle waitSet) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// Reclaims every quiescing slot whose WaitSet is no longer referenced
    /// and returns how many it reclaimed.
    [[nodiscard]] Foundation::Result<Foundation::Size, WaitSetPoolFailure> Collect() noexcept;

    [[nodiscard]] WaitSetPoolSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] static constexpr Resource::ResourceReservation GetReservation() noexcept
    {
        return Resource::ResourceReservation{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = sizeof(WaitSet),
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .Objects = {0U, 0U, 0U, 0U, 1U, 0U, 0U, 0U},
        };
    }

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing,
    };

    struct Slot
    {
        alignas(WaitSet) Foundation::UInt8 Storage[sizeof(WaitSet)];
        WaitSet* Instance;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin DomainPin;
        Object::ObjectId Identity;
        SlotState State;
    };

    [[nodiscard]] static WaitSetPoolFailure
    MakeFailure(WaitSetPoolError error, Object::CapabilityError capability = Object::CapabilityError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
                Object::ObjectRegistryError registry = Object::ObjectRegistryError::Ok) noexcept
    {
        return Object::MakeObjectPoolFailure(error, capability, resource, registry, 0U);
    }
    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] Slot* FindSlotLocked(const Object::IKernelObject& instance) noexcept;
    /// Mints the creator's capability to a registered slot, or undoes it.
    [[gnu::noinline]] [[nodiscard]] Foundation::Result<WaitSetCreation, WaitSetPoolFailure>
    MintLocked(Slot& slot, const WaitSetCreateRequest& request) noexcept;
    /// Ends the slot's WaitSet, releases its charge and pin, and frees it.
    [[nodiscard]] WaitSetPoolFailure ReleaseSlotLocked(Slot& slot) noexcept;
    [[nodiscard]] WaitSetPoolFailure FaultLocked(WaitSetPoolFailure failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot m_slots[Capacity]{};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    WaitSetPoolState m_state{WaitSetPoolState::Ready};
};

static_assert(sizeof(WaitSetPoolState) == 1U);
static_assert(sizeof(WaitSetPoolSnapshot) == 56U);
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_WAIT_SET_POOL_HPP_INCLUDED
