// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_USER_THREAD_TABLE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_USER_THREAD_TABLE_HPP_INCLUDED

#include "Foundation/IndexSequence.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Memory/IPagerResolution.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Memory/PagerChannel.hpp"
#include "Kernel/Memory/RecordUserMemoryAccess.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Protection/ProtectionDomainPool.hpp"
#include "Kernel/Scheduler/IUserThreadBackend.hpp"
#include "Kernel/Scheduler/ThreadRequest.hpp"
#include "Kernel/Scheduler/ThreadPool.hpp"
#include "Kernel/System/IObjectManagement.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/System/WaitSetSystemCallDispatcher.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Kernel/Memory/FaultDeferral.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"

namespace Asteria::Platform::Amd64::System
{
/// A user domain's fault supervisor, which takes one running thread's page fault at a time.
class IAmd64FaultSupervisor
{
public:
    /// Delivers the running thread's fault and blocks it until a resolution
    /// wakes it; a Terminate never returns.
    [[nodiscard]] virtual Kernel::Memory::FaultDelivery
    DeliverFault(Kernel::Scheduler::Thread& thread, Scheduler::Amd64KernelContext& context,
                 const Kernel::Memory::PagerFaultSource& source,
                 Scheduler::Amd64ProcessorFaultState& state) noexcept = 0;

    /// A deferred fault's scheduling point: wakes the supervisor once a
    /// delivered fault signalled it, so it can serve before the deferred
    /// thread runs again. False on a kernel invariant failure.
    [[nodiscard]] virtual bool Progress() noexcept = 0;

