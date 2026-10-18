// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
/// The platform's run-time AddressSpaces: a fixed number of slots, each an AddressSpace over a
/// page-table root the platform prepares from the boot seed on a reserved arena, so a slot's space
/// starts with the kernel's mappings and an empty user half. A slot holds one AddressSpace at a
/// time: ReleaseSlot ends a quiesced one, and the next InitializeSlot prepares the arena again.
class IAddressSpaceProvider
{
public:
    /// How many slots the platform has, at most AddressSpacePool::Capacity.
    [[nodiscard]] virtual Foundation::Size GetSlotCount() const noexcept = 0;

    /// The kernel-metadata bytes one slot's space costs: the AddressSpace
    /// and its root's page-table storage.
    [[nodiscard]] virtual Foundation::UInt64 GetSlotBytes(bool largeArena = false) const noexcept = 0;

    /// Exhaustion must leave the ordinary slot free.
    [[nodiscard]] virtual bool HasAvailableArena(bool) const noexcept
    {
        return true;
    }

    /// Prepares slot's root and initializes its AddressSpace charged to
    /// chargeOwner, and returns it, unregistered; null if the slot holds a
    /// space or its root cannot be prepared.
    [[nodiscard]] virtual AddressSpace* InitializeSlot(Foundation::Size slot, Object::ResourceDomainId chargeOwner,
                                                       bool largeArena = false) noexcept = 0;

    /// Ends slot's quiesced AddressSpace and what its root was built from,
    /// so InitializeSlot may build them again; false if it cannot, which
    /// spends the slot.
    [[nodiscard]] virtual bool ReleaseSlot(Foundation::Size slot) noexcept = 0;

protected:
    constexpr IAddressSpaceProvider() noexcept = default;
    ~IAddressSpaceProvider() noexcept = default;
};

enum class AddressSpacePoolState : Foundation::UInt8
{
    /// No provider yet: every creation is CapacityExceeded.
    Unconfigured,
    Ready,
    Faulted,
};

/// One ObjectCreate of an AddressSpace.
struct AddressSpaceCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
    bool LargePageTableArena{false};
};

struct AddressSpaceCreation
{
    AddressSpaceId Space;
    Object::CapabilityHandle Capability;
};

struct AddressSpacePoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    /// Slots the provider could not prepare or release; they stay unusable
    /// for the boot.
    Foundation::Size SpentSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    AddressSpacePoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns the AddressSpaces user code creates through ObjectCreate type 2.
/// Create charges the provider's slot bytes in kernel metadata to the ResourceDomain a Manage
/// capability names, has the provider prepare and initialize a free slot's space charged to that
/// domain, registers and binds it, and mints the creator's capability under that authority. A
/// failure after the provider initialized the slot quiesces the space and releases the slot.
/// Destroy makes an owned space quiescing; Collect reclaims it once it is resident nowhere and no
/// ProtectionDomain, fault or lease pins it: it quiesces the space, which releases what its
/// mappings pin, returns the charge and has the provider release the slot. A slot the provider
/// cannot prepare or release is spent.
class AddressSpacePool final
{
public:
    /// Sixteen: the product's device tier, storage, the Component Manager, the Directory, the
    /// session and the shell, and the applications they launch.
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::AddressSpace)]};

    /// The rights of a created space's first capability: Map for MemoryMap and MemoryUnmap into it,
    /// Manage for a ProtectionDomain over it; never Revoke.
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Map)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr AddressSpacePool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    AddressSpacePool(const AddressSpacePool&) = delete;
    AddressSpacePool& operator=(const AddressSpacePool&) = delete;
    AddressSpacePool(AddressSpacePool&&) = delete;
    AddressSpacePool& operator=(AddressSpacePool&&) = delete;

    /// Gives the pool its provider; once only. False on a second call or a
    /// provider with no slots or more than Capacity.
    [[nodiscard]] bool Configure(IAddressSpaceProvider& provider) noexcept;

    [[nodiscard]] Foundation::Result<AddressSpaceCreation, Object::ObjectPoolFailure>
    Create(const AddressSpaceCreateRequest& request) noexcept;

    /// Makes a space this pool owns, which space names with Manage,
    /// quiescing; NotOwned for another. A later Collect reclaims it.
    [[nodiscard]] Object::ObjectPoolFailure Destroy(Object::CapabilitySpace& space,
                                                    Object::CapabilityHandle handle) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// Reclaims every quiescing space that is resident nowhere and nothing
    /// pins any more; returns how many.
    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;

    [[nodiscard]] AddressSpacePoolSnapshot GetSnapshot() const noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing,
        Spent,
    };

    struct Slot
    {
        AddressSpace* Instance;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin DomainPin;
        Resource::ResourceReservation Charge;
        Object::ObjectId Identity;
        SlotState State;
    };

    [[nodiscard]] static Object::ObjectPoolFailure
    MakeFailure(Object::ObjectPoolError error, Object::CapabilityError capability = Object::CapabilityError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
                Object::ObjectRegistryError registry = Object::ObjectRegistryError::Ok) noexcept
    {
        return Object::MakeObjectPoolFailure(error, capability, resource, registry);
    }
    /// Releases slot's charge and pin and marks it state.
    [[nodiscard]] Object::ObjectPoolFailure ReleaseSlotLocked(Slot& slot, SlotState state) noexcept;
    /// Quiesces the slot's unregistered space, releases its charge and pin,
    /// and frees the slot if the provider releases it (spends it otherwise).
    [[nodiscard]] Object::ObjectPoolFailure RetireSlotLocked(Foundation::Size index) noexcept;
    /// Registers and binds slot's initialized space and mints its
    /// capability; spends the slot on failure.
    [[nodiscard]] Foundation::Result<AddressSpaceCreation, Object::ObjectPoolFailure>
    PublishLocked(Slot& slot, Object::CapabilitySpace& caller, Object::CapabilityHandle authority) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    IAddressSpaceProvider* m_provider{nullptr};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot m_slots[Capacity]{};
    Foundation::Size m_slotCount{0U};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    AddressSpacePoolState m_state{AddressSpacePoolState::Unconfigured};
};

static_assert(sizeof(AddressSpacePoolState) == 1U);
static_assert(sizeof(AddressSpacePoolSnapshot) == 64U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_POOL_HPP_INCLUDED
