// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_INTERRUPT_INTERRUPT_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_INTERRUPT_INTERRUPT_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IInterruptLines.hpp"
#include "Hal/Interrupt/IMessageVectors.hpp"
#include "Kernel/Device/PciFunctionControl.hpp"
#include "Kernel/Interrupt/Interrupt.hpp"
#include "Kernel/Interrupt/InterruptLine.hpp"
#include "Kernel/Interrupt/MessageSignaledSource.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Interrupt
{
/// One ObjectCreate of an Interrupt.
struct InterruptCreateRequest
{
    /// The caller's capability space: it holds Authority and Function and
    /// receives the new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
    /// Source=1 names an InterruptLine (Invoke), otherwise a PciFunction. Caller,
    /// the caller's ProtectionDomain, must be a PCI function's assignee.
    Object::CapabilityHandle Function;
    Protection::ProtectionDomainId Caller;
    /// The function's interrupt as the platform routes it: 0, its MSI where
    /// the pool has message vectors, its INTx line where it has lines.
    Foundation::UInt64 Source;
};

struct InterruptCreation
{
    Object::ObjectId Interrupt;
    Object::CapabilityHandle Capability;
};

/// What WaitSetBind registers for an Interrupt: its Notification.
struct InterruptReadiness
{
    Ipc::NotificationId Identity;
    Ipc::Notification* Notification;
};

struct InterruptPoolSnapshot
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

/// The Interrupts user code creates for its assigned functions' messages. Each slot holds the
/// Interrupt, its software-masked message source and its own Notification, both registered. Create
/// charges the authority's ResourceDomain, initializes the Notification and the Interrupt (Masked),
/// binds a processor vector, and has the device control program the function's MSI to raise it; the
/// caller must be the function's assignee, one Interrupt per function. Destroy and the drain turn
/// the messages off, acknowledge and mask, and quiesce, all at once; the vector stays bound until
/// Collect, so a late message lands on the quiesced Interrupt. Collect frees a slot once neither
/// object is referenced: a live WaitSet registration keeps the Notification, and so the slot, until
/// that WaitSet goes. Without Configure every Create is InvalidConfiguration. Every operation runs
/// on the processor messages target with interrupts disabled (Interrupt.md), so none interleaves
/// with the handler. One lock (rank 1) covers the slots; it calls the device control (rank 2), the
/// Interrupt (rank 5) and the registry.
class InterruptPool final
{
public:
    static constexpr Foundation::Size Capacity{4U};
    /// The objects each slot registers: the Interrupt and its Notification.
    static constexpr Foundation::Size RegisteredObjectsPerSlot{2U};
    /// The rights of a created Interrupt's first capability: Inspect, Wait,
    /// Transfer, Duplicate, Derive and Manage, without Revoke.
    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Wait)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)
                                                                .With(Object::CapabilityRight::Manage)};
    /// InterruptControl's operations: acknowledge and arm, or acknowledge and
    /// keep masked.
    static constexpr Foundation::UInt64 Rearm{1U};
    static constexpr Foundation::UInt64 KeepMasked{2U};

    constexpr InterruptPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    InterruptPool(const InterruptPool&) = delete;
    InterruptPool& operator=(const InterruptPool&) = delete;

    /// Once: the platform's vectors and the device control.
    [[nodiscard]] bool Configure(Hal::Interrupt::IMessageVectors& vectors,
                                 Device::PciFunctionControl& functions) noexcept;

    /// Once, in place of Configure on a platform whose drivers have no MSI: an Interrupt takes its
    /// function's INTx line, which lines route, and the device control enables and disables the
    /// function's INTx.
    [[nodiscard]] bool ConfigureLines(Hal::Interrupt::IInterruptLines& lines,
                                      Device::PciFunctionControl& functions) noexcept;

    [[nodiscard]] Foundation::Result<InterruptCreation, Object::ObjectPoolFailure>
    Create(const InterruptCreateRequest& request) noexcept;

    /// Ends the Interrupt that interrupt, a capability with Manage in space,
    /// names. One this pool does not own is NotOwned.
    [[nodiscard]] Foundation::Result<Object::ObjectId, Object::ObjectPoolFailure>
    Destroy(Object::CapabilitySpace& space, Object::CapabilityHandle interrupt) noexcept;

    /// The drain's first device: ends every live Interrupt
    /// charged to a domain of subtree.
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;

    /// InterruptControl (Manage): Rearm acknowledges a pending event and arms
    /// (and arms a masked Interrupt without one); KeepMasked acknowledges a
    /// pending event and masks. Returns the last event's sequence.
    [[nodiscard]] Foundation::Result<Foundation::UInt64, Object::ObjectPoolFailure>
    Control(Object::CapabilitySpace& space, Object::CapabilityHandle interrupt, Foundation::UInt64 operation) noexcept;

    /// The Notification of the Interrupt interrupt (Wait) names, for
    /// WaitSetBind.
    [[nodiscard]] Foundation::Result<InterruptReadiness, Object::ObjectPoolFailure>
    FindReadiness(Object::CapabilitySpace& space, Object::CapabilityHandle interrupt) noexcept;

    [[nodiscard]] InterruptPoolSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] static constexpr Resource::ResourceReservation GetReservation() noexcept
    {
        return Resource::ResourceReservation{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = sizeof(Interrupt) + sizeof(Ipc::Notification),
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
        alignas(Ipc::Notification) Foundation::UInt8 NotificationStorage[sizeof(Ipc::Notification)];
        alignas(Interrupt) Foundation::UInt8 InterruptStorage[sizeof(Interrupt)];
        MessageSignaledSource Source;
        /// The line's source the Interrupt is built on, where lines route
        /// it; null for a message.
        Hal::Interrupt::IInterruptSource* Line;
        Ipc::Notification* Notification;
        Interrupt* Instance;
        const Device::PciFunction* Function;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin DomainPin;
        Object::ObjectPin LinePin;
        Foundation::UInt32 GlobalSystemInterrupt;
        Object::ObjectId Identity;
        Object::ObjectId NotificationIdentity;
        Foundation::UInt64 Vector;
        /// The function's messages, or its INTx, are enabled.
        bool MessagesEnabled;
        bool InterruptCollected;
        bool NotificationCollected;
        SlotState State;
    };

    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] Slot* FindSlotLocked(const Object::IKernelObject& instance) noexcept;
    /// The created slot's vector, messages, registration and capability.
    [[gnu::noinline]] [[nodiscard]] Foundation::Result<InterruptCreation, Object::ObjectPoolFailure>
    CompleteLocked(Slot& slot, const InterruptCreateRequest& request) noexcept;
    enum class StopOutcome : Foundation::UInt8
    {
        Stopped,
        /// The messages are still on: the Interrupt is masked, not quiesced.
        Retry,
        Failed,
    };

    /// Messages off, the Interrupt acknowledged, masked and quiesced.
    [[nodiscard]] StopOutcome StopLocked(Slot& slot) noexcept;
    /// Stops the slot and destroys both registrations; the slot quiesces.
    [[nodiscard]] Object::ObjectPoolFailure DestroySlotLocked(Slot& slot) noexcept;
    /// Undoes a creation that registered nothing callers can reach.
    void AbandonLocked(Slot& slot) noexcept;
    void ReleaseSlotLocked(Slot& slot) noexcept;
    /// Reserves the slot's function's INTx line and its vector.
    [[nodiscard]] Object::ObjectPoolFailure ReserveLineLocked(Slot& slot) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure CollectRegistrationLocked(Object::ObjectId identity,
                                                                      const Object::IKernelObject& instance,
                                                                      bool& collected) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure FaultLocked(Object::ObjectPoolFailure failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Hal::Interrupt::IMessageVectors* m_vectors{nullptr};
    Hal::Interrupt::IInterruptLines* m_lines{nullptr};
    Device::PciFunctionControl* m_functions{nullptr};
    Slot m_slots[Capacity]{};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    bool m_faulted{false};
};
} // Namespace Asteria::Kernel::Interrupt

#endif // ASTERIA_KERNEL_INTERRUPT_INTERRUPT_POOL_HPP_INCLUDED
