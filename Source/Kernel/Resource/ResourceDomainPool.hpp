// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Resource
{
enum class ResourceDomainPoolState : Foundation::UInt8
{
    Unconfigured,
    Ready,
    Faulted,
};

using ResourceDomainPoolError = Object::ObjectPoolError;
using ResourceDomainPoolFailure = Object::ObjectPoolFailure;

/// One ObjectCreate of a ResourceDomain.
struct ResourceDomainCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the parent, charged
    /// the child's slot.
    Object::CapabilityHandle Authority;
    /// The child's limits. KernelMetadataBytes and CapabilityHandles must be
    /// zero: user code cannot compute kernel-metadata sizes, so the child
    /// takes its parent's metadata limit, and nothing charges handles yet.
    ResourceDomainLimits Limits;
    Foundation::UInt64 Flags{0U};
    bool ExplicitObjectLimits{false};
};

struct ResourceDomainCreation
{
    Object::ObjectId Domain;
    Object::ResourceDomainId Identity;
    Object::CapabilityHandle Capability;
};

struct ResourceDomainPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 NextIdentity;
    ResourceDomainPoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns a fixed set of child ResourceDomains that user code creates through the management ABI.
/// Create charges the parent, named by a Manage capability, the child's slot in kernel-metadata
/// bytes, initializes the child under it (its limits within the parent's, one descendant slot from
/// every ancestor), gives it the next identity of a counter that never reuses one (the identity is
/// the IPC sender id and the quota key of everything charged to the child), registers it and mints
/// the creator's capability. A failure that leaves no object behind changes nothing; a rollback
/// that cannot complete faults the pool, which then refuses every operation. Every child shares its
/// parent's ledger, which the owner configures with the first identity the pool may hand out.
class ResourceDomainPool final
{
public:
    /// Sixteen: the product's device tier, storage, the Component Manager, the Directory, the
    /// session and the shell, and the applications they launch.
    static constexpr Foundation::Size Capacity{16U};

    /// The rights of a created domain's first capability: Manage, Inspect,
    /// Duplicate, Transfer and Derive, without Revoke.
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Manage}
                                                                .With(Object::CapabilityRight::Inspect)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Derive)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool.
    constexpr ResourceDomainPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    /// Preconditions:
    /// - every slot is Free.
    ~ResourceDomainPool() noexcept = default;

    ResourceDomainPool(const ResourceDomainPool&) = delete;
    ResourceDomainPool& operator=(const ResourceDomainPool&) = delete;
    ResourceDomainPool(ResourceDomainPool&&) = delete;
    ResourceDomainPool& operator=(ResourceDomainPool&&) = delete;

    /// Gives the pool the ledger of the tree its children join and the first
    /// identity it may hand out (above every identity the boot assigned).
    /// Once only; false for an invalid identity or a second call.
    [[nodiscard]] bool Configure(ResourceDomainLedger& ledger, Object::ResourceDomainId firstIdentity) noexcept;

    [[nodiscard]] Foundation::Result<ResourceDomainCreation, ResourceDomainPoolFailure>
    Create(const ResourceDomainCreateRequest& request) noexcept;

    [[nodiscard]] ResourceDomainPoolSnapshot GetSnapshot() const noexcept;

    /// The registry identity of the child that domain, a capability with
    /// Manage in space, names, if this pool created it and it is not
    /// destroyed yet. Identities are never reused, so a later call that
    /// names the child by it cannot reach a successor in the same slot.
    [[nodiscard]] Foundation::Result<Object::ObjectId, ResourceDomainPoolFailure>
    FindOwned(Object::CapabilitySpace& space, Object::CapabilityHandle domain) noexcept;

    /// Whether candidate is the child root names or one of its descendants.
    /// Callers may hold a pool lock: this one orders after theirs.
    [[nodiscard]] bool IsInSubtree(const ResourceDomain* candidate, Object::ObjectId root) const noexcept;

    /// Destroys the child root names and every descendant in the registry
    /// (their capabilities fail ObjectNotFound from now on); each is
    /// reclaimed by a Collect once nothing pins it, which happens only when
    /// everything charged to it was reclaimed. NotOwned once root is gone.
    [[nodiscard]] ResourceDomainPoolFailure DestroySubtree(Object::ObjectId root) noexcept;

    /// Marks the domain root names Draining once its drain starts: nothing new may be charged to it
    /// or below it. Idempotent; NotOwned when no live slot holds root.
    [[nodiscard]] ResourceDomainPoolFailure BeginDrain(Object::ObjectId root) noexcept;

    /// Reclaims every destroyed child nothing pins any more, descendants
    /// before ancestors: quiesces it (its usage is zero by then), releases
    /// its parent's charge and pin and frees its slot. Returns how many.
    [[nodiscard]] Foundation::Result<Foundation::Size, ResourceDomainPoolFailure> Collect() noexcept;

    /// What a child's slot charges its parent.
    [[nodiscard]] static constexpr ResourceReservation GetReservation() noexcept
    {
        return ResourceReservation{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = sizeof(ResourceDomain),
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
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
        alignas(ResourceDomain) Foundation::UInt8 Storage[sizeof(ResourceDomain)];
        ResourceDomain* Instance;
        /// The parent, charged the slot, and the pin that keeps it
        /// registered while the slot holds the charge.
        ResourceDomain* Parent;
        Object::ObjectPin ParentPin;
        Object::ObjectId Identity;
        SlotState State;
    };

    [[nodiscard]] static ResourceDomainPoolFailure
    MakeFailure(ResourceDomainPoolError error, Object::CapabilityError capability = Object::CapabilityError::Ok,
                ResourceDomainError resource = ResourceDomainError::None,
                Object::ObjectRegistryError registry = Object::ObjectRegistryError::Ok) noexcept
    {
        return Object::MakeObjectPoolFailure(error, capability, resource, registry, 0U);
    }
    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] const Slot* FindSlotLocked(const ResourceDomain* instance) const noexcept;
    [[nodiscard]] bool IsInSubtreeLocked(const ResourceDomain* candidate, Object::ObjectId root) const noexcept;
    /// Initializes the child in slot under parent with the next identity.
    [[gnu::noinline]] [[nodiscard]] ResourceDomainPoolFailure
    InitializeLocked(Slot& slot, ResourceDomain& parent, Object::ObjectId parentObject,
                     const ResourceDomainCreateRequest& request) noexcept;
    /// Registers the initialized child and mints the creator's capability,
    /// or undoes both.
    [[gnu::noinline]] [[nodiscard]] Foundation::Result<ResourceDomainCreation, ResourceDomainPoolFailure>
    PublishLocked(Slot& slot, const ResourceDomainCreateRequest& request) noexcept;
    /// Quiesces the slot's child, releases its charge and pin, and frees it.
    [[nodiscard]] ResourceDomainPoolFailure ReleaseSlotLocked(Slot& slot) noexcept;
    [[nodiscard]] ResourceDomainPoolFailure FaultLocked(ResourceDomainPoolFailure failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    ResourceDomainLedger* m_ledger{nullptr};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot m_slots[Capacity]{};
    Foundation::UInt64 m_nextIdentity{0U};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    ResourceDomainPoolState m_state{ResourceDomainPoolState::Unconfigured};
};

static_assert(sizeof(ResourceDomainPoolState) == 1U);
} // Namespace Asteria::Kernel::Resource

#endif // ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_POOL_HPP_INCLUDED