    /// A created thread of a domain this supervisor created (nonzero supervision tag) ended its run
    /// with ThreadExit: the supervisor's readiness is signalled, so a wait for faults also returns
    /// for the exit.
    virtual void OnExit() noexcept = 0;

protected:
    constexpr IAmd64FaultSupervisor() noexcept = default;
    ~IAmd64FaultSupervisor() = default;
};

// Per-record native supervisor over the named channel.
class Amd64ChannelSupervisor final : public IAmd64FaultSupervisor
{
public:
    void Bind(Scheduler::Amd64ProcessorRunLoop& loop, Kernel::Memory::FaultChannel& channel,
              IAmd64FaultSupervisor& progress) noexcept
    {
        m_loop = &loop;
        m_channel = &channel;
        m_progress = &progress;
    }
    [[nodiscard]] Kernel::Memory::FaultDelivery
    DeliverFault(Kernel::Scheduler::Thread& thread, Scheduler::Amd64KernelContext& context,
                 const Kernel::Memory::PagerFaultSource& source,
                 Scheduler::Amd64ProcessorFaultState& state) noexcept override;
    [[nodiscard]] bool Progress() noexcept override
    {
        m_channel->GetChannel().Retry();
        return m_progress->Progress();
    }
    void OnExit() noexcept override
    {
        static_cast<void>(m_channel->GetReadiness().Signal());
    }

private:
    Scheduler::Amd64ProcessorRunLoop* m_loop{nullptr};
    Kernel::Memory::FaultChannel* m_channel{nullptr};
    IAmd64FaultSupervisor* m_progress{nullptr};
};

/// What a created thread's system calls and faults reach in the domain it runs in: the boot runtime
/// registers a boot domain's, and the ProtectionDomain pool has the table record a created
/// domain's, whose supervision tag its fault deliveries carry. Every pointer outlives the record. A
/// null Supervisor leaves the domain's faults unsupervised.
struct Amd64UserDomainRecord
{
    Kernel::Protection::ProtectionDomain* Protection;
    Kernel::Memory::IUserMemoryAccess* UserMemory;
    Kernel::System::IObjectManagement* ObjectManagement;
    Kernel::Memory::IPagerResolution* PagerResolution;
    IAmd64FaultSupervisor* Supervisor;
    Foundation::UInt64 SupervisionTag;
};

/// The native slots one table holds, and its domain records: one boot
/// domain's (the pager's) and those of the domains user code creates.
inline constexpr Foundation::Size Amd64UserThreadSlots{Kernel::Scheduler::ThreadPool::Capacity};
inline constexpr Foundation::Size Amd64BootDomainRecords{1U};
inline constexpr Foundation::Size Amd64CreatedDomainRecords{Kernel::Protection::ProtectionDomainPool::Capacity};
inline constexpr Foundation::Size Amd64UserDomainRecords{Amd64BootDomainRecords + Amd64CreatedDomainRecords};

/// What progressing the created threads' waits did.
enum class Amd64ThreadWaitProgress : Foundation::UInt8
{
    /// At least one wait completed and its thread is Ready.
    Woken,
    /// No wait completed.
    Waiting,
    Failed,
};

/// The kernel stacks of the native slots, each mapped supervisor RW/NX in
/// every root with a guard page below it, and the CPU0 entry state a created
/// thread enters CPL3 through.
struct Amd64UserThreadTableInput
{
    Scheduler::WritableAmd64KernelStackView Stacks[Amd64UserThreadSlots];
    Interrupt::Amd64DescriptorTables* Tables;
    Interrupt::Amd64EntryContext* EntryContext;
    Scheduler::Amd64ProcessorRunLoop* Loops[Kernel::Scheduler::MaximumProcessorCount]{};
    Time::Amd64ApicTimer* Timers[Kernel::Scheduler::MaximumProcessorCount]{};
};

struct Amd64UserThreadTableSnapshot
{
    Foundation::Size FreeSlots;
    /// Threads started, ThreadExit calls that ended a run, native slots the
    /// reaper freed, and DebugWrite lines created threads wrote.
    Foundation::UInt64 Started;
    Foundation::UInt64 Exits;
    Foundation::UInt64 Reaped;
    Foundation::UInt64 Lines;
    /// Page faults and other exceptions of created threads, those deferred behind another, and the
    /// exceptions among them.
    Foundation::UInt64 Faults;
    Foundation::UInt64 Deferrals;
    Foundation::UInt64 Exceptions;
    /// Faults of threads in created domains (a nonzero supervision tag), the
    /// tag of the last, and their ThreadExits that woke the supervisor.
    Foundation::UInt64 TaggedFaults;
    Foundation::UInt64 LastTag;
    Foundation::UInt64 DomainExits;
    /// Created threads ThreadTerminate ended.
    Foundation::UInt64 Kills;
    /// WaitSetWait calls of created threads that blocked, and those of them their deadline
    /// completed.
    Foundation::UInt64 WaitsBlocked;
    Foundation::UInt64 WaitExpiries;
    /// Waits ThreadTerminate aborted, and the threads that then ended as they resumed.
    Foundation::UInt64 WaitAborts;
    Foundation::UInt64 AbortedExits;
};

/// The AMD64 side of thread creation: native slots, each a kernel context with its FXSAVE area, a
/// kernel stack and a launch frame, for threads that run in a registered user domain on CPU0. Start
/// claims a free slot for the thread the kernel's ThreadPool built, prepares its context to enter
/// the requested entry at CPL3 with the argument in RDI, binds it to the run loop with the caller's
/// ProtectionDomain and enqueues it Ready. The trap paths find a running created thread with
/// FindRunning and send its system calls and ticks here. Reap unbinds every exited created thread
/// that is not running and frees its slot; a thread is released once no slot holds it. Every
/// operation runs on CPU0 with interrupts disabled, outside an ISR.
class Amd64UserThreadTable final : public Kernel::Scheduler::IUserThreadBackend,
                                   public Kernel::Protection::ICreatedDomainRecords
{
public:
    /// Registry and wake are the BSP's, which every slot's wait dispatcher uses.
    Amd64UserThreadTable(Scheduler::Amd64ProcessorRunLoop& loop, Time::Amd64ApicTimer& timer,
                         Kernel::Object::ObjectRegistry& registry,
                         Kernel::Scheduler::ProcessorSchedulerWakeFacade& wake) noexcept
        : Amd64UserThreadTable{loop, timer, registry, wake, Foundation::MakeIndexSequence<Amd64UserThreadSlots>{}}
    {
    }

    Amd64UserThreadTable(const Amd64UserThreadTable&) = delete;
    Amd64UserThreadTable& operator=(const Amd64UserThreadTable&) = delete;

private:
    // Every slot's wait dispatcher and its system-call dispatcher, one per
    // index of the slot array.
    template <Foundation::Size... Slots>
    Amd64UserThreadTable(Scheduler::Amd64ProcessorRunLoop& loop, Time::Amd64ApicTimer& timer,
                         Kernel::Object::ObjectRegistry& registry,
                         Kernel::Scheduler::ProcessorSchedulerWakeFacade& wake,
                         Foundation::IndexSequence<Slots...>) noexcept
        : m_loop{loop}, m_timer{timer}, m_waits{{(static_cast<void>(Slots), registry), wake}...},
          m_waitCalls{{registry, m_waits[Slots]}...}
    {
    }

public:
    /// Gives the table its stacks and entry state; once only. Until then
    /// Start is Exhausted.
    [[nodiscard]] bool Configure(const Amd64UserThreadTableInput& input) noexcept;
    [[nodiscard]] bool IsConfigured() const noexcept
    {
        return __atomic_load_n(&m_configured, __ATOMIC_ACQUIRE);
    }

    /// Lets created threads' WaitSetWait take a timeout in TSC ticks of the table's timer,
    /// ticksPerMicrosecond (nonzero) of them per microsecond; once only.
    [[nodiscard]] bool ConfigureWaits(Foundation::UInt64 ticksPerMicrosecond) noexcept;

    /// Wakes each blocked created thread whose wait has a signalled member or whose deadline
    /// passed; called wherever the pager's wait is progressed.
    [[nodiscard]] Amd64ThreadWaitProgress ProgressWaits() noexcept;

    /// The earliest deadline of a blocked created thread's wait, or zero.
    [[nodiscard]] Foundation::UInt64 GetWaitDeadline() const noexcept;

    /// Admits threads created in record's domain; Start finds the caller's
    /// record by the run loop's active ProtectionDomain.
    [[nodiscard]] bool RegisterDomain(const Amd64UserDomainRecord& record) noexcept;

    /// The supervisor and object management of every domain user code creates: the root service's;
    /// once only. Until then RecordDomain refuses.
    [[nodiscard]] bool ConfigureCreatedDomains(IAmd64FaultSupervisor& supervisor,
                                               Kernel::System::IObjectManagement& objects) noexcept;

    /// Records a created domain with its own record-backed user memory and
    /// the configured supervisor; false with no free record.
    [[nodiscard]] bool RecordDomain(const Kernel::Protection::CreatedDomain& domain) noexcept override;

    /// Forgets domain's record once no slot holds a thread of it.
    [[nodiscard]] bool RetireDomain(const Kernel::Protection::ProtectionDomain& domain) noexcept override;

    [[nodiscard]] Foundation::UInt64 GetNativeSlotBytes() const noexcept override;
    [[nodiscard]] Kernel::Scheduler::UserThreadSchedule GetSchedule() const noexcept override;
    [[nodiscard]] Kernel::Scheduler::UserThreadBackendError
    Reserve(Kernel::Scheduler::Thread& thread, Kernel::Scheduler::ThreadId threadIdentity,
            Kernel::Scheduler::SchedulingContext& context, Kernel::Scheduler::SchedulingContextId contextIdentity,
            const Kernel::Scheduler::UserThreadStart& start) noexcept override;
    [[nodiscard]] Kernel::Scheduler::UserThreadBackendError
    Start(Kernel::Scheduler::Thread& thread, Kernel::Scheduler::ThreadId threadIdentity,
          Kernel::Scheduler::SchedulingContext& context, Kernel::Scheduler::SchedulingContextId contextIdentity,
          const Kernel::Scheduler::UserThreadStart& start) noexcept override;
    [[nodiscard]] bool IsReleased(const Kernel::Scheduler::Thread& thread) const noexcept override;

    /// Takes a Ready created thread out of the ready queue and ends it with exitCode; its context
    /// stays where the thread stopped, never resumed, until the reaper frees the slot. Blocked in
    /// WaitSetWait, its wait is aborted: it is woken and ends with exitCode as soon as it runs,
    /// without returning to user mode. Blocked on a fault, or held by the run loop where it cannot
    /// be removed now, it is Blocked; the running caller is Running.
    [[nodiscard]] Kernel::Scheduler::UserThreadTerminateError Terminate(Kernel::Scheduler::Thread& thread,
                                                                        Foundation::UInt64 exitCode) noexcept override;

    /// The slot of the run loop's active thread, or Amd64UserThreadSlots when
    /// no created thread runs.
    [[nodiscard]] Foundation::Size FindRunning() const noexcept;

    /// Whether a slot holds thread.
    [[nodiscard]] bool Holds(const Kernel::Scheduler::Thread* thread) const noexcept;

    /// The running created thread's system call: ThreadExit ends its run Exited and never returns;
    /// WaitSetWait waits in the slot's own dispatcher, blocking the thread until ProgressWaits
    /// wakes it; anything else goes through the common ingress with its domain's user memory, pager
    /// resolution and object management. False only on a kernel invariant failure.
    [[nodiscard]] bool DispatchSystemCall(Foundation::Size slot, const Kernel::System::SystemCallRequest& request,
                                          Kernel::System::SystemCallResponse& response,
                                          Kernel::System::SystemCallScratch& scratch) noexcept;

    /// Ends the running created thread's run Ready at a fired run stop and
    /// dispatches the next thread; returns once it runs again.
    [[nodiscard]] bool Preempt(Foundation::Size slot) noexcept;

    /// The running created thread's page fault or other exception, through its domain's supervisor:
    /// returns once the thread may run the faulting instruction again, resolved, or deferred behind
    /// another fault after it yielded, however often (Kernel::Memory::DecideFaultOutcome); a
    /// Terminate never returns. False without a supervisor or on a kernel invariant failure.
    [[nodiscard, gnu::noinline]] bool BlockOnFault(Foundation::Size slot,
                                                   const Kernel::Memory::FaultRecord& fault) noexcept;

    /// Unbinds every exited created thread other than the running one and
    /// frees its slot; false on a run-loop refusal, a kernel invariant failure.
    [[nodiscard]] bool Reap() noexcept;

    // Waits protect their current continuation.
    void ServiceRequests(bool schedulingPoint) noexcept;

    [[nodiscard]] Amd64UserThreadTableSnapshot GetSnapshot() const noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Pending,
        Bound,
    };

