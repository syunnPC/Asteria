// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_HPP_INCLUDED
#define ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Resource
{
inline constexpr Foundation::UInt8 MaximumResourceDomainDepth{8U};

/// Identifies one ResourceDomain generation within one ObjectRegistry.
/// The originating registry outlives every copy and use of this value.
class ResourceDomainObjectId final
{
public:
    constexpr ResourceDomainObjectId() noexcept = default;

    constexpr ResourceDomainObjectId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
        : m_registry{&registry}, m_object{object}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_object.GetValue();
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_registry != nullptr) && m_object.IsValid();
    }

    [[nodiscard]] constexpr Object::ObjectId GetObjectId() const noexcept
    {
        return m_object;
    }

    [[nodiscard]] constexpr bool IsFromRegistry(const Object::ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const ResourceDomainObjectId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class ResourceDomainState : Foundation::UInt8
{
    Uninitialized,
    Initializing,
    Ready,
    Quiesced,
    /// A drain of the domain's subtree began: nothing new may be charged to it or below it, while
    /// releases, the drain's retries and the final quiesce go on.
    Draining,
};

enum class ResourceDomainError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    ParentPinRejected,
    ParentInstanceMismatch,
    ParentNotReady,
    DepthExceeded,
    LimitExceeded,
    ArithmeticOverflow,
    InvalidReservation,
    ReservationUnderflow,
    InUse,
    NotReady,
    Quiesced,
    InvariantViolation,
    /// The domain, or one above it, is draining: no new charge or child.
    Draining,
    ObjectLimitExceeded,
};

struct ResourceDomainLimits
{
    Foundation::UInt64 CommittedMemoryPages;
    Foundation::UInt64 ResidentMemoryPages;
    Foundation::UInt64 KernelMetadataBytes;
    Foundation::UInt64 CapabilityHandles;
    Foundation::UInt64 IpcQueueBytes;
    Foundation::UInt64 Threads;
    Foundation::UInt64 ChildDomains;
    Abi::ObjectCreationLimits Objects{};
};

struct ResourceReservation
{
    Foundation::UInt64 CommittedMemoryPages;
    Foundation::UInt64 ResidentMemoryPages;
    Foundation::UInt64 KernelMetadataBytes;
    Foundation::UInt64 CapabilityHandles;
    Foundation::UInt64 IpcQueueBytes;
    Foundation::UInt64 Threads;
    Foundation::UInt16 Objects[Abi::LimitedObjectCount]{};
    Foundation::UInt16 LargePageTableArenas{0U};
};

struct ResourceDomainUsage
{
    Foundation::UInt64 CommittedMemoryPages;
    Foundation::UInt64 ResidentMemoryPages;
    Foundation::UInt64 KernelMetadataBytes;
    Foundation::UInt64 CapabilityHandles;
    Foundation::UInt64 IpcQueueBytes;
    Foundation::UInt64 Threads;
    Foundation::UInt64 ChildDomains;
    Foundation::UInt16 Objects[Abi::LimitedObjectCount]{};
    Foundation::UInt16 LargePageTableArenas{0U};
};

struct ResourceDomainFailure
{
    ResourceDomainError Error;
    Object::ObjectPinError Pin;
    Foundation::UInt8 Reserved[6];
};

struct ResourceDomainSnapshot
{
    Object::ResourceDomainId Identity;
    ResourceDomainObjectId Parent;
    ResourceDomainLimits Limits;
    ResourceDomainUsage Usage;
    Foundation::UInt8 Depth;
    ResourceDomainState State;
    Foundation::UInt8 Reserved[6];
};

/// Supplies one serialization domain for a complete ResourceDomain tree.
/// It must remain alive and immovable until every domain using it is destroyed.
class ResourceDomainLedger final
{
public:
    constexpr ResourceDomainLedger() noexcept = default;

    ResourceDomainLedger(const ResourceDomainLedger&) = delete;
    ResourceDomainLedger& operator=(const ResourceDomainLedger&) = delete;
    ResourceDomainLedger(ResourceDomainLedger&&) = delete;
    ResourceDomainLedger& operator=(ResourceDomainLedger&&) = delete;

private:
    friend class ResourceDomain;
    Platform::Synchronization::SpinLock m_synchronization;
};

/// Enforces bounded, reclaimable resource reservations across one fixed-depth
/// hierarchy. Every reservation is checked against the target and all of its
/// ancestors before any counter changes.
class ResourceDomain final : public Object::IKernelObject
{
public:
    explicit constexpr ResourceDomain(ResourceDomainLedger& ledger) noexcept : m_ledger{ledger}
    {
    }

