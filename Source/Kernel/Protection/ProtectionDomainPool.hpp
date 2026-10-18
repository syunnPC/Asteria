// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_PROTECTION_PROTECTION_DOMAIN_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_PROTECTION_PROTECTION_DOMAIN_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Kernel/Memory/FaultChannel.hpp"
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Protection
{
/// A created domain as the platform records it: its ProtectionDomain and identity, the
/// CapabilitySpace and AddressSpace it binds, and the supervision tag its creator chose, which
/// every fault delivery of its threads carries. Every pointer stays valid until the domain is
/// retired.
struct CreatedDomain
{
    ProtectionDomain* Protection;
    ProtectionDomainId Identity;
    Object::CapabilitySpace* Capabilities;
    Memory::AddressSpace* AddressSpace;
    Memory::AddressSpaceId AddressSpaceIdentity;
    Foundation::UInt64 SupervisionTag;
    Memory::FaultChannel* Channel{nullptr};
};

/// The platform's records of created domains: what a thread started in one
/// reaches for its system calls, faults and supervision.
class ICreatedDomainRecords
{
public:
    /// Records domain; false when the platform has no free record, which
    /// fails the creation.
    [[nodiscard]] virtual bool RecordDomain(const CreatedDomain& domain) noexcept = 0;

    /// Forgets domain's record once no thread is bound in it; false while
    /// one is (or after a kernel invariant failure), and the record stays.
    [[nodiscard]] virtual bool RetireDomain(const ProtectionDomain& domain) noexcept = 0;

protected:
    constexpr ICreatedDomainRecords() noexcept = default;
    ~ICreatedDomainRecords() noexcept = default;
};

enum class ProtectionDomainPoolState : Foundation::UInt8
{
    /// No records yet: every creation is CapacityExceeded.
    Unconfigured,
    Ready,
    Faulted,
};

/// One ObjectCreate of a ProtectionDomain.
struct ProtectionDomainCreateRequest
{
    /// The caller's capability space: it holds every handle below and
    /// receives the new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage: the domain charged, also
    /// the one the new domain's activity charges.
    Object::CapabilityHandle Authority;
    /// The AddressSpace and CapabilitySpace the domain binds, each with
    /// Manage.
    Object::CapabilityHandle AddressSpace;
    Object::CapabilityHandle Capabilities;
    /// Nonzero; the creator's name for the domain in its fault deliveries.
    Foundation::UInt64 SupervisionTag;
    Object::CapabilityHandle Channel{};
    Foundation::UInt64 DiagnosticComponentKind{};
};

struct ProtectionDomainCreation
{
    ProtectionDomainId Domain;
    Object::CapabilityHandle Capability;
};

struct ProtectionDomainPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    ProtectionDomainPoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns the ProtectionDomains user code creates through ObjectCreate type 1. Create resolves the
/// AddressSpace and CapabilitySpace handles (Manage), pins the space's object for the slot's life,
/// initializes the domain over both, charged to the ResourceDomain a Manage capability names (the
/// domain reserves its own size and pins its AddressSpace and ResourceDomain), registers and binds
/// it, has the platform record it, and mints the creator's capability. Destroy is Busy while the
/// platform still has a thread bound in the domain; otherwise the record goes and the domain is
/// quiescing, and Collect quiesces and frees it once it is unreferenced. Create's lookups end
/// before the domain pins its AddressSpace and ResourceDomain and the slot its CapabilitySpace;
/// that is safe only while every management call runs on the bootstrap processor with interrupts
/// disabled, so nothing destroys those objects in between. Management calls on application
/// processors must hold the leases until the pins are taken.
class ProtectionDomainPool final
{
public:
    /// Sixteen: the product's device tier, storage, the Component Manager, the Directory, the
    /// session and the shell, and the applications they launch.
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::ProtectionDomain)]};

    /// The rights of a created domain's first capability: Manage authorizes a thread in it and its
    /// destruction; never Revoke.
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr ProtectionDomainPool(Object::ObjectRegistry& objectRegistry,
                                   Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    ProtectionDomainPool(const ProtectionDomainPool&) = delete;
    ProtectionDomainPool& operator=(const ProtectionDomainPool&) = delete;
    ProtectionDomainPool(ProtectionDomainPool&&) = delete;
    ProtectionDomainPool& operator=(ProtectionDomainPool&&) = delete;

    /// Gives the pool the platform's records; once only.
    [[nodiscard]] bool Configure(ICreatedDomainRecords& records) noexcept;

    [[nodiscard]] Foundation::Result<ProtectionDomainCreation, Object::ObjectPoolFailure>
    Create(const ProtectionDomainCreateRequest& request) noexcept;

    /// Makes the domain domain (Manage in space) names quiescing once the
    /// platform retired its record; Busy while a thread is bound in it.
    [[nodiscard]] Object::ObjectPoolFailure Destroy(Object::CapabilitySpace& space,
                                                    Object::CapabilityHandle domain) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// Quiesces and frees every quiescing domain no longer referenced and
    /// returns how many.
    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;

    [[nodiscard]] ProtectionDomainPoolSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] bool IsCreated(ProtectionDomainId domain) const noexcept;
    [[nodiscard]] Foundation::UInt64 GetTagRefusals() const noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing,
    };

    struct Slot
    {
        alignas(ProtectionDomain) Foundation::UInt8 Storage[sizeof(ProtectionDomain)];
        ProtectionDomain* Instance;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin CapabilitiesPin;
        Object::ObjectPin ChannelPin;
        Memory::FaultChannel* Channel{nullptr};
        Foundation::UInt64 Tag{0U};
        ProtectionDomainId Identity;
        SlotState State;
    };

    /// What Create resolved from the caller's handles.
    struct Resolved
    {
        Resource::ResourceDomain* Domain;
        Resource::ResourceDomainObjectId DomainIdentity;
        Memory::AddressSpace* AddressSpace;
        Memory::AddressSpaceId AddressSpaceIdentity;
        Object::ObjectId CapabilitiesObject;
        Object::CapabilitySpace* Capabilities;
        Object::ObjectPin DomainPin;
        Object::ObjectPin AddressSpacePin;
        Object::ObjectPin CapabilitiesPin;
        Object::ObjectPin ChannelPin;
        Memory::FaultChannel* Channel{nullptr};
    };

    [[nodiscard]] static Object::ObjectPoolFailure
    MakeFailure(Object::ObjectPoolError error, Object::CapabilityError capability = Object::CapabilityError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
                Object::ObjectRegistryError registry = Object::ObjectRegistryError::Ok) noexcept
    {
        return Object::MakeObjectPoolFailure(error, capability, resource, registry);
    }
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure ResolveLocked(const ProtectionDomainCreateRequest& request,
                                                                         Resolved& resolved) noexcept;
    [[nodiscard, gnu::noinline]] Foundation::Result<ProtectionDomainCreation, Object::ObjectPoolFailure>
    PublishLocked(Slot& slot, const ProtectionDomainCreateRequest& request, const Resolved& resolved) noexcept;
    /// Quiesces and ends slot's domain and frees the slot.
    [[nodiscard]] Object::ObjectPoolFailure ReleaseSlotLocked(Slot& slot) noexcept;
    /// Withdraws a registered domain no capability names and releases it.
    [[nodiscard]] Object::ObjectPoolFailure WithdrawLocked(Slot& slot) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    ICreatedDomainRecords* m_records{nullptr};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot m_slots[Capacity]{};
    Foundation::UInt64 m_tagRefusals{0U};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    ProtectionDomainPoolState m_state{ProtectionDomainPoolState::Unconfigured};
};

static_assert(sizeof(ProtectionDomainPoolState) == 1U);
static_assert(sizeof(ProtectionDomainPoolSnapshot) == 56U);
} // Namespace Asteria::Kernel::Protection

#endif // ASTERIA_KERNEL_PROTECTION_PROTECTION_DOMAIN_POOL_HPP_INCLUDED
