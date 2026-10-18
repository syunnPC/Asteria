// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_APPLICATION_PROCESSOR_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_APPLICATION_PROCESSOR_RUNTIME_HPP_INCLUDED

#include "Kernel/Memory/TlbShootdownDrain.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Time/DeferredTimerWorker.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"
#include "Platform/Amd64/System/Amd64WaitRuntime.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Kernel::System
{
class ObjectManager;
}
namespace Asteria::Platform::Amd64::System
{
class Amd64UserThreadTable;
}
namespace Asteria::Platform::Amd64::Scheduler
{
struct Amd64ApplicationProcessorInput
{
    Kernel::Object::ObjectRegistry* Registry;
    Kernel::Resource::ResourceDomain* Resources;
    Kernel::Resource::ResourceDomainObjectId ResourceIdentity;
    Kernel::Memory::AddressSpace* SharedSpace;
    Kernel::Memory::AddressSpace* IdleSpace;
    System::Amd64UserThreadTable* UserThreads;
    Kernel::System::ObjectManager* Objects;
    Interrupt::Amd64DescriptorTables* Descriptors;
    Interrupt::Amd64EntryContext* EntryContext;
    WritableAmd64KernelStackView WorkerStack;
    Foundation::UInt64 ApicAddress;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt32 HardwareId;
    bool X2Apic;
};

/// What an application processor observed while it proved its local timer
/// and one deferred timer delivery, before it reported serving. The worker's
/// first dispatch has blocked it (InitialRuns); a TimerQueue timer armed for
/// RequestedDeadlineTicks raised ObservedInterrupts local-timer interrupts by
/// ObservedTicks; the DeferredTimerWorker then drained the queue (the drain
/// receipt's count, stop reason and final queue state, and whether every
/// delivered timer was the AP's own), the AP claimed and consumed the
/// Notification, and the controller refreshed the deadline (its state, the
/// local timer's deadline, and the xAPIC LVT timer entry and initial count
/// read back afterwards). Counts, sequences and states are recorded as
/// observed; the kernel does not compare them with what the scenario expects.
/// Enumerations are stored as their underlying values.
struct Amd64ApplicationProcessorTimerObservation
{
    Foundation::UInt64 InitialRuns;
    Foundation::UInt64 RequestedDeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    Foundation::UInt64 ObservedInterrupts;
    Foundation::UInt64 DeliveredCount;
    Foundation::UInt64 DrainStop;
    Foundation::UInt64 DeliveredOwnTimer;
    Foundation::UInt64 QueueNextDeadlineTicks;
    Foundation::UInt64 QueueScheduledEntries;
    Foundation::UInt64 QueuePendingEntries;
    Foundation::UInt64 QueueClaimedEntries;
    Foundation::UInt64 NotificationClaimSequence;
    Foundation::UInt64 NotificationState;
    Foundation::UInt64 NotificationPending;
    Foundation::UInt64 NotificationLastClaimSequence;
    Foundation::UInt64 DisarmedDeadlineState;
    Foundation::UInt64 DisarmedDeadlineTicks;
    Foundation::UInt64 DisarmedTimerLvt;
    Foundation::UInt64 DisarmedInitialCount;
};

/// One CPU's permanent kernel service context. The BSP constructs it before
/// INIT/SIPI. Service requests have one publisher, one outstanding sequence,
/// and an acquire/release acknowledgement before arguments may be reused.
/// The AP initiates a system stop (ApplicationProcessorStage) when one of its operations is
/// rejected or its scheduler or timer is not in the state the next step
/// requires; what its diagnostic timer scenario observed is recorded
/// (GetTimerObservation), not judged.
class Amd64ApplicationProcessorRuntime final
{
public:
    explicit Amd64ApplicationProcessorRuntime(const Amd64ApplicationProcessorInput& input) noexcept;
    [[noreturn]] void Run() noexcept;
    [[nodiscard]] bool OnTimerInterrupt() noexcept;
    void EnableUsers() noexcept
    {
        m_usersEnabled.StoreRelease(1U);
    }
    void EndOfInterrupt() noexcept
    {
        m_timer.EndOfInterrupt();
    }
    [[nodiscard]] Time::Amd64ApicTimer& GetLocalTimer() noexcept
    {
        return m_timer;
    }
    [[nodiscard]] Kernel::Scheduler::ProcessorRunController& GetController() noexcept
    {
        return m_controller;
    }
    [[nodiscard]] Amd64ProcessorRunLoop& GetRunLoop() noexcept
    {
        return m_loop;
    }
    [[nodiscard]] System::Amd64WaitRuntime& GetWaitRuntime() noexcept
    {
        return m_waitRuntime;
    }
    /// Publishes the next service request (one outstanding at a time); it is
    /// complete once GetCompleted reaches the previous count plus one.
    [[nodiscard]] bool RequestService(Foundation::UInt64 address = 0U) noexcept;
    [[nodiscard]] Foundation::UInt64 GetState() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetCompleted() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetRuns() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetTimerInterrupts() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetTimerDeliveries() const noexcept;
    /// The word the worker read at the address of the last completed request.
    [[nodiscard]] Foundation::UInt64 GetObservation() const noexcept;
    /// Valid once GetState returned 1 (the acquire orders it after the AP's writes).
    [[nodiscard]] const Amd64ApplicationProcessorTimerObservation& GetTimerObservation() const noexcept;
    /// Before the first startup IPI only; the drain remains valid permanently.
    void BindDrain(Kernel::Memory::ITlbShootdownDrain& drain) noexcept;

private:
    [[nodiscard, gnu::noinline]] bool PrepareObjects() noexcept;
    [[nodiscard, gnu::noinline]] bool AdmitWorker() noexcept;
    [[nodiscard, gnu::noinline]] bool WakeWorker() noexcept;
    [[nodiscard, gnu::noinline]] bool DispatchBlocked() noexcept;
    [[noreturn, gnu::noinline]] static void Worker(void* argument) noexcept;
    [[noreturn]] void Fail(Foundation::UInt64 stage) noexcept;
    void Service() noexcept;
    [[nodiscard, gnu::noinline]] bool ServiceUsers() noexcept;
    [[nodiscard]] bool ProbeTimer() noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareTimerDelivery() noexcept;
    [[nodiscard, gnu::noinline]] bool CompleteTimerDelivery() noexcept;

