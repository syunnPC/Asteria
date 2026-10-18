// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/System/Amd64UserThreadTable.hpp"

#include "Platform/Amd64/System/Amd64UserScheduling.hpp"
#include "Kernel/Scheduler/ProcessorPlacement.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"

#include "Platform/Amd64/Memory/UserAddressPolicy.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Platform::Amd64::System
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Scheduler::UserThreadBackendError;

namespace
{
[[noreturn]] void FailLaunch() noexcept
{
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::BootStage,
                           static_cast<UInt64>(Diagnostics::Amd64BootStage::CreatedThreadStack));
}
} // Namespace

bool Amd64UserThreadTable::Configure(const Amd64UserThreadTableInput& input) noexcept
{
    if (m_configured || (input.Tables == nullptr) || (input.EntryContext == nullptr))
    {
        return false;
    }
    for (Size index{0U}; index < Amd64UserThreadSlots; ++index)
    {
        const Scheduler::WritableAmd64KernelStackView& stack = input.Stacks[index];
        if ((stack.Data == nullptr) || (stack.SizeBytes != input.Stacks[0U].SizeBytes))
        {
            return false;
        }
        m_slots[index].Stack = stack;
        m_slots[index].Owner = this;
        __atomic_store_n(&m_slots[index].State, SlotState::Free, __ATOMIC_RELEASE);
    }
    for (Size cpu{0U}; cpu < Kernel::Scheduler::MaximumProcessorCount; ++cpu)
    {
        m_homeLoops[cpu] = input.Loops[cpu];
        m_homeTimers[cpu] = input.Timers[cpu];
    }
    if (m_homeLoops[0U] == nullptr)
    {
        m_homeLoops[0U] = &m_loop;
    }
    if (m_homeTimers[0U] == nullptr)
    {
        m_homeTimers[0U] = &m_timer;
    }
    m_tables = input.Tables;
    m_entryContext = input.EntryContext;
#if defined(ASTERIA_TARGET_AMD64)
    Platform::Synchronization::BindThreadRequests(this,
                                                  [](void* owner, const bool point) noexcept
                                                  {
                                                      static_cast<Amd64UserThreadTable*>(owner)->ServiceRequests(point);
                                                  });
#endif
    __atomic_store_n(&m_configured, true, __ATOMIC_RELEASE);
    return true;
}

bool Amd64UserThreadTable::RegisterDomain(const Amd64UserDomainRecord& record) noexcept
{
    if ((record.Protection == nullptr) || (record.UserMemory == nullptr))
    {
        return false;
    }
    for (Size index{0U}; index < Amd64BootDomainRecords; ++index)
    {
        if (__atomic_load_n(&m_domains[index].Protection, __ATOMIC_ACQUIRE) == nullptr)
        {
            m_domains[index].UserMemory = record.UserMemory;
            m_domains[index].ObjectManagement = record.ObjectManagement;
            m_domains[index].PagerResolution = record.PagerResolution;
            m_domains[index].Supervisor = record.Supervisor;
            m_domains[index].SupervisionTag = record.SupervisionTag;
            __atomic_store_n(&m_domains[index].Protection, record.Protection, __ATOMIC_RELEASE);
            return true;
        }
    }
    return false;
}

Kernel::Memory::FaultDelivery Amd64ChannelSupervisor::DeliverFault(Kernel::Scheduler::Thread& thread,
                                                                   Scheduler::Amd64KernelContext& context,
                                                                   const Kernel::Memory::PagerFaultSource& source,
                                                                   Scheduler::Amd64ProcessorFaultState& state) noexcept
{
    auto& channel = m_channel->GetChannel();
    const auto prepared = channel.PrepareFault(source);
    if (prepared != Kernel::Memory::PagerChannelError::None)
    {
        if (prepared == Kernel::Memory::PagerChannelError::Busy)
        {
            return Kernel::Memory::FaultDelivery::Busy;
        }
        const auto snapshot = channel.GetSnapshot();
        Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::InvariantViolated, static_cast<UInt64>(prepared),
                               static_cast<UInt64>(snapshot.Ingress.Error),
                               static_cast<UInt64>(snapshot.Ingress.Factory),
                               static_cast<UInt64>(snapshot.Ingress.Dispatcher));
    }
    if (Scheduler::CurrentAmd64ProcessorRunLoop(*m_loop).FinishFaultAndDispatch(thread, context, channel, state.Receipt,
                                                                                state.Failure) &&
        state.Receipt.ThreadStorage == &thread &&
        state.Receipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active)
    {
        return Kernel::Memory::FaultDelivery::Resolved;
    }
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::InvariantViolated, 0x4641'554CU,
                           static_cast<UInt64>(state.Failure.Error), static_cast<UInt64>(state.Failure.ControllerError),
                           state.Failure.ActiveRunRetained);
}

bool Amd64UserThreadTable::ConfigureCreatedDomains(IAmd64FaultSupervisor& supervisor,
                                                   Kernel::System::IObjectManagement& objects) noexcept
{
    if (m_createdSupervisor != nullptr)
    {
        return false;
    }
    m_createdSupervisor = &supervisor;
    m_createdObjects = &objects;
    return true;
}

