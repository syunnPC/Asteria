// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SYSTEM_IA64_USER_THREAD_TABLE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SYSTEM_IA64_USER_THREAD_TABLE_HPP_INCLUDED

#include "Foundation/IndexSequence.hpp"
#include "Hal/Time/IMonotonicClock.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Memory/FaultDeferral.hpp"
#include "Kernel/Memory/IPagerResolution.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Memory/PagerChannel.hpp"
#include "Kernel/Memory/RecordUserMemoryAccess.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Protection/ProtectionDomainPool.hpp"
#include "Kernel/Scheduler/IUserThreadBackend.hpp"
#include "Kernel/Scheduler/ThreadRequest.hpp"
#include "Kernel/System/IObjectManagement.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/System/WaitSetSystemCallDispatcher.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Kernel/Time/IMonotonicClock.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"
#include "Platform/Ia64/Scheduler/Ia64ProcessorRunLoop.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"
#include "Platform/Ia64/System/Ia64UserPreemption.hpp"

namespace Asteria::Platform::Ia64::System
{
/// A user domain's fault supervisor, which takes one running thread's page fault at a time.
class IIa64FaultSupervisor
{
public:
    /// Delivers the running thread's fault and blocks it until a resolution
    /// wakes it; a Terminate never returns. The caller selects the thread's
    /// entry contexts again once it runs.
    [[nodiscard]] virtual Kernel::Memory::FaultDelivery
    DeliverFault(Kernel::Scheduler::Thread& thread, Scheduler::Ia64KernelContext& context,
                 const Kernel::Memory::PagerFaultSource& source) noexcept = 0;

    /// A deferred fault's scheduling point: wakes the supervisor once a
    /// delivered fault signalled it. False on a kernel invariant failure.
    [[nodiscard]] virtual bool Progress() noexcept = 0;

