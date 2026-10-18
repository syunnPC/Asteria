// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_THREAD_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_THREAD_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectPoolFailure.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainSubtree.hpp"
#include "Kernel/Scheduler/IUserThreadBackend.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Scheduler
{
enum class ThreadPoolState : Foundation::UInt8
{
    Ready,
    Faulted,
};

/// One ObjectCreate of a Thread.
struct ThreadCreateRequest
{
    /// The caller's capability space: it holds Authority and receives the
    /// new capability.
    Object::CapabilitySpace* Space;
    /// A ResourceDomain capability with Manage in Space: the domain charged.
    Object::CapabilityHandle Authority;
    UserThreadStart Start;
};

struct ThreadCreation
{
    Object::ObjectId Thread;
    Object::CapabilityHandle Capability;
};

struct ThreadPoolSnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::UInt64 Created;
    Foundation::UInt64 Destroyed;
    Foundation::UInt64 Reclaimed;
    /// The exit code of the last thread ObjectDestroy reclaimed.
    Foundation::UInt64 LastExitCode;
    /// The live threads ThreadTerminate ended.
    Foundation::UInt64 Terminated;
    ThreadPoolState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns the threads user code creates through the management ABI: each a Thread and a
/// SchedulingContext constructed in a slot, registered, charged one Threads and their metadata plus
/// a native slot's to the ResourceDomain a Manage capability names, and started in the caller's
/// domain through the platform's IUserThreadBackend, Ready when its home serves the bind. Destroy
/// accepts only an Exited thread: it makes the thread and its SchedulingContext quiescing and
/// returns the exit code; any other state is Busy. Collect ends a quiescing slot once the backend
/// released its native slot (the platform unbinds an exited thread before management calls and in
/// its idle loop) and both registrations can be collected. Create, Destroy and Collect otherwise
/// follow EndpointPool: the capability is minted under the authority and every failure undoes the
/// earlier steps.
class ThreadPool final
{
public:
    /// Thirty-two : about eleven single-thread domains run before any application.
    static constexpr Foundation::Size Capacity{32U};

    static constexpr Object::CapabilityRights CreatedRights{Object::CapabilityRights{Object::CapabilityRight::Inspect}
                                                                .With(Object::CapabilityRight::Manage)
                                                                .With(Object::CapabilityRight::Transfer)
                                                                .With(Object::CapabilityRight::Duplicate)
                                                                .With(Object::CapabilityRight::Derive)};

    /// Preconditions:
    /// - issuer is bound to objectRegistry, and both outlive this pool;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr ThreadPool(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Gives the pool the platform's native slots; once only. Until then
    /// Create is CapacityExceeded.
    [[nodiscard]] bool Configure(IUserThreadBackend& backend) noexcept;

    [[nodiscard]] Foundation::Result<ThreadCreation, Object::ObjectPoolFailure>
    Create(const ThreadCreateRequest& request) noexcept;

    /// Reclaims an Exited thread: its exit code on success.
    [[nodiscard]] Foundation::Result<Foundation::UInt64, Object::ObjectPoolFailure>
    Destroy(Object::CapabilitySpace& space, Object::CapabilityHandle thread) noexcept;

    /// Destroys every live slot charged to a domain of subtree, as Destroy does one; Busy when one
    /// cannot be destroyed yet (a later call takes the rest).
    [[nodiscard]] Object::ObjectPoolFailure DestroyCharged(const Resource::IResourceDomainSubtree& subtree) noexcept;

    /// Whether every live thread charged to a domain of subtree has exited:
    /// a drain's precondition (the kernel never kills one for a drain).
    [[nodiscard]] bool HaveChargedExited(const Resource::IResourceDomainSubtree& subtree) const noexcept;

    /// Ends a live thread this pool created, which space names with Manage, with exitCode through
    /// the backend; an Exited one is left as it is. Busy when it is blocked on a page fault, Busy
    /// for a posted remote request, InvalidRequest for the running caller, InvalidConfiguration
    /// when the backend cannot remove a thread.
    [[nodiscard]] Object::ObjectPoolFailure Terminate(Object::CapabilitySpace& space, Object::CapabilityHandle thread,
                                                      Foundation::UInt64 exitCode) noexcept;

    [[nodiscard]] Foundation::Result<Foundation::Size, Object::ObjectPoolFailure> Collect() noexcept;

    [[nodiscard]] ThreadPoolSnapshot GetSnapshot() const noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Reserved,
        Active,
        Quiescing,
    };

    struct Slot
    {
        alignas(Thread) Foundation::UInt8 ThreadStorage[sizeof(Thread)];
        alignas(SchedulingContext) Foundation::UInt8 ContextStorage[sizeof(SchedulingContext)];
        Thread* ThreadInstance;
        SchedulingContext* ContextInstance;
        Resource::ResourceDomain* Domain;
        Object::ObjectPin DomainPin;
        Resource::ResourceReservation Charge;
        Object::ObjectId ThreadIdentity;
        Object::ObjectId ContextIdentity;
        /// The SchedulingContext released its pin on the Thread.
        bool ContextUnbound;
        // Pins storage over an unlocked backend call.
        bool BackendReserved;
        SlotState State;
    };

    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] Slot* FindSlotLocked(const Object::IKernelObject& instance) noexcept;
    /// Constructs/registers the charged slot, mints its private capability and
    /// reserves native storage and binding capacity; failure undoes all steps.
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure ReserveLocked(Slot& slot, const ThreadCreateRequest& request,
                                                                         Resource::ResourceDomain& domain,
                                                                         Object::CapabilityHandle& minted) noexcept;
    /// Initializes, registers and binds the slot's Thread and SchedulingContext.
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure ConstructLocked(Slot& slot,
                                                                           Resource::ResourceDomain& domain) noexcept;
    /// Undoes registered objects that never ran: closes minted (if valid),
    /// destroys and collects both registrations and frees the slot.
    [[nodiscard, gnu::noinline]] Object::ObjectPoolFailure AbandonLocked(Slot& slot, Object::CapabilitySpace& space,
                                                                         Object::CapabilityHandle minted,
                                                                         Object::ObjectPoolFailure cause) noexcept;
    /// Ends the slot's objects, releases its charge and pin, frees the slot.
    [[nodiscard]] Object::ObjectPoolFailure ReleaseSlotLocked(Slot& slot) noexcept;
    [[nodiscard]] Object::ObjectPoolFailure FaultLocked(Object::ObjectPoolFailure failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    IUserThreadBackend* m_backend{nullptr};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot m_slots[Capacity]{};
    Foundation::UInt64 m_created{0U};
    Foundation::UInt64 m_destroyed{0U};
    Foundation::UInt64 m_reclaimed{0U};
    /// Whether a slot may be Quiescing: set as one becomes so, cleared by a Collect that leaves
    /// none, so a Collect with nothing to reclaim scans nothing.
    bool m_quiescing{false};
    Foundation::UInt64 m_lastExitCode{0U};
    Foundation::UInt64 m_terminated{0U};
    ThreadPoolState m_state{ThreadPoolState::Ready};
};

static_assert(sizeof(ThreadPoolState) == 1U);
static_assert(sizeof(ThreadPoolSnapshot) == 72U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_THREAD_POOL_HPP_INCLUDED
