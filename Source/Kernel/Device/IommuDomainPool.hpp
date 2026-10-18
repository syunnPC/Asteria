// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_IOMMU_DOMAIN_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_IOMMU_DOMAIN_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Device/IIommuDomainBackend.hpp"
#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Device
{
/// Stops an IommuDomain's DMA for good before its pool ends it: bus mastering off on every function
/// attached to it, then IommuDomain::Quiesce. The device control implements it; the pool calls it
/// under its own lock (rank 1).
class IIommuDomainQuiescer
{
public:
    [[nodiscard]] virtual bool QuiesceDomain(IommuDomain& domain) noexcept = 0;
    /// The same in a drain of subtree, where no one can prove the dead driver's DMA ended yet: the
    /// domain's mapped Frames stay pinned with the functions the drain quarantines until their
    /// Device Manager confirms them quiet.
    [[nodiscard]] virtual bool QuiesceDrainedDomain(IommuDomain& domain,
                                                    const Resource::IResourceDomainSubtree& subtree) noexcept = 0;

protected:
    constexpr IIommuDomainQuiescer() noexcept = default;
    ~IIommuDomainQuiescer() noexcept = default;
};

/// One ObjectCreate of an IommuDomain.
struct IommuDomainCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
    Foundation::UInt8 InputAddressBits;
    Foundation::UInt8 Reserved[7];
};

struct IommuDomainCreation
{
    Object::ObjectId Domain;
    Object::CapabilityHandle Capability;
};

/// One slot's storage, which the platform provides with its backend, so a
/// kernel without an IOMMU carries none.
struct IommuDomainSlot
{
    alignas(IommuDomain) Foundation::UInt8 Storage[sizeof(IommuDomain)];
    IommuDomain* Instance;
    Hal::Device::IIommuDomainBackend* Backend;
    Resource::ResourceDomain* Domain;
    Object::ObjectId Identity;
    Foundation::UInt8 State;
    Foundation::UInt8 Reserved[7];
};

struct IommuDomainPoolSnapshot
{
    Foundation::Size Slots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    bool Faulted;
    Foundation::UInt8 Reserved[7];
};

/// The IommuDomains user code creates, one per platform backend. Create initializes the domain on
/// its slot's backend (IommuDomain pins and charges the ResourceDomain a Manage capability names),
/// registers it and mints the creator's capability under that authority. Destroy and the drain have
/// the quiescer stop the domain's DMA and quiesce it before the registry destroys it; Collect ends
/// its lifetime once nothing references it. Without Configure every Create is InvalidConfiguration.
/// One lock (rank 1) covers the slots; it calls the quiescer, IommuDomain and the registry.
class IommuDomainPool final
{
public:
    static constexpr Foundation::Size Capacity{4U};
    /// The rights of a created IommuDomain's first capability: Inspect, Map,
    /// Transfer, Duplicate, Derive and Manage, without Revoke.
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Map)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};

    constexpr IommuDomainPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    IommuDomainPool(const IommuDomainPool&) = delete;
    IommuDomainPool& operator=(const IommuDomainPool&) = delete;

    /// Once: the slots and their backends, every slot free, and the
    /// platform's firmware DMA reservations every created domain enforces
    /// (IommuDomain's constructor), or null for none.
    [[nodiscard]] bool Configure(IommuDomainSlot (&slots)[Capacity],
                                 Hal::Device::IIommuDomainBackend* const (&backends)[Capacity],
                                 const IDmaReservations* reservations = nullptr) noexcept;

    [[nodiscard]] Foundation::Result<IommuDomainCreation, Object::ObjectPoolFailure>
    Create(const IommuDomainCreateRequest& request) noexcept;

    /// Quiesces and destroys the IommuDomain that domain, a capability with
    /// Manage in space, names. One this pool does not own is NotOwned.
    [[nodiscard]] Foundation::Result<Object::ObjectId, Object::ObjectPoolFailure>
    Destroy(Object::CapabilitySpace& space, Object::CapabilityHandle domain, IIommuDomainQuiescer& quiescer) noexcept;

    /// The drain's IOMMU: quiesces and destroys every live
    /// IommuDomain charged to a domain of subtree.
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree,
                                                           IIommuDomainQuiescer& quiescer) noexcept;

    /// Reclaims every destroyed slot nothing references any more and returns
    /// how many it reclaimed.
    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;

    [[nodiscard]] IommuDomainPoolSnapshot GetSnapshot() const noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing,
    };

    [[nodiscard]] IommuDomainSlot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] IommuDomainSlot* FindSlotLocked(const Object::IKernelObject& instance) noexcept;
    [[gnu::noinline]] [[nodiscard]] Foundation::Result<IommuDomainCreation, Object::ObjectPoolFailure>
    MintLocked(IommuDomainSlot& slot, const IommuDomainCreateRequest& request) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure
    DestroySlotLocked(IommuDomainSlot& slot, IIommuDomainQuiescer& quiescer,
                      const Resource::IResourceDomainSubtree* drained = nullptr) noexcept;
    /// Ends a quiesced slot's IommuDomain and frees the slot.
    void FreeSlotLocked(IommuDomainSlot& slot) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure FaultLocked(Object::ObjectPoolFailure failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    IommuDomainSlot* m_slots{nullptr};
    const IDmaReservations* m_reservations{nullptr};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    bool m_faulted{false};
};
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_IOMMU_DOMAIN_POOL_HPP_INCLUDED