bool Amd64UserThreadTable::RecordDomain(const Kernel::Protection::CreatedDomain& domain) noexcept
{
    if ((m_createdSupervisor == nullptr) || (domain.Protection == nullptr) || (domain.AddressSpace == nullptr) ||
        (domain.SupervisionTag == 0U))
    {
        return false;
    }
    for (Size created{0U}; created < Amd64CreatedDomainRecords; ++created)
    {
        Amd64UserDomainRecord& record = m_domains[Amd64BootDomainRecords + created];
        if (__atomic_load_n(&record.Protection, __ATOMIC_ACQUIRE) != nullptr)
        {
            continue;
        }
        if (!m_accessors[created].Bind(*domain.AddressSpace, domain.AddressSpaceIdentity))
        {
            return false;
        }
        // The selected channel supplies supervision; FaultResolve independently
        // selects the issuer from the token.
        if (domain.Channel != nullptr)
        {
            m_channelSupervisors[created].Bind(m_loop, *domain.Channel, *m_createdSupervisor);
        }
        record.UserMemory = &m_accessors[created];
        record.ObjectManagement = m_createdObjects;
        record.PagerResolution = nullptr;
        record.Supervisor = domain.Channel != nullptr ? &m_channelSupervisors[created] : m_createdSupervisor;
        record.SupervisionTag = domain.SupervisionTag;
        __atomic_store_n(&record.Protection, domain.Protection, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

bool Amd64UserThreadTable::RetireDomain(const Kernel::Protection::ProtectionDomain& domain) noexcept
{
    for (Size created{0U}; created < Amd64CreatedDomainRecords; ++created)
    {
        Amd64UserDomainRecord& record = m_domains[Amd64BootDomainRecords + created];
        if (__atomic_load_n(&record.Protection, __ATOMIC_ACQUIRE) != &domain)
        {
            continue;
        }
        for (const Slot& slot : m_slots)
        {
            if ((__atomic_load_n(&slot.State, __ATOMIC_ACQUIRE) != SlotState::Free) &&
                (__atomic_load_n(&slot.Domain, __ATOMIC_ACQUIRE) == &record))
            {
                return false;
            }
        }
        if (!m_accessors[created].Reset())
        {
            return false;
        }
        __atomic_store_n(&record.Protection, static_cast<Kernel::Protection::ProtectionDomain*>(nullptr),
                         __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

UInt64 Amd64UserThreadTable::GetNativeSlotBytes() const noexcept
{
    return sizeof(Slot) + m_slots[0U].Stack.SizeBytes;
}

Kernel::Scheduler::UserThreadSchedule Amd64UserThreadTable::GetSchedule() const noexcept
{
    return Kernel::Scheduler::UserThreadSchedule{
        .Configuration = MakeAmd64UserSchedulingConfiguration(),
        .Now = Kernel::Object::MonotonicTime{m_timer.GetTicks()},
    };
}

UserThreadBackendError Amd64UserThreadTable::Reserve(Kernel::Scheduler::Thread& thread,
                                                     const Kernel::Scheduler::ThreadId threadIdentity,
                                                     Kernel::Scheduler::SchedulingContext& context,
                                                     const Kernel::Scheduler::SchedulingContextId contextIdentity,
                                                     const Kernel::Scheduler::UserThreadStart& start) noexcept
{
    if (!IsConfigured())
    {
        return UserThreadBackendError::Exhausted;
    }
    const Amd64UserDomainRecord* const domain =
        (start.Domain != nullptr) ? FindDomain(*start.Domain) : FindCallerDomain();
    if ((domain == nullptr) || !Memory::IsUserAddress(start.Entry))
    {
        return UserThreadBackendError::Rejected;
    }
    for (Slot& slot : m_slots)
    {
        if (!slot.Request.Reserve())
        {
            continue;
        }
        slot.Start = start;
        __atomic_store_n(&slot.Domain, domain, __ATOMIC_RELEASE);
        slot.Home = Kernel::Scheduler::CpuId{0U};
        slot.Thread = &thread;
        slot.ThreadIdentity = threadIdentity;
        slot.ContextIdentity = contextIdentity;
        slot.Scheduling = &context;
        const auto prepared = Prepare(slot, context);
        if (prepared != UserThreadBackendError::None)
        {
            return prepared;
        }
        __atomic_store_n(&slot.State, SlotState::Pending, __ATOMIC_RELEASE);
        slot.Request.PublishReserved(thread, slot.Home);
        return UserThreadBackendError::None;
    }
    return UserThreadBackendError::Exhausted;
}

UserThreadBackendError Amd64UserThreadTable::Start(Kernel::Scheduler::Thread& thread,
                                                   const Kernel::Scheduler::ThreadId threadIdentity,
                                                   Kernel::Scheduler::SchedulingContext& context,
                                                   const Kernel::Scheduler::SchedulingContextId contextIdentity,
                                                   const Kernel::Scheduler::UserThreadStart&) noexcept
{
    for (Slot& slot : m_slots)
    {
        if (!slot.Request.Matches(thread))
        {
            continue;
        }
        const auto current = Platform::Synchronization::CurrentProcessor();
        // After publication the home can run, reap and reuse
        // the native slot before this poster returns. Retain the destination.
        const auto home = slot.Home;
        const bool remote = current.IsValid() && current.GetValue() != home.GetValue();
        if (remote)
        {
            slot.Request.Publish(thread, home, true);
            Platform::Synchronization::KickProcessor(home);
            return UserThreadBackendError::None;
        }
        const auto admitted = Admit(slot, thread, threadIdentity, context, contextIdentity);
        if (admitted == UserThreadBackendError::None)
        {
            slot.Request.Publish(thread, home, false);
            static_cast<void>(__atomic_fetch_add(&m_started, 1U, __ATOMIC_RELAXED));
        }
        return admitted;
    }
    return UserThreadBackendError::Faulted;
}

bool Amd64UserThreadTable::Owns(const Slot& slot) const noexcept
{
    auto cpu = Platform::Synchronization::CurrentProcessor();
    if (!cpu.IsValid())
    {
        cpu = Kernel::Scheduler::CpuId{0U};
    }
    // Bound publishes the new home and payload on slot reuse. Read it first
    // so an old home observation cannot authorize a newly rebound slot.
    return __atomic_load_n(&slot.State, __ATOMIC_ACQUIRE) == SlotState::Bound &&
           slot.Request.Home().GetValue() == cpu.GetValue();
}

Time::Amd64ApicTimer& Amd64UserThreadTable::LocalTimer() const noexcept
{
    const auto cpu = Platform::Synchronization::CurrentProcessor();
    return *m_homeTimers[cpu.IsValid() ? cpu.GetValue() : 0U];
}

bool Amd64UserThreadTable::Place(Slot& slot, Kernel::Scheduler::SchedulingContext& context) noexcept
{
    const auto binding = slot.Domain->Protection->GetBinding();
    if (!binding.HasValue() || binding.GetValue().Capabilities == nullptr)
    {
        return false;
    }
    // Boot pager and its preemption self-test share the CPU-0 mask.
    // A shared poll advances only after the other thread ran on that processor.
    const bool fixed =
        slot.Domain->SupervisionTag == 0U || binding.GetValue().Capabilities->HasProcessorDeviceAuthority();
    // ThreadPool::ReserveLocked serializes home selection and reservation.
    // Home reaping is concurrent; binding loads are acquire observations.
    auto eligible = Platform::Synchronization::RuntimeProcessorSet().UserPlacementMask();
#if !defined(ASTERIA_TARGET_AMD64)
    // Hosted native-table owners need not install the platform processor set.
    if (eligible == 0U && !Platform::Synchronization::ProcessorPowerStopping())
    {
        eligible = 1U;
    }
#endif
    Foundation::Size loads[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Size cpu{0U}; cpu < Kernel::Scheduler::MaximumProcessorCount; ++cpu)
    {
        if (m_homeLoops[cpu] == nullptr || m_homeTimers[cpu] == nullptr)
        {
            eligible &= ~(Foundation::ProcessorMask{1U} << cpu);
        }
        else
        {
            loads[cpu] = m_homeLoops[cpu]->GetPlacementLoad();
        }
    }
    slot.BalancedPlacement = !fixed;
    slot.Home =
        fixed ? Kernel::Scheduler::CpuId{0U} : Kernel::Scheduler::SelectProcessorHome(eligible, loads, m_lastPlacement);
    if (!slot.Home.IsValid() || (eligible & (Foundation::ProcessorMask{1U} << slot.Home.GetValue())) == 0U ||
        context.SetInitialHome(slot.Home) != Kernel::Scheduler::SchedulingContextError::None ||
        !HomeLoop(slot).ReserveBinding(slot.BindingReservation))
    {
        slot.Home = Kernel::Scheduler::CpuId{0U};
        return false;
    }
    if (slot.BalancedPlacement)
    {
        m_lastPlacement = slot.Home;
    }
    return true;
}

UserThreadBackendError Amd64UserThreadTable::Prepare(Slot& slot, Kernel::Scheduler::SchedulingContext& context) noexcept
{
    const auto binding = slot.Domain->Protection->GetBinding();
    if (!binding.HasValue() || !slot.Access.Bind(*binding.GetValue().AddressSpaceStorage,
                                                 binding.GetValue().AddressSpace, slot.Domain->SupervisionTag == 0U))
    {
        Clear(slot);
        return UserThreadBackendError::Rejected;
    }
    // A slot's context is prepared once, so each start builds a fresh one;
    // Attaching the area gives the thread the initial x87 and SSE state.
    slot.Context = ::new (static_cast<void*>(slot.ContextStorage)) Scheduler::Amd64KernelContext{};
    slot.Context->AttachExtendedState(slot.ExtendedState);
    if (slot.Context->Prepare(slot.Stack, Launch, &slot) != Scheduler::Amd64KernelContextError::None)
    {
        Clear(slot);
        return UserThreadBackendError::Faulted;
    }
    if (!Place(slot, context))
    {
        Clear(slot);
        return UserThreadBackendError::Exhausted;
    }
    return UserThreadBackendError::None;
}

UserThreadBackendError
Amd64UserThreadTable::Admit(Slot& slot, Kernel::Scheduler::Thread& thread,
                            const Kernel::Scheduler::ThreadId threadIdentity,
                            Kernel::Scheduler::SchedulingContext& context,
                            const Kernel::Scheduler::SchedulingContextId contextIdentity) noexcept
{
    // Start from the home's clock, not the poster's.
    if (context.SetInitialTime(Kernel::Object::MonotonicTime{LocalTimer().GetTicks()}) !=
            Kernel::Scheduler::SchedulingContextError::None ||
        thread.MakeReady() != Kernel::Scheduler::ThreadError::None)
    {
        Clear(slot);
        return UserThreadBackendError::Faulted;
    }
    const Kernel::Scheduler::ProcessorKernelRunLoopError bound =
        LocalLoop().Bind(thread, *slot.Context, *slot.Domain->Protection, &slot.BindingReservation);
    if (bound != Kernel::Scheduler::ProcessorKernelRunLoopError::None)
    {
        Clear(slot);
        return (bound == Kernel::Scheduler::ProcessorKernelRunLoopError::BindingCapacityExceeded)
                   ? UserThreadBackendError::Exhausted
                   : UserThreadBackendError::Faulted;
    }
    auto& observed = Diagnostics::GetAmd64EvidenceRecord().UserProcessors.Threads[static_cast<Size>(&slot - m_slots)];
    observed = {threadIdentity.GetValue(),
                slot.Domain->Protection->GetIdentity().GetValue(),
                slot.Home.GetValue(),
                slot.Domain->SupervisionTag,
                0U,
                0U,
                0U,
                0U};
    // The home made the thread Ready. A binding never enqueued may be undone
    // while the creator runs.
    if (!LocalLoop().EnqueueBoundThread(contextIdentity, context, threadIdentity, thread, slot.Failure))
    {
        if (LocalLoop().Unbind(thread) != Kernel::Scheduler::ProcessorKernelRunLoopError::None)
        {
            return UserThreadBackendError::Faulted;
        }
        Clear(slot);
        return UserThreadBackendError::Exhausted;
    }
    slot.Thread = &thread;
    slot.ThreadIdentity = threadIdentity;
    slot.ContextIdentity = contextIdentity;
    slot.Deferrals = 0U;
    __atomic_store_n(&slot.State, SlotState::Bound, __ATOMIC_RELEASE);
    return UserThreadBackendError::None;
}

void Amd64UserThreadTable::Clear(Slot& slot) noexcept
{
    slot.Owner->HomeLoop(slot).CancelBinding(slot.BindingReservation);
    static_cast<void>(slot.Access.Reset());
    slot.Home = Kernel::Scheduler::CpuId{0U};
    if (slot.Context != nullptr)
    {
        slot.Context->~Amd64KernelContext();
    }
    slot.Context = nullptr;
    slot.Thread = nullptr;
    slot.ThreadIdentity = {};
    slot.ContextIdentity = {};
    slot.Deferrals = 0U;
    __atomic_store_n(&slot.Domain, static_cast<const Amd64UserDomainRecord*>(nullptr), __ATOMIC_RELEASE);
    slot.Start = {};
    slot.AbortExitCode = 0U;
    __atomic_store_n(&slot.State, SlotState::Free, __ATOMIC_RELEASE);
    slot.Scheduling = nullptr;
    slot.Request.Release();
}

bool Amd64UserThreadTable::IsReleased(const Kernel::Scheduler::Thread& thread) const noexcept
{
    // A quiescing pool slot's storage hosts no new thread before the pool
    // frees it, so a thread no slot holds was reaped (or never started).
    return !Holds(&thread);
}

Kernel::Scheduler::UserThreadTerminateError Amd64UserThreadTable::Terminate(Kernel::Scheduler::Thread& thread,
                                                                            const UInt64 exitCode) noexcept
{
    using Kernel::Scheduler::UserThreadTerminateError;
    const auto current = Platform::Synchronization::CurrentProcessor();
    for (Slot& slot : m_slots)
    {
        if (!slot.Request.Matches(thread))
        {
            continue;
        }
        Kernel::Scheduler::CpuId home{};
        if (!slot.Request.PostTerminate(thread, exitCode, &home))
        {
            return UserThreadTerminateError::Pending;
        }
        if (current.IsValid() && current.GetValue() != home.GetValue())
        {
            Platform::Synchronization::KickProcessor(home);
            return UserThreadTerminateError::Pending;
        }
        if (slot.Request.Take(home, nullptr) != Kernel::Scheduler::ThreadRequestKind::Terminate)
        {
            return UserThreadTerminateError::Pending;
        }
        const auto answer = TerminateLocal(thread, exitCode);
        slot.Request.Complete();
        return answer;
    }
    return thread.GetSnapshot().State == Kernel::Scheduler::ThreadState::Exited ? UserThreadTerminateError::None
                                                                                : UserThreadTerminateError::Faulted;
}

Kernel::Scheduler::UserThreadTerminateError Amd64UserThreadTable::TerminateLocal(Kernel::Scheduler::Thread& thread,
                                                                                 const UInt64 exitCode) noexcept
{
    using Kernel::Scheduler::UserThreadTerminateError;
    if (thread.GetSnapshot().State == Kernel::Scheduler::ThreadState::Exited)
    {
        return UserThreadTerminateError::None;
    }
    for (Size index{0U}; index < Amd64UserThreadSlots; ++index)
    {
        Slot& slot = m_slots[index];
        if ((!Owns(slot)) || (slot.Thread != &thread))
        {
            continue;
        }
        switch (thread.GetSnapshot().State)
        {
        case Kernel::Scheduler::ThreadState::Ready:
            break;
        case Kernel::Scheduler::ThreadState::Running:
            return UserThreadTerminateError::Running;
        case Kernel::Scheduler::ThreadState::Blocked:
            return AbortWait(index, exitCode);
        default:
            return UserThreadTerminateError::Faulted;
        }
        Kernel::Scheduler::ReadyQueueState removed{};
        if (!LocalLoop().RemoveBoundThread(thread, removed, slot.Failure))
        {
            // Retained or selected by the run loop: not removable now.
            return ((slot.Failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ExternalFrameRetained) ||
                    (slot.Failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::WrongActiveThread))
                       ? UserThreadTerminateError::Blocked
                       : UserThreadTerminateError::Faulted;
        }
        if (thread.Terminate(exitCode) != Kernel::Scheduler::ThreadError::None)
        {
            return UserThreadTerminateError::Faulted;
        }
        static_cast<void>(__atomic_fetch_add(&m_kills, 1U, __ATOMIC_RELAXED));
        return UserThreadTerminateError::None;
    }
    return UserThreadTerminateError::Faulted;
}

// A wait-blocked thread is woken with its wait aborted; it ends with
// exitCode as it resumes (DispatchWait). A thread blocked on a fault is left
// to its supervisor, and queue pressure to a later kill.
Kernel::Scheduler::UserThreadTerminateError Amd64UserThreadTable::AbortWait(const Size index,
                                                                            const UInt64 exitCode) noexcept
{
    using Kernel::Scheduler::UserThreadTerminateError;
    if (GetWaitState(index) != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked)
    {
        return UserThreadTerminateError::Blocked;
    }
    m_slots[index].AbortExitCode = exitCode;
    const auto aborted = m_waits[index].Abort();
    if (!aborted.HasValue())
    {
        return (GetWaitState(index) == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
                   ? UserThreadTerminateError::Faulted
                   : UserThreadTerminateError::Blocked;
    }
    if (!aborted.GetValue().Aborted)
    {
        return UserThreadTerminateError::Blocked;
    }
    static_cast<void>(__atomic_fetch_add(&m_waitAborts, 1U, __ATOMIC_RELAXED));
    static_cast<void>(__atomic_fetch_add(&m_kills, 1U, __ATOMIC_RELAXED));
    return UserThreadTerminateError::None;
}

bool Amd64UserThreadTable::Holds(const Kernel::Scheduler::Thread* const thread) const noexcept
{
    for (const Slot& slot : m_slots)
    {
        if ((thread != nullptr) && slot.Request.Matches(*thread))
        {
            return true;
        }
    }
    return false;
}

const Amd64UserDomainRecord* Amd64UserThreadTable::FindCallerDomain() const noexcept
{
    const auto active = LocalLoop().GetActiveProtectionDomainBinding();
    if (!active.HasValue())
    {
        return nullptr;
    }
    for (const Amd64UserDomainRecord& record : m_domains)
    {
        if (__atomic_load_n(&record.Protection, __ATOMIC_ACQUIRE) == active.GetValue().Storage)
        {
            return &record;
        }
    }
    return nullptr;
}

const Amd64UserDomainRecord*
Amd64UserThreadTable::FindDomain(const Kernel::Protection::ProtectionDomain& domain) const noexcept
{
    for (const Amd64UserDomainRecord& record : m_domains)
    {
        if (__atomic_load_n(&record.Protection, __ATOMIC_ACQUIRE) == &domain)
        {
            return &record;
        }
    }
    return nullptr;
}

Size Amd64UserThreadTable::FindRunning() const noexcept
{
    const auto snapshot = LocalLoop().GetSnapshot();
    if (snapshot.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active)
    {
        return Amd64UserThreadSlots;
    }
    for (Size index{0U}; index < Amd64UserThreadSlots; ++index)
    {
        if ((Owns(m_slots[index])) && (m_slots[index].Thread == snapshot.ActiveThread))
        {
            return index;
        }
    }
    return Amd64UserThreadSlots;
}

[[noreturn]] void Amd64UserThreadTable::Launch(void* const argument) noexcept
{
    // The run loop activated the domain's root before entering this context.
    auto& slot = *static_cast<Slot*>(argument);
    Kernel::Diagnostics::RecordUserProcessor(
        Diagnostics::GetAmd64EvidenceRecord().UserProcessors, Platform::Synchronization::CurrentProcessor().GetValue(),
        static_cast<Size>(&slot - slot.Owner->m_slots), slot.Domain->SupervisionTag == 0U, false);
    auto& frame = slot.LaunchFrame;
    frame = Interrupt::Amd64TrapFrame{};
    frame.Rip = slot.Start.Entry;
    frame.Rsp = slot.Start.UserStackTop;
    frame.Rdi = slot.Start.Argument;
    frame.Cs = Interrupt::UserCodeSelector;
    frame.Ss = Interrupt::UserDataSelector;
    frame.Rflags = Interrupt::UserReturnRequiredFlags;
    if (!Interrupt::Amd64UserEntry::SelectKernelStack(*slot.Owner->m_tables, *slot.Owner->m_entryContext,
                                                      slot.Stack.VirtualStart + slot.Stack.SizeBytes))
    {
        FailLaunch();
    }
    Interrupt::Amd64UserEntry::Enter(frame);
}

bool Amd64UserThreadTable::DispatchSystemCall(const Size index, const Kernel::System::SystemCallRequest& request,
                                              Kernel::System::SystemCallResponse& response,
                                              Kernel::System::SystemCallScratch& scratch) noexcept
{
    if ((index >= Amd64UserThreadSlots) || (!Owns(m_slots[index])))
    {
        return false;
    }
    Slot& slot = m_slots[index];
    if (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::ThreadExit))
    {
        return Exit(slot, request, response);
    }
    if (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::WaitSetWait))
    {
        return DispatchWait(index, request, response);
    }
    const Amd64UserDomainRecord& domain = *slot.Domain;
    if (!Kernel::System::SystemCallIngress::Dispatch(LocalLoop(), slot.Access, scratch, request, response,
                                                     slot.IngressFailure, domain.PagerResolution,
                                                     domain.ObjectManagement))
    {
        return false;
    }
    if ((request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::DebugWrite)) &&
        (response.Error == Kernel::System::SystemCallError::None))
    {
        static_cast<void>(__atomic_fetch_add(&m_lines, 1U, __ATOMIC_RELAXED));
    }
    return true;
}

// As the pager's wait (Amd64PagerRuntime::DispatchWait): a wait that cannot complete at once blocks
// the thread's run, and the thread continues here once ProgressWaits woke it and the run loop
// admitted it again.
bool Amd64UserThreadTable::DispatchWait(const Size index, const Kernel::System::SystemCallRequest& request,
                                        Kernel::System::SystemCallResponse& response) noexcept
{
    Slot& slot = m_slots[index];
    Kernel::System::WaitSetSystemCallDispatchResult& result = m_waitResults[index];
    Kernel::System::WaitSetSystemCallIngressFailure& failure = m_waitFailures[index];
    if (!Kernel::System::WaitSetSystemCallIngress::Dispatch(LocalLoop(), m_waitCalls[index], request, result, failure))
    {
        return false;
    }
    if (result.Disposition == Kernel::System::WaitSetSystemCallDisposition::SuspendPrepared)
    {
        static_cast<void>(__atomic_fetch_add(&m_waitsBlocked, 1U, __ATOMIC_RELAXED));
        if (!LocalLoop().FinishWaitAndDispatch(*slot.Thread, *slot.Context, m_waits[index], slot.Receipt,
                                               slot.Failure) ||
            (slot.Receipt.ThreadStorage != slot.Thread) ||
            (slot.Receipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active) ||
            !Kernel::System::WaitSetSystemCallIngress::ConsumeCompletion(LocalLoop(), m_waitCalls[index], result,
                                                                         failure))
        {
            return false;
        }
        // Killed while it waited: it never returns to user mode.
        if (result.Wait.Aborted)
        {
            static_cast<void>(__atomic_fetch_add(&m_abortedExits, 1U, __ATOMIC_RELAXED));
            return End(slot, slot.AbortExitCode);
        }
    }
    response = result.Response;
    return true;
}

bool Amd64UserThreadTable::ConfigureWaits(const UInt64 ticksPerMicrosecond) noexcept
{
    if (m_waitsConfigured || (ticksPerMicrosecond == 0U))
    {
        return false;
    }
    for (Kernel::System::WaitSetSystemCallDispatcher& wait : m_waitCalls)
    {
        if (!wait.ConfigureTimeouts(m_timer, ticksPerMicrosecond))
        {
            return false;
        }
    }
    m_waitsConfigured = true;
    return true;
}

Amd64ThreadWaitProgress Amd64UserThreadTable::ProgressWaits() noexcept
{
    Platform::Synchronization::ServiceThreadRequests(true);
    Amd64ThreadWaitProgress progress{Amd64ThreadWaitProgress::Waiting};
    for (Size index{0U}; index < Amd64UserThreadSlots; ++index)
    {
        const Amd64ThreadWaitProgress slot = ProgressWait(index);
        if (slot == Amd64ThreadWaitProgress::Failed)
        {
            return slot;
        }
        progress = (slot == Amd64ThreadWaitProgress::Woken) ? slot : progress;
    }
    return progress;
}

// Readiness first, so a signalled member wins over a passed deadline; queue
// pressure leaves a wait for the next scheduling point.
Kernel::Ipc::WaitSetThreadDispatcherState Amd64UserThreadTable::GetWaitState(const Size index) const noexcept
{
    return m_waits[index].GetSnapshot().State;
}

Amd64ThreadWaitProgress Amd64UserThreadTable::ProgressWait(const Size index) noexcept
{
    Kernel::Ipc::WaitSetThreadDispatcher& wait = m_waits[index];
    if (!Owns(m_slots[index]))
    {
        return Amd64ThreadWaitProgress::Waiting;
    }
    const Kernel::Ipc::WaitSetThreadDispatcherState state = GetWaitState(index);
    if (state == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
    {
        return Amd64ThreadWaitProgress::Failed;
    }
    if ((!Owns(m_slots[index])) || ((state != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked) &&
                                    (state != Kernel::Ipc::WaitSetThreadDispatcherState::QueuePressure)))
    {
        return Amd64ThreadWaitProgress::Waiting;
    }
    const auto polled = wait.Poll();
    if (!polled.HasValue())
    {
        return Amd64ThreadWaitProgress::Failed;
    }
    return (polled.GetValue().State == Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
               ? Amd64ThreadWaitProgress::Woken
               : ExpireWait(index);
}

Amd64ThreadWaitProgress Amd64UserThreadTable::ExpireWait(const Size index) noexcept
{
    Kernel::Ipc::WaitSetThreadDispatcher& wait = m_waits[index];
    const auto expired = wait.Expire(LocalTimer().GetTicks());
    if (!expired.HasValue())
    {
        return (GetWaitState(index) == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
                   ? Amd64ThreadWaitProgress::Failed
                   : Amd64ThreadWaitProgress::Waiting;
    }
    if (expired.GetValue().State != Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
    {
        return Amd64ThreadWaitProgress::Waiting;
    }
    static_cast<void>(__atomic_fetch_add(&m_waitExpiries, 1U, __ATOMIC_RELAXED));
    return Amd64ThreadWaitProgress::Woken;
}

UInt64 Amd64UserThreadTable::GetWaitDeadline() const noexcept
{
    UInt64 earliest{0U};
    for (Size index{0U}; index < Amd64UserThreadSlots; ++index)
    {
        if ((!Owns(m_slots[index])) || (GetWaitState(index) != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked))
        {
            continue;
        }
        const UInt64 deadline = m_waits[index].GetDeadline();
        if ((deadline != 0U) && ((earliest == 0U) || (deadline < earliest)))
        {
            earliest = deadline;
        }
    }
    return earliest;
}

bool Amd64UserThreadTable::Exit(Slot& slot, const Kernel::System::SystemCallRequest& request,
                                Kernel::System::SystemCallResponse& response) noexcept
{
    UInt64 exitCode{0U};
    if (!Kernel::System::SystemCallDispatcher::DecodeThreadExit(request, exitCode))
    {
        response = Kernel::System::SystemCallResponse{
            .Error = Kernel::System::SystemCallError::InvalidArguments, .Value0 = 0U, .Value1 = 0U};
        return true;
    }
    static_cast<void>(__atomic_fetch_add(&m_exits, 1U, __ATOMIC_RELAXED));
    return End(slot, exitCode);
}

bool Amd64UserThreadTable::End(Slot& slot, const UInt64 exitCode) noexcept
{
    // A created domain's supervisor learns of the exit; the pager's own threads it polls for.
    if ((slot.Domain->SupervisionTag != 0U) && (slot.Domain->Supervisor != nullptr))
    {
        static_cast<void>(__atomic_fetch_add(&m_domainExits, 1U, __ATOMIC_RELAXED));
        slot.Domain->Supervisor->OnExit();
    }
    // An exited run never resumes; the reaper frees the slot later.
    static_cast<void>(LocalLoop().FinishAndDispatch(*slot.Thread, *slot.Context,
                                                    Kernel::Scheduler::ThreadRunDisposition::Exited, exitCode,
                                                    slot.Receipt, slot.Failure));
    return false;
}

bool Amd64UserThreadTable::Preempt(const Size index) noexcept
{
    Platform::Synchronization::ServiceThreadRequests(true);
    if ((index >= Amd64UserThreadSlots) || (!Owns(m_slots[index])))
    {
        return false;
    }
    Slot& slot = m_slots[index];
    return LocalLoop().FinishAndDispatch(*slot.Thread, *slot.Context, Kernel::Scheduler::ThreadRunDisposition::Ready,
                                         0U, slot.Receipt, slot.Failure) &&
           (slot.Receipt.ThreadStorage == slot.Thread) &&
           (slot.Receipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active);
}

bool Amd64UserThreadTable::BlockOnFault(const Size index, const Kernel::Memory::FaultRecord& fault) noexcept
{
    if ((index >= Amd64UserThreadSlots) || (!Owns(m_slots[index])) || (m_slots[index].Domain->Supervisor == nullptr))
    {
        return false;
    }
    Slot& slot = m_slots[index];
    IAmd64FaultSupervisor& supervisor = *slot.Domain->Supervisor;
    auto& processors = Diagnostics::GetAmd64EvidenceRecord().UserProcessors;
    const auto cpu = Platform::Synchronization::CurrentProcessor();
    if (cpu.IsValid())
    {
        Kernel::Diagnostics::CountUserProcessor(processors.Processors[cpu.GetValue()].Faults);
    }
    Kernel::Diagnostics::CountUserProcessor(processors.Threads[index].Faults);
    static_cast<void>(__atomic_fetch_add(&m_faults, 1U, __ATOMIC_RELAXED));
    static_cast<void>(__atomic_fetch_add(
        &m_exceptions, (fault.Cause == Kernel::Memory::FaultCause::Exception) ? 1U : 0U, __ATOMIC_RELAXED));
    if (slot.Domain->SupervisionTag != 0U)
    {
        static_cast<void>(__atomic_fetch_add(&m_taggedFaults, 1U, __ATOMIC_RELAXED));
        __atomic_store_n(&m_lastTag, slot.Domain->SupervisionTag, __ATOMIC_RELEASE);
    }
    const Kernel::Memory::PagerFaultSource source{
        .Thread = slot.ThreadIdentity,
        .AddressSpace = slot.Domain->Protection->GetSnapshot().AddressSpace,
        .SchedulingContext = slot.ContextIdentity,
        .Fault = fault,
        .SupervisionTag = slot.Domain->SupervisionTag,
        .ProtectionDomainIdentity = slot.Domain->Protection->GetIdentity().GetValue(),
    };
    switch (Kernel::Memory::DecideFaultOutcome(
        supervisor.DeliverFault(*slot.Thread, *slot.Context, source, slot.FaultState), slot.Deferrals))
    {
    case Kernel::Memory::FaultOutcome::Resume:
        if (cpu.IsValid())
        {
            Kernel::Diagnostics::CountUserProcessor(processors.Processors[cpu.GetValue()].Resolved);
        }
        return true;
    case Kernel::Memory::FaultOutcome::Yield:
        // The supervisor serves the fault it holds once it runs; this thread
        // yields and faults again when it next runs.
        static_cast<void>(__atomic_fetch_add(&m_deferrals, 1U, __ATOMIC_RELAXED));
        if (supervisor.Progress() && Preempt(index))
        {
            return true;
        }
        Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::InvariantViolated, 0x5949'454CU,
                               static_cast<UInt64>(slot.Failure.Error),
                               static_cast<UInt64>(slot.Failure.ControllerError), slot.Failure.ActiveRunRetained);
    case Kernel::Memory::FaultOutcome::Stop:
        break;
    }
    return false;
}

void Amd64UserThreadTable::ServiceRequests(const bool schedulingPoint) noexcept
{
    auto current = Platform::Synchronization::CurrentProcessor();
    if (!current.IsValid())
    {
        current = Kernel::Scheduler::CpuId{0U};
    }
    const auto active = LocalLoop().GetSnapshot();
    // External-frame admission/return owns a retained context until it lands.
    // Service at the next complete scheduling point, not inside that transition.
    if (active.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle &&
        active.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active)
    {
        return;
    }
    const auto* protectedThread = schedulingPoint ? nullptr : active.ActiveThread;
    for (Slot& slot : m_slots)
    {
        const auto request = slot.Request.Take(current, protectedThread);
        if (request != Kernel::Scheduler::ThreadRequestKind::None)
        {
            Kernel::Diagnostics::CountUserProcessor(
                Diagnostics::GetAmd64EvidenceRecord().UserProcessors.Processors[current.GetValue()].RequestsServed);
        }
        if (request == Kernel::Scheduler::ThreadRequestKind::Bind)
        {
            if (Admit(slot, *slot.Thread, slot.ThreadIdentity, *slot.Scheduling, slot.ContextIdentity) !=
                UserThreadBackendError::None)
            {
                Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 3U);
            }
            static_cast<void>(__atomic_fetch_add(&m_started, 1U, __ATOMIC_RELAXED));
            slot.Request.Complete();
        }
        else if (request == Kernel::Scheduler::ThreadRequestKind::Terminate)
        {
            const UInt64 exitCode = slot.Request.ExitCode();
            const bool running = slot.Thread->GetSnapshot().State == Kernel::Scheduler::ThreadState::Running;
            if (running)
            {
                if (active.ActiveThread != slot.Thread)
                {
                    slot.Request.Defer(request);
                    continue;
                }
                // Complete before the non-returning switch; a wait never takes this branch.
                slot.Request.Complete();
                static_cast<void>(__atomic_fetch_add(&m_kills, 1U, __ATOMIC_RELAXED));
                Platform::Synchronization::LeaveThreadRequestService();
                static_cast<void>(End(slot, exitCode));
                Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 3U);
            }
            const auto answer = TerminateLocal(*slot.Thread, exitCode);
            slot.Request.Complete();
            // Fault-blocked requests are withdrawn: only FaultResolve can end them.
            if (answer == Kernel::Scheduler::UserThreadTerminateError::Faulted)
            {
                Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 3U);
            }
        }
    }
}

bool Amd64UserThreadTable::Reap() noexcept
{
    for (Size index{0U}; index < Amd64UserThreadSlots; ++index)
    {
        Slot& slot = m_slots[index];
        if ((!Owns(slot)) || (slot.Thread->GetSnapshot().State != Kernel::Scheduler::ThreadState::Exited))
        {
            continue;
        }
        if (!slot.Request.Retire(*slot.Thread))
        {
            continue;
        }
        // A thread killed after its wait completed never consumed it.
        if ((LocalLoop().Unbind(*slot.Thread) != Kernel::Scheduler::ProcessorKernelRunLoopError::None) ||
            !m_waits[index].Reset().HasValue())
        {
            return false;
        }
        Clear(slot);
        static_cast<void>(__atomic_fetch_add(&m_reaped, 1U, __ATOMIC_RELAXED));
    }
    return true;
}

Amd64UserThreadTableSnapshot Amd64UserThreadTable::GetSnapshot() const noexcept
{
    Size free{0U};
    for (const Slot& slot : m_slots)
    {
        free += (IsConfigured() && (__atomic_load_n(&slot.State, __ATOMIC_ACQUIRE) == SlotState::Free)) ? 1U : 0U;
    }
    return Amd64UserThreadTableSnapshot{
        .FreeSlots = free,
        .Started = __atomic_load_n(&m_started, __ATOMIC_ACQUIRE),
        .Exits = __atomic_load_n(&m_exits, __ATOMIC_ACQUIRE),
        .Reaped = __atomic_load_n(&m_reaped, __ATOMIC_ACQUIRE),
        .Lines = __atomic_load_n(&m_lines, __ATOMIC_ACQUIRE),
        .Faults = __atomic_load_n(&m_faults, __ATOMIC_ACQUIRE),
        .Deferrals = __atomic_load_n(&m_deferrals, __ATOMIC_ACQUIRE),
        .Exceptions = __atomic_load_n(&m_exceptions, __ATOMIC_ACQUIRE),
        .TaggedFaults = __atomic_load_n(&m_taggedFaults, __ATOMIC_ACQUIRE),
        .LastTag = __atomic_load_n(&m_lastTag, __ATOMIC_ACQUIRE),
        .DomainExits = __atomic_load_n(&m_domainExits, __ATOMIC_ACQUIRE),
        .Kills = __atomic_load_n(&m_kills, __ATOMIC_ACQUIRE),
        .WaitsBlocked = __atomic_load_n(&m_waitsBlocked, __ATOMIC_ACQUIRE),
        .WaitExpiries = __atomic_load_n(&m_waitExpiries, __ATOMIC_ACQUIRE),
        .WaitAborts = __atomic_load_n(&m_waitAborts, __ATOMIC_ACQUIRE),
        .AbortedExits = __atomic_load_n(&m_abortedExits, __ATOMIC_ACQUIRE),
    };
}
} // Namespace Asteria::Platform::Amd64::System
