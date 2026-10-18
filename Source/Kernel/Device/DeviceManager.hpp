// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_DEVICE_MANAGER_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_DEVICE_MANAGER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Device/PciFunction.hpp"
#include "Kernel/Device/PciFunctionCatalog.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class CapabilitySpace;
class RootCapabilityIssuer;
}

namespace Asteria::Kernel::Device
{
enum class DeviceManagerState : Foundation::UInt8
{
    Ready,
    Faulted,
};

enum class DeviceManagerError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidDescriptor,
    RegionOverlap,
    CapacityExceeded,
    RegionRejected,
    RegistrationRejected,
    CapabilityRejected,
    RollbackRejected,
    InvalidRegion,
    RegionNotOwned,
    RegionBusy,
    DestroyRejected,
    InvalidDestructionRecord,
    RecycleRejected,
    Faulted,
};

struct DeviceRegionCreateRequest
{
    Object::CapabilitySpace* Destination;
    Resource::ResourceDomain* ResourceDomainStorage;
    Resource::ResourceDomainObjectId ResourceDomainIdentity;
    DeviceRegionDescriptor Descriptor;
    Object::RevocationDomainId RevocationDomain;
    Object::MonotonicTime LeaseDeadline;
};

struct DeviceRegionCreation
{
    DeviceRegion* Storage;
    DeviceRegionId Region;
    Object::CapabilityHandle Capability;
};

struct DeviceManagerFailure
{
    Object::ObjectId Region;
    DeviceRegionFailure RegionFailure;
    Object::ObjectRegistryError RegistryError;
    Object::CapabilityError CapabilityError;
    DeviceManagerError Error;
    Foundation::UInt8 Reserved[5];
};

struct DeviceManagerSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::Size FaultedSlots;
    DeviceManagerState State;
    Foundation::UInt8 Reserved[7];
};

enum class PciFunctionManagerError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    CatalogNotBound,
    CatalogAlreadyBound,
    CatalogRejected,
    FunctionAlreadyOwned,
    CapacityExceeded,
    FunctionRejected,
    RegistrationRejected,
    CapabilityRejected,
    RollbackRejected,
    InvalidFunction,
    FunctionNotOwned,
    FunctionBusy,
    DestroyRejected,
    InvalidDestructionRecord,
    RecycleRejected,
    Faulted,
};

struct PciFunctionCreateRequest
{
    Object::CapabilitySpace* Destination;
    Resource::ResourceDomain* ResourceDomainStorage;
    Resource::ResourceDomainObjectId ResourceDomainIdentity;
    Object::RevocationDomainId RevocationDomain;
    Object::MonotonicTime LeaseDeadline;
    PciFunctionAddress Address;
    Foundation::UInt8 Reserved[2];
};

struct PciFunctionCreation
{
    PciFunctionId Function;
    Object::CapabilityHandle Capability;
};

struct PciFunctionManagerFailure
{
    Object::ObjectId Function;
    PciFunctionCatalogFailure CatalogFailure;
    PciFunctionFailure FunctionFailure;
    Object::ObjectRegistryError RegistryError;
    Object::CapabilityError CapabilityError;
    PciFunctionManagerError Error;
    Foundation::UInt8 Reserved[5];
};

struct PciFunctionManagerSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::Size RetiredSlots;
    Foundation::Size FaultedSlots;
    DeviceManagerState State;
    bool CatalogBound;
    Foundation::UInt8 Reserved[6];
};

/// Owns fixed sets of authoritative DeviceRegion and PciFunction objects. Platform discovery
/// supplies validated candidates; this manager prevents two live objects of the same address-space
/// kind from covering the same hardware range, registers the exact generation, and mints one
/// bounded initial capability. Driver restart revokes or replaces capabilities without retiring the
/// hardware object. Physical removal uses Retire followed by the shared registry collector and an
/// exact type-specific Reclaim operation. PCI records are accepted only from a completed read-only
/// catalog.
class DeviceManager final
{
public:
    static constexpr Foundation::Size MaximumRegions{32U};
    /// Real machines and hypervisors expose many root-port and bridge
    /// functions (VMware: four eight-function root ports alone).
    static constexpr Foundation::Size MaximumPciFunctions{256U};

