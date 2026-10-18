// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_WAIT_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_WAIT_RUNTIME_HPP_INCLUDED

#include "Abi/BootstrapFixture.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Kernel/Time/DeferredTimerWorker.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"

namespace Asteria::Platform::Amd64::System
{
/// The tag the wait fixture registers its timer Notification with
/// (Abi/BootstrapFixture.hpp).
using Abi::BootstrapWaitTag;

/// One permanently owned CPL3 thread and one outstanding syscall continuation. All operations run
/// on CPU0 with interrupts disabled. The IRQ only publishes an atomic count; object delivery and
/// scheduler wake happen on the idle stack. The initial component's diagnostic timer signals the
/// registered WaitSet: every suspended wait arms it once. The runtime fail-stops only when an
/// operation is rejected or the thread, run loop or dispatcher is not in the state the next step
/// requires; the response and what each resumption observed are passed through and recorded in the
/// evidence record (Diagnostics::Amd64WaitEvidence), not compared with the fixture's values.
class Amd64WaitRuntime final
{
public:
    Amd64WaitRuntime(Kernel::Object::ObjectRegistry& registry, Kernel::Time::TimerQueue& queue,
                     Time::Amd64ApicTimer& timer, Kernel::Scheduler::ProcessorRunController& controller,
                     Scheduler::Amd64ProcessorRunLoop& loop, Kernel::Scheduler::Thread& thread,
                     Scheduler::Amd64KernelContext& context) noexcept;
    [[nodiscard]] bool Initialize(Kernel::Object::CapabilitySpace& capabilities,
                                  Kernel::Object::RevocationRegistry& revocations) noexcept;
    [[nodiscard]] bool Dispatch(const Kernel::System::SystemCallRequest& request,
                                Kernel::System::SystemCallResponse& response) noexcept;
    /// Whether a wait is suspended until its timer resumes it: from its arm
    /// until ResumeFromIdle published the wake.
    [[nodiscard]] bool IsSuspended() const noexcept
    {
        return m_suspended;
    }
    /// Idle stack, a wait suspended: waits for its timer's interrupt and
    /// publishes the wake. An interrupt that arrives before the wait's
    /// deadline is another deadline's (a budget replenishment, counted because
    /// every kernel-mode tick counts): it returns true at once with the wait
    /// still suspended, so the idle loop dispatches what became runnable.
    [[nodiscard]] bool ResumeFromIdle() noexcept;
    /// Idle stack only, while a wait is suspended: dispatching found the
    /// fixture's timer due before the local deadline could be programmed
    /// (DeadlineReached: the vCPU was not scheduled for the whole delay), so
    /// no interrupt will come for it. Arms the timer again with a new delay:
    /// the wait still resumes through one interrupt after its deadline. False
    /// if no wait is suspended or the arm is rejected.
    [[nodiscard]] bool RearmDueTimer() noexcept;
    /// Whether a wait is suspended and its timer's deadline has passed. A
    /// DeadlineReached dispatch while the timer is not yet due was another
    /// admission's (a run stop that passed before entry): the idle loop
    /// retries it and leaves the wait's timer armed.
    [[nodiscard]] bool IsTimerDue() const noexcept;
    /// Whether a timer interrupt reached the suspended wait since its timer
    /// was armed: the idle loop then drains it (ResumeFromIdle returns at
    /// once) instead of arming the timer again.
    [[nodiscard]] bool HasObservedInterrupt() const noexcept
    {
        return m_suspended && (m_interrupts.LoadAcquire() != m_armedInterrupts);
    }
    /// A BSP timer interrupt the fixture observes: every one taken in kernel context, and a user
    /// tick only when the TimerQueue deadline caused it, so a suspended wait counts the interrupt
    /// of its own timer.
    void OnTimerInterrupt() noexcept;
    [[nodiscard]] Kernel::Object::CapabilityHandle GetCapability() const noexcept
    {
        return m_capability;
    }
    /// Why ResumeFromIdle last failed: 1 a precondition, 2 no interrupt in
    /// time, 3 the drain, 4 the deadline refresh, 0x50 | state the poll.
    [[nodiscard]] Foundation::UInt8 GetFailureStage() const noexcept
    {
        return m_failureStage;
    }

private:
    [[nodiscard]] bool Reject(Foundation::UInt8 stage) noexcept;
    [[nodiscard, gnu::noinline]] bool Arm() noexcept;
    [[nodiscard, gnu::noinline]] bool CompleteFromIdle() noexcept;
    Kernel::Object::ObjectRegistry& m_registry;
    Kernel::Time::TimerQueue& m_queue;
    Time::Amd64ApicTimer& m_timer;
    Kernel::Scheduler::ProcessorRunController& m_controller;
    Scheduler::Amd64ProcessorRunLoop& m_loop;
    Kernel::Scheduler::Thread& m_thread;
    Scheduler::Amd64KernelContext& m_context;
    Kernel::Time::Timer m_object;
    Kernel::Ipc::Notification m_notification;
    Kernel::Ipc::WaitSet m_waitSet;
    Kernel::Time::DeferredTimerWorker m_deferred;
    Kernel::Ipc::WaitSetThreadDispatcher m_wait;
    Kernel::System::WaitSetSystemCallDispatcher m_dispatcher;
    Kernel::Time::TimerId m_timerId{};
    Kernel::Object::CapabilityHandle m_capability{};
    Kernel::System::WaitSetSystemCallDispatchResult m_result{};
    Kernel::System::WaitSetSystemCallIngressFailure m_failure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_receipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_switchFailure{};
    Synchronization::AtomicUInt64 m_interrupts;
    Foundation::UInt64 m_armedInterrupts{0U};
    Foundation::UInt64 m_deadline{0U};
    Foundation::UInt8 m_failureStage{0U};
    bool m_initialized{false};
    bool m_suspended{false};
};
} // Namespace Asteria::Platform::Amd64::System
#endif