    struct Slot
    {
        alignas(Scheduler::Amd64KernelContext) Foundation::UInt8 ContextStorage[sizeof(Scheduler::Amd64KernelContext)];
        Scheduler::Amd64ExtendedStateArea ExtendedState;
        Scheduler::Amd64KernelContext* Context;
        Scheduler::WritableAmd64KernelStackView Stack;
        Interrupt::Amd64TrapFrame LaunchFrame;
        Kernel::Scheduler::UserThreadStart Start;
        Kernel::Scheduler::Thread* Thread;
        Kernel::Scheduler::ThreadId ThreadIdentity;
        Kernel::Scheduler::SchedulingContextId ContextIdentity;
        /// Consecutive deferrals of the thread's current fault.
        Foundation::UInt64 Deferrals;
        const Amd64UserDomainRecord* Domain;
        Amd64UserThreadTable* Owner;
        Kernel::System::SystemCallIngressFailure IngressFailure;
        Scheduler::Amd64ProcessorFaultState FaultState{};
        Kernel::Scheduler::ProcessorKernelRunLoopReceipt Receipt;
        Kernel::Scheduler::ProcessorKernelRunLoopFailure Failure;
        /// The exit code of a thread whose wait ThreadTerminate aborted.
        Foundation::UInt64 AbortExitCode;
        // Fixed home, and a write reservation per thread.
        Kernel::Scheduler::ProcessorBindingReservation BindingReservation{};
        Kernel::Scheduler::ThreadRequest Request{};
        Kernel::Scheduler::SchedulingContext* Scheduling{};
        Kernel::Scheduler::CpuId Home{0U};
        bool BalancedPlacement{false};
        Kernel::Memory::RecordUserMemoryAccess Access{};
        SlotState State;
    };