    Amd64ApplicationProcessorInput m_input;
    Kernel::Object::RootCapabilityIssuer m_issuer;
    Time::Amd64ApicTimer m_timer;
    Kernel::Time::TimerQueue m_queue;
    Kernel::Scheduler::ProcessorRunController m_controller;
    Kernel::Memory::ProcessorAddressSpaceController m_addressSpaces;
    Amd64KernelContext m_idle;
    Amd64KernelContext m_worker;
    Amd64ProcessorRunLoop m_loop;
    System::Amd64WaitRuntime m_waitRuntime;
    Kernel::Protection::ProtectionDomain m_protection;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_scheduling;
    Kernel::Time::Timer m_timerObject;
    Kernel::Ipc::Notification m_notification;
    Kernel::Time::DeferredTimerWorker m_deferred;
    Kernel::Time::TimerId m_timerId{};
    Kernel::Scheduler::ThreadId m_threadId{};
    Kernel::Scheduler::SchedulingContextId m_schedulingId{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_idleReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_idleFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_workerReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_workerFailure{};
    Kernel::Memory::ITlbShootdownDrain* m_drain{nullptr};
    Synchronization::AtomicUInt64 m_usersEnabled;
    bool m_idleAdopted{false};
    Foundation::UInt64 m_idleEntryStackTop{0U};
    Synchronization::AtomicUInt64 m_state;
    Synchronization::AtomicUInt64 m_requested;
    Synchronization::AtomicUInt64 m_completed;
    Synchronization::AtomicUInt64 m_runs;
    Synchronization::AtomicUInt64 m_timerInterrupts;
    Synchronization::AtomicUInt64 m_timerDeliveries;
    Foundation::UInt64 m_observeAddress{0U};
    Foundation::UInt64 m_observation{0U};
    Amd64ApplicationProcessorTimerObservation m_timerObservation{};
};

static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, InitialRuns) == 0U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, RequestedDeadlineTicks) == 8U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, ObservedTicks) == 16U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, ObservedInterrupts) == 24U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, DeliveredCount) == 32U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, DrainStop) == 40U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, DeliveredOwnTimer) == 48U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, QueueNextDeadlineTicks) == 56U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, QueueScheduledEntries) == 64U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, QueuePendingEntries) == 72U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, QueueClaimedEntries) == 80U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, NotificationClaimSequence) == 88U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, NotificationState) == 96U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, NotificationPending) == 104U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, NotificationLastClaimSequence) == 112U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, DisarmedDeadlineState) == 120U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, DisarmedDeadlineTicks) == 128U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, DisarmedTimerLvt) == 136U);
static_assert(__builtin_offsetof(Amd64ApplicationProcessorTimerObservation, DisarmedInitialCount) == 144U);
static_assert(sizeof(Amd64ApplicationProcessorTimerObservation) == 152U);
} // Namespace Asteria::Platform::Amd64::Scheduler
#endif