    /// Preconditions:
    /// - issuer is bound to registry;
    /// - the trusted platform owner constructs exactly one manager for the
    /// platform MMIO, port-I/O, and PCI discovery namespaces;
    /// - registry, issuer, every supplied ResourceDomain and capability space,
    /// and the shared ResourceDomain ledgers outlive their retained uses;
    /// - platform discovery is the sole source of Create requests.
    constexpr DeviceManager(Object::ObjectRegistry& registry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_registry{registry}, m_issuer{issuer}
    {
    }

    /// Preconditions:
    /// - every region slot is Free;
    /// - every PCI slot is Free or Retired;
    /// - every PciFunctionId and PciFunctionAssignmentToken copy and use has
    /// ended, so this manager's embedded function storage can be destroyed;
    /// - no concurrent operation accesses this object.
    ~DeviceManager() noexcept = default;

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;
    DeviceManager(DeviceManager&&) = delete;
    DeviceManager& operator=(DeviceManager&&) = delete;

    /// Creates one unique MMIO or port-I/O authority and mints its initial
    /// capability. Every pre-publication failure restores the slot immediately;
    /// a mint failure starts registry destruction and awaits Reclaim.
    /// Blocking: may spin and is not callable from an ISR or same-CPU reentry.
    [[nodiscard]] Foundation::Result<DeviceRegionCreation, DeviceManagerFailure>
    Create(const DeviceRegionCreateRequest& request) noexcept;

    /// Prevents new lookups for one owned region. Existing leases and pins
    /// drain through the registry before the destruction record is collectable.
    /// Blocking: may spin and is not callable from an ISR or same-CPU reentry.
    [[nodiscard]] Foundation::Result<Object::ObjectId, DeviceManagerFailure> Retire(DeviceRegionId region) noexcept;

    /// Consumes the exact record returned by the shared registry collector,
    /// releases the ResourceDomain charge, and restores the fixed slot.
    /// Blocking: may spin and is not callable from an ISR or same-CPU reentry.
    [[nodiscard]] Foundation::Result<Object::ObjectId, DeviceManagerFailure>
    Reclaim(const Object::ObjectDestructionRecord& record) noexcept;

    /// Blocking: may spin and is not callable from an ISR or same-CPU reentry.
    [[nodiscard]] DeviceManagerSnapshot GetSnapshot() const noexcept;

    /// Permanently binds the one immutable authoritative PCI catalog used by
    /// this manager. The catalog outlives this manager after success.
    /// Lock order: DeviceManager precedes PciFunctionCatalog; the catalog must
    /// never call back into this manager while a query is in progress.
    [[nodiscard]] PciFunctionManagerError BindPciFunctionCatalog(const PciFunctionCatalog& catalog) noexcept;

    /// Materializes one exact discovered PCI function and mints only
    /// Inspect, Manage, and Transfer. The immutable catalog descriptor is
    /// copied; the caller cannot substitute descriptor fields.
    /// Lock order: DeviceManager precedes PciFunctionCatalog, PciFunction,
    /// ObjectRegistry, ProtectionDomain, and ResourceDomain. None may call
    /// back into this manager while the operation is in progress.
    /// Blocking: may spin and is not callable from an ISR or same-CPU reentry.
    [[nodiscard]] Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
    CreatePciFunction(const PciFunctionCreateRequest& request) noexcept;

    /// Begins destruction only while the function has no driver or IOMMU
    /// relationship. Existing capabilities and pins drain through the registry.
    [[nodiscard]] Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>
    RetirePciFunction(PciFunctionId function) noexcept;

    /// Consumes the exact PCI-function destruction record, returns its charge,
    /// and restores or permanently retires the fixed storage slot.
    [[nodiscard]] Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>
    ReclaimPciFunction(const Object::ObjectDestructionRecord& record) noexcept;

    /// BSP boot before user assignment. Marks a captured firmware requester
    /// unavailable to user drivers; the restriction stays with its authority.
    [[nodiscard]] bool ReserveFirmwareDma(PciFunctionAddress address) noexcept;

    /// What keeps the active function at address from any user driver (PciFunction:
    /// GetAssignmentRestriction): None when it may be assigned, InvalidConfiguration when no active
    /// function has that address.
    [[nodiscard]] PciFunctionError GetAssignmentRestriction(PciFunctionAddress address) noexcept;

    [[nodiscard]] PciFunctionManagerSnapshot GetPciFunctionSnapshot() const noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing,
        Faulted,
    };

    struct RegionSlot
    {
        DeviceRegion Region;
        Object::ObjectId Identity;
        DeviceRegionDescriptor Descriptor{};
        SlotState State{SlotState::Free};
    };