    /// A created thread of a domain this supervisor created (nonzero supervision tag) ended its run
    /// with ThreadExit: the supervisor's readiness is signalled, so a wait for faults also returns
    /// for the exit. A kill wakes no one.
    virtual void OnExit() noexcept = 0;

protected:
    constexpr IIa64FaultSupervisor() noexcept = default;
    ~IIa64FaultSupervisor() noexcept = default;
};

// Per-record native supervisor over the named channel.
class Ia64ChannelSupervisor final : public IIa64FaultSupervisor
{
public:
    void Bind(Scheduler::Ia64ProcessorRunLoop& loop, Kernel::Memory::FaultChannel& channel,
              IIa64FaultSupervisor& progress) noexcept
    {
        m_loop = &loop;
        m_channel = &channel;
        m_progress = &progress;
    }
    [[nodiscard]] Kernel::Memory::FaultDelivery
    DeliverFault(Kernel::Scheduler::Thread& thread, Scheduler::Ia64KernelContext& context,
                 const Kernel::Memory::PagerFaultSource& source) noexcept override;
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
    Scheduler::Ia64ProcessorRunLoop* m_loop{nullptr};
    Kernel::Memory::FaultChannel* m_channel{nullptr};
    IIa64FaultSupervisor* m_progress{nullptr};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_receipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_failure{};
};

/// What a created thread's system calls and faults reach in the domain it runs in: the pager's,
/// which the BSP runtime configures, or one the ProtectionDomain pool has the table record for a
/// domain user code created, whose supervision tag its fault deliveries carry. Every pointer
/// outlives the record.
struct Ia64UserDomainRecord
{
    Kernel::Protection::ProtectionDomain* Protection;
    Kernel::Memory::IUserMemoryAccess* UserMemory;
    Kernel::System::IObjectManagement* ObjectManagement;
    Kernel::Memory::IPagerResolution* PagerResolution;
    IIa64FaultSupervisor* Supervisor;
    Foundation::UInt64 SupervisionTag;
};

/// The domains the table records: the pager's and those user code creates
/// (one per ProtectionDomain pool slot).
inline constexpr Foundation::Size Ia64CreatedDomainRecords{Kernel::Protection::ProtectionDomainPool::Capacity};

/// What the BSP runtime lends the table.
struct Ia64UserThreadTableCollaborators
{
    Scheduler::Ia64ProcessorRunLoop* RunLoop;
    Memory::Ia64BspRuntimeMemory* RuntimeMemory;
    Kernel::Time::IMonotonicClock* Clock;
    Kernel::System::SystemCallScratch* Scratch;
    IIa64UserPreemption* Preemption;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 ImplementedVirtualAddressMostSignificantBit;
    /// The BSP's registry and wake authority, which every slot's wait dispatcher uses, and the ITC
    /// its timed waits expire by.
    Kernel::Object::ObjectRegistry* Registry;
    Kernel::Scheduler::ProcessorSchedulerWakeFacade* Wake;
    const Hal::Time::IMonotonicClock* WaitClock;
};

/// How a wake pass over the created threads' waits went.
enum class Ia64ThreadWaitProgress : Foundation::UInt8
{
    /// Nothing changed.
    Waiting,
    /// A blocked thread's wait completed or expired: it is Ready.
    Woken,
    Failed,
};

struct Ia64UserThreadTableSnapshot
{
    Foundation::Size FreeSlots;
    /// Threads started, ThreadExit calls that ended a run, slots the reaper
    /// freed, page faults of created threads and those deferred behind
    /// another.
    Foundation::UInt64 Started;
    Foundation::UInt64 Exits;
    Foundation::UInt64 Reaped;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Deferrals;
    /// The most consecutive deferrals one fault of a created thread had: a busy supervisor defers a
    /// fault without bound.
    Foundation::UInt64 LongestDeferral;
    /// Created threads ThreadTerminate ended.
    Foundation::UInt64 Kills;
    /// The faults that were PL3 exceptions.
    Foundation::UInt64 Exceptions;
    /// Created domains recorded and retired, and the ThreadExit calls of their threads that woke
    /// the supervisor.
    Foundation::UInt64 DomainsRecorded;
    Foundation::UInt64 DomainsRetired;
    Foundation::UInt64 DomainExits;
    /// The faults of created domains' threads (a nonzero tag) and the last
    /// one's tag.
    Foundation::UInt64 TaggedFaults;
    Foundation::UInt64 LastTag;
    /// WaitSetWait calls of created threads that blocked, those of them whose deadline passed, the
    /// waits ThreadTerminate aborted, and the threads that then ended as they resumed.
    Foundation::UInt64 WaitsBlocked;
    Foundation::UInt64 WaitExpiries;
    Foundation::UInt64 WaitAborts;
    Foundation::UInt64 AbortedExits;
};

/// The IA-64 side of thread creation: created-thread slots whose entry storage is the BSP runtime
/// memory's thread entry slots, each with a kernel context rebuilt for every start on that storage,
/// its f32-f127 area, a launch request and a system-call bridge. Every
/// slot's system-call and user-fault entry contexts are admitted once, at Configure. Start reads
/// the entry's function descriptor {ip, gp} from the caller's memory, through the caller's record
/// (the active root is the caller's), and derives the launch: the code range
/// is the page holding ip; the user stack top names a 32 KiB region whose lower 16 KiB is the RSE
/// backing store and upper 16 KiB the memory stack; the argument goes to r4. The thread runs in the
/// start's domain (the caller's by default): its slot's bridge takes that domain's user memory,
/// object management and pager resolution for the run. Every operation runs on the BSP with
/// external interrupts disabled, outside an ISR. The object is immovable and lives for the boot.
class Ia64UserThreadTable final : public Kernel::Scheduler::IUserThreadBackend,
                                  public Kernel::Protection::ICreatedDomainRecords
{
public:
    static constexpr Foundation::Size Slots{Memory::Ia64BspRuntimeThreadEntrySlots};
    /// The user region below the stack top: the RSE backing store, then the
    /// memory stack.
    static constexpr Foundation::UInt64 UserRseBytes{16U * 1024U};
    static constexpr Foundation::UInt64 UserStackBytes{16U * 1024U};

    explicit Ia64UserThreadTable(const Ia64UserThreadTableCollaborators& collaborators) noexcept;

    Ia64UserThreadTable(const Ia64UserThreadTable&) = delete;
    Ia64UserThreadTable& operator=(const Ia64UserThreadTable&) = delete;

private:
    // Every slot's wait dispatcher and its system-call dispatcher, one per
    // index of the slot array.
    template <Foundation::Size... Indices>
    Ia64UserThreadTable(const Ia64UserThreadTableCollaborators& collaborators,
                        Foundation::IndexSequence<Indices...>) noexcept
        : m_collaborators{collaborators},
          m_waits{{(static_cast<void>(Indices), *collaborators.Registry), *collaborators.Wake}...},
          m_waitCalls{{*collaborators.Registry, m_waits[Indices]}...}
    {
        for (Kernel::System::WaitSetSystemCallDispatchResult& result : m_waitResults)
        {
            result = Kernel::System::WaitSetSystemCallDispatchResult{};
        }
    }

public:
    /// Registers the one domain created threads run in and admits every
    /// slot's entry contexts; once, before any thread enters PL3.
    [[nodiscard]] bool Configure(const Ia64UserDomainRecord& domain) noexcept;

    /// The supervisor and object management of every domain user code creates: the root service's;
    /// once, after Configure. Until then RecordDomain refuses.
    [[nodiscard]] bool ConfigureCreatedDomains(IIa64FaultSupervisor& supervisor,
                                               Kernel::System::IObjectManagement& objects) noexcept;

    /// Records a created domain with its own record-backed user memory and
    /// the configured supervisor; false with no free record.
    [[nodiscard]] bool RecordDomain(const Kernel::Protection::CreatedDomain& domain) noexcept override;

    /// Forgets domain's record once no slot holds a thread of it (a killed
    /// thread still does until the reaper frees its slot).
    [[nodiscard]] bool RetireDomain(const Kernel::Protection::ProtectionDomain& domain) noexcept override;

    [[nodiscard]] Foundation::UInt64 GetNativeSlotBytes() const noexcept override;

    /// The kernel-metadata bytes of one slot: its kernel context and
    /// buffers and its share of the runtime memory's entry storage.
    [[nodiscard]] static Foundation::UInt64 ComputeNativeSlotBytes() noexcept;
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

    /// Lets created threads' WaitSetWait take a timeout in ITC ticks, ticksPerMicrosecond (nonzero)
    /// of them per microsecond; once only.
    [[nodiscard]] bool ConfigureWaits(Foundation::UInt64 ticksPerMicrosecond) noexcept;

    /// Wakes each blocked created thread whose wait has a signalled member or whose deadline
    /// passed; called wherever the pager's wait is progressed.
    [[nodiscard]] Ia64ThreadWaitProgress ProgressWaits() noexcept;

    /// The earliest ITC deadline of a blocked created thread's wait, or zero.
    [[nodiscard]] Foundation::UInt64 GetWaitDeadline() const noexcept;

    /// Takes a Ready created thread out of the BSP's ready queue and ends it with exitCode; its
    /// context stays where it stopped, never resumed, until the reaper resets its entry frames and
    /// frees the slot. Blocked in WaitSetWait, its wait is aborted: it is woken and ends with
    /// exitCode as soon as it runs, without returning to user mode. Blocked on a fault, or held by
    /// the run loop where it cannot be removed now, it is Blocked; the running caller is Running.
    [[nodiscard]] Kernel::Scheduler::UserThreadTerminateError Terminate(Kernel::Scheduler::Thread& thread,
                                                                        Foundation::UInt64 exitCode) noexcept override;

    /// Unbinds every exited created thread other than the running one,
    /// returns its slot's frames to Inactive and frees the slot; false on a
    /// refusal, a kernel invariant failure.
    [[nodiscard]] bool Reap() noexcept;

    // Waits protect their current continuation.
    void ServiceRequests(bool schedulingPoint) noexcept;
    void ServiceWaitingRequests() noexcept;

    /// Ends slot's running thread's run Ready and dispatches the next
    /// thread; returns once it runs again. The caller selects its entry
    /// contexts.
    [[nodiscard]] bool Preempt(Foundation::Size slot) noexcept;

    [[nodiscard]] Ia64UserThreadTableSnapshot GetSnapshot() const noexcept;

    /// The kernel context entry of every slot's thread: launches it at PL3.
    /// Returns only if the launch was rejected.
    static void LaunchSlot(void* slot) noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Pending,
        Bound,
    };

