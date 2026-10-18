// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_I_USER_THREAD_BACKEND_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_I_USER_THREAD_BACKEND_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"

namespace Asteria::Kernel::Protection
{
class ProtectionDomain;
}

namespace Asteria::Kernel::Scheduler
{
/// Where a created user thread starts, and in which domain: null for the caller's, or a
/// ProtectionDomain the caller manages.
struct UserThreadStart
{
    Foundation::UInt64 Entry;
    Foundation::UInt64 UserStackTop;
    Foundation::UInt64 Argument;
    const Protection::ProtectionDomain* Domain;
};

/// The scheduling a created thread receives and the time its
/// SchedulingContext starts at.
struct UserThreadSchedule
{
    SchedulingContextConfiguration Configuration;
    Object::MonotonicTime Now;
};

enum class UserThreadBackendError : Foundation::UInt8
{
    None,
    /// No native slot is free.
    Exhausted,
    /// The start is not one the platform admits (an entry or stack outside
    /// the domain's image, for example).
    Rejected,
    /// The platform could not complete or undo the start.
    Faulted,
};

/// Why the platform did not end a thread.
enum class UserThreadTerminateError : Foundation::UInt8
{
    None,
    /// Posted to the home processor, or an earlier request is still pending.
    Pending,
    /// Blocked on a page fault: its supervisor ends it through FaultResolve.
    Blocked,
    /// Running: the caller itself.
    Running,
    /// The platform cannot take a thread out of its run queue.
    Unsupported,
    /// The platform could not complete or undo the removal.
    Faulted,
};

/// The platform side of thread creation: native slots (kernel context, extended-state area, kernel
/// stack, launch frame) in the calling thread's domain, bound to the processor's run loop.
class IUserThreadBackend
{
public:
    /// Kernel-metadata bytes one native slot costs, charged with the thread.
    [[nodiscard]] virtual Foundation::UInt64 GetNativeSlotBytes() const noexcept = 0;

    [[nodiscard]] virtual UserThreadSchedule GetSchedule() const noexcept = 0;

    /// Under the pool lock, validate the launch and reserve its native slot and home binding
    /// capacity. No wait/post.
    [[nodiscard]] virtual UserThreadBackendError Reserve(Thread&, ThreadId, SchedulingContext&, SchedulingContextId,
                                                         const UserThreadStart&) noexcept = 0;

    /// Starts the prepared native slot in the calling thread's domain,
    /// binds it to the home run loop and
    /// makes the Dormant thread Ready with its bound SchedulingContext. A remote
    /// start posts a bind and returns before the home initializes its time. Every
    /// failure leaves no slot, binding or placement behind.
    [[nodiscard]] virtual UserThreadBackendError Start(Thread& thread, ThreadId threadIdentity,
                                                       SchedulingContext& context, SchedulingContextId contextIdentity,
                                                       const UserThreadStart& start) noexcept = 0;

    /// Whether thread's native slot was released: it exited and was unbound,
    /// so its Thread and SchedulingContext may end.
    [[nodiscard]] virtual bool IsReleased(const Thread& thread) const noexcept = 0;

    /// Locally removes a Ready thread or aborts a WaitSet wait; remotely posts to its home and
    /// returns Pending. A fault remains owned by FaultResolve. Exited storage is released by the
    /// reaper.
    [[nodiscard]] virtual UserThreadTerminateError Terminate(Thread& thread, Foundation::UInt64 exitCode) noexcept = 0;

protected:
    constexpr IUserThreadBackend() noexcept = default;
    ~IUserThreadBackend() noexcept = default;
};

static_assert(sizeof(UserThreadBackendError) == 1U);
static_assert(sizeof(UserThreadTerminateError) == 1U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_I_USER_THREAD_BACKEND_HPP_INCLUDED