    [[nodiscard]] bool Owns(const Slot& slot) const noexcept;
    [[nodiscard]] Scheduler::Amd64ProcessorRunLoop& HomeLoop(const Slot& slot) const noexcept
    {
        return *m_homeLoops[slot.Home.GetValue()];
    }
    [[nodiscard]] Time::Amd64ApicTimer& LocalTimer() const noexcept;
    [[nodiscard]] bool Place(Slot& slot, Kernel::Scheduler::SchedulingContext& context) noexcept;
    [[nodiscard, gnu::noinline]] Kernel::Scheduler::UserThreadBackendError
    Prepare(Slot& slot, Kernel::Scheduler::SchedulingContext& context) noexcept;
    [[nodiscard]] Kernel::Scheduler::UserThreadTerminateError TerminateLocal(Kernel::Scheduler::Thread& thread,
                                                                             Foundation::UInt64 exitCode) noexcept;

    [[nodiscard]] const Amd64UserDomainRecord* FindCallerDomain() const noexcept;

    /// The record of domain, a ProtectionDomain the caller named; null for an
    /// unregistered one.
    [[nodiscard]] const Amd64UserDomainRecord*
    FindDomain(const Kernel::Protection::ProtectionDomain& domain) const noexcept;
    /// Prepares slot's fresh context to launch start and binds and enqueues
    /// thread; every failure undoes the earlier steps.
    [[nodiscard, gnu::noinline]] Kernel::Scheduler::UserThreadBackendError
    Admit(Slot& slot, Kernel::Scheduler::Thread& thread, Kernel::Scheduler::ThreadId threadIdentity,
          Kernel::Scheduler::SchedulingContext& context,
          Kernel::Scheduler::SchedulingContextId contextIdentity) noexcept;
    /// Ends the slot's context and forgets its thread.
    static void Clear(Slot& slot) noexcept;
    [[nodiscard, gnu::noinline]] bool Exit(Slot& slot, const Kernel::System::SystemCallRequest& request,
                                           Kernel::System::SystemCallResponse& response) noexcept;
    /// Ends the running thread's run Exited with exitCode, waking its
    /// created domain's supervisor; returns only if the run loop refused.
    [[nodiscard]] bool End(Slot& slot, Foundation::UInt64 exitCode) noexcept;
    /// Aborts the wait of slot index's thread, blocked in WaitSetWait, for
    /// exitCode.
    [[nodiscard]] Kernel::Scheduler::UserThreadTerminateError AbortWait(Foundation::Size index,
                                                                        Foundation::UInt64 exitCode) noexcept;
    /// Progresses slot index's wait if it is blocked: readiness, else its
    /// deadline.
    [[nodiscard, gnu::noinline]] Amd64ThreadWaitProgress ProgressWait(Foundation::Size index) noexcept;
    [[nodiscard, gnu::noinline]] Amd64ThreadWaitProgress ExpireWait(Foundation::Size index) noexcept;
    /// The state of slot index's wait, out of line: its snapshot is large.
    [[nodiscard, gnu::noinline]] Kernel::Ipc::WaitSetThreadDispatcherState
    GetWaitState(Foundation::Size index) const noexcept;
    /// WaitSetWait of slot index's running thread.
    [[nodiscard, gnu::noinline]] bool DispatchWait(Foundation::Size index,
                                                   const Kernel::System::SystemCallRequest& request,
                                                   Kernel::System::SystemCallResponse& response) noexcept;
    [[noreturn]] static void Launch(void* argument) noexcept;