    /// One slot: its system calls and its faults and ticks reach it.
    class Slot final : public IIa64SystemCallThread, public Interrupt::IIa64UserFaultHandler
    {
    public:
        constexpr Slot() noexcept = default;

        [[nodiscard]] bool DispatchWait(const Kernel::System::SystemCallRequest& request,
                                        Kernel::System::SystemCallResponse& response) noexcept override;
        [[nodiscard]] bool DispatchExit(const Kernel::System::SystemCallRequest& request,
                                        Kernel::System::SystemCallResponse& response) noexcept override;
        [[nodiscard]] bool PrepareManagement() noexcept override;
        [[nodiscard]] Interrupt::Ia64UserFaultDisposition
        HandleUserFault(Interrupt::Ia64PageFaultFrame& frame,
                        const Kernel::Memory::FaultRecord& fault) noexcept override;
        [[nodiscard]] Interrupt::Ia64UserFaultDisposition
        HandleUserInterrupt(Interrupt::Ia64PageFaultFrame& frame) noexcept override;

        Ia64UserThreadTable* Owner{nullptr};
        Foundation::Size Index{0U};
        alignas(Scheduler::Ia64KernelContext) Foundation::UInt8 ContextStorage[sizeof(Scheduler::Ia64KernelContext)]{};
        Scheduler::Ia64KernelContext* Context{nullptr};
        Scheduler::Ia64HighFloatingArea HighFloating{};
        Ia64SystemCallBridgeContext Bridge{};
        Scheduler::Ia64UserModeSupervisorContinuation Supervisor{};
        Scheduler::Ia64UserModeLaunchRequest LaunchRequest{};
        Foundation::UInt64 EntryDescriptor[2U]{};
        /// The domain the thread runs in.
        const Ia64UserDomainRecord* Domain{nullptr};
        Kernel::Scheduler::Thread* Thread{nullptr};
        Kernel::Scheduler::ThreadId ThreadIdentity{};
        Kernel::Scheduler::SchedulingContextId ContextIdentity{};
        /// Consecutive deferrals of the thread's current fault.
        Foundation::UInt64 Deferrals{0U};
        /// The exit code of a thread whose wait ThreadTerminate aborted.
        Foundation::UInt64 AbortExitCode{0U};
        Kernel::Scheduler::ProcessorKernelRunLoopReceipt Receipt{};
        Kernel::Scheduler::ProcessorKernelRunLoopFailure Failure{};
        // Fixed home, and a write reservation per thread.
        Kernel::Scheduler::ProcessorBindingReservation BindingReservation{};
        Kernel::Scheduler::ThreadRequest Request{};
        Kernel::Scheduler::SchedulingContext* Scheduling{};
        Kernel::Scheduler::CpuId Home{0U};
        Kernel::Memory::RecordUserMemoryAccess Access{};
        SlotState State{SlotState::Free};
    };

