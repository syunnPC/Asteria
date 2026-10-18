// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Scheduler/Amd64ApplicationProcessorRuntime.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"

#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"
#include "Platform/Amd64/Time/Amd64TscClock.hpp"
#include "Platform/Amd64/System/Amd64UserThreadTable.hpp"
#include "Kernel/System/ObjectManager.hpp"

namespace Asteria::Platform::Amd64::Scheduler
{
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadState;
using Foundation::UInt64;

namespace
{
// The diagnostic timer probe: one TimerQueue deadline this far after the probe
// starts, and the bound on waiting for its interrupt.
constexpr UInt64 TimerProbeDelayTicks{50'000'000U};
constexpr UInt64 TimerProbeTimeoutTicks{2'000'000'000U};
// Arms of the probe's deadline that may fall due before the controller could
// Program it (ProbeTimer).
constexpr Foundation::Size TimerProbeArmAttempts{8U};
[[nodiscard]] UInt64 ProgressTicks(void*) noexcept
{
    return Time::ReadTsc();
}
void ProgressStop(const Kernel::Diagnostics::StopCode code, const UInt64 detail) noexcept
{
    Diagnostics::StopAmd64(
        code, detail,
        code == Kernel::Diagnostics::StopCode::Lockup ? Platform::Synchronization::ProcessorWatchdogPeriodMilliseconds()
                                                      : 0U,
        code == Kernel::Diagnostics::StopCode::Lockup ? Platform::Synchronization::CurrentProcessor().GetValue() : 0U,
        code == Kernel::Diagnostics::StopCode::Lockup ? Platform::Synchronization::ProcessorWatchdogFailureAge() : 0U);
}
} // Namespace

Amd64ApplicationProcessorRuntime::Amd64ApplicationProcessorRuntime(const Amd64ApplicationProcessorInput& input) noexcept
    : m_input{input}, m_issuer{*input.Registry}, m_queue{*input.Registry, m_timer},
      m_controller{*input.Registry, m_queue, m_timer, input.Processor}, m_loop{m_controller, m_addressSpaces, m_idle},
      m_waitRuntime{*input.Registry, m_queue, m_timer, m_controller, m_loop, m_thread, m_worker},
      m_scheduling{*input.Registry}, m_timerObject{m_timer}, m_deferred{*input.Registry, m_queue}
{
}

UInt64 Amd64ApplicationProcessorRuntime::GetState() const noexcept
{
    return m_state.LoadAcquire();
}
UInt64 Amd64ApplicationProcessorRuntime::GetCompleted() const noexcept
{
    return m_completed.LoadAcquire();
}
UInt64 Amd64ApplicationProcessorRuntime::GetRuns() const noexcept
{
    return m_runs.LoadAcquire();
}
UInt64 Amd64ApplicationProcessorRuntime::GetTimerInterrupts() const noexcept
{
    return m_timerInterrupts.LoadAcquire();
}
UInt64 Amd64ApplicationProcessorRuntime::GetTimerDeliveries() const noexcept
{
    return m_timerDeliveries.LoadAcquire();
}
UInt64 Amd64ApplicationProcessorRuntime::GetObservation() const noexcept
{
    return m_observation;
}
const Amd64ApplicationProcessorTimerObservation& Amd64ApplicationProcessorRuntime::GetTimerObservation() const noexcept
{
    return m_timerObservation;
}
void Amd64ApplicationProcessorRuntime::BindDrain(Kernel::Memory::ITlbShootdownDrain& drain) noexcept
{
    m_drain = &drain;
}

[[noreturn]] void Amd64ApplicationProcessorRuntime::Fail(const UInt64 stage) noexcept
{
    m_state.StoreRelease(0x100U + stage);
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::ApplicationProcessorStage, stage);
}

bool Amd64ApplicationProcessorRuntime::PrepareObjects() noexcept
{
    auto& registry = *m_input.Registry;
    auto& resources = *m_input.Resources;
    const auto shared = m_input.SharedSpace->GetSnapshot().Identity;
    if (m_protection.InitializeKernelOnly(registry, m_input.ResourceIdentity, resources, shared).Error !=
        Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    const auto protection = m_issuer.CreateObject(m_protection);
    if (!protection.HasValue() || m_protection.BindIdentity(registry, {registry, protection.GetValue()}).Error !=
                                      Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    if (m_thread.Initialize(resources.GetIdentifier()) != ThreadError::None)
    {
        return false;
    }
    const auto thread = m_issuer.CreateObject(m_thread);
    if (!thread.HasValue())
    {
        return false;
    }
    m_threadId = {registry, thread.GetValue()};
    const UInt64 now = m_timer.GetTicks();
    const UInt64 budget = ~UInt64{0U} - now - 1U;
    const Kernel::Scheduler::SchedulingContextConfiguration configuration{
        budget,
        budget,
        budget,
        1U,
        0xFFU,
        Kernel::Scheduler::SchedulingCriticality::Emergency,
        static_cast<Foundation::UInt8>(1U << m_input.Processor.GetValue()),
        Kernel::Scheduler::SchedulingClass::EmergencyRecovery,
        0U};
    if (m_scheduling.Initialize(configuration, resources.GetIdentifier(), Kernel::Object::MonotonicTime{now}) !=
        Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }
    const auto scheduling = m_issuer.CreateObject(m_scheduling);
    if (!scheduling.HasValue())
    {
        return false;
    }
    m_schedulingId = {registry, scheduling.GetValue()};
    return m_scheduling.Bind(m_threadId) == Kernel::Scheduler::SchedulingContextError::None &&
           m_worker.Prepare(m_input.WorkerStack, Worker, this) == Amd64KernelContextError::None;
}

bool Amd64ApplicationProcessorRuntime::AdmitWorker() noexcept
{
    return m_thread.MakeReady() == ThreadError::None &&
           m_loop.Bind(m_thread, m_worker, m_protection) == Kernel::Scheduler::ProcessorKernelRunLoopError::None &&
           m_loop.EnqueueBoundThread(m_schedulingId, m_scheduling, m_threadId, m_thread, m_idleFailure);
}

bool Amd64ApplicationProcessorRuntime::DispatchBlocked() noexcept
{
    const bool dispatched = Amd64DispatchFromIdle(m_loop, m_idleReceipt, m_idleFailure);
    return !dispatched && m_idleFailure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected &&
           m_idleFailure.ControllerError == Kernel::Scheduler::ProcessorRunError::NoRunnableThread &&
           m_loop.GetSnapshot().State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle &&
           m_thread.GetSnapshot().State == ThreadState::Blocked && m_thread.GetSnapshot().BlockToken.IsValid();
}

bool Amd64ApplicationProcessorRuntime::WakeWorker() noexcept
{
    const auto token = m_thread.GetSnapshot().BlockToken;
    auto& wake = m_controller.GetWakeFacade();
    const auto reserved = wake.ReserveBlockedEnqueue(m_schedulingId, m_scheduling, m_threadId, m_thread, token);
    if (!reserved.HasValue())
    {
        return false;
    }
    if (m_thread.Wake(token) != ThreadError::None)
    {
        static_cast<void>(wake.CancelReservedEnqueue(reserved.GetValue()));
        return false;
    }
    return wake.CommitReservedEnqueue(reserved.GetValue()).HasValue();
}

[[noreturn]] void Amd64ApplicationProcessorRuntime::Worker(void* const argument) noexcept
{
    auto& runtime = *static_cast<Amd64ApplicationProcessorRuntime*>(argument);
    for (;;)
    {
        const UInt64 runs = runtime.m_runs.LoadAcquire();
        if (runs == ~UInt64{0U})
        {
            runtime.Fail(5U);
        }
        if (runtime.m_observeAddress != 0U)
        {
            // A BSP-published diagnostic mapping, never an untrusted user pointer.
            const auto* const word =
                reinterpret_cast<const UInt64*>(static_cast<Foundation::UIntPointer>(runtime.m_observeAddress));
            runtime.m_observation = *word;
        }
        runtime.m_runs.StoreRelease(runs + 1U);
        if (!runtime.m_loop.FinishAndDispatch(runtime.m_thread, runtime.m_worker, ThreadRunDisposition::Blocked, 0U,
                                              runtime.m_workerReceipt, runtime.m_workerFailure) ||
            runtime.m_workerReceipt.ThreadStorage != &runtime.m_thread ||
            runtime.m_workerReceipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active)
        {
            runtime.Fail(6U);
        }
    }
}

bool Amd64ApplicationProcessorRuntime::RequestService(const UInt64 address) noexcept
{
    const UInt64 completed = m_completed.LoadAcquire();
    if (m_state.LoadAcquire() != 1U || m_requested.LoadAcquire() != completed || completed == ~UInt64{0U} ||
        (address & 7U) != 0U)
    {
        return false;
    }
    m_observeAddress = address;
    m_requested.StoreRelease(completed + 1U);
    return true;
}

void Amd64ApplicationProcessorRuntime::Service() noexcept
{
    Platform::Synchronization::ProcessorWaitProgress();
    const UInt64 requested = m_requested.LoadAcquire();
    const UInt64 completed = m_completed.LoadAcquire();
    if (requested == completed)
    {
        return;
    }
    // One outstanding request; the woken worker runs once and blocks again.
    // How many times it has run is the BSP's to record (GetRuns).
    if (requested != completed + 1U || !WakeWorker() || !DispatchBlocked())
    {
        Fail(8U);
    }
    m_completed.StoreRelease(requested);
}

bool Amd64ApplicationProcessorRuntime::OnTimerInterrupt() noexcept
{
    if (m_timer.AcknowledgeInterrupt())
    {
        m_timerInterrupts.StoreRelease(m_timerInterrupts.LoadAcquire() + 1U);
        return true;
    }
    return false;
}

bool Amd64ApplicationProcessorRuntime::ServiceUsers() noexcept
{
    if (m_usersEnabled.LoadAcquire() == 0U)
    {
        return true;
    }
    if (!m_idleAdopted)
    {
        if (!m_addressSpaces.AdoptIdleRoot(*m_input.IdleSpace))
        {
            return false;
        }
        m_idleAdopted = true;
        auto& users = Diagnostics::GetAmd64EvidenceRecord().UserProcessors;
        UInt64 root{0U};
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("mov %%cr3,%0" : "=r"(root));
#endif
        users.Processors[m_input.Processor.GetValue()].IdleRoot = root;
    }
    // Only this home touches its continuations, wait deadlines and native bindings. Each dispatch
    // returns to this CPU's idle stack/root when its local queue empties.
    auto& users = *m_input.UserThreads;
    Platform::Synchronization::ServiceThreadRequests(true);
    if (!users.Reap() || users.ProgressWaits() == System::Amd64ThreadWaitProgress::Failed)
    {
        return false;
    }
    const bool dispatched = Amd64DispatchFromIdle(m_loop, m_idleReceipt, m_idleFailure);
    if (dispatched)
    {
        return false;
    }
    // Intel SDM, rev. 093, Vol. 3A 7.14.4: a CPL change loads the
    // owning TSS's inner RSP. Restore the idle entry stack after users.
    if (!Interrupt::Amd64UserEntry::SelectKernelStack(*m_input.Descriptors, *m_input.EntryContext, m_idleEntryStackTop))
    {
        return false;
    }
    m_input.EntryContext->UserStackScratch = 0U;
    // Bounded background work uses this CPU's idle root.
    if (!m_input.Objects->ScrubIdle())
    {
        return false;
    }
    Kernel::Diagnostics::CountUserProcessor(
        Diagnostics::GetAmd64EvidenceRecord().UserProcessors.Processors[m_input.Processor.GetValue()].IdleScrubVisits);
    const auto error = m_idleFailure.ControllerError;
    return (m_idleFailure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::AddressSpaceControllerRejected &&
            m_idleFailure.AddressSpaceController.AdmissionRollbackSafe &&
            m_loop.GetSnapshot().State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) ||
           (m_idleFailure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected &&
            (error == Kernel::Scheduler::ProcessorRunError::NoRunnableThread ||
             error == Kernel::Scheduler::ProcessorRunError::DeadlineReached)) ||
           (m_idleFailure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerEntryRejected &&
            error == Kernel::Scheduler::ProcessorRunError::DeadlineReached && m_idleFailure.ActiveRunRetained == 0U);
}

bool Amd64ApplicationProcessorRuntime::ProbeTimer() noexcept
{
    UInt64 start{0U};
    UInt64 deadline{0U};
    bool armed{false};
    for (Foundation::Size attempt{0U}; (attempt < TimerProbeArmAttempts) && !armed; ++attempt)
    {
        // Even a deadline-arm retry services this CPU.
        Platform::Synchronization::ProcessorWaitProgress();
        start = m_timer.GetTicks();
        if (start > ~UInt64{0U} - TimerProbeTimeoutTicks)
        {
            return false;
        }
        deadline = start + TimerProbeDelayTicks;
        m_timerObservation.RequestedDeadlineTicks = deadline;
        if (!m_queue.Arm(m_timerId, m_timerObject, Kernel::Object::MonotonicTime{deadline}).HasValue())
        {
            return false;
        }
        // The controller must program exactly the queued deadline; otherwise
        // no interrupt could end the wait below. If the vCPU was not scheduled
        // for the whole delay, the deadline is due before it could be
        // programmed (DeadlineReached, nonterminal): no interrupt will come
        // for it, so the probe arms the same timer again.
        const auto refreshed = m_controller.RefreshTimerQueueDeadline(start);
        if (!refreshed.HasValue())
        {
            if (refreshed.GetError().Error != Kernel::Scheduler::ProcessorRunError::DeadlineReached)
            {
                return false;
            }
            continue;
        }
        if (refreshed.GetValue().State != Kernel::Time::ProcessorDeadlineState::Armed ||
            refreshed.GetValue().SelectedSources !=
                static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue) ||
            m_timer.GetDeadline() != deadline)
        {
            return false;
        }
        armed = true;
    }
    if (!armed)
    {
        return false;
    }
    for (Foundation::Size attempt{0U}; attempt < 20'000'000U; ++attempt)
    {
        Platform::Synchronization::ProcessorWaitProgress();
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("sti; pause; cli" : : : "memory");
#endif
        const UInt64 interrupts = m_timerInterrupts.LoadAcquire();
        if (interrupts != 0U)
        {
            m_timerObservation.ObservedTicks = m_timer.GetTicks();
            m_timerObservation.ObservedInterrupts = interrupts;
            return CompleteTimerDelivery();
        }
        if (m_timer.GetTicks() > start + TimerProbeTimeoutTicks)
        {
            break;
        }
    }
    // No interrupt within the bound: the AP's local timer does not work.
    return false;
}

bool Amd64ApplicationProcessorRuntime::PrepareTimerDelivery() noexcept
{
    const auto owner = m_input.Resources->GetIdentifier();
    if (m_timerObject.Initialize(owner) != Kernel::Time::TimerError::None ||
        m_notification.Initialize(owner) != Kernel::Ipc::NotificationError::None)
    {
        return false;
    }
    const auto timer = m_issuer.CreateObject(m_timerObject);
    const auto notification = m_issuer.CreateObject(m_notification);
    if (!timer.HasValue() || !notification.HasValue())
    {
        return false;
    }
    m_timerId = {*m_input.Registry, timer.GetValue()};
    const Kernel::Ipc::NotificationId notificationId{*m_input.Registry, notification.GetValue()};
    return m_deferred.Register(m_timerId, m_timerObject, notificationId, m_notification).HasValue();
}

bool Amd64ApplicationProcessorRuntime::CompleteTimerDelivery() noexcept
{
    // The IRQ has returned to the AP's idle stack with IF clear. Delivery may
    // take object locks here; the blocked worker and its continuation stay intact.
    if (m_thread.GetSnapshot().State != ThreadState::Blocked ||
        m_loop.GetSnapshot().State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle ||
        m_notification.GetSnapshot().Pending != 0U ||
        m_deferred.DrainDueRetained() != Kernel::Time::DeferredTimerDrainOutcome::Drained)
    {
        return false;
    }
    const auto& delivered = m_deferred.GetRetainedDrainReceipt();
    auto& observation = m_timerObservation;
    observation.DeliveredCount = delivered.DeliveredCount;
    observation.DrainStop = static_cast<UInt64>(delivered.Stop);
    observation.DeliveredOwnTimer = (delivered.DeliveredCount != 0U) &&
                                            delivered.FirstTimerIdentity.IsEqualTo(m_timerId) &&
                                            delivered.LastTimerIdentity.IsEqualTo(m_timerId)
                                        ? 1U
                                        : 0U;
    observation.QueueNextDeadlineTicks = delivered.QueueState.NextDeadlineTicks;
    observation.QueueScheduledEntries = delivered.QueueState.ScheduledEntries;
    observation.QueuePendingEntries = delivered.QueueState.PendingEntries;
    observation.QueueClaimedEntries = delivered.QueueState.ClaimedEntries;
    const auto claim = m_notification.Claim();
    if (!claim.HasValue())
    {
        return false;
    }
    observation.NotificationClaimSequence = claim.GetValue().GetSequence();
    const auto consumed = m_notification.CommitClaim(claim.GetValue());
    if (!consumed.HasValue())
    {
        return false;
    }
    observation.NotificationState = static_cast<UInt64>(consumed.GetValue().State);
    observation.NotificationPending = consumed.GetValue().Pending;
    observation.NotificationLastClaimSequence = consumed.GetValue().LastClaimSequence;
    const auto disarmed = m_controller.RefreshTimerQueueDeadline(m_timer.GetTicks());
    if (!disarmed.HasValue())
    {
        return false;
    }
    observation.DisarmedDeadlineState = static_cast<UInt64>(disarmed.GetValue().State);
    observation.DisarmedDeadlineTicks = m_timer.GetDeadline();
    observation.DisarmedTimerLvt = m_timer.Read(Time::ApicLvtTimerRegister);
    observation.DisarmedInitialCount = m_timer.Read(Time::ApicInitialCountRegister);
    m_timerDeliveries.StoreRelease(m_timerDeliveries.LoadAcquire() + 1U);
    return true;
}

[[noreturn]] void Amd64ApplicationProcessorRuntime::Run() noexcept
{
    m_idleEntryStackTop = m_input.EntryContext->KernelStackTop;
    Interrupt::Amd64UserEntry::Install(*m_input.Descriptors, *m_input.EntryContext);
    using Platform::Synchronization::BindProcessorProgress;
    using Platform::Synchronization::ProcessorProgressHooks;
    using Platform::Synchronization::RuntimeProcessorSet;
    BindProcessorProgress(
        m_input.Processor,
        ProcessorProgressHooks{m_drain, Kernel::Memory::DrainProcessorShootdowns, nullptr, ProgressTicks, ProgressStop,
                               Time::GetTscFrequency() * Kernel::Scheduler::Amd64CrossProcessorWaitSeconds,
                               &RuntimeProcessorSet()});
    if (!m_addressSpaces.Initialize(m_input.Processor, *m_input.SharedSpace).HasValue() ||
        !m_timer.Initialize(m_input.ApicAddress))
    {
        Fail(1U);
    }
    if (m_timer.GetApic().IsX2Apic() != m_input.X2Apic)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-APIC:AP-MODE-MISMATCH\r\n");
        Fail(11U);
    }
    if (m_timer.GetApic().GetId() != m_input.HardwareId)
    {
        Fail(1U);
    }
#if defined(ASTERIA_TARGET_AMD64)
    UInt64 cr4{0U};
    __asm__ __volatile__("movq %%cr4,%0" : "=r"(cr4));
    if ((cr4 & ((1U << 7U) | (1U << 17U))) != 0U)
    {
        Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, cr4);
    }
#endif
    if (!RuntimeProcessorSet().Admit(m_input.Processor))
    {
        Fail(1U);
    }
    Diagnostics::Amd64StopProcessorOnline(m_input.Processor.GetValue());
    if (!PrepareObjects() || !AdmitWorker())
    {
        Fail(2U);
    }
    if (!DispatchBlocked())
    {
        Fail(3U);
    }
    m_timerObservation.InitialRuns = m_runs.LoadAcquire();
    if (!PrepareTimerDelivery() || !ProbeTimer())
    {
        Fail(4U);
    }
    // Timer probe completed; no user deadline exists.
    if (!Platform::Synchronization::SynchronizeOnlineProcessorClock(m_timer, Time::GetTscFrequency(),
                                                                    Diagnostics::GetAmd64EvidenceRecord().Clocks))
    {
        Fail(4U);
    }
    Platform::Synchronization::ProcessorWatchdogOnline();
    m_timer.EnableWatchdog(Platform::Synchronization::ProcessorWatchdogCheckTicks());
    m_state.StoreRelease(1U);
    for (;;)
    {
        Service();
        if (!ServiceUsers())
        {
            Fail(12U);
        }
#if defined(ASTERIA_TARGET_AMD64)
        // Descriptor installation and Service's lock-taking work have completed before this
        // lock-free window.
        __asm__ __volatile__(".globl AsteriaAmd64ApInterruptWindowBegin\n"
                             ".hidden AsteriaAmd64ApInterruptWindowBegin\n"
                             "AsteriaAmd64ApInterruptWindowBegin:\n"
                             "sti; pause; cli\n"
                             ".globl AsteriaAmd64ApInterruptWindowEnd\n"
                             ".hidden AsteriaAmd64ApInterruptWindowEnd\n"
                             "AsteriaAmd64ApInterruptWindowEnd:\n"
                             :
                             :
                             : "memory");
#endif
    }
}
} // Namespace Asteria::Platform::Amd64::Scheduler