    enum class PciSlotState : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing,
        Retired,
        Faulted,
    };

    struct PciSlot
    {
        PciFunction Function;
        Object::ObjectId Identity;
        PciFunctionDescriptor Descriptor{};
        PciSlotState State{PciSlotState::Free};
    };

    [[nodiscard]] static DeviceManagerFailure
    MakeFailure(DeviceManagerError error, Object::ObjectId region = {}, DeviceRegionFailure regionFailure = {},
                Object::ObjectRegistryError registryError = Object::ObjectRegistryError::Ok,
                Object::CapabilityError capabilityError = Object::CapabilityError::Ok) noexcept;
    [[nodiscard]] static Object::CapabilityRights GetInitialRights(const DeviceRegionDescriptor& descriptor) noexcept;
    [[nodiscard]] static PciFunctionManagerFailure
    MakePciFunctionFailure(PciFunctionManagerError error, Object::ObjectId function = {},
                           PciFunctionCatalogFailure catalogFailure = {}, PciFunctionFailure functionFailure = {},
                           Object::ObjectRegistryError registryError = Object::ObjectRegistryError::Ok,
                           Object::CapabilityError capabilityError = Object::CapabilityError::Ok) noexcept;
    [[nodiscard]] static constexpr Object::CapabilityRights GetPciFunctionInitialRights() noexcept;
    [[nodiscard]] bool OverlapsLocked(const DeviceRegionDescriptor& descriptor) const noexcept;
    [[nodiscard]] RegionSlot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] RegionSlot* FindSlotLocked(DeviceRegionId region) noexcept;
    [[nodiscard]] RegionSlot* FindSlotLocked(Object::IKernelObject* instance) noexcept;
    [[nodiscard]] Foundation::Result<DeviceRegionCreation, DeviceManagerFailure>
    CreateInSlotLocked(const DeviceRegionCreateRequest& request, RegionSlot& slot) noexcept;
    [[nodiscard]] Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
    CreatePciFunctionFromCatalogLocked(const PciFunctionCreateRequest& request) noexcept;
    [[nodiscard]] Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
    CreatePciFunctionInSlotLocked(const PciFunctionCreateRequest& request, const PciFunctionDescriptor& descriptor,
                                  PciSlot& slot) noexcept;
    [[nodiscard]] Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
    RecycleUnregisteredPciFunctionLocked(PciSlot& slot, Object::ObjectRegistryError registrationError) noexcept;
    [[nodiscard]] Foundation::Result<PciFunctionCreation, PciFunctionManagerFailure>
    MintPciFunctionLocked(const PciFunctionCreateRequest& request, PciSlot& slot) noexcept;
    [[nodiscard]] Foundation::Result<Object::ObjectId, PciFunctionManagerFailure>
    RetirePciFunctionLocked(PciSlot& slot) noexcept;
    [[nodiscard]] bool HasPciFunctionLocked(PciFunctionAddress address) const noexcept;
    [[nodiscard]] PciSlot* FindFreePciSlotLocked() noexcept;
    [[nodiscard]] PciSlot* FindPciSlotLocked(PciFunctionId function) noexcept;
    [[nodiscard]] PciSlot* FindPciSlotLocked(Object::IKernelObject* instance) noexcept;
    void EnterFaultedLocked(RegionSlot& slot) noexcept;
    void EnterFaultedLocked(PciSlot& slot) noexcept;

    Object::ObjectRegistry& m_registry;
    Object::RootCapabilityIssuer& m_issuer;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    RegionSlot m_regionSlots[MaximumRegions]{};
    PciSlot m_pciSlots[MaximumPciFunctions]{};
    const PciFunctionCatalog* m_pciCatalog{nullptr};
    DeviceManagerState m_state{DeviceManagerState::Ready};
};

static_assert(sizeof(DeviceManagerState) == 1U);
static_assert(sizeof(DeviceManagerError) == 1U);
static_assert(sizeof(DeviceRegionCreateRequest) == 72U);
static_assert(alignof(DeviceRegionCreateRequest) == 8U);
static_assert(sizeof(DeviceRegionCreation) == 40U);
static_assert(alignof(DeviceRegionCreation) == 8U);
static_assert(sizeof(DeviceManagerFailure) == 24U);
static_assert(alignof(DeviceManagerFailure) == 8U);
static_assert(sizeof(DeviceManagerSnapshot) == 40U);
static_assert(alignof(DeviceManagerSnapshot) == 8U);
static_assert(sizeof(PciFunctionManagerError) == 1U);
static_assert(sizeof(PciFunctionCreateRequest) == 56U);
static_assert(alignof(PciFunctionCreateRequest) == 8U);
static_assert(sizeof(PciFunctionCreation) == 32U);
static_assert(alignof(PciFunctionCreation) == 8U);
static_assert(sizeof(PciFunctionManagerFailure) == 40U);
static_assert(alignof(PciFunctionManagerFailure) == 8U);
static_assert(sizeof(PciFunctionManagerSnapshot) == 48U);
static_assert(alignof(PciFunctionManagerSnapshot) == 8U);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_DEVICE_MANAGER_HPP_INCLUDED