    [[nodiscard]] Memory::Ia64BspRuntimeEntrySlot EntrySlot(const Slot& slot) const noexcept
    {
        return Memory::Ia64BspRuntimeThreadEntrySlot(slot.Index);
    }
    /// Derives slot's launch request from start, reading the entry
    /// descriptor through caller's user memory; false when the start is not
    /// one the launch admits.
    [[nodiscard, gnu::noinline]] bool PrepareLaunch(Slot& slot, const Ia64UserDomainRecord& caller,
                                                    const Kernel::Scheduler::UserThreadStart& start) noexcept;
    /// The record of the running caller's domain, or of domain; null if none.
    [[nodiscard, gnu::noinline]] Kernel::Scheduler::UserThreadBackendError
    Prepare(Slot& slot, Kernel::Scheduler::SchedulingContext& context) noexcept;
    template <bool SchedulingPoint> void ServiceOwnedRequests() noexcept;
    [[nodiscard]] Kernel::Scheduler::UserThreadTerminateError TerminateLocal(Kernel::Scheduler::Thread& thread,
                                                                             Foundation::UInt64 exitCode) noexcept;

    [[nodiscard]] const Ia64UserDomainRecord* FindCallerDomain() const noexcept;
    [[nodiscard]] const Ia64UserDomainRecord*
    FindDomain(const Kernel::Protection::ProtectionDomain& domain) const noexcept;
    /// Builds slot's fresh kernel context on its entry storage and binds and
    /// enqueues thread; every failure undoes the earlier steps.
    [[nodiscard, gnu::noinline]] Kernel::Scheduler::UserThreadBackendError
    Admit(Slot& slot, Kernel::Scheduler::Thread& thread, Kernel::Scheduler::ThreadId threadIdentity,
          Kernel::Scheduler::SchedulingContext& context,
          Kernel::Scheduler::SchedulingContextId contextIdentity) noexcept;
    /// Ends the slot's context and forgets its thread.
    static void Clear(Slot& slot) noexcept;
    [[nodiscard]] bool SelectEntryContexts(const Slot& slot) noexcept;
    [[nodiscard, gnu::noinline]] bool Exit(Slot& slot, const Kernel::System::SystemCallRequest& request,
                                           Kernel::System::SystemCallResponse& response) noexcept;
    /// Ends slot's running thread's run Exited with exitCode, waking its
    /// created domain's supervisor; returns only if the run loop refused.
    [[nodiscard]] bool End(Slot& slot, Foundation::UInt64 exitCode) noexcept;
    /// WaitSetWait of slot's running thread, in the slot's own dispatcher:
    /// blocks the thread until ProgressWaits wakes it.
    [[nodiscard, gnu::noinline]] bool DispatchWait(Slot& slot, const Kernel::System::SystemCallRequest& request,
                                                   Kernel::System::SystemCallResponse& response) noexcept;
    /// Aborts the wait of slot index's thread, blocked in WaitSetWait, for
    /// exitCode.
    [[nodiscard, gnu::noinline]] Kernel::Scheduler::UserThreadTerminateError
    AbortWait(Foundation::Size index, Foundation::UInt64 exitCode) noexcept;
    /// Progresses slot index's wait if it is blocked: readiness, else its
    /// deadline.
    [[nodiscard, gnu::noinline]] Ia64ThreadWaitProgress ProgressWait(Foundation::Size index) noexcept;
    [[nodiscard, gnu::noinline]] Ia64ThreadWaitProgress ExpireWait(Foundation::Size index) noexcept;
    /// The state of slot index's wait, out of line: its snapshot is large.
    [[nodiscard, gnu::noinline]] Kernel::Ipc::WaitSetThreadDispatcherState
    GetWaitState(Foundation::Size index) const noexcept;
    [[nodiscard, gnu::noinline]] bool BlockOnFault(Slot& slot, const Kernel::Memory::FaultRecord& fault) noexcept;
    void Launch(Slot& slot) noexcept;

