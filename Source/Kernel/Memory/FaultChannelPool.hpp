// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_CHANNEL_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_CHANNEL_POOL_HPP_INCLUDED
#include "Kernel/Memory/FaultChannel.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
namespace Asteria::Kernel::Memory
{
struct FaultChannelPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    Foundation::UInt64 Deliveries;
    Foundation::UInt64 Resolutions;
    Foundation::UInt64 Terminations;
};
struct FaultChannelMetrics
{
    Foundation::UInt64 Identity{0U};
    Foundation::UInt64 Deliveries{0U};
    Foundation::UInt64 Resolutions{0U};
    Foundation::UInt64 Terminations{0U};
    Foundation::UInt64 Busy{0U};
};
class FaultChannelPool final
{
public:
    static constexpr Foundation::Size Capacity{
        Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::FaultChannel)]};
    // Channel, readiness and one private token factory.
    static constexpr Foundation::Size RegisteredObjectsPerSlot{2U};
    static constexpr Foundation::Size MaximumRegisteredObjects{
        Capacity * (RegisteredObjectsPerSlot + FaultTokenFactory::MaximumTokens)};
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Read)
                                                                .With(Object::CapabilityRight::Manage)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Derive)};
    constexpr FaultChannelPool(Object::ObjectRegistry& registry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_registry{registry}, m_issuer{issuer}
    {
    }
    [[nodiscard]] bool Configure(Object::RevocationRegistry& revocations, Time::IMonotonicClock& clock,
                                 const Object::CapabilityValidationKey* keys,
                                 Scheduler::ProcessorSchedulerWakeFacade& wake, TlbShootdownDispatcher& shootdown,
                                 Hal::Memory::ITlbShootdownTransport& transport,
                                 IPagerShootdownProgress& progress) noexcept;
    [[nodiscard]] Foundation::Result<Object::CapabilityHandle, Object::ObjectPoolFailure>
    Create(Object::CapabilitySpace& caller, Object::CapabilityHandle authority, Object::ObjectId receiver) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure Destroy(Object::CapabilitySpace& caller,
                                                    Object::CapabilityHandle channel) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;
    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;
    [[nodiscard]] bool Progress() noexcept;
    [[nodiscard]] FaultChannelPoolSnapshot GetSnapshot() const noexcept;
    [[nodiscard]] FaultChannelMetrics GetMetrics(Foundation::Size index) const noexcept;
    [[nodiscard]] static constexpr Resource::ResourceReservation GetReservation() noexcept
    {
        return {.CommittedMemoryPages = 0U,
                .ResidentMemoryPages = 0U,
                .KernelMetadataBytes = sizeof(FaultChannel),
                .CapabilityHandles = 0U,
                .IpcQueueBytes = Ipc::EndpointConfiguration::Default().MaximumQueuedBytes,
                .Threads = 0U,
                .Objects = {0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U}};
    }

private:
    enum class State : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing
    };
    struct Slot
    {
        alignas(FaultChannel) Foundation::UInt8 Storage[sizeof(FaultChannel)];
        FaultChannel* Instance;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin DomainPin;
        Object::ObjectPin ReceiverPin;
        Object::ObjectId Identity;
        Object::ObjectId Readiness;
        Object::RevocationDomainId Root;
        State Status;
        FaultChannelMetrics Metrics{};
    };
    [[nodiscard, gnu::noinline]] Foundation::Result<Object::CapabilityHandle, Object::ObjectPoolFailure>
    Publish(Slot& slot, Object::CapabilitySpace& caller, Object::CapabilityHandle authority) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure DestroySlot(Slot& slot) noexcept;
    [[nodiscard]] bool ReleaseSlot(Slot& slot) noexcept;
    Object::ObjectRegistry& m_registry;
    Object::RootCapabilityIssuer& m_issuer;
    Object::RevocationRegistry* m_revocations{nullptr};
    Time::IMonotonicClock* m_clock{nullptr};
    Scheduler::ProcessorSchedulerWakeFacade* m_wake{nullptr};
    TlbShootdownDispatcher* m_shootdown{nullptr};
    Hal::Memory::ITlbShootdownTransport* m_transport{nullptr};
    IPagerShootdownProgress* m_progress{nullptr};
    Object::CapabilityValidationKey m_keys[Capacity]{};
    mutable Platform::Synchronization::SpinLock m_lock;
    Slot m_slots[Capacity]{};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    Foundation::UInt64 m_deliveries{0U};
    Foundation::UInt64 m_resolutions{0U};
    Foundation::UInt64 m_terminations{0U};
    bool m_faulted{false};
};
} // Namespace Asteria::Kernel::Memory
#endif
