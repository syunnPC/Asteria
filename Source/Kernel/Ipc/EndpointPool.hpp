// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_ENDPOINT_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_ENDPOINT_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Ipc
{
enum class EndpointPoolState : Foundation::UInt8
{
    Ready,
    Faulted,
};

using EndpointPoolError = Object::ObjectPoolError;
using EndpointPoolFailure = Object::ObjectPoolFailure;

/// One ObjectCreate of an Endpoint.
struct EndpointCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
    EndpointConfiguration Configuration;
    /// The Endpoint's receiver space: null for Space itself, or a created CapabilitySpace, whose
    /// registered object ReceiverObject names and the slot pins while the Endpoint lives.
    Object::CapabilitySpace* Receiver;
    Object::ObjectId ReceiverObject;
    /// The sealed selector the creator's capability carries, and every copy of it; zero for none.
    Foundation::UInt64 SealedSelector;
};

struct EndpointCreation
{
    Object::ObjectId Endpoint;
    Object::CapabilityHandle Capability;
};

struct EndpointPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    EndpointPoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns a fixed set of Endpoint slots that user code creates and destroys through the management
/// ABI. Create charges the ResourceDomain named by a Manage capability MetadataBytes in
/// kernel-metadata bytes and the configured queue bytes in IPC-queue bytes, constructs the Endpoint
/// and its readiness Notification in a free slot, registers both, and mints the creator's
/// capability under that authority. Destroy makes an owned Endpoint and its Notification quiescing.
/// Collect reclaims the two registrations as each loses its last lease and pin (a WaitSet's
/// registration pins the Notification) and the slot once both are gone: it discards unread
/// messages, ends both lifetimes, releases the charge, and unpins the domain. A failure that leaves
/// no object behind changes nothing; a rollback that cannot complete faults the pool, which then
/// refuses every operation. Every receiver space must outlive the Endpoints created in it until
/// they are collected: the caller's own by its owner, a created one by the slot's pin on its
/// object.
class EndpointPool final
{
public:
    /// Forty-eight.
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::Endpoint)]};

    /// The objects each slot registers: the Endpoint and its readiness.
    static constexpr Foundation::Size RegisteredObjectsPerSlot{2U};

    /// The kernel metadata one created Endpoint is charged.
    static constexpr Foundation::Size MetadataBytes{sizeof(Endpoint) + sizeof(Notification)};

    /// The rights of a created Endpoint's first capability: every right an Endpoint uses, without
    /// Revoke.
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Invoke)
                                                                .With(Object::CapabilityRight::Read)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr EndpointPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    /// Preconditions:
    /// - every slot is Free.
    ~EndpointPool() noexcept = default;

    EndpointPool(const EndpointPool&) = delete;
    EndpointPool& operator=(const EndpointPool&) = delete;
    EndpointPool(EndpointPool&&) = delete;
    EndpointPool& operator=(EndpointPool&&) = delete;

    [[nodiscard]] Foundation::Result<EndpointCreation, EndpointPoolFailure>
    Create(const EndpointCreateRequest& request) noexcept;

    /// Makes the Endpoint that endpoint, a capability with Manage in space,
    /// names quiescing: every capability to it fails ObjectNotFound from now
    /// on and stays closeable. The caller's own lease ends when Destroy
    /// returns, so the slot is collected by a later Collect.
    [[nodiscard]] Foundation::Result<Object::ObjectId, EndpointPoolFailure>
    Destroy(Object::CapabilitySpace& space, Object::CapabilityHandle endpoint) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// Reclaims every quiescing slot whose Endpoint is no longer referenced
    /// and returns how many it reclaimed.
    [[nodiscard]] Foundation::Result<Foundation::Size, EndpointPoolFailure> Collect() noexcept;

    [[nodiscard]] EndpointPoolSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] static constexpr Resource::ResourceReservation
    GetReservation(const EndpointConfiguration& configuration) noexcept
    {
        return Resource::ResourceReservation{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = MetadataBytes,
            .CapabilityHandles = 0U,
            .IpcQueueBytes = configuration.MaximumQueuedBytes,
            .Threads = 0U,
            .Objects = {0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U},
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
        alignas(Endpoint) Foundation::UInt8 Storage[sizeof(Endpoint)];
        alignas(Notification) Foundation::UInt8 ReadinessStorage[sizeof(Notification)];
        Endpoint* Instance;
        Notification* Readiness;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin DomainPin;
        Object::ObjectPin ReceiverPin;
        Resource::ResourceReservation Charge;
        Object::ObjectId Identity;
        Object::ObjectId ReadinessIdentity;
        SlotState State;
        /// Which registrations a quiescing slot has collected; the registry
        /// finalizes each once.
        bool EndpointCollected;
        bool ReadinessCollected;
    };

    [[nodiscard]] static EndpointPoolFailure
    MakeFailure(EndpointPoolError error, Object::CapabilityError capability = Object::CapabilityError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None,
                Object::ObjectRegistryError registry = Object::ObjectRegistryError::Ok,
                EndpointErrorCode endpoint = EndpointErrorCode::None) noexcept
    {
        return Object::MakeObjectPoolFailure(error, capability, resource, registry,
                                             static_cast<Foundation::UInt8>(endpoint));
    }
    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] Slot* FindSlotLocked(const Object::IKernelObject& instance) noexcept;
    /// Collects one registration of a quiescing slot: true once it is
    /// collected (now or before), false while it is still referenced.
    [[nodiscard]] EndpointPoolFailure CollectRegistrationLocked(Object::ObjectId identity,
                                                                const Object::IKernelObject& instance,
                                                                bool& collected) noexcept;
    /// Registers the slot's Notification and attaches it as the registered
    /// Endpoint's readiness; on failure undoes the whole slot.
    [[gnu::noinline]] [[nodiscard]] EndpointPoolFailure RegisterReadinessLocked(Slot& slot) noexcept;
    /// Destroys and collects registrations no capability names yet.
    [[nodiscard]] Object::ObjectRegistryError UnregisterLocked(Object::ObjectId identity) noexcept;
    /// Ends the slot's Endpoint and Notification, releases its charge and
    /// pins, and frees it.
    [[nodiscard]] EndpointPoolFailure ReleaseSlotLocked(Slot& slot) noexcept;
    [[nodiscard]] EndpointPoolFailure FaultLocked(EndpointPoolFailure failure) noexcept;

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
    EndpointPoolState m_state{EndpointPoolState::Ready};
};

static_assert(sizeof(EndpointPoolState) == 1U);
static_assert(sizeof(EndpointPoolSnapshot) == 56U);
static_assert(alignof(EndpointPoolSnapshot) == 8U);
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_ENDPOINT_POOL_HPP_INCLUDED