    Ia64UserThreadTableCollaborators m_collaborators;
    /// Each slot's wait, kept across the thread's block like the pager's.
    Kernel::Ipc::WaitSetThreadDispatcher m_waits[Slots];
    Kernel::System::WaitSetSystemCallDispatcher m_waitCalls[Slots];
    // Cleared in the constructor's body, one result at a time: a value
    // initializer here builds the whole array on the constructor's stack.
    Kernel::System::WaitSetSystemCallDispatchResult m_waitResults[Slots];
    Kernel::System::WaitSetSystemCallIngressFailure m_waitFailures[Slots]{};
    /// The pager's record, then the created domains'.
    Ia64UserDomainRecord m_domains[1U + Ia64CreatedDomainRecords]{};
    Kernel::Memory::RecordUserMemoryAccess m_accessors[Ia64CreatedDomainRecords]{};
    Ia64ChannelSupervisor m_channelSupervisors[Kernel::Protection::ProtectionDomainPool::Capacity]{};
    IIa64FaultSupervisor* m_createdSupervisor{nullptr};
    Kernel::System::IObjectManagement* m_createdObjects{nullptr};
    Slot m_slots[Slots]{};
    Foundation::UInt64 m_started{0U};
    Foundation::UInt64 m_exits{0U};
    Foundation::UInt64 m_reaped{0U};
    Foundation::UInt64 m_kills{0U};
    Foundation::UInt64 m_exceptions{0U};
    Foundation::UInt64 m_faults{0U};
    Foundation::UInt64 m_deferrals{0U};
    Foundation::UInt64 m_longestDeferral{0U};
    Foundation::UInt64 m_domainsRecorded{0U};
    Foundation::UInt64 m_domainsRetired{0U};
    Foundation::UInt64 m_domainExits{0U};
    Foundation::UInt64 m_taggedFaults{0U};
    Foundation::UInt64 m_lastTag{0U};
    Foundation::UInt64 m_waitsBlocked{0U};
    Foundation::UInt64 m_waitExpiries{0U};
    Foundation::UInt64 m_waitAborts{0U};
    Foundation::UInt64 m_abortedExits{0U};
    bool m_waitsConfigured{false};
    bool m_configured{false};
};
} // Namespace Asteria::Platform::Ia64::System

#endif // ASTERIA_PLATFORM_IA64_SYSTEM_IA64_USER_THREAD_TABLE_HPP_INCLUDED
