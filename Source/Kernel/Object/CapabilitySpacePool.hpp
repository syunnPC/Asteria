// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_CAPABILITY_SPACE_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_CAPABILITY_SPACE_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
/// The kernel object a CapabilitySpace capability names: a registered identity around one
/// CapabilitySpace, which a pool slot constructs in place for every creation, so each incarnation
/// has a new space identifier.
class CapabilitySpaceObject final : public IKernelObject
{
public:
    constexpr CapabilitySpaceObject(CapabilitySpace& space, const ResourceDomainId chargeOwner) noexcept
        : m_space{space}, m_chargeOwner{chargeOwner}
    {
    }

    CapabilitySpaceObject(const CapabilitySpaceObject&) = delete;
    CapabilitySpaceObject& operator=(const CapabilitySpaceObject&) = delete;

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::CapabilitySpace;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_chargeOwner;
    }

    [[nodiscard]] CapabilitySpace& GetSpace() const noexcept
    {
        return m_space;
    }

private:
    CapabilitySpace& m_space;
    ResourceDomainId m_chargeOwner;
};

enum class CapabilitySpacePoolState : Foundation::UInt8
{
    /// No keys yet: every creation is CapacityExceeded.
    Unconfigured,
    Ready,
    Faulted,
};

/// One ObjectCreate of a CapabilitySpace.
struct CapabilitySpaceCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    CapabilityHandle Authority;
};

struct CapabilitySpaceCreation
{
    ObjectId Space;
    CapabilityHandle Capability;
};

struct CapabilitySpacePoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    CapabilitySpacePoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns the fixed CapabilitySpace slots user code creates through ObjectCreate type 3. Each slot
/// has its own validation key, derived at boot from a purpose of its own (CapabilityKeyPurpose:
/// RuntimeSpace0 onward); the space identifier in every authenticator keeps incarnations of one
/// slot apart. Create charges sizeof(CapabilitySpaceObject) in kernel-metadata bytes to the
/// ResourceDomain a Manage capability names, constructs the space with the boot runtime's
/// registries, clock and lock, registers it and mints the creator's capability under that
/// authority. Destroy makes an owned space quiescing; Collect reclaims it once no ProtectionDomain
/// or Endpoint pins it: the destructors drop every capability it holds (none pins anything) and the
/// slot is free for a space with a new identifier.
class CapabilitySpacePool final
{
public:
    /// Sixteen: the product's device tier, storage, the Component Manager, the Directory, the
    /// session and the shell, and the applications they launch.
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::CapabilitySpace)]};

    /// The rights of a created space's first capability: Manage authorizes a ProtectionDomain over
    /// it, a CapabilityGrant into it and an Endpoint receiving in it; never Revoke.
    static constexpr CapabilityRights CreatedRights{CapabilityRights{CapabilityRight::Inspect}
                                                        .With(CapabilityRight::Transfer)
                                                        .With(CapabilityRight::Duplicate)
                                                        .With(CapabilityRight::Derive)
                                                        .With(CapabilityRight::Manage)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr CapabilitySpacePool(ObjectRegistry& objectRegistry, RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    CapabilitySpacePool(const CapabilitySpacePool&) = delete;
    CapabilitySpacePool& operator=(const CapabilitySpacePool&) = delete;
    CapabilitySpacePool(CapabilitySpacePool&&) = delete;
    CapabilitySpacePool& operator=(CapabilitySpacePool&&) = delete;

    /// Gives the pool the collaborators every created space shares with the
    /// boot spaces and one key per slot; once only. False on a second call or
    /// a zero key.
    /// Preconditions:
    /// - revocationRegistry shares the object registry's lock and, with
    /// clock, outlives the pool.
    [[nodiscard]] bool Configure(RevocationRegistry& revocationRegistry, Time::IMonotonicClock& clock,
                                 const CapabilityValidationKey (&keys)[Capacity]) noexcept;

    [[nodiscard]] Foundation::Result<CapabilitySpaceCreation, ObjectPoolFailure>
    Create(const CapabilitySpaceCreateRequest& request) noexcept;

    /// Makes a space this pool owns, which space names with Manage,
    /// quiescing; NotOwned for another. A later Collect reclaims it.
    [[nodiscard]] ObjectPoolFailure Destroy(CapabilitySpace& space, CapabilityHandle handle) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// Reclaims every quiescing space nothing pins any more; returns how
    /// many. A reclaimed space holding a capability a prepared send holds
    /// faults the pool.
    [[nodiscard]] Foundation::Result<Foundation::Size, ObjectPoolFailure> Collect() noexcept;

    /// The registered identity of space if it is one this pool created and
    /// has not destroyed; invalid otherwise.
    [[nodiscard]] ObjectId FindIdentity(const CapabilitySpace& space) const noexcept;

    [[nodiscard]] CapabilitySpacePoolSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] static constexpr Resource::ResourceReservation GetReservation() noexcept
    {
        return Resource::ResourceReservation{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = sizeof(CapabilitySpace) + sizeof(CapabilitySpaceObject),
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .Objects = {0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U},
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
        alignas(CapabilitySpace) Foundation::UInt8 SpaceStorage[sizeof(CapabilitySpace)];
        alignas(CapabilitySpaceObject) Foundation::UInt8 ObjectStorage[sizeof(CapabilitySpaceObject)];
        CapabilitySpace* Space;
        CapabilitySpaceObject* Instance;
        Resource::ResourceDomain* Domain;
        ObjectPin DomainPin;
        ObjectId Identity;
        SlotState State;
    };

    [[nodiscard]] static ObjectPoolFailure
    MakeFailure(ObjectPoolError error, CapabilityError capability = CapabilityError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
                ObjectRegistryError registry = ObjectRegistryError::Ok) noexcept
    {
        return MakeObjectPoolFailure(error, capability, resource, registry);
    }
    /// Ends a slot's space and object, releases its charge and pin.
    [[nodiscard]] ObjectPoolFailure ReleaseSlotLocked(Slot& slot) noexcept;
    /// Registers slot's prepared object and mints its capability; undoes the
    /// slot on failure.
    [[nodiscard]] Foundation::Result<CapabilitySpaceCreation, ObjectPoolFailure>
    PublishLocked(Slot& slot, CapabilitySpace& caller, CapabilityHandle authority) noexcept;

    ObjectRegistry& m_objectRegistry;
    RootCapabilityIssuer& m_issuer;
    RevocationRegistry* m_revocationRegistry{nullptr};
    Time::IMonotonicClock* m_clock{nullptr};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    CapabilityValidationKey m_keys[Capacity]{};
    Slot m_slots[Capacity]{};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    CapabilitySpacePoolState m_state{CapabilitySpacePoolState::Unconfigured};
};

/// The key purpose of pool slot `index`.
[[nodiscard]] constexpr CapabilityKeyPurpose RuntimeSpaceKeyPurpose(const Foundation::Size index) noexcept
{
    return static_cast<CapabilityKeyPurpose>(static_cast<Foundation::UInt32>(CapabilityKeyPurpose::RuntimeSpace0) +
                                             static_cast<Foundation::UInt32>(index));
}

static_assert(RuntimeSpaceKeyPurpose(CapabilitySpacePool::Capacity - 1U) == CapabilityKeyPurpose::RuntimeSpace15);
static_assert(sizeof(CapabilitySpacePoolState) == 1U);
static_assert(sizeof(CapabilitySpacePoolSnapshot) == 56U);
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_CAPABILITY_SPACE_POOL_HPP_INCLUDED