    [[nodiscard]] Scheduler::Amd64ProcessorRunLoop& LocalLoop() const noexcept
    {
        return Scheduler::CurrentAmd64ProcessorRunLoop(m_loop);
    }
    Scheduler::Amd64ProcessorRunLoop* m_homeLoops[Kernel::Scheduler::MaximumProcessorCount]{};
    Time::Amd64ApicTimer* m_homeTimers[Kernel::Scheduler::MaximumProcessorCount]{};
    Kernel::Scheduler::CpuId m_lastPlacement{0U};
    Scheduler::Amd64ProcessorRunLoop& m_loop;
    Time::Amd64ApicTimer& m_timer;
    /// Each slot's wait, kept across the thread's block like the pager's.
    Kernel::Ipc::WaitSetThreadDispatcher m_waits[Amd64UserThreadSlots];
    Kernel::System::WaitSetSystemCallDispatcher m_waitCalls[Amd64UserThreadSlots];
    Kernel::System::WaitSetSystemCallDispatchResult m_waitResults[Amd64UserThreadSlots]{};
    Kernel::System::WaitSetSystemCallIngressFailure m_waitFailures[Amd64UserThreadSlots]{};
    Interrupt::Amd64DescriptorTables* m_tables{nullptr};
    Interrupt::Amd64EntryContext* m_entryContext{nullptr};
    Slot m_slots[Amd64UserThreadSlots]{};
    /// Boot records first, then created ones; a record with no Protection is
    /// free. Created record k reads its domain's memory through accessor k.
    Amd64UserDomainRecord m_domains[Amd64UserDomainRecords]{};
    Kernel::Memory::RecordUserMemoryAccess m_accessors[Amd64CreatedDomainRecords]{};
    Amd64ChannelSupervisor m_channelSupervisors[Kernel::Protection::ProtectionDomainPool::Capacity]{};
    IAmd64FaultSupervisor* m_createdSupervisor{nullptr};
    Kernel::System::IObjectManagement* m_createdObjects{nullptr};
    Foundation::UInt64 m_started{0U};
    Foundation::UInt64 m_exits{0U};
    Foundation::UInt64 m_reaped{0U};
    Foundation::UInt64 m_lines{0U};
    Foundation::UInt64 m_faults{0U};
    Foundation::UInt64 m_deferrals{0U};
    Foundation::UInt64 m_exceptions{0U};
    Foundation::UInt64 m_taggedFaults{0U};
    Foundation::UInt64 m_lastTag{0U};
    Foundation::UInt64 m_domainExits{0U};
    Foundation::UInt64 m_kills{0U};
    Foundation::UInt64 m_waitsBlocked{0U};
    Foundation::UInt64 m_waitExpiries{0U};
    Foundation::UInt64 m_waitAborts{0U};
    Foundation::UInt64 m_abortedExits{0U};
    bool m_waitsConfigured{false};
    bool m_configured{false};
};
} // Namespace Asteria::Platform::Amd64::System

#endif // ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_USER_THREAD_TABLE_HPP_INCLUDED