    /// Preconditions:
    /// - State is Uninitialized or Quiesced;
    /// - no reservation or child remains live.
    ~ResourceDomain() noexcept = default;

    ResourceDomain(const ResourceDomain&) = delete;
    ResourceDomain& operator=(const ResourceDomain&) = delete;
    ResourceDomain(ResourceDomain&&) = delete;
    ResourceDomain& operator=(ResourceDomain&&) = delete;

    /// Initializes a hierarchy root. The identity is boot-unique within the
    /// resource manager and is also the root object's charge owner.
    [[nodiscard]] ResourceDomainFailure InitializeRoot(Object::ResourceDomainId identity,
                                                       const ResourceDomainLimits& limits) noexcept;

    /// Initializes a child and reserves one descendant slot from every
    /// ancestor. Failure returns this object to Uninitialized.
    [[nodiscard]] ResourceDomainFailure InitializeChild(Object::ObjectRegistry& registry,
                                                        Object::ResourceDomainId identity,
                                                        ResourceDomainObjectId parentIdentity, ResourceDomain& parent,
                                                        const ResourceDomainLimits& limits) noexcept;

    /// Atomically reserves the requested quantities in this domain and every
    /// ancestor. A zero reservation is rejected.
    [[nodiscard]] ResourceDomainError TryReserve(const ResourceReservation& reservation) noexcept;

    /// Atomically releases a prior reservation from this domain and every
    /// ancestor. Underflow or releasing committed pages still resident is
    /// rejected without changing any counter.
    [[nodiscard]] ResourceDomainError Release(const ResourceReservation& reservation) noexcept;

    /// Succeeds only after every reservation and descendant has gone away.
    /// A child also releases its one descendant slot from every ancestor.
    /// Marks a drain's root Draining (idempotent): TryReserve and child
    /// creation are then refused for it and every domain below it, while
    /// Release and Quiesce still run. Ready or Draining only.
    [[nodiscard]] ResourceDomainError BeginDrain() noexcept;

    [[nodiscard]] ResourceDomainError Quiesce() noexcept;

    [[nodiscard]] bool IsDescendantOf(const ResourceDomain& ancestor) const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] Object::ResourceDomainId GetIdentifier() const noexcept;
    /// Boot-only, once, before granting imported RAM.
    [[nodiscard]] bool FinalizeMemoryImport(Foundation::UInt64 allocatablePages) noexcept;
    [[nodiscard]] ResourceDomainSnapshot GetSnapshot() const noexcept;
    [[nodiscard, gnu::noinline]] ResourceDomainUsage GetUsage() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    [[nodiscard]] static ResourceDomainFailure
    MakeFailure(ResourceDomainError error, Object::ObjectPinError pin = Object::ObjectPinError::Ok) noexcept;

    ResourceDomainLedger& m_ledger;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Object::ObjectPin m_parentPin;
    ResourceDomain* m_parent{nullptr};
    ResourceDomainObjectId m_parentIdentity{};
    Object::ResourceDomainId m_identity{};
    ResourceDomainLimits m_limits{};
    ResourceDomainUsage m_usage{};
    Foundation::UInt8 m_depth{0U};
    bool m_memoryImported{false};
    ResourceDomainState m_state{ResourceDomainState::Uninitialized};
};

static_assert(sizeof(ResourceDomainObjectId) == 16U);
static_assert(alignof(ResourceDomainObjectId) == 8U);
static_assert(sizeof(ResourceDomainState) == 1U);
static_assert(sizeof(ResourceDomainError) == 1U);
static_assert(sizeof(ResourceDomainLimits) == 80U);
static_assert(alignof(ResourceDomainLimits) == 8U);
static_assert(sizeof(ResourceReservation) == 72U);
static_assert(alignof(ResourceReservation) == 8U);
static_assert(sizeof(ResourceDomainUsage) == 80U);
static_assert(alignof(ResourceDomainUsage) == 8U);
static_assert(sizeof(ResourceDomainFailure) == 8U);
static_assert(alignof(ResourceDomainFailure) == 1U);
static_assert(sizeof(ResourceDomainSnapshot) == 192U);
static_assert(alignof(ResourceDomainSnapshot) == 8U);
} // Namespace Asteria::Kernel::Resource

#endif // ASTERIA_KERNEL_RESOURCE_RESOURCE_DOMAIN_HPP_INCLUDED
